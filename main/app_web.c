#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "app_web.h"
#include "app_ir.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "web"

#define HTTP_PORT        CONFIG_IR_TOOL_HTTP_PORT
#define MAX_BODY_LEN     32768

static httpd_handle_t s_server = NULL;

/* forward declarations (defined below) */
static void respond_json(httpd_req_t *req, int status, const char *json);
static char *httpd_read_body(httpd_req_t *req);

/* ---- web login (token auth; NVS user/pass, default admin/admin) ---- */
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
static bool web_auth_token_ok(const char *token)
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
static bool require_auth(httpd_req_t *req)
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
        respond_json(req, 429, buf);
        return ESP_OK;
    }

    char *body = httpd_read_body(req);
    if (!body) {
        respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        respond_json(req, 400, "{\"error\":\"bad json\"}");
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
        respond_json(req, 401, "{\"error\":\"bad credentials\"}");
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
    respond_json(req, 200, buf);
    return ESP_OK;
}

/* Embedded web UI (main/web/index.html via EMBED_TXTFILES) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ---------- helpers ---------- */

static void respond_json(httpd_req_t *req, int status, const char *json)
{
    const char *status_str = "200 OK";
    if (status == 400) {
        status_str = "400 Bad Request";
    } else if (status == 401) {
        status_str = "401 Unauthorized";
    } else if (status == 429) {
        status_str = "429 Too Many Requests";
    }
    httpd_resp_set_status(req, status_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

static void respond_ok(httpd_req_t *req)
{
    respond_json(req, 200, "{\"ok\":true}");
}

static char *httpd_read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > MAX_BODY_LEN) {
        return NULL;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) {
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, (size_t)(total - received));
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    return buf;
}

/* ---------- WiFi config API ---------- */

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
    if (!require_auth(req)) {
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
        respond_json(req, 400, "{\"error\":\"oom\"}");
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
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *body = httpd_read_body(req);
    if (!body) {
        respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        respond_json(req, 400, "{\"error\":\"bad json\"}");
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
            respond_json(req, 400, "{\"error\":\"ap_password too long\"}");
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
            respond_json(req, 400, "{\"error\":\"sta_password too long\"}");
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
        respond_json(req, 400, "{\"error\":\"ap_ssid empty\"}");
        return ESP_OK;
    }
    if (cfg.ap_password[0] != '\0' && strlen(cfg.ap_password) < 8) {
        cJSON_Delete(root);
        respond_json(req, 400, "{\"error\":\"ap_password needs >=8 chars or empty\"}");
        return ESP_OK;
    }
    if (!cfg.sta_dhcp && (cfg.sta_ip == 0 || cfg.sta_ip == 0xFFFFFFFFU)) {
        cJSON_Delete(root);
        respond_json(req, 400, "{\"error\":\"invalid static ip\"}");
        return ESP_OK;
    }
    (void)ip_str;

    esp_err_t err = wifi_web_config_save(&cfg);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        respond_json(req, 400, "{\"error\":\"nvs write failed\"}");
        return ESP_OK;
    }

    respond_json(req, 200, "{\"ok\":true,\"restart\":true}");
    schedule_restart();
    return ESP_OK;
}

/* ---------- handlers ---------- */

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

