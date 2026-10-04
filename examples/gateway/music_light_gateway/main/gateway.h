/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "music_analyzer.h"
#include "genre_classifier.h"
#include "light_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wi-Fi (gateway_wifi.c) */
esp_err_t gateway_wifi_start(void);
bool gateway_wifi_is_connected(void);
/** Gateway id derived from the station MAC, e.g. "a0b1c2d3e4f5" */
const char *gateway_get_id(void);

/* MQTT (gateway_mqtt.c) */
esp_err_t gateway_mqtt_start(void);
bool gateway_mqtt_is_connected(void);
/** Publish the current music analysis to <prefix>/<id>/music */
esp_err_t gateway_mqtt_publish_music(music_genre_t genre, float confidence, const music_features_t *f);
/** Publish the light state to <prefix>/<id>/light/state (retained) */
esp_err_t gateway_mqtt_publish_light_state(void);

/* Microphone and analysis (audio_input.c) */
esp_err_t audio_input_start(void);

/* RGBW output (light_rgbw.c), all functions are thread safe */
esp_err_t light_rgbw_start(void);
void light_on_frame(const music_frame_t *frame);
void light_set_genre(music_genre_t genre);
void light_set_mode(light_mode_t mode);
void light_set_color(rgbw_t color);
void light_set_brightness(float brightness);
/** Cycle music -> static -> off */
void light_next_mode(void);
/** Format the light state as JSON, returns the length */
int light_get_state_json(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
