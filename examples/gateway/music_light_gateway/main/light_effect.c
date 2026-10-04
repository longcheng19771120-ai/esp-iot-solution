/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include "light_effect.h"

#define PALETTE_MAX 4

typedef struct {
    rgbw_t palette[PALETTE_MAX];
    int palette_len;
    int beats_per_step;     /* advance the palette every N beats, 0 = time based */
    float seconds_per_step; /* used when beats_per_step is 0 or no beats arrive */
    float fade_s;           /* color crossfade time constant */
    float base;             /* brightness floor */
    float level_gain;       /* brightness from loudness */
    float pulse_gain;       /* brightness flash on each beat */
} genre_style_t;

static const genre_style_t s_styles[MUSIC_GENRE_MAX] = {
    [MUSIC_GENRE_SILENCE] = {
        .palette = {{1.0f, 0.55f, 0.2f, 0.6f}}, .palette_len = 1,
        .seconds_per_step = 10, .fade_s = 2.0f, .base = 0.08f,
    },
    [MUSIC_GENRE_AMBIENT] = {
        .palette = {{0.1f, 0.3f, 1.0f, 0}, {0.45f, 0.15f, 1.0f, 0}, {0.0f, 0.7f, 0.8f, 0.1f}}, .palette_len = 3,
        .seconds_per_step = 8, .fade_s = 3.0f, .base = 0.3f, .level_gain = 0.2f,
    },
    [MUSIC_GENRE_CLASSICAL] = {
        .palette = {{1.0f, 0.6f, 0.25f, 0.8f}, {1.0f, 0.45f, 0.1f, 0.4f}, {0.9f, 0.75f, 0.5f, 1.0f}}, .palette_len = 3,
        .seconds_per_step = 6, .fade_s = 2.5f, .base = 0.25f, .level_gain = 0.55f,
    },
    [MUSIC_GENRE_POP] = {
        .palette = {{1.0f, 0.2f, 0.6f, 0}, {0.1f, 0.8f, 1.0f, 0}, {1.0f, 0.85f, 0.1f, 0}, {0.6f, 0.2f, 1.0f, 0}},
        .palette_len = 4, .beats_per_step = 2, .seconds_per_step = 3, .fade_s = 0.25f,
        .base = 0.25f, .level_gain = 0.4f, .pulse_gain = 0.3f,
    },
    [MUSIC_GENRE_ROCK] = {
        .palette = {{1.0f, 0.05f, 0.0f, 0}, {1.0f, 0.4f, 0.0f, 0}, {1.0f, 0.3f, 0.2f, 0.7f}}, .palette_len = 3,
        .beats_per_step = 4, .seconds_per_step = 3, .fade_s = 0.15f,
        .base = 0.2f, .level_gain = 0.4f, .pulse_gain = 0.5f,
    },
    [MUSIC_GENRE_ELECTRONIC] = {
        .palette = {{0.0f, 0.9f, 1.0f, 0}, {1.0f, 0.0f, 0.9f, 0}, {0.1f, 0.2f, 1.0f, 0}, {0.2f, 1.0f, 0.3f, 0}},
        .palette_len = 4, .beats_per_step = 1, .seconds_per_step = 2, .fade_s = 0.08f,
        .base = 0.15f, .level_gain = 0.35f, .pulse_gain = 0.6f,
    },
    [MUSIC_GENRE_HIPHOP] = {
        .palette = {{0.55f, 0.0f, 1.0f, 0}, {1.0f, 0.65f, 0.0f, 0.2f}, {1.0f, 0.0f, 0.25f, 0}}, .palette_len = 3,
        .beats_per_step = 2, .seconds_per_step = 3, .fade_s = 0.2f,
        .base = 0.2f, .level_gain = 0.35f, .pulse_gain = 0.5f,
    },
};

static const char *const s_mode_names[LIGHT_MODE_MAX] = {"music", "static", "off"};

const char *light_mode_name(light_mode_t mode)
{
    return mode < LIGHT_MODE_MAX ? s_mode_names[mode] : "unknown";
}

