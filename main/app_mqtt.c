#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_mqtt.h"
#include "app_web_internal.h"   /* web_rpc_exec / web_status_json / web_frame_to_json */
#include "app_ir.h"
#include "app_wifi.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "mqtt"

/* Same upper bound as the web layer: one frame with IR_RAW_MAX_SEGS raw
 * durations (each <= 5 digits) fits comfortably. */
#define MQTT_FRAME_JSON_CAP 16384
/* MQTT send/receive buffer: large enough for one full IR frame payload
 * (raw durations) so multi-KB publishes go out in a single packet. */
#define MQTT_BUFFER_SIZE 12288

/* Frame publish queue. IR frames are best-effort real-time data (same
 * semantics as the WebSocket push) and are sent at QoS 0 from a dedicated
 * publisher task, NOT through the esp-mqtt outbox: esp-mqtt's task loop only
 * drains one outbox message per poll-read cycle (default 1 s), which would
 * throttle a continuous IR stream to ~1 frame/second. With a dedicated task,
 * throughput is limited only by the network. */
#define MQTT_FRAME_QUEUE_DEPTH 4

typedef struct {
    char *data;
    int len;
} mqtt_frame_msg_t;

#define MQTT_NS "ir_tool"

/* NVS keys for the web-editable MQTT configuration */
#define KEY_ENABLE "mqtt_enable"
#define KEY_PROTO  "mqtt_proto"
#define KEY_TLS    "mqtt_tls"
#define KEY_TSFX   "mqtt_tsfx"
#define KEY_BROKER "mqtt_broker"
#define KEY_USER   "mqtt_user"
#define KEY_PWD    "mqtt_pwd"
#define KEY_CID    "mqtt_cid"
#define KEY_T_CMD  "mqtt_t_cmd"
#define KEY_T_RSP  "mqtt_t_rsp"
#define KEY_T_ST   "mqtt_t_st"
#define KEY_T_FR   "mqtt_t_fr"
#define KEY_QOS    "mqtt_qos"
#define KEY_PUB_FR "mqtt_pub_fr"
#define KEY_PUB_ST "mqtt_pub_st"

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_started = false;
static volatile bool s_connected = false;

/* Runtime settings resolved from NVS (menuconfig provides defaults).
 * With the per-device topic suffix enabled the effective topics embed the
 * Client ID, so they can exceed the base topic length. */
#define MQTT_TOPIC_EFF_LEN 160
static char s_topic_cmd[MQTT_TOPIC_EFF_LEN];
static char s_topic_rsp[MQTT_TOPIC_EFF_LEN];
static char s_topic_status[MQTT_TOPIC_EFF_LEN];
static char s_topic_frame[MQTT_TOPIC_EFF_LEN];
static int s_qos = 1;
static bool s_publish_frames = true;
static bool s_publish_status = true;
static QueueHandle_t s_frame_queue = NULL;

/* ---------------- NVS config load / save ---------------- */

