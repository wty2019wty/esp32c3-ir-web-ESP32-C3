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
#define LOGIN_LOCK_IP_MAX 8                        /* distinct client IPs tracked for lockout */

typedef struct {
    char user[33];
    char pass[65];
} web_auth_cfg_t;

/* Per-client-IP lockout: consecutive failures from one source IP lock that IP
 * (not the whole device), so a LAN attacker cannot lock the admin out globally.
 * Entries are created on failure and cleared on success. */
typedef struct {
    uint32_t ip;            /* peer IP, host byte order; 0 = free slot */
    int fails;              /* consecutive failed logins from this IP */
    int64_t lock_until;     /* 0 = not locked */
} auth_ip_lock_t;

static char s_token[AUTH_TOKEN_LEN + 1] = ""; /* single active session token */
static int64_t s_token_ts = 0;                 /* token issue time (us since boot) */
static int s_login_fails = 0;                  /* consecutive failed logins (IP unknown fallback) */
static int64_t s_login_lock_until = 0;         /* login lockout until this time (us) */
static uint32_t s_auth_gen = 1;                /* bumped whenever the session is invalidated */
static auth_ip_lock_t s_ip_locks[LOGIN_LOCK_IP_MAX];
static int s_single_session_cache = -1;        /* NVS single-session flag cache (-1 = unloaded) */

/* Parse "a.b.c.d" into a host-order uint32; returns 0 when unknown/parseable
 * only as 0.0.0.0 (which is never a real peer), so 0 doubles as "no IP". */
static uint32_t auth_parse_ip(const char *s)
{
    if (!s) {
        return 0;
    }
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) {
        return 0;
    }
    uint32_t v = (a << 24) | (b << 16) | (c << 8) | d;
    return v == 0 ? 0 : v;
}

static auth_ip_lock_t *auth_ip_lock_find(uint32_t ip, bool create)
{
    auth_ip_lock_t *free_slot = NULL;
    for (int i = 0; i < LOGIN_LOCK_IP_MAX; i++) {
        if (s_ip_locks[i].ip == ip) {
            return &s_ip_locks[i];
        }
        if (!free_slot && s_ip_locks[i].ip == 0) {
            free_slot = &s_ip_locks[i];
        }
    }
    if (create && free_slot) {
        free_slot->ip = ip;
        free_slot->fails = 0;
        free_slot->lock_until = 0;
        return free_slot;
    }
    return NULL;
}

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
    if (s_single_session_cache < 0) {
        uint8_t v = 1; /* default: single-session on */
        nvs_handle_t h;
        if (nvs_open(AUTH_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, AUTH_KEY_SINGLE, &v) != ESP_OK) {
                v = 1;
            }
            nvs_close(h);
        }
        s_single_session_cache = v != 0;
    }
    return s_single_session_cache != 0;
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
    if (e == ESP_OK && single_session) {
        s_single_session_cache = *single_session ? 1 : 0;
    }
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

/* True while a live session exists (token issued and not yet expired). On
 * expiry the session is invalidated (generation bumped), so connected
 * WebSocket sessions are rejected on their next command instead of silently
 * staying authorized past the documented 24h TTL. */
bool web_auth_session_live(void)
{
    if (s_token[0] == '\0') {
        return false;
    }
    if (s_token_ts == 0 || esp_timer_get_time() - s_token_ts > TOKEN_TTL_US) {
        ESP_LOGW(TAG, "auth: session token expired");
        web_auth_invalidate();
        return false;
    }
    return true;
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
        web_auth_invalidate();
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
 * peer_ip identifies the connecting client for the per-IP failure lockout
 * (NULL/unknown = fall back to the global counter). Returns a malloc'd complete
 * response JSON (with "type":"login"), caller frees. On success the caller
 * should mark the connection as authenticated. */
esp_err_t web_auth_login(const char *user, const char *pass, const char *peer_ip,
                         char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t ip = auth_parse_ip(peer_ip);
    auth_ip_lock_t *lock = ip ? auth_ip_lock_find(ip, false) : NULL;

    int64_t now = esp_timer_get_time();
    int64_t lock_until = lock ? lock->lock_until : s_login_lock_until;
    bool locked = lock ? (now < lock_until) : (now < s_login_lock_until);
    if (locked) {
        int remain = (int)((lock_until - now + 999999) / 1000000);
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
        if (ip) {
            /* create a per-IP entry (the lock check above only looks for an
             * existing one); if the table is full fall back to the global count */
            lock = auth_ip_lock_find(ip, true);
        }
        if (lock) {
            if (++lock->fails >= LOGIN_MAX_FAILS) {
                lock->lock_until = now + LOGIN_LOCK_US;
                lock->fails = 0;
                ESP_LOGW(TAG, "login: too many failures from this IP, locked for %lld s",
                         (long long)(LOGIN_LOCK_US / 1000000));
            }
        } else {
            s_login_fails++;
            if (s_login_fails >= LOGIN_MAX_FAILS) {
                s_login_fails = 0;
                s_login_lock_until = now + LOGIN_LOCK_US;
                ESP_LOGW(TAG, "login: too many failures, locked for %lld s",
                         (long long)(LOGIN_LOCK_US / 1000000));
            }
        }
        *out_json = strdup("{\"type\":\"login\",\"ok\":false,\"error\":\"bad credentials\"}");
        return ESP_FAIL;
    }
    /* success: clear the lockout state for this client IP and the fallback */
    if (lock) {
        lock->fails = 0;
        lock->lock_until = 0;
    }
    s_login_fails = 0;
    s_login_lock_until = 0;

    if (s_token[0] == '\0') {
        /* first login (or session expired/logged out): mint a fresh token and
         * bump the generation so any residual session is invalidated too */
        token_new();
        web_auth_gen_bump();
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
    if (!*out_json) {
        /* caller treats NULL + non-OK as a failed login and must NOT mark the
         * connection authenticated (see app_web_ws.c login branch) */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Current login user + session policy as a JSON string (password is never
 * returned; caller frees). Built with cJSON so a user name containing a quote
 * or backslash (allowed for legacy NVS values / menuconfig defaults) cannot
 * break the settings page JSON. */
char *web_authcfg_get_json(void)
{
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "user", cfg.user);
    cJSON_AddBoolToObject(root, "single_session", web_auth_single_session_get());
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
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
        /* Reject characters that would make the user name ambiguous in the
         * settings echo / confusing to log in with. */
        if (strlen(j->valuestring) >= sizeof(cfg.user) ||
            strpbrk(j->valuestring, "\"\\")) {
            *err = "user must be 1-32 chars without quotes";
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(j->valuestring, cfg.user) != 0) {
            strlcpy(cfg.user, j->valuestring, sizeof(cfg.user));
            creds_changed = true;
        }
    }
    j = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        if (strlen(j->valuestring) < 4 || strlen(j->valuestring) >= sizeof(cfg.pass)) {
            *err = "pass must be 4-64 chars";
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
