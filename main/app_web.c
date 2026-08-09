#include <string.h>
#include <stdio.h>
#include "app_web.h"
#include "app_web_internal.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "web"

#define HTTP_PORT CONFIG_IR_TOOL_HTTP_PORT

/* Web UI toggle: when disabled the embedded page is not served, only the
 * /api/ws endpoint stays active (front/back separation). Persisted in NVS. */
#define WEB_NS            "ir_tool"
#define WEB_KEY_UI_ENABLED "web_ui_enabled"

static httpd_handle_t s_server = NULL;
static int s_web_ui = -1; /* tri-state cache (-1 = not loaded yet) */

static void web_restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Restarting to apply web config...");
    esp_restart();
}

static void schedule_restart(void)
{
    static esp_timer_handle_t t = NULL;
    if (!t) {
        const esp_timer_create_args_t args = {
            .callback = web_restart_cb,
            .name = "web_restart",
        };
        esp_timer_create(&args, &t);
    }
    if (t) {
        esp_timer_start_once(t, 2 * 1000 * 1000);
    }
}

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
    schedule_restart();
    return ESP_OK;
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