esp_err_t mqtt_web_config_load(mqtt_web_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = CONFIG_IR_TOOL_MQTT_ENABLE;
    cfg->mqtt5 = false; /* default protocol: 3.1.1 */
    cfg->tls_skip = false; /* default: verify against the built-in certificate bundle */
    cfg->topic_suffix = false; /* default: topics as configured */
    strlcpy(cfg->broker_uri, CONFIG_IR_TOOL_MQTT_BROKER_URI, sizeof(cfg->broker_uri));
    strlcpy(cfg->username, CONFIG_IR_TOOL_MQTT_USERNAME, sizeof(cfg->username));
    strlcpy(cfg->password, CONFIG_IR_TOOL_MQTT_PASSWORD, sizeof(cfg->password));
    strlcpy(cfg->client_id, CONFIG_IR_TOOL_MQTT_CLIENT_ID, sizeof(cfg->client_id));
    strlcpy(cfg->topic_cmd, CONFIG_IR_TOOL_MQTT_TOPIC_CMD, sizeof(cfg->topic_cmd));
    strlcpy(cfg->topic_rsp, CONFIG_IR_TOOL_MQTT_TOPIC_RSP, sizeof(cfg->topic_rsp));
    strlcpy(cfg->topic_status, CONFIG_IR_TOOL_MQTT_TOPIC_STATUS, sizeof(cfg->topic_status));
    strlcpy(cfg->topic_frame, CONFIG_IR_TOOL_MQTT_TOPIC_FRAME, sizeof(cfg->topic_frame));
    cfg->qos = CONFIG_IR_TOOL_MQTT_QOS;
    cfg->publish_frames = CONFIG_IR_TOOL_MQTT_PUBLISH_FRAMES;
    cfg->publish_status = CONFIG_IR_TOOL_MQTT_PUBLISH_STATUS;

    nvs_handle_t h;
    if (nvs_open(MQTT_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK; /* defaults only */
    }
    uint8_t v8 = cfg->enabled ? 1 : 0;
    if (nvs_get_u8(h, KEY_ENABLE, &v8) == ESP_OK) {
        cfg->enabled = v8 != 0;
    }
    v8 = 0;
    if (nvs_get_u8(h, KEY_PROTO, &v8) == ESP_OK) {
        cfg->mqtt5 = v8 != 0;
    }
    v8 = 0;
    if (nvs_get_u8(h, KEY_TLS, &v8) == ESP_OK) {
        cfg->tls_skip = v8 != 0;
    }
    v8 = 0;
    if (nvs_get_u8(h, KEY_TSFX, &v8) == ESP_OK) {
        cfg->topic_suffix = v8 != 0;
    }
    size_t len;
    len = sizeof(cfg->broker_uri);
    if (nvs_get_str(h, KEY_BROKER, cfg->broker_uri, &len) != ESP_OK) {
        strlcpy(cfg->broker_uri, CONFIG_IR_TOOL_MQTT_BROKER_URI, sizeof(cfg->broker_uri));
    }
    len = sizeof(cfg->username);
    if (nvs_get_str(h, KEY_USER, cfg->username, &len) != ESP_OK) {
        strlcpy(cfg->username, CONFIG_IR_TOOL_MQTT_USERNAME, sizeof(cfg->username));
    }
    len = sizeof(cfg->password);
    if (nvs_get_str(h, KEY_PWD, cfg->password, &len) != ESP_OK) {
        strlcpy(cfg->password, CONFIG_IR_TOOL_MQTT_PASSWORD, sizeof(cfg->password));
    }
    len = sizeof(cfg->client_id);
    if (nvs_get_str(h, KEY_CID, cfg->client_id, &len) != ESP_OK) {
        strlcpy(cfg->client_id, CONFIG_IR_TOOL_MQTT_CLIENT_ID, sizeof(cfg->client_id));
    }
    len = sizeof(cfg->topic_cmd);
    if (nvs_get_str(h, KEY_T_CMD, cfg->topic_cmd, &len) != ESP_OK) {
        strlcpy(cfg->topic_cmd, CONFIG_IR_TOOL_MQTT_TOPIC_CMD, sizeof(cfg->topic_cmd));
    }
    len = sizeof(cfg->topic_rsp);
    if (nvs_get_str(h, KEY_T_RSP, cfg->topic_rsp, &len) != ESP_OK) {
        strlcpy(cfg->topic_rsp, CONFIG_IR_TOOL_MQTT_TOPIC_RSP, sizeof(cfg->topic_rsp));
    }
    len = sizeof(cfg->topic_status);
    if (nvs_get_str(h, KEY_T_ST, cfg->topic_status, &len) != ESP_OK) {
        strlcpy(cfg->topic_status, CONFIG_IR_TOOL_MQTT_TOPIC_STATUS, sizeof(cfg->topic_status));
    }
    len = sizeof(cfg->topic_frame);
    if (nvs_get_str(h, KEY_T_FR, cfg->topic_frame, &len) != ESP_OK) {
        strlcpy(cfg->topic_frame, CONFIG_IR_TOOL_MQTT_TOPIC_FRAME, sizeof(cfg->topic_frame));
    }
    uint8_t qos = (uint8_t)cfg->qos;
    if (nvs_get_u8(h, KEY_QOS, &qos) == ESP_OK && qos <= 2) {
        cfg->qos = qos;
    }
    v8 = cfg->publish_frames ? 1 : 0;
    if (nvs_get_u8(h, KEY_PUB_FR, &v8) == ESP_OK) {
        cfg->publish_frames = v8 != 0;
    }
    v8 = cfg->publish_status ? 1 : 0;
    if (nvs_get_u8(h, KEY_PUB_ST, &v8) == ESP_OK) {
        cfg->publish_status = v8 != 0;
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t mqtt_web_config_save(const mqtt_web_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(MQTT_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_u8(h, KEY_ENABLE, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_PROTO, cfg->mqtt5 ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TLS, cfg->tls_skip ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TSFX, cfg->topic_suffix ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_BROKER, cfg->broker_uri);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_USER, cfg->username);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PWD, cfg->password);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_CID, cfg->client_id);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_T_CMD, cfg->topic_cmd);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_T_RSP, cfg->topic_rsp);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_T_ST, cfg->topic_status);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_T_FR, cfg->topic_frame);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_QOS, (uint8_t)cfg->qos);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_PUB_FR, cfg->publish_frames ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_PUB_ST, cfg->publish_status ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------------- web RPC cores (shared with the WebSocket channel) ---------------- */

