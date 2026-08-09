#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_web_internal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#define TAG "web"

/* web login (token auth; NVS user/pass, default admin/admin) */
#define AUTH_NS        "ir_tool"
#define AUTH_KEY_USER  "web_user"
#define AUTH_KEY_PASS  "web_pass"
#define AUTH_DEF_USER  "admin"
#define AUTH_DEF_PASS  "admin"
#define AUTH_TOKEN_LEN 32   /* hex chars */
#define TOKEN_TTL_US   (24LL * 60 * 60 * 1000000)  /* session validity: 24h */
#define LOGIN_MAX_FAILS 5
#define LOGIN_LOCK_US   (30LL * 1000000)           /* lockout after repeated failures */

typedef struct {
    char user[33];
    char pass[65];
} web_auth_cfg_t;

static char s_token[AUTH_TOKEN_LEN + 1] = ""; /* single active session token */
static int64_t s_token_ts = 0;                 /* token issue time (us since boot) */
static int s_login_fails = 0;                  /* consecutive failed logins */
static int64_t s_login_lock_until = 0;         /* login lockout until this time (us) */
static uint32_t s_auth_gen = 1;                /* bumped whenever the session is invalidated */

/* Invalidate the active session token and bump the session generation so that
 * already-connected WebSocket sessions authenticated against the old token are
 * rejected (see app_web_ws.c). */
void web_auth_invalidate(void)
{
    s_token[0] = '\0';
    s_token_ts = 0;
    if (++s_auth_gen == 0) {
        s_auth_gen = 1;
    }
}

uint32_t web_auth_get_gen(void)
{
    return s_auth_gen;
}

/* Extend the current session for another full TTL; fails if no session is active. */
esp_err_t web_auth_renew(uint32_t *expires_in)
{
    if (s_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    s_token_ts = esp_timer_get_time();
    if (expires_in) {
        *expires_in = (uint32_t)(TOKEN_TTL_US / 1000000);
    }
    return ESP_OK;
}

static void web_auth_load(web_auth_cfg_t *cfg)
{
    strlcpy(cfg->user, AUTH_DEF_USER, sizeof(cfg->user));
    strlcpy(cfg->pass, AUTH_DEF_PASS, sizeof(cfg->pass));
    nvs_handle_t h;
    if (nvs_open(AUTH_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(cfg->user);
    if (nvs_get_str(h, AUTH_KEY_USER, cfg->user, &len) != ESP_OK) {
        strlcpy(cfg->user, AUTH_DEF_USER, sizeof(cfg->user));
    }
    len = sizeof(cfg->pass);
    if (nvs_get_str(h, AUTH_KEY_PASS, cfg->pass, &len) != ESP_OK) {
        strlcpy(cfg->pass, AUTH_DEF_PASS, sizeof(cfg->pass));
    }
    nvs_close(h);
}

static esp_err_t web_auth_save(const web_auth_cfg_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(AUTH_NS, NVS_READWRITE, &h), TAG, "open nvs");
    esp_err_t err = nvs_set_str(h, AUTH_KEY_USER, cfg->user);
    if (err == ESP_OK) err = nvs_set_str(h, AUTH_KEY_PASS, cfg->pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Constant-time equality; returns false on length mismatch. */
static bool ct_equal(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) {
        return false;
    }
    unsigned char acc = 0;
    for (size_t i = 0; i < la; i++) {
        acc |= (unsigned char)(a[i] ^ b[i]);
    }
    return acc == 0;
}

static bool creds_are_default(const web_auth_cfg_t *cfg)
{
    return strcmp(cfg->user, AUTH_DEF_USER) == 0 &&
           strcmp(cfg->pass, AUTH_DEF_PASS) == 0;
}

/* Check a token string against the active session (constant-time, with TTL). */
bool web_auth_token_ok(const char *token)
{
    if (s_token[0] == '\0') {
        ESP_LOGW(TAG, "auth: no token issued (login first)");
        return false;
    }
    if (s_token_ts == 0 || esp_timer_get_time() - s_token_ts > TOKEN_TTL_US) {
        ESP_LOGW(TAG, "auth: session token expired");
        s_token[0] = '\0';
        s_token_ts = 0;
        return false;
    }
    return token && ct_equal(token, s_token);
}

/* Verify the X-Auth-Token header against the active session token. */
static bool web_auth_ok(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "X-Auth-Token");
    if (len == 0) {
        char ua[512] = "";
        size_t ulen = httpd_req_get_hdr_value_len(req, "User-Agent");
        if (ulen > 0 && ulen < sizeof(ua)) {
            httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
        }
        /* requests without a User-Agent are non-browser clients (noise), stay quiet */
        if (ua[0] != '\0') {
            ESP_LOGW(TAG, "auth: no token header (UA: %s)", ua);
        }
        return false;
    }
    if (len > 64) {
        ESP_LOGW(TAG, "auth: token header too long (%u)", (unsigned)len);
        return false;
    }
    char buf[65];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW(TAG, "auth: header read failed");
        return false;
    }
    if (!web_auth_token_ok(buf)) {
        ESP_LOGW(TAG, "auth: token mismatch (got \"%.12s...\" len=%u, expect \"%.8s...\")",
                 buf, (unsigned)strlen(buf), s_token);
        return false;
    }
    return true;
}

/* Generate a fresh session token (invalidates any previous one). */
static void token_new(void)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        snprintf(&s_token[i * 2], 3, "%02x", rnd[i]);
    }
    s_token_ts = esp_timer_get_time();
}

