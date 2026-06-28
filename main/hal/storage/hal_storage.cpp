#include "hal_storage.h"
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hal_storage";

#define BASE_PATH   "/data"
#define SD_SPI_HOST SPI2_HOST
#define SD_PIN_MOSI GPIO_NUM_13
#define SD_PIN_MISO GPIO_NUM_14
#define SD_PIN_SCLK GPIO_NUM_15
#define SD_PIN_CS   GPIO_NUM_47

bool s_spi_bus_inited = false;

static hal_storage_media_t s_media = APP_STORAGE_MEDIA_SPIFLASH;
static bool s_mounted              = false;
static wl_handle_t s_wl_handle     = WL_INVALID_HANDLE;
static sdmmc_card_t *s_sd_card     = NULL;
static SemaphoreHandle_t s_storage_lock = NULL;

static void ensure_storage_lock(void)
{
    if (!s_storage_lock) {
        s_storage_lock = xSemaphoreCreateMutex();
    }
}

void hal_storage_lock(void)
{
    ensure_storage_lock();
    if (s_storage_lock) xSemaphoreTake(s_storage_lock, portMAX_DELAY);
}

void hal_storage_unlock(void)
{
    if (s_storage_lock) xSemaphoreGive(s_storage_lock);
}

static esp_err_t bus_spi_init_once(void)
{
    if (s_spi_bus_inited) return ESP_OK;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 8192,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_spi_bus_inited = true;
        return ESP_OK;
    }
    return ret;
}

static esp_err_t _mount_flash(void)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 0,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, "storage", &mount_cfg, &s_wl_handle);
}

static esp_err_t _unmount_flash(void)
{
    if (s_wl_handle == WL_INVALID_HANDLE) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
    s_wl_handle   = WL_INVALID_HANDLE;
    return ret;
}

static esp_err_t _mount_sd(void)
{
    esp_err_t ret = bus_spi_init_once();
    if (ret != ESP_OK) return ret;

    sdmmc_host_t host                 = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    host.slot                         = SD_SPI_HOST;
    slot_config.host_id               = SD_SPI_HOST;
    slot_config.gpio_cs               = SD_PIN_CS;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 0,
    };

    const int supported_freqs[] = {20000, 10000, 4000};
    for (int i = 0; i < 3; i++) {
        host.max_freq_khz = supported_freqs[i];
        for (int retry = 0; retry < 2; retry++) {
            ret = esp_vfs_fat_sdspi_mount(BASE_PATH, &host, &slot_config, &mount_cfg, &s_sd_card);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "SD card mounted at %s @ %d KHz", BASE_PATH, supported_freqs[i]);
                sdmmc_card_print_info(stdout, s_sd_card);
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    ESP_LOGE(TAG, "SD card mount failed at all freqs: %s", esp_err_to_name(ret));
    return ret;
}

static esp_err_t _unmount_sd(void)
{
    if (!s_sd_card) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(BASE_PATH, s_sd_card);
    s_sd_card     = NULL;
    return ret;
}

static esp_err_t _mount_media(hal_storage_media_t media)
{
    return (media == APP_STORAGE_MEDIA_SPIFLASH) ? _mount_flash() : _mount_sd();
}

static esp_err_t _unmount_media(hal_storage_media_t media)
{
    return (media == APP_STORAGE_MEDIA_SPIFLASH) ? _unmount_flash() : _unmount_sd();
}

esp_err_t hal_storage_init(hal_storage_media_t media)
{
    hal_storage_lock();
    esp_err_t ret;
    if (s_mounted) {
        ESP_LOGW(TAG, "Storage already initialized");
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = _mount_media(media);
        if (ret == ESP_OK) {
            s_media   = media;
            s_mounted = true;
            ESP_LOGI(TAG, "Storage mounted: %s", media == APP_STORAGE_MEDIA_SPIFLASH ? "SPIFLASH" : "SDMMC");
        }
    }
    hal_storage_unlock();
    return ret;
}

esp_err_t hal_storage_switch(hal_storage_media_t media)
{
    hal_storage_lock();

    if (s_mounted && s_media == media) {
        hal_storage_unlock();
        return ESP_OK;
    }

    if (s_mounted) {
        esp_err_t r = _unmount_media(s_media);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "unmount %s failed: %s",
                     s_media == APP_STORAGE_MEDIA_SPIFLASH ? "SPIFLASH" : "SDMMC", esp_err_to_name(r));
        }
        s_mounted = false;
    }

    esp_err_t ret = _mount_media(media);
    if (ret == ESP_OK) {
        s_media   = media;
        s_mounted = true;
        ESP_LOGI(TAG, "Switched storage to %s", media == APP_STORAGE_MEDIA_SPIFLASH ? "SPIFLASH" : "SDMMC");
    } else {
        ESP_LOGE(TAG, "Failed to mount %s after switch: %s",
                 media == APP_STORAGE_MEDIA_SPIFLASH ? "SPIFLASH" : "SDMMC", esp_err_to_name(ret));
    }
    hal_storage_unlock();
    return ret;
}

hal_storage_media_t hal_storage_get_media(void)
{
    return s_media;
}
