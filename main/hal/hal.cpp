/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <string.h>
#include <time.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <M5Unified.hpp>
#include <M5GFX.h>
#include "storage/hal_storage.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "wifi/hal_wifi.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/semphr.h"

using namespace hal_wifi;

static const char* TAG = "Hal";

/* ---------- millis() ---------- */
static inline uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int cstring_compare(const char* lhs, const char* rhs)
{
    if (lhs == nullptr && rhs == nullptr) return 0;
    if (lhs == nullptr) return -1;
    if (rhs == nullptr) return 1;

    while (*lhs && (*lhs == *rhs)) {
        ++lhs;
        ++rhs;
    }

    return static_cast<unsigned char>(*lhs) - static_cast<unsigned char>(*rhs);
}

size_t cstring_copy(char* dst, const char* src, size_t dst_size)
{
    size_t src_len = 0;
    if (src != nullptr) {
        while (src[src_len] != '\0') {
            ++src_len;
        }
    }

    if (dst_size == 0 || dst == nullptr) {
        return src_len;
    }

    size_t copy_len = src_len;
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }

    for (size_t i = 0; i < copy_len; ++i) {
        dst[i] = src[i];
    }
    dst[copy_len] = '\0';

    return src_len;
}

bool normalize_device_name(const char* input, char* output, size_t output_size)
{
    static constexpr const char* k_default_device_name = "papercolor";

    if (output == nullptr || output_size == 0) {
        return true;
    }

    size_t write_index = 0;
    bool changed       = false;

    if (input != nullptr) {
        while (*input != '\0' && write_index + 1 < output_size) {
            char ch = *input++;
            if (ch >= 'A' && ch <= 'Z') {
                ch      = (char)(ch - 'A' + 'a');
                changed = true;
            }

            bool is_alpha = ch >= 'a' && ch <= 'z';
            bool is_digit = ch >= '0' && ch <= '9';
            bool is_dash  = ch == '-';
            if (!(is_alpha || is_digit || is_dash)) {
                changed = true;
                continue;
            }

            if (ch == '-' && write_index == 0) {
                changed = true;
                continue;
            }

            output[write_index++] = ch;
        }

        while (write_index > 0 && output[write_index - 1] == '-') {
            --write_index;
            changed = true;
        }
    } else {
        changed = true;
    }

    output[write_index] = '\0';

    if (write_index == 0) {
        cstring_copy(output, k_default_device_name, output_size);
        return true;
    }

    if (input == nullptr) {
        return true;
    }

    return changed || cstring_compare(output, input) != 0;
}

void Hal::init()
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON) {
        // Non-normal power-on, perform I2C bus recovery
        i2cBusRecovery();
    }
    auto cfg          = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.setRotation(3);
    Canvas = new M5Canvas(&M5.Display);
    Canvas->createSprite(M5.Display.width(), M5.Display.height());
    M5.Speaker.begin();
    s_spi_bus_inited = true;
    pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
    pm1.setI2cConfig(0);
    pm1.pinMode(SD_DET_EN, OUTPUT);
    pm1.digitalWrite(SD_DET_EN, HIGH);
    pm1.pinMode(SD_DEC, INPUT_PULLUP);
    pm1.pinMode(EPD_EN, OUTPUT);
    pm1.digitalWrite(EPD_EN, HIGH);
    pm1.setChargeEnable(true);
    pm1.setBoostEnable(true);

    uint16_t battery_mv = 0;
    if (pm1.readVbat(&battery_mv) == M5PM1_OK && battery_mv < 3100) {
        pm1.shutdown();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    _led_status_indicate_event_group = xEventGroupCreate();
    xTaskCreate(LedStatusIndicateTask, "LedStatusIndicateTask", 4096, this, 5, &_led_status_indicate_task_handle);
    _settings_mutex = xSemaphoreCreateMutex();
}

bool Hal::lowPowerModeEnabled() const
{
    return settings.low_power_mode;
}

bool Hal::isRtcWakeBoot() const
{
    return _is_rtc_wake_boot;
}

void Hal::detectWakeSource()
{
    _is_rtc_wake_boot = false;

    uint8_t wake_source = 0;
    if (pm1.getWakeSource(&wake_source, M5PM1_CLEAN_NONE) != M5PM1_OK) {
        return;
    }

    _is_rtc_wake_boot = (wake_source & M5PM1_WAKE_SRC_EXT_WAKE) != 0;
}

