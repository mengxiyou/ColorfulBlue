/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Storage media type.
 */
typedef enum {
    APP_STORAGE_MEDIA_SPIFLASH = 0, /*!< Internal SPI Flash (read-only via web UI) */
    APP_STORAGE_MEDIA_SDMMC,        /*!< External SD/MMC card (writable via web UI) */
} hal_storage_media_t;

/**
 * @brief Mounts the selected FAT media at /data. USB-MSC is not exposed —
 *        the filesystem is owned by the firmware only.
 */
esp_err_t hal_storage_init(hal_storage_media_t media);

/**
 * @brief Switches storage media by unmounting the current backend and mounting
 *        the requested one at /data.
 */
esp_err_t hal_storage_switch(hal_storage_media_t media);

/**
 * @brief Returns the currently mounted storage media.
 */
hal_storage_media_t hal_storage_get_media(void);

/** @brief Locks storage access. */
void hal_storage_lock(void);
/** @brief Unlocks storage access. */
void hal_storage_unlock(void);

extern bool s_spi_bus_inited;

#ifdef __cplusplus
}
#endif
