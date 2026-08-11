#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app_web_internal.h"
#include "app_ir.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define TAG "web"

#if CONFIG_HTTPD_WS_SUPPORT

/* The /api/ws Origin allow-list can only be enforced before the 101 upgrade,
 * where the handshake headers are still readable. Without the pre-handshake
 * callback the check would silently compile away (the per-frame ws_handler
 * runs after the upgrade with no headers), turning the whitelist into a no-op
 * while the settings page still advertises it. Fail the build instead. */
#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT must be enabled: the /api/ws Origin allow-list cannot be enforced without it"
#endif

#define WS_MAX_CLIENTS 6

/* Idle connections get reaped by consumer-router NAT timeouts, which shows up
 * as ECONNRESET (104) on the next send. Push a status heartbeat this often even
 * when nothing changed so the path stays warm (choose a value comfortably below
 * typical home-router NAT idle timeouts). */
#define WS_HEARTBEAT_INTERVAL_US (20LL * 1000000)

typedef struct {
    int fd;
    bool authed;
    uint32_t gen;        /* session generation the client authenticated against */
    uint32_t acked_id;   /* last status id confirmed by this client (0 = none) */
    int pending;         /* queued async sends, for back-pressure */
    uint32_t seq;        /* bumped each time the slot is (re)assigned to a client */
} ws_client_t;

static httpd_handle_t s_ws_server = NULL;
static ws_client_t s_ws_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;
static uint32_t s_status_id = 0;    /* bumped on every status change */
static char *s_status_inner = NULL; /* cached status payload (data part) */
static int64_t s_last_forced_push_us = 0; /* last heartbeat/forced status push (us) */

/* Reset every client slot matching fd. Caller must hold s_ws_mutex. */
static void ws_client_reset_locked(int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            s_ws_clients[i].fd = -1;
            s_ws_clients[i].authed = false;
            s_ws_clients[i].pending = 0;
        }
    }
}

