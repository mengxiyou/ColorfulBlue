/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_manager.h"
#include "hal/hal.h"
#include "hal/wifi/hal_wifi.h"
#include "hal/utils/audio/audio.h"
#include "apps/local_photo_slideshow/local_photo_slideshow.h"
#include "apps/app_server/app_server.h"
#include "esp_log.h"
#include <esp_timer.h>
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <M5Unified.h>
#include "qrcode.h"
#include "hal/utils/image/image_utils.h"
#include "hal/storage/hal_storage.h"
#include "driver/usb_serial_jtag.h"

#ifndef APP_ASSETS_USE_EMBEDDED
#define APP_ASSETS_USE_EMBEDDED 0
#endif

#if APP_ASSETS_USE_EMBEDDED
extern const uint8_t _binary_PaperColor_png_start[] asm("_binary_PaperColor_png_start");
extern const uint8_t _binary_PaperColor_png_end[] asm("_binary_PaperColor_png_end");
extern const uint8_t _binary_arrowClickToPowerOn_png_start[] asm("_binary_arrowClickToPowerOn_png_start");
extern const uint8_t _binary_arrowClickToPowerOn_png_end[] asm("_binary_arrowClickToPowerOn_png_end");
#endif

using namespace hal_wifi;

static const char* g_tag = "AppMgr";

// ---- Global objects ----
PhotoSlideshow photo_slideshow;

// ---- State ----
static uint32_t g_btn_press_start = 0;
static bool g_btn_long_pressed    = false;

// ---- WiFi AP auto-off timer ----
static uint32_t g_ap_auto_off_timer     = 0;
static bool g_ap_auto_off_timer_running = false;

// ---- Activity / refresh tracking ----
static uint32_t g_low_power_last_activity_ms         = 0;
static bool g_refresh_in_progress                    = false;
static constexpr uint32_t LOW_POWER_IDLE_SHUTDOWN_MS = 60000;