static void mqtt_restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Restarting to apply MQTT config...");
    esp_restart();
}

static void mqtt_schedule_restart(void)
{
    /* restart 2s later so the WS response is flushed first */
    static esp_timer_handle_t t = NULL;
    if (!t) {
        const esp_timer_create_args_t args = {
            .callback = mqtt_restart_cb,
            .name = "mqtt_restart",
        };
        esp_timer_create(&args, &t);
    }
    if (t) {
        esp_timer_start_once(t, 2 * 1000 * 1000);
    }
}

static bool mqtt_topic_ok(const char *t)
{
    if (!t || t[0] == '\0') {
        return false;
    }
    for (const char *p = t; *p; p++) {
        if (*p == '+' || *p == '#') {
            return false;
        }
    }
    return true;
}

/* Replace characters that are invalid inside an MQTT topic filter
 * (wildcards +/# and the level separator /) so the Client ID can be embedded. */
static void mqtt_topic_sanitize(const char *in, char *out, size_t out_sz)
{
    size_t i = 0;
    for (; in[i] != '\0' && i + 1 < out_sz; i++) {
        char c = in[i];
        out[i] = (c == '+' || c == '#' || c == '/') ? '-' : c;
    }
    out[i] = '\0';
}

/* Insert the per-device suffix right after the first path level:
 * "ir-web/cmd" + "esp-a1b2c3" -> "ir-web/esp-a1b2c3/cmd". */
static void mqtt_effective_topic(const char *base, const char *suffix, bool enabled,
                                 char *out, size_t out_sz)
{
    if (!enabled || !suffix || suffix[0] == '\0') {
        strlcpy(out, base, out_sz);
        return;
    }
    const char *slash = strchr(base, '/');
    if (slash) {
        int root_len = (int)(slash - base + 1);
        snprintf(out, out_sz, "%.*s%s/%s", root_len, base, suffix, slash + 1);
    } else {
        snprintf(out, out_sz, "%s/%s", base, suffix);
    }
}

/* Current (NVS) MQTT configuration as a JSON string (caller frees).
 * Passwords are never echoed back, only a password_set flag. */
char *web_mqttcfg_get_json(void)
{
    mqtt_web_config_t cfg;
    mqtt_web_config_load(&cfg);

    char *buf = malloc(2048);
    if (!buf) {
        return NULL;
    }
    int n = snprintf(buf, 2048,
        "{\"enabled\":%s,\"protocol\":\"%s\",\"tls_verify\":\"%s\","
        "\"topic_suffix\":%s,"
        "\"broker_uri\":\"%s\",\"username\":\"%s\","
        "\"password_set\":%s,\"client_id\":\"%s\","
        "\"topic_cmd\":\"%s\",\"topic_rsp\":\"%s\",\"topic_status\":\"%s\","
        "\"topic_frame\":\"%s\",\"qos\":%d,"
        "\"publish_frames\":%s,\"publish_status\":%s}",
        cfg.enabled ? "true" : "false",
        cfg.mqtt5 ? "5" : "311",
        cfg.tls_skip ? "skip" : "bundle",
        cfg.topic_suffix ? "true" : "false",
        cfg.broker_uri, cfg.username, cfg.password[0] ? "true" : "false",
        cfg.client_id,
        cfg.topic_cmd, cfg.topic_rsp, cfg.topic_status, cfg.topic_frame,
        cfg.qos,
        cfg.publish_frames ? "true" : "false",
        cfg.publish_status ? "true" : "false");
    (void)n;
    return buf;
}

