/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Gateway link state, used to drive the status LED
 */
typedef enum {
    GATEWAY_STATE_WIFI_CONNECTING = 0,  /*!< Waiting for Wi-Fi / IP */
    GATEWAY_STATE_MQTT_CONNECTING,      /*!< Got IP, waiting for broker */
    GATEWAY_STATE_ONLINE,               /*!< Connected to broker */
    GATEWAY_STATE_MAX,
} gateway_state_t;

/** Implemented in app_main.c */
void gateway_set_state(gateway_state_t state);

/* Wi-Fi (gateway_wifi.c) */
esp_err_t gateway_wifi_start(void);
bool gateway_wifi_is_connected(void);
/** Gateway id derived from the station MAC, e.g. "a0b1c2d3e4f5" */
const char *gateway_get_id(void);

/* MQTT (gateway_mqtt.c) */
esp_err_t gateway_mqtt_start(void);
bool gateway_mqtt_is_connected(void);
/** Publish a frame received from a sub-device to <prefix>/<id>/up */
esp_err_t gateway_mqtt_publish_uplink(const char *data, size_t len);
/** Publish a gateway event (e.g. button) to <prefix>/<id>/event */
esp_err_t gateway_mqtt_publish_event(const char *event);

/* Sub-device UART (gateway_uart.c) */
esp_err_t gateway_uart_start(void);
/** Write a downlink frame to the sub-device bus, terminated with '\n' */
esp_err_t gateway_uart_send(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