/* Require auth; on failure sends 401 and returns false. */
bool web_require_auth(httpd_req_t *req)
{
    if (web_auth_ok(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return false;
}

/* POST /api/login {"user":"...","pass":"..."} -> {"ok":true,"token":"..."} */
static esp_err_t login_handler(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now < s_login_lock_until) {
        int remain = (int)((s_login_lock_until - now + 999999) / 1000000);
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"error\":\"too many attempts\",\"retry_after\":%d}", remain);
        web_respond_json(req, 429, buf);
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
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);
    cJSON *u = cJSON_GetObjectItem(root, "user");
    cJSON *p = cJSON_GetObjectItem(root, "pass");
    bool ok = cJSON_IsString(u) && cJSON_IsString(p) &&
              ct_equal(u->valuestring, cfg.user) &&
              ct_equal(p->valuestring, cfg.pass);
    cJSON_Delete(root);
    if (!ok) {
        s_login_fails++;
        if (s_login_fails >= LOGIN_MAX_FAILS) {
            s_login_fails = 0;
            s_login_lock_until = esp_timer_get_time() + LOGIN_LOCK_US;
            ESP_LOGW(TAG, "login: too many failures, locked for %lld s",
                     (long long)(LOGIN_LOCK_US / 1000000));
        }
        web_respond_json(req, 401, "{\"error\":\"bad credentials\"}");
        return ESP_OK;
    }
    s_login_fails = 0;
    s_login_lock_until = 0;

    /* reuse the existing session token if one is active, so multiple tabs
     * / devices sharing the same credentials do not kick each other out */
    if (s_token[0] == '\0') {
        token_new();
    } else {
        s_token_ts = esp_timer_get_time(); /* refresh validity for active sessions */
    }
    bool must_change = creds_are_default(&cfg);
    ESP_LOGI(TAG, "login ok, token issued%s", must_change ? " (default password, must change)" : "");
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"token\":\"%s\",\"expires_in\":%lld,\"must_change_pwd\":%s}",
             s_token, (long long)(TOKEN_TTL_US / 1000000), must_change ? "true" : "false");
    web_respond_json(req, 200, buf);
    return ESP_OK;
}

/* GET /api/authcfg -> current login user (password is never returned) */
char *web_authcfg_get_json(void)
{
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);
    size_t cap = strlen(cfg.user) + 32;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    snprintf(buf, cap, "{\"user\":\"%s\"}", cfg.user);
    return buf;
}

static esp_err_t authcfg_get_handler(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *s = web_authcfg_get_json();
    if (!s) {
        web_respond_json(req, 400, "{\"error\":\"oom\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

/* Apply login credential changes from a JSON body; pass empty = keep current.
 * On success the session is invalidated (forced re-login). */
esp_err_t web_authcfg_set(cJSON *root, const char **err)
{
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);

    cJSON *j = cJSON_GetObjectItem(root, "user");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        strlcpy(cfg.user, j->valuestring, sizeof(cfg.user));
    }
    j = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        if (strlen(j->valuestring) < 4 || strlen(j->valuestring) >= sizeof(cfg.pass)) {
            *err = "pass must be 4-63 chars";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.pass, j->valuestring, sizeof(cfg.pass));
    }

    esp_err_t ret = web_auth_save(&cfg);
    if (ret != ESP_OK) {
        *err = "nvs write failed";
        return ret;
    }
    /* credentials changed: force a fresh login (kills existing WS sessions too) */
    web_auth_invalidate();
    return ESP_OK;
}

/* POST /api/authcfg {"user":"...","pass":"..."} — pass empty = keep current */
static esp_err_t authcfg_post_handler(httpd_req_t *req)
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

    const char *err = NULL;
    esp_err_t ret = web_authcfg_set(root, &err);
    cJSON_Delete(root);
    if (ret == ESP_OK) {
        web_respond_ok(req);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", err ? err : "invalid");
        web_respond_json(req, 400, buf);
    }
    return ESP_OK;
}

/* POST /api/logout - invalidate the current session token. */
static esp_err_t logout_handler(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    web_auth_invalidate();
    web_respond_ok(req);
    return ESP_OK;
}

/* POST /api/renew - extend the current session for another full TTL. */
static esp_err_t renew_handler(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    uint32_t expires_in = 0;
    esp_err_t ret = web_auth_renew(&expires_in);
    if (ret != ESP_OK) {
        web_respond_json(req, 401, "{\"error\":\"unauthorized\"}");
        return ESP_OK;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"expires_in\":%lu}", (unsigned long)expires_in);
    web_respond_json(req, 200, buf);
    return ESP_OK;
}

esp_err_t web_auth_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/login",   .method = HTTP_POST, .handler = login_handler},
        {.uri = "/api/authcfg", .method = HTTP_GET,  .handler = authcfg_get_handler},
        {.uri = "/api/authcfg", .method = HTTP_POST, .handler = authcfg_post_handler},
        {.uri = "/api/logout",  .method = HTTP_POST, .handler = logout_handler},
        {.uri = "/api/renew",   .method = HTTP_POST, .handler = renew_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}
