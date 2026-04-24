#include "serial_console.h"
#include "app_config.h"
#include "app_state.h"
#include "ble_console.h"
#include "can_bus.h"
#include "control.h"
#include "diagnostics.h"
#include "ota_update.h"

#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CMD_RX_BUF_SIZE 1024
#define HOST_UART_NUM UART_NUM_0
#define HOST_UART_RX_BUF_SIZE 1024
#define HOST_UART_TX_BUF_SIZE 2048
#define HOST_TASK_STACK_SIZE 8192
#define HOST_TASK_PRIORITY 5
#define JSONRPC_ID_BUF_SIZE 64
#define JSONRPC_METHOD_BUF_SIZE 48
#define JSONRPC_RESULT_BUF_SIZE 1536
#define SERIAL_OUTPUT_BUF_SIZE 2048
#define DIAG_EVENTS_DEFAULT_LIMIT 8
#define DIAG_EVENTS_DIAGNOSTICS_LIMIT 5
#define DIAG_EVENTS_MAX_LIMIT 8
#define DEVICE_RESET_DELAY_MS 500

#define JSONRPC_PARSE_ERROR -32700
#define JSONRPC_INVALID_REQUEST -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS -32602
#define JSONRPC_SERVER_ERROR -32000

static const char *TAG = "serial_console";
static SemaphoreHandle_t s_uart_write_lock;

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }

    return p;
}

static bool is_value_delimiter(char c)
{
    return c == '\0' || c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *find_key_value(const char *json, const char *key)
{
    char pattern[40];
    int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    if (pattern_len <= 0 || pattern_len >= (int)sizeof(pattern)) {
        return NULL;
    }

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *value = skip_ws(p + pattern_len);
        if (*value == ':') {
            return skip_ws(value + 1);
        }
        p += pattern_len;
    }

    return NULL;
}

static bool parse_string_value(const char *value, char *out, size_t out_len)
{
    if (!value || *value != '"' || out_len == 0) {
        return false;
    }

    value++;
    size_t out_pos = 0;

    while (*value) {
        if (*value == '"') {
            out[out_pos] = '\0';
            return true;
        }

        if (*value == '\\') {
            value++;
            if (!*value) {
                return false;
            }
        }

        if (out_pos >= out_len - 1) {
            return false;
        }

        out[out_pos++] = *value++;
    }

    return false;
}

static bool parse_bool_value(const char *value, bool *out)
{
    if (!value || !out) {
        return false;
    }

    if (strncmp(value, "true", 4) == 0) {
        if (!is_value_delimiter(value[4])) {
            return false;
        }
        *out = true;
        return true;
    }

    if (strncmp(value, "false", 5) == 0) {
        if (!is_value_delimiter(value[5])) {
            return false;
        }
        *out = false;
        return true;
    }

    return false;
}