bool Hal::configureRtcWakePin()
{
    if (pm1.gpioSetFunc(RTC_WAKE_GPIO, M5PM1_GPIO_FUNC_WAKE) != M5PM1_OK) return false;
    if (pm1.gpioSetPull(RTC_WAKE_GPIO, M5PM1_GPIO_PULL_UP) != M5PM1_OK) return false;
    if (pm1.gpioSetWakeEdge(RTC_WAKE_GPIO, M5PM1_GPIO_WAKE_FALLING) != M5PM1_OK) return false;
    if (pm1.gpioSetWakeEnable(RTC_WAKE_GPIO, true) != M5PM1_OK) return false;
    return true;
}

bool Hal::scheduleNextWakeMinutes(int interval_minutes)
{
    if (interval_minutes <= 0) return false;

    m5::rtc_date_t date;
    m5::rtc_time_t time;
    if (!M5.Rtc.getDateTime(&date, &time)) return false;

    struct tm tm_now = {};
    tm_now.tm_year   = date.year + 100;
    tm_now.tm_mon    = date.month - 1;
    tm_now.tm_mday   = date.date;
    tm_now.tm_hour   = time.hours;
    tm_now.tm_min    = time.minutes + interval_minutes;
    tm_now.tm_sec    = time.seconds;
    if (mktime(&tm_now) < 0) return false;

    if (M5.Rtc.setAlarmIRQ(&tm_now) <= 0) return false;
    return true;
}

void Hal::clearWakeFlags()
{
    uint8_t src = 0;
    pm1.getWakeSource(&src, M5PM1_CLEAN_ALL);
    M5.Rtc.clearIRQ();
    pm1.timerClear();
}

void Hal::powerOff()
{
    pm1.sysCmd(M5PM1_SYS_CMD_OFF);
}

