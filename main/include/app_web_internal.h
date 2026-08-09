#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_ir.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

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

/* ---- app_web_api_ir.c (status / frames / play / carrier / rxpause) ---- */
esp_err_t web_api_ir_register(httpd_handle_t server);

/* shared command cores (used by REST handlers and WebSocket RPC) */
esp_err_t web_ir_play_exec(cJSON *root);              /* {"type","freq?",value/data/seq} -> ESP_OK if accepted */
esp_err_t web_ir_carrier_exec(cJSON *root, uint32_t *freq_out); /* ESP_ERR_NOT_FOUND = missing freq */
esp_err_t web_ir_rxpause_exec(cJSON *root, bool *enabled_out);  /* ESP_ERR_NOT_FOUND = missing enabled */

/* ---- app_web_api_wifi.c (WiFi config API) ---- */
esp_err_t web_api_wifi_register(httpd_handle_t server);
char *web_wificfg_get_json(void);                     /* caller frees */
esp_err_t web_wificfg_set(cJSON *root, const char **err);

/* ---- app_web_auth.c (login / token auth / account config) ---- */
esp_err_t web_auth_register(httpd_handle_t server);
bool web_require_auth(httpd_req_t *req);
bool web_auth_token_ok(const char *token);
void web_auth_invalidate(void);                       /* clear token + bump session generation */
uint32_t web_auth_get_gen(void);                      /* current session generation */
char *web_authcfg_get_json(void);                     /* caller frees */
esp_err_t web_authcfg_set(cJSON *root, const char **err);
esp_err_t web_auth_renew(uint32_t *expires_in);

/* ---- app_web_rpc.c (shared REST/WebSocket command execution) ---- */
/* Execute a command on a parsed JSON body and return the REST-equivalent
 * response JSON (caller frees), or NULL with *err set to a short message. */
char *web_rpc_exec(const char *cmd, cJSON *body, const char **err);
/* Incremental frame history as a single JSON object; a bounded buffer may
 * truncate the list, in which case "truncated":true is embedded and last_seq
 * reflects the last frame actually included (caller frees). */
char *web_rpc_frames(uint32_t since);

/* ---- app_web_ws.c (WebSocket status/frame push + command RPC) ---- */
esp_err_t web_ws_register(httpd_handle_t server);
void web_ws_close_fn(httpd_handle_t hd, int sockfd);

#ifdef __cplusplus
}
#endif
