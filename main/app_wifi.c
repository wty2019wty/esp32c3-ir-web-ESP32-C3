#include <string.h>
#include <stdio.h>
#include "app_wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "wifi"

#define AP_CHANNEL     CONFIG_IR_TOOL_WIFI_AP_CHANNEL
#define AP_MAX_CONN    CONFIG_IR_TOOL_WIFI_AP_MAX_CONN
#define STA_TIMEOUT_MS CONFIG_IR_TOOL_WIFI_STA_TIMEOUT_MS
#define NVS_NS         "ir_tool"

static wifi_role_t s_role;
static wifi_mode_t s_actual_mode;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static volatile bool s_sta_connected = false;
static bool s_fell_back = false;
static char s_sta_ip[16] = "";

/* Web-editable configuration, loaded once at boot */
static wifi_web_config_t s_web_cfg;

static bool role_wants_sta(void)
{
    return s_role != WIFI_ROLE_AP && s_web_cfg.sta_ssid[0] != '\0';
}

/* ---------------- NVS config load / save ---------------- */

esp_err_t wifi_web_config_load(wifi_web_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->ap_ssid, CONFIG_IR_TOOL_WIFI_AP_SSID, sizeof(cfg->ap_ssid));
    strlcpy(cfg->ap_password, CONFIG_IR_TOOL_WIFI_AP_PASSWORD, sizeof(cfg->ap_password));
    strlcpy(cfg->sta_ssid, CONFIG_IR_TOOL_WIFI_STA_SSID, sizeof(cfg->sta_ssid));
    strlcpy(cfg->sta_password, CONFIG_IR_TOOL_WIFI_STA_PASSWORD, sizeof(cfg->sta_password));
    cfg->sta_dhcp = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK; /* defaults only */
    }
    size_t len;
    len = sizeof(cfg->ap_ssid);
    if (nvs_get_str(h, "ap_ssid", cfg->ap_ssid, &len) != ESP_OK) {
        strlcpy(cfg->ap_ssid, CONFIG_IR_TOOL_WIFI_AP_SSID, sizeof(cfg->ap_ssid));
    }
    len = sizeof(cfg->ap_password);
    if (nvs_get_str(h, "ap_pwd", cfg->ap_password, &len) != ESP_OK) {
        strlcpy(cfg->ap_password, CONFIG_IR_TOOL_WIFI_AP_PASSWORD, sizeof(cfg->ap_password));
    }
    len = sizeof(cfg->sta_ssid);
    if (nvs_get_str(h, "sta_ssid", cfg->sta_ssid, &len) != ESP_OK) {
        strlcpy(cfg->sta_ssid, CONFIG_IR_TOOL_WIFI_STA_SSID, sizeof(cfg->sta_ssid));
    }
    len = sizeof(cfg->sta_password);
    if (nvs_get_str(h, "sta_pwd", cfg->sta_password, &len) != ESP_OK) {
        strlcpy(cfg->sta_password, CONFIG_IR_TOOL_WIFI_STA_PASSWORD, sizeof(cfg->sta_password));
    }
    uint8_t v8 = 1;
    if (nvs_get_u8(h, "sta_dhcp", &v8) == ESP_OK) {
        cfg->sta_dhcp = v8 != 0;
    }
    nvs_get_u32(h, "sta_ip", &cfg->sta_ip);
    nvs_get_u32(h, "sta_gw", &cfg->sta_gw);
    nvs_get_u32(h, "sta_mask", &cfg->sta_mask);
    nvs_get_u32(h, "sta_dns", &cfg->sta_dns);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t wifi_web_config_save(const wifi_web_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open nvs");
    esp_err_t err = nvs_set_str(h, "ap_ssid", cfg->ap_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "ap_pwd", cfg->ap_password);
    if (err == ESP_OK) err = nvs_set_str(h, "sta_ssid", cfg->sta_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "sta_pwd", cfg->sta_password);
    if (err == ESP_OK) err = nvs_set_u8(h, "sta_dhcp", cfg->sta_dhcp ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(h, "sta_ip", cfg->sta_ip);
    if (err == ESP_OK) err = nvs_set_u32(h, "sta_gw", cfg->sta_gw);
    if (err == ESP_OK) err = nvs_set_u32(h, "sta_mask", cfg->sta_mask);
    if (err == ESP_OK) err = nvs_set_u32(h, "sta_dns", cfg->sta_dns);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------------- events ---------------- */

static void sta_apply_static_ip(void)
{
    if (s_web_cfg.sta_dhcp || !s_sta_netif) {
        return;
    }
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = s_web_cfg.sta_ip;
    ip.gw.addr = s_web_cfg.sta_gw;
    ip.netmask.addr = s_web_cfg.sta_mask ? s_web_cfg.sta_mask : 0x00FFFFFFU; /* 255.255.255.0 */
    esp_netif_set_ip_info(s_sta_netif, &ip);
    if (s_web_cfg.sta_dns) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = s_web_cfg.sta_dns;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ESP_LOGI(TAG, "STA static IP applied");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting to \"%s\"", s_web_cfg.sta_ssid);
            if (!s_web_cfg.sta_dhcp) {
                sta_apply_static_ip();
            }
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected");
            s_sta_connected = true;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "STA disconnected");
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            /* reconnect unless we fell back to AP-only mode */
            if (!s_fell_back && role_wants_sta()) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP started");
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "AP client joined: " MACSTR " (aid=%d)",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGW(TAG, "AP client left: " MACSTR " (aid=%d, reason=%d)",
                     MAC2STR(ev->mac), ev->aid, ev->reason);
            break;
        }
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
    strlcpy((char *)cfg.ap.ssid, s_web_cfg.ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)strlen((const char *)cfg.ap.ssid); /* strlcpy guarantees NUL, <= 32 */
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONN;
    if (s_web_cfg.ap_password[0] != '\0' && strlen(s_web_cfg.ap_password) >= 8) {
        strlcpy((char *)cfg.ap.password, s_web_cfg.ap_password, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (s_web_cfg.ap_password[0] != '\0') {
            ESP_LOGW(TAG, "AP password shorter than 8 chars, using open network");
        }
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_LOGI(TAG, "SoftAP \"%s\" (auth=%d, ch=%d, max=%d)",
             s_web_cfg.ap_ssid, cfg.ap.authmode, AP_CHANNEL, AP_MAX_CONN);
}

static void wifi_configure_sta(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_web_cfg.sta_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_web_cfg.sta_password, sizeof(cfg.sta.password));
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

    wifi_web_config_load(&s_web_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* AP netif is always created: in AP/APSTA roles it serves the hotspot;
     * in STA role it is required by the SoftAP fallback path (DHCP server
     * starts automatically when the AP interface comes up). */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_ERR_NO_MEM, TAG, "create AP netif");
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

    /* configure AP always: in STA role this prepares the SoftAP fallback */
    wifi_configure_ap();
    if (role_wants_sta()) {
        wifi_configure_sta();
    }

    wifi_mode_t mode;
    if (s_role == WIFI_ROLE_APSTA) {
        mode = WIFI_MODE_APSTA;
    } else if (s_role == WIFI_ROLE_STA) {
        mode = s_web_cfg.sta_ssid[0] != '\0' ? WIFI_MODE_STA : WIFI_MODE_AP;
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
    char ip[16];
    if (wifi_get_ap_ip(ip, sizeof(ip))) {
        ESP_LOGI(TAG, "SoftAP IP: %s (open http://%s/)", ip, ip);
    }
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
    return s_web_cfg.ap_ssid;
}

const char *wifi_sta_ssid(void)
{
    return s_web_cfg.sta_ssid;
}
