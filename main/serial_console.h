#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t meb_serial_console_init(void);
void meb_serial_printf(const char *fmt, ...);
void meb_serial_console_process_command(const char *cmd);
