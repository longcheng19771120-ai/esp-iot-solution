idf_component_register(SRCS "bt_receiver.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_driver_i2s esp_driver_uart esp_ringbuf)
