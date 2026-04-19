#pragma once

#include <stddef.h>

#include "esp_err.h"

#define MEB_BLE_DEVICE_NAME "MEB-Preheat"
#define MEB_BLE_SERVICE_UUID "7e57c000-f8aa-4a1f-9af3-9c0b7fd90e00"
#define MEB_BLE_RX_UUID "7e57c001-f8aa-4a1f-9af3-9c0b7fd90e00"
#define MEB_BLE_TX_UUID "7e57c002-f8aa-4a1f-9af3-9c0b7fd90e00"

esp_err_t meb_ble_console_init(void);
void meb_ble_console_write(const char *data, size_t len);
