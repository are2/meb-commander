#include "app_state.h"
#include "app_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static meb_state_snapshot_t s_state;
static SemaphoreHandle_t s_state_lock;

static void lock_state(void)
{
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}

esp_err_t meb_state_init(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    if (!s_state_lock) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.heating_enabled = false;
    return ESP_OK;
}

void meb_state_get_snapshot(meb_state_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    lock_state();
    *snapshot = s_state;
    unlock_state();
}

void meb_state_set_heating_enabled(bool enabled)
{
    lock_state();
    s_state.heating_enabled = enabled;
    unlock_state();
}

bool meb_state_is_heating_enabled(void)
{
    bool enabled;

    lock_state();
    enabled = s_state.heating_enabled;
    unlock_state();

    return enabled;
}

bool meb_state_take_session_error(void)
{
    bool session_error;

    lock_state();
    session_error = s_state.session_error;
    s_state.session_error = false;
    unlock_state();

    return session_error;
}

void meb_state_update_diag_response(const uint8_t *data, uint16_t len)
{
    if (!data || len < 4) {
        return;
    }

    if (data[0] == 0x03 && data[1] == 0x7F && data[2] == 0x2F && data[3] == 0x7F) {
        lock_state();
        s_state.session_error = true;
        unlock_state();
    }
}

void meb_state_update_heating_status(const uint8_t *data, uint16_t len)
{
    if (!data || len < 8) {
        return;
    }

    lock_state();
    s_state.battery_heating_active = (data[4] & 0x40) >> 6;
    s_state.heating_request = (data[5] & 0xE0) >> 5;
    s_state.cooling_request = (data[5] & 0x1C) >> 2;
    s_state.power_battery_heating_watt = data[6];
    s_state.power_battery_heating_req_watt = data[7];
    unlock_state();
}

void meb_state_update_charging_optimization(const uint8_t *data, uint16_t len)
{
    if (!data || len < 3) {
        return;
    }

    lock_state();
    s_state.temperature_status_charge = (((data[2] & 0x03) << 1) | (data[1] >> 7));
    unlock_state();
}

void meb_state_update_dynamic(const uint8_t *data, uint16_t len)
{
    if (!data || len < 8) {
        return;
    }

    lock_state();
    s_state.max_charge_power_kw = ((data[7] << 5) | (data[6] >> 3)) * 0.1;
    s_state.max_charge_current_amp = (((data[4] & 0x3F) << 7) | (data[3] >> 1)) * 0.2;
    unlock_state();
}

void meb_state_update_temperature(const uint8_t *data, uint16_t len)
{
    if (!data || len < 5) {
        return;
    }

    lock_state();
    s_state.battery_max_temp = data[3] * 0.5 - 40.0;
    s_state.battery_min_temp = data[4] * 0.5 - 40.0;
    unlock_state();
}