/* Apply an MQTT configuration from a JSON body and schedule a restart.
 * password absent/null = keep current; "" = clear; string = set. */
esp_err_t web_mqttcfg_set(cJSON *root, const char **err)
{
    mqtt_web_config_t cfg;
    mqtt_web_config_load(&cfg); /* start from current values */

    cJSON *j = NULL;
    j = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(j)) {
        cfg.enabled = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItem(root, "protocol");
    if (cJSON_IsString(j)) {
        if (strcmp(j->valuestring, "311") == 0) {
            cfg.mqtt5 = false;
        } else if (strcmp(j->valuestring, "5") == 0) {
            cfg.mqtt5 = true;
        } else {
            *err = "protocol invalid";
            return ESP_ERR_INVALID_ARG;
        }
    }
    j = cJSON_GetObjectItem(root, "tls_verify");
    if (cJSON_IsString(j)) {
        if (strcmp(j->valuestring, "bundle") == 0) {
            cfg.tls_skip = false;
        } else if (strcmp(j->valuestring, "skip") == 0) {
            cfg.tls_skip = true;
        } else {
            *err = "tls_verify invalid";
            return ESP_ERR_INVALID_ARG;
        }
    }
    j = cJSON_GetObjectItem(root, "topic_suffix");
    if (cJSON_IsBool(j)) {
        cfg.topic_suffix = cJSON_IsTrue(j);
    }

    static const char *const str_fields[] = {
        "broker_uri", "username", "client_id",
        "topic_cmd", "topic_rsp", "topic_status", "topic_frame"
    };
    static const size_t str_caps[] = {
        sizeof(cfg.broker_uri), sizeof(cfg.username), sizeof(cfg.client_id),
        sizeof(cfg.topic_cmd), sizeof(cfg.topic_rsp),
        sizeof(cfg.topic_status), sizeof(cfg.topic_frame)
    };
    char *str_dsts[] = {
        cfg.broker_uri, cfg.username, cfg.client_id,
        cfg.topic_cmd, cfg.topic_rsp, cfg.topic_status, cfg.topic_frame
    };
    for (size_t i = 0; i < sizeof(str_fields) / sizeof(str_fields[0]); i++) {
        cJSON *it = cJSON_GetObjectItem(root, str_fields[i]);
        if (!cJSON_IsString(it)) {
            continue;
        }
        /* Reject characters that would break the JSON echo or MQTT topics */
        if (strlen(it->valuestring) >= str_caps[i] ||
            strpbrk(it->valuestring, "\"\\")) {
            *err = "invalid value";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(str_dsts[i], it->valuestring, str_caps[i]);
    }

    j = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(j)) {
        /* Passwords are stored in NVS and passed to esp-mqtt directly, never
         * echoed back, so only the length is constrained. */
        if (strlen(j->valuestring) >= sizeof(cfg.password)) {
            *err = "invalid password";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(cfg.password, j->valuestring, sizeof(cfg.password));
    }

    j = cJSON_GetObjectItem(root, "qos");
    if (cJSON_IsNumber(j)) {
        int q = (int)j->valuedouble;
        if (q < 0 || q > 2) {
            *err = "qos invalid";
            return ESP_ERR_INVALID_ARG;
        }
        cfg.qos = q;
    }
    j = cJSON_GetObjectItem(root, "publish_frames");
    if (cJSON_IsBool(j)) {
        cfg.publish_frames = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItem(root, "publish_status");
    if (cJSON_IsBool(j)) {
        cfg.publish_status = cJSON_IsTrue(j);
    }

    /* validation */
    if (!mqtt_topic_ok(cfg.topic_cmd) || !mqtt_topic_ok(cfg.topic_rsp) ||
        !mqtt_topic_ok(cfg.topic_status) || !mqtt_topic_ok(cfg.topic_frame)) {
        *err = "topics must be non-empty and contain no wildcards (+/#)";
        return ESP_ERR_INVALID_ARG;
    }
    /* With the topic suffix enabled the Client ID is embedded in topics, where
     * / + # are not allowed. Rejecting them here keeps the topic suffix an
     * exact copy of the Client ID, so two IDs that differ only in those
     * characters (e.g. "a/b" vs "a-b") cannot collapse onto the same topics. */
    if (cfg.topic_suffix && cfg.client_id[0] != '\0' &&
        strpbrk(cfg.client_id, "/+#")) {
        *err = "client_id must not contain + # / when topic suffix is enabled";
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mqtt_web_config_save(&cfg);
    if (ret != ESP_OK) {
        *err = "nvs write failed";
        return ret;
    }

    mqtt_schedule_restart();
    return ESP_OK;
}

/* ---------------- publish helpers ---------------- */

/* Publish a heap/stack payload to a topic. Copies into the outbox
 * (store=true) so the caller may free the buffer right after this returns;
 * the actual network send happens inside the MQTT task. */
static void mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain)
{
    if (!s_client || !s_connected || !payload) {
        return;
    }
    int msg_id = esp_mqtt_client_enqueue(s_client, topic, payload, len,
                                         qos, retain ? 1 : 0, true);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish failed to \"%s\" (msg_id=%d)", topic, msg_id);
    }
}