/* Build the status object as a JSON string (caller frees). */
static char *status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "mode", wifi_mode_str());
    char ip[16];
    bool ap_active = wifi_ap_active();
    cJSON_AddStringToObject(root, "ap_ip", ap_active && wifi_get_ap_ip(ip, sizeof(ip)) ? ip : "");
    cJSON_AddStringToObject(root, "sta_ip", wifi_get_sta_ip(ip, sizeof(ip)) ? ip : "");
    cJSON_AddStringToObject(root, "ap_ssid", ap_active ? wifi_ap_ssid() : "");
    cJSON_AddStringToObject(root, "sta_ssid", wifi_sta_ssid());
    cJSON_AddStringToObject(root, "sta_ip_mode", wifi_sta_ip_mode());
    cJSON_AddBoolToObject(root, "sta_connected", wifi_is_sta_connected());
    cJSON_AddNumberToObject(root, "carrier_hz", ir_get_carrier_freq());
    cJSON_AddBoolToObject(root, "rx_pause_on_play", ir_get_rx_pause_enabled());
    cJSON_AddBoolToObject(root, "playing", ir_is_playing());
    cJSON_AddNumberToObject(root, "history_count", ir_history_count());

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *s = status_json();
    if (!s) {
        respond_json(req, 400, "{\"error\":\"oom\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

/* Serialize one frame as a JSON object into buf; returns length, or -1. */
static int frame_to_json(const ir_frame_t *f, char *buf, size_t cap)
{
    int off = snprintf(buf, cap,
        "{\"seq\":%lu,\"ts\":%lu,"
        "\"nec\":{\"ok\":%s,\"repeat\":%s,\"ext\":%s,\"chksum\":%s,\"bits\":%u,"
        "\"addr\":%u,\"cmd\":%u,\"raw\":%lu,\"hxd\":\"%08lX\"},"
        "\"feat\":{\"total_us\":%lu,\"pulses\":%lu,\"min_pulse\":%lu,\"max_pulse\":%lu,"
        "\"leader_pulse\":%lu,\"leader_space\":%lu,\"last_gap\":%lu,\"seg_count\":%lu},"
        "\"freq\":%lu,\"durs\":[",
        (unsigned long)f->seq, (unsigned long)f->timestamp_ms,
        f->nec_ok ? "true" : "false", f->nec_repeat ? "true" : "false",
        f->nec_ext_addr ? "true" : "false", f->nec_chksum_ok ? "true" : "false",
        f->nec_bits, f->nec_addr, f->nec_cmd,
        (unsigned long)f->nec_raw, (unsigned long)f->nec_raw,
        (unsigned long)f->total_us, (unsigned long)f->pulse_count,
        (unsigned long)f->min_pulse_us, (unsigned long)f->max_pulse_us,
        (unsigned long)f->leader_pulse_us, (unsigned long)f->leader_space_us,
        (unsigned long)f->last_gap_us, (unsigned long)f->seg_count,
        (unsigned long)ir_get_carrier_freq());
    if (off < 0 || off >= (int)cap) {
        return -1;
    }
    for (uint32_t i = 0; i < f->raw_count; i++) {
        int n = snprintf(buf + off, cap - (size_t)off, "%s%lu",
                         i ? "," : "", (unsigned long)f->raw_durs[i]);
        if (n < 0 || off + n >= (int)cap) {
            break; /* truncated guard */
        }
        off += n;
    }
    if (off + 3 < (int)cap) {
        off += snprintf(buf + off, cap - (size_t)off, "]}");
    }
    return off;
}

/* One frame as a JSON object, streamed as a single chunk. */
static esp_err_t send_frame_chunk(httpd_req_t *req, const ir_frame_t *f)
{
    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_chunk(req, "{}", 2);
    }
    int off = frame_to_json(f, buf, 4096);
    if (off < 0) {
        free(buf);
        return httpd_resp_send_chunk(req, "{}", 2);
    }
    esp_err_t ret = httpd_resp_send_chunk(req, buf, (ssize_t)off);
    free(buf);
    return ret;
}

/* GET /api/frames?since=N -> {"last_seq":N,"frames":[...]} */
static esp_err_t frames_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    uint32_t since = 0;
    char query[32];
    int qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < (int)sizeof(query)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
                since = (uint32_t)strtoul(val, NULL, 10);
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    char head[64];
    snprintf(head, sizeof(head), "{\"last_seq\":%lu,\"frames\":[",
             (unsigned long)ir_get_latest_seq());
    httpd_resp_send_chunk(req, head, (ssize_t)strlen(head));

    uint32_t n = ir_history_count();
    bool first = true;
    for (uint32_t i = 0; i < n; i++) {
        ir_frame_t fr;
        if (ir_history_get(i, &fr) != ESP_OK) {
            continue;
        }
        if (fr.seq <= since) {
            continue;
        }
        if (!first) {
            httpd_resp_send_chunk(req, ",", 1);
        }
        first = false;
        send_frame_chunk(req, &fr);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0); /* terminate chunked response */
    return ESP_OK;
}

