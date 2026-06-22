/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

/** @brief Enables or disables automatic shutdown of the Wi-Fi AP. */
#ifndef WIFI_AP_AUTO_OFF_ENABLE
#define WIFI_AP_AUTO_OFF_ENABLE (0)
#endif

/**
 * @brief Timeout in minutes before the Wi-Fi AP is turned off automatically.
 *
 * The timeout only applies when no client is connected and the STA interface
 * is already connected.
 */
#define WIFI_AP_AUTO_OFF_TIMEOUT_MIN (10)

/** @brief Current application software version. */
#define APP_SW_VERSION "1.0.1"

/**
 * @brief Displays the boot guide image.
 */
void display_boot_guide_image(void);

/**
 * @brief Restores factory settings and reinitializes runtime state.
 */
void app_manager_factory_reset_machine(void);

/**
 * @brief Starts the application manager.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if startup fails
 */
esp_err_t app_manager_start(void);

/**
 * @brief Disconnects the STA interface while keeping the AP enabled.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if disconnect fails
 */
esp_err_t app_manager_disconnect_sta_keep_ap(void);

/**
 * @brief Marks the app manager as having recent user or system activity.
 */
void app_manager_mark_activity(void);

/**
 * @brief Updates the refresh-in-progress state.
 *
 * @param in_progress True while a refresh is running, otherwise false.
 */
void app_manager_set_refresh_in_progress(bool in_progress);

/**
 * @brief Plays the shutdown chime, schedules the next RTC wake if a periodic
 *        slideshow is configured (low_power_mode + auto_slideshow + interval),
 *        and cuts PMIC power. Does not return.
 *
 * Intended for the front-panel C button: a tap powers the device off; the next
 * wake is either a button press or the configured slideshow timer.
 */
void app_manager_request_shutdown(void);
