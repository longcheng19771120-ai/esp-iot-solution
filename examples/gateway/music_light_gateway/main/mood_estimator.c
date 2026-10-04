/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include "mood_estimator.h"
#include "mood_model.h"

static const char *const s_names[MOOD_MAX] = {
    [MOOD_SILENCE] = "silence",
    [MOOD_CALM] = "calm",
    [MOOD_HAPPY] = "happy",
    [MOOD_TENSE] = "tense",
    [MOOD_SAD] = "sad",
};

const char *mood_quadrant_name(mood_quadrant_t q)
{
    return q < MOOD_MAX ? s_names[q] : "unknown";
}

mood_quadrant_t mood_quadrant(const mood_t *m)
{
    if (m->silent) {
        return MOOD_SILENCE;
    }
    if (m->valence >= 0.5f) {
        return m->energy >= 0.5f ? MOOD_HAPPY : MOOD_CALM;
    }
    return m->energy >= 0.5f ? MOOD_TENSE : MOOD_SAD;
}

bool mood_quadrant_target(const char *name, float *valence, float *energy)
{
    /* Targets sit inside each quadrant rather than at the corners */
    static const float targets[MOOD_MAX][2] = {
        [MOOD_CALM] = {0.75f, 0.25f},
        [MOOD_HAPPY] = {0.75f, 0.75f},
        [MOOD_TENSE] = {0.25f, 0.75f},
        [MOOD_SAD] = {0.25f, 0.25f},
    };
    for (int q = MOOD_CALM; q < MOOD_MAX; q++) {
        if (strcmp(name, s_names[q]) == 0) {
            *valence = targets[q][0];
            *energy = targets[q][1];
            return true;
        }
    }
    return false;
}

bool mood_model_trained(void)
{
    return MOOD_MODEL_TRAINED;
}

static float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

static float clamp01(float x)
{
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

/* Tempo estimates are often off by an octave: use a folded 90..180 scale */
static float fold_tempo(float bpm)
{
    while (bpm > 0 && bpm < 90) {
        bpm *= 2;
    }
    while (bpm >= 180) {
        bpm /= 2;
    }
    return bpm;
}

void mood_feature_vector(const music_features_t *f, float v[MOOD_FEATURE_COUNT])
{
    /* Drum loops and noise give a key estimate too; trust the mode only when the key is clear */
    float tonal = clamp01((f->key_strength - 0.5f) / 0.3f);

    v[0] = fold_tempo(f->bpm) / 100.0f;
    v[1] = f->regularity;
    v[2] = f->flux_contrast;
    v[3] = f->onset_rate;
    v[4] = f->bass_ratio;
    v[5] = f->mid_ratio;
    v[6] = f->high_ratio;
    v[7] = f->level_db_range / 10.0f;
    v[8] = log10f(f->centroid_hz + 1.0f);
    v[9] = f->mode * tonal;
    v[10] = f->key_strength;
}

static mood_t estimate_model(const float v[MOOD_FEATURE_COUNT])
{
    float zv = mood_model_valence_b, ze = mood_model_energy_b;
    for (int i = 0; i < MOOD_FEATURE_COUNT; i++) {
        float x = (v[i] - mood_model_mean[i]) / mood_model_std[i];
        zv += mood_model_valence_w[i] * x;
        ze += mood_model_energy_w[i] * x;
    }
    return (mood_t) {
        .valence = sigmoid(zv), .energy = sigmoid(ze)
    };
}

/*
 * Built-in weights. Energy follows the beat: how strong and periodic it is,
 * how dense the onsets are, tempo, and high-frequency content (cymbals,
 * distortion). Valence follows harmony first (major vs minor), then tempo and
 * a steady beat, which listeners associate with positive mood.
 */
static mood_t estimate_rules(const music_features_t *f, const float v[MOOD_FEATURE_COUNT])
{
    /* A periodic but smooth flux (vibrato, bowed strings) is not a beat */
    float percussive = clamp01((f->flux_contrast - 0.25f) / 0.6f);
    float beat = f->regularity * percussive;

    float ze = 2.5f * (beat - 0.3f) + 1.0f * (f->onset_rate - 1.8f) + 2.0f * (v[0] - 1.2f) +
               8.0f * (f->high_ratio - 0.06f) + 1.5f * (f->bass_ratio - 0.4f);
    float zv = 9.0f * v[9] + 1.2f * (v[0] - 1.15f) + 1.0f * (beat - 0.3f);
    return (mood_t) {
        .valence = sigmoid(zv), .energy = sigmoid(ze)
    };
}

mood_t mood_estimate(const music_features_t *f)
{
    float v[MOOD_FEATURE_COUNT];
    mood_t m;

    if (f->silent_ratio > 0.7f) {
        return (mood_t) {
            .silent = true, .valence = 0.5f, .energy = 0
        };
    }
    mood_feature_vector(f, v);
    m = MOOD_MODEL_TRAINED ? estimate_model(v) : estimate_rules(f, v);
    m.silent = false;
    return m;
}
