#include "can_bus.h"
#include "app_config.h"
#include "app_state.h"
#include "diagnostics.h"

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
#include "soc/soc_caps.h"

#define RX_QUEUE_DEPTH 64
#define RX_TASK_STACK_SIZE 4096
#define RX_TASK_PRIORITY 8
#define TEST_TX_TASK_STACK_SIZE 4096
#define TEST_TX_TASK_PRIORITY 5
#define DIAG_POLL_PERIOD_MS 100
#define DIAG_REPORT_PERIOD_MS 1000
#define FULL_MASK TWAI_EXT_ID_MASK

typedef struct {
    twai_frame_header_t header;
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
    uint16_t len;
} meb_can_rx_message_t;

static const char *TAG = "can_bus";
static twai_node_handle_t s_twai_node;
static QueueHandle_t s_rx_queue;
static volatile uint32_t s_rx_queue_overflow_count;
static volatile uint32_t s_error_event_count;
static volatile uint32_t s_last_error_flags;
static volatile uint32_t s_error_flags_seen;
static volatile uint32_t s_state_change_count;
static volatile uint32_t s_last_old_state;
static volatile uint32_t s_last_new_state;
static volatile uint32_t s_state_entry_count[4];
static volatile uint32_t s_tx_failure_count;
static volatile uint32_t s_last_tx_failure_id;

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
        if (xQueueSendFromISR(rx_queue, &msg, &woken) != pdPASS) {
            s_rx_queue_overflow_count++;
        }
    }

    return woken == pdTRUE;
}

static bool twai_tx_done_callback(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;

    if (!edata->is_tx_success) {
        s_tx_failure_count++;
        s_last_tx_failure_id = edata->done_tx_frame ? edata->done_tx_frame->header.id : 0;
    }

    return false;
}

static bool twai_error_callback(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    s_error_event_count++;
    s_last_error_flags = edata->err_flags.val;
    s_error_flags_seen |= edata->err_flags.val;
    return false;
}

static bool twai_state_change_callback(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    s_state_change_count++;
    s_last_old_state = (uint32_t)edata->old_sta;
    s_last_new_state = (uint32_t)edata->new_sta;
    if ((uint32_t)edata->new_sta < 4U) {
        s_state_entry_count[(uint32_t)edata->new_sta]++;
    }
    return false;
}

static const char *twai_state_name_short(uint32_t state)
{
    switch ((twai_error_state_t)state) {
    case TWAI_ERROR_ACTIVE:
        return "act";
    case TWAI_ERROR_WARNING:
        return "warn";
    case TWAI_ERROR_PASSIVE:
        return "pass";
    case TWAI_ERROR_BUS_OFF:
        return "off";
    default:
        return "?";
    }
}

static void append_flag_name(char *buf, size_t buf_len, size_t *pos, const char *name)
{
    if (!buf || !pos || !name || *pos >= buf_len - 1) {
        return;
    }

    int len = snprintf(buf + *pos, buf_len - *pos, "%s%s", *pos > 0 ? "|" : "", name);
    if (len < 0) {
        return;
    }

    size_t written = (size_t)len;
    if (written >= buf_len - *pos) {
        *pos = buf_len - 1;
        return;
    }

    *pos += written;
}