static void ws_client_remove(int fd)
{
    if (!s_ws_mutex || fd < 0) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    ws_client_reset_locked(fd); /* drop any stale back-pressure */
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
            s_ws_clients[i].gen = authed ? web_auth_get_gen() : 0;
            s_ws_clients[i].acked_id = 0;
            s_ws_clients[i].pending = 0;
            if (++s_ws_clients[i].seq == 0) {
                s_ws_clients[i].seq = 1;
            }
            xSemaphoreGive(s_ws_mutex);
            return;
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd < 0) {
            s_ws_clients[i].fd = fd;
            s_ws_clients[i].authed = authed;
            s_ws_clients[i].gen = authed ? web_auth_get_gen() : 0;
            s_ws_clients[i].acked_id = 0;
            s_ws_clients[i].pending = 0; /* slot reuse must not inherit stale counts */
            if (++s_ws_clients[i].seq == 0) {
                s_ws_clients[i].seq = 1;
            }
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

/* True when the client authenticated against the current session generation AND
 * the session is still live (token not expired). The TTL check matters: without
 * it a connection authenticated before the 24h expiry would keep issuing commands
 * indefinitely, contradicting the documented session lifetime. */
static bool ws_client_gen_ok(int fd)
{
    if (!web_auth_session_live()) {
        return false; /* session expired/invalidated: reject every client */
    }
    uint32_t cur = web_auth_get_gen();
    bool ok = false;
    if (!s_ws_mutex || fd < 0) {
        return false;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd && s_ws_clients[i].authed &&
            s_ws_clients[i].gen == cur) {
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    return ok;
}

static void ws_client_ack(int fd, uint32_t id)
{
    if (!s_ws_mutex || fd < 0) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            s_ws_clients[i].acked_id = id;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static bool ws_has_authed_clients(void)
{
    if (!s_ws_mutex || !web_auth_session_live()) {
        return false; /* no live session: nothing to push to anyone */
    }
    bool has = false;
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

/* ---- async push: serialize ALL sends through the httpd task --------------
 * httpd_ws_send_frame_async() writes to the socket from the *caller's* task
 * while the httpd task concurrently writes command replies/pongs to the same
 * fd -> interleaved bytes -> "Invalid frame header" on the client. Instead we
 * queue the frame via httpd_ws_send_data_async() so every write happens in the
 * httpd task context. The payload is copied here and freed in the callback. */

#define WS_MAX_PENDING 4   /* queued frames per client; extra pushes are dropped */

typedef struct {
    int fd;
    int slot;        /* client slot the send was queued for */
    uint32_t seq;    /* slot generation at queue time (see ws_client_t.seq) */
    char *payload;
} ws_async_msg_t;

static void ws_async_send_done(esp_err_t err, int socket, void *arg)
{
    ws_async_msg_t *m = arg;
    /* Runs in the httpd task. The slot + seq recorded at queue time only match
     * the CURRENT occupant when it is the same client that queued the send. A
     * freed slot (fd == -1) or a recycled fd that was re-assigned to a new
     * client (seq bumped in ws_client_add) no longer matches, so a stale
     * callback can never evict or decrement a stranger. The ownership check and
     * the mutation both run under s_ws_mutex so they stay atomic. */
    if (m && m->slot >= 0 && m->slot < WS_MAX_CLIENTS &&
        s_ws_mutex && xSemaphoreTake(s_ws_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_ws_clients[m->slot].fd == m->fd &&
            s_ws_clients[m->slot].seq == m->seq) {
            if (err != ESP_OK) {
                ws_client_reset_locked(m->fd);
            } else if (s_ws_clients[m->slot].pending > 0) {
                s_ws_clients[m->slot].pending--;
            }
        }
        xSemaphoreGive(s_ws_mutex);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "ws: async send to fd %d failed: %s", socket, esp_err_to_name(err));
    }
    if (m) {
        free(m->payload);
        free(m);
    }
}

/* Caller must hold s_ws_mutex. Copies the text and queues the send. */
static void ws_async_send_text_locked(int fd, const char *json)
{
    if (!s_ws_server || fd < 0) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd && s_ws_clients[i].pending >= WS_MAX_PENDING) {
            ESP_LOGD(TAG, "ws: dropping push to fd %d (queue full)", fd);
            return; /* back-pressure: skip this push, client resyncs via frames cmd */
        }
    }
    size_t len = strlen(json);
    char *dup = malloc(len + 1);
    if (!dup) {
        return;
    }
    memcpy(dup, json, len + 1);
    ws_async_msg_t *m = malloc(sizeof(*m));
    if (!m) {
        free(dup);
        return;
    }
    m->fd = fd;
    m->slot = -1;
    m->seq = 0;
    m->payload = dup;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            m->slot = i;
            m->seq = s_ws_clients[i].seq;
            s_ws_clients[i].pending++;
            break;
        }
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)dup,
        .len = len,
    };
    esp_err_t err = httpd_ws_send_data_async(s_ws_server, fd, &frame, ws_async_send_done, m);
    if (err != ESP_OK) {
        /* Send never got queued, so the done-callback will not fire. Clean up
         * inline. We hold s_ws_mutex here, so do NOT call ws_async_send_done()
         * (its error path takes the mutex -> deadlock on a non-recursive mutex). */
        ESP_LOGD(TAG, "ws: async send to fd %d failed: %s", fd, esp_err_to_name(err));
        ws_client_reset_locked(fd);
        free(m->payload);
        free(m);
    }
}