static bool parse_uint_value(const char *value, uint32_t *out)
{
    if (!value || !out || *value < '0' || *value > '9') {
        return false;
    }

    uint32_t parsed = 0;
    while (*value >= '0' && *value <= '9') {
        uint32_t digit = (uint32_t)(*value - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
        value++;
    }

    if (!is_value_delimiter(*value)) {
        return false;
    }

    *out = parsed;
    return true;
}

static bool parse_id_token(const char *json, char *id_token, size_t id_token_len, bool *has_id)
{
    const char *value = find_key_value(json, "id");
    *has_id = value != NULL;

    if (!value) {
        snprintf(id_token, id_token_len, "null");
        return true;
    }

    size_t out_pos = 0;

    if (*value == '"') {
        do {
            if (out_pos >= id_token_len - 1) {
                return false;
            }
            id_token[out_pos++] = *value;
            if (*value == '\\' && value[1]) {
                value++;
                if (out_pos >= id_token_len - 1) {
                    return false;
                }
                id_token[out_pos++] = *value;
            } else if (*value == '"' && out_pos > 1) {
                id_token[out_pos] = '\0';
                return true;
            }
            value++;
        } while (*value);

        return false;
    }

    while (*value && *value != ',' && *value != '}' && *value != ' ' && *value != '\t' &&
           *value != '\r' && *value != '\n') {
        if (out_pos >= id_token_len - 1) {
            return false;
        }
        id_token[out_pos++] = *value++;
    }

    if (out_pos == 0) {
        return false;
    }

    id_token[out_pos] = '\0';
    return strcmp(id_token, "null") == 0 || id_token[0] == '-' || (id_token[0] >= '0' && id_token[0] <= '9');
}

static bool parse_required_string_key(const char *json, const char *key, char *out, size_t out_len)
{
    return parse_string_value(find_key_value(json, key), out, out_len);
}

void meb_serial_printf(const char *fmt, ...)
{
    char buf[SERIAL_OUTPUT_BUF_SIZE];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    if (s_uart_write_lock) {
        xSemaphoreTake(s_uart_write_lock, portMAX_DELAY);
    }
    if (uart_is_driver_installed(HOST_UART_NUM)) {
        (void)uart_write_bytes(HOST_UART_NUM, buf, len);
    }
    meb_ble_console_write(buf, (size_t)len);
    if (s_uart_write_lock) {
        xSemaphoreGive(s_uart_write_lock);
    }
}

static void send_rpc_error(const char *id_token, int code, const char *message)
{
    meb_serial_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}\n",
                      id_token ? id_token : "null", code, message);
}

static void send_rpc_result(bool has_id, const char *id_token, const char *result_fmt, ...)
{
    if (!has_id) {
        return;
    }

    char result[JSONRPC_RESULT_BUF_SIZE];
    va_list args;

    va_start(args, result_fmt);
    int len = vsnprintf(result, sizeof(result), result_fmt, args);
    va_end(args);

    if (len < 0) {
        send_rpc_error(id_token, JSONRPC_INVALID_REQUEST, "Internal response formatting error");
        return;
    }
    if ((size_t)len >= sizeof(result)) {
        send_rpc_error(id_token, JSONRPC_INVALID_REQUEST, "Response too large");
        return;
    }

    meb_serial_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id_token, result);
}

static void format_auto_off_remaining(const meb_state_snapshot_t *state, char *buf, size_t buf_len)
{
    if (state->auto_off_remaining_minutes.valid) {
        snprintf(buf, buf_len, "%" PRIu32, state->auto_off_remaining_minutes.value);
    } else {
        snprintf(buf, buf_len, "null");
    }
}

