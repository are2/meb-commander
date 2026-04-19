#include "ble_console.h"

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "serial_console.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_CMD_RX_BUF_SIZE 1024
#define BLE_CMD_QUEUE_DEPTH 8
#define BLE_CMD_TASK_STACK_SIZE 8192
#define BLE_CMD_TASK_PRIORITY 5
#define BLE_NOTIFY_MIN_CHUNK_SIZE 20

typedef struct {
    char line[BLE_CMD_RX_BUF_SIZE];
} ble_command_t;

typedef struct {
    bool in_use;
    bool notify_enabled;
    uint16_t conn_handle;
} ble_peer_t;

static const char *TAG = "ble_console";

/* UUID bytes are in NimBLE's little-endian order. Canonical UUID strings are in ble_console.h. */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x00, 0x0e, 0xd9, 0x7f, 0x0b, 0x9c, 0xf3, 0x9a,
                     0x1f, 0x4a, 0xaa, 0xf8, 0x00, 0xc0, 0x57, 0x7e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x00, 0x0e, 0xd9, 0x7f, 0x0b, 0x9c, 0xf3, 0x9a,
                     0x1f, 0x4a, 0xaa, 0xf8, 0x01, 0xc0, 0x57, 0x7e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x00, 0x0e, 0xd9, 0x7f, 0x0b, 0x9c, 0xf3, 0x9a,
                     0x1f, 0x4a, 0xaa, 0xf8, 0x02, 0xc0, 0x57, 0x7e);

static uint8_t s_own_addr_type;
static uint16_t s_rx_val_handle;
static uint16_t s_tx_val_handle;
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_notify_lock;
static ble_peer_t s_peers[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];
static char s_rx_line[BLE_CMD_RX_BUF_SIZE];
static size_t s_rx_line_len;
static bool s_ble_started;

void ble_store_config_init(void);

static void advertise(void);

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_rx_val_handle,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            {
                0,
            },
        },
    },
    {
        0,
    },
};

static ble_peer_t *find_peer(uint16_t conn_handle)
{
    for (size_t i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        if (s_peers[i].in_use && s_peers[i].conn_handle == conn_handle) {
            return &s_peers[i];
        }
    }

    return NULL;
}

static ble_peer_t *alloc_peer(uint16_t conn_handle)
{
    ble_peer_t *peer = find_peer(conn_handle);
    if (peer) {
        return peer;
    }

    for (size_t i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        if (!s_peers[i].in_use) {
            s_peers[i].in_use = true;
            s_peers[i].notify_enabled = false;
            s_peers[i].conn_handle = conn_handle;
            return &s_peers[i];
        }
    }

    return NULL;
}

static void remove_peer(uint16_t conn_handle)
{
    ble_peer_t *peer = find_peer(conn_handle);
    if (peer) {
        memset(peer, 0, sizeof(*peer));
    }
}

static void queue_line(const char *line)
{
    if (!s_command_queue || !line || line[0] == '\0') {
        return;
    }

    ble_command_t cmd = {0};
    snprintf(cmd.line, sizeof(cmd.line), "%s", line);

    if (xQueueSend(s_command_queue, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "dropping BLE JSON-RPC command because command queue is full");
    }
}

