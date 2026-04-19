#include "ota_update.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinycrypt/sha256.h"

#define OTA_REBOOT_TASK_STACK_SIZE 2048
#define OTA_REBOOT_TASK_PRIORITY 5

typedef struct {
    bool active;
    bool pending_reboot;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t expected_size;
    uint32_t written;
    bool has_sha256;
    uint8_t expected_sha256[32];
    struct tc_sha256_state_struct sha256_ctx;
} ota_update_state_t;

static const char *TAG = "ota_update";
static ota_update_state_t s_ota;

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t parse_sha256(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < 32; i++) {
        int hi = hex_digit(hex[i * 2]);
        int lo = hex_digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return ESP_OK;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static esp_err_t decode_base64(const char *src, uint8_t *dst, size_t dst_len, size_t *out_len)
{
    size_t src_len = strlen(src);
    if (src_len == 0 || (src_len % 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        int val[4] = {0};
        int pad = 0;

        for (size_t j = 0; j < 4; j++) {
            char c = src[i + j];
            if (c == '=') {
                if (j < 2 || (i + 4) != src_len) {
                    return ESP_ERR_INVALID_ARG;
                }
                pad++;
            } else {
                if (pad > 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                val[j] = base64_value(c);
                if (val[j] < 0) {
                    return ESP_ERR_INVALID_ARG;
                }
            }
        }

        if (out >= dst_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        dst[out++] = (uint8_t)((val[0] << 2) | (val[1] >> 4));

        if (pad < 2) {
            if (out >= dst_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            dst[out++] = (uint8_t)(((val[1] & 0x0f) << 4) | (val[2] >> 2));
        }

        if (pad < 1) {
            if (out >= dst_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            dst[out++] = (uint8_t)(((val[2] & 0x03) << 6) | val[3]);
        }
    }

    *out_len = out;
    return ESP_OK;
}

static void abort_active_update(void)
{
    if (s_ota.active) {
        (void)esp_ota_abort(s_ota.handle);
    }
    memset(&s_ota, 0, sizeof(s_ota));
}

esp_err_t meb_ota_update_begin(uint32_t size, const char *sha256_hex)
{
    if (s_ota.active || s_ota.pending_reboot) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t expected_sha256[32];
    ESP_RETURN_ON_ERROR(parse_sha256(sha256_hex, expected_sha256), TAG, "invalid sha256");

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }
    if (size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.active = true;
    s_ota.handle = handle;
    s_ota.partition = partition;
    s_ota.expected_size = size;
    s_ota.has_sha256 = true;
    memcpy(s_ota.expected_sha256, expected_sha256, sizeof(s_ota.expected_sha256));

    if (tc_sha256_init(&s_ota.sha256_ctx) != 1) {
        abort_active_update();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started OTA update to %s, size=%" PRIu32, partition->label, size);
    return ESP_OK;
}

esp_err_t meb_ota_update_write(uint32_t offset, const char *base64_data, uint32_t *decoded_len)
{
    if (decoded_len) {
        *decoded_len = 0;
    }
    if (!s_ota.active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!base64_data || offset != s_ota.written) {
        return MEB_ERR_OTA_PROTOCOL;
    }

    size_t base64_len = strlen(base64_data);
    if (base64_len == 0 || base64_len > MEB_OTA_MAX_BASE64_CHARS) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t decoded[(MEB_OTA_MAX_BASE64_CHARS / 4) * 3];
    size_t olen = 0;
    esp_err_t err = decode_base64(base64_data, decoded, sizeof(decoded), &olen);
    if (err != ESP_OK) {
        return err;
    }
    if (s_ota.written + olen > s_ota.expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_ota_write(s_ota.handle, decoded, olen);
    if (err != ESP_OK) {
        return err;
    }

    if (tc_sha256_update(&s_ota.sha256_ctx, decoded, olen) != 1) {
        return ESP_FAIL;
    }
    s_ota.written += (uint32_t)olen;

    if (decoded_len) {
        *decoded_len = (uint32_t)olen;
    }
    return ESP_OK;
}

esp_err_t meb_ota_update_end(void)
{
    if (!s_ota.active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.written != s_ota.expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t actual_sha256[32];
    if (tc_sha256_final(actual_sha256, &s_ota.sha256_ctx) != 1) {
        abort_active_update();
        return ESP_FAIL;
    }

    if (memcmp(actual_sha256, s_ota.expected_sha256, sizeof(actual_sha256)) != 0) {
        abort_active_update();
        return MEB_ERR_OTA_CHECKSUM;
    }

    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err != ESP_OK) {
        abort_active_update();
        return err;
    }

    err = esp_ota_set_boot_partition(s_ota.partition);
    if (err != ESP_OK) {
        memset(&s_ota, 0, sizeof(s_ota));
        return err;
    }

    s_ota.pending_reboot = true;
    s_ota.active = false;
    ESP_LOGI(TAG, "OTA update complete; reboot required");
    return ESP_OK;
}

esp_err_t meb_ota_update_cancel(void)
{
    if (s_ota.active) {
        abort_active_update();
        return ESP_OK;
    }

    if (s_ota.pending_reboot) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (!running) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(running), TAG, "failed to restore running partition");
        memset(&s_ota, 0, sizeof(s_ota));
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

void meb_ota_update_get_status(meb_ota_status_t *status)
{
    if (!status) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->active = s_ota.active;
    status->pending_reboot = s_ota.pending_reboot;
    status->expected_size = s_ota.expected_size;
    status->written = s_ota.written;
    status->has_sha256 = s_ota.has_sha256;

    if (s_ota.partition) {
        status->partition_size = s_ota.partition->size;
        snprintf(status->partition_label, sizeof(status->partition_label), "%s", s_ota.partition->label);
    }
}

const char *meb_ota_update_err_message(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "OK";
    case ESP_ERR_INVALID_ARG:
        return "Invalid OTA argument";
    case ESP_ERR_INVALID_SIZE:
        return "Invalid OTA size";
    case ESP_ERR_INVALID_STATE:
        return "OTA update is not in the required state";
    case ESP_ERR_NOT_FOUND:
        return "No OTA update partition available";
    case ESP_ERR_NO_MEM:
        return "Not enough memory for OTA update";
    case MEB_ERR_OTA_CHECKSUM:
        return "Firmware SHA-256 mismatch";
    case MEB_ERR_OTA_PROTOCOL:
        return "Unexpected firmware chunk offset or payload";
    default:
        return "OTA operation failed";
    }
}

static void reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

esp_err_t meb_ota_update_schedule_reboot(uint32_t delay_ms)
{
    BaseType_t ok = xTaskCreate(reboot_task, "ota_reboot", OTA_REBOOT_TASK_STACK_SIZE,
                                (void *)(uintptr_t)delay_ms, OTA_REBOOT_TASK_PRIORITY, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