static esp_err_t play_by_seq(uint32_t seq, uint32_t freq)
{
    uint32_t n = ir_history_count();
    for (uint32_t i = 0; i < n; i++) {
        ir_frame_t fr;
        if (ir_history_get(i, &fr) == ESP_OK && fr.seq == seq) {
            return ir_play_history(i, freq);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* POST /api/play {"type":"hxd"|"raw"|"frame", ...} */
static esp_err_t play_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *body = httpd_read_body(req);
    if (!body) {
        respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        respond_json(req, 400, "{\"error\":\"bad json\"}");
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *freq_item = cJSON_GetObjectItem(root, "freq");
    uint32_t freq = 0;
    if (cJSON_IsNumber(freq_item)) {
        double fv = freq_item->valuedouble;
        /* 0 = use global carrier; otherwise must be in the valid range */
        if (fv != 0.0 && (fv < IR_CARRIER_FREQ_MIN || fv > IR_CARRIER_FREQ_MAX)) {
            cJSON_Delete(root);
            respond_json(req, 400, "{\"error\":\"bad freq\"}");
            return ESP_OK;
        }
        freq = (uint32_t)fv;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(type) && strcmp(type->valuestring, "hxd") == 0) {
        cJSON *v = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(v)) {
            ret = ir_play_hxd(v->valuestring, freq);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "raw") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsArray(data)) {
            int n = cJSON_GetArraySize(data);
            if (n <= 0) {
                ret = ESP_ERR_INVALID_ARG;
            } else {
                uint32_t *durs = malloc((size_t)n * sizeof(uint32_t));
                if (durs) {
                    bool good = true;
                    for (int i = 0; i < n; i++) {
                        cJSON *it = cJSON_GetArrayItem(data, i);
                        double dv = cJSON_IsNumber(it) ? it->valuedouble : -1.0;
                        if (dv <= 0.0 || dv > 65000.0) {
                            good = false;
                            break;
                        }
                        durs[i] = (uint32_t)dv;
                    }
                    if (good) {
                        ret = ir_play_raw(durs, (uint32_t)n, freq);
                    } else {
                        ret = ESP_ERR_INVALID_ARG;
                    }
                    free(durs);
                } else {
                    ret = ESP_ERR_NO_MEM;
                }
            }
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "frame") == 0) {
        cJSON *s = cJSON_GetObjectItem(root, "seq");
        if (cJSON_IsNumber(s)) {
            ret = play_by_seq((uint32_t)s->valuedouble, freq);
        }
    }

    cJSON_Delete(root);

    if (ret == ESP_OK) {
        respond_ok(req);
    } else {
        respond_json(req, 400, "{\"error\":\"playback failed\"}");
    }
    return ESP_OK;
}

/* POST /api/carrier {"freq":38000} */
static esp_err_t carrier_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *body = httpd_read_body(req);
    if (!body) {
        respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        respond_json(req, 400, "{\"error\":\"bad json\"}");
        return ESP_OK;
    }
    cJSON *freq_item = cJSON_GetObjectItem(root, "freq");
    if (!cJSON_IsNumber(freq_item)) {
        cJSON_Delete(root);
        respond_json(req, 400, "{\"error\":\"missing freq\"}");
        return ESP_OK;
    }
    double fv = freq_item->valuedouble;
    if (fv < IR_CARRIER_FREQ_MIN || fv > IR_CARRIER_FREQ_MAX) {
        cJSON_Delete(root);
        respond_json(req, 400, "{\"error\":\"invalid freq\"}");
        return ESP_OK;
    }
    uint32_t freq = (uint32_t)fv;
    esp_err_t ret = ir_set_carrier_freq(freq);
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"freq\":%lu}", (unsigned long)ir_get_carrier_freq());
        respond_json(req, 200, buf);
    } else {
        respond_json(req, 400, "{\"error\":\"invalid freq\"}");
    }
    return ESP_OK;
}

/* POST /api/rxpause {"enabled":true|false} */
static esp_err_t rx_pause_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *body = httpd_read_body(req);
    if (!body) {
        respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        respond_json(req, 400, "{\"error\":\"bad json\"}");
        return ESP_OK;
    }
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(en)) {
        cJSON_Delete(root);
        respond_json(req, 400, "{\"error\":\"missing enabled\"}");
        return ESP_OK;
    }
    ir_set_rx_pause_enabled(cJSON_IsTrue(en));
    cJSON_Delete(root);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"rx_pause_on_play\":%s}",
             ir_get_rx_pause_enabled() ? "true" : "false");
    respond_json(req, 200, buf);
    return ESP_OK;
}

/* GET /api/authcfg -> current login user (password is never returned) */
static esp_err_t authcfg_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    web_auth_cfg_t cfg;
    web_auth_load(&cfg);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"user\":\"%s\"}", cfg.user);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/authcfg {"user":"...","pass":"..."} — pass empty = keep current */