static void feed_rx_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\n' || c == '\r') {
            if (s_rx_line_len > 0) {
                s_rx_line[s_rx_line_len] = '\0';
                queue_line(s_rx_line);
                s_rx_line_len = 0;
            }
        } else if (s_rx_line_len == 0) {
            if (c == '{') {
                s_rx_line[s_rx_line_len++] = (char)c;
            }
        } else if (s_rx_line_len < (sizeof(s_rx_line) - 1)) {
            s_rx_line[s_rx_line_len++] = (char)c;
        } else {
            ESP_LOGW(TAG, "dropping overlong BLE JSON-RPC command");
            s_rx_line_len = 0;
        }
    }
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (attr_handle != s_rx_val_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > BLE_CMD_RX_BUF_SIZE) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t buf[BLE_CMD_RX_BUF_SIZE];
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &copied);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        feed_rx_bytes(buf, copied);
        return 0;

    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (attr_handle == s_tx_val_handle) {
            static const char ready[] = "MEB JSON-RPC TX notifications\n";
            int rc = os_mbuf_append(ctxt->om, ready, sizeof(ready) - 1);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            (void)alloc_peer(event->connect.conn_handle);
            ESP_LOGI(TAG, "BLE client connected, handle=%u", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE client disconnected, reason=%d", event->disconnect.reason);
        remove_peer(event->disconnect.conn.conn_handle);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            ble_peer_t *peer = alloc_peer(event->subscribe.conn_handle);
            if (peer) {
                peer->notify_enabled = event->subscribe.cur_notify != 0;
            }
            ESP_LOGI(TAG, "BLE TX notifications %s for handle=%u",
                     event->subscribe.cur_notify ? "enabled" : "disabled",
                     event->subscribe.conn_handle);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU updated, handle=%u mtu=%u",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)MEB_BLE_DEVICE_NAME;
    fields.name_len = strlen(MEB_BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set BLE advertisement fields, rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set BLE scan response fields, rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start BLE advertising, rc=%d", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to ensure BLE address, rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer BLE address type, rc=%d", rc);
        return;
    }

    s_ble_started = true;
    advertise();
    ESP_LOGI(TAG, "BLE JSON-RPC service advertising as %s", MEB_BLE_DEVICE_NAME);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void command_task(void *arg)
{
    (void)arg;

    ble_command_t cmd;
    while (1) {
        if (xQueueReceive(s_command_queue, &cmd, portMAX_DELAY) == pdPASS) {
            meb_serial_console_process_command(cmd.line);
        }
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "failed to erase NVS");
        err = nvs_flash_init();
    }

    return err;
}

esp_err_t meb_ble_console_init(void)
{
    s_command_queue = xQueueCreate(BLE_CMD_QUEUE_DEPTH, sizeof(ble_command_t));
    if (!s_command_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_notify_lock = xSemaphoreCreateMutex();
    if (!s_notify_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "failed to initialize NVS for BLE");
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "failed to initialize NimBLE");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = 1;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to count BLE GATT config, rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to add BLE GATT services, rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(MEB_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set BLE device name, rc=%d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();

    BaseType_t ok = xTaskCreate(command_task, "ble_cmd", BLE_CMD_TASK_STACK_SIZE,
                                NULL, BLE_CMD_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void meb_ble_console_write(const char *data, size_t len)
{
    if (!s_ble_started || !data || len == 0 || !s_notify_lock || s_tx_val_handle == 0) {
        return;
    }

    xSemaphoreTake(s_notify_lock, portMAX_DELAY);

    for (size_t i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        if (!s_peers[i].in_use || !s_peers[i].notify_enabled) {
            continue;
        }

        size_t offset = 0;
        while (offset < len) {
            uint16_t mtu = ble_att_mtu(s_peers[i].conn_handle);
            size_t max_chunk = mtu > 3 ? (size_t)(mtu - 3) : BLE_NOTIFY_MIN_CHUNK_SIZE;
            if (max_chunk < BLE_NOTIFY_MIN_CHUNK_SIZE) {
                max_chunk = BLE_NOTIFY_MIN_CHUNK_SIZE;
            }

            size_t chunk_len = len - offset;
            if (chunk_len > max_chunk) {
                chunk_len = max_chunk;
            }

            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + offset, chunk_len);
            if (!om) {
                ESP_LOGW(TAG, "failed to allocate BLE notification buffer");
                break;
            }

            int rc = ble_gatts_notify_custom(s_peers[i].conn_handle, s_tx_val_handle, om);
            if (rc != 0) {
                ESP_LOGD(TAG, "BLE notification failed for handle=%u rc=%d",
                         s_peers[i].conn_handle, rc);
                break;
            }

            offset += chunk_len;
        }
    }

    xSemaphoreGive(s_notify_lock);
}

#else

esp_err_t meb_ble_console_init(void)
{
    return ESP_OK;
}

void meb_ble_console_write(const char *data, size_t len)
{
    (void)data;
    (void)len;
}

#endif
