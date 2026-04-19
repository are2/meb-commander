#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MEB_OTA_MAX_BASE64_CHARS 768
#define MEB_ERR_OTA_CHECKSUM ((esp_err_t)0x7001)
#define MEB_ERR_OTA_PROTOCOL ((esp_err_t)0x7002)

typedef struct {
    bool active;
    bool pending_reboot;
    uint32_t expected_size;
    uint32_t written;
    uint32_t partition_size;
    bool has_sha256;
    char partition_label[17];
} meb_ota_status_t;

esp_err_t meb_ota_update_begin(uint32_t size, const char *sha256_hex);
esp_err_t meb_ota_update_write(uint32_t offset, const char *base64_data, uint32_t *decoded_len);
esp_err_t meb_ota_update_end(void);
esp_err_t meb_ota_update_cancel(void);
void meb_ota_update_get_status(meb_ota_status_t *status);
const char *meb_ota_update_err_message(esp_err_t err);
esp_err_t meb_ota_update_schedule_reboot(uint32_t delay_ms);
