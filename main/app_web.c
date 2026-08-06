#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
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

typedef struct {
    char user[33];
    char pass[65];
} web_auth_cfg_t;

static char s_token[AUTH_TOKEN_LEN + 1] = ""; /* single active session token */

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

/* Verify the X-Auth-Token header against the active session token. */
static bool web_auth_ok(httpd_req_t *req)
{
    if (s_token[0] == '\0') {
        ESP_LOGW(TAG, "auth: no token issued (login first)");
        return false;
    }
    size_t len = httpd_req_get_hdr_value_len(req, "X-Auth-Token");
    if (len == 0) {
        char ua[64] = "";
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
    if (strcmp(buf, s_token) != 0) {
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
              strcmp(u->valuestring, cfg.user) == 0 &&
              strcmp(p->valuestring, cfg.pass) == 0;
    cJSON_Delete(root);
    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"bad credentials\"}");
        return ESP_OK;
    }
    /* reuse the existing session token if one is active, so multiple tabs
     * / devices sharing the same credentials do not kick each other out */
    if (s_token[0] == '\0') {
        token_new();
    }
    ESP_LOGI(TAG, "login ok, token issued");
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"token\":\"%s\"}", s_token);
    respond_json(req, 200, buf);
    return ESP_OK;
}

/* Embedded web UI (main/web/index.html via EMBED_TXTFILES) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ---------- helpers ---------- */

static void respond_json(httpd_req_t *req, int status, const char *json)
{
    httpd_resp_set_status(req, status == 200 ? "200 OK" : "400 Bad Request");
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
        "{\"ap_ssid\":\"%s\",\"ap_password\":\"%s\","
        "\"sta_ssid\":\"%s\",\"sta_password\":\"%s\","
        "\"sta_dhcp\":%s,\"sta_ip\":\"%s\",\"sta_gw\":\"%s\","
        "\"sta_mask\":\"%s\",\"sta_dns\":\"%s\"}",
        cfg.ap_ssid, cfg.ap_password, cfg.sta_ssid, cfg.sta_password,
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
        strlcpy(cfg.ap_password, j->valuestring, sizeof(cfg.ap_password));
    }
    j = cJSON_GetObjectItem(root, "sta_ssid");
    if (cJSON_IsString(j)) {
        strlcpy(cfg.sta_ssid, j->valuestring, sizeof(cfg.sta_ssid));
    }
    j = cJSON_GetObjectItem(root, "sta_password");
    if (cJSON_IsString(j)) {
        strlcpy(cfg.sta_password, j->valuestring, sizeof(cfg.sta_password));
    }
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

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        respond_json(req, 400, "{\"error\":\"oom\"}");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "mode", wifi_mode_str());
    char ip[16];
    cJSON_AddStringToObject(root, "ap_ip", wifi_get_ap_ip(ip, sizeof(ip)) ? ip : "");
    cJSON_AddStringToObject(root, "sta_ip", wifi_get_sta_ip(ip, sizeof(ip)) ? ip : "");
    cJSON_AddStringToObject(root, "ap_ssid", wifi_ap_ssid());
    cJSON_AddStringToObject(root, "sta_ssid", wifi_sta_ssid());
    cJSON_AddStringToObject(root, "sta_ip_mode", wifi_sta_ip_mode());
    cJSON_AddBoolToObject(root, "sta_connected", wifi_is_sta_connected());
    cJSON_AddNumberToObject(root, "carrier_hz", ir_get_carrier_freq());
    cJSON_AddBoolToObject(root, "rx_pause_on_play", ir_get_rx_pause_enabled());
    cJSON_AddBoolToObject(root, "playing", ir_is_playing());
    cJSON_AddNumberToObject(root, "history_count", ir_history_count());

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        respond_json(req, 400, "{\"error\":\"oom\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

/* One frame as a JSON object, streamed as a single chunk. */
static esp_err_t send_frame_chunk(httpd_req_t *req, const ir_frame_t *f)
{
    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_chunk(req, "{}", 2);
    }
    int off = snprintf(buf, 4096,
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
    if (off < 0 || off >= 4096) {
        free(buf);
        return httpd_resp_send_chunk(req, "{}", 2);
    }
    for (uint32_t i = 0; i < f->raw_count; i++) {
        int n = snprintf(buf + off, 4096 - (size_t)off, "%s%lu",
                         i ? "," : "", (unsigned long)f->raw_durs[i]);
        if (n < 0 || off + n >= 4096) {
            break; /* truncated guard */
        }
        off += n;
    }
    if (off + 3 < 4096) {
        off += snprintf(buf + off, 4096 - (size_t)off, "]}");
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

esp_err_t web_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16; /* 11 registered handlers + headroom */

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", HTTP_PORT);
        return ESP_FAIL;
    }

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
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Web UI ready: http://192.168.4.1:%d/", HTTP_PORT);
    return ESP_OK;
}
