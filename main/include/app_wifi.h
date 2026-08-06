#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WIFI_ROLE_AP = 0,    /* device creates a hotspot only */
    WIFI_ROLE_STA,       /* join a router network; falls back to AP on timeout */
    WIFI_ROLE_APSTA,     /* hotspot + station at the same time */
} wifi_role_t;

/**
 * Initialize WiFi (netif, event loop, driver), apply the configured role
 * and start. In STA role this blocks up to CONFIG_IR_TOOL_WIFI_STA_TIMEOUT_MS
 * waiting for the connection, then falls back to SoftAP so the web UI stays
 * reachable. Must be called after nvs_flash_init().
 */
esp_err_t wifi_init(void);

wifi_role_t wifi_get_role(void);
const char *wifi_mode_str(void);        /* actual mode: "AP" / "STA" / "AP+STA" */
bool wifi_is_sta_connected(void);
bool wifi_get_ap_ip(char *buf, size_t len);
bool wifi_get_sta_ip(char *buf, size_t len);
const char *wifi_ap_ssid(void);
const char *wifi_sta_ssid(void);