static void format_error_flags(uint32_t flags, char *buf, size_t buf_len)
{
    size_t pos = 0;

    if (!buf || buf_len == 0) {
        return;
    }

    buf[0] = '\0';
    if (flags == 0) {
        snprintf(buf, buf_len, "unknown");
        return;
    }

    if (flags & BIT(0)) {
        append_flag_name(buf, buf_len, &pos, "arb");
    }
    if (flags & BIT(1)) {
        append_flag_name(buf, buf_len, &pos, "bit");
    }
    if (flags & BIT(2)) {
        append_flag_name(buf, buf_len, &pos, "form");
    }
    if (flags & BIT(3)) {
        append_flag_name(buf, buf_len, &pos, "stuff");
    }
    if (flags & BIT(4)) {
        append_flag_name(buf, buf_len, &pos, "ack");
    }

    if (pos == 0) {
        snprintf(buf, buf_len, "other");
    }
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

static void report_isr_diagnostics(void)
{
    static uint32_t last_overflow_count;
    static uint32_t last_error_event_count;
    static uint32_t last_state_change_count;
    static uint32_t last_tx_failure_count;
    static uint32_t last_state_entry_count[4];

    twai_node_status_t node_status = {0};
    bool have_node_status = s_twai_node && twai_node_get_info(s_twai_node, &node_status, NULL) == ESP_OK;

    uint32_t overflow_count = s_rx_queue_overflow_count;
    if (overflow_count != last_overflow_count) {
        uint32_t dropped = overflow_count - last_overflow_count;
        last_overflow_count = overflow_count;
        ESP_LOGW(TAG, "dropped %" PRIu32 " CAN frames because the RX queue was full", dropped);
        meb_diag_record_eventf("can", "rx_queue_full", "dropped=%" PRIu32, dropped);
    }

    uint32_t error_event_count = s_error_event_count;
    if (error_event_count != last_error_event_count) {
        uint32_t errors = error_event_count - last_error_event_count;
        uint32_t last_flags = s_last_error_flags;
        uint32_t flags_seen = s_error_flags_seen;
        char flag_names[24];
        last_error_event_count = error_event_count;
        s_error_flags_seen = 0;
        format_error_flags(flags_seen, flag_names, sizeof(flag_names));

        if (have_node_status) {
            ESP_LOGW(TAG, "TWAI errors=%" PRIu32 " flags=0x%" PRIX32 " (%s) last=0x%" PRIX32
                          " cur=%s tec=%u rec=%u",
                     errors, flags_seen, flag_names, last_flags,
                     twai_state_name_short((uint32_t)node_status.state),
                     (unsigned)node_status.tx_error_count,
                     (unsigned)node_status.rx_error_count);
            meb_diag_record_eventf("can", "error",
                                   "n=%" PRIu32 " f=0x%" PRIX32 "(%s) s=%s tec=%u rec=%u",
                                   errors, flags_seen, flag_names,
                                   twai_state_name_short((uint32_t)node_status.state),
                                   (unsigned)node_status.tx_error_count,
                                   (unsigned)node_status.rx_error_count);
        } else {
            ESP_LOGW(TAG, "TWAI errors=%" PRIu32 " flags=0x%" PRIX32 " (%s) last=0x%" PRIX32,
                     errors, flags_seen, flag_names, last_flags);
            meb_diag_record_eventf("can", "error", "n=%" PRIu32 " f=0x%" PRIX32 "(%s)",
                                   errors, flags_seen, flag_names);
        }
    }

    uint32_t state_change_count = s_state_change_count;
    if (state_change_count != last_state_change_count) {
        uint32_t changes = state_change_count - last_state_change_count;
        uint32_t old_state = s_last_old_state;
        uint32_t new_state = s_last_new_state;
        uint32_t active_entries = s_state_entry_count[TWAI_ERROR_ACTIVE] - last_state_entry_count[TWAI_ERROR_ACTIVE];
        uint32_t warning_entries = s_state_entry_count[TWAI_ERROR_WARNING] - last_state_entry_count[TWAI_ERROR_WARNING];
        uint32_t passive_entries = s_state_entry_count[TWAI_ERROR_PASSIVE] - last_state_entry_count[TWAI_ERROR_PASSIVE];
        uint32_t bus_off_entries = s_state_entry_count[TWAI_ERROR_BUS_OFF] - last_state_entry_count[TWAI_ERROR_BUS_OFF];
        last_state_change_count = state_change_count;
        last_state_entry_count[TWAI_ERROR_ACTIVE] = s_state_entry_count[TWAI_ERROR_ACTIVE];
        last_state_entry_count[TWAI_ERROR_WARNING] = s_state_entry_count[TWAI_ERROR_WARNING];
        last_state_entry_count[TWAI_ERROR_PASSIVE] = s_state_entry_count[TWAI_ERROR_PASSIVE];
        last_state_entry_count[TWAI_ERROR_BUS_OFF] = s_state_entry_count[TWAI_ERROR_BUS_OFF];

        if (have_node_status) {
            ESP_LOGW(TAG, "TWAI state changes=%" PRIu32 " last=%s->%s enter[a=%" PRIu32 " w=%" PRIu32
                          " p=%" PRIu32 " b=%" PRIu32 "] cur=%s",
                     changes,
                     twai_state_name_short(old_state),
                     twai_state_name_short(new_state),
                     active_entries, warning_entries, passive_entries, bus_off_entries,
                     twai_state_name_short((uint32_t)node_status.state));
            meb_diag_record_eventf("can", "state",
                                   "n=%" PRIu32 " %s>%s a=%" PRIu32 " w=%" PRIu32 " p=%" PRIu32 " b=%" PRIu32
                                   " c=%s",
                                   changes,
                                   twai_state_name_short(old_state),
                                   twai_state_name_short(new_state),
                                   active_entries, warning_entries, passive_entries, bus_off_entries,
                                   twai_state_name_short((uint32_t)node_status.state));
        } else {
            ESP_LOGW(TAG, "TWAI state changes=%" PRIu32 " last=%s->%s enter[a=%" PRIu32 " w=%" PRIu32
                          " p=%" PRIu32 " b=%" PRIu32 "]",
                     changes,
                     twai_state_name_short(old_state),
                     twai_state_name_short(new_state),
                     active_entries, warning_entries, passive_entries, bus_off_entries);
            meb_diag_record_eventf("can", "state",
                                   "n=%" PRIu32 " %s>%s a=%" PRIu32 " w=%" PRIu32 " p=%" PRIu32 " b=%" PRIu32,
                                   changes,
                                   twai_state_name_short(old_state),
                                   twai_state_name_short(new_state),
                                   active_entries, warning_entries, passive_entries, bus_off_entries);
        }
    }

    uint32_t tx_failure_count = s_tx_failure_count;
    if (tx_failure_count != last_tx_failure_count) {
        uint32_t failures = tx_failure_count - last_tx_failure_count;
        uint32_t last_id = s_last_tx_failure_id;
        last_tx_failure_count = tx_failure_count;
        ESP_LOGW(TAG, "TWAI TX failures=%" PRIu32 " last_id=0x%" PRIX32, failures, last_id);
        meb_diag_record_eventf("can", "tx_failed_isr", "count=%" PRIu32 " id=0x%" PRIX32, failures, last_id);
    }
}

static void can_rx_task(void *arg)
{
    (void)arg;
    meb_can_rx_message_t msg;
    TickType_t last_report_tick = xTaskGetTickCount();

    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, pdMS_TO_TICKS(DIAG_POLL_PERIOD_MS)) == pdTRUE) {
            process_rx_message(&msg);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_report_tick) >= pdMS_TO_TICKS(DIAG_REPORT_PERIOD_MS)) {
            report_isr_diagnostics();
            last_report_tick = now;
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
        meb_diag_record_eventf("can", "tx_failed", "%s:%s", name, esp_err_to_name(err));
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

#if MEB_CAN_TEST_TX_ENABLED
static void can_test_tx_task(void *arg)
{
    (void)arg;
    uint8_t counter = 0;

    while (1) {
        uint8_t data[8] = {
            0xCA, 0xFE, 0xBA, 0xBE, counter, (uint8_t)~counter, 0x55, 0xAA,
        };

        (void)transmit_fd_frame(MEB_CAN_TEST_TX_ID, data, "test_tx");
        counter++;
        vTaskDelay(pdMS_TO_TICKS(MEB_CAN_TEST_TX_PERIOD_MS));
    }
}
#endif

esp_err_t meb_can_start_test_tx(void)
{
#if MEB_CAN_TEST_TX_ENABLED
    BaseType_t ok = xTaskCreate(can_test_tx_task, "can_test_tx", TEST_TX_TASK_STACK_SIZE, NULL,
                                TEST_TX_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create CAN test TX task");
    ESP_LOGW(TAG, "temporary CAN test TX enabled: ID 0x%" PRIX32 " every %d ms",
             (uint32_t)MEB_CAN_TEST_TX_ID, MEB_CAN_TEST_TX_PERIOD_MS);
#endif
    return ESP_OK;
}

static esp_err_t configure_exact_canfd_timing(void)
{
    const twai_timing_advanced_config_t bit_timing = {
        .brp = 4,
        .prop_seg = 8,
        .tseg_1 = 26,
        .tseg_2 = 5,
        .sjw = 2,
    };
    twai_timing_advanced_config_t data_timing = {
        .prop_seg = 8,
        .tseg_1 = 26,
        .tseg_2 = 5,
        .sjw = 2,
    };

    switch (MEB_TWAI_DATA_BITRATE) {
    case 1000000:
        data_timing.brp = 2;
        break;
    case 2000000:
        data_timing.brp = 1;
        break;
    default:
        ESP_LOGE(TAG, "unsupported exact CAN FD data bitrate: %d", MEB_TWAI_DATA_BITRATE);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return twai_node_reconfig_timing(s_twai_node, &bit_timing, &data_timing);
}

static esp_err_t configure_rx_filters(void)
{
#if SOC_TWAI_MASK_FILTER_NUM < 3 || SOC_TWAI_RANGE_FILTER_NUM < 1
#error "This firmware expects at least 3 TWAI mask filters and 1 range filter"
#endif
    static const twai_mask_filter_config_t disable_filter = {
        .id = UINT32_MAX,
        .mask = UINT32_MAX,
        .is_ext = true,
    };

    for (uint8_t i = 0; i < SOC_TWAI_MASK_FILTER_NUM; i++) {
        ESP_RETURN_ON_ERROR(twai_node_config_mask_filter(s_twai_node, i, &disable_filter), TAG,
                            "failed to disable TWAI mask filter %u", i);
    }

#if SOC_TWAI_RANGE_FILTER_NUM > 0
    static const twai_range_filter_config_t dynamic_filter = {
        .range_low = MEB_CAN_ID_DYNAMIC,
        .range_high = MEB_CAN_ID_HEATING_STATUS,
        .is_ext = true,
    };
    ESP_RETURN_ON_ERROR(twai_node_config_range_filter(s_twai_node, 0, &dynamic_filter), TAG,
                        "failed to configure TWAI range filter");
#endif

    const uint32_t exact_ids[] = {
        MEB_CAN_ID_DIAG_RESP,
        MEB_CAN_ID_CHARGING_OPTIMIZATION,
        MEB_CAN_ID_TEMPERATURE,
    };

    for (uint8_t i = 0; i < sizeof(exact_ids) / sizeof(exact_ids[0]); i++) {
        twai_mask_filter_config_t filter = {
            .id = exact_ids[i],
            .mask = FULL_MASK,
            .is_ext = true,
        };
        ESP_RETURN_ON_ERROR(twai_node_config_mask_filter(s_twai_node, i, &filter), TAG,
                            "failed to configure TWAI mask filter %u", i);
    }

    return ESP_OK;
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
        .clk_src = TWAI_CLK_SRC_PLL_F80M,
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
    ESP_RETURN_ON_ERROR(configure_exact_canfd_timing(), TAG, "failed to configure exact TWAI timing");
    ESP_RETURN_ON_ERROR(configure_rx_filters(), TAG, "failed to configure TWAI RX filters");

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
    meb_diag_record_eventf("can", "started", "%d/%d bit/s", MEB_TWAI_BITRATE, MEB_TWAI_DATA_BITRATE);
    return ESP_OK;
}
