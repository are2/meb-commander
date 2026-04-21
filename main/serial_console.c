#include "serial_console.h"
#include "app_config.h"
#include "app_state.h"
#include "ble_console.h"
#include "control.h"
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
    char buf[1024];
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

    char result[640];
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
    if (state->auto_off_remaining_valid) {
        snprintf(buf, buf_len, "%" PRIu32, state->auto_off_remaining_minutes);
    } else {
        snprintf(buf, buf_len, "null");
    }
}

static void send_heating_state_result(bool has_id, const char *id_token)
{
    meb_state_snapshot_t state;
    meb_state_get_snapshot(&state);
    char remaining_minutes[16];
    format_auto_off_remaining(&state, remaining_minutes, sizeof(remaining_minutes));

    send_rpc_result(has_id, id_token,
                    "{\"heating_enabled\":%s,\"active\":%u,\"request\":%u,\"cooling_request\":%u,"
                    "\"power_w\":%u,\"power_req_w\":%u,\"temperature_status\":%u,"
                    "\"auto_off_timer_enabled\":%s,\"auto_off_timer_minutes\":%" PRIu32 ","
                    "\"auto_off_remaining_minutes\":%s}",
                    state.heating_enabled ? "true" : "false",
                    state.battery_heating_active,
                    state.heating_request,
                    state.cooling_request,
                    state.power_battery_heating_watt,
                    state.power_battery_heating_req_watt,
                    state.temperature_status_charge,
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