static void format_u8_or_null(bool valid, uint8_t value, char *buf, size_t buf_len)
{
    if (valid) {
        snprintf(buf, buf_len, "%u", (unsigned)value);
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

static void send_heating_state_result(bool has_id, const char *id_token)
{
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

    format_auto_off_remaining(&state, remaining_minutes, sizeof(remaining_minutes));
    format_double_or_null(state.bms_soc_percent.valid, state.bms_soc_percent.value,
                          bms_soc_percent, sizeof(bms_soc_percent));
    format_u8_or_null(state.heating_status.valid, state.heating_status.active,
                      heating_active, sizeof(heating_active));
    format_u8_or_null(state.heating_status.valid, state.heating_status.request,
                      heating_request, sizeof(heating_request));
    format_u8_or_null(state.heating_status.valid, state.heating_status.cooling_request,
                      cooling_request, sizeof(cooling_request));
    format_u8_or_null(state.heating_status.valid, state.heating_status.power_w,
                      heating_power_w, sizeof(heating_power_w));
    format_u8_or_null(state.heating_status.valid, state.heating_status.power_req_w,
                      heating_power_req_w, sizeof(heating_power_req_w));
    format_u8_or_null(state.temperature_status_charge.valid,
                      state.temperature_status_charge.value,
                      temperature_status, sizeof(temperature_status));

    send_rpc_result(has_id, id_token,
                    "{\"heating_enabled\":%s,\"active\":%s,\"request\":%s,\"cooling_request\":%s,"
                    "\"power_w\":%s,\"power_req_w\":%s,\"temperature_status\":%s,"
                    "\"soc_bms_percent\":%s,"
                    "\"auto_off_timer_enabled\":%s,\"auto_off_timer_minutes\":%" PRIu32 ","
                    "\"auto_off_remaining_minutes\":%s}",
                    state.heating_enabled ? "true" : "false",
                    heating_active,
                    heating_request,
                    cooling_request,
                    heating_power_w,
                    heating_power_req_w,
                    temperature_status,
                    bms_soc_percent,
                    state.auto_off_timer_enabled ? "true" : "false",
                    state.auto_off_timer_minutes,
                    remaining_minutes);
}

static void handle_heating_set(bool has_id, const char *id_token, const char *cmd)
{
    bool enabled = false;
    const char *params = find_key_value(cmd, "params");

    if (!params || *params != '{' || !parse_bool_value(find_key_value(params, "enabled"), &enabled)) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.enabled boolean");
        return;
    }

    meb_state_set_heating_enabled(enabled);
    send_heating_state_result(has_id, id_token);
}

static void handle_heating_get(bool has_id, const char *id_token)
{
    send_heating_state_result(has_id, id_token);
}

static void handle_heating_set_auto_off_timer(bool has_id, const char *id_token, const char *cmd)
{
    uint32_t minutes = 0;
    const char *params = find_key_value(cmd, "params");

    if (!params || *params != '{' || !parse_uint_value(find_key_value(params, "minutes"), &minutes)) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.minutes integer; 0 disables timer");
        return;
    }

    esp_err_t err = meb_state_set_auto_off_timer_minutes(minutes);
    if (err != ESP_OK) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "params.minutes outside allowed range");
        return;
    }

    send_heating_state_result(has_id, id_token);
}

static bool json_appendf(char *buf, size_t buf_len, size_t *pos, const char *fmt, ...)
{
    if (!buf || !pos || *pos >= buf_len) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf + *pos, buf_len - *pos, fmt, args);
    va_end(args);

    if (len < 0 || (size_t)len >= buf_len - *pos) {
        return false;
    }

    *pos += (size_t)len;
    return true;
}

static bool parse_optional_limit(const char *cmd, uint32_t default_limit, uint32_t *limit)
{
    if (!limit) {
        return false;
    }

    *limit = default_limit;
    const char *params = find_key_value(cmd, "params");
    if (!params) {
        return true;
    }
    if (*params != '{') {
        return false;
    }

    const char *limit_value = find_key_value(params, "limit");
    if (!limit_value) {
        return true;
    }

    return parse_uint_value(limit_value, limit);
}

