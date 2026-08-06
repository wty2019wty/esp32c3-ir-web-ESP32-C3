#pragma once

#include "esp_err.h"

/**
 * Start the HTTP server (embedded web UI + JSON API).
 * Must be called after ir_init() and wifi_init().
 */
esp_err_t web_init(void);