static esp_err_t authcfg_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *body = httpd_read_body(req);
    if (!body) {
        respond_json(req, 400, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        respond_json(req, 400, "{\"error\":\"bad json\"}");
        return ESP_OK;
    }

    web_auth_cfg_t cfg;
    web_auth_load(&cfg);

    cJSON *j = cJSON_GetObjectItem(root, "user");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        strlcpy(cfg.user, j->valuestring, sizeof(cfg.user));
    }
    j = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        if (strlen(j->valuestring) < 4 || strlen(j->valuestring) >= sizeof(cfg.pass)) {
            cJSON_Delete(root);
            respond_json(req, 400, "{\"error\":\"pass must be 4-63 chars\"}");
            return ESP_OK;
        }
        strlcpy(cfg.pass, j->valuestring, sizeof(cfg.pass));
    }
    cJSON_Delete(root);

    esp_err_t err = web_auth_save(&cfg);
    if (err != ESP_OK) {
        respond_json(req, 400, "{\"error\":\"nvs write failed\"}");
        return ESP_OK;
    }
    /* credentials changed: force a fresh login */
    s_token[0] = '\0';
    respond_ok(req);
    return ESP_OK;
}

/* POST /api/logout - invalidate the current session token. */
static esp_err_t logout_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    s_token[0] = '\0';
    s_token_ts = 0;
    respond_ok(req);
    return ESP_OK;
}

/* POST /api/renew - extend the current session for another full TTL. */
static esp_err_t renew_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    s_token_ts = esp_timer_get_time();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"expires_in\":%lld}",
             (long long)(TOKEN_TTL_US / 1000000));
    respond_json(req, 200, buf);
    return ESP_OK;
}

/* ---------- WebSocket push (status every 1s + new IR frames) ---------- */
#if CONFIG_HTTPD_WS_SUPPORT

#define WS_MAX_CLIENTS 6

typedef struct {
    int fd;
    bool authed;
} ws_client_t;

static ws_client_t s_ws_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

static void ws_client_remove(int fd)
{
    if (!s_ws_mutex || fd < 0) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            s_ws_clients[i].fd = -1;
            s_ws_clients[i].authed = false;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void ws_client_add(int fd, bool authed)
{
    if (!s_ws_mutex || fd < 0) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            s_ws_clients[i].authed = authed;
            xSemaphoreGive(s_ws_mutex);
            return;
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd < 0) {
            s_ws_clients[i].fd = fd;
            s_ws_clients[i].authed = authed;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static bool ws_has_authed_clients(void)
{
    bool has = false;
    if (!s_ws_mutex) {
        return false;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd >= 0 && s_ws_clients[i].authed) {
            has = true;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    return has;
}

static void ws_send_text_all(const char *json)
{
    if (!s_server || !s_ws_mutex) {
        return;
    }
    int dead[WS_MAX_CLIENTS];
    int ndead = 0;
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd < 0 || !s_ws_clients[i].authed) {
            continue;
        }
        esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_clients[i].fd, &frame);
        if (err != ESP_OK && ndead < WS_MAX_CLIENTS) {
            dead[ndead++] = s_ws_clients[i].fd;
            ESP_LOGD(TAG, "ws: async send to fd %d failed: %s", s_ws_clients[i].fd, esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_ws_mutex);
    for (int i = 0; i < ndead; i++) {
        ws_client_remove(dead[i]);
    }
}

static esp_err_t ws_reply_text(httpd_req_t *req, const char *json)
{
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    return httpd_ws_send_frame(req, &frame);
}

static char *ws_status_wrapped(void)
{
    char *inner = status_json();
    if (!inner) {
        return NULL;
    }
    size_t cap = strlen(inner) + 64;
    char *buf = malloc(cap);
    if (!buf) {
        free(inner);
        return NULL;
    }
    snprintf(buf, cap, "{\"type\":\"status\",\"data\":%s}", inner);
    free(inner);
    return buf;
}

static void ws_status_timer_cb(void *arg)
{
    (void)arg;
    if (!ws_has_authed_clients()) {
        return;
    }
    char *s = ws_status_wrapped();
    if (!s) {
        return;
    }
    ws_send_text_all(s);
    free(s);
}

static void ws_frame_push_cb(const ir_frame_t *fr, void *arg)
{
    (void)arg;
    if (!ws_has_authed_clients()) {
        return;
    }
    size_t cap = 4096 + 64;
    char *buf = malloc(cap);
    if (!buf) {
        return;
    }
    int off = snprintf(buf, cap, "{\"type\":\"frame\",\"data\":");
    if (off < 0 || off >= (int)cap) {
        free(buf);
        return;
    }
    int n = frame_to_json(fr, buf + off, cap - (size_t)off);
    if (n < 0) {
        free(buf);
        return;
    }
    off += n;
    if (off + 2 < (int)cap) {
        off += snprintf(buf + off, cap - (size_t)off, "}");
    }
    ws_send_text_all(buf);
    free(buf);
}

static void ws_session_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ws_client_remove(sockfd);
    /* With a custom close_fn set, esp_http_server does NOT close the socket
     * itself; we must close it here or every closed session leaks an lwIP
     * socket (eventually accept() fails with ENFILE / errno 23). */
    close(sockfd);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t frame = {0};

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > 1024) {
        ESP_LOGW(TAG, "ws: oversized frame (%u bytes) from fd %d", (unsigned)frame.len, fd);
        return ESP_FAIL;
    }

    char *payload = malloc(frame.len + 1);
    if (!payload) {
        return ESP_OK;
    }
    frame.payload = (uint8_t *)payload;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(payload);
        return ESP_FAIL;
    }
    payload[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_PONG,
            .payload = (uint8_t *)payload,
            .len = frame.len,
        };
        err = httpd_ws_send_frame(req, &pong);
        free(payload);
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        httpd_ws_frame_t close = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_CLOSE,
            .payload = (uint8_t *)payload,
            .len = frame.len,
        };
        httpd_ws_send_frame(req, &close);
        free(payload);
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        free(payload);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(payload);
    free(payload);
    if (!root) {
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "auth") == 0) {
        cJSON *tok = cJSON_GetObjectItem(root, "token");
        if (cJSON_IsString(tok) && web_auth_token_ok(tok->valuestring)) {
            ws_client_add(fd, true);
            ESP_LOGI(TAG, "ws: client fd %d authenticated", fd);
            ws_reply_text(req, "{\"type\":\"auth\",\"ok\":true}");
            char *s = ws_status_wrapped();
            if (s) {
                ws_reply_text(req, s);
                free(s);
            }
        } else {
            ESP_LOGW(TAG, "ws: auth failed for fd %d", fd);
            ws_reply_text(req, "{\"type\":\"auth\",\"ok\":false,\"error\":\"unauthorized\"}");
            cJSON_Delete(root);
            return ESP_FAIL; /* close the socket */
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

#endif /* CONFIG_HTTPD_WS_SUPPORT */

esp_err_t web_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 20; /* registered handlers + headroom */
#if CONFIG_HTTPD_WS_SUPPORT
    cfg.close_fn = ws_session_close;
#endif

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", HTTP_PORT);
        return ESP_FAIL;
    }

#if CONFIG_HTTPD_WS_SUPPORT
    s_ws_mutex = xSemaphoreCreateMutex();
    if (s_ws_mutex) {
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            s_ws_clients[i].fd = -1;
        }
        const esp_timer_create_args_t targs = {
            .callback = ws_status_timer_cb,
            .name = "ws_status",
        };
        esp_timer_handle_t status_timer = NULL;
        if (esp_timer_create(&targs, &status_timer) == ESP_OK) {
            esp_timer_start_periodic(status_timer, 1000 * 1000);
        }
    }
    ir_set_frame_cb(ws_frame_push_cb, NULL);
