#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_web_internal.h"
#include "app_ir.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG "web"

void web_respond_json(httpd_req_t *req, int status, const char *json)
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

void web_respond_ok(httpd_req_t *req)
{
    web_respond_json(req, 200, "{\"ok\":true}");
}

char *web_httpd_read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > WEB_MAX_BODY_LEN) {
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

/* Build the status object as a JSON string (caller frees). */
char *web_status_json(void)
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

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* Serialize one frame as a JSON object into buf; returns length, or -1. */
int web_frame_to_json(const ir_frame_t *f, char *buf, size_t cap)
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
