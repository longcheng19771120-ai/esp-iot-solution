/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Calibrated genre model. This default is untrained, so genre_classify() uses
 * its built-in rules. Regenerate from your own recordings with
 * host_test/fit_genre_model.py, see the README.
 */

#pragma once

#define GENRE_MODEL_TRAINED 0

static const unsigned char genre_model_present[MUSIC_GENRE_MAX] = {0};
static const float genre_model_mean[MUSIC_GENRE_MAX][GENRE_FEATURE_COUNT] = {{0}};
static const float genre_model_std[MUSIC_GENRE_MAX][GENRE_FEATURE_COUNT] = {{0}};
