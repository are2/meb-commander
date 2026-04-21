#include "power_management.h"
#include "app_config.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED \
    (CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && MEB_PM_AUTOMATIC_LIGHT_SLEEP_ENABLED)

#define DEBUG_PM_TASK_STACK_SIZE 3072
#define DEBUG_PM_TASK_PRIORITY 2

static const char *TAG = "power";
#if MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED
static esp_pm_lock_handle_t s_debug_pm_lock;
static volatile bool s_debug_pm_lock_held;
#endif

#if MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED
static esp_err_t keep_gpio_active_in_light_sleep(gpio_num_t gpio_num)
{
    if (gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }

    return gpio_sleep_sel_dis(gpio_num);
}

static esp_err_t configure_light_sleep_gpio_behavior(void)
{
#if CONFIG_PM_SLP_DISABLE_GPIO
    ESP_RETURN_ON_ERROR(keep_gpio_active_in_light_sleep(MEB_LED_GPIO), TAG, "failed to keep LED GPIO active in sleep");
    ESP_RETURN_ON_ERROR(keep_gpio_active_in_light_sleep(MEB_HOST_UART_TX_GPIO), TAG, "failed to keep UART TX GPIO active in sleep");
    ESP_RETURN_ON_ERROR(keep_gpio_active_in_light_sleep(MEB_HOST_UART_RX_GPIO), TAG, "failed to keep UART RX GPIO active in sleep");
    ESP_RETURN_ON_ERROR(keep_gpio_active_in_light_sleep(MEB_TWAI_TX_GPIO), TAG, "failed to keep TWAI TX GPIO active in sleep");
    ESP_RETURN_ON_ERROR(keep_gpio_active_in_light_sleep(MEB_TWAI_RX_GPIO), TAG, "failed to keep TWAI RX GPIO active in sleep");
    ESP_LOGI(TAG, "kept app GPIOs active during automatic light sleep");
#endif

    return ESP_OK;
}

static esp_err_t set_debug_pm_lock(bool hold)
{
    if (!s_debug_pm_lock || hold == s_debug_pm_lock_held) {
        return ESP_OK;
    }

    esp_err_t err = hold ? esp_pm_lock_acquire(s_debug_pm_lock) : esp_pm_lock_release(s_debug_pm_lock);
    if (err == ESP_OK) {
        s_debug_pm_lock_held = hold;
    }

    return err;
}
#endif

#if MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED
static void debug_pm_task(void *arg)
{
    const int64_t grace_until_us = esp_timer_get_time() + ((int64_t)MEB_PM_JTAG_BOOT_GRACE_MS * 1000);
    const TickType_t poll_ticks = pdMS_TO_TICKS(MEB_PM_DEBUG_POLL_MS);
    bool last_debugger_attached = false;

    (void)arg;

    while (1) {
        const bool debugger_attached = esp_cpu_dbgr_is_attached();
        const bool in_boot_grace = esp_timer_get_time() < grace_until_us;

        if (debugger_attached != last_debugger_attached) {
            ESP_LOGI(TAG, "JTAG debugger %s", debugger_attached ? "attached" : "detached");
            last_debugger_attached = debugger_attached;
        }

        esp_err_t err = set_debug_pm_lock(debugger_attached || in_boot_grace);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to update debug PM lock: %s", esp_err_to_name(err));
        }

        vTaskDelay(poll_ticks > 0 ? poll_ticks : 1);
    }
}

static esp_err_t start_debug_pm_monitor(void)
{
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "debug", &s_debug_pm_lock),
                        TAG, "failed to create debug PM lock");
    ESP_RETURN_ON_ERROR(set_debug_pm_lock(true), TAG, "failed to acquire debug PM lock");

    BaseType_t ok = xTaskCreate(debug_pm_task, "debug_pm", DEBUG_PM_TASK_STACK_SIZE, NULL,
                                DEBUG_PM_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create debug PM task");

    ESP_LOGI(TAG, "automatic light sleep held for %d ms to allow JTAG attach", MEB_PM_JTAG_BOOT_GRACE_MS);
    return ESP_OK;
}
#endif

esp_err_t meb_power_management_init(void)
{
#if CONFIG_PM_ENABLE
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = MEB_PM_MAX_CPU_FREQ_MHZ,
        .min_freq_mhz = MEB_PM_MIN_CPU_FREQ_MHZ,
#if MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };

    ESP_RETURN_ON_ERROR(esp_pm_configure(&pm_config), TAG, "failed to configure power management");
#if MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED
    ESP_RETURN_ON_ERROR(configure_light_sleep_gpio_behavior(), TAG, "failed to configure light sleep GPIO behavior");
    ESP_RETURN_ON_ERROR(start_debug_pm_monitor(), TAG, "failed to start debug PM monitor");
#endif
    ESP_LOGI(TAG, "power management enabled: CPU %d-%d MHz, automatic light sleep %s, BLE sleep %s",
             MEB_PM_MIN_CPU_FREQ_MHZ,
             MEB_PM_MAX_CPU_FREQ_MHZ,
#if MEB_PM_LIGHT_SLEEP_RUNTIME_ENABLED
             "enabled",
#else
             "disabled",
#endif
#if CONFIG_BT_LE_SLEEP_ENABLE
             "enabled"
#else
             "disabled"
#endif
    );
#else
    ESP_LOGW(TAG, "power management disabled in sdkconfig");
#endif

    return ESP_OK;
}
