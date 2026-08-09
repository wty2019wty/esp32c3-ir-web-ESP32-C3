#include <string.h>
#include <stdio.h>
#include "app_web.h"
#include "app_web_internal.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"

#define TAG "web"

#define HTTP_PORT CONFIG_IR_TOOL_HTTP_PORT

static httpd_handle_t s_server = NULL;

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
    cfg.max_uri_handlers = 20; /* registered handlers + headroom */
#if CONFIG_HTTPD_WS_SUPPORT
    cfg.close_fn = web_ws_close_fn;
#endif

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", HTTP_PORT);
        return ESP_FAIL;
    }

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

    esp_err_t err = web_auth_register(s_server);
    if (err != ESP_OK) {
        return err;
    }
    err = web_api_ir_register(s_server);
    if (err != ESP_OK) {
        return err;
    }
    err = web_api_wifi_register(s_server);
    if (err != ESP_OK) {
        return err;
    }
    err = web_ws_register(s_server);
    if (err != ESP_OK) {
        return err;
    }

    char ip[16];
    if (wifi_is_sta_connected() ? wifi_get_sta_ip(ip, sizeof(ip)) : wifi_get_ap_ip(ip, sizeof(ip))) {
        ESP_LOGI(TAG, "Web UI ready: http://%s:%d/", ip, HTTP_PORT);
    } else {
        ESP_LOGI(TAG, "Web UI ready: port %d", HTTP_PORT);
    }
    return ESP_OK;
}
