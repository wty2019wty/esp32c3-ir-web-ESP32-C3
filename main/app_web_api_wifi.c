#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "app_web_internal.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

#define TAG "web"

static bool parse_ipv4(const char *s, uint32_t *out)
{
    if (!s || !*s) {
        return false;
    }
    uint8_t b[4] = {0};
    int idx = 0;
    const char *p = s;
    while (*p) {
        if (!isdigit((unsigned char)*p) || idx >= 4) {
            return false;
        }
        unsigned v = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (unsigned)(*p - '0');
            if (v > 255 || ++digits > 3) {
                return false;
            }
            p++;
        }
        b[idx++] = (uint8_t)v;
        if (*p == '.') {
            p++;
            if (!isdigit((unsigned char)*p)) {
                return false;
            }
        } else if (*p != '\0') {
            return false;
        }
    }
    if (idx != 4) {
        return false;
    }
    /* esp_ip4_addr_t stores the dotted bytes in memory order (a.b.c.d
     * as bytes 0..3), i.e. addr = a | b<<8 | c<<16 | d<<24 on little-endian. */
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static void format_ipv4(uint32_t ip, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u.%u",
             (unsigned)ip & 0xFF, (unsigned)(ip >> 8) & 0xFF,
             (unsigned)(ip >> 16) & 0xFF, (unsigned)(ip >> 24) & 0xFF);
}

/* Shared WiFi configuration cores, invoked by the WebSocket RPC dispatcher
 * (app_web_rpc.c). The REST API has been removed — all control now happens
 * over the /api/ws command channel. */

/* Current (NVS) WiFi configuration as a JSON string (caller frees). */
char *web_wificfg_get_json(void)
{
    wifi_web_config_t cfg;
    wifi_web_config_load(&cfg);
    char ip[16], gw[16], mask[16], dns[16];
    format_ipv4(cfg.sta_ip, ip, sizeof(ip));
    format_ipv4(cfg.sta_gw, gw, sizeof(gw));
    format_ipv4(cfg.sta_mask, mask, sizeof(mask));
    format_ipv4(cfg.sta_dns, dns, sizeof(dns));

    /* Built with cJSON so every value is JSON-escaped: an SSID containing a
     * quote or control character must not break the settings page. */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "ap_ssid", cfg.ap_ssid);
    cJSON_AddStringToObject(root, "ap_password", "");
    cJSON_AddBoolToObject(root, "ap_password_set", cfg.ap_password[0] != '\0');
    cJSON_AddStringToObject(root, "sta_ssid", cfg.sta_ssid);
    cJSON_AddStringToObject(root, "sta_password", "");
    cJSON_AddBoolToObject(root, "sta_password_set", cfg.sta_password[0] != '\0');
    cJSON_AddBoolToObject(root, "sta_dhcp", cfg.sta_dhcp);
    cJSON_AddStringToObject(root, "sta_ip", ip);
    cJSON_AddStringToObject(root, "sta_gw", gw);
    cJSON_AddStringToObject(root, "sta_mask", mask);
    cJSON_AddStringToObject(root, "sta_dns", dns);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* Apply a WiFi configuration from a JSON body and schedule a restart.
 * On success the device reboots ~2s later to apply the new settings. */
esp_err_t web_wificfg_set(cJSON *root, const char **err)
{
    wifi_web_config_t cfg;
    wifi_web_config_load(&cfg); /* start from current values */

    /* helper to grab a string field */
    cJSON *j = NULL;
    j = cJSON_GetObjectItem(root, "ap_ssid");
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(cfg.ap_ssid)) {
            *err = "ap_ssid too long";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.ap_ssid, j->valuestring, sizeof(cfg.ap_ssid));
    }
    j = cJSON_GetObjectItem(root, "ap_password");
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(cfg.ap_password)) {
            *err = "ap_password too long";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.ap_password, j->valuestring, sizeof(cfg.ap_password));
    }
    /* null or absent: keep current password */
    j = cJSON_GetObjectItem(root, "sta_ssid");
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(cfg.sta_ssid)) {
            *err = "sta_ssid too long";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.sta_ssid, j->valuestring, sizeof(cfg.sta_ssid));
    }
    j = cJSON_GetObjectItem(root, "sta_password");
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(cfg.sta_password)) {
            *err = "sta_password too long";
            return ESP_ERR_INVALID_ARG;
        }
        if (j->valuestring[0] != '\0' && strlen(j->valuestring) < 8) {
            *err = "sta_password needs >=8 chars or empty";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.sta_password, j->valuestring, sizeof(cfg.sta_password));
    }
    /* null or absent: keep current password */
    j = cJSON_GetObjectItem(root, "sta_dhcp");
    if (cJSON_IsBool(j)) {
        cfg.sta_dhcp = cJSON_IsTrue(j);
    }
    /* static IP fields (only used when sta_dhcp=false). A malformed value must
     * be reported, not silently ignored, or the save would "succeed" while the
     * device keeps the old address and the user only notices after reboot.
     * Convention (matches the password fields): absent/null = keep the current
     * value; empty string "" = clear the field. Automation clients that want to
     * leave a field untouched must send null/omit it, not "". */
    uint32_t tmp;
    j = cJSON_GetObjectItem(root, "sta_ip");
    if (cJSON_IsString(j)) {
        if (j->valuestring[0] != '\0' && !parse_ipv4(j->valuestring, &tmp)) {
            *err = "invalid sta_ip";
            return ESP_ERR_INVALID_ARG;
        }
        cfg.sta_ip = j->valuestring[0] != '\0' ? tmp : 0;
    }
    j = cJSON_GetObjectItem(root, "sta_gw");
    if (cJSON_IsString(j)) {
        if (j->valuestring[0] != '\0' && !parse_ipv4(j->valuestring, &tmp)) {
            *err = "invalid sta_gw";
            return ESP_ERR_INVALID_ARG;
        }
        cfg.sta_gw = j->valuestring[0] != '\0' ? tmp : 0;
    }
    j = cJSON_GetObjectItem(root, "sta_mask");
    if (cJSON_IsString(j)) {
        if (j->valuestring[0] != '\0' && !parse_ipv4(j->valuestring, &tmp)) {
            *err = "invalid sta_mask";
            return ESP_ERR_INVALID_ARG;
        }
        cfg.sta_mask = j->valuestring[0] != '\0' ? tmp : 0;
    }
    j = cJSON_GetObjectItem(root, "sta_dns");
    if (cJSON_IsString(j)) {
        if (j->valuestring[0] != '\0' && !parse_ipv4(j->valuestring, &tmp)) {
            *err = "invalid sta_dns";
            return ESP_ERR_INVALID_ARG;
        }
        cfg.sta_dns = j->valuestring[0] != '\0' ? tmp : 0;
    }

    /* validation */
    if (cfg.ap_ssid[0] == '\0') {
        *err = "ap_ssid empty";
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.ap_password[0] != '\0' && strlen(cfg.ap_password) < 8) {
        *err = "ap_password needs >=8 chars or empty";
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg.sta_dhcp && (cfg.sta_ip == 0 || cfg.sta_ip == 0xFFFFFFFFU)) {
        *err = "invalid static ip";
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wifi_web_config_save(&cfg);
    if (ret != ESP_OK) {
        *err = "nvs write failed";
        return ret;
    }

    web_schedule_restart();
    return ESP_OK;
}
