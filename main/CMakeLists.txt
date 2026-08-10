idf_component_register(
    SRCS "app_main.c"
         "app_ir.c" "app_ir_store.c" "app_ir_nec.c" "app_ir_play.c"
         "app_wifi.c"
         "app_mqtt.c"
         "app_web.c" "app_web_util.c" "app_web_auth.c"
         "app_web_api_ir.c" "app_web_api_wifi.c" "app_web_rpc.c" "app_web_ws.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES esp_driver_rmt esp_driver_gpio freertos esp_timer nvs_flash esp_wifi esp_netif esp_event esp_http_server cjson mqtt mbedtls
)
