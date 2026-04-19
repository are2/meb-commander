#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t battery_heating_active;
    uint8_t heating_request;
    uint8_t cooling_request;
    uint8_t power_battery_heating_watt;
    uint8_t power_battery_heating_req_watt;
    uint8_t temperature_status_charge;
    double max_charge_power_kw;
    double max_charge_current_amp;
    double battery_min_temp;
    double battery_max_temp;
    bool session_error;
    bool heating_enabled;
} meb_state_snapshot_t;

esp_err_t meb_state_init(void);
void meb_state_get_snapshot(meb_state_snapshot_t *snapshot);
void meb_state_set_heating_enabled(bool enabled);
bool meb_state_is_heating_enabled(void);
bool meb_state_take_session_error(void);

void meb_state_update_diag_response(const uint8_t *data, uint16_t len);
void meb_state_update_heating_status(const uint8_t *data, uint16_t len);
void meb_state_update_charging_optimization(const uint8_t *data, uint16_t len);
void meb_state_update_dynamic(const uint8_t *data, uint16_t len);
void meb_state_update_temperature(const uint8_t *data, uint16_t len);
