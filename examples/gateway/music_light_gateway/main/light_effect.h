/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "music_analyzer.h"
#include "mood_estimator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform independent RGBW effect engine, values are linear 0..1 */

typedef struct {
    float r, g, b, w;
} rgbw_t;

typedef enum {
    LIGHT_MODE_MUSIC = 0,   /*!< Color follows mood, brightness follows energy, level and beats */
    LIGHT_MODE_STATIC,      /*!< Fixed color */
    LIGHT_MODE_OFF,
    LIGHT_MODE_MAX,
} light_mode_t;

typedef struct {
    light_mode_t mode;
    rgbw_t static_color;
    float brightness;       /* user brightness 0..1 */
    float hop_s;            /* duration of one analyzer hop */

    mood_t target;          /* latest estimate */
    float valence;          /* smoothed toward target, drives the color */
    float energy;           /* smoothed toward target, drives dynamics */
    float silence;          /* 0..1 blend toward the idle effect */
    bool accent;            /* showing the accent color instead of the main one */
    int beat_count;
    float step_timer;       /* seconds since the last main/accent swap */
    float level_env;        /* smoothed loudness */
    float pulse;            /* beat flash, decays */
    float phase;            /* idle breathing phase */
    rgbw_t color;           /* current color, crossfades toward the target */
    rgbw_t out;             /* smoothed output */
} light_effect_t;

/** hop_s: duration of one analyzer hop, MA_HOP_SIZE / sample_rate */
void light_effect_init(light_effect_t *fx, float hop_s);
void light_effect_set_mood(light_effect_t *fx, const mood_t *mood);
/** Feed one analyzer hop */
void light_effect_on_frame(light_effect_t *fx, const music_frame_t *frame);
/** Advance by dt seconds and compute the output */
void light_effect_render(light_effect_t *fx, float dt, rgbw_t *out);
/** Main color for a mood, before brightness */
rgbw_t light_effect_mood_color(float valence, float energy, bool accent);

const char *light_mode_name(light_mode_t mode);

#ifdef __cplusplus
}
#endif
