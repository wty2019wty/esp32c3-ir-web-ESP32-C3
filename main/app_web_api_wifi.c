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

static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Restarting to apply WiFi config...");
    esp_restart();
}

static void schedule_restart(void)
{
    /* restart 2s later so the HTTP response is flushed first */
    static esp_timer_handle_t t = NULL;
    if (!t) {
        const esp_timer_create_args_t args = {
            .callback = restart_cb,
            .name = "restart",
        };
        esp_timer_create(&args, &t);
    }
    if (t) {
        esp_timer_start_once(t, 2 * 1000 * 1000);
    }
}

/* GET /api/wificfg -> current (NVS) WiFi configuration */
static esp_err_t wificfg_get_handler(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    wifi_web_config_t cfg;
    wifi_web_config_load(&cfg);
    char ip[16], gw[16], mask[16], dns[16];
    format_ipv4(cfg.sta_ip, ip, sizeof(ip));
    format_ipv4(cfg.sta_gw, gw, sizeof(gw));
    format_ipv4(cfg.sta_mask, mask, sizeof(mask));
    format_ipv4(cfg.sta_dns, dns, sizeof(dns));

    char *buf = NULL;
    size_t cap = 1024;
    buf = malloc(cap);
    if (!buf) {
        web_respond_json(req, 400, "{\"error\":\"oom\"}");
        return ESP_OK;
    }
    int n = snprintf(buf, cap,
        "{\"ap_ssid\":\"%s\",\"ap_password\":\"\",\"ap_password_set\":%s,"
        "\"sta_ssid\":\"%s\",\"sta_password\":\"\",\"sta_password_set\":%s,"
        "\"sta_dhcp\":%s,\"sta_ip\":\"%s\",\"sta_gw\":\"%s\","
        "\"sta_mask\":\"%s\",\"sta_dns\":\"%s\"}",
        cfg.ap_ssid, cfg.ap_password[0] ? "true" : "false",
        cfg.sta_ssid, cfg.sta_password[0] ? "true" : "false",
        cfg.sta_dhcp ? "true" : "false", ip, gw, mask, dns);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    (void)n;
    free(buf);
    return ESP_OK;
}

/* POST /api/wificfg -> save config, respond, restart */
static esp_err_t wificfg_post_handler(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_httpd_read_body(req);
    if (!body) {
        web_respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        web_respond_json(req, 400, "{\"error\":\"bad json\"}");
        return ESP_OK;
    }

    wifi_web_config_t cfg;
    wifi_web_config_load(&cfg); /* start from current values */

    /* helper to grab a string field */
    cJSON *j = NULL;
    j = cJSON_GetObjectItem(root, "ap_ssid");
    if (cJSON_IsString(j)) {
        strlcpy(cfg.ap_ssid, j->valuestring, sizeof(cfg.ap_ssid));
    }
    j = cJSON_GetObjectItem(root, "ap_password");
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(cfg.ap_password)) {
            cJSON_Delete(root);
            web_respond_json(req, 400, "{\"error\":\"ap_password too long\"}");
            return ESP_OK;
        }
        strlcpy(cfg.ap_password, j->valuestring, sizeof(cfg.ap_password));
    }
    /* null or absent: keep current password */
    j = cJSON_GetObjectItem(root, "sta_ssid");
    if (cJSON_IsString(j)) {
        strlcpy(cfg.sta_ssid, j->valuestring, sizeof(cfg.sta_ssid));
    }
    j = cJSON_GetObjectItem(root, "sta_password");
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(cfg.sta_password)) {
            cJSON_Delete(root);
            web_respond_json(req, 400, "{\"error\":\"sta_password too long\"}");
            return ESP_OK;
        }
        strlcpy(cfg.sta_password, j->valuestring, sizeof(cfg.sta_password));
    }
    /* null or absent: keep current password */
    j = cJSON_GetObjectItem(root, "sta_dhcp");
    if (cJSON_IsBool(j)) {
        cfg.sta_dhcp = cJSON_IsTrue(j);
    }
    /* static IP fields (only used when sta_dhcp=false) */
    const char *ip_str = NULL;
    uint32_t tmp;
    j = cJSON_GetObjectItem(root, "sta_ip");
    if (cJSON_IsString(j) && parse_ipv4(j->valuestring, &tmp)) {
        cfg.sta_ip = tmp;
        ip_str = j->valuestring;
    }
    j = cJSON_GetObjectItem(root, "sta_gw");
    if (cJSON_IsString(j) && parse_ipv4(j->valuestring, &tmp)) {
        cfg.sta_gw = tmp;
    }
    j = cJSON_GetObjectItem(root, "sta_mask");
    if (cJSON_IsString(j) && parse_ipv4(j->valuestring, &tmp)) {
        cfg.sta_mask = tmp;
    }
    j = cJSON_GetObjectItem(root, "sta_dns");
    if (cJSON_IsString(j) && parse_ipv4(j->valuestring, &tmp)) {
        cfg.sta_dns = tmp;
    }

    /* validation */
    if (cfg.ap_ssid[0] == '\0') {
        cJSON_Delete(root);
        web_respond_json(req, 400, "{\"error\":\"ap_ssid empty\"}");
        return ESP_OK;
    }
    if (cfg.ap_password[0] != '\0' && strlen(cfg.ap_password) < 8) {
        cJSON_Delete(root);
        web_respond_json(req, 400, "{\"error\":\"ap_password needs >=8 chars or empty\"}");
        return ESP_OK;
    }
    if (!cfg.sta_dhcp && (cfg.sta_ip == 0 || cfg.sta_ip == 0xFFFFFFFFU)) {
        cJSON_Delete(root);
        web_respond_json(req, 400, "{\"error\":\"invalid static ip\"}");
        return ESP_OK;
    }
    (void)ip_str;

    esp_err_t err = wifi_web_config_save(&cfg);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        web_respond_json(req, 400, "{\"error\":\"nvs write failed\"}");
        return ESP_OK;
    }

    web_respond_json(req, 200, "{\"ok\":true,\"restart\":true}");
    schedule_restart();
    return ESP_OK;
}

esp_err_t web_api_wifi_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/wificfg", .method = HTTP_GET,  .handler = wificfg_get_handler},
        {.uri = "/api/wificfg", .method = HTTP_POST, .handler = wificfg_post_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}
