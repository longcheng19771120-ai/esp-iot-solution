/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Calibrated mood model. This default is untrained, so mood_estimate() uses
 * its built-in weights. Regenerate from your own recordings with
 * host_test/fit_mood_model.py, see the README.
 */

#pragma once

#define MOOD_MODEL_TRAINED 0

/* Inputs are standardised with mean/std, then each output is sigmoid(w . x + b) */
static const float mood_model_mean[MOOD_FEATURE_COUNT] = {0};
static const float mood_model_std[MOOD_FEATURE_COUNT] = {0};
static const float mood_model_valence_w[MOOD_FEATURE_COUNT] = {0};
static const float mood_model_valence_b = 0;
static const float mood_model_energy_w[MOOD_FEATURE_COUNT] = {0};
static const float mood_model_energy_b = 0;
