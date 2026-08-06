#include <string.h>
#include <stdio.h>
#include "app_wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "wifi"

#define AP_SSID        CONFIG_IR_TOOL_WIFI_AP_SSID
#define AP_PASSWORD    CONFIG_IR_TOOL_WIFI_AP_PASSWORD
#define AP_CHANNEL     CONFIG_IR_TOOL_WIFI_AP_CHANNEL
#define AP_MAX_CONN    CONFIG_IR_TOOL_WIFI_AP_MAX_CONN
#define STA_SSID       CONFIG_IR_TOOL_WIFI_STA_SSID
#define STA_PASSWORD   CONFIG_IR_TOOL_WIFI_STA_PASSWORD
#define STA_TIMEOUT_MS CONFIG_IR_TOOL_WIFI_STA_TIMEOUT_MS

static wifi_role_t s_role;
static wifi_mode_t s_actual_mode;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static volatile bool s_sta_connected = false;
static bool s_fell_back = false;
static char s_sta_ip[16] = "";

static bool role_wants_ap(void)
{
    return s_role != WIFI_ROLE_STA;
}

static bool role_wants_sta(void)
{
    return s_role != WIFI_ROLE_AP && STA_SSID[0] != '\0';
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting to \"%s\"", STA_SSID);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected");
            s_sta_connected = true;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "STA disconnected");
            s_sta_connected = false;
            /* reconnect unless we fell back to AP-only mode */
            if (!s_fell_back && role_wants_sta()) {
                esp_wifi_connect();
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
        s_sta_connected = true;
    }
}

static void wifi_configure_ap(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.ap.ssid, AP_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONN;
    if (AP_PASSWORD[0] != '\0' && strlen(AP_PASSWORD) >= 8) {
        strlcpy((char *)cfg.ap.password, AP_PASSWORD, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (AP_PASSWORD[0] != '\0') {
            ESP_LOGW(TAG, "AP password shorter than 8 chars, using open network");
        }
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_LOGI(TAG, "SoftAP \"%s\" (auth=%d, ch=%d, max=%d)",
             AP_SSID, cfg.ap.authmode, AP_CHANNEL, AP_MAX_CONN);
}

static void wifi_configure_sta(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, STA_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, STA_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

esp_err_t wifi_init(void)
{
#if defined(CONFIG_IR_TOOL_WIFI_MODE_STA)
    s_role = WIFI_ROLE_STA;
#elif defined(CONFIG_IR_TOOL_WIFI_MODE_APSTA)
    s_role = WIFI_ROLE_APSTA;
#else
    s_role = WIFI_ROLE_AP;
#endif

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (role_wants_ap()) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_ap_netif, ESP_ERR_NO_MEM, TAG, "create AP netif");
    }
    if (role_wants_sta()) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif, ESP_ERR_NO_MEM, TAG, "create STA netif");
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "set storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG, "reg wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG, "reg ip events");

    if (role_wants_ap()) {
        wifi_configure_ap();
    }
    if (role_wants_sta()) {
        wifi_configure_sta();
    }

    wifi_mode_t mode;
    if (s_role == WIFI_ROLE_APSTA) {
        mode = WIFI_MODE_APSTA;
    } else if (s_role == WIFI_ROLE_STA) {
        mode = STA_SSID[0] != '\0' ? WIFI_MODE_STA : WIFI_MODE_AP;
    } else {
        mode = WIFI_MODE_AP;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    s_actual_mode = mode;

    /* STA-only role: wait for the connection, then fall back to AP on timeout */
    if (mode == WIFI_MODE_STA) {
        ESP_LOGI(TAG, "Waiting for STA connection (%lu ms max)...", (unsigned long)STA_TIMEOUT_MS);
        uint32_t waited = 0;
        while (!s_sta_connected && waited < STA_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        if (!s_sta_connected) {
            ESP_LOGW(TAG, "STA connect timeout, falling back to SoftAP");
            s_fell_back = true;
            ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "wifi stop for fallback");
            ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start AP");
            s_actual_mode = WIFI_MODE_AP;
        }
    }

    ESP_LOGI(TAG, "WiFi ready, actual mode: %s", wifi_mode_str());
    return ESP_OK;
}

wifi_role_t wifi_get_role(void)
{
    return s_role;
}

const char *wifi_mode_str(void)
{
    switch (s_actual_mode) {
    case WIFI_MODE_AP:     return "AP";
    case WIFI_MODE_STA:    return "STA";
    case WIFI_MODE_APSTA:  return "AP+STA";
    default:               return "?";
    }
}

bool wifi_is_sta_connected(void)
{
    return s_sta_connected;
}

bool wifi_get_ap_ip(char *buf, size_t len)
{
    if (!s_ap_netif) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_ap_netif, &info) != ESP_OK) {
        return false;
    }
    snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    return true;
}

bool wifi_get_sta_ip(char *buf, size_t len)
{
    if (!s_sta_netif) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    return true;
}

const char *wifi_ap_ssid(void)
{
    return AP_SSID;
}

const char *wifi_sta_ssid(void)
{
    return STA_SSID;
}
