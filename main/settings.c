#include "settings.h"
#include "app_config.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_NAMESPACE "meb"
#define TELEMETRY_INTERVAL_KEY "tel_ms"

static const char *TAG = "settings";
static bool s_settings_initialized;

esp_err_t meb_settings_init(void)
{
    if (s_settings_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "failed to erase NVS");
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        s_settings_initialized = true;
    }

    return err;
}

esp_err_t meb_settings_get_telemetry_interval_ms(uint32_t *interval_ms)
{
    if (!interval_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(meb_settings_init(), TAG, "failed to initialize NVS");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(handle, TELEMETRY_INTERVAL_KEY, interval_ms);
    nvs_close(handle);
    return err;
}

esp_err_t meb_settings_set_telemetry_interval_ms(uint32_t interval_ms)
{
    if (interval_ms < MEB_TELEMETRY_MIN_PERIOD_MS || interval_ms > MEB_TELEMETRY_MAX_PERIOD_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(meb_settings_init(), TAG, "failed to initialize NVS");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, TELEMETRY_INTERVAL_KEY, interval_ms);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}