/* Dedicated publisher for IR frames (QoS 0, direct synchronous send). */
static void mqtt_frame_pub_task(void *arg)
{
    (void)arg;
    for (;;) {
        mqtt_frame_msg_t *msg = NULL;
        if (xQueueReceive(s_frame_queue, &msg, portMAX_DELAY) != pdTRUE || !msg) {
            continue;
        }
        if (s_client && s_connected) {
            int r = esp_mqtt_client_publish(s_client, s_topic_frame,
                                            msg->data, msg->len, 0, 0);
            if (r < 0) {
                ESP_LOGW(TAG, "frame publish failed (msg_id=%d)", r);
            }
        }
        free(msg->data);
        free(msg);
    }
}

static void mqtt_publish_status(void)
{
    if (!s_publish_status) {
        return;
    }
    char *json = web_status_json();
    if (!json) {
        return;
    }
    /* Retained so a subscriber joining later gets the last known state; the
     * LWT "offline" message replaces it when the device drops. */
    mqtt_publish(s_topic_status, json, (int)strlen(json), s_qos, true);
    free(json);
}

/* ---------------- command dispatch (same RPC cores as the WebSocket channel) ---------------- */

static void mqtt_respond(const char *cmd, const char *id, char *data, const char *err)
{
    const char *emsg = err ? err : "failed";
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(data);
        return;
    }
    cJSON_AddBoolToObject(resp, "ok", data != NULL);
    if (id) {
        cJSON_AddStringToObject(resp, "id", id);
    }
    if (cmd) {
        cJSON_AddStringToObject(resp, "cmd", cmd);
    }
    if (data) {
        cJSON *result = cJSON_Parse(data);
        if (result) {
            cJSON_AddItemToObject(resp, "result", result);
        } else {
            cJSON_AddStringToObject(resp, "result", data);
        }
    } else {
        cJSON_AddStringToObject(resp, "error", emsg);
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) {
        mqtt_publish(s_topic_rsp, json, (int)strlen(json), s_qos, false);
        free(json);
    }
    free(data);
}

static void mqtt_dispatch(const char *cmd, const char *id, cJSON *body)
{
    const char *err = NULL;
    char *data = NULL;
    if (!cmd) {
        err = "missing cmd";
    } else {
        data = web_rpc_exec(cmd, body, &err);
    }
    mqtt_respond(cmd, id, data, err);
}

/* Command topic payload: JSON {"id":"...","cmd":"...","body":{...}}.
 * A bare string payload is also accepted and treated as the command name
 * (e.g. "status"). The optional "id" is echoed back in the response. */
