# light_sync_proto.h is shared with the gateway
idf_component_register(SRCS "sync_light.c"
                       INCLUDE_DIRS "." "../../main"
                       PRIV_REQUIRES bt nvs_flash esp_driver_ledc esp_driver_gpio esp_timer)
