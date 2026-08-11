#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_CFG_SSID_LEN 32
#define WIFI_CFG_PWD_LEN  64

/* Web-editable WiFi configuration (persisted in NVS, menuconfig provides defaults) */
typedef struct {
    char ap_ssid[WIFI_CFG_SSID_LEN + 1];
    char ap_password[WIFI_CFG_PWD_LEN + 1];   /* empty = open network */
    char sta_ssid[WIFI_CFG_SSID_LEN + 1];     /* empty = skip station */
    char sta_password[WIFI_CFG_PWD_LEN + 1];
    bool sta_dhcp;                            /* true = DHCP, false = static */
    uint32_t sta_ip;                          /* static config, network byte order */
    uint32_t sta_gw;
    uint32_t sta_mask;
    uint32_t sta_dns;
    uint32_t sta_dns2;                        /* optional backup resolver */
} wifi_web_config_t;

/**
 * Initialize WiFi (netif, event loop, driver), load the web configuration from
 * NVS (menuconfig values as defaults) and start.
 *
 * STA and SoftAP are mutually exclusive (automatic): if a station SSID is
 * configured the device joins the router as STA only (no hotspot); if the
 * station SSID is empty the device opens the hotspot. In STA mode this blocks
 * up to CONFIG_IR_TOOL_WIFI_STA_TIMEOUT_MS waiting for the connection, then
 * falls back to SoftAP so the web UI stays reachable.
 * Must be called after nvs_flash_init().
 */
esp_err_t wifi_init(void);

/* Load/save the web-editable WiFi configuration (NVS, namespace "ir_tool"). */
esp_err_t wifi_web_config_load(wifi_web_config_t *cfg);
esp_err_t wifi_web_config_save(const wifi_web_config_t *cfg);

const char *wifi_mode_str(void);        /* actual mode: "AP" or "STA" */
bool wifi_ap_active(void);              /* true only when the SoftAP is running */
bool wifi_is_sta_connected(void);
bool wifi_get_ap_ip(char *buf, size_t len);
bool wifi_get_sta_ip(char *buf, size_t len);
const char *wifi_sta_ip_mode(void);     /* "dhcp" / "static" / "-" */
const char *wifi_ap_ssid(void);
const char *wifi_sta_ssid(void);
void wifi_ensure_dns(void);             /* fall back to public DNS when DHCP gave none */
