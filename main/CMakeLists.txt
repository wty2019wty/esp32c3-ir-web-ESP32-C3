idf_component_register(
    SRCS "app_main.c" "app_ir.c" "app_wifi.c" "app_web.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES esp_driver_rmt esp_driver_gpio freertos esp_timer nvs_flash esp_wifi esp_netif esp_event esp_http_server cjson
)
