#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_web_internal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#define TAG "web"

/* web login (token auth; NVS user/pass, default admin/admin) */
#define AUTH_NS         "ir_tool"
#define AUTH_KEY_USER   "web_user"
#define AUTH_KEY_PASS   "web_pass"
#define AUTH_KEY_SINGLE "auth_single"
#define AUTH_DEF_USER   "admin"
#define AUTH_DEF_PASS   "admin"
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

/* Bump the session generation; connected WebSocket sessions authenticated
 * against an older generation are rejected (see app_web_ws.c). */
static void web_auth_gen_bump(void)
{
    if (++s_auth_gen == 0) {
        s_auth_gen = 1;
    }
}

/* Invalidate the active session token and bump the session generation so that
 * already-connected WebSocket sessions authenticated against the old token are
 * rejected (see app_web_ws.c). */
void web_auth_invalidate(void)
{
    s_token[0] = '\0';
    s_token_ts = 0;
    web_auth_gen_bump();
}

uint32_t web_auth_get_gen(void)
{
    return s_auth_gen;
}

/* Single-session mode: every login issues a brand-new token and bumps the
 * session generation, so logging in on another device/tab kicks out all
 * previously authenticated sessions. Persisted in NVS (default: on). */
bool web_auth_single_session_get(void)
{
    uint8_t v = 1; /* default: single-session on */
    nvs_handle_t h;
    if (nvs_open(AUTH_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, AUTH_KEY_SINGLE, &v) != ESP_OK) {
            v = 1;
        }
        nvs_close(h);
    }
    return v != 0;
}

/* Persist credentials and/or the single-session policy through one NVS handle
 * and a single commit, so a partial failure cannot leave the two settings out
 * of sync. Pass NULL for a field that must be left untouched. */
static esp_err_t web_auth_save_all(const web_auth_cfg_t *cfg, const bool *single_session,
                                   const char **err)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(AUTH_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        if (err) *err = "nvs open failed";
        return e;
    }
    if (cfg) {
        e = nvs_set_str(h, AUTH_KEY_USER, cfg->user);
        if (e == ESP_OK) {
            e = nvs_set_str(h, AUTH_KEY_PASS, cfg->pass);
        }
    }
    if (e == ESP_OK && single_session) {
        e = nvs_set_u8(h, AUTH_KEY_SINGLE, *single_session ? 1 : 0);
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK && err) {
        *err = "nvs write failed";
    }
    return e;
}

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

/* WS login entry (the only unauthenticated operation — REST /api/login is gone).
 * Returns a malloc'd complete response JSON (with "type":"login"), caller frees.
 * On success the caller should mark the connection as authenticated. */
esp_err_t web_auth_login(const char *user, const char *pass, char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_login_lock_until) {
        int remain = (int)((s_login_lock_until - now + 999999) / 1000000);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"login\",\"ok\":false,\"error\":\"too many attempts\",\"retry_after\":%d}",
                 remain);
        *out_json = strdup(buf);
        return ESP_ERR_TIMEOUT;
    }
    if (!user || !pass) {
        *out_json = strdup("{\"type\":\"login\",\"ok\":false,\"error\":\"bad login\"}");
        return ESP_ERR_INVALID_ARG;
    }

    web_auth_cfg_t cfg;
    web_auth_load(&cfg);
    bool ok = ct_equal(user, cfg.user) && ct_equal(pass, cfg.pass);
    if (!ok) {
        s_login_fails++;
        if (s_login_fails >= LOGIN_MAX_FAILS) {
            s_login_fails = 0;
            s_login_lock_until = esp_timer_get_time() + LOGIN_LOCK_US;
            ESP_LOGW(TAG, "login: too many failures, locked for %lld s",
                     (long long)(LOGIN_LOCK_US / 1000000));
        }
        *out_json = strdup("{\"type\":\"login\",\"ok\":false,\"error\":\"bad credentials\"}");
        return ESP_FAIL;
    }
    s_login_fails = 0;
    s_login_lock_until = 0;

    if (s_token[0] == '\0') {
        /* first login: mint a token */
        token_new();
    } else if (web_auth_single_session_get()) {
        /* single-session mode: a fresh login issues a brand-new token and bumps
         * the session generation, kicking out every previously authenticated
         * WS session (they hold the old token/gen) */
        token_new();
        web_auth_gen_bump();
    } else {
        /* reuse the existing session token if one is active, so multiple tabs
         * / devices sharing the same credentials do not kick each other out */
        s_token_ts = esp_timer_get_time(); /* refresh validity for active sessions */
    }
    bool must_change = creds_are_default(&cfg);
    ESP_LOGI(TAG, "login ok, token issued%s", must_change ? " (default password, must change)" : "");
    char buf[176];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"login\",\"ok\":true,\"token\":\"%s\",\"expires_in\":%llu,\"must_change_pwd\":%s}",
             s_token, (unsigned long long)(TOKEN_TTL_US / 1000000), must_change ? "true" : "false");
    *out_json = strdup(buf);
    return ESP_OK;
}

/* Current login user + session policy as a JSON string (password is never
 * returned; caller frees). */
char *web_authcfg_get_json(void)
{
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);
    size_t cap = strlen(cfg.user) + 64;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    snprintf(buf, cap, "{\"user\":\"%s\",\"single_session\":%s}",
             cfg.user, web_auth_single_session_get() ? "true" : "false");
    return buf;
}

/* Apply login credential / session-policy changes from a JSON body; pass empty
 * = keep current. When user/pass actually change the session is invalidated
 * (forced re-login); a pure single_session toggle does not log anyone out.
 * *invalidated (may be NULL) is set to true when the session was invalidated. */
esp_err_t web_authcfg_set(cJSON *root, bool *invalidated, const char **err)
{
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);

    bool creds_changed = false;
    cJSON *j = cJSON_GetObjectItem(root, "user");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        if (strcmp(j->valuestring, cfg.user) != 0) {
            strlcpy(cfg.user, j->valuestring, sizeof(cfg.user));
            creds_changed = true;
        }
    }
    j = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        if (strlen(j->valuestring) < 4 || strlen(j->valuestring) >= sizeof(cfg.pass)) {
            *err = "pass must be 4-63 chars";
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(j->valuestring, cfg.pass) != 0) {
            strlcpy(cfg.pass, j->valuestring, sizeof(cfg.pass));
            creds_changed = true;
        }
    }

    cJSON *s = cJSON_GetObjectItem(root, "single_session");
    bool single_present = cJSON_IsBool(s);

    if (creds_changed || single_present) {
        bool single_val = cJSON_IsTrue(s);
        esp_err_t ret = web_auth_save_all(creds_changed ? &cfg : NULL,
                                          single_present ? &single_val : NULL,
                                          err);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (creds_changed) {
        /* credentials changed: force a fresh login (kills existing WS sessions too) */
        web_auth_invalidate();
    }
    if (invalidated) {
        *invalidated = creds_changed;
    }
    return ESP_OK;
}
