#include "usb_console.h"
#include "app_config.h"
#include "app_state.h"
#include "control.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CMD_RX_BUF_SIZE 256
#define USB_TASK_STACK_SIZE 4096
#define USB_TASK_PRIORITY 5
#define JSONRPC_ID_BUF_SIZE 64
#define JSONRPC_METHOD_BUF_SIZE 48

#define JSONRPC_PARSE_ERROR -32700
#define JSONRPC_INVALID_REQUEST -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS -32602

static const char *TAG = "usb_console";
static SemaphoreHandle_t s_usb_write_lock;

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

void meb_usb_printf(const char *fmt, ...)
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

    if (s_usb_write_lock) {
        xSemaphoreTake(s_usb_write_lock, portMAX_DELAY);
    }
    if (usb_serial_jtag_is_driver_installed()) {
        (void)usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
    }
    if (s_usb_write_lock) {
        xSemaphoreGive(s_usb_write_lock);
    }
}

static void send_rpc_error(const char *id_token, int code, const char *message)
{
    meb_usb_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}\n",
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

    meb_usb_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id_token, result);
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
    send_rpc_result(has_id, id_token, "{\"heating_enabled\":%s}", enabled ? "true" : "false");
}

static void handle_heating_get(bool has_id, const char *id_token)
{
    meb_state_snapshot_t state;
    meb_state_get_snapshot(&state);

    send_rpc_result(has_id, id_token,
                    "{\"heating_enabled\":%s,\"active\":%u,\"request\":%u,\"cooling_request\":%u,"
                    "\"power_w\":%u,\"power_req_w\":%u,\"temperature_status\":%u}",
                    state.heating_enabled ? "true" : "false",
                    state.battery_heating_active,
                    state.heating_request,
                    state.cooling_request,
                    state.power_battery_heating_watt,
                    state.power_battery_heating_req_watt,
                    state.temperature_status_charge);
}

static void handle_device_info(bool has_id, const char *id_token)
{
    send_rpc_result(has_id, id_token,
                    "{\"version\":\"%s\",\"about\":\"%s\",\"protocol_version\":%d,"
                    "\"telemetry_interval_ms\":%u,\"can\":{\"tx_gpio\":%d,\"rx_gpio\":%d,"
                    "\"bitrate\":%d,\"data_bitrate\":%d}}",
                    MEB_APP_VERSION,
                    MEB_APP_ABOUT,
                    MEB_PROTOCOL_VERSION,
                    meb_control_get_telemetry_interval_ms(),
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

static void process_command(const char *cmd)
{
    char id_token[JSONRPC_ID_BUF_SIZE];
    bool has_id = false;
    char jsonrpc[8];
    char method[JSONRPC_METHOD_BUF_SIZE];

    if (!cmd || *skip_ws(cmd) != '{') {
        send_rpc_error("null", JSONRPC_PARSE_ERROR, "Parse error");
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
    } else if (strcmp(method, "device.info") == 0) {
        handle_device_info(has_id, id_token);
    } else if (strcmp(method, "telemetry.set_interval") == 0) {
        handle_telemetry_set_interval(has_id, id_token, cmd);
    } else {
        send_rpc_error(id_token, JSONRPC_METHOD_NOT_FOUND, "Method not found");
    }
}

static void usb_command_task(void *arg)
{
    (void)arg;
    char cmd_buf[CMD_RX_BUF_SIZE];
    size_t cmd_len = 0;

    while (1) {
        uint8_t chunk[64];
        int nread = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));

        for (int i = 0; i < nread; i++) {
            uint8_t c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (cmd_len > 0) {
                    cmd_buf[cmd_len] = '\0';
                    process_command(cmd_buf);
                    cmd_len = 0;
                }
            } else if (cmd_len < (sizeof(cmd_buf) - 1)) {
                cmd_buf[cmd_len++] = (char)c;
            } else {
                cmd_buf[cmd_len] = '\0';
                process_command(cmd_buf);
                cmd_len = 0;
            }
        }
    }
}

esp_err_t meb_usb_console_init(void)
{
    s_usb_write_lock = xSemaphoreCreateMutex();
    if (!s_usb_write_lock) {
        return ESP_ERR_NO_MEM;
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }

    BaseType_t ok = xTaskCreate(usb_command_task, "usb_cmd", USB_TASK_STACK_SIZE, NULL, USB_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB Serial/JTAG JSON-RPC command interface ready");
    return ESP_OK;
}
