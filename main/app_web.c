#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_web.h"
#include "app_ir.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "cJSON.h"

#define TAG "web"

#define HTTP_PORT        CONFIG_IR_TOOL_HTTP_PORT
#define MAX_BODY_LEN     32768

/* Embedded web UI (main/web/index.html via EMBED_TXTFILES) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;

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

/* ---------- handlers ---------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
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
    cJSON_AddBoolToObject(root, "sta_connected", wifi_is_sta_connected());
    cJSON_AddNumberToObject(root, "carrier_hz", ir_get_carrier_freq());
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
        freq = (uint32_t)freq_item->valuedouble;
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
            uint32_t *durs = malloc((size_t)n * sizeof(uint32_t));
            if (durs) {
                for (int i = 0; i < n; i++) {
                    cJSON *it = cJSON_GetArrayItem(data, i);
                    durs[i] = cJSON_IsNumber(it) ? (uint32_t)it->valuedouble : 0;
                }
                ret = ir_play_raw(durs, (uint32_t)n, freq);
                free(durs);
            } else {
                ret = ESP_ERR_NO_MEM;
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
    uint32_t freq = (uint32_t)freq_item->valuedouble;
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

esp_err_t web_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", HTTP_PORT);
        return ESP_FAIL;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/api/status",   .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/api/frames",   .method = HTTP_GET,  .handler = frames_handler},
        {.uri = "/api/play",     .method = HTTP_POST, .handler = play_handler},
        {.uri = "/api/carrier",  .method = HTTP_POST, .handler = carrier_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Web UI ready: http://<ip>:%d/", HTTP_PORT);
    return ESP_OK;
}
