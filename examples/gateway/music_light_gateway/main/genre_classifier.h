/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "music_analyzer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_GENRE_SILENCE = 0,
    MUSIC_GENRE_AMBIENT,
    MUSIC_GENRE_CLASSICAL,
    MUSIC_GENRE_POP,
    MUSIC_GENRE_ROCK,
    MUSIC_GENRE_ELECTRONIC,
    MUSIC_GENRE_HIPHOP,
    MUSIC_GENRE_MAX,
} music_genre_t;

typedef struct {
    music_genre_t genre;
    float confidence;       /*!< 0..1 */
} genre_result_t;

#define GENRE_FEATURE_COUNT 9

/**
 * @brief Classify one analysis window
 *
 * Uses the calibrated model in genre_model.h when one has been generated with
 * host_test/fit_genre_model.py, otherwise a rule-based baseline built on tempo,
 * beat regularity, spectral balance and dynamics.
 */
genre_result_t genre_classify(const music_features_t *f);

/**
 * @brief Feature vector used by the calibrated model
 *
 * Shared by the device and the host tools so the training data and the
 * on-device inputs are computed by the same code.
 */
void genre_feature_vector(const music_features_t *f, float v[GENRE_FEATURE_COUNT]);

/** True when a calibrated model is compiled in */
bool genre_model_trained(void);

const char *genre_name(music_genre_t genre);

#ifdef __cplusplus
}
#endif