static void ws_send_text_all(const char *json)
{
    if (!s_ws_server || !s_ws_mutex) {
        return;
    }
    /* Session TTL check on the push path too: without it a connection
     * authenticated just before the 24h expiry would keep receiving status/frame
     * pushes indefinitely until it happened to send a command. On expiry
     * web_auth_session_live() invalidates the session and bumps the generation,
     * so every client's gen check below fails from here on. */
    if (!web_auth_session_live()) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    uint32_t cur_gen = web_auth_get_gen();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd < 0 || !s_ws_clients[i].authed ||
            s_ws_clients[i].gen != cur_gen) {
            continue;
        }
        ws_async_send_text_locked(s_ws_clients[i].fd, json);
    }
    xSemaphoreGive(s_ws_mutex);
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

/* Send the cached status to clients whose acked_id is stale. Caller must
 * hold s_ws_mutex. */
static void ws_send_status_locked(void)
{
    if (!s_status_inner || !s_ws_server) {
        return;
    }
    /* Same TTL gate as ws_send_text_all: an expired session gets no more
     * status pushes (the gen bump on invalidation then stops all clients). */
    if (!web_auth_session_live()) {
        return;
    }
    size_t cap = strlen(s_status_inner) + 64;
    char *wrapped = malloc(cap);
    if (!wrapped) {
        return;
    }
    snprintf(wrapped, cap, "{\"type\":\"status\",\"id\":%lu,\"data\":%s}",
             (unsigned long)s_status_id, s_status_inner);
    uint32_t cur_gen = web_auth_get_gen();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd < 0 || !s_ws_clients[i].authed ||
            s_ws_clients[i].gen != cur_gen ||
            s_ws_clients[i].acked_id == s_status_id) {
            continue;
        }
        ws_async_send_text_locked(s_ws_clients[i].fd, wrapped);
    }
    free(wrapped);
}

/* Rebuild status and broadcast it with a fresh id immediately. Used for
 * event-driven pushes (e.g. playback start/stop) where the 1s sampling
 * timer could miss a short-lived state change. */
static void ws_push_status_now(void)
{
    if (!s_ws_server || !s_ws_mutex) {
        return;
    }
    char *inner = web_status_json();
    if (!inner) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        free(inner);
        return;
    }
    free(s_status_inner);
    s_status_inner = inner;
    s_status_id++;
    if (s_status_id == 0) {
        s_status_id = 1;
    }
    s_last_forced_push_us = esp_timer_get_time();
    ws_send_status_locked();
    xSemaphoreGive(s_ws_mutex);
}

static void ws_status_timer_cb(void *arg)
{
    (void)arg;
    if (!ws_has_authed_clients()) {
        return;
    }

    char *inner = web_status_json();
    if (!inner) {
        return;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        free(inner);
        return;
    }

    /* only push when the status actually changed, or as a periodic heartbeat
     * to keep idle connections alive through NAT idle timeouts */
    bool changed = !s_status_inner || strcmp(s_status_inner, inner) != 0;
    int64_t now = esp_timer_get_time();
    bool heartbeat = !changed && (now - s_last_forced_push_us >= WS_HEARTBEAT_INTERVAL_US);
    if (changed || heartbeat) {
        free(s_status_inner);
        s_status_inner = inner;
        s_status_id++;
        if (s_status_id == 0) {
            s_status_id = 1; /* skip 0 so "not acked yet" stays distinguishable */
        }
        if (heartbeat) {
            s_last_forced_push_us = now;
        }
    } else {
        free(inner);
    }

    /* send the latest status to clients that have not confirmed receipt yet;
     * unchanged states are never re-broadcast to clients that already acked */
    ws_send_status_locked();
    xSemaphoreGive(s_ws_mutex);
}

/* Playback start/stop: push the status immediately so the UI never stays
 * stuck at "playing" waiting for the next 1s sampling tick. */
static void ws_play_cb(bool playing, void *arg)
{
    (void)playing;
    (void)arg;
    ws_push_status_now();
}

