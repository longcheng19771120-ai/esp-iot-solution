/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Synthetic signals with known tempo, harmony and character: checks tempo, genre, mode and mood ordering */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "music_analyzer.h"
#include "mood_estimator.h"
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
    static float prev;
    float t = (float)n / SR, b = 60.f / 140;
    float ph = fmodf(t, 2 * b);
    float sp = fmodf(t + b, 2 * b);
    float saw = fmodf(t * 146.8f, 1) * 2 - 1;
    float gtr = tanhf(4 * (saw + (fmodf(t * 220, 1) * 2 - 1))) * 0.18f;
    /* Ride cymbal wash: differentiated noise has most of its energy up high */
    float w = noise(), ride = (w - prev) * 0.08f * (0.6f + 0.4f * expf(-fmodf(t, b / 2) * 10));
    prev = w;
    return 0.5f * kick(ph) + 0.5f * snare(sp) + 0.2f * hat(fmodf(t, b / 2)) + gtr + ride + 0.005f * noise();
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
/* Same piano-like arrangement over I-IV-V-I, in C major or C minor */
static float progression(int n, int minor)
{
    static const float major_chords[4][3] = {{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}};
    static const float minor_chords[4][3] = {{60, 63, 67}, {65, 68, 72}, {67, 71, 74}, {60, 63, 67}};
    float t = (float)n / SR, beat = 60.f / 100;
    int chord = (int)(t / (4 * beat)) % 4;
    float tb = fmodf(t, beat), v = 0;
    for (int i = 0; i < 3; i++) {
        float midi = minor ? minor_chords[chord][i] : major_chords[chord][i];
        float f = 440.0f * powf(2, (midi - 69) / 12);
        v += (sinf(2 * M_PI * f * t) + 0.3f * sinf(4 * M_PI * f * t)) * expf(-tb * 3);
    }
    float bass_f = 440.0f * powf(2, ((minor ? minor_chords : major_chords)[chord][0] - 12 - 69) / 12);
    return 0.08f * v + 0.1f * sinf(2 * M_PI * bass_f * t) + 0.003f * noise();
}

static float g_major(int n)
{
    return progression(n, 0);
}

static float g_minor(int n)
{
    return progression(n, 1);
}

static int failures;

typedef struct {
    float valence, energy, mode;
    int silent;
} result_t;

/*
 * Analyse 3 windows, expect the genre and the tempo within 4% in 2 of them
 * (MUSIC_GENRE_MAX and 0 = don't care), return the mean mood
 */
static result_t run(const char *name, gen_t g, music_genre_t want, float want_bpm)
{
    static music_analyzer_t ma;
    music_analyzer_init(&ma, SR, -65);
    float hop[MA_HOP_SIZE];
    int n = 0, bpm_hits = 0, genre_hits = 0;
    music_frame_t fr;
    music_features_t f;
    result_t res = {0};

    for (int w = 0; w < 3;) {
        for (int i = 0; i < MA_HOP_SIZE; i++) {
            hop[i] = g(n++);
        }
        if (!music_analyzer_process(&ma, hop, &fr, &f)) {
            continue;
        }
        w++;
        mood_t m = mood_estimate(&f);
        genre_result_t r = genre_classify(&f);
        genre_hits += want == MUSIC_GENRE_MAX || r.genre == want;
        res.valence += m.valence / 3;
        res.energy += m.energy / 3;
        res.mode += f.mode / 3;
        res.silent += m.silent;
        /* Tempo octave errors (x2, /2) are acceptable for lighting */
        for (float k = 0.5f; k <= 2.0f; k *= 2) {
            if (want_bpm == 0 || fabsf(f.bpm - want_bpm * k) < want_bpm * k * 0.04f) {
                bpm_hits++;
                break;
            }
        }
        printf("%-10s win%d: %-10s | %-7s valence %.2f energy %.2f | mode %+.2f key strength %.2f | bpm %6.1f\n", name, w,
               genre_name(r.genre), mood_quadrant_name(mood_quadrant(&m)), m.valence, m.energy, f.mode, f.key_strength, f.bpm);
    }
    if (bpm_hits < 2 || genre_hits < 2) {
        printf("FAIL %s: expected %s at %.0f bpm\n", name, want < MUSIC_GENRE_MAX ? genre_name(want) : "any genre",
               want_bpm);
        failures++;
    }
    return res;
}

static void expect(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

int main(void)
{
    result_t silence = run("silence", g_silence, MUSIC_GENRE_SILENCE, 0);
    result_t edm = run("edm128", g_edm, MUSIC_GENRE_ELECTRONIC, 128);
    result_t hiphop = run("hiphop90", g_hiphop, MUSIC_GENRE_HIPHOP, 90);
    result_t rock = run("rock140", g_rock, MUSIC_GENRE_ROCK, 140);
    result_t classical = run("classical", g_classical, MUSIC_GENRE_CLASSICAL, 0);
    result_t ambient = run("ambient", g_ambient, MUSIC_GENRE_AMBIENT, 0);
    result_t major = run("major", g_major, MUSIC_GENRE_MAX, 0);
    result_t minor = run("minor", g_minor, MUSIC_GENRE_MAX, 0);

    expect(silence.silent == 3, "silence is silent");
    expect(edm.energy > ambient.energy + 0.3f && edm.energy > classical.energy + 0.3f, "edm more energetic than calm music");
    expect(rock.energy > ambient.energy + 0.3f, "rock more energetic than ambient");
    expect(hiphop.energy > ambient.energy + 0.2f, "hip-hop more energetic than ambient");
    expect(major.mode > 0.1f && minor.mode < -0.05f, "major and minor detected");
    expect(major.valence > minor.valence + 0.3f, "major happier than minor");
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
