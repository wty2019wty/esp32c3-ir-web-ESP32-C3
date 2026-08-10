#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the MQTT client.
 *
 * Reads menuconfig options (broker URI, credentials, topics), creates the
 * esp-mqtt client and hooks it to the WiFi/IP events so it connects whenever
 * the station interface is up and stops when it goes down. In SoftAP-only
 * mode the client is created but never started (no route to the broker).
 * Must be called after nvs_flash_init() and wifi_init().
 *
 * Returns ESP_OK even when MQTT is disabled by menuconfig or the broker URI
 * is empty (the client is simply left inactive).
 */
esp_err_t mqtt_init(void);

#ifdef __cplusplus
}
#endif
