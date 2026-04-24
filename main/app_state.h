#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool valid;
    uint8_t value;
} meb_nullable_u8_t;

typedef struct {
    bool valid;
    uint32_t value;
} meb_nullable_u32_t;

typedef struct {
    bool valid;
    double value;
} meb_nullable_double_t;

typedef struct {
    bool valid;
    uint8_t active;
    uint8_t request;
    uint8_t cooling_request;
    uint8_t power_w;
    uint8_t power_req_w;
} meb_heating_status_t;

typedef struct {
    bool valid;
    double power_kw;
    double current_a;
} meb_charge_limits_t;

typedef struct {
    bool valid;
    double min_c;
    double max_c;
} meb_battery_temperature_t;

typedef struct {
    meb_heating_status_t heating_status;
    meb_nullable_u8_t temperature_status_charge;
    meb_charge_limits_t charge_limits;
    meb_battery_temperature_t battery_temperature;
    meb_nullable_double_t bms_soc_percent;
    bool session_error;
    bool heating_enabled;
    bool auto_off_timer_enabled;
    uint32_t auto_off_timer_minutes;
    meb_nullable_u32_t auto_off_remaining_minutes;
} meb_state_snapshot_t;

typedef enum {
    MEB_HEATING_AUTO_OFF_NONE = 0,
    MEB_HEATING_AUTO_OFF_TIMER,
    MEB_HEATING_AUTO_OFF_SAFETY,
} meb_heating_auto_off_reason_t;

esp_err_t meb_state_init(void);
void meb_state_get_snapshot(meb_state_snapshot_t *snapshot);
void meb_state_set_heating_enabled(bool enabled);
bool meb_state_is_heating_enabled(void);
esp_err_t meb_state_set_auto_off_timer_minutes(uint32_t minutes);
meb_heating_auto_off_reason_t meb_state_apply_auto_off(void);
bool meb_state_take_session_error(void);

void meb_state_update_diag_response(const uint8_t *data, uint16_t len);
void meb_state_update_heating_status(const uint8_t *data, uint16_t len);
void meb_state_update_charging_optimization(const uint8_t *data, uint16_t len);
void meb_state_update_dynamic(const uint8_t *data, uint16_t len);
void meb_state_update_temperature(const uint8_t *data, uint16_t len);