static bool format_event_log_json(char *buf, size_t buf_len, uint32_t limit)
{
    meb_diag_event_snapshot_t snapshot;
    meb_diag_get_events(&snapshot);

    if (limit > DIAG_EVENTS_MAX_LIMIT) {
        limit = DIAG_EVENTS_MAX_LIMIT;
    }
    if (limit > snapshot.count) {
        limit = snapshot.count;
    }

    size_t pos = 0;
    uint32_t start = snapshot.count > limit ? snapshot.count - limit : 0;

    if (!json_appendf(buf, buf_len, &pos,
                      "{\"capacity\":%" PRIu32 ",\"count\":%" PRIu32 ",\"returned\":%" PRIu32
                      ",\"overwritten\":%" PRIu32 ",\"events\":[",
                      snapshot.capacity,
                      snapshot.count,
                      limit,
                      snapshot.overwritten)) {
        return false;
    }

    for (uint32_t i = start; i < snapshot.count; i++) {
        const meb_diag_event_t *event = &snapshot.events[i];
        char component[(MEB_DIAG_COMPONENT_LEN * 2) + 1];
        char name[(MEB_DIAG_EVENT_LEN * 2) + 1];
        char detail[(MEB_DIAG_DETAIL_LEN * 2) + 1];

        meb_diag_json_escape(event->component, component, sizeof(component));
        meb_diag_json_escape(event->event, name, sizeof(name));
        meb_diag_json_escape(event->detail, detail, sizeof(detail));

        if (i > start && !json_appendf(buf, buf_len, &pos, ",")) {
            return false;
        }

        if (!json_appendf(buf, buf_len, &pos,
                          "{\"seq\":%" PRIu32 ",\"ts_ms\":%" PRIu64
                          ",\"c\":\"%s\",\"e\":\"%s\",\"d\":\"%s\","
                          "\"heap\":%" PRIu32 ",\"min_heap\":%" PRIu32 "}",
                          event->seq,
                          event->uptime_ms,
                          component,
                          name,
                          detail,
                          event->free_heap,
                          event->minimum_free_heap)) {
            return false;
        }
    }

    return json_appendf(buf, buf_len, &pos, "]}");
}

static void handle_device_uptime(bool has_id, const char *id_token)
{
    meb_diag_status_t status;
    meb_diag_get_status(&status);

    send_rpc_result(has_id, id_token,
                    "{\"uptime_ms\":%" PRIu64 ",\"reset\":{\"reason\":%d,\"name\":\"%s\"}}",
                    status.uptime_ms,
                    (int)status.reset_reason,
                    meb_diag_reset_reason_name(status.reset_reason));
}

static void handle_device_reset(bool has_id, const char *id_token)
{
    esp_err_t err = meb_ota_update_schedule_reboot(DEVICE_RESET_DELAY_MS);
    if (err != ESP_OK) {
        send_rpc_error(id_token, JSONRPC_SERVER_ERROR, "Failed to schedule device reset");
        return;
    }

    send_rpc_result(has_id, id_token, "{\"resetting\":true,\"delay_ms\":%u}", DEVICE_RESET_DELAY_MS);
}

static void handle_device_events(bool has_id, const char *id_token, const char *cmd)
{
    uint32_t limit = DIAG_EVENTS_DEFAULT_LIMIT;
    if (!parse_optional_limit(cmd, DIAG_EVENTS_DEFAULT_LIMIT, &limit)) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.limit integer");
        return;
    }

    char event_log[1200];
    if (!format_event_log_json(event_log, sizeof(event_log), limit)) {
        send_rpc_error(id_token, JSONRPC_SERVER_ERROR, "Diagnostics event response too large");
        return;
    }

    send_rpc_result(has_id, id_token, "%s", event_log);
}

static void handle_device_diagnostics(bool has_id, const char *id_token, const char *cmd)
{
    uint32_t limit = DIAG_EVENTS_DIAGNOSTICS_LIMIT;
    if (!parse_optional_limit(cmd, DIAG_EVENTS_DIAGNOSTICS_LIMIT, &limit)) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.limit integer");
        return;
    }
    if (limit > DIAG_EVENTS_DIAGNOSTICS_LIMIT) {
        limit = DIAG_EVENTS_DIAGNOSTICS_LIMIT;
    }

    meb_diag_status_t status;
    meb_diag_get_status(&status);

    char event_log[900];
    if (!format_event_log_json(event_log, sizeof(event_log), limit)) {
        send_rpc_error(id_token, JSONRPC_SERVER_ERROR, "Diagnostics event response too large");
        return;
    }

    send_rpc_result(has_id, id_token,
                    "{\"uptime_ms\":%" PRIu64 ",\"reset\":{\"reason\":%d,\"name\":\"%s\"},"
                    "\"heap\":{\"free\":%" PRIu32 ",\"minimum_free\":%" PRIu32
                    ",\"free_8bit\":%" PRIu32 ",\"minimum_free_8bit\":%" PRIu32
                    ",\"largest_free_8bit_block\":%" PRIu32 "},\"event_log\":%s}",
                    status.uptime_ms,
                    (int)status.reset_reason,
                    meb_diag_reset_reason_name(status.reset_reason),
                    status.free_heap,
                    status.minimum_free_heap,
                    status.free_8bit_heap,
                    status.minimum_free_8bit_heap,
                    status.largest_free_8bit_block,
                    event_log);
}

