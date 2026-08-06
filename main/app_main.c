#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "app_ir.h"
#include "app_wifi.h"
#include "app_web.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C3 IR web monitor starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = ir_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IR init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "IR capture ready");

    err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return;
    }

    err = web_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Ready. Open the web UI (see WiFi mode/IP in logs).");
}
