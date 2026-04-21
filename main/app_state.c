#include "app_state.h"
#include "app_config.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static meb_state_snapshot_t s_state;
static SemaphoreHandle_t s_state_lock;
static int64_t s_heating_enabled_since_us;
static int64_t s_user_auto_off_deadline_us;

#define US_PER_MINUTE (60LL * 1000LL * 1000LL)

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

static int64_t minutes_to_us(uint32_t minutes)
{
    return (int64_t)minutes * US_PER_MINUTE;
}

static uint32_t remaining_minutes_ceil(int64_t remaining_us)
{
    if (remaining_us <= 0) {
        return 0;
    }

    return (uint32_t)((remaining_us + US_PER_MINUTE - 1) / US_PER_MINUTE);
}

static int64_t safety_auto_off_deadline_locked(void)
{
#if MEB_SAFETY_AUTO_OFF_ENABLED
    if (s_heating_enabled_since_us > 0 && MEB_SAFETY_AUTO_OFF_MINUTES > 0) {
        return s_heating_enabled_since_us + minutes_to_us(MEB_SAFETY_AUTO_OFF_MINUTES);
    }
#endif

    return 0;
}

static int64_t effective_auto_off_deadline_locked(void)
{
    int64_t deadline_us = s_user_auto_off_deadline_us;
    int64_t safety_deadline_us = safety_auto_off_deadline_locked();

    if (safety_deadline_us > 0 && (deadline_us == 0 || safety_deadline_us < deadline_us)) {
        deadline_us = safety_deadline_us;
    }

    return deadline_us;
}

esp_err_t meb_state_init(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    if (!s_state_lock) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.heating_enabled = false;
    s_heating_enabled_since_us = 0;
    s_user_auto_off_deadline_us = 0;
    return ESP_OK;
}

void meb_state_get_snapshot(meb_state_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    lock_state();
    *snapshot = s_state;
    snapshot->auto_off_timer_enabled = s_state.auto_off_timer_minutes > 0;
    snapshot->auto_off_remaining_valid = false;
    snapshot->auto_off_remaining_minutes = 0;

    int64_t deadline_us = effective_auto_off_deadline_locked();
    if (s_state.heating_enabled && deadline_us > 0) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        snapshot->auto_off_remaining_valid = true;
        snapshot->auto_off_remaining_minutes = remaining_minutes_ceil(remaining_us);
    }
    unlock_state();
}

void meb_state_set_heating_enabled(bool enabled)
{
    lock_state();
    if (enabled) {
        if (!s_state.heating_enabled) {
            int64_t now_us = esp_timer_get_time();
            s_heating_enabled_since_us = now_us;
            s_user_auto_off_deadline_us = s_state.auto_off_timer_minutes > 0
                ? now_us + minutes_to_us(s_state.auto_off_timer_minutes)
                : 0;
        }
    } else {
        s_heating_enabled_since_us = 0;
        s_user_auto_off_deadline_us = 0;
    }
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

esp_err_t meb_state_set_auto_off_timer_minutes(uint32_t minutes)
{
    lock_state();
    s_state.auto_off_timer_minutes = minutes;
    if (s_state.heating_enabled) {
        s_user_auto_off_deadline_us = minutes > 0
            ? esp_timer_get_time() + minutes_to_us(minutes)
            : 0;
    } else {
        s_user_auto_off_deadline_us = 0;
    }
    unlock_state();

    return ESP_OK;
}

meb_heating_auto_off_reason_t meb_state_apply_auto_off(void)
{
    meb_heating_auto_off_reason_t reason = MEB_HEATING_AUTO_OFF_NONE;
    int64_t now_us = esp_timer_get_time();

    lock_state();
    if (s_state.heating_enabled) {
        int64_t user_deadline_us = s_user_auto_off_deadline_us;
        int64_t safety_deadline_us = safety_auto_off_deadline_locked();
        int64_t deadline_us = user_deadline_us;

        if (safety_deadline_us > 0 && (deadline_us == 0 || safety_deadline_us < deadline_us)) {
            deadline_us = safety_deadline_us;
        }

        if (deadline_us > 0 && now_us >= deadline_us) {
            if (user_deadline_us > 0 && user_deadline_us <= deadline_us) {
                reason = MEB_HEATING_AUTO_OFF_TIMER;
            } else {
                reason = MEB_HEATING_AUTO_OFF_SAFETY;
            }

            s_state.heating_enabled = false;
            s_heating_enabled_since_us = 0;
            s_user_auto_off_deadline_us = 0;
        }
    }
    unlock_state();

    return reason;
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
