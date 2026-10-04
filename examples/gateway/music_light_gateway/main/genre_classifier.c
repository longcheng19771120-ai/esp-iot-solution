/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include "genre_classifier.h"

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

genre_result_t genre_classify(const music_features_t *f)
{
    genre_result_t res = { .genre = MUSIC_GENRE_SILENCE, .confidence = 1 };
    if (f->silent_ratio > 0.7f) {
        return res;
    }

    /* Half-time hip-hop is often detected at double tempo */
    float bpm = f->bpm;
    float bpm_half = bpm > 130 ? bpm / 2 : bpm;
    /* A periodic but smooth flux (vibrato, tremolo) is not a beat: weight by percussiveness */
    float beat = f->regularity * in_range(f->flux_contrast, 0.8f, INFINITY, 0.4f);
    float score[MUSIC_GENRE_MAX] = {0};

    score[MUSIC_GENRE_AMBIENT] = below(beat, 0.15f, 0.15f) * below(f->onset_rate, 1.0f, 1.0f) *
                                 below(f->level_db_range, 8, 6);
    score[MUSIC_GENRE_CLASSICAL] = below(beat, 0.25f, 0.15f) * above(f->level_db_range, 8, 6) *
                                   below(f->bass_ratio, 0.35f, 0.2f);
    score[MUSIC_GENRE_ELECTRONIC] = above(beat, 0.45f, 0.2f) * in_range(bpm, 118, 150, 12) *
                                    above(f->bass_ratio, 0.35f, 0.2f) * below(f->mid_ratio, 0.3f, 0.15f) *
                                    below(f->level_db_range, 8, 6);
    score[MUSIC_GENRE_HIPHOP] = above(beat, 0.3f, 0.2f) * in_range(bpm_half, 75, 102, 10) *
                                above(f->bass_ratio, 0.4f, 0.2f);
    score[MUSIC_GENRE_ROCK] = above(beat, 0.25f, 0.2f) * in_range(bpm, 95, 175, 15) *
                              /* guitars and vocals fill the mids, cymbals the highs */
                              fmaxf(above(f->high_ratio, 0.12f, 0.08f), above(f->mid_ratio, 0.3f, 0.15f)) *
                              above(f->onset_rate, 2.0f, 1.5f) *
                              below(f->bass_ratio, 0.55f, 0.2f);
    /* Pop is the catch-all for steady, moderately bright music */
    score[MUSIC_GENRE_POP] = 0.35f * above(beat, 0.2f, 0.2f) * in_range(bpm, 85, 135, 20);

    float best = 0, total = 0;
    for (int g = MUSIC_GENRE_AMBIENT; g < MUSIC_GENRE_MAX; g++) {
        total += score[g];
        if (score[g] > best) {
            best = score[g];
            res.genre = (music_genre_t)g;
        }
    }
    if (best <= 0) {
        /* Sound present but nothing matched: treat as pop at low confidence */
        res.genre = MUSIC_GENRE_POP;
        res.confidence = 0.2f;
        return res;
    }
    res.confidence = best / total;
    return res;
}
