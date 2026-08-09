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

/* ---- app_web_util.c (shared JSON helpers) ---- */
char *web_status_json(void);            /* caller frees */
int web_frame_to_json(const ir_frame_t *f, char *buf, size_t cap);

/* ---- app_web_api_ir.c (IR command cores, WS-only) ---- */
esp_err_t web_ir_play_exec(cJSON *root);              /* {"type","freq?",value/data/seq} -> ESP_OK if accepted */
esp_err_t web_ir_carrier_exec(cJSON *root, uint32_t *freq_out); /* ESP_ERR_NOT_FOUND = missing freq */
esp_err_t web_ir_rxpause_exec(cJSON *root, bool *enabled_out);  /* ESP_ERR_NOT_FOUND = missing enabled */

/* ---- app_web_api_wifi.c (WiFi config cores, WS-only) ---- */
char *web_wificfg_get_json(void);                     /* caller frees */
esp_err_t web_wificfg_set(cJSON *root, const char **err);

/* ---- app_web_auth.c (login / token auth / account config) ---- */
bool web_auth_token_ok(const char *token);
void web_auth_invalidate(void);                       /* clear token + bump session generation */
uint32_t web_auth_get_gen(void);                      /* current session generation */
char *web_authcfg_get_json(void);                     /* caller frees */
esp_err_t web_authcfg_set(cJSON *root, bool *invalidated, const char **err);
bool web_auth_single_session_get(void);               /* true = every login kicks out older sessions (default on) */
esp_err_t web_auth_renew(uint32_t *expires_in);
/* WS login (the only unauthenticated operation). Returns a malloc'd complete
 * response JSON with "type":"login" (caller frees); ESP_OK = success, caller
 * must then mark the connection as authenticated. */
esp_err_t web_auth_login(const char *user, const char *pass, char **out_json);

/* ---- app_web.c (HTTP server bootstrap + web UI toggle) ---- */
bool web_ui_enabled_get(void);                        /* true = serve embedded page (default) */
esp_err_t web_ui_enabled_set(bool enabled, const char **err); /* saves to NVS + schedules restart */

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
