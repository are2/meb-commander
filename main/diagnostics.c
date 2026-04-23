#include "diagnostics.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_diag_lock;
static meb_diag_event_t s_events[MEB_DIAG_EVENT_CAPACITY];
static uint32_t s_event_count;
static uint32_t s_next_index;
static uint32_t s_next_seq = 1;
static uint32_t s_overwritten;

static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    snprintf(dst, dst_len, "%s", src);
}

static void lock_diag(void)
{
    if (s_diag_lock) {
        xSemaphoreTake(s_diag_lock, portMAX_DELAY);
    }
}

static void unlock_diag(void)
{
    if (s_diag_lock) {
        xSemaphoreGive(s_diag_lock);
    }
}

const char *meb_diag_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_UNKNOWN:
        return "unknown";
    default:
        return "other";
    }
}

esp_err_t meb_diag_init(void)
{
    s_diag_lock = xSemaphoreCreateMutex();
    if (!s_diag_lock) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0;
    s_next_index = 0;
    s_next_seq = 1;
    s_overwritten = 0;

    meb_diag_record_eventf("system", "boot", "reset=%s", meb_diag_reset_reason_name(esp_reset_reason()));
    return ESP_OK;
}

void meb_diag_get_status(meb_diag_status_t *status)
{
    if (!status) {
        return;
    }

    status->uptime_ms = uptime_ms();
    status->reset_reason = esp_reset_reason();
    status->free_heap = esp_get_free_heap_size();
    status->minimum_free_heap = esp_get_minimum_free_heap_size();
    status->free_8bit_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    status->minimum_free_8bit_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    status->largest_free_8bit_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void meb_diag_get_events(meb_diag_event_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    lock_diag();
    snapshot->capacity = MEB_DIAG_EVENT_CAPACITY;
    snapshot->count = s_event_count;
    snapshot->overwritten = s_overwritten;
    snapshot->next_seq = s_next_seq;

    uint32_t first = (s_next_index + MEB_DIAG_EVENT_CAPACITY - s_event_count) % MEB_DIAG_EVENT_CAPACITY;
    for (uint32_t i = 0; i < s_event_count; i++) {
        uint32_t index = (first + i) % MEB_DIAG_EVENT_CAPACITY;
        snapshot->events[i] = s_events[index];
    }
    unlock_diag();
}

void meb_diag_record_event(const char *component, const char *event, const char *detail)
{
    meb_diag_event_t item = {
        .uptime_ms = uptime_ms(),
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = esp_get_minimum_free_heap_size(),
    };
    copy_text(item.component, sizeof(item.component), component);
    copy_text(item.event, sizeof(item.event), event);
    copy_text(item.detail, sizeof(item.detail), detail);

    lock_diag();
    item.seq = s_next_seq++;
    s_events[s_next_index] = item;
    s_next_index = (s_next_index + 1) % MEB_DIAG_EVENT_CAPACITY;
    if (s_event_count < MEB_DIAG_EVENT_CAPACITY) {
        s_event_count++;
    } else {
        s_overwritten++;
    }
    unlock_diag();
}

void meb_diag_record_eventv(const char *component, const char *event, const char *detail_fmt, va_list args)
{
    char detail[MEB_DIAG_DETAIL_LEN];

    if (detail_fmt) {
        vsnprintf(detail, sizeof(detail), detail_fmt, args);
    } else {
        detail[0] = '\0';
    }

    meb_diag_record_event(component, event, detail);
}

void meb_diag_record_eventf(const char *component, const char *event, const char *detail_fmt, ...)
{
    va_list args;

    va_start(args, detail_fmt);
    meb_diag_record_eventv(component, event, detail_fmt, args);
    va_end(args);
}

size_t meb_diag_json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;

    if (!dst || dst_len == 0) {
        return 0;
    }

    if (!src) {
        src = "";
    }

    while (*src && out < dst_len - 1) {
        unsigned char c = (unsigned char)*src++;
        const char *escaped = NULL;

        switch (c) {
        case '"':
            escaped = "\\\"";
            break;
        case '\\':
            escaped = "\\\\";
            break;
        case '\b':
            escaped = "\\b";
            break;
        case '\f':
            escaped = "\\f";
            break;
        case '\n':
            escaped = "\\n";
            break;
        case '\r':
            escaped = "\\r";
            break;
        case '\t':
            escaped = "\\t";
            break;
        default:
            break;
        }

        if (escaped) {
            size_t len = strlen(escaped);
            if (out + len >= dst_len) {
                break;
            }
            memcpy(dst + out, escaped, len);
            out += len;
        } else if (c < 0x20) {
            if (out + 6 >= dst_len) {
                break;
            }
            int len = snprintf(dst + out, dst_len - out, "\\u%04x", c);
            if (len < 0) {
                break;
            }
            out += (size_t)len;
        } else {
            dst[out++] = (char)c;
        }
    }

    dst[out] = '\0';
    return out;
}
