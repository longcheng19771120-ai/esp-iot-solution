idf_component_register(SRCS "app_main.c" "gateway_wifi.c" "gateway_mqtt.c" "audio_input.c" "light_rgbw.c"
                            "music_analyzer.c" "mood_estimator.c" "genre_classifier.c" "light_effect.c" "display_ui.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash mqtt esp_app_format esp_timer esp_driver_i2s
                                     esp_driver_ledc esp_driver_gpio esp_driver_spi esp_driver_i2c esp_lcd)
