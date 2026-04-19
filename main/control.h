#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t meb_control_start(void);
uint32_t meb_control_get_telemetry_interval_ms(void);
esp_err_t meb_control_set_telemetry_interval_ms(uint32_t interval_ms);
