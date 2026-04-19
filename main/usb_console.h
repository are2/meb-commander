#pragma once

#include "esp_err.h"

esp_err_t meb_usb_console_init(void);
void meb_usb_printf(const char *fmt, ...);
