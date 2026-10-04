/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include "genre_classifier.h"
#include "genre_model.h"

static const char *const s_names[MUSIC_GENRE_MAX] = {
    [MUSIC_GENRE_SILENCE] = "silence",
    [MUSIC_GENRE_AMBIENT] = "ambient",
    [MUSIC_GENRE_CLASSICAL] = "classical",
    [MUSIC_GENRE_POP] = "pop",
    [MUSIC_GENRE_ROCK] = "rock",
    [MUSIC_GENRE_ELECTRONIC] = "electronic",
    [MUSIC_GENRE_HIPHOP] = "hiphop",
};

const char *genre_name(music_genre_t genre)
{
    return genre < MUSIC_GENRE_MAX ? s_names[genre] : "unknown";
}

/* Smooth membership: 1 inside [lo, hi], falling off linearly over `soft` outside */
static float in_range(float x, float lo, float hi, float soft)
{
    if (x < lo) {
        return fmaxf(0, 1 - (lo - x) / soft);
    }
    if (x > hi) {
        return fmaxf(0, 1 - (x - hi) / soft);
    }
    return 1;
}

static float above(float x, float lo, float soft)
{
    return in_range(x, lo, INFINITY, soft);
}

static float below(float x, float hi, float soft)
{
    return in_range(x, -INFINITY, hi, soft);
}

/* Tempo estimates are often off by an octave: compare on a folded 90..180 scale */
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

void genre_feature_vector(const music_features_t *f, float v[GENRE_FEATURE_COUNT])
{
    v[0] = fold_tempo(f->bpm) / 100.0f;
    v[1] = f->regularity;
    v[2] = f->flux_contrast;
    v[3] = f->onset_rate;
    v[4] = f->bass_ratio;
    v[5] = f->mid_ratio;
    v[6] = f->high_ratio;
    v[7] = f->level_db_range / 10.0f;
    v[8] = log10f(f->centroid_hz + 1.0f);
}

bool genre_model_trained(void)
{
    return GENRE_MODEL_TRAINED;
}

/* Gaussian naive Bayes over the calibrated per-genre mean and spread */
static genre_result_t classify_model(const music_features_t *f)
{
    genre_result_t res = { .genre = MUSIC_GENRE_POP, .confidence = 0 };
    float v[GENRE_FEATURE_COUNT];
    float ll[MUSIC_GENRE_MAX];
    float best = -INFINITY;

    genre_feature_vector(f, v);
    for (int g = 0; g < MUSIC_GENRE_MAX; g++) {
        ll[g] = -INFINITY;
        if (!genre_model_present[g]) {
            continue;
        }
        ll[g] = 0;
        for (int i = 0; i < GENRE_FEATURE_COUNT; i++) {
            float z = (v[i] - genre_model_mean[g][i]) / genre_model_std[g][i];
            ll[g] -= 0.5f * z * z + logf(genre_model_std[g][i]);
        }
        if (ll[g] > best) {
            best = ll[g];
            res.genre = (music_genre_t)g;
        }
    }
    float sum = 0;
    for (int g = 0; g < MUSIC_GENRE_MAX; g++) {
        if (ll[g] > -INFINITY) {
            sum += expf(ll[g] - best);
        }
    }
    res.confidence = sum > 0 ? 1.0f / sum : 0;
    return res;
}

genre_result_t genre_classify(const music_features_t *f)
{
    genre_result_t res = { .genre = MUSIC_GENRE_SILENCE, .confidence = 1 };
    if (f->silent_ratio > 0.7f) {
        return res;
    }
    if (GENRE_MODEL_TRAINED) {
        return classify_model(f);
    }

    float tempo = fold_tempo(f->bpm);
    /* Hip-hop sits at 75..102 BPM, which may show up folded to 150..204 */
    float tempo_low = tempo >= 150 ? tempo / 2 : tempo;
    /*
     * A periodic but smooth flux (vibrato, tremolo, bowed strings) is not a beat, so
     * weight regularity by percussiveness. Room reverb smears onsets and lowers the
     * contrast, hence the low threshold.
     */
    float beat = f->regularity * in_range(f->flux_contrast, 0.6f, INFINITY, 0.35f);
    float score[MUSIC_GENRE_MAX] = {0};

    score[MUSIC_GENRE_AMBIENT] = below(beat, 0.15f, 0.15f) * below(f->onset_rate, 1.0f, 1.0f) *
                                 below(f->level_db_range, 8, 6);
    /* Orchestra, piano and small ensembles: mids dominate, little sub-bass */
    score[MUSIC_GENRE_CLASSICAL] = below(beat, 0.3f, 0.2f) * below(f->bass_ratio, 0.35f, 0.15f) *
                                   above(f->mid_ratio, 0.6f, 0.2f) * below(f->high_ratio, 0.15f, 0.1f);
    score[MUSIC_GENRE_ELECTRONIC] = above(beat, 0.3f, 0.2f) * in_range(tempo, 118, 150, 12) *
                                    above(f->bass_ratio, 0.55f, 0.2f) * below(f->mid_ratio, 0.35f, 0.15f);
    score[MUSIC_GENRE_HIPHOP] = above(beat, 0.25f, 0.2f) * above(f->bass_ratio, 0.6f, 0.2f) *
                                fmaxf(in_range(tempo_low, 75, 102, 10), in_range(f->bpm, 62, 75, 5));
    /* Cymbals and distorted guitars put real energy above 2 kHz */
    score[MUSIC_GENRE_ROCK] = above(beat, 0.25f, 0.2f) * in_range(tempo, 95, 175, 15) *
                              above(f->high_ratio, 0.12f, 0.06f) * above(f->onset_rate, 2.0f, 1.5f);
    /* Pop is the catch-all for music with a beat that matches nothing more specific */
    score[MUSIC_GENRE_POP] = 0.35f * above(beat, 0.2f, 0.1f);

    float best = 0, total = 0;
    for (int g = MUSIC_GENRE_AMBIENT; g < MUSIC_GENRE_MAX; g++) {
        total += score[g];
        if (score[g] > best) {
            best = score[g];
            res.genre = (music_genre_t)g;
        }
    }
    if (best <= 0) {
        /* Sound present but nothing matched: decide by beat and dynamics, at low confidence */
        if (beat >= 0.15f) {
            res.genre = MUSIC_GENRE_POP;
        } else {
            res.genre = f->level_db_range > 6 ? MUSIC_GENRE_CLASSICAL : MUSIC_GENRE_AMBIENT;
        }
        res.confidence = 0.2f;
        return res;
    }
    res.confidence = best / total;
    return res;
}
