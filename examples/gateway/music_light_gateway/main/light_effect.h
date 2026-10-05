/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "music_analyzer.h"
#include "mood_estimator.h"
#include "genre_classifier.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform independent RGBW effect engine, values are linear 0..1 */

typedef struct {
    float r, g, b, w;
} rgbw_t;

typedef enum {
    LIGHT_MODE_MUSIC = 0,   /*!< Genre picks the palette, mood tints it, energy sets the dynamics */
    LIGHT_MODE_STATIC,      /*!< Fixed color */
    LIGHT_MODE_OFF,
    LIGHT_MODE_MAX,
} light_mode_t;

typedef enum {
    LIGHT_THEME_VIVID = 0,  /*!< Saturated modern colors */
    LIGHT_THEME_SONG,       /*!< Muted Chinese traditional colors in the Song dynasty style */
    LIGHT_THEME_MAX,
} light_theme_t;

typedef struct {
    light_mode_t mode;
    light_theme_t theme;
    rgbw_t static_color;
    float brightness;       /* user brightness 0..1 */
    float hop_s;            /* duration of one analyzer hop */

    music_genre_t genre;    /* picks the palette and base timing */
    int palette_idx;
    mood_t target;          /* latest estimate */
    float valence;          /* smoothed toward target, tints the palette */
    float energy;           /* smoothed toward target, drives dynamics */
    float silence;          /* 0..1 blend toward the idle effect */
    int beat_count;
    float step_timer;       /* seconds since the last palette step */
    float level_env;        /* smoothed loudness */
    float pulse;            /* beat flash, decays */
    float phase;            /* idle breathing phase */
    rgbw_t color;           /* current color, crossfades toward the target */
    rgbw_t out;             /* smoothed output */
} light_effect_t;

/** hop_s: duration of one analyzer hop, MA_HOP_SIZE / sample_rate */
void light_effect_init(light_effect_t *fx, float hop_s);
void light_effect_set_genre(light_effect_t *fx, music_genre_t genre);
void light_effect_set_theme(light_effect_t *fx, light_theme_t theme);
void light_effect_set_mood(light_effect_t *fx, const mood_t *mood);
/** Feed one analyzer hop */
void light_effect_on_frame(light_effect_t *fx, const music_frame_t *frame);
/** Advance by dt seconds and compute the output */
void light_effect_render(light_effect_t *fx, float dt, rgbw_t *out);
/** Palette color of a genre in a theme, tinted by valence, before brightness */
rgbw_t light_effect_color(light_theme_t theme, music_genre_t genre, int palette_idx, float valence);

const char *light_mode_name(light_mode_t mode);
const char *light_theme_name(light_theme_t theme);

#ifdef __cplusplus
}
#endif
