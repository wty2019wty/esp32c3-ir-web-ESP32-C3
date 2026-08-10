#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_mqtt.h"
#include "app_web_internal.h"   /* web_rpc_exec / web_status_json / web_frame_to_json */
#include "app_ir.h"
#include "app_wifi.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"

#define TAG "mqtt"

/* Same upper bound as the web layer: one frame with IR_RAW_MAX_SEGS raw
 * durations (each <= 5 digits) fits comfortably. */
#define MQTT_FRAME_JSON_CAP 16384

/* Drop frame publishes while this many QoS 1/2 messages are unacknowledged,
 * so a slow broker cannot balloon the outbox with multi-KB frames. */
#define MQTT_MAX_PENDING_FRAMES 4

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_started = false;
static volatile bool s_connected = false;

#if CONFIG_IR_TOOL_MQTT_QOS > 0
static volatile int s_pending = 0; /* unacked QoS1/2 publishes, approximate */
#endif

/* Publish a heap/stack payload to a topic. Copies into the outbox
 * (store=true) so the caller may free the buffer right after this returns;
 * the actual network send happens inside the MQTT task. */
static void mqtt_publish(const char *topic, const char *payload, int len, bool retain)
{
    if (!s_client || !s_connected || !payload) {
        return;
    }
    int msg_id = esp_mqtt_client_enqueue(s_client, topic, payload, len,
                                         CONFIG_IR_TOOL_MQTT_QOS,
                                         retain ? 1 : 0, true);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish failed to \"%s\" (msg_id=%d)", topic, msg_id);
    }
}

static void mqtt_publish_status(void)
{
#if CONFIG_IR_TOOL_MQTT_PUBLISH_STATUS
    char *json = web_status_json();
    if (!json) {
        return;
    }
    /* Retained so a subscriber joining later gets the last known state; the
     * LWT "offline" message replaces it when the device drops. */
    mqtt_publish(CONFIG_IR_TOOL_MQTT_TOPIC_STATUS, json, (int)strlen(json), true);
    free(json);
#endif
}

/* ---- command dispatch (same RPC cores as the WebSocket channel) ---- */

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
        mqtt_publish(CONFIG_IR_TOOL_MQTT_TOPIC_RSP, json, (int)strlen(json), false);
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

/* ---- IR frame / playback callbacks (invoked from IR tasks) ---- */

static void mqtt_frame_cb(const ir_frame_t *frame, void *arg)
{
    (void)arg;
#if CONFIG_IR_TOOL_MQTT_PUBLISH_FRAMES
    if (!s_connected || !frame) {
        return;
    }
#if CONFIG_IR_TOOL_MQTT_QOS > 0
    if (s_pending >= MQTT_MAX_PENDING_FRAMES) {
        ESP_LOGW(TAG, "dropping frame (outbox backlog)");
        return;
    }
#endif
    char *buf = malloc(MQTT_FRAME_JSON_CAP);
    if (!buf) {
        return;
    }
    int n = web_frame_to_json(frame, buf, MQTT_FRAME_JSON_CAP);
    if (n < 0) {
        free(buf);
        return;
    }
#if CONFIG_IR_TOOL_MQTT_QOS > 0
    s_pending++;
#endif
    mqtt_publish(CONFIG_IR_TOOL_MQTT_TOPIC_FRAME, buf, n, false);
    free(buf);
#endif
}

static void mqtt_play_cb(bool playing, void *arg)
{
    (void)arg;
    /* Publish full status (carries "playing") when playback starts/stops. */
    mqtt_publish_status();
}

/* ---- esp-mqtt events ---- */

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
        esp_mqtt_client_subscribe_single(s_client, CONFIG_IR_TOOL_MQTT_TOPIC_CMD,
                                         CONFIG_IR_TOOL_MQTT_QOS);
        mqtt_publish_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (ev && ev->data_len > 0 &&
            ev->topic_len == (int)strlen(CONFIG_IR_TOOL_MQTT_TOPIC_CMD) &&
            strncmp(ev->topic, CONFIG_IR_TOOL_MQTT_TOPIC_CMD, (size_t)ev->topic_len) == 0) {
            if (ev->total_data_len == ev->data_len) {
                mqtt_handle_command(ev->data, ev->data_len);
            } else {
                ESP_LOGW(TAG, "fragmented command ignored (%d/%d bytes)",
                         ev->data_len, ev->total_data_len);
            }
        }
        break;
#if CONFIG_IR_TOOL_MQTT_QOS > 0
    case MQTT_EVENT_PUBLISHED:
        if (s_pending > 0) {
            s_pending--;
        }
        break;
#endif
    case MQTT_EVENT_ERROR:
        if (ev && ev->error_handle) {
            ESP_LOGW(TAG, "MQTT error type=%d", ev->error_handle->error_type);
        }
        break;
    default:
        break;
    }
}

/* ---- WiFi lifecycle hooks ---- */

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

/* ---- public API ---- */

esp_err_t mqtt_init(void)
{
#if CONFIG_IR_TOOL_MQTT_ENABLE
    const char *uri = CONFIG_IR_TOOL_MQTT_BROKER_URI;
    if (uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled (broker URI empty, set in menuconfig)");
        return ESP_OK;
    }

    /* Client ID: menuconfig value, or auto-generate one from the MAC. */
    static char s_client_id[32];
    const char *cid = CONFIG_IR_TOOL_MQTT_CLIENT_ID;
    if (cid[0] == '\0') {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_client_id, sizeof(s_client_id), "ir-web-%02X%02X%02X",
                     mac[3], mac[4], mac[5]);
            cid = s_client_id;
        } else {
            cid = NULL; /* let esp-mqtt fall back to its default */
        }
    }

    const char *user = CONFIG_IR_TOOL_MQTT_USERNAME;
    const char *pass = CONFIG_IR_TOOL_MQTT_PASSWORD;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = cid,
        .credentials.username = (user && user[0]) ? user : NULL,
        .credentials.authentication.password = (pass && pass[0]) ? pass : NULL,
        .session.keepalive = 60,
        .session.last_will.topic = CONFIG_IR_TOOL_MQTT_TOPIC_STATUS,
        .session.last_will.msg = "offline",
        .session.last_will.qos = CONFIG_IR_TOOL_MQTT_QOS,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
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

    /* wifi_init() blocks until STA is connected (or falls back to AP), so the
     * GOT_IP event may already have fired before we registered. Start now. */
    if (wifi_is_sta_connected()) {
        mqtt_start_if_needed();
    } else {
        ESP_LOGI(TAG, "MQTT ready (will connect when station link is up)");
    }

    ESP_LOGI(TAG, "MQTT configured: broker=%s client_id=%s", uri,
             cid ? cid : "(auto)");
#else
    ESP_LOGI(TAG, "MQTT disabled by menuconfig");
#endif
    return ESP_OK;
}
