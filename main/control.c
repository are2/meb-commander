#include "control.h"
#include "app_config.h"
#include "app_state.h"
#include "can_bus.h"
#include "diagnostics.h"
#include "serial_console.h"

#include <inttypes.h>
#include <stdio.h>

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

    meb_diag_record_eventf("control", "telemetry_interval", "ms=%" PRIu32, interval_ms);
    return ESP_OK;
}

static void control_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(MEB_CONTROL_INITIAL_DELAY_MS));

    while (1) {
        meb_heating_auto_off_reason_t auto_off_reason = meb_state_apply_auto_off();
        if (auto_off_reason != MEB_HEATING_AUTO_OFF_NONE) {
            const char *reason = auto_off_reason == MEB_HEATING_AUTO_OFF_TIMER ? "timer" : "safety";
            meb_diag_record_eventf("control", "heating_auto_off", "reason=%s", reason);
            meb_serial_printf("{\"v\":%d,\"type\":\"control.heating_auto_off\",\"reason\":\"%s\"}\n",
                              MEB_PREHEATER_PROTOCOL_VERSION,
                              reason);
        }

        meb_state_snapshot_t state;
        meb_state_get_snapshot(&state);

        if (state.heating_enabled && state.temperature_status_charge == MEB_TEMPERATURE_STATUS_UNDER_OPTIMAL) {
            if (meb_state_take_session_error()) {
                meb_diag_record_event("control", "diag_session_retry", "");
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
    int64_t last_soc_request_us = -((int64_t)MEB_BMS_SOC_POLL_PERIOD_MS * 1000LL);

    while (1) {
        uint32_t interval_ms = meb_control_get_telemetry_interval_ms();
        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_soc_request_us) >= ((int64_t)MEB_BMS_SOC_POLL_PERIOD_MS * 1000LL)) {
            if (meb_can_request_bms_soc() == ESP_OK) {
                last_soc_request_us = now_us;
            }
        }

        meb_state_snapshot_t state;
        meb_state_get_snapshot(&state);
        char remaining_minutes[16];
        char bms_soc_percent[16];

        if (state.auto_off_remaining_valid) {
            snprintf(remaining_minutes, sizeof(remaining_minutes), "%" PRIu32, state.auto_off_remaining_minutes);
        } else {
            snprintf(remaining_minutes, sizeof(remaining_minutes), "null");
        }

        if (state.bms_soc_valid) {
            snprintf(bms_soc_percent, sizeof(bms_soc_percent), "%.1f", state.bms_soc_percent);
        } else {
            snprintf(bms_soc_percent, sizeof(bms_soc_percent), "null");
        }

        meb_serial_printf("{"
                          "\"v\":%d,"
                          "\"type\":\"telemetry\","
                          "\"ts_ms\":%llu,"
                          "\"battery\":{"
                              "\"soc_bms_percent\":%s"
                          "},"
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
                              "\"heating_enabled\":%s,"
                              "\"auto_off_timer_minutes\":%" PRIu32 ","
                              "\"auto_off_remaining_minutes\":%s"
                          "}"
                          "}\n",
                          MEB_PREHEATER_PROTOCOL_VERSION,
                          (unsigned long long)(esp_timer_get_time() / 1000ULL),
                          bms_soc_percent,
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
                          state.heating_enabled ? "true" : "false",
                          state.auto_off_timer_minutes,
                          remaining_minutes);

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
