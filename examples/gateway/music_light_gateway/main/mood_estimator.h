/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "music_analyzer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mood on two continuous axes (Russell's circumplex model):
 *   valence: 0 = negative (sad, tense) .. 1 = positive (happy, calm)
 *   energy:  0 = calm .. 1 = energetic
 * The four quadrants are named for display and for calibration labels.
 */
typedef enum {
    MOOD_SILENCE = 0,
    MOOD_CALM,      /*!< positive, low energy */
    MOOD_HAPPY,     /*!< positive, high energy */
    MOOD_TENSE,     /*!< negative, high energy */
    MOOD_SAD,       /*!< negative, low energy */
    MOOD_MAX,
} mood_quadrant_t;

typedef struct {
    bool silent;
    float valence;
    float energy;
} mood_t;

#define MOOD_FEATURE_COUNT 11

/**
 * @brief Estimate the mood of one analysis window
 *
 * Uses the calibrated model in mood_model.h when one has been generated with
 * host_test/fit_mood_model.py, otherwise built-in weights based on tempo, beat
 * strength, onset density, spectral balance and major/minor mode.
 */
mood_t mood_estimate(const music_features_t *f);

/**
 * @brief Feature vector used by the calibrated model
 *
 * Shared by the device and the host tools so the training data and the
 * on-device inputs are computed by the same code.
 */
void mood_feature_vector(const music_features_t *f, float v[MOOD_FEATURE_COUNT]);

mood_quadrant_t mood_quadrant(const mood_t *m);
const char *mood_quadrant_name(mood_quadrant_t q);
/** Quadrant name to target values for calibration, false if unknown */
bool mood_quadrant_target(const char *name, float *valence, float *energy);
/** True when a calibrated model is compiled in */
bool mood_model_trained(void);

#ifdef __cplusplus
}
#endif