static float clamp01(float x)
{
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

/* First order low-pass step toward target with time constant tau */
static float approach(float cur, float target, float dt, float tau)
{
    return tau <= 0 ? target : cur + (target - cur) * (1.0f - expf(-dt / tau));
}

static void approach_rgbw(rgbw_t *cur, const rgbw_t *target, float dt, float tau)
{
    cur->r = approach(cur->r, target->r, dt, tau);
    cur->g = approach(cur->g, target->g, dt, tau);
    cur->b = approach(cur->b, target->b, dt, tau);
    cur->w = approach(cur->w, target->w, dt, tau);
}

void light_effect_init(light_effect_t *fx, float hop_s)
{
    memset(fx, 0, sizeof(*fx));
    fx->hop_s = hop_s;
    fx->mode = LIGHT_MODE_MUSIC;
    fx->brightness = 1.0f;
    fx->static_color = (rgbw_t) {
        0, 0, 0, 1.0f
    };
    fx->genre = MUSIC_GENRE_SILENCE;
    fx->color = s_styles[MUSIC_GENRE_SILENCE].palette[0];
}

void light_effect_set_genre(light_effect_t *fx, music_genre_t genre)
{
    if (genre >= MUSIC_GENRE_MAX || genre == fx->genre) {
        return;
    }
    fx->genre = genre;
    fx->palette_idx = 0;
    fx->beat_count = 0;
    fx->palette_timer = 0;
}

static void step_palette(light_effect_t *fx, const genre_style_t *st)
{
    fx->palette_idx = (fx->palette_idx + 1) % st->palette_len;
    fx->palette_timer = 0;
}

void light_effect_on_frame(light_effect_t *fx, const music_frame_t *frame)
{
    const genre_style_t *st = &s_styles[fx->genre];

    /* Fast attack, slower release on loudness */
    float tau = frame->level > fx->level_env ? 0.03f : 0.25f;
    fx->level_env = approach(fx->level_env, frame->level, fx->hop_s, tau);

    if (frame->beat) {
        fx->pulse = fmaxf(fx->pulse, 0.5f + 0.5f * frame->beat_strength);
        if (st->beats_per_step > 0 && ++fx->beat_count >= st->beats_per_step) {
            fx->beat_count = 0;
            step_palette(fx, st);
        }
    }
}

void light_effect_render(light_effect_t *fx, float dt, rgbw_t *out)
{
    const genre_style_t *st = &s_styles[fx->genre];
    rgbw_t target = {0};

    switch (fx->mode) {
    case LIGHT_MODE_MUSIC: {
        /* Time based steps for beatless styles, and as a fallback if beats stop */
        fx->palette_timer += dt;
        if (fx->palette_timer >= st->seconds_per_step) {
            step_palette(fx, st);
        }
        approach_rgbw(&fx->color, &st->palette[fx->palette_idx], dt, st->fade_s);
        fx->pulse *= expf(-dt / 0.12f);

        float intensity;
        if (fx->genre == MUSIC_GENRE_SILENCE) {
            fx->phase += dt;
            intensity = st->base * (0.6f + 0.4f * sinf(fx->phase * 6.2832f / 6.0f));
        } else {
            intensity = st->base + st->level_gain * fx->level_env + st->pulse_gain * fx->pulse;
        }
        intensity = clamp01(intensity) * fx->brightness;
        target.r = fx->color.r * intensity;
        target.g = fx->color.g * intensity;
        target.b = fx->color.b * intensity;
        target.w = fx->color.w * intensity;
        break;
    }
    case LIGHT_MODE_STATIC:
        target.r = fx->static_color.r * fx->brightness;
        target.g = fx->static_color.g * fx->brightness;
        target.b = fx->static_color.b * fx->brightness;
        target.w = fx->static_color.w * fx->brightness;
        break;
    default:
        break;
    }

    /* Short output smoothing removes steps without blunting beat flashes */
    approach_rgbw(&fx->out, &target, dt, 0.02f);
    out->r = clamp01(fx->out.r);
    out->g = clamp01(fx->out.g);
    out->b = clamp01(fx->out.b);
    out->w = clamp01(fx->out.w);
}
