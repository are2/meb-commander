#include "app_config.h"
#include "app_state.h"
#include "ble_console.h"
#include "can_bus.h"
#include "control.h"
#include "power_management.h"
#include "serial_console.h"
#include "status_led.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "meb-preheat";

void app_main(void)
{
    ESP_ERROR_CHECK(meb_state_init());
    ESP_ERROR_CHECK(meb_power_management_init());
    ESP_ERROR_CHECK(meb_serial_console_init());
    ESP_ERROR_CHECK(meb_ble_console_init());
    ESP_ERROR_CHECK(meb_status_led_start());
    ESP_ERROR_CHECK(meb_can_init());
    ESP_ERROR_CHECK(meb_can_start_test_tx());
    ESP_ERROR_CHECK(meb_control_start());

    ESP_LOGI(TAG, "MEB preheat controller running");
    meb_serial_printf("{\"v\":%d,\"type\":\"device.ready\",\"version\":\"%s\"}\n", MEB_PROTOCOL_VERSION, MEB_APP_VERSION);
}
