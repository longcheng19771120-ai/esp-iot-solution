/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "light_effect.h"

#define PALETTE_MAX 4

/*
 * Each genre has its own palette and timing. Mood adjusts them: valence tints
 * the palette warmer or cooler, energy speeds up or slows down the base timing
 * and scales the beat flashes. At valence and energy 0.5 the genre style is
 * used unchanged.
 */
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

/*
 * Happy music turns warm hues a little toward gold and adds warm white; sad music
 * turns cool hues toward deep blue, mutes warm ones and removes white. Each hue only
 * moves within its own half of the color wheel, so orange never passes through red
 * on its way to blue, and the shift is capped so every genre keeps its own colors.
 */
#define WARM_HUE    40.0f   /* degrees */
#define COOL_HUE    230.0f
#define MAX_SHIFT   25.0f   /* hue rotation at valence 0 or 1, degrees */
#define WHITE_TINT  0.25f   /* warm white added or removed at valence 1 or 0 */

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

static void rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) {
        *h = 0;
    } else if (mx == r) {
        *h = 60.0f * fmodf((g - b) / d + 6.0f, 6.0f);
    } else if (mx == g) {
        *h = 60.0f * ((b - r) / d + 2.0f);
    } else {
        *h = 60.0f * ((r - g) / d + 4.0f);
    }
}

static void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b)
{
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    int sector = (int)(h / 60.0f) % 6;
    const float tab[6][3] = {{c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x}};
    *r = tab[sector][0] + m;
    *g = tab[sector][1] + m;
    *b = tab[sector][2] + m;
}

/* Move hue h toward target along the shorter way round, by at most max_deg */
static float hue_toward(float h, float target, float max_deg)
{
    float d = fmodf(target - h + 540.0f, 360.0f) - 180.0f;
    d = fmaxf(-max_deg, fminf(max_deg, d));
    return fmodf(h + d + 360.0f, 360.0f);
}

rgbw_t light_effect_color(music_genre_t genre, int palette_idx, float valence)
{
    const genre_style_t *st = &s_styles[genre < MUSIC_GENRE_MAX ? genre : MUSIC_GENRE_SILENCE];
    rgbw_t c = st->palette[palette_idx % st->palette_len];
    float t = (clamp01(valence) - 0.5f) * 2.0f;
    float h, s, v;

    rgb_to_hsv(c.r, c.g, c.b, &h, &s, &v);
    /* Warm half: magenta through red and yellow to green */
    bool warm = h >= 300.0f || h < 120.0f;
    if (s > 0.05f && t > 0 && warm) {
        h = hue_toward(h, WARM_HUE, MAX_SHIFT * t);
    } else if (s > 0.05f && t < 0 && !warm) {
        h = hue_toward(h, COOL_HUE, MAX_SHIFT * -t);
    } else if (t < 0) {
        s *= 1.0f + 0.4f * t;
    }
    hsv_to_rgb(h, s, v, &c.r, &c.g, &c.b);
    c.w = t >= 0 ? fminf(1.0f, c.w + WHITE_TINT * t) : c.w * (1.0f + t);
    return c;
}

/* Energy 0..1 to a multiplier around 1: calm music slows the genre timing down, energetic speeds it up */
static float tempo_scale(float energy)
{
    return lerp(1.5f, 0.67f, clamp01(energy));
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
    fx->genre = MUSIC_GENRE_SILENCE;
    fx->valence = 0.5f;
    fx->energy = 0.5f;
    fx->silence = 1.0f;
    fx->color = s_idle;
}

void light_effect_set_genre(light_effect_t *fx, music_genre_t genre)
{
    if (genre >= MUSIC_GENRE_MAX || genre == fx->genre) {
        return;
    }
    fx->genre = genre;
    fx->palette_idx = 0;
    fx->beat_count = 0;
    fx->step_timer = 0;
}

static void step_palette(light_effect_t *fx)
{
    fx->palette_idx = (fx->palette_idx + 1) % s_styles[fx->genre].palette_len;
    fx->beat_count = 0;
    fx->step_timer = 0;
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
        int per_step = s_styles[fx->genre].beats_per_step;
        if (per_step > 0) {
            per_step = (int)fmaxf(1.0f, lroundf(per_step * tempo_scale(fx->energy)));
            if (++fx->beat_count >= per_step) {
                step_palette(fx);
            }
        }
    }
}

void light_effect_render(light_effect_t *fx, float dt, rgbw_t *out)
{
    rgbw_t target = {0};

    /* Mood moves slowly so colors drift instead of jumping between windows */
    fx->valence = approach(fx->valence, fx->target.valence, dt, 4.0f);
    fx->energy = approach(fx->energy, fx->target.silent ? 0.5f : fx->target.energy, dt, 3.0f);
    fx->silence = approach(fx->silence, fx->target.silent ? 1.0f : 0.0f, dt, 1.5f);

    switch (fx->mode) {
    case LIGHT_MODE_MUSIC: {
        const genre_style_t *st = &s_styles[fx->genre];
        const float k = tempo_scale(fx->energy);
        /* Time based steps for beatless genres, and as a fallback if beats stop */
        fx->step_timer += dt;
        if (fx->step_timer >= st->seconds_per_step * k) {
            step_palette(fx);
        }
        rgbw_t palette_color = light_effect_color(fx->genre, fx->palette_idx, fx->valence);
        rgbw_t color = mix(&palette_color, &s_idle, fx->silence);
        approach_rgbw(&fx->color, &color, dt, st->fade_s * k);
        fx->pulse *= expf(-dt / 0.12f);

        /* Energetic music gets bigger beat flashes than the genre's default */
        float music = st->base + st->level_gain * fx->level_env + st->pulse_gain / k * fx->pulse;
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
