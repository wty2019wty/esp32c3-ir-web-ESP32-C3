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

#define TAG "web"

#if CONFIG_HTTPD_WS_SUPPORT

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
} ws_client_t;

static httpd_handle_t s_ws_server = NULL;
static ws_client_t s_ws_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;
static uint32_t s_status_id = 0;    /* bumped on every status change */
static char *s_status_inner = NULL; /* cached status payload (data part) */
static int64_t s_last_forced_push_us = 0; /* last heartbeat/forced status push (us) */

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
            s_ws_clients[i].pending = 0; /* drop any stale back-pressure */
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
            s_ws_clients[i].gen = authed ? web_auth_get_gen() : 0;
            s_ws_clients[i].acked_id = 0;
            s_ws_clients[i].pending = 0;
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
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static bool ws_client_is_authed(int fd)
{
    bool a = false;
    if (!s_ws_mutex || fd < 0) {
        return false;
    }
    if (xSemaphoreTake(s_ws_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd && s_ws_clients[i].authed) {
            a = true;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    return a;
}

/* True when the client authenticated against the current session generation;
 * false after logout / credential changes invalidate the session. */
static bool ws_client_gen_ok(int fd)
{
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

/* ---- async push: serialize ALL sends through the httpd task --------------
 * httpd_ws_send_frame_async() writes to the socket from the *caller's* task
 * while the httpd task concurrently writes command replies/pongs to the same
 * fd -> interleaved bytes -> "Invalid frame header" on the client. Instead we
 * queue the frame via httpd_ws_send_data_async() so every write happens in the
 * httpd task context. The payload is copied here and freed in the callback. */

#define WS_MAX_PENDING 4   /* queued frames per client; extra pushes are dropped */

typedef struct {
    int fd;
    char *payload;
} ws_async_msg_t;

static void ws_async_send_done(esp_err_t err, int socket, void *arg)
{
    ws_async_msg_t *m = arg;
    if (err != ESP_OK) {
        /* Runs in the httpd task (s_ws_mutex not held). Remove the client only
         * if it still has this queued send outstanding; if the slot was already
         * freed (close) or reused (fd value recycled for a new client, pending
         * reset to 0), leave the current occupant alone. */
        ESP_LOGD(TAG, "ws: async send to fd %d failed: %s", socket, esp_err_to_name(err));
        if (m && s_ws_mutex && xSemaphoreTake(s_ws_mutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (s_ws_clients[i].fd == m->fd && s_ws_clients[i].pending > 0) {
                    s_ws_clients[i].fd = -1;
                    s_ws_clients[i].authed = false;
                    s_ws_clients[i].pending = 0;
                    break;
                }
            }
            xSemaphoreGive(s_ws_mutex);
        }
    } else if (m && s_ws_mutex && xSemaphoreTake(s_ws_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].fd == m->fd && s_ws_clients[i].pending > 0) {
                s_ws_clients[i].pending--;
                break;
            }
        }
        xSemaphoreGive(s_ws_mutex);
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
    m->payload = dup;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
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
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].fd == fd) {
                s_ws_clients[i].fd = -1;
                s_ws_clients[i].authed = false;
                s_ws_clients[i].pending = 0;
                break;
            }
        }
        free(m->payload);
        free(m);
    }
}

static void ws_send_text_all(const char *json)
{
    if (!s_ws_server || !s_ws_mutex) {
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
        char *out = NULL;
        esp_err_t lret = web_auth_login(user, pass, &out);
        if (out) {
            ws_reply_text(req, out);
            free(out);
        }
        if (lret == ESP_OK) {
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
        /* command RPC: requires an authenticated session */
        if (!ws_client_is_authed(fd) || !ws_client_gen_ok(fd)) {
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
        char *reply = NULL;
        if (data) {
            size_t cap = strlen(data) + 96;
            reply = malloc(cap);
            if (reply) {
                snprintf(reply, cap, "{\"type\":\"resp\",\"id\":%ld,\"ok\":true,\"data\":%s}",
                         (long)id->valuedouble, data);
            }
            free(data);
        } else {
            const char *msg = err ? err : "failed";
            size_t cap = strlen(msg) + 64;
            reply = malloc(cap);
            if (reply) {
                snprintf(reply, cap, "{\"type\":\"resp\",\"id\":%ld,\"ok\":false,\"error\":\"%s\"}",
                         (long)id->valuedouble, msg);
            }
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
