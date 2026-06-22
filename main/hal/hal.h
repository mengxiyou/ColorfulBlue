/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <M5Unified.hpp>
#include <M5GFX.h>
#include <M5PM1.h>
#include <driver/gpio.h>
#include "wifi/hal_wifi.h"

/**
 * @brief Identifiers for persisted settings values.
 */
enum SettingKey : uint8_t {
    SETTING_WIFI_SSID,
    SETTING_WIFI_PASSWORD,
    SETTING_ROTATION,
    SETTING_AUTO_SLIDESHOW,
    SETTING_INTERVAL,
    SETTING_BOOT_SOUND,
    SETTING_DEVICE_NAME,
    SETTING_LOW_POWER_MODE
};

/** @brief Normalizes a device name. */
bool normalize_device_name(const char* input, char* output, size_t output_size);
/** @brief Compares two C strings. */
int cstring_compare(const char* lhs, const char* rhs);
/** @brief Copies a C string into a fixed-size buffer. */
size_t cstring_copy(char* dst, const char* src, size_t dst_size);

/**
 * @brief Persisted device settings.
 */
struct Settings {
    char wifi_ssid[64];
    char wifi_password[64];
    uint8_t rotation;  // 0=landscape, 1=portrait
    bool auto_slideshow;
    int interval_minutes;
    bool boot_sound;
    char device_name[64];
    bool low_power_mode;
};

#define OPERATION_EVENT_FAILED           BIT0
#define OPERATION_EVENT_ERROR_IMAGE_READ BIT1
#define OPERATION_EVENT_REFRESH_START    BIT2
#define OPERATION_EVENT_REFRESH_COMPLETE BIT3
#define OPERATION_EVENT_SUCCESS          BIT4
#define OPERATION_EVENT_WAITING_WIFI     BIT5
#define OPERATION_EVENT_STARTUP_SUCCESS  BIT6

/**
 * @brief Central hardware abstraction for the device.
 */
class Hal {
public:
    M5PM1 pm1;
    M5Canvas* Canvas = nullptr;
    float temperature = 0.0f;
    float humidity    = 0.0f;

    Settings settings;

    /** @brief Initializes hardware and runtime state. */
    void init();
    /** @brief Loads settings from storage. */
    void settingsInit();
    /** @brief Saves one settings field identified by key. */
    void settingsSave(SettingKey key);
    /** @brief Locks settings access. */
    void settingsLock();
    /** @brief Unlocks settings access. */
    void settingsUnlock();
    /** @brief Runs the periodic hardware update loop. */
    void update();
    /** @brief Reads the device MAC address. */
    void getDeviceMac(uint8_t* mac);
    /** @brief Returns whether an SD card is inserted. */
    bool isSDCardInserted();
    /** @brief Reads temperature and humidity from SHT40. */
    bool sht40Read(float* temp, float* humi);
    /** @brief Writes a byte to RX8130 RAM. */
    bool rx8130RamWrite(uint8_t index, uint8_t value);
    /** @brief Reads a byte from RX8130 RAM. */
    bool rx8130RamRead(uint8_t index, uint8_t* value);
    /** @brief Returns whether low power mode is enabled. */
    bool lowPowerModeEnabled() const;
    /** @brief Returns whether the current boot was triggered by RTC wake. */
    bool isRtcWakeBoot() const;
    /** @brief Detects the current wake source. */
    void detectWakeSource();
    /** @brief Configures the RTC wake pin. */
    bool configureRtcWakePin();
    /** @brief Schedules the next RTC wake by minutes. */
    bool scheduleNextWakeMinutes(int interval_minutes);
    /** @brief Clears wake-related flags. */
    void clearWakeFlags();
    /** @brief Powers the device off. */
    void powerOff();
    /** @brief Sends a status event. */
    bool statusEventSend(EventBits_t event_bits);

private:
    void i2cBusRecovery();
    static void LedStatusIndicateTask(void* task_parameters);
    static constexpr m5pm1_gpio_num_t SD_DEC            = M5PM1_GPIO_NUM_1;
    static constexpr m5pm1_gpio_num_t EPD_EN            = M5PM1_GPIO_NUM_0;
    static constexpr m5pm1_gpio_num_t SD_DET_EN         = M5PM1_GPIO_NUM_4;
    static constexpr gpio_num_t SYS_SCL_PIN             = GPIO_NUM_2;
    static constexpr gpio_num_t SYS_SDA_PIN             = GPIO_NUM_3;
    TaskHandle_t _led_status_indicate_task_handle       = nullptr;
    EventGroupHandle_t _led_status_indicate_event_group = nullptr;
    SemaphoreHandle_t _settings_mutex                   = nullptr;

    static constexpr uint8_t SHT4X_ADDR             = 0x44;
    static constexpr uint8_t SHT4X_CMD_HI_PRE       = 0xFD;
    static constexpr uint8_t RX8130_ADDR            = 0x32;
    static constexpr uint8_t RX8130_RAM_BASE        = 0x20;  // RAM start register address
    static constexpr uint8_t RX8130_RAM_SIZE        = 4;     // 4 bytes (0x20~0x23)
    static constexpr m5pm1_gpio_num_t RTC_WAKE_GPIO = M5PM1_GPIO_NUM_2;
    bool _is_rtc_wake_boot                          = false;

    typedef enum {
        LED_STATE_IDLE,
        LED_STATE_SUCCESS_FADE,
        LED_STATE_ERROR_FADE,
        LED_STATE_REFRESH_START_BLINK,
        LED_STATE_REFRESH_COMPLETE_BLINK,
        LED_STATE_WAITING_WIFI_BLINK,
        LED_STATE_SUCCESS_STEADY_BLINK,
        LED_STATE_REFRESH_START_STEADY_BLINK,
        LED_STATE_REFRESH_COMPLETE_STEADY_BLINK
    } LedState;
};

extern Hal hal;
