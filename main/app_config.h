#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

// Firmware metadata exposed through the serial console and BLE API.
#define MEB_APP_VERSION "0.6.1"
#define MEB_APP_ABOUT "MEB preheat CAN controller"
#define MEB_PREHEATER_PROTOCOL_VERSION 2

#ifdef CONFIG_MEB_PRODUCTION_BUILD
#define MEB_RELEASE_BUILD 1
#define MEB_BUILD_MODE "production"
#else
#define MEB_RELEASE_BUILD 0
#define MEB_BUILD_MODE "development"
#endif

#ifdef CONFIG_MEB_AUTOMATIC_LIGHT_SLEEP
#define MEB_PM_AUTOMATIC_LIGHT_SLEEP_ENABLED 1
#else
#define MEB_PM_AUTOMATIC_LIGHT_SLEEP_ENABLED 0
#endif

// ESP-IDF power-management limits. Development builds keep automatic light
// sleep disabled so USB-JTAG flashing/debugging remains reliable.
#define MEB_PM_MAX_CPU_FREQ_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define MEB_PM_MIN_CPU_FREQ_MHZ 48
#define MEB_PM_JTAG_BOOT_GRACE_MS CONFIG_MEB_JTAG_BOOT_GRACE_MS
#define MEB_PM_DEBUG_POLL_MS 250

// Single WS2812 status LED configuration.
#define MEB_LED_GPIO GPIO_NUM_27
#define MEB_LED_COUNT 1
#define MEB_LED_RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define MEB_LED_BRIGHTNESS 12

// Host UART used for the text/JSON serial console.
#define MEB_HOST_UART_TX_GPIO GPIO_NUM_11
#define MEB_HOST_UART_RX_GPIO GPIO_NUM_12
#define MEB_HOST_UART_BAUD_RATE 115200

// TWAI/CAN-FD pins, bus speeds, and transmit queue size.
#define MEB_TWAI_TX_GPIO GPIO_NUM_4
#define MEB_TWAI_RX_GPIO GPIO_NUM_5
#define MEB_TWAI_BITRATE 500000
#define MEB_TWAI_DATA_BITRATE 2000000
#define MEB_TWAI_TX_QUEUE_DEPTH 8

// Main task timing. Telemetry period can be adjusted at runtime within the
// min/max bounds.
#define MEB_CONTROL_INITIAL_DELAY_MS 250
#define MEB_CONTROL_PERIOD_MS 500
#define MEB_TELEMETRY_PERIOD_MS 1000
#define MEB_TELEMETRY_MIN_PERIOD_MS 100
#define MEB_TELEMETRY_MAX_PERIOD_MS 60000
#define MEB_BMS_SOC_POLL_PERIOD_MS 5000
#define MEB_CAN_SIGNAL_STALE_MS 30000
#define MEB_LED_PERIOD_MS 500

// Heating auto-off. A user timer of 0 minutes disables the user-configured
// timer, while the safety limit remains active unless disabled in Kconfig.
#ifdef CONFIG_MEB_DISABLE_SAFETY_AUTO_OFF
#define MEB_SAFETY_AUTO_OFF_ENABLED 0
#else
#define MEB_SAFETY_AUTO_OFF_ENABLED 1
#endif

#ifndef CONFIG_MEB_SAFETY_AUTO_OFF_MINUTES
#define CONFIG_MEB_SAFETY_AUTO_OFF_MINUTES 180
#endif
#define MEB_SAFETY_AUTO_OFF_MINUTES CONFIG_MEB_SAFETY_AUTO_OFF_MINUTES

// Temporary bench-test CAN transmitter. Set to 0 before connecting to a real car.
#define MEB_CAN_TEST_TX_ENABLED 0
#define MEB_CAN_TEST_TX_ID 0x12345678U
#define MEB_CAN_TEST_TX_PERIOD_MS 500

// Volkswagen MEB CAN identifiers used by diagnostics, status, and telemetry.
#define MEB_CAN_ID_DIAG_REQ 0x17FC007BU
#define MEB_CAN_ID_DIAG_RESP 0x17FE007BU
#define MEB_CAN_ID_HEATING_STATUS 0x12DD54D2U
#define MEB_CAN_ID_CHARGING_OPTIMIZATION 0x1A5555B2U
#define MEB_CAN_ID_DYNAMIC 0x12DD54D0U
#define MEB_CAN_ID_TEMPERATURE 0x16A954A6U

// Temperature-status value that means battery temperature is below target.
#define MEB_TEMPERATURE_STATUS_UNDER_OPTIMAL 1