void Hal::settingsInit()
{
    ESP_LOGI(TAG, "settingsInit start");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Defaults (first boot / garbage NVS)
    memset(&settings, 0, sizeof(settings));
    settings.rotation         = 1;
    settings.auto_slideshow   = false;
    settings.interval_minutes = 60;
    settings.boot_sound       = true;
    settings.low_power_mode   = false;
    cstring_copy(settings.device_name, "papercolor", sizeof(settings.device_name));

    bool device_name_changed = false;
    nvs_handle_t nvs_handle;
    if (nvs_open("papercolor", NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t value_size;

        value_size = sizeof(settings.wifi_ssid);
        nvs_get_str(nvs_handle, "wifi_ssid", settings.wifi_ssid, &value_size);

        value_size = sizeof(settings.wifi_password);
        nvs_get_str(nvs_handle, "wifi_pass", settings.wifi_password, &value_size);

        uint8_t value_u8;
        if (nvs_get_u8(nvs_handle, "rotation", &value_u8) == ESP_OK && value_u8 <= 1) settings.rotation = value_u8;

        if (nvs_get_u8(nvs_handle, "auto_slide", &value_u8) == ESP_OK) settings.auto_slideshow = (value_u8 != 0);

        if (nvs_get_u8(nvs_handle, "boot_sound", &value_u8) == ESP_OK) settings.boot_sound = (value_u8 != 0);
        if (nvs_get_u8(nvs_handle, "low_power", &value_u8) == ESP_OK) settings.low_power_mode = (value_u8 != 0);

        uint16_t value_u16;
        if (nvs_get_u16(nvs_handle, "interval", &value_u16) == ESP_OK && value_u16 >= 1 && value_u16 <= 255)
            settings.interval_minutes = (int)value_u16;

        value_size = sizeof(settings.device_name);
        if (nvs_get_str(nvs_handle, "device_name", settings.device_name, &value_size) == ESP_OK) {
            char normalized_device_name[sizeof(settings.device_name)];
            device_name_changed =
                normalize_device_name(settings.device_name, normalized_device_name, sizeof(normalized_device_name));
            cstring_copy(settings.device_name, normalized_device_name, sizeof(settings.device_name));
        }

        // Daily image URL list: count in "daily_n", entries in "daily_0".."daily_9".
        uint8_t url_count = 0;
        if (nvs_get_u8(nvs_handle, "daily_n", &url_count) == ESP_OK && url_count <= DAILY_IMAGE_URL_MAX) {
            settings.daily_image_url_count = url_count;
            char key[16];
            for (uint8_t url_index = 0; url_index < url_count; url_index++) {
                snprintf(key, sizeof(key), "daily_%u", (unsigned)url_index);
                value_size = DAILY_IMAGE_URL_MAXLEN;
                if (nvs_get_str(nvs_handle, key, settings.daily_image_urls[url_index], &value_size) != ESP_OK) {
                    settings.daily_image_urls[url_index][0] = '\0';
                }
            }
        }

        nvs_close(nvs_handle);
    }

    {
        char normalized_device_name[sizeof(settings.device_name)];
        device_name_changed =
            normalize_device_name(settings.device_name, normalized_device_name, sizeof(normalized_device_name));
        cstring_copy(settings.device_name, normalized_device_name, sizeof(settings.device_name));
    }

    if (device_name_changed) {
        settingsSave(SETTING_DEVICE_NAME);
    }

    Canvas->setRotation(settings.rotation);

    ESP_LOGI(TAG, "Settings loaded: rot=%d, slide=%d, interval=%d, boot_sound=%d", settings.rotation,
             settings.auto_slideshow, settings.interval_minutes, settings.boot_sound ? 1 : 0);
}

void Hal::settingsSave(SettingKey key)
{
    nvs_handle_t h;
    if (nvs_open("papercolor", NVS_READWRITE, &h) != ESP_OK) return;

    bool changed = false;

    switch (key) {
        case SETTING_WIFI_SSID: {
            char prev[64] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "wifi_ssid", prev, &sz) != ESP_OK || cstring_compare(prev, settings.wifi_ssid) != 0) {
                nvs_set_str(h, "wifi_ssid", settings.wifi_ssid);
                changed = true;
            }
            break;
        }
        case SETTING_WIFI_PASSWORD: {
            char prev[64] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "wifi_pass", prev, &sz) != ESP_OK ||
                cstring_compare(prev, settings.wifi_password) != 0) {
                nvs_set_str(h, "wifi_pass", settings.wifi_password);
                changed = true;
            }
            break;
        }
        case SETTING_ROTATION: {
            uint8_t v;
            if (nvs_get_u8(h, "rotation", &v) != ESP_OK || v != settings.rotation) {
                nvs_set_u8(h, "rotation", settings.rotation);
                changed = true;
            }
            break;
        }
        case SETTING_AUTO_SLIDESHOW: {
            uint8_t v;
            if (nvs_get_u8(h, "auto_slide", &v) != ESP_OK || (v != 0) != settings.auto_slideshow) {
                nvs_set_u8(h, "auto_slide", settings.auto_slideshow ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_INTERVAL: {
            uint16_t v;
            if (nvs_get_u16(h, "interval", &v) != ESP_OK || v != (uint16_t)settings.interval_minutes) {
                nvs_set_u16(h, "interval", (uint16_t)settings.interval_minutes);
                changed = true;
            }
            break;
        }
        case SETTING_BOOT_SOUND: {
            uint8_t v;
            if (nvs_get_u8(h, "boot_sound", &v) != ESP_OK || (v != 0) != settings.boot_sound) {
                nvs_set_u8(h, "boot_sound", settings.boot_sound ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_LOW_POWER_MODE: {
            uint8_t v;
            if (nvs_get_u8(h, "low_power", &v) != ESP_OK || (v != 0) != settings.low_power_mode) {
                nvs_set_u8(h, "low_power", settings.low_power_mode ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_DEVICE_NAME: {
            char normalized_device_name[sizeof(settings.device_name)];
            normalize_device_name(settings.device_name, normalized_device_name, sizeof(normalized_device_name));
            cstring_copy(settings.device_name, normalized_device_name, sizeof(settings.device_name));

            char prev[64] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "device_name", prev, &sz) != ESP_OK ||
                cstring_compare(prev, settings.device_name) != 0) {
                nvs_set_str(h, "device_name", settings.device_name);
                changed = true;
            }
            break;
        }
        case SETTING_DAILY_IMAGE_URLS: {
            // Rewrite the whole list: count + every kept entry, then erase
            // any trailing keys from a previously longer list so NVS doesn't
            // leak stale URLs across saves.
            uint8_t prev_count = 0;
            (void)nvs_get_u8(h, "daily_n", &prev_count);
            nvs_set_u8(h, "daily_n", settings.daily_image_url_count);
            char key[16];
            for (uint8_t url_index = 0; url_index < settings.daily_image_url_count; url_index++) {
                snprintf(key, sizeof(key), "daily_%u", (unsigned)url_index);
                nvs_set_str(h, key, settings.daily_image_urls[url_index]);
            }
            for (uint8_t url_index = settings.daily_image_url_count;
                 url_index < prev_count && url_index < DAILY_IMAGE_URL_MAX; url_index++) {
                snprintf(key, sizeof(key), "daily_%u", (unsigned)url_index);
                nvs_erase_key(h, key);
            }
            changed = true;
            break;
        }
    }

    if (changed) {
        nvs_commit(h);
        ESP_LOGI(TAG, "Settings: key %d saved", (int)key);
    }
    nvs_close(h);
}

void Hal::settingsLock()
{
    if (_settings_mutex) xSemaphoreTake(_settings_mutex, portMAX_DELAY);
}

void Hal::settingsUnlock()
{
    if (_settings_mutex) xSemaphoreGive(_settings_mutex);
}

void Hal::update()
{
    M5.update();
}

void Hal::getDeviceMac(uint8_t* mac)
{
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
}

bool Hal::isSDCardInserted()
{
    return pm1.digitalRead(SD_DEC) == LOW ? true : false;
}

bool Hal::sht40Read(float* temp, float* humi)
{
    uint8_t cmd = SHT4X_CMD_HI_PRE;
    uint8_t buf[6];

    if (!M5.In_I2C.start(SHT4X_ADDR, false, 400000)) return false;
    M5.In_I2C.write(&cmd, 1);
    M5.In_I2C.stop();

    vTaskDelay(pdMS_TO_TICKS(10));  // High precision mode requires ~8.3ms

    if (!M5.In_I2C.start(SHT4X_ADDR, true, 400000)) return false;
    M5.In_I2C.read(buf, 6);
    M5.In_I2C.stop();

    uint16_t raw_t = (buf[0] << 8) | buf[1];
    uint16_t raw_h = (buf[3] << 8) | buf[4];

    *temp = -45.0f + 175.0f * raw_t / 65535.0f;
    *humi = -6.0f + 125.0f * raw_h / 65535.0f;
    if (*humi > 100.0f) *humi = 100.0f;
    if (*humi < 0.0f) *humi = 0.0f;

    return true;
}

/**
 * @brief  Write one byte to RX8130CE RAM
 * @param  index  Address index 0~3 (corresponding to registers 0x20~0x23)
 * @param  value  Value to write, 0~255
 * @return true on success
 */
bool Hal::rx8130RamWrite(uint8_t index, uint8_t value)
{
    if (index >= RX8130_RAM_SIZE) return false;

    uint8_t buf[2];
    buf[0] = RX8130_RAM_BASE + index;  // Register address
    buf[1] = value;                    // Data

    if (!M5.In_I2C.start(RX8130_ADDR, false, 400000)) return false;
    M5.In_I2C.write(buf, 2);
    M5.In_I2C.stop();
    return true;
}

/**
 * @brief  Read one byte from RX8130CE RAM
 * @param  index  Address index 0~3 (corresponding to registers 0x20~0x23)
 * @param  value  Pointer to store the read value
 * @return true on success
 */
bool Hal::rx8130RamRead(uint8_t index, uint8_t* value)
{
    if (index >= RX8130_RAM_SIZE || value == nullptr) return false;

    uint8_t reg = RX8130_RAM_BASE + index;

    // Write the register address first
    if (!M5.In_I2C.start(RX8130_ADDR, false, 400000)) return false;
    M5.In_I2C.write(&reg, 1);
    M5.In_I2C.stop();

    // Then read the data
    if (!M5.In_I2C.start(RX8130_ADDR, true, 400000)) return false;
    M5.In_I2C.read(value, 1);
    M5.In_I2C.stop();
    return true;
}

void Hal::i2cBusRecovery()
{
    ESP_LOGW(TAG, "Performing I2C bus recovery");

    gpio_config_t cfg;
    cfg.pin_bit_mask = (1ULL << SYS_SCL_PIN) | (1ULL << SYS_SDA_PIN);
    cfg.mode         = GPIO_MODE_INPUT_OUTPUT_OD;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    // Release both lines
    gpio_set_level(SYS_SDA_PIN, 1);
    gpio_set_level(SYS_SCL_PIN, 1);
    esp_rom_delay_us(10);

    // Send 9 clock pulses
    for (int i = 0; i < 9; i++) {
        gpio_set_level(SYS_SCL_PIN, 0);
        esp_rom_delay_us(5);
        gpio_set_level(SYS_SCL_PIN, 1);
        esp_rom_delay_us(5);

        // Exit early if SDA has already been released
        if (gpio_get_level(SYS_SDA_PIN) == 1) {
            break;
        }
    }

    gpio_set_level(SYS_SDA_PIN, 0);
    esp_rom_delay_us(5);
    gpio_set_level(SYS_SCL_PIN, 1);
    esp_rom_delay_us(5);
    gpio_set_level(SYS_SDA_PIN, 1);
    esp_rom_delay_us(5);

    gpio_reset_pin(SYS_SCL_PIN);
    gpio_reset_pin(SYS_SDA_PIN);
}

bool Hal::statusEventSend(EventBits_t event_bits)
{
    if (_led_status_indicate_event_group == nullptr) return false;
    xEventGroupSetBits(_led_status_indicate_event_group, event_bits);
    return true;
}

void Hal::ledStatusSuspend()
{
    if (_led_status_indicate_task_handle) {
        vTaskSuspend(_led_status_indicate_task_handle);
    }
}

void Hal::ledStatusResume()
{
    if (_led_status_indicate_task_handle) {
        vTaskResume(_led_status_indicate_task_handle);
    }
}

void Hal::LedStatusIndicateTask(void* task_parameters)
{
    Hal* instance = static_cast<Hal*>(task_parameters);
    EventBits_t event_bits;
    const EventBits_t all_event_bits = OPERATION_EVENT_FAILED | OPERATION_EVENT_ERROR_IMAGE_READ |
                                       OPERATION_EVENT_REFRESH_START | OPERATION_EVENT_REFRESH_COMPLETE |
                                       OPERATION_EVENT_SUCCESS | OPERATION_EVENT_WAITING_WIFI |
                                       OPERATION_EVENT_STARTUP_SUCCESS;

    LedState current_state    = LED_STATE_IDLE;
    LedState previous_state   = LED_STATE_IDLE;
    uint32_t state_start_time = 0;
    uint32_t blink_phase      = 0;

    while (1) {
        static bool startup_animation_played = false;
        if (!startup_animation_played) {
            const uint32_t rainbow_colors[] = {0xFF0066, 0xFFCC00, 0x006633, 0x336699, 0x990099};
            const int color_count           = sizeof(rainbow_colors) / sizeof(rainbow_colors[0]);

            for (int color_index = 0; color_index < color_count; color_index++) {
                uint8_t red   = (rainbow_colors[color_index] >> 16) & 0xFF;
                uint8_t green = (rainbow_colors[color_index] >> 8) & 0xFF;
                uint8_t blue  = rainbow_colors[color_index] & 0xFF;

                for (int brightness = 0; brightness <= 95; brightness += 3) {
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(red, green, blue);
                    M5.Led.display();
                    vTaskDelay(pdMS_TO_TICKS(12));
                }
                vTaskDelay(pdMS_TO_TICKS(30));
                for (int brightness = 95; brightness >= 0; brightness -= 3) {
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(red, green, blue);
                    M5.Led.display();
                    vTaskDelay(pdMS_TO_TICKS(12));
                }
                vTaskDelay(pdMS_TO_TICKS(30));
            }

            for (int flash_index = 0; flash_index < 2; flash_index++) {
                M5.Led.setBrightness(100);
                M5.Led.setAllColor(0, 255, 0);
                M5.Led.display();
                vTaskDelay(pdMS_TO_TICKS(200));
                M5.Led.setBrightness(0);
                M5.Led.display();
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            startup_animation_played = true;
        }

        event_bits = xEventGroupWaitBits(instance->_led_status_indicate_event_group, all_event_bits, pdTRUE, pdFALSE,
                                         pdMS_TO_TICKS(10));

        uint32_t now = millis_();

        if (event_bits & OPERATION_EVENT_FAILED) {
            ESP_LOGW(TAG, "Event: Directory open error");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_ERROR_FADE;
            state_start_time = now;
        }
        if (event_bits & OPERATION_EVENT_ERROR_IMAGE_READ) {
            ESP_LOGE(TAG, "Event: Image read error");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_ERROR_FADE;
            state_start_time = now;
        }
        if (event_bits & OPERATION_EVENT_REFRESH_START) {
            ESP_LOGI(TAG, "Event: Refresh start");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_REFRESH_START_BLINK;
            state_start_time = now;
            blink_phase      = 0;
        }
        if (event_bits & OPERATION_EVENT_REFRESH_COMPLETE) {
            ESP_LOGI(TAG, "Event: Refresh complete");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_REFRESH_COMPLETE_BLINK;
            state_start_time = now;
            blink_phase      = 0;
        }
        if (event_bits & OPERATION_EVENT_SUCCESS) {
            ESP_LOGI(TAG, "Event: Operation success");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_SUCCESS_FADE;
            state_start_time = now;
        }
        if (event_bits & OPERATION_EVENT_WAITING_WIFI) {
            ESP_LOGI(TAG, "Event: Waiting for WiFi connection");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_WAITING_WIFI_BLINK;
            state_start_time = now;
            blink_phase      = 0;
        }
        if (event_bits & OPERATION_EVENT_STARTUP_SUCCESS) {
            ESP_LOGI(TAG, "Event: Startup success");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_SUCCESS_STEADY_BLINK;
            state_start_time = now;
        }

        uint32_t elapsed = now - state_start_time;
        int brightness   = 0;

        switch (current_state) {
            case LED_STATE_SUCCESS_FADE:
                if (elapsed < 1050) {
                    brightness = 100 - (elapsed / 50) * 5;
                    brightness = (brightness < 0) ? 0 : brightness;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(0, 255, 0);
                    M5.Led.display();
                } else {
                    current_state    = LED_STATE_SUCCESS_STEADY_BLINK;
                    state_start_time = now;
                }
                break;

            case LED_STATE_ERROR_FADE:
                if (elapsed < 1050) {
                    brightness = 100 - (elapsed / 50) * 5;
                    brightness = (brightness < 0) ? 0 : brightness;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(255, 0, 0);
                    M5.Led.display();
                } else {
                    if (previous_state != LED_STATE_IDLE) {
                        current_state = previous_state;
                    } else {
                        current_state = LED_STATE_IDLE;
                    }
                    state_start_time = now;
                }
                break;

            case LED_STATE_REFRESH_START_BLINK:
                blink_phase = elapsed / 100;
                if (blink_phase < 4) {
                    brightness = (blink_phase % 2 == 0) ? 100 : 0;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(0, 255, 255);
                    M5.Led.display();
                } else if (elapsed < 2400) {
                    M5.Led.setBrightness(0);
                    M5.Led.display();
                } else {
                    current_state    = LED_STATE_REFRESH_START_STEADY_BLINK;
                    state_start_time = now;
                }
                break;

            case LED_STATE_REFRESH_COMPLETE_BLINK:
                blink_phase = elapsed / 100;
                if (blink_phase < 4) {
                    brightness = (blink_phase % 2 == 0) ? 100 : 0;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(0, 255, 0);
                    M5.Led.display();
                } else {
                    current_state    = LED_STATE_SUCCESS_STEADY_BLINK;
                    state_start_time = now;
                }
                break;

            case LED_STATE_WAITING_WIFI_BLINK:
                if (elapsed % 1000 < 100) {
                    M5.Led.setBrightness(100);
                    M5.Led.setAllColor(255, 0, 0);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_SUCCESS_STEADY_BLINK:
                if (elapsed >= 2000 && (elapsed - 2000) % 2000 < 50) {
                    M5.Led.setBrightness(50);
                    M5.Led.setAllColor(0, 255, 0);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_REFRESH_START_STEADY_BLINK:
                if (elapsed >= 2000 && (elapsed - 2000) % 2000 < 50) {
                    M5.Led.setBrightness(50);
                    M5.Led.setAllColor(0, 255, 255);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_REFRESH_COMPLETE_STEADY_BLINK:
                if (elapsed % 2000 < 100) {
                    M5.Led.setBrightness(100);
                    M5.Led.setAllColor(0, 255, 0);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_IDLE:
            default:
                break;
        }
    }

    M5.Led.setBrightness(0);
    M5.Led.display();
    instance->_led_status_indicate_task_handle = nullptr;
    vTaskDelete(nullptr);
}