static const char *can_state_name(uint32_t state)
{
    switch ((twai_error_state_t)state) {
    case TWAI_ERROR_ACTIVE:
        return "active";
    case TWAI_ERROR_WARNING:
        return "warning";
    case TWAI_ERROR_PASSIVE:
        return "passive";
    case TWAI_ERROR_BUS_OFF:
        return "bus_off";
    default:
        return "unknown";
    }
}

static void handle_device_can_diagnostics(bool has_id, const char *id_token)
{
    meb_can_diagnostics_t diag;
    meb_can_get_diagnostics(&diag);

    send_rpc_result(has_id, id_token,
                    "{"
                    "\"ready\":%s,"
                    "\"timing\":{\"arb_sample_point_permill\":%" PRIu32 ","
                    "\"data_sample_point_permill\":%" PRIu32 ","
                    "\"data_ssp_permill\":%" PRIu32 "},"
                    "\"node\":{\"state\":{\"value\":%u,\"name\":\"%s\"},"
                    "\"tx_error_count\":%u,\"rx_error_count\":%u,"
                    "\"tx_queue_remaining\":%" PRIu32 ","
                    "\"bus_error_count\":%" PRIu32 "},"
                    "\"counters\":{\"rx_queue_overflow\":%" PRIu32 ","
                    "\"error_events\":%" PRIu32 ","
                    "\"state_changes\":%" PRIu32 ","
                    "\"tx_failures\":%" PRIu32 "},"
                    "\"last\":{\"error_flags\":%" PRIu32 ","
                    "\"tx_failure_id\":%" PRIu32 "},"
                    "\"state_entries\":{\"active\":%" PRIu32 ","
                    "\"warning\":%" PRIu32 ","
                    "\"passive\":%" PRIu32 ","
                    "\"bus_off\":%" PRIu32 "}"
                    "}",
                    diag.ready ? "true" : "false",
                    diag.arbitration_sample_point_permill,
                    diag.data_sample_point_permill,
                    diag.data_ssp_permill,
                    (unsigned)diag.node_status.state,
                    can_state_name((uint32_t)diag.node_status.state),
                    (unsigned)diag.node_status.tx_error_count,
                    (unsigned)diag.node_status.rx_error_count,
                    diag.node_status.tx_queue_remaining,
                    diag.node_record.bus_err_num,
                    diag.rx_queue_overflow_count,
                    diag.error_event_count,
                    diag.state_change_count,
                    diag.tx_failure_count,
                    diag.last_error_flags,
                    diag.last_tx_failure_id,
                    diag.state_entry_count[TWAI_ERROR_ACTIVE],
                    diag.state_entry_count[TWAI_ERROR_WARNING],
                    diag.state_entry_count[TWAI_ERROR_PASSIVE],
                    diag.state_entry_count[TWAI_ERROR_BUS_OFF]);
}