static void mqtt_handle_command(const char *payload, int len)
{
    if (!payload || len <= 0) {
        return;
    }

    const char *cmd = NULL;
    const char *id = NULL;
    cJSON *body = NULL;
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (root) {
        cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
        if (cJSON_IsString(jcmd)) {
            cmd = jcmd->valuestring;
        }
        cJSON *jid = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(jid)) {
            id = jid->valuestring;
        }
        cJSON *jbody = cJSON_GetObjectItem(root, "body");
        if (cJSON_IsObject(jbody)) {
            body = jbody;
        }
        mqtt_dispatch(cmd, id, body);
        cJSON_Delete(root);
        return;
    }

    /* Not JSON: treat the whole payload as a bare command name. */
    char *bare = malloc((size_t)len + 1);
    if (!bare) {
        return;
    }
    memcpy(bare, payload, (size_t)len);
    bare[len] = '\0';
    mqtt_dispatch(bare, NULL, NULL);
    free(bare);
}

/* ---------------- IR frame / playback callbacks (invoked from IR tasks) ---------------- */

static void mqtt_frame_cb(const ir_frame_t *frame, void *arg)
{
    (void)arg;
    if (!s_publish_frames || !s_connected || !frame || !s_frame_queue) {
        return;
    }
    char *buf = malloc(MQTT_FRAME_JSON_CAP);
    if (!buf) {
        return;
    }
    int n = web_frame_to_json(frame, buf, MQTT_FRAME_JSON_CAP);
    if (n < 0) {
        free(buf);
        return;
    }
    mqtt_frame_msg_t *msg = malloc(sizeof(*msg));
    if (!msg) {
        free(buf);
        return;
    }
    msg->data = buf;
    msg->len = n;
    if (xQueueSend(s_frame_queue, &msg, 0) != pdTRUE) {
        /* Rate-limit the log: under a continuous IR stream this can fire at
         * capture rate and would flood the console. */
        static int64_t s_last_drop_log_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_drop_log_us > 1000 * 1000) {
            s_last_drop_log_us = now;
            ESP_LOGW(TAG, "dropping frame (publish queue full)");
        }
        free(msg->data);
        free(msg);
        return;
    }
}

static void mqtt_play_cb(bool playing, void *arg)
{
    (void)arg;
    (void)playing;
    /* Publish full status (carries "playing") when playback starts/stops. */
    mqtt_publish_status();
}

/* ---------------- esp-mqtt events ---------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        s_connected = true;
        esp_mqtt_client_subscribe_single(s_client, s_topic_cmd, s_qos);
        mqtt_publish_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (ev && ev->data_len > 0 &&
            ev->topic_len == (int)strlen(s_topic_cmd) &&
            strncmp(ev->topic, s_topic_cmd, (size_t)ev->topic_len) == 0) {
            if (ev->total_data_len == ev->data_len) {
                mqtt_handle_command(ev->data, ev->data_len);
            } else {
                ESP_LOGW(TAG, "fragmented command ignored (%d/%d bytes)",
                         ev->data_len, ev->total_data_len);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev && ev->error_handle) {
            ESP_LOGW(TAG, "MQTT error type=%d", ev->error_handle->error_type);
        }
        break;
    default:
        break;
    }
}

/* ---------------- WiFi lifecycle hooks ---------------- */

static void mqtt_start_if_needed(void)
{
    if (!s_client || s_started) {
        return;
    }
    if (esp_mqtt_client_start(s_client) == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "MQTT client started");
    } else {
        ESP_LOGE(TAG, "MQTT client failed to start");
    }
}

static void mqtt_stop_if_running(void)
{
    if (!s_client || !s_started) {
        return;
    }
    esp_mqtt_client_stop(s_client);
    s_started = false;
    s_connected = false;
    ESP_LOGW(TAG, "MQTT client stopped (station link lost)");
}

static void mqtt_wifi_event_handler(void *arg, esp_event_base_t base,
                                    int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        mqtt_start_if_needed();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_stop_if_running();
    }
}

/* ---------------- public API ---------------- */

