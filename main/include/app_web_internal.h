#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_ir.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_MAX_BODY_LEN 32768

/* ---- app_web_util.c (shared HTTP/JSON helpers) ---- */
void web_respond_json(httpd_req_t *req, int status, const char *json);
void web_respond_ok(httpd_req_t *req);
char *web_httpd_read_body(httpd_req_t *req);
char *web_status_json(void);            /* caller frees */
int web_frame_to_json(const ir_frame_t *f, char *buf, size_t cap);

/* ---- app_web_auth.c (login / token auth / account config) ---- */
esp_err_t web_auth_register(httpd_handle_t server);
bool web_require_auth(httpd_req_t *req);
bool web_auth_token_ok(const char *token);

/* ---- app_web_api_ir.c (status / frames / play / carrier / rxpause) ---- */
esp_err_t web_api_ir_register(httpd_handle_t server);

/* ---- app_web_api_wifi.c (WiFi config API) ---- */
esp_err_t web_api_wifi_register(httpd_handle_t server);

/* ---- app_web_ws.c (WebSocket status/frame push) ---- */
esp_err_t web_ws_register(httpd_handle_t server);
void web_ws_close_fn(httpd_handle_t hd, int sockfd);

#ifdef __cplusplus
}
#endif
