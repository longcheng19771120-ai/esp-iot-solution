/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Run the on-device music analysis on a WAV file, to check the genre and mood
 * estimates against real recordings without flashing.
 *
 *   ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav
 *   ./analyze_wav song.wav
 *
 * With --csv <label> it prints one feature row per window instead. A genre
 * label (ambient, classical, pop, rock, electronic, hiphop) gives rows for
 * fit_genre_model.py; a mood label, a quadrant (calm, happy, tense, sad) or a
 * "valence,energy" pair in 0..1, gives rows for fit_mood_model.py:
 *
 *   ./analyze_wav --csv rock song.wav >> genre.csv
 *   ./analyze_wav --csv happy song.wav >> mood.csv
 *   ./analyze_wav --csv 0.3,0.9 song.wav >> mood.csv
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "music_analyzer.h"
#include "mood_estimator.h"
#include "genre_classifier.h"

static const char *const s_keys[24] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
    "Cm", "C#m", "Dm", "D#m", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "A#m", "Bm",
};

int main(int argc, char **argv)
{
    const char *label = NULL, *path = NULL;
    float target_v = 0, target_e = 0;
    bool genre_label = false;

    if (argc == 2) {
        path = argv[1];
    } else if (argc == 4 && strcmp(argv[1], "--csv") == 0) {
        label = argv[2];
        path = argv[3];
        for (int g = 1; g < MUSIC_GENRE_MAX; g++) {
            genre_label |= strcmp(label, genre_name((music_genre_t)g)) == 0;
        }
        if (!genre_label && !mood_quadrant_target(label, &target_v, &target_e) &&
                sscanf(label, "%f,%f", &target_v, &target_e) != 2) {
            fprintf(stderr, "label must be a genre, calm, happy, tense, sad or valence,energy\n");
            return 1;
        }
    } else {
        fprintf(stderr, "usage: %s [--csv <label>] <16 kHz mono 16-bit wav>\n", argv[0]);
        return 1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return 1;
    }

    /* Walk the RIFF chunks to find fmt and data */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "not a WAV file\n");
        return 1;
    }
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) {
            fprintf(stderr, "no data chunk\n");
            return 1;
        }
        uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (size < 16 || fread(fmt, 1, 16, fp) != 16) {
                return 1;
            }
            channels = fmt[2] | fmt[3] << 8;
            rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
            bits = fmt[14] | fmt[15] << 8;
            fseek(fp, size - 16 + (size & 1), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            break;
        } else {
            fseek(fp, size + (size & 1), SEEK_CUR);
        }
    }
    if (channels != 1 || bits != 16) {
        fprintf(stderr, "need mono 16-bit PCM (got %u ch, %u bit), convert with ffmpeg first\n", channels, bits);
        return 1;
    }

    static music_analyzer_t ma;
    /* Same as the CONFIG_AUDIO_SILENCE_DB default */
    music_analyzer_init(&ma, rate, -65);
    int16_t pcm[MA_HOP_SIZE];
    float hop[MA_HOP_SIZE];
    music_frame_t frame;
    music_features_t f;
    int window = 0;

    while (fread(pcm, sizeof(int16_t), MA_HOP_SIZE, fp) == MA_HOP_SIZE) {
        for (int i = 0; i < MA_HOP_SIZE; i++) {
            hop[i] = pcm[i] / 32768.0f;
        }
        if (!music_analyzer_process(&ma, hop, &frame, &f)) {
            continue;
        }
        window++;
        if (label) {
            /* Silent windows carry no genre or mood information */
            if (f.silent_ratio > 0.7f) {
                continue;
            }
            if (genre_label) {
                float gv[GENRE_FEATURE_COUNT];
                genre_feature_vector(&f, gv);
                printf("%s,%s", label, path);
                for (int i = 0; i < GENRE_FEATURE_COUNT; i++) {
                    printf(",%.4f", gv[i]);
                }
                printf("\n");
                continue;
            }
            float v[MOOD_FEATURE_COUNT];
            mood_feature_vector(&f, v);
            /* The current estimate goes along so the fit can be compared against it */
            mood_t m = mood_estimate(&f);
            printf("%.2f,%.2f,%s,%.3f,%.3f", target_v, target_e, path, m.valence, m.energy);
            for (int i = 0; i < MOOD_FEATURE_COUNT; i++) {
                printf(",%.4f", v[i]);
            }
            printf("\n");
            continue;
        }
        mood_t m = mood_estimate(&f);
        genre_result_t g = genre_classify(&f);
        printf("%6.1fs %-10s %.2f | %-7s valence %.2f energy %.2f | key %-3s mode %+.2f strength %.2f | bpm %5.1f reg %.2f "
               "contrast %.2f | onsets %.1f/s | bass %.2f mid %.2f high %.2f | %5.1f dB\n",
               (float)window * MA_WINDOW_HOPS * MA_HOP_SIZE / rate, genre_name(g.genre), g.confidence, mood_quadrant_name(mood_quadrant(&m)),
               m.valence, m.energy, s_keys[f.key], f.mode, f.key_strength, f.bpm, f.regularity, f.flux_contrast,
               f.onset_rate, f.bass_ratio, f.mid_ratio, f.high_ratio, f.level_db_mean);
    }
    fclose(fp);
    return 0;
}