static void ws_frame_push_cb(const ir_frame_t *fr, void *arg)
{
    (void)arg;
    if (!ws_has_authed_clients()) {
        return;
    }
    size_t cap = 16384 + 64; /* enough for IR_RAW_MAX_SEGS (1024) durations */
    char *buf = malloc(cap);
    if (!buf) {
        return;
    }
    int off = snprintf(buf, cap, "{\"type\":\"frame\",\"data\":");
    if (off < 0 || off >= (int)cap) {
        free(buf);
        return;
    }
    int n = web_frame_to_json(fr, buf + off, cap - (size_t)off);
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

void web_ws_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ws_client_remove(sockfd);
    /* With a custom close_fn set, esp_http_server does NOT close the socket
     * itself; we must close it here or every closed session leaks an lwIP
     * socket (eventually accept() fails with ENFILE / errno 23). */
    close(sockfd);
}

/* Best-effort peer IP of a WS client ("a.b.c.d"), used for the per-IP login
 * failure lockout. Returns false when the address cannot be resolved. */
static bool ws_peer_ip(httpd_req_t *req, char *buf, size_t len)
{
    if (!req || !buf || len < 16) {
        return false;
    }
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &alen) != 0) {
        return false;
    }
    uint32_t ip = ntohl(addr.sin_addr.s_addr);
    snprintf(buf, len, "%lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xFF), (unsigned long)((ip >> 16) & 0xFF),
             (unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF));
    return true;
}

