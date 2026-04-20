#include "control.h"
#include "app_config.h"
#include "app_state.h"
#include "can_bus.h"
#include "serial_console.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONTROL_TASK_STACK_SIZE 4096
#define CONTROL_TASK_PRIORITY 6
#define TELEMETRY_TASK_STACK_SIZE 4096
#define TELEMETRY_TASK_PRIORITY 4

static const char *TAG = "control";
static portMUX_TYPE s_telemetry_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_telemetry_interval_ms = MEB_TELEMETRY_PERIOD_MS;

uint32_t meb_control_get_telemetry_interval_ms(void)
{
    uint32_t interval_ms;

    taskENTER_CRITICAL(&s_telemetry_lock);
    interval_ms = s_telemetry_interval_ms;
    taskEXIT_CRITICAL(&s_telemetry_lock);

    return interval_ms;
}

esp_err_t meb_control_set_telemetry_interval_ms(uint32_t interval_ms)
{
    if (interval_ms < MEB_TELEMETRY_MIN_PERIOD_MS || interval_ms > MEB_TELEMETRY_MAX_PERIOD_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_telemetry_lock);
    s_telemetry_interval_ms = interval_ms;
    taskEXIT_CRITICAL(&s_telemetry_lock);

    return ESP_OK;
}

static void control_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(MEB_CONTROL_INITIAL_DELAY_MS));

    while (1) {
        meb_state_snapshot_t state;
        meb_state_get_snapshot(&state);

        if (state.heating_enabled && state.temperature_status_charge == MEB_TEMPERATURE_STATUS_UNDER_OPTIMAL) {
            if (meb_state_take_session_error()) {
                meb_serial_printf("{\"v\":%d,\"type\":\"control.diag_session_retry\"}\n", MEB_PREHEATER_PROTOCOL_VERSION);
                (void)meb_can_request_diag_session();
            } else {
                (void)meb_can_send_heat_request();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MEB_CONTROL_PERIOD_MS));
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t interval_ms = meb_control_get_telemetry_interval_ms();
        meb_state_snapshot_t state;
        meb_state_get_snapshot(&state);

        meb_serial_printf("{"
                          "\"v\":%d,"
                          "\"type\":\"telemetry\","
                          "\"ts_ms\":%llu,"
                          "\"heating\":{"
                              "\"active\":%u,"
                              "\"request\":%u,"
                              "\"cooling_request\":%u,"
                              "\"power_w\":%u,"
                              "\"power_req_w\":%u"
                          "},"
                          "\"thermal\":{"
                              "\"status\":%u"
                          "},"
                          "\"predicted\":{"
                              "\"power_kw\":%.1f,"
                              "\"current_a\":%.1f"
                          "},"
                          "\"battery_temp_c\":{"
                              "\"min\":%.1f,"
                              "\"max\":%.1f"
                          "},"
                          "\"control\":{"
                              "\"heating_enabled\":%s"
                          "}"
                          "}\n",
                          MEB_PREHEATER_PROTOCOL_VERSION,
                          (unsigned long long)(esp_timer_get_time() / 1000ULL),
                          state.battery_heating_active,
                          state.heating_request,
                          state.cooling_request,
                          state.power_battery_heating_watt,
                          state.power_battery_heating_req_watt,
                          state.temperature_status_charge,
                          state.max_charge_power_kw,
                          state.max_charge_current_amp,
                          state.battery_min_temp,
                          state.battery_max_temp,
                          state.heating_enabled ? "true" : "false");

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

esp_err_t meb_control_start(void)
{
    BaseType_t ok = xTaskCreate(control_task, "control", CONTROL_TASK_STACK_SIZE, NULL, CONTROL_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(telemetry_task, "telemetry", TELEMETRY_TASK_STACK_SIZE, NULL, TELEMETRY_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create telemetry task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