#endif

    static const httpd_uri_t uris[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/index.html",   .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/api/login",    .method = HTTP_POST, .handler = login_handler},
        {.uri = "/api/status",   .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/api/frames",   .method = HTTP_GET,  .handler = frames_handler},
        {.uri = "/api/play",     .method = HTTP_POST, .handler = play_handler},
        {.uri = "/api/carrier",  .method = HTTP_POST, .handler = carrier_handler},
        {.uri = "/api/rxpause",  .method = HTTP_POST, .handler = rx_pause_handler},
        {.uri = "/api/wificfg",  .method = HTTP_GET,  .handler = wificfg_get_handler},
        {.uri = "/api/wificfg",  .method = HTTP_POST, .handler = wificfg_post_handler},
        {.uri = "/api/authcfg",  .method = HTTP_GET,  .handler = authcfg_get_handler},
        {.uri = "/api/authcfg",  .method = HTTP_POST, .handler = authcfg_post_handler},
        {.uri = "/api/logout",   .method = HTTP_POST, .handler = logout_handler},
        {.uri = "/api/renew",    .method = HTTP_POST, .handler = renew_handler},
#if CONFIG_HTTPD_WS_SUPPORT
        {.uri = "/api/ws",       .method = HTTP_GET,  .handler = ws_handler,
         .is_websocket = true, .handle_ws_control_frames = true},
#endif
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
            return ESP_FAIL;
        }
    }

    char ip[16];
    if (wifi_is_sta_connected() ? wifi_get_sta_ip(ip, sizeof(ip)) : wifi_get_ap_ip(ip, sizeof(ip))) {
        ESP_LOGI(TAG, "Web UI ready: http://%s:%d/", ip, HTTP_PORT);
    } else {
        ESP_LOGI(TAG, "Web UI ready: port %d", HTTP_PORT);
    }
    return ESP_OK;
}
