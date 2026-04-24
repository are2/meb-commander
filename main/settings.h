#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t meb_settings_init(void);
esp_err_t meb_settings_get_telemetry_interval_ms(uint32_t *interval_ms);
esp_err_t meb_settings_set_telemetry_interval_ms(uint32_t interval_ms);