esp_err_t mqtt_init(void)
{
#if CONFIG_IR_TOOL_MQTT_ENABLE
    mqtt_web_config_t cfg;
    mqtt_web_config_load(&cfg);

    if (!cfg.enabled || cfg.broker_uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled (enabled=%d, broker URI empty)", cfg.enabled);
        return ESP_OK;
    }

    /* Client ID: configured value, or auto-generate one from the MAC.
     * s_topic_suffix holds the full sanitized Client ID (validated at save
     * time to contain no / + #), so it is never silently truncated and two
     * devices with distinct Client IDs always get distinct topic suffixes. */
    static char s_client_id[32];
    static char s_topic_suffix[MQTT_CFG_STR_LEN];
    const char *cid = cfg.client_id[0] ? cfg.client_id : NULL;
    if (!cid) {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_client_id, sizeof(s_client_id), "ir-web-%02X%02X%02X",
                     mac[3], mac[4], mac[5]);
            cid = s_client_id;
        }
    }
    if (cid) {
        /* Sanitization is a no-op for validated Client IDs, but keep it as a
         * guard for the auto-generated/MAC path. */
        mqtt_topic_sanitize(cid, s_topic_suffix, sizeof(s_topic_suffix));
    } else {
        /* No Client ID at all: suffixing would collapse every device onto the
         * same topics (the exact cross-talk it exists to prevent), so disable
         * it rather than fall back to a constant suffix. */
        ESP_LOGW(TAG, "no Client ID available, disabling topic suffix");
        cfg.topic_suffix = false;
    }

    mqtt_effective_topic(cfg.topic_cmd, s_topic_suffix, cfg.topic_suffix,
                         s_topic_cmd, sizeof(s_topic_cmd));
    mqtt_effective_topic(cfg.topic_rsp, s_topic_suffix, cfg.topic_suffix,
                         s_topic_rsp, sizeof(s_topic_rsp));
    mqtt_effective_topic(cfg.topic_status, s_topic_suffix, cfg.topic_suffix,
                         s_topic_status, sizeof(s_topic_status));
    mqtt_effective_topic(cfg.topic_frame, s_topic_suffix, cfg.topic_suffix,
                         s_topic_frame, sizeof(s_topic_frame));
    s_qos = cfg.qos;
    s_publish_frames = cfg.publish_frames;
    s_publish_status = cfg.publish_status;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg.broker_uri,
        .credentials.client_id = cid,
        .credentials.username = cfg.username[0] ? cfg.username : NULL,
        .credentials.authentication.password = cfg.password[0] ? cfg.password : NULL,
        /* TLS: verify against the built-in ESP-IDF certificate bundle, or skip
         * verification entirely when the user opts for a self-signed / private
         * CA broker (esp-tls falls back to VERIFY_NONE in that case). */
        .broker.verification.crt_bundle_attach = cfg.tls_skip ? NULL : esp_crt_bundle_attach,
        .session.protocol_ver = cfg.mqtt5 ? MQTT_PROTOCOL_V_5 : MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 60,
        .session.last_will.topic = s_topic_status,
        .session.last_will.msg = "offline",
        .session.last_will.qos = s_qos,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = MQTT_BUFFER_SIZE,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               mqtt_wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               mqtt_wifi_event_handler, NULL);

    ir_set_frame_cb(mqtt_frame_cb, NULL);
    ir_set_play_cb(mqtt_play_cb, NULL);

    /* Dedicated frame publisher: keeps the IR capture task non-blocking and
     * bypasses the esp-mqtt outbox / 1-message-per-poll-cycle throttle. */
    s_frame_queue = xQueueCreate(MQTT_FRAME_QUEUE_DEPTH, sizeof(mqtt_frame_msg_t *));
    if (s_frame_queue &&
        xTaskCreate(mqtt_frame_pub_task, "mqtt_pub", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to create frame publisher task");
        vQueueDelete(s_frame_queue);
        s_frame_queue = NULL;
    }

    /* wifi_init() blocks until STA is connected (or falls back to AP), so the
     * GOT_IP event may already have fired before we registered. Start now. */
    if (wifi_is_sta_connected()) {
        mqtt_start_if_needed();
    } else {
        ESP_LOGI(TAG, "MQTT ready (will connect when station link is up)");
    }

    ESP_LOGI(TAG, "MQTT configured: protocol=%s tls=%s broker=%s client_id=%s",
             cfg.mqtt5 ? "5.0" : "3.1.1", cfg.tls_skip ? "skip-verify" : "bundle",
             cfg.broker_uri, cid ? cid : "(auto)");
    ESP_LOGI(TAG, "MQTT topics: cmd=%s rsp=%s status=%s frame=%s",
             s_topic_cmd, s_topic_rsp, s_topic_status, s_topic_frame);
#else
    ESP_LOGI(TAG, "MQTT disabled by menuconfig");
#endif
    return ESP_OK;
}
