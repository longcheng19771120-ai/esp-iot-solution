idf_component_register(SRCS "app_main.c" "gateway_wifi.c" "gateway_mqtt.c" "gateway_uart.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash mqtt esp_driver_uart esp_app_format esp_timer)
