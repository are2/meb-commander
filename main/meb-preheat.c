#include "app_config.h"
#include "app_state.h"
#include "can_bus.h"
#include "control.h"
#include "status_led.h"
#include "usb_console.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "meb-preheat";

void app_main(void)
{
    ESP_ERROR_CHECK(meb_state_init());
    ESP_ERROR_CHECK(meb_usb_console_init());
    ESP_ERROR_CHECK(meb_status_led_start());
    ESP_ERROR_CHECK(meb_can_init());
    ESP_ERROR_CHECK(meb_control_start());

    ESP_LOGI(TAG, "MEB preheat controller running");
    meb_usb_printf("{\"v\":%d,\"type\":\"device.ready\",\"version\":\"%s\"}\n", MEB_PROTOCOL_VERSION, MEB_APP_VERSION);
}
