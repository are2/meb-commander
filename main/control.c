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

static void format_u8_or_null(bool valid, uint8_t value, char *buf, size_t buf_len)
{
    if (valid) {
        snprintf(buf, buf_len, "%u", (unsigned)value);
    } else {
        snprintf(buf, buf_len, "null");
    }
}

static void format_u32_or_null(bool valid, uint32_t value, char *buf, size_t buf_len)
{
    if (valid) {
        snprintf(buf, buf_len, "%" PRIu32, value);
    } else {
        snprintf(buf, buf_len, "null");
    }
}

static void format_double_or_null(bool valid, double value, char *buf, size_t buf_len)
{
    if (valid) {
        snprintf(buf, buf_len, "%.1f", value);
    } else {
        snprintf(buf, buf_len, "null");
    }
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

        if (state.heating_enabled && state.temperature_status_valid &&
            state.temperature_status_charge == MEB_TEMPERATURE_STATUS_UNDER_OPTIMAL) {
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
        char heating_active[16];
        char heating_request[16];
        char cooling_request[16];
        char heating_power_w[16];
        char heating_power_req_w[16];
        char temperature_status[16];
        char max_charge_power_kw[16];
        char max_charge_current_a[16];
        char battery_min_temp_c[16];
        char battery_max_temp_c[16];

        format_u32_or_null(state.auto_off_remaining_valid, state.auto_off_remaining_minutes,
                           remaining_minutes, sizeof(remaining_minutes));
        format_double_or_null(state.bms_soc_valid, state.bms_soc_percent,
                              bms_soc_percent, sizeof(bms_soc_percent));
        format_u8_or_null(state.heating_status_valid, state.battery_heating_active,
                          heating_active, sizeof(heating_active));
        format_u8_or_null(state.heating_status_valid, state.heating_request,
                          heating_request, sizeof(heating_request));
        format_u8_or_null(state.heating_status_valid, state.cooling_request,
                          cooling_request, sizeof(cooling_request));
        format_u8_or_null(state.heating_status_valid, state.power_battery_heating_watt,
                          heating_power_w, sizeof(heating_power_w));
        format_u8_or_null(state.heating_status_valid, state.power_battery_heating_req_watt,
                          heating_power_req_w, sizeof(heating_power_req_w));
        format_u8_or_null(state.temperature_status_valid, state.temperature_status_charge,
                          temperature_status, sizeof(temperature_status));
        format_double_or_null(state.charge_limits_valid, state.max_charge_power_kw,
                              max_charge_power_kw, sizeof(max_charge_power_kw));
        format_double_or_null(state.charge_limits_valid, state.max_charge_current_amp,
                              max_charge_current_a, sizeof(max_charge_current_a));
        format_double_or_null(state.battery_temperature_valid, state.battery_min_temp,
                              battery_min_temp_c, sizeof(battery_min_temp_c));
        format_double_or_null(state.battery_temperature_valid, state.battery_max_temp,
                              battery_max_temp_c, sizeof(battery_max_temp_c));

        meb_serial_printf("{"
                          "\"v\":%d,"
                          "\"type\":\"telemetry\","
                          "\"ts_ms\":%llu,"
                          "\"battery\":{"
                              "\"soc_bms_percent\":%s"
                          "},"
                          "\"heating\":{"
                              "\"active\":%s,"
                              "\"request\":%s,"
                              "\"cooling_request\":%s,"
                              "\"power_w\":%s,"
                              "\"power_req_w\":%s"
                          "},"
                          "\"thermal\":{"
                              "\"status\":%s"
                          "},"
                          "\"predicted\":{"
                              "\"power_kw\":%s,"
                              "\"current_a\":%s"
                          "},"
                          "\"battery_temp_c\":{"
                              "\"min\":%s,"
                              "\"max\":%s"
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
                          heating_active,
                          heating_request,
                          cooling_request,
                          heating_power_w,
                          heating_power_req_w,
                          temperature_status,
                          max_charge_power_kw,
                          max_charge_current_a,
                          battery_min_temp_c,
                          battery_max_temp_c,
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
