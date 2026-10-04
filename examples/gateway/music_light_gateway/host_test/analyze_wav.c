/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Run the on-device music analysis on a WAV file, to tune the genre rules
 * against real recordings without flashing.
 *
 *   ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav
 *   ./analyze_wav song.wav
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "music_analyzer.h"
#include "genre_classifier.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <16 kHz mono 16-bit wav>\n", argv[0]);
        return 1;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
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
    music_analyzer_init(&ma, rate, -55);
    int16_t pcm[MA_HOP_SIZE];
    float hop[MA_HOP_SIZE];
    music_frame_t frame;
    music_features_t f;
    int window = 0;

    while (fread(pcm, sizeof(int16_t), MA_HOP_SIZE, fp) == MA_HOP_SIZE) {
        for (int i = 0; i < MA_HOP_SIZE; i++) {
            hop[i] = pcm[i] / 32768.0f;
        }
        if (music_analyzer_process(&ma, hop, &frame, &f)) {
            genre_result_t r = genre_classify(&f);
            printf("%6.1fs %-10s conf %.2f | bpm %5.1f reg %.2f contrast %.2f | %5.1f dB range %4.1f | "
                   "onsets %.1f/s | bass %.2f mid %.2f high %.2f\n",
                   (float)++window * MA_WINDOW_HOPS * MA_HOP_SIZE / rate, genre_name(r.genre), r.confidence,
                   f.bpm, f.regularity, f.flux_contrast, f.level_db_mean, f.level_db_range, f.onset_rate,
                   f.bass_ratio, f.mid_ratio, f.high_ratio);
        }
    }
    fclose(fp);
    return 0;
}