static void handle_device_info(bool has_id, const char *id_token)
{
    send_rpc_result(has_id, id_token,
                    "{\"version\":\"%s\",\"about\":\"%s\",\"protocol_version\":%d,"
                    "\"release_build\":%s,\"build_mode\":\"%s\","
                    "\"serial\":{\"uart\":0,\"baud_rate\":%d,\"tx_gpio\":%d,\"rx_gpio\":%d},"
                    "\"ble\":{\"name\":\"%s\",\"service_uuid\":\"%s\",\"rx_uuid\":\"%s\",\"tx_uuid\":\"%s\"},"
                    "\"telemetry_interval_ms\":%u,"
                    "\"heating\":{\"safety_auto_off_enabled\":%s,\"safety_auto_off_minutes\":%d},"
                    "\"can\":{\"tx_gpio\":%d,\"rx_gpio\":%d,"
                    "\"bitrate\":%d,\"data_bitrate\":%d}}",
                    MEB_APP_VERSION,
                    MEB_APP_ABOUT,
                    MEB_PREHEATER_PROTOCOL_VERSION,
                    MEB_RELEASE_BUILD ? "true" : "false",
                    MEB_BUILD_MODE,
                    MEB_HOST_UART_BAUD_RATE,
                    MEB_HOST_UART_TX_GPIO,
                    MEB_HOST_UART_RX_GPIO,
                    MEB_BLE_DEVICE_NAME,
                    MEB_BLE_SERVICE_UUID,
                    MEB_BLE_RX_UUID,
                    MEB_BLE_TX_UUID,
                    meb_control_get_telemetry_interval_ms(),
                    MEB_SAFETY_AUTO_OFF_ENABLED ? "true" : "false",
                    (int)MEB_SAFETY_AUTO_OFF_MINUTES,
                    MEB_TWAI_TX_GPIO,
                    MEB_TWAI_RX_GPIO,
                    MEB_TWAI_BITRATE,
                    MEB_TWAI_DATA_BITRATE);
}

static void handle_telemetry_set_interval(bool has_id, const char *id_token, const char *cmd)
{
    uint32_t interval_ms = 0;
    const char *params = find_key_value(cmd, "params");

    if (!params || *params != '{' || !parse_uint_value(find_key_value(params, "ms"), &interval_ms)) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.ms integer");
        return;
    }

    esp_err_t err = meb_control_set_telemetry_interval_ms(interval_ms);
    if (err != ESP_OK) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "params.ms outside allowed range");
        return;
    }

    send_rpc_result(has_id, id_token, "{\"interval_ms\":%u}", interval_ms);
}

static void send_ota_error(const char *id_token, esp_err_t err)
{
    send_rpc_error(id_token, JSONRPC_SERVER_ERROR, meb_ota_update_err_message(err));
}

static void format_ota_status_result(char *buf, size_t buf_len, const meb_ota_status_t *status)
{
    snprintf(buf, buf_len,
             "{\"active\":%s,\"pending_reboot\":%s,\"written\":%" PRIu32 ","
             "\"expected_size\":%" PRIu32 ",\"partition\":\"%s\","
             "\"partition_size\":%" PRIu32 ",\"has_sha256\":%s,"
             "\"max_base64_chars\":%u}",
             status->active ? "true" : "false",
             status->pending_reboot ? "true" : "false",
             status->written,
             status->expected_size,
             status->partition_label,
             status->partition_size,
             status->has_sha256 ? "true" : "false",
             MEB_OTA_MAX_BASE64_CHARS);
}

static void handle_firmware_begin(bool has_id, const char *id_token, const char *cmd)
{
    uint32_t size = 0;
    char sha256[65];
    const char *params = find_key_value(cmd, "params");

    if (!params || *params != '{' ||
        !parse_uint_value(find_key_value(params, "size"), &size) ||
        !parse_string_value(find_key_value(params, "sha256"), sha256, sizeof(sha256))) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.size and params.sha256");
        return;
    }

    esp_err_t err = meb_ota_update_begin(size, sha256);
    if (err != ESP_OK) {
        send_ota_error(id_token, err);
        return;
    }

    meb_ota_status_t status;
    meb_ota_update_get_status(&status);
    char result[256];
    format_ota_status_result(result, sizeof(result), &status);
    send_rpc_result(has_id, id_token, "%s", result);
}