/* Origin allow-list check, run as the WS *pre-handshake* callback. This is the
 * only point where esp_http_server still exposes the handshake headers: once
 * the 101 response is sent the request aux is re-initialized for raw frame
 * processing, so the per-frame ws_handler() can never read "Origin" again
 * (httpd_req_get_hdr_value_* would find no headers -> olen == 0 -> the old
 * in-handler check silently allowed every origin).
 *
 * Browsers always send an Origin header here; header-less clients (native
 * apps/scripts) are accepted by design — see web_origin_allowed(). When an
 * allow-list is configured and the header cannot be read (oversized/unparsable)
 * we fail closed: an unverifiable origin must not slip past the list.
 */
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
static esp_err_t ws_pre_handshake_cb(httpd_req_t *req)
{
    size_t olen = httpd_req_get_hdr_value_len(req, "Origin");
    if (olen == 0) {
        return ESP_OK; /* no Origin header: no cross-site browser authority to defend */
    }
    char origin[256];
    size_t want = olen < sizeof(origin) - 1 ? olen : sizeof(origin) - 1;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, want + 1) != ESP_OK) {
        if (web_origin_restricted()) {
            ESP_LOGW(TAG, "ws: unreadable Origin header rejected (allow-list active)");
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "origin not allowed");
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (!web_origin_allowed(origin)) {
        ESP_LOGW(TAG, "ws: origin \"%s\" rejected", origin);
        /* Reject before the 101 handshake, so the client receives a clean
         * HTTP 403 and the connection is never upgraded. */
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "origin not allowed");
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif /* CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT */

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t frame = {0};

    /* Origin validation happens in ws_pre_handshake_cb() (before the 101), not
     * here: after the upgrade this handler runs with no handshake headers. */

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > WEB_MAX_BODY_LEN) {
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
    if (cJSON_IsString(type) && strcmp(type->valuestring, "login") == 0) {
        /* WS login — the only unauthenticated operation (REST /api/login is
         * gone). On success this connection becomes authenticated immediately. */
        cJSON *u = cJSON_GetObjectItem(root, "user");
        cJSON *p = cJSON_GetObjectItem(root, "pass");
        const char *user = cJSON_IsString(u) ? u->valuestring : NULL;
        const char *pass = cJSON_IsString(p) ? p->valuestring : NULL;
        char peer[16] = "";
        ws_peer_ip(req, peer, sizeof(peer));
        char *out = NULL;
        esp_err_t lret = web_auth_login(user, pass, peer[0] ? peer : NULL, &out);
        if (out) {
            ws_reply_text(req, out);
            free(out);
        }
        /* Only mark the connection authenticated when the login truly succeeded
         * AND the client received its token. On OOM (ESP_ERR_NO_MEM) out is NULL
         * and marking it authed would leave a session that can never prove itself. */
        if (lret == ESP_OK && out) {
            ws_client_add(fd, true);
            ESP_LOGI(TAG, "ws: client fd %d logged in", fd);
            ws_push_status_now(); /* deliver current status immediately */
        }
        cJSON_Delete(root);
        return ESP_OK;
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "auth") == 0) {
        cJSON *tok = cJSON_GetObjectItem(root, "token");
        if (cJSON_IsString(tok) && web_auth_token_ok(tok->valuestring)) {
            ws_client_add(fd, true);
            ESP_LOGI(TAG, "ws: client fd %d authenticated", fd);
            ws_reply_text(req, "{\"type\":\"auth\",\"ok\":true}");
            /* the current status is delivered by the 1s push timer */
        } else {
            ESP_LOGW(TAG, "ws: auth failed for fd %d", fd);
            ws_reply_text(req, "{\"type\":\"auth\",\"ok\":false,\"error\":\"unauthorized\"}");
            cJSON_Delete(root);
            return ESP_FAIL; /* close the socket */
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "ack") == 0) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsNumber(id)) {
            ws_client_ack(fd, (uint32_t)id->valuedouble);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "cmd") == 0) {
        /* command RPC: requires an authenticated session against the current generation */
        if (!ws_client_gen_ok(fd)) {
            ws_reply_text(req, "{\"type\":\"resp\",\"ok\":false,\"error\":\"unauthorized\"}");
            cJSON_Delete(root);
            return ESP_FAIL; /* close the socket */
        }
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
        cJSON *body = cJSON_GetObjectItem(root, "body");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(cmd)) {
            ws_reply_text(req, "{\"type\":\"resp\",\"ok\":false,\"error\":\"bad cmd\"}");
            cJSON_Delete(root);
            return ESP_OK;
        }
        const char *cmdstr = cmd->valuestring;
        cJSON *bodyobj = cJSON_IsObject(body) ? body : root;
        const char *err = NULL;
        char *data = web_rpc_exec(cmdstr, bodyobj, &err);
        /* Build with cJSON (like mqtt_respond) so arbitrary command data / error
         * strings can never break out of the response JSON. */
        cJSON *resp = cJSON_CreateObject();
        char *reply = NULL;
        if (resp) {
            cJSON_AddStringToObject(resp, "type", "resp");
            cJSON_AddNumberToObject(resp, "id", id->valuedouble);
            if (data) {
                cJSON_AddBoolToObject(resp, "ok", true);
                cJSON *result = cJSON_Parse(data);
                if (result) {
                    cJSON_AddItemToObject(resp, "data", result);
                } else {
                    cJSON_AddStringToObject(resp, "data", data);
                }
            } else {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", err ? err : "failed");
            }
            reply = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
        }
        if (data) {
            free(data);
        }
        if (reply) {
            ws_reply_text(req, reply);
            free(reply);
        }
        bool is_logout = strcmp(cmdstr, "logout") == 0;
        cJSON_Delete(root);
        if (is_logout) {
            ws_client_remove(fd);
            return ESP_FAIL; /* close the socket after the response is flushed */
        }
        return ESP_OK;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t web_ws_register(httpd_handle_t server)
{
    s_ws_server = server;
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
    ir_set_play_cb(ws_play_cb, NULL);

    static const httpd_uri_t ws_uri = {
        .uri = "/api/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
        .ws_pre_handshake_cb = ws_pre_handshake_cb,
#endif
    };
    return httpd_register_uri_handler(server, &ws_uri);
}

#else /* !CONFIG_HTTPD_WS_SUPPORT */

esp_err_t web_ws_register(httpd_handle_t server)
{
    (void)server;
    return ESP_OK;
}

void web_ws_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    (void)sockfd;
}

#endif /* CONFIG_HTTPD_WS_SUPPORT */
