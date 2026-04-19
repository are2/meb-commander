#pragma once

#include "esp_err.h"

esp_err_t meb_can_init(void);
esp_err_t meb_can_start_test_tx(void);
esp_err_t meb_can_request_diag_session(void);
esp_err_t meb_can_send_heat_request(void);
