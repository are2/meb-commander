#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_twai_types.h"

typedef struct {
    bool ready;
    uint32_t arbitration_sample_point_permill;
    uint32_t data_sample_point_permill;
    uint32_t data_ssp_permill;
    uint32_t rx_queue_overflow_count;
    uint32_t error_event_count;
    uint32_t state_change_count;
    uint32_t tx_failure_count;
    uint32_t last_error_flags;
    uint32_t last_tx_failure_id;
    uint32_t state_entry_count[4];
    twai_node_status_t node_status;
    twai_node_record_t node_record;
} meb_can_diagnostics_t;

esp_err_t meb_can_init(void);
esp_err_t meb_can_start_test_tx(void);
esp_err_t meb_can_request_diag_session(void);
esp_err_t meb_can_send_heat_request(void);
void meb_can_get_diagnostics(meb_can_diagnostics_t *diag);
