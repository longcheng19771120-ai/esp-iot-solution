/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include "light_effect.h"

/*
 * Mood color map: one main and one accent color per corner of the
 * valence/energy plane, blended bilinearly for everything in between.
 */
typedef struct {
    rgbw_t main, accent;
} corner_t;

static const corner_t s_sad = {      /* negative, calm: deep blue and teal */
    {0.10f, 0.20f, 1.00f, 0.00f}, {0.00f, 0.50f, 0.70f, 0.05f},
};
static const corner_t s_calm = {     /* positive, calm: warm white and amber */
    {1.00f, 0.60f, 0.25f, 0.80f}, {1.00f, 0.45f, 0.10f, 0.40f},
};
static const corner_t s_tense = {    /* negative, energetic: red and violet */
    {1.00f, 0.00f, 0.15f, 0.00f}, {0.60f, 0.00f, 1.00f, 0.00f},
};
static const corner_t s_happy = {    /* positive, energetic: gold and pink */
    {1.00f, 0.55f, 0.00f, 0.20f}, {1.00f, 0.15f, 0.55f, 0.00f},
};
static const rgbw_t s_idle = {1.00f, 0.55f, 0.20f, 0.60f};

static const char *const s_mode_names[LIGHT_MODE_MAX] = {"music", "static", "off"};

const char *light_mode_name(light_mode_t mode)
{
    return mode < LIGHT_MODE_MAX ? s_mode_names[mode] : "unknown";
}

static float clamp01(float x)
{
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static rgbw_t mix(const rgbw_t *a, const rgbw_t *b, float t)
{
    return (rgbw_t) {
        lerp(a->r, b->r, t), lerp(a->g, b->g, t), lerp(a->b, b->b, t), lerp(a->w, b->w, t)
    };
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

rgbw_t light_effect_mood_color(float valence, float energy, bool accent)
{
    valence = clamp01(valence);
    energy = clamp01(energy);
    rgbw_t low_neg = accent ? s_sad.accent : s_sad.main;
    rgbw_t low_pos = accent ? s_calm.accent : s_calm.main;
    rgbw_t high_neg = accent ? s_tense.accent : s_tense.main;
    rgbw_t high_pos = accent ? s_happy.accent : s_happy.main;
    rgbw_t low = mix(&low_neg, &low_pos, valence);
    rgbw_t high = mix(&high_neg, &high_pos, valence);
    return mix(&low, &high, energy);
}

void light_effect_init(light_effect_t *fx, float hop_s)
{
    memset(fx, 0, sizeof(*fx));
    fx->mode = LIGHT_MODE_MUSIC;
    fx->brightness = 1.0f;
    fx->hop_s = hop_s;
    fx->static_color = (rgbw_t) {
        0, 0, 0, 1.0f
    };
    fx->target = (mood_t) {
        .silent = true, .valence = 0.5f, .energy = 0
    };
    fx->valence = 0.5f;
    fx->silence = 1.0f;
    fx->color = s_idle;
}

void light_effect_set_mood(light_effect_t *fx, const mood_t *mood)
{
    fx->target = *mood;
}

void light_effect_on_frame(light_effect_t *fx, const music_frame_t *frame)
{
    /* Fast attack, slower release on loudness */
    float tau = frame->level > fx->level_env ? 0.03f : 0.25f;
    fx->level_env = approach(fx->level_env, frame->level, fx->hop_s, tau);

    if (frame->beat) {
        fx->pulse = fmaxf(fx->pulse, 0.5f + 0.5f * frame->beat_strength);
        /* Swap main and accent every 1 beat when energetic, up to every 8 when calm */
        int beats_per_step = 1 + (int)lroundf((1.0f - fx->energy) * 7.0f);
        if (++fx->beat_count >= beats_per_step) {
            fx->beat_count = 0;
            fx->accent = !fx->accent;
            fx->step_timer = 0;
        }
    }
}

void light_effect_render(light_effect_t *fx, float dt, rgbw_t *out)
{
    rgbw_t target = {0};

    /* Mood moves slowly so colors drift instead of jumping between windows */
    fx->valence = approach(fx->valence, fx->target.valence, dt, 4.0f);
    fx->energy = approach(fx->energy, fx->target.silent ? 0 : fx->target.energy, dt, 3.0f);
    fx->silence = approach(fx->silence, fx->target.silent ? 1.0f : 0.0f, dt, 1.5f);

    switch (fx->mode) {
    case LIGHT_MODE_MUSIC: {
        const float e = fx->energy;
        /* Calm music still changes color, on a timer instead of beats */
        fx->step_timer += dt;
        if (fx->step_timer >= lerp(8.0f, 2.0f, e)) {
            fx->accent = !fx->accent;
            fx->step_timer = 0;
        }
        rgbw_t mood_color = light_effect_mood_color(fx->valence, e, fx->accent);
        rgbw_t color = mix(&mood_color, &s_idle, fx->silence);
        approach_rgbw(&fx->color, &color, dt, lerp(2.5f, 0.1f, e));
        fx->pulse *= expf(-dt / 0.12f);

        /* Energetic music: lower floor, bigger beat flashes */
        float music = lerp(0.3f, 0.15f, e) + lerp(0.3f, 0.4f, e) * fx->level_env + lerp(0.05f, 0.6f, e) * fx->pulse;
        fx->phase += dt;
        float idle = 0.08f * (0.6f + 0.4f * sinf(fx->phase * 6.2832f / 6.0f));
        float intensity = clamp01(lerp(music, idle, fx->silence)) * fx->brightness;

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
