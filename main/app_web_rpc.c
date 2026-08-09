#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_web_internal.h"
#include "app_ir.h"
#include "app_wifi.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG "web"

/* Upper bound for one WS frames-backfill response. REST /api/frames keeps the
 * streaming path for the pathological >48KB case; a WS client that hits this
 * truncation should fall back to REST to resync. */
#define RPC_FRAMES_MAX_BYTES (48 * 1024)

/* single frame JSON buffer, sized for IR_RAW_MAX_SEGS durations */
#define FRAME_JSON_CAP 16384

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/* Serialize one frame to a heap JSON string (caller frees). */
static char *frame_json(const ir_frame_t *f)
{
    char *buf = malloc(FRAME_JSON_CAP);
    if (!buf) {
        return NULL;
    }
    if (web_frame_to_json(f, buf, FRAME_JSON_CAP) < 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* simple growable string buffer */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
} sbuf_t;

static bool sbuf_reserve(sbuf_t *b, size_t add)
{
    if (b->len + add + 1 <= b->cap) {
        return true;
    }
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < b->len + add + 1) {
        ncap *= 2;
    }
    char *np = realloc(b->p, ncap);
    if (!np) {
        return false;
    }
    b->p = np;
    b->cap = ncap;
    return true;
}

static bool sbuf_append_len(sbuf_t *b, const char *s, size_t n)
{
    if (!sbuf_reserve(b, n)) {
        return false;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return true;
}

static bool sbuf_append(sbuf_t *b, const char *s)
{
    return sbuf_append_len(b, s, strlen(s));
}

/* Incremental frame history as a single JSON object. Bounded output; on
 * truncation "truncated":true is embedded and last_seq reflects the last frame
 * actually included so the client never skips frames it does not have. */
char *web_rpc_frames(uint32_t since)
{
    sbuf_t b = {0};
    uint32_t n = ir_history_count();
    bool first = true;
    bool truncated = false;
    uint32_t last_known = since;

    for (uint32_t i = 0; i < n; i++) {
        ir_frame_t fr;
        if (ir_history_get(i, &fr) != ESP_OK) {
            continue;
        }
        if (fr.seq <= since) {
            continue;
        }
        char *one = frame_json(&fr);
        if (!one) {
            truncated = true;
            break;
        }
        size_t onelen = strlen(one);
        if (b.len + onelen + (first ? 0 : 1) + 8 > RPC_FRAMES_MAX_BYTES) {
            free(one);
            truncated = true;
            break;
        }
        if (!first && !sbuf_append(&b, ",")) {
            free(one);
            truncated = true;
            break;
        }
        first = false;
        if (!sbuf_append(&b, one)) {
            free(one);
            truncated = true;
            break;
        }
        last_known = fr.seq;
        free(one);
    }

    char *out = malloc(b.len + 96);
    if (!out) {
        free(b.p);
        return NULL;
    }
    int off = snprintf(out, 96, "{\"last_seq\":%lu,\"frames\":[",
                       (unsigned long)last_known);
    if (b.len) {
        memcpy(out + off, b.p, b.len);
        off += (int)b.len;
    }
    off += snprintf(out + off, 48, truncated ? "],\"truncated\":true}" : "]}");
    (void)off;
    free(b.p);
    return out;
}

/* Execute one command and return the canonical response JSON (without the
 * outer WS envelope). On failure returns NULL and sets *err to a short message
 * safe to embed in a JSON string. */
char *web_rpc_exec(const char *cmd, cJSON *body, const char **err)
{
    if (!cmd) {
        *err = "missing cmd";
        return NULL;
    }

    if (strcmp(cmd, "status") == 0) {
        return web_status_json();
    }

    if (strcmp(cmd, "frames") == 0) {
        uint32_t since = 0;
        cJSON *s = cJSON_GetObjectItem(body, "since");
        if (cJSON_IsNumber(s)) {
            since = (uint32_t)s->valuedouble;
        }
        return web_rpc_frames(since);
    }

    if (strcmp(cmd, "play") == 0) {
        esp_err_t r = web_ir_play_exec(body);
        if (r != ESP_OK) {
            *err = "playback failed";
            return NULL;
        }
        return xstrdup("{}");
    }

    if (strcmp(cmd, "carrier") == 0) {
        uint32_t freq = 0;
        esp_err_t r = web_ir_carrier_exec(body, &freq);
        if (r == ESP_ERR_NOT_FOUND) {
            *err = "missing freq";
            return NULL;
        }
        if (r != ESP_OK) {
            *err = "invalid freq";
            return NULL;
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"freq\":%lu}", (unsigned long)freq);
        return xstrdup(buf);
    }

    if (strcmp(cmd, "rxpause") == 0) {
        bool enabled = false;
        esp_err_t r = web_ir_rxpause_exec(body, &enabled);
        if (r != ESP_OK) {
            *err = "missing enabled";
            return NULL;
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"rx_pause_on_play\":%s}",
                 enabled ? "true" : "false");
        return xstrdup(buf);
    }

    if (strcmp(cmd, "wificfg") == 0) {
        /* present config fields => set; otherwise => get */
        bool has_set =
            cJSON_GetObjectItem(body, "ap_ssid") ||
            cJSON_GetObjectItem(body, "ap_password") ||
            cJSON_GetObjectItem(body, "sta_ssid") ||
            cJSON_GetObjectItem(body, "sta_password") ||
            cJSON_GetObjectItem(body, "sta_dhcp") ||
            cJSON_GetObjectItem(body, "sta_ip") ||
            cJSON_GetObjectItem(body, "sta_gw") ||
            cJSON_GetObjectItem(body, "sta_mask") ||
            cJSON_GetObjectItem(body, "sta_dns");
        if (has_set) {
            const char *e = NULL;
            if (web_wificfg_set(body, &e) != ESP_OK) {
                *err = e ? e : "invalid";
                return NULL;
            }
            return xstrdup("{\"restart\":true}");
        }
        return web_wificfg_get_json();
    }

    if (strcmp(cmd, "authcfg") == 0) {
        /* present user/pass/single_session fields => set; otherwise => get.
         * The set reply carries "invalidated":true when the session was
         * invalidated (credentials actually changed), so the client only
         * forces a re-login when the server really did. */
        if (cJSON_GetObjectItem(body, "user") || cJSON_GetObjectItem(body, "pass") ||
            cJSON_GetObjectItem(body, "single_session")) {
            const char *e = NULL;
            bool invalidated = false;
            if (web_authcfg_set(body, &invalidated, &e) != ESP_OK) {
                *err = e ? e : "invalid";
                return NULL;
            }
            return xstrdup(invalidated ? "{\"invalidated\":true}"
                                       : "{\"invalidated\":false}");
        }
        return web_authcfg_get_json();
    }

    if (strcmp(cmd, "renew") == 0) {
        uint32_t expires_in = 0;
        if (web_auth_renew(&expires_in) != ESP_OK) {
            *err = "unauthorized";
            return NULL;
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"expires_in\":%lu}", (unsigned long)expires_in);
        return xstrdup(buf);
    }

    if (strcmp(cmd, "logout") == 0) {
        web_auth_invalidate();
        return xstrdup("{}");
    }

    if (strcmp(cmd, "webcfg") == 0) {
        /* present web_ui => set; otherwise => get */
        cJSON *j = cJSON_GetObjectItem(body, "web_ui");
        if (cJSON_IsBool(j)) {
            const char *e = NULL;
            if (web_ui_enabled_set(cJSON_IsTrue(j), &e) != ESP_OK) {
                *err = e ? e : "invalid";
                return NULL;
            }
            return xstrdup("{\"restart\":true}");
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"web_ui\":%s}",
                 web_ui_enabled_get() ? "true" : "false");
        return xstrdup(buf);
    }

    *err = "unknown cmd";
    return NULL;
}
