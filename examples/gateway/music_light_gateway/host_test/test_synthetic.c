/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Synthetic signals with known tempo and character, checks tempo and genre */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "music_analyzer.h"
#include "genre_classifier.h"
#define SR 16000
static float noise(void)
{
    return (float)rand() / RAND_MAX * 2 - 1;
}
typedef float (*gen_t)(int n);
static float g_silence(int n)
{
    (void)n;
    return 0.0003f * noise();
}
static float kick(float t)
{
    return t < 0.15f ? sinf(2 * M_PI * (50 + 80 * expf(-t * 30)) * t) * expf(-t * 20) : 0;
}
static float hat(float t)
{
    return t < 0.04f ? noise() * expf(-t * 120) : 0;
}
static float snare(float t)
{
    return t < 0.12f ? (noise() * 0.6f + 0.4f * sinf(2 * M_PI * 190 * t)) * expf(-t * 30) : 0;
}
static float g_edm(int n)
{
    float t = (float)n / SR, b = 60.f / 128; float ph = fmodf(t, b); float hp = fmodf(t + b / 2, b);
    return 0.6f * kick(ph) + 0.15f * hat(hp) + 0.1f * sinf(2 * M_PI * 55 * t) * (0.5f + 0.5f * sinf(2 * M_PI * t / b)) + 0.005f * noise();
}
static float g_hiphop(int n)
{
    float t = (float)n / SR, b = 60.f / 90; float ph = fmodf(t, 2 * b); float sp = fmodf(t + b, 2 * b);
    return 0.7f * kick(ph) + 0.4f * snare(sp) + 0.25f * sinf(2 * M_PI * 45 * t) + 0.05f * hat(fmodf(t, b / 2)) + 0.005f * noise();
}
static float g_rock(int n)
{
    float t = (float)n / SR, b = 60.f / 140; float ph = fmodf(t, 2 * b); float sp = fmodf(t + b, 2 * b);
    float saw = fmodf(t * 146.8f, 1) * 2 - 1; float gtr = tanhf(4 * (saw + (fmodf(t * 220, 1) * 2 - 1))) * 0.18f;
    return 0.5f * kick(ph) + 0.5f * snare(sp) + 0.2f * hat(fmodf(t, b / 2)) + gtr + 0.005f * noise();
}
static float g_classical(int n)
{
    float t = (float)n / SR; float env = 0.05f + 0.3f * (0.5f + 0.5f * sinf(2 * M_PI * t / 9)); float v = 0;
    float f[] = {261.6f, 329.6f, 392, 523.3f, 659.3f}; for (int i = 0; i < 5; i++) v += sinf(2 * M_PI * f[i] * (1 + 0.003f * sinf(2 * M_PI * 5 * t)) * t) / (i + 1);
    int note = (int)(t / 1.7f); v += 0.3f * sinf(2 * M_PI * (440 * powf(2, (note % 7) / 12.f)) * t) * expf(-fmodf(t, 1.7f) * 1.2f);
    return env * v * 0.4f + 0.002f * noise();
}
static float g_ambient(int n)
{
    float t = (float)n / SR;
    return 0.08f * (sinf(2 * M_PI * 110 * t) + 0.6f * sinf(2 * M_PI * 164.8f * t) + 0.4f * sinf(2 * M_PI * 220.5f * t)) * (0.9f + 0.1f * sinf(2 * M_PI * t / 12)) + 0.004f * noise();
}
static int failures;

/* Analyse 3 windows, expect the genre in at least 2 and the tempo within 4% (0 = don't care) */
static void run(const char *name, gen_t g, music_genre_t want, float want_bpm)
{
    static music_analyzer_t ma;
    music_analyzer_init(&ma, SR, -50);
    float hop[MA_HOP_SIZE];
    int n = 0, hits = 0, bpm_hits = 0;
    music_frame_t fr;
    music_features_t f;

    for (int w = 0; w < 3;) {
        for (int i = 0; i < MA_HOP_SIZE; i++) {
            hop[i] = g(n++);
        }
        if (!music_analyzer_process(&ma, hop, &fr, &f)) {
            continue;
        }
        w++;
        genre_result_t r = genre_classify(&f);
        hits += r.genre == want;
        /* Tempo octave errors (x2, /2) are acceptable for lighting */
        for (float k = 0.5f; k <= 2.0f; k *= 2) {
            if (want_bpm == 0 || fabsf(f.bpm - want_bpm * k) < want_bpm * k * 0.04f) {
                bpm_hits++;
                break;
            }
        }
        printf("%-10s win%d: %-10s conf %.2f bpm %6.1f reg %.2f contrast %.2f\n", name, w,
               genre_name(r.genre), r.confidence, f.bpm, f.regularity, f.flux_contrast);
    }
    if (hits < 2 || bpm_hits < 2) {
        printf("FAIL %s: expected %s at %.0f bpm\n", name, genre_name(want), want_bpm);
        failures++;
    }
}

int main(void)
{
    run("silence", g_silence, MUSIC_GENRE_SILENCE, 0);
    run("edm128", g_edm, MUSIC_GENRE_ELECTRONIC, 128);
    run("hiphop90", g_hiphop, MUSIC_GENRE_HIPHOP, 90);
    run("rock140", g_rock, MUSIC_GENRE_ROCK, 140);
    run("classical", g_classical, MUSIC_GENRE_CLASSICAL, 0);
    run("ambient", g_ambient, MUSIC_GENRE_AMBIENT, 0);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
