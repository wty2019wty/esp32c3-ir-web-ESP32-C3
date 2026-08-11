#include <string.h>
#include <stdio.h>
#include "app_web.h"
#include "app_web_internal.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "web"

#define HTTP_PORT CONFIG_IR_TOOL_HTTP_PORT

/* Web UI toggle: when disabled the embedded page is not served, only the
 * /api/ws endpoint stays active (front/back separation). Persisted in NVS. */
#define WEB_NS            "ir_tool"
#define WEB_KEY_UI_ENABLED "web_ui_enabled"
#define WEB_KEY_WS_ORIGIN  "ws_origin"

static httpd_handle_t s_server = NULL;
static int s_web_ui = -1; /* tri-state cache (-1 = not loaded yet) */

/* true = serve the embedded web page; false = WS endpoint only. */
bool web_ui_enabled_get(void)
{
    if (s_web_ui < 0) {
        uint8_t v = 1; /* default: page enabled */
        nvs_handle_t h;
        if (nvs_open(WEB_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, WEB_KEY_UI_ENABLED, &v) != ESP_OK) {
                v = 1;
            }
            nvs_close(h);
        }
        s_web_ui = v;
    }
    return s_web_ui != 0;
}

esp_err_t web_ui_enabled_set(bool enabled, const char **err)
{
    nvs_handle_t h;
    if (nvs_open(WEB_NS, NVS_READWRITE, &h) != ESP_OK) {
        *err = "nvs open failed";
        return ESP_FAIL;
    }
    esp_err_t e = nvs_set_u8(h, WEB_KEY_UI_ENABLED, enabled ? 1 : 0);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        *err = "nvs write failed";
        return e;
    }
    s_web_ui = enabled ? 1 : 0;
    web_schedule_restart();
    return ESP_OK;
}

/* Allowed WebSocket Origin for /api/ws (empty = allow all origins).
 * Persisted in NVS; the embedded page is same-origin by default, and the
 * front/back separation feature serves the page from an external host, so
 * this must stay an opt-in allow-list rather than a hard check. */
#define WEB_ORIGIN_MAX 256

static char s_ws_origin[WEB_ORIGIN_MAX];
static int s_ws_origin_loaded = 0; /* 0 = not loaded yet, 1 = cached */

char *web_origin_get(void)
{
    char *buf = malloc(WEB_ORIGIN_MAX);
    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(WEB_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = WEB_ORIGIN_MAX;
        if (nvs_get_str(h, WEB_KEY_WS_ORIGIN, buf, &len) != ESP_OK) {
            buf[0] = '\0';
        }
        nvs_close(h);
    }
    return buf;
}

esp_err_t web_origin_set(const char *origin, const char **err)
{
    if (!origin) {
        origin = ""; /* allow all */
    }
    if (strlen(origin) >= WEB_ORIGIN_MAX) {
        if (err) *err = "origin too long";
        return ESP_ERR_INVALID_ARG;
    }
    for (const char *p = origin; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (err) *err = "origin contains invalid char";
            return ESP_ERR_INVALID_ARG;
        }
    }
    nvs_handle_t h;
    if (nvs_open(WEB_NS, NVS_READWRITE, &h) != ESP_OK) {
        if (err) *err = "nvs open failed";
        return ESP_FAIL;
    }
    esp_err_t e = nvs_set_str(h, WEB_KEY_WS_ORIGIN, origin);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK && err) {
        *err = "nvs write failed";
    } else if (e == ESP_OK) {
        strlcpy(s_ws_origin, origin, sizeof(s_ws_origin));
        s_ws_origin_loaded = 1;
    }
    return e;
}

/* Match a WS handshake Origin header against the allow-list. Empty config (the
 * default) accepts every origin. The allow-list is cached in RAM (refreshed on
 * set) so per-connection checks never touch flash. On cache load failure we
 * fail OPEN so an OOM can never lock legitimate clients out of the device. */
bool web_origin_allowed(const char *origin)
{
    if (!origin || origin[0] == '\0') {
        return true;
    }
    if (!s_ws_origin_loaded) {
        nvs_handle_t h;
        if (nvs_open(WEB_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_ws_origin);
            s_ws_origin[0] = '\0';
            if (nvs_get_str(h, WEB_KEY_WS_ORIGIN, s_ws_origin, &len) != ESP_OK) {
                s_ws_origin[0] = '\0';
            }
            nvs_close(h);
        }
        s_ws_origin_loaded = 1;
    }
    return s_ws_origin[0] == '\0' || strcmp(s_ws_origin, origin) == 0;
}

/* Embedded web UI (main/web/index.html via EMBED_TXTFILES) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* always fetch the latest embedded page (browser cache caused stale UIs) */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

esp_err_t web_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.stack_size = 16384; /* handlers hold one ir_frame_t (~4.2KB at 1024 segments) */
    cfg.max_uri_handlers = 8; /* "/", "/index.html", "/api/ws" + headroom */
#if CONFIG_HTTPD_WS_SUPPORT
    cfg.close_fn = web_ws_close_fn;
#endif

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", HTTP_PORT);
        return ESP_FAIL;
    }

    /* When the embedded web UI is disabled, the page is not served (front-end
     * is hosted elsewhere); only the /api/ws endpoint stays active. */
    if (web_ui_enabled_get()) {
        static const httpd_uri_t uris[] = {
            {.uri = "/",           .method = HTTP_GET, .handler = index_handler},
            {.uri = "/index.html", .method = HTTP_GET, .handler = index_handler},
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
            if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
                return ESP_FAIL;
            }
        }
    }

    esp_err_t err = web_ws_register(s_server);
    if (err != ESP_OK) {
        return err;
    }

    if (!web_ui_enabled_get()) {
        ESP_LOGI(TAG, "Web UI disabled: page not served, ws://<ip>:%d/api/ws active", HTTP_PORT);
        return ESP_OK;
    }
    char ip[16];
    if (wifi_is_sta_connected() ? wifi_get_sta_ip(ip, sizeof(ip)) : wifi_get_ap_ip(ip, sizeof(ip))) {
        ESP_LOGI(TAG, "Web UI ready: http://%s:%d/", ip, HTTP_PORT);
    } else {
        ESP_LOGI(TAG, "Web UI ready: port %d", HTTP_PORT);
    }
    return ESP_OK;
}