// ---- millis() ----
static inline uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void get_ap_name(char* ap_name, size_t ap_name_size)
{
    uint8_t mac[6];
    hal.getDeviceMac(mac);
    snprintf(ap_name, ap_name_size, "PaperColor-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static esp_err_t ensure_apsta_started()
{
    if (WiFi.getMode() == WiFiMode::APSTA) {
        return ESP_OK;
    }

    char ap_name[32];
    get_ap_name(ap_name, sizeof(ap_name));

    esp_err_t err = WiFi.setMode(WiFiMode::APSTA);
    if (err != ESP_OK) {
        return err;
    }

    err = WiFi.softAP(ap_name, "", 6);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(g_tag, "AP SSID: %s", ap_name);
    ESP_LOGI(g_tag, "AP IP : %s", WiFi.softAPIP().c_str());
    ESP_LOGI(g_tag, "AP MAC : %s", WiFi.macAddressAP().c_str());
    return ESP_OK;
}

static esp_err_t disconnect_sta_keep_ap_internal()
{
    g_ap_auto_off_timer_running  = false;

    esp_err_t err = ensure_apsta_started();
    if (err != ESP_OK) {
        return err;
    }

    err = WiFi.disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }

    return ESP_OK;
}

static bool prepare_next_low_power_wake()
{
    if (!hal.lowPowerModeEnabled()) {
        return false;
    }
    if (!hal.settings.auto_slideshow || hal.settings.interval_minutes <= 0) {
        return false;
    }

    if (!hal.configureRtcWakePin()) {
        ESP_LOGW(g_tag, "Failed to configure RTC wake pin");
        return false;
    }
    if (!hal.scheduleNextWakeMinutes(hal.settings.interval_minutes)) {
        ESP_LOGW(g_tag, "Failed to schedule next RTC wake");
        return false;
    }
    return true;
}

static bool has_user_activity()
{
    return M5.BtnA.wasPressed() || M5.BtnA.wasClicked() || M5.BtnA.isPressed() || M5.BtnB.wasPressed() ||
           M5.BtnB.wasClicked() || M5.BtnB.isPressed() || M5.BtnC.wasPressed() || M5.BtnC.wasClicked() ||
           M5.BtnC.isPressed();
}

void app_manager_mark_activity(void)
{
    g_low_power_last_activity_ms = millis_();
}

void app_manager_set_refresh_in_progress(bool in_progress)
{
    g_refresh_in_progress = in_progress;
}

static bool should_idle_power_off_in_low_power_mode()
{
    if (!hal.lowPowerModeEnabled()) {
        g_low_power_last_activity_ms = 0;
        g_refresh_in_progress        = false;
        return false;
    }
    if (!hal.settings.auto_slideshow || hal.settings.interval_minutes <= 0) {
        g_low_power_last_activity_ms = 0;
        g_refresh_in_progress        = false;
        return false;
    }

    WiFiMode current_wifi_mode = WiFi.getMode();
    bool ap_active             = (current_wifi_mode == WiFiMode::APSTA || current_wifi_mode == WiFiMode::AP);
    bool has_ap_clients        = ap_active && (WiFi.softAPgetStationNum() > 0);
    if (has_ap_clients) {
        app_manager_mark_activity();
        return false;
    }

    uint32_t now = millis_();
    if (has_user_activity()) {
        app_manager_mark_activity();
        return false;
    }

    if (g_refresh_in_progress) {
        return false;
    }

    if (g_low_power_last_activity_ms == 0) {
        g_low_power_last_activity_ms = now;
        return false;
    }

    return now - g_low_power_last_activity_ms >= LOW_POWER_IDLE_SHUTDOWN_MS;
}

// Front-line guard for every shutdown path: while USB is plugged in (host
// is enumerating us, or sourcing VBUS through the USB-Serial/JTAG link),
// keep the device awake so a developer can flash / inspect logs without
// having to wake it first.
static bool usb_link_active(void)
{
    return usb_serial_jtag_is_connected();
}

static void shutdown_after_low_power_cycle()
{
    if (usb_link_active()) {
        ESP_LOGI(g_tag, "Low-power shutdown skipped because USB is connected");
        app_manager_mark_activity();
        return;
    }
    if (!prepare_next_low_power_wake()) {
        ESP_LOGW(g_tag, "Low-power shutdown skipped because wake scheduling failed");
        return;
    }

    hal.clearWakeFlags();
    vTaskDelay(pdMS_TO_TICKS(50));
    hal.powerOff();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_manager_request_shutdown(void)
{
    ESP_LOGI(g_tag, "Shutdown requested (front-panel C button)");

    if (usb_link_active()) {
        // Tell the user the press registered but the device is staying on.
        // Single longer beep, distinct from the double-tap shutdown chime.
        ESP_LOGW(g_tag, "Shutdown ignored: USB is connected, staying awake");
        M5.Speaker.setVolume(200);
        audio::play_tone_from_midi(108, 0.25);
        vTaskDelay(pdMS_TO_TICKS(300));
        return;
    }

    // Two short blips at the same pitch/volume as the A/B navigation feedback
    // (MIDI 120, 80 ms each). The double tap distinguishes shutdown from a
    // navigation press; lower-pitched melodies got drowned out by the speaker
    // driver's per-tone fade-out at this duration.
    //
    // audio::play_tone_from_midi queues each buffer via M5.Speaker.playRaw
    // (async) and returns immediately; we must keep the task alive until the
    // speaker driver has emitted all queued samples, otherwise hal.powerOff
    // cuts PMIC power before any sound reaches the speaker.
    M5.Speaker.setVolume(200);
    audio::play_tone_from_midi(120, 0.08);
    vTaskDelay(pdMS_TO_TICKS(60));         // brief gap so it's clearly two beeps
    audio::play_tone_from_midi(120, 0.08);
    vTaskDelay(pdMS_TO_TICKS(300));        // let the second beep finish + fade tail

    // Optionally schedule the next periodic wake; if not configured this
    // silently no-ops and the device stays off until a button press.
    if (!prepare_next_low_power_wake()) {
        ESP_LOGI(g_tag, "Shutdown: no periodic wake scheduled (off until button press)");
    }

    hal.clearWakeFlags();
    vTaskDelay(pdMS_TO_TICKS(50));
    hal.powerOff();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static bool run_rtc_wake_one_shot_cycle()
{
    if (!hal.isRtcWakeBoot() || !hal.lowPowerModeEnabled() || !hal.settings.auto_slideshow ||
        hal.settings.interval_minutes <= 0) {
        return false;
    }

    ESP_LOGI(g_tag, "RTC wake low-power cycle start");

    photo_slideshow.init("/data", 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!photo_slideshow.runOneShotRefresh()) {
        ESP_LOGW(g_tag, "RTC one-shot local refresh did not display a photo");
    }

    shutdown_after_low_power_cycle();
    return true;
}

esp_err_t app_manager_disconnect_sta_keep_ap(void)
{
    return disconnect_sta_keep_ap_internal();
}

// ---- WiFi QR code display ----
static void wifi_qrcode_display_cb(esp_qrcode_handle_t qrcode)
{
    int size  = esp_qrcode_get_size(qrcode);
    int scale = 4;
    if (scale < 1) scale = 1;

    int qr_px    = scale * size;
    int offset_x = 196 - qr_px / 2;
    int offset_y = 100 - qr_px / 2;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                hal.Canvas->fillRect(offset_x + x * scale, offset_y + y * scale, scale, scale, TFT_BLACK);
            }
        }
    }

    hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
    hal.Canvas->pushSprite(0, 0);
    hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
}

