#include "status_led.h"
#include "app_config.h"
#include "app_state.h"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define LED_TASK_STACK_SIZE 3072
#define LED_TASK_PRIORITY 3

static const char *TAG = "status_led";
static led_strip_handle_t s_led_strip;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb_t;

static esp_err_t configure_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = MEB_LED_GPIO,
        .max_leds = MEB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = MEB_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip), TAG, "failed to create LED strip");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_led_strip), TAG, "failed to clear LED");
    return ESP_OK;
}

static rgb_t status_color(const meb_state_snapshot_t *state)
{
    const bool requested = state->heating_enabled;
    const bool active = state->battery_heating_active != 0;

    if (requested && active) {
        return (rgb_t){.red = MEB_LED_BRIGHTNESS, .green = 0, .blue = 0};
    }
    if (requested && !active) {
        return (rgb_t){.red = MEB_LED_BRIGHTNESS, .green = MEB_LED_BRIGHTNESS, .blue = 0};
    }
    if (!requested && active) {
        return (rgb_t){.red = 0, .green = 0, .blue = MEB_LED_BRIGHTNESS};
    }

    return (rgb_t){.red = 0, .green = MEB_LED_BRIGHTNESS, .blue = 0};
}

static void led_task(void *arg)
{
    (void)arg;

    while (1) {
        meb_state_snapshot_t state;
        meb_state_get_snapshot(&state);
        rgb_t color = status_color(&state);

        ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, color.red, color.green, color.blue));
        ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));

        vTaskDelay(pdMS_TO_TICKS(MEB_LED_PERIOD_MS));
    }
}

esp_err_t meb_status_led_start(void)
{
    ESP_RETURN_ON_ERROR(configure_led(), TAG, "failed to configure LED");

    BaseType_t ok = xTaskCreate(led_task, "status_led", LED_TASK_STACK_SIZE, NULL, LED_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create LED task");

    return ESP_OK;
}
