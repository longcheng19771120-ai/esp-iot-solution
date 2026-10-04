/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform independent music analysis: no ESP-IDF dependencies, so it can be
 * unit tested on a host PC.
 *
 * Every MA_HOP_SIZE new samples a MA_FFT_SIZE Hann-windowed FFT is computed.
 * Per hop it reports the loudness and whether a beat (onset) was detected.
 * Every MA_WINDOW_HOPS hops it summarises the window into music_features_t,
 * which the mood estimator consumes. A longer FFT feeds a chromagram for key
 * and major/minor mode detection.
 */
#define MA_FFT_SIZE     512
#define MA_HOP_SIZE     256
#define MA_WINDOW_HOPS  320     /* 5.12 s at 16 kHz */
#define MA_CHROMA_FFT   2048    /* longer FFT for pitch resolution, run every MA_CHROMA_EVERY hops */
#define MA_CHROMA_EVERY 4

typedef struct {
    float level_db;         /*!< RMS level of the hop, dBFS */
    float level;            /*!< Level 0..1 relative to the recent peak (auto gain) */
    float bass;             /*!< Share of spectral energy below 250 Hz, 0..1 */
    float mid;              /*!< Share of spectral energy 250 Hz..2 kHz, 0..1 */
    float high;             /*!< Share of spectral energy above 2 kHz, 0..1 */
    bool silent;            /*!< Level below the silence threshold */
    bool beat;              /*!< An onset was detected in this hop */
    float beat_strength;    /*!< 0..1, how far the onset exceeded the threshold */
} music_frame_t;

typedef struct {
    float level_db_mean;    /*!< Mean level over the window, dBFS */
    float level_db_range;   /*!< Dynamic range between loud and quiet 0.5 s blocks, dB */
    float bass_ratio;       /*!< Mean bass share */
    float mid_ratio;        /*!< Mean mid share */
    float high_ratio;       /*!< Mean high share */
    float centroid_hz;      /*!< Mean spectral centroid */
    float flux_mean;        /*!< Mean spectral flux (onset strength) */
    float flux_contrast;    /*!< Std / mean of the flux, high for percussive music */
    float onset_rate;       /*!< Detected onsets per second */
    float bpm;              /*!< Tempo estimate from onset autocorrelation, 0 if none */
    float regularity;       /*!< 0..1, strength of the periodic beat */
    float silent_ratio;     /*!< Share of hops below the silence threshold */
    float mode;             /*!< Major minus minor key correlation, > 0 leans major */
    float key_strength;     /*!< Best key correlation, 0..1, low for atonal or drum-only audio */
    int key;                /*!< Best key, 0..11 = C..B major, 12..23 = C..B minor */
} music_features_t;

typedef struct {
    int sample_rate;
    float silence_db;

    float window[MA_FFT_SIZE];          /* Hann window */
    float twiddle_re[MA_CHROMA_FFT / 2];
    float twiddle_im[MA_CHROMA_FFT / 2];
    float buf[MA_CHROMA_FFT];           /* sliding input buffer, newest samples last */
    float re[MA_CHROMA_FFT];
    float im[MA_CHROMA_FFT];
    uint8_t bin_pitch[MA_CHROMA_FFT / 2]; /* pitch class of each chroma FFT bin, 0xff if unused */
    float prev_logmag[MA_FFT_SIZE / 2 + 1];

    float peak_rms;                     /* auto gain tracker */
    float flux_hist[64];                /* ~1 s of flux for the onset threshold */
    int flux_hist_pos;
    int hops_since_onset;

    /* window accumulators */
    int hop_in_window;
    float onset_env[MA_WINDOW_HOPS];
    float level_db[MA_WINDOW_HOPS];
    double sum_bass, sum_mid, sum_high, sum_centroid, sum_flux;
    double chroma[12];
    int onsets, silent_hops, voiced_hops, hop_count;
} music_analyzer_t;

/**
 * @brief Initialise the analyzer
 *
 * @param sample_rate Input sample rate in Hz, 16000 recommended
 * @param silence_db  Hops quieter than this (dBFS) are treated as silence
 */
void music_analyzer_init(music_analyzer_t *ma, int sample_rate, float silence_db);

/**
 * @brief Feed MA_HOP_SIZE new samples in the range -1..1
 *
 * @param[out] frame    Per hop result
 * @param[out] features Filled when a full analysis window has completed
 * @return true if features was filled
 */
bool music_analyzer_process(music_analyzer_t *ma, const float *samples, music_frame_t *frame,
                            music_features_t *features);

#ifdef __cplusplus
}
#endif