static void handle_firmware_write(bool has_id, const char *id_token, const char *cmd)
{
    uint32_t offset = 0;
    char data[MEB_OTA_MAX_BASE64_CHARS + 1];
    const char *params = find_key_value(cmd, "params");

    if (!params || *params != '{' ||
        !parse_uint_value(find_key_value(params, "offset"), &offset) ||
        !parse_string_value(find_key_value(params, "data"), data, sizeof(data))) {
        send_rpc_error(id_token, JSONRPC_INVALID_PARAMS, "Expected params.offset and params.data");
        return;
    }

    uint32_t decoded_len = 0;
    esp_err_t err = meb_ota_update_write(offset, data, &decoded_len);
    if (err != ESP_OK) {
        send_ota_error(id_token, err);
        return;
    }

    meb_ota_status_t status;
    meb_ota_update_get_status(&status);
    send_rpc_result(has_id, id_token,
                    "{\"received\":%" PRIu32 ",\"written\":%" PRIu32 ",\"expected_size\":%" PRIu32 "}",
                    decoded_len,
                    status.written,
                    status.expected_size);
}

static void handle_firmware_end(bool has_id, const char *id_token)
{
    esp_err_t err = meb_ota_update_end();
    if (err != ESP_OK) {
        send_ota_error(id_token, err);
        return;
    }

    meb_ota_status_t status;
    meb_ota_update_get_status(&status);
    char result[256];
    format_ota_status_result(result, sizeof(result), &status);
    send_rpc_result(has_id, id_token, "%s", result);
}

static void handle_firmware_cancel(bool has_id, const char *id_token)
{
    esp_err_t err = meb_ota_update_cancel();
    if (err != ESP_OK) {
        send_ota_error(id_token, err);
        return;
    }

    meb_ota_status_t status;
    meb_ota_update_get_status(&status);
    char result[256];
    format_ota_status_result(result, sizeof(result), &status);
    send_rpc_result(has_id, id_token, "%s", result);
}

static void handle_firmware_status(bool has_id, const char *id_token)
{
    meb_ota_status_t status;
    meb_ota_update_get_status(&status);
    char result[256];
    format_ota_status_result(result, sizeof(result), &status);
    send_rpc_result(has_id, id_token, "%s", result);
}

static void handle_firmware_reboot(bool has_id, const char *id_token)
{
    meb_ota_status_t status;
    meb_ota_update_get_status(&status);
    if (!status.pending_reboot) {
        send_ota_error(id_token, ESP_ERR_INVALID_STATE);
        return;
    }

    esp_err_t err = meb_ota_update_schedule_reboot(500);
    if (err != ESP_OK) {
        send_ota_error(id_token, err);
        return;
    }

    send_rpc_result(has_id, id_token, "{\"rebooting\":true}");
}

