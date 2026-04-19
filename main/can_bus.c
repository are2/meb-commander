#include "can_bus.h"
#include "app_config.h"
#include "app_state.h"

#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/twai_types.h"

#define RX_QUEUE_DEPTH 32
#define RX_TASK_STACK_SIZE 4096
#define RX_TASK_PRIORITY 8

typedef struct {
    twai_frame_header_t header;
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
    uint16_t len;
} meb_can_rx_message_t;

static const char *TAG = "can_bus";
static twai_node_handle_t s_twai_node;
static QueueHandle_t s_rx_queue;

static uint16_t frame_payload_len(const twai_frame_header_t *header)
{
    if (header->fdf) {
        return twaifd_dlc2len(header->dlc);
    }

    return header->dlc <= TWAI_FRAME_MAX_LEN ? header->dlc : TWAI_FRAME_MAX_LEN;
}

static bool twai_rx_done_callback(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)edata;
    QueueHandle_t rx_queue = (QueueHandle_t)user_ctx;
    meb_can_rx_message_t msg = {0};
    twai_frame_t frame = {
        .buffer = msg.data,
        .buffer_len = sizeof(msg.data),
    };
    BaseType_t woken = pdFALSE;

    if (twai_node_receive_from_isr(handle, &frame) == ESP_OK) {
        msg.header = frame.header;
        msg.len = frame_payload_len(&frame.header);
        (void)xQueueSendFromISR(rx_queue, &msg, &woken);
    }

    return woken == pdTRUE;
}

static bool twai_tx_done_callback(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;

    if (!edata->is_tx_success) {
        ESP_EARLY_LOGW(TAG, "TWAI TX failed for ID 0x%" PRIX32, edata->done_tx_frame->header.id);
    }

    return false;
}

static bool twai_error_callback(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    ESP_EARLY_LOGW(TAG, "TWAI error flags 0x%" PRIX32, edata->err_flags.val);
    return false;
}

static bool twai_state_change_callback(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    ESP_EARLY_LOGW(TAG, "TWAI state %d -> %d", edata->old_sta, edata->new_sta);
    return false;
}

static void process_rx_message(const meb_can_rx_message_t *msg)
{
    if (!msg->header.ide) {
        return;
    }

    switch (msg->header.id) {
    case MEB_CAN_ID_DIAG_RESP:
        meb_state_update_diag_response(msg->data, msg->len);
        break;
    case MEB_CAN_ID_HEATING_STATUS:
        meb_state_update_heating_status(msg->data, msg->len);
        break;
    case MEB_CAN_ID_CHARGING_OPTIMIZATION:
        meb_state_update_charging_optimization(msg->data, msg->len);
        break;
    case MEB_CAN_ID_DYNAMIC:
        meb_state_update_dynamic(msg->data, msg->len);
        break;
    case MEB_CAN_ID_TEMPERATURE:
        meb_state_update_temperature(msg->data, msg->len);
        break;
    default:
        break;
    }
}

static void can_rx_task(void *arg)
{
    (void)arg;
    meb_can_rx_message_t msg;

    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            process_rx_message(&msg);
        }
    }
}

static esp_err_t transmit_fd_frame(uint32_t id, const uint8_t data[8], const char *name)
{
    if (!s_twai_node) {
        return ESP_ERR_INVALID_STATE;
    }

    twai_frame_t frame = {
        .header = {
            .id = id,
            .dlc = 8,
            .ide = 1,
            .fdf = 1,
            .brs = 1,
        },
        .buffer = (uint8_t *)data,
        .buffer_len = 8,
    };

    esp_err_t err = twai_node_transmit(s_twai_node, &frame, 500);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s transmit failed: %s", name, esp_err_to_name(err));
    }

    return err;
}

esp_err_t meb_can_request_diag_session(void)
{
    static const uint8_t data[8] = {
        0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    return transmit_fd_frame(MEB_CAN_ID_DIAG_REQ, data, "diag_session");
}

esp_err_t meb_can_send_heat_request(void)
{
    static const uint8_t data[8] = {
        0x07, 0x2F, 0x80, 0x37, 0x03, 0x00, 0x05, 0x32,
    };

    return transmit_fd_frame(MEB_CAN_ID_DIAG_REQ, data, "heat_request");
}

esp_err_t meb_can_init(void)
{
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(meb_can_rx_message_t));
    ESP_RETURN_ON_FALSE(s_rx_queue, ESP_ERR_NO_MEM, TAG, "failed to create RX queue");

    twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = MEB_TWAI_TX_GPIO,
            .rx = MEB_TWAI_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {
            .bitrate = MEB_TWAI_BITRATE,
        },
        .data_timing = {
            .bitrate = MEB_TWAI_DATA_BITRATE,
        },
        .timestamp_resolution_hz = 1000000,
        .fail_retry_cnt = 3,
        .tx_queue_depth = MEB_TWAI_TX_QUEUE_DEPTH,
        .flags = {
            .no_receive_rtr = true,
        },
    };

    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_config, &s_twai_node), TAG, "failed to create TWAI node");

    twai_range_filter_config_t filter = {
        .range_low = 0x12000000U,
        .range_high = 0x1AFFFFFFU,
        .is_ext = true,
    };
    ESP_RETURN_ON_ERROR(twai_node_config_range_filter(s_twai_node, 0, &filter), TAG, "failed to configure TWAI range filter");

    twai_event_callbacks_t callbacks = {
        .on_rx_done = twai_rx_done_callback,
        .on_tx_done = twai_tx_done_callback,
        .on_error = twai_error_callback,
        .on_state_change = twai_state_change_callback,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_twai_node, &callbacks, s_rx_queue), TAG, "failed to register TWAI callbacks");
    ESP_RETURN_ON_ERROR(twai_node_enable(s_twai_node), TAG, "failed to enable TWAI node");

    BaseType_t ok = xTaskCreate(can_rx_task, "can_rx", RX_TASK_STACK_SIZE, NULL, RX_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create CAN RX task");

    ESP_LOGI(TAG, "TWAI FD started: TX GPIO %d, RX GPIO %d, %d/%d bit/s", MEB_TWAI_TX_GPIO, MEB_TWAI_RX_GPIO,
             MEB_TWAI_BITRATE, MEB_TWAI_DATA_BITRATE);
    return ESP_OK;
}
