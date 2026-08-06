#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_ir.h"
#include "app_wifi.h"
#include "app_web.h"

static const char *TAG = "app";

/* BOOT button (GPIO9) held for 50ms within the first 2s after boot
 * triggers a factory reset: erase NVS and restart, which falls back to
 * the default open SoftAP (menuconfig defaults, empty password).
 * Note: holding BOOT at power-on enters the ROM download mode instead. */
#define BOOT_RESET_GPIO         9
#define BOOT_RESET_WINDOW_MS    2000
#define BOOT_RESET_DEBOUNCE_MS  50
#define BOOT_RESET_POLL_MS      10

static void boot_reset_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    uint32_t low_ms = 0;
    for (uint32_t elapsed = 0; elapsed < BOOT_RESET_WINDOW_MS; elapsed += BOOT_RESET_POLL_MS) {
        if (gpio_get_level(BOOT_RESET_GPIO) == 0) {
            low_ms += BOOT_RESET_POLL_MS;
        } else {
            low_ms = 0;
        }
        if (low_ms >= BOOT_RESET_DEBOUNCE_MS) {
            ESP_LOGW(TAG, "BOOT held during startup window -> factory reset");
            vTaskDelay(pdMS_TO_TICKS(100)); /* let boot logs flush */
            nvs_flash_erase();
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_RESET_POLL_MS));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp32c3-ir-web starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* BOOT-held factory reset watchdog (does not block normal startup) */
    if (xTaskCreate(boot_reset_task, "boot_reset", 2048, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create boot reset task");
    }

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
