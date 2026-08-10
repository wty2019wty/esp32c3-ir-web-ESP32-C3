#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_CFG_BROKER_LEN 128
#define MQTT_CFG_STR_LEN    64

/* Web-editable MQTT configuration (persisted in NVS, menuconfig provides
 * defaults). Topics must be non-empty and must not contain MQTT wildcards. */
typedef struct {
    bool enabled;
    bool mqtt5;                            /* false = MQTT 3.1.1, true = MQTT 5.0 */
    bool tls_skip;                         /* false = verify against built-in cert bundle */
    char broker_uri[MQTT_CFG_BROKER_LEN];  /* empty = MQTT disabled */
    char username[MQTT_CFG_STR_LEN];       /* empty = anonymous */
    char password[MQTT_CFG_STR_LEN];       /* empty = no password */
    char client_id[MQTT_CFG_STR_LEN];      /* empty = auto from MAC */
    char topic_cmd[MQTT_CFG_STR_LEN];
    char topic_rsp[MQTT_CFG_STR_LEN];
    char topic_status[MQTT_CFG_STR_LEN];   /* also used for LWT */
    char topic_frame[MQTT_CFG_STR_LEN];
    int qos;                               /* 0..2 */
    bool publish_frames;
    bool publish_status;
} mqtt_web_config_t;

/**
 * Initialize the MQTT client.
 *
 * Loads the web-editable configuration from NVS (menuconfig values as
 * defaults), creates the esp-mqtt client and hooks it to the WiFi/IP events
 * so it connects whenever the station interface is up and stops when it goes
 * down. In SoftAP-only mode the client is created but never started (no route
 * to the broker).
 * Must be called after nvs_flash_init() and wifi_init().
 *
 * Returns ESP_OK even when MQTT is disabled by menuconfig / the enabled flag
 * / an empty broker URI (the client is simply left inactive).
 */
esp_err_t mqtt_init(void);

/* Load/save the web-editable MQTT configuration (NVS, namespace "ir_tool"). */
esp_err_t mqtt_web_config_load(mqtt_web_config_t *cfg);
esp_err_t mqtt_web_config_save(const mqtt_web_config_t *cfg);

/* MQTT configuration cores for the WebSocket RPC dispatcher. */
char *web_mqttcfg_get_json(void);                    /* caller frees */
esp_err_t web_mqttcfg_set(cJSON *root, const char **err); /* schedules restart on success */

#ifdef __cplusplus
}
#endif
