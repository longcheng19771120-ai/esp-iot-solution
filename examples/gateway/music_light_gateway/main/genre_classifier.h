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

/**
 * @brief Classify one analysis window
 *
 * This is a rule-based baseline built on tempo, beat regularity, spectral
 * balance and dynamics. It is meant to be replaced by a trained model (for
 * example an ESP-DL network on log-mel features) behind the same interface.
 */
genre_result_t genre_classify(const music_features_t *f);

const char *genre_name(music_genre_t genre);

#ifdef __cplusplus
}
#endif