static void show_wifi_config_qrcode(const char* ap_ssid)
{
    uint8_t prev_rotation = hal.Canvas->getRotation();
    hal.Canvas->setRotation(1);

    int img_w = 0;
    int img_h = 0;
    int scr_w = hal.Canvas->width();
    int scr_h = hal.Canvas->height();

    M5.Display.setEpdMode(epd_mode_t::epd_fastest);

#if APP_ASSETS_USE_EMBEDDED
    const uint8_t* bg_data = _binary_PaperColor_png_start;
    size_t bg_len          = (size_t)(_binary_PaperColor_png_end - _binary_PaperColor_png_start);
    if (get_image_size_from_memory(bg_data, bg_len, &img_w, &img_h, ".png")) {
        float s    = std::min((float)scr_w / img_w, (float)scr_h / img_h);
        int draw_x = (scr_w - (int)(img_w * s)) / 2;
        int draw_y = (scr_h - (int)(img_h * s)) / 2;
        hal.Canvas->fillScreen(TFT_WHITE);
        hal.Canvas->drawPng(bg_data, bg_len, draw_x, draw_y, 0, 0, 0, 0, s, s);
    } else {
        hal.Canvas->fillScreen(TFT_WHITE);
    }
#else
    const char* bg_path = "/data/PaperColor.png";
    hal.Canvas->fillScreen(TFT_WHITE);
    hal_storage_lock();
    if (get_image_size_from_file(bg_path, &img_w, &img_h)) {
        float s    = std::min((float)scr_w / img_w, (float)scr_h / img_h);
        int draw_x = (scr_w - (int)(img_w * s)) / 2;
        int draw_y = (scr_h - (int)(img_h * s)) / 2;
        hal.Canvas->drawPngFile(bg_path, draw_x, draw_y, 0, 0, 0, 0, s, s);
    }
    hal_storage_unlock();
#endif

    // Clear the QR code area
    hal.Canvas->fillRect(142, 46, 108, 108, TFT_WHITE);

    // Generate WiFi QR code
    char wifi_url[128];
    snprintf(wifi_url, sizeof(wifi_url), "WIFI:S:%s;T:nopass;;", ap_ssid);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func        = wifi_qrcode_display_cb;
    cfg.max_qrcode_version  = 10;
    cfg.qrcode_ecc_level    = ESP_QRCODE_ECC_LOW;

    esp_err_t ret = esp_qrcode_generate(&cfg, wifi_url);
    if (ret != ESP_OK) {
        ESP_LOGE(g_tag, "WiFi QRCode failed: %d", ret);
    }
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    hal.Canvas->setRotation(prev_rotation);
}

void display_boot_guide_image()
{
    int img_w = 0;
    int img_h = 0;
    int scr_w = hal.Canvas->width();
    int scr_h = hal.Canvas->height();

#if APP_ASSETS_USE_EMBEDDED
    const uint8_t* data = _binary_arrowClickToPowerOn_png_start;
    size_t len          = (size_t)(_binary_arrowClickToPowerOn_png_end - _binary_arrowClickToPowerOn_png_start);
    if (get_image_size_from_memory(data, len, &img_w, &img_h, ".png")) {
        float s    = std::min((float)scr_w / img_w, (float)scr_h / img_h);
        int draw_x = (scr_w - (int)(img_w * s)) / 2;
        int draw_y = (scr_h - (int)(img_h * s)) / 2;
        hal.Canvas->fillScreen(TFT_WHITE);
        hal.Canvas->drawPng(data, len, draw_x, draw_y, 0, 0, 0, 0, s, s);
    }
#else
    hal.Canvas->fillScreen(TFT_WHITE);
#endif

    hal.Canvas->setTextColor(RED);
    hal.Canvas->setTextDatum(middle_left);
    hal.Canvas->setFont(&fonts::efontJA_24_b);
    hal.Canvas->drawString("Press to ON", 100, 540);

    hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
    hal.Canvas->pushSprite(0, 0);
    hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
}

