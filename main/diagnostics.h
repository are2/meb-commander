#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_system.h"

#define MEB_DIAG_EVENT_CAPACITY 16
#define MEB_DIAG_COMPONENT_LEN 16
#define MEB_DIAG_EVENT_LEN 24
#define MEB_DIAG_DETAIL_LEN 64

typedef struct {
    uint32_t seq;
    uint64_t uptime_ms;
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    char component[MEB_DIAG_COMPONENT_LEN];
    char event[MEB_DIAG_EVENT_LEN];
    char detail[MEB_DIAG_DETAIL_LEN];
} meb_diag_event_t;

typedef struct {
    uint32_t capacity;
    uint32_t count;
    uint32_t overwritten;
    uint32_t next_seq;
    meb_diag_event_t events[MEB_DIAG_EVENT_CAPACITY];
} meb_diag_event_snapshot_t;

typedef struct {
    uint64_t uptime_ms;
    esp_reset_reason_t reset_reason;
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    uint32_t free_8bit_heap;
    uint32_t minimum_free_8bit_heap;
    uint32_t largest_free_8bit_block;
} meb_diag_status_t;

esp_err_t meb_diag_init(void);
void meb_diag_get_status(meb_diag_status_t *status);
void meb_diag_get_events(meb_diag_event_snapshot_t *snapshot);
void meb_diag_record_event(const char *component, const char *event, const char *detail);
void meb_diag_record_eventf(const char *component, const char *event, const char *detail_fmt, ...);
void meb_diag_record_eventv(const char *component, const char *event, const char *detail_fmt, va_list args);
const char *meb_diag_reset_reason_name(esp_reset_reason_t reason);
size_t meb_diag_json_escape(const char *src, char *dst, size_t dst_len);
