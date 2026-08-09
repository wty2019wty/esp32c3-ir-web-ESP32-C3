#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_web_internal.h"
#include "app_ir.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG "web"

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *s = web_status_json();
    if (!s) {
        web_respond_json(req, 400, "{\"error\":\"oom\"}");
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
    char *buf = malloc(16384); /* enough for IR_RAW_MAX_SEGS (1024) durations */
    if (!buf) {
        return httpd_resp_send_chunk(req, "{}", 2);
    }
    int off = web_frame_to_json(f, buf, 16384);
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
    if (!web_require_auth(req)) {
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

/* Shared command core for playback. Body: {"type","freq?",value/data/seq}. */
esp_err_t web_ir_play_exec(cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *freq_item = cJSON_GetObjectItem(root, "freq");
    uint32_t freq = 0;
    if (cJSON_IsNumber(freq_item)) {
        double fv = freq_item->valuedouble;
        /* 0 = use global carrier; otherwise must be in the valid range */
        if (fv != 0.0 && (fv < IR_CARRIER_FREQ_MIN || fv > IR_CARRIER_FREQ_MAX)) {
            return ESP_ERR_INVALID_ARG;
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
    return ret;
}

/* POST /api/play {"type":"hxd"|"raw"|"frame", ...} */
static esp_err_t play_handler(httpd_req_t *req)
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

    esp_err_t ret = web_ir_play_exec(root);
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        web_respond_ok(req);
    } else {
        web_respond_json(req, 400, "{\"error\":\"playback failed\"}");
    }
    return ESP_OK;
}

/* Shared command core for carrier. Body: {"freq":38000}. */
esp_err_t web_ir_carrier_exec(cJSON *root, uint32_t *freq_out)
{
    cJSON *freq_item = cJSON_GetObjectItem(root, "freq");
    if (!cJSON_IsNumber(freq_item)) {
        return ESP_ERR_NOT_FOUND;
    }
    double fv = freq_item->valuedouble;
    if (fv < IR_CARRIER_FREQ_MIN || fv > IR_CARRIER_FREQ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t freq = (uint32_t)fv;
    esp_err_t ret = ir_set_carrier_freq(freq);
    if (ret == ESP_OK && freq_out) {
        *freq_out = ir_get_carrier_freq();
    }
    return ret;
}

/* POST /api/carrier {"freq":38000} */
static esp_err_t carrier_handler(httpd_req_t *req)
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
    uint32_t freq = 0;
    esp_err_t ret = web_ir_carrier_exec(root, &freq);
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"freq\":%lu}", (unsigned long)freq);
        web_respond_json(req, 200, buf);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        web_respond_json(req, 400, "{\"error\":\"missing freq\"}");
    } else {
        web_respond_json(req, 400, "{\"error\":\"invalid freq\"}");
    }
    return ESP_OK;
}

/* Shared command core for RX pause. Body: {"enabled":true|false}. */
esp_err_t web_ir_rxpause_exec(cJSON *root, bool *enabled_out)
{
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(en)) {
        return ESP_ERR_NOT_FOUND;
    }
    ir_set_rx_pause_enabled(cJSON_IsTrue(en));
    if (enabled_out) {
        *enabled_out = ir_get_rx_pause_enabled();
    }
    return ESP_OK;
}

/* POST /api/rxpause {"enabled":true|false} */
static esp_err_t rx_pause_handler(httpd_req_t *req)
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
    bool enabled = false;
    esp_err_t ret = web_ir_rxpause_exec(root, &enabled);
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"rx_pause_on_play\":%s}",
                 enabled ? "true" : "false");
        web_respond_json(req, 200, buf);
    } else {
        web_respond_json(req, 400, "{\"error\":\"missing enabled\"}");
    }
    return ESP_OK;
}

esp_err_t web_api_ir_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/status",  .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/api/frames",  .method = HTTP_GET,  .handler = frames_handler},
        {.uri = "/api/play",    .method = HTTP_POST, .handler = play_handler},
        {.uri = "/api/carrier", .method = HTTP_POST, .handler = carrier_handler},
        {.uri = "/api/rxpause", .method = HTTP_POST, .handler = rx_pause_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}