void app_manager_factory_reset_machine()
{
    ESP_LOGI(g_tag, "Factory reset: restoring defaults");

    photo_slideshow.stop();

    hal.settingsLock();
    memset(&hal.settings, 0, sizeof(hal.settings));
    hal.settings.rotation         = 1;
    hal.settings.auto_slideshow   = false;
    hal.settings.interval_minutes = 60;
    hal.settings.boot_sound       = true;
    hal.settings.low_power_mode   = false;
    cstring_copy(hal.settings.device_name, "papercolor", sizeof(hal.settings.device_name));
    hal.settingsUnlock();

    hal.settingsSave(SETTING_WIFI_SSID);
    hal.settingsSave(SETTING_WIFI_PASSWORD);
    hal.settingsSave(SETTING_ROTATION);
    hal.settingsSave(SETTING_AUTO_SLIDESHOW);
    hal.settingsSave(SETTING_INTERVAL);
    hal.settingsSave(SETTING_BOOT_SOUND);
    hal.settingsSave(SETTING_DEVICE_NAME);
    hal.settingsSave(SETTING_LOW_POWER_MODE);

    hal.Canvas->setRotation(1);
    display_boot_guide_image();

    vTaskDelay(pdMS_TO_TICKS(500));
    hal.pm1.shutdown();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---- Main event loop ----
static void app_task(void* param)
{
    static uint32_t s_led_blink_timer = 0;
    static bool s_led_state           = false;

    // Generate AP name once (based on MAC)
    uint8_t mac[6];
    hal.getDeviceMac(mac);
    char ap_name[32];
    snprintf(ap_name, sizeof(ap_name), "PaperColor-%02X%02X%02X", mac[3], mac[4], mac[5]);

    while (1) {
        hal.update();

        if (should_idle_power_off_in_low_power_mode()) {
            ESP_LOGI(g_tag, "Low-power idle timeout reached, scheduling wake and powering off");
            shutdown_after_low_power_cycle();
        }

        // ==================== Button handling ====================
        bool btn_a = M5.BtnA.isPressed();
        if (btn_a && g_btn_press_start == 0) {
            g_btn_press_start  = millis_();
            g_btn_long_pressed = false;
            s_led_blink_timer  = millis_();
            s_led_state        = false;
        }

        if (!btn_a) {
            if (g_btn_press_start != 0 && !g_btn_long_pressed) {
                // Restore the AP-idle green LED instead of going dark, so any
                // accidental short press of A doesn't make the device look off.
                M5.Led.setBrightness(60);
                M5.Led.setAllColor(0, 255, 0);
                M5.Led.display();
            }
            g_btn_press_start  = 0;
            g_btn_long_pressed = false;
        }

        if (btn_a && !g_btn_long_pressed) {
            // White LED blink every 200 ms
            if (millis_() - s_led_blink_timer >= 200) {
                s_led_blink_timer = millis_();
                s_led_state       = !s_led_state;
                if (s_led_state) {
                    M5.Led.setBrightness(100);
                    M5.Led.setAllColor(255, 255, 255);
                } else {
                    M5.Led.setAllColor(0, 0, 0);
                }
                M5.Led.display();
            }

            // Long press for 5s: enable WiFi AP
            if (millis_() - g_btn_press_start >= 5000) {
                g_btn_long_pressed = true;

                // Green light confirmation
                M5.Led.setAllColor(0, 255, 0);
                M5.Led.display();

                // Prompt tone
                audio::play_tone_from_midi(100, 0.08);
                vTaskDelay(pdMS_TO_TICKS(80));
                audio::play_tone_from_midi(119, 0.08);

                vTaskDelay(pdMS_TO_TICKS(500));
                // Restore the AP-idle green indicator (don't leave it dark).
                M5.Led.setBrightness(60);
                M5.Led.setAllColor(0, 255, 0);
                M5.Led.display();

                // Enable AP
                esp_err_t err = ensure_apsta_started();
                if (err != ESP_OK) {
                    ESP_LOGW(g_tag, "Failed to re-enable AP by long press: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(g_tag, "AP re-enabled by long press");
                    show_wifi_config_qrcode(ap_name);
                }
            }
        }

        // ==================== WiFi AP auto-off ====================
#if WIFI_AP_AUTO_OFF_ENABLE
        {
            WiFiMode current_wifi_mode = WiFi.getMode();
            bool ap_active             = (current_wifi_mode == WiFiMode::APSTA || current_wifi_mode == WiFiMode::AP);
            bool sta_connected         = WiFi.isConnected();
            bool has_ap_clients        = WiFi.softAPgetStationNum() > 0;

            if (ap_active && !has_ap_clients && sta_connected) {
                if (!g_ap_auto_off_timer_running) {
                    g_ap_auto_off_timer         = millis_();
                    g_ap_auto_off_timer_running = true;
                    ESP_LOGI(g_tag, "AP auto-off timer started (%d min)", WIFI_AP_AUTO_OFF_TIMEOUT_MIN);
                }

                uint32_t elapsed_ms = millis_() - g_ap_auto_off_timer;
                uint32_t timeout_ms = (uint32_t)WIFI_AP_AUTO_OFF_TIMEOUT_MIN * 60000UL;
                if (elapsed_ms >= timeout_ms) {
                    WiFi.softAPdisconnect();
                    g_ap_auto_off_timer_running = false;
                    ESP_LOGI(g_tag, "AP auto-off: closed after %d min", WIFI_AP_AUTO_OFF_TIMEOUT_MIN);
                }
            } else {
                g_ap_auto_off_timer_running = false;
            }
        }
#endif

        // ==================== Slideshow update ====================
        photo_slideshow.update();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- Startup ----
esp_err_t app_manager_start()
{
    ESP_LOGI(g_tag, "Software version: V%s", APP_SW_VERSION);

    // ── First boot guide image ──
    nvs_handle_t h;
    uint8_t first_boot_done = 0;
    if (nvs_open("papercolor", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "first_boot_done", &first_boot_done);
        nvs_close(h);
    }

    if (!first_boot_done) {
        ESP_LOGI(g_tag, "Show the factory boot guide image at first boot.");
        display_boot_guide_image();

        struct tm factory_tm = {};
        factory_tm.tm_year   = 2026 - 1900;
        factory_tm.tm_mon    = 0;
        factory_tm.tm_mday   = 1;
        factory_tm.tm_hour   = 0;
        factory_tm.tm_min    = 0;
        factory_tm.tm_sec    = 0;
        mktime(&factory_tm);
        M5.Rtc.setDateTime(&factory_tm);

        if (nvs_open("papercolor", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "first_boot_done", 1);
            nvs_commit(h);
            nvs_close(h);
        }

        hal.pm1.shutdown();

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (run_rtc_wake_one_shot_cycle()) {
        return ESP_OK;
    }

    photo_slideshow.init("/data", hal.settings.auto_slideshow ? (uint8_t)hal.settings.interval_minutes : 0);

    ESP_ERROR_CHECK(WiFi.begin());
    ESP_ERROR_CHECK(ensure_apsta_started());

    // AP-idle indicator: take the LED away from the status-event task and pin
    // it solid green. The user explicitly wants no e-paper refresh on boot
    // (no QR code, no slideshow), so the green LED is the only sign that the
    // device is up and the SoftAP is reachable.
    hal.ledStatusSuspend();
    M5.Led.setBrightness(60);
    M5.Led.setAllColor(0, 255, 0);
    M5.Led.display();

    photo_slideshow.start();
    ESP_LOGI(g_tag, "Slideshow ready (no boot refresh; LED green = AP idle)");

    // Auto-reconnect to the saved STA in the background. settingsInit loads
    // wifi_ssid/wifi_pass from NVS into hal.settings, but nothing else turns
    // those into a connect call — without this, the device only joins STA
    // after the user re-submits the captive-portal form. WiFi.connect blocks
    // until it succeeds / times out, so we fire it off a one-shot task to
    // keep app_main moving (button handling + AP must come up promptly).
    if (hal.settings.wifi_ssid[0] != '\0') {
        xTaskCreate(
            [](void *) {
                ESP_LOGI(g_tag, "Auto-connect: SSID='%s'", hal.settings.wifi_ssid);
                esp_err_t err = WiFi.connect(hal.settings.wifi_ssid, hal.settings.wifi_password, 15000);
                if (err == ESP_OK) {
                    ESP_LOGI(g_tag, "Auto-connect: STA up");
                } else {
                    ESP_LOGW(g_tag, "Auto-connect: failed (%s) — AP stays up for re-config",
                             esp_err_to_name(err));
                }
                vTaskDelete(nullptr);
            },
            "wifi_auto", 4096, nullptr, 4, nullptr);
    } else {
        ESP_LOGI(g_tag, "Auto-connect: no SSID saved, staying AP-only until web config");
    }

    xTaskCreate(app_task, "app_mgr", 10240, NULL, 5, NULL);
    return ESP_OK;
}