void meb_serial_console_process_command(const char *cmd)
{
    char id_token[JSONRPC_ID_BUF_SIZE];
    bool has_id = false;
    char jsonrpc[8];
    char method[JSONRPC_METHOD_BUF_SIZE];

    if (!cmd || *skip_ws(cmd) == '\0') {
        return;
    }

    if (*skip_ws(cmd) != '{') {
        return;
    }

    if (!parse_id_token(cmd, id_token, sizeof(id_token), &has_id)) {
        send_rpc_error("null", JSONRPC_INVALID_REQUEST, "Invalid id");
        return;
    }

    if (!parse_required_string_key(cmd, "jsonrpc", jsonrpc, sizeof(jsonrpc)) || strcmp(jsonrpc, "2.0") != 0 ||
        !parse_required_string_key(cmd, "method", method, sizeof(method))) {
        send_rpc_error(id_token, JSONRPC_INVALID_REQUEST, "Expected JSON-RPC 2.0 request");
        return;
    }

    if (strcmp(method, "heating.set") == 0) {
        handle_heating_set(has_id, id_token, cmd);
    } else if (strcmp(method, "heating.get") == 0) {
        handle_heating_get(has_id, id_token);
    } else if (strcmp(method, "heating.set_auto_off_timer") == 0) {
        handle_heating_set_auto_off_timer(has_id, id_token, cmd);
    } else if (strcmp(method, "device.info") == 0) {
        handle_device_info(has_id, id_token);
    } else if (strcmp(method, "device.uptime") == 0) {
        handle_device_uptime(has_id, id_token);
    } else if (strcmp(method, "device.reset") == 0) {
        handle_device_reset(has_id, id_token);
    } else if (strcmp(method, "device.diagnostics") == 0) {
        handle_device_diagnostics(has_id, id_token, cmd);
    } else if (strcmp(method, "device.can_diagnostics") == 0) {
        handle_device_can_diagnostics(has_id, id_token);
    } else if (strcmp(method, "device.events") == 0) {
        handle_device_events(has_id, id_token, cmd);
    } else if (strcmp(method, "telemetry.set_interval") == 0) {
        handle_telemetry_set_interval(has_id, id_token, cmd);
    } else if (strcmp(method, "firmware.begin") == 0) {
        handle_firmware_begin(has_id, id_token, cmd);
    } else if (strcmp(method, "firmware.write") == 0) {
        handle_firmware_write(has_id, id_token, cmd);
    } else if (strcmp(method, "firmware.end") == 0) {
        handle_firmware_end(has_id, id_token);
    } else if (strcmp(method, "firmware.cancel") == 0) {
        handle_firmware_cancel(has_id, id_token);
    } else if (strcmp(method, "firmware.status") == 0) {
        handle_firmware_status(has_id, id_token);
    } else if (strcmp(method, "firmware.reboot") == 0) {
        handle_firmware_reboot(has_id, id_token);
    } else {
        send_rpc_error(id_token, JSONRPC_METHOD_NOT_FOUND, "Method not found");
    }
}

static void uart_command_task(void *arg)
{
    (void)arg;
    char cmd_buf[CMD_RX_BUF_SIZE];
    size_t cmd_len = 0;

    while (1) {
        uint8_t chunk[64];
        int nread = uart_read_bytes(HOST_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(100));

        for (int i = 0; i < nread; i++) {
            uint8_t c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (cmd_len > 0) {
                    cmd_buf[cmd_len] = '\0';
                    meb_serial_console_process_command(cmd_buf);
                    cmd_len = 0;
                }
            } else if (cmd_len == 0) {
                if (c == '{') {
                    cmd_buf[cmd_len++] = (char)c;
                }
            } else if (cmd_len < (sizeof(cmd_buf) - 1)) {
                cmd_buf[cmd_len++] = (char)c;
            } else {
                cmd_buf[cmd_len] = '\0';
                meb_serial_console_process_command(cmd_buf);
                cmd_len = 0;
            }
        }
    }
}

esp_err_t meb_serial_console_init(void)
{
    s_uart_write_lock = xSemaphoreCreateMutex();
    if (!s_uart_write_lock) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t config = {
        .baud_rate = MEB_HOST_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(HOST_UART_NUM, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_set_pin(HOST_UART_NUM, MEB_HOST_UART_TX_GPIO, MEB_HOST_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    if (!uart_is_driver_installed(HOST_UART_NUM)) {
        err = uart_driver_install(HOST_UART_NUM, HOST_UART_RX_BUF_SIZE, HOST_UART_TX_BUF_SIZE, 0, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    uart_flush_input(HOST_UART_NUM);

    BaseType_t ok = xTaskCreate(uart_command_task, "uart_cmd", HOST_TASK_STACK_SIZE, NULL, HOST_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART0 JSON-RPC command interface ready on TX GPIO %d / RX GPIO %d at %d bit/s",
             MEB_HOST_UART_TX_GPIO, MEB_HOST_UART_RX_GPIO, MEB_HOST_UART_BAUD_RATE);
    return ESP_OK;
}
