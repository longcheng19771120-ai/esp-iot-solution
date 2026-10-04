/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include "music_analyzer.h"

#define BINS            (MA_FFT_SIZE / 2 + 1)
#define FLUX_HIST_LEN   (int)(sizeof(((music_analyzer_t *)0)->flux_hist) / sizeof(float))
#define ONSET_K         1.5f    /* threshold = mean + K * std of recent flux */
#define ONSET_RATIO     1.6f    /* and at least RATIO * mean */
#define MIN_BPM         60.0f
#define MAX_BPM         180.0f
#define TWO_PI          6.28318530718f

void music_analyzer_init(music_analyzer_t *ma, int sample_rate, float silence_db)
{
    memset(ma, 0, sizeof(*ma));
    ma->sample_rate = sample_rate;
    ma->silence_db = silence_db;
    ma->peak_rms = 1e-4f;
    for (int i = 0; i < MA_FFT_SIZE; i++) {
        ma->window[i] = 0.5f - 0.5f * cosf(TWO_PI * i / MA_FFT_SIZE);
    }
    for (int i = 0; i < MA_CHROMA_FFT / 2; i++) {
        ma->twiddle_re[i] = cosf(TWO_PI * i / MA_CHROMA_FFT);
        ma->twiddle_im[i] = -sinf(TWO_PI * i / MA_CHROMA_FFT);
    }
    /* Pitch classes from A2 (110 Hz) to A7 (3520 Hz); below that the bins are too coarse */
    const float bin_hz = (float)sample_rate / MA_CHROMA_FFT;
    for (int k = 0; k < MA_CHROMA_FFT / 2; k++) {
        float f = k * bin_hz;
        ma->bin_pitch[k] = 0xff;
        if (f >= 110.0f && f <= 3520.0f) {
            /* 0 = C: A is 9 semitones above C */
            int midi = (int)lroundf(69.0f + 12.0f * log2f(f / 440.0f));
            ma->bin_pitch[k] = (uint8_t)(midi % 12);
        }
    }
}

/* In-place iterative radix-2 FFT of size n (a power of two up to MA_CHROMA_FFT) */
static void fft(music_analyzer_t *ma, int n)
{
    float *re = ma->re, *im = ma->im;

    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = MA_CHROMA_FFT / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = ma->twiddle_re[k * step];
                float wi = ma->twiddle_im[k * step];
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

/* Tempo from the autocorrelation of the onset envelope */
static void estimate_tempo(const music_analyzer_t *ma, float *bpm, float *regularity)
{
    const float hops_per_s = (float)ma->sample_rate / MA_HOP_SIZE;
    const int n = MA_WINDOW_HOPS;
    float env[MA_WINDOW_HOPS];
    float mean = 0;

    for (int i = 0; i < n; i++) {
        mean += ma->onset_env[i];
    }
    mean /= n;
    float r0 = 0;
    for (int i = 0; i < n; i++) {
        env[i] = ma->onset_env[i] - mean;
        r0 += env[i] * env[i];
    }
    *bpm = 0;
    *regularity = 0;
    if (r0 <= 1e-9f) {
        return;
    }

    int min_lag = (int)(hops_per_s * 60.0f / MAX_BPM);
    int max_lag = (int)(hops_per_s * 60.0f / MIN_BPM + 0.5f);
    float best = 0;
    int best_lag = 0;
    for (int lag = min_lag; lag <= max_lag && lag < n / 2; lag++) {
        float r = 0;
        for (int i = 0; i + lag < n; i++) {
            r += env[i] * env[i + lag];
        }
        r /= (n - lag);
        /* Mild preference for tempos around 120 BPM to reduce octave errors */
        float t = 60.0f * hops_per_s / lag;
        float w = expf(-0.5f * powf(log2f(t / 120.0f) / 1.0f, 2));
        if (r * w > best) {
            best = r * w;
            best_lag = lag;
        }
    }
    if (best_lag == 0) {
        return;
    }
    /* Parabolic interpolation around the peak for sub-hop precision */
    float lag = best_lag;
    if (best_lag > min_lag && best_lag < max_lag) {
        float rm = 0, rp = 0, rc = 0;
        for (int i = 0; i + best_lag + 1 < n; i++) {
            rm += env[i] * env[i + best_lag - 1];
            rc += env[i] * env[i + best_lag];
            rp += env[i] * env[i + best_lag + 1];
        }
        float denom = rm - 2 * rc + rp;
        if (denom < 0) {
            lag += 0.5f * (rm - rp) / denom;
        }
    }
    *bpm = 60.0f * hops_per_s / lag;
    float rr = 0;
    for (int i = 0; i + best_lag < n; i++) {
        rr += env[i] * env[i + best_lag];
    }
    *regularity = fmaxf(0, rr / (n - best_lag)) / (r0 / n);
    if (*regularity > 1) {
        *regularity = 1;
    }
}

/* Accumulate a normalised pitch-class profile from the long FFT */
static void accumulate_chroma(music_analyzer_t *ma)
{
    float frame[12] = {0}, total = 0;

    for (int i = 0; i < MA_CHROMA_FFT; i++) {
        float w = 0.5f - 0.5f * cosf(TWO_PI * i / MA_CHROMA_FFT);
        ma->re[i] = ma->buf[i] * w;
        ma->im[i] = 0;
    }
    fft(ma, MA_CHROMA_FFT);
    for (int k = 1; k < MA_CHROMA_FFT / 2; k++) {
        if (ma->bin_pitch[k] != 0xff) {
            float mag = sqrtf(ma->re[k] * ma->re[k] + ma->im[k] * ma->im[k]);
            frame[ma->bin_pitch[k]] += mag;
            total += mag;
        }
    }
    /* Every frame counts equally, so loud passages don't dominate the key */
    if (total > 1e-6f) {
        for (int p = 0; p < 12; p++) {
            ma->chroma[p] += frame[p] / total;
        }
    }
}

/* Krumhansl-Kessler key profiles, index 0 = tonic */
static const float s_major[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
static const float s_minor[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

static float correlate(const double *chroma, const float *profile, int tonic)
{
    double mc = 0, mp = 0, num = 0, dc = 0, dp = 0;
    for (int i = 0; i < 12; i++) {
        mc += chroma[i];
        mp += profile[i];
    }
    mc /= 12;
    mp /= 12;
    for (int i = 0; i < 12; i++) {
        double c = chroma[(i + tonic) % 12] - mc, p = profile[i] - mp;
        num += c * p;
        dc += c * c;
        dp += p * p;
    }
    return dc > 0 ? (float)(num / sqrt(dc * dp)) : 0;
}

static void estimate_key(const music_analyzer_t *ma, music_features_t *f)
{
    float best_major = -1, best_minor = -1;
    int key_major = 0, key_minor = 0;

    for (int t = 0; t < 12; t++) {
        float r = correlate(ma->chroma, s_major, t);
        if (r > best_major) {
            best_major = r;
            key_major = t;
        }
        r = correlate(ma->chroma, s_minor, t);
        if (r > best_minor) {
            best_minor = r;
            key_minor = t;
        }
    }
    f->mode = best_major - best_minor;
    f->key_strength = fmaxf(0, fmaxf(best_major, best_minor));
    f->key = best_major >= best_minor ? key_major : 12 + key_minor;
}

static void finish_window(music_analyzer_t *ma, music_features_t *f)
{
    const float seconds = (float)MA_WINDOW_HOPS * MA_HOP_SIZE / ma->sample_rate;
    const int block = MA_WINDOW_HOPS / 10;      /* ~0.5 s, so beats don't count as dynamics */
    int voiced = ma->voiced_hops > 0 ? ma->voiced_hops : 1;
    double sum_db = 0;
    float block_min = INFINITY, block_max = -INFINITY;

    for (int b = 0; b < 10; b++) {
        double e = 0;
        for (int i = b * block; i < (b + 1) * block; i++) {
            sum_db += ma->level_db[i];
            e += powf(10.0f, ma->level_db[i] / 10.0f);
        }
        float db = 10.0f * log10f(e / block + 1e-14f);
        block_min = fminf(block_min, db);
        block_max = fmaxf(block_max, db);
    }

    memset(f, 0, sizeof(*f));
    f->level_db_mean = sum_db / MA_WINDOW_HOPS;
    f->level_db_range = block_max - block_min;
    f->bass_ratio = ma->sum_bass / voiced;
    f->mid_ratio = ma->sum_mid / voiced;
    f->high_ratio = ma->sum_high / voiced;
    f->centroid_hz = ma->sum_centroid / voiced;
    f->flux_mean = ma->sum_flux / voiced;
    if (f->flux_mean > 0) {
        double var = 0;
        for (int i = 0; i < MA_WINDOW_HOPS; i++) {
            double d = ma->onset_env[i] - f->flux_mean;
            var += d * d;
        }
        f->flux_contrast = sqrt(var / MA_WINDOW_HOPS) / f->flux_mean;
    }
    f->onset_rate = ma->onsets / seconds;
    f->silent_ratio = (float)ma->silent_hops / MA_WINDOW_HOPS;
    estimate_tempo(ma, &f->bpm, &f->regularity);
    estimate_key(ma, f);

    ma->hop_in_window = 0;
    ma->sum_bass = ma->sum_mid = ma->sum_high = ma->sum_centroid = ma->sum_flux = 0;
    ma->onsets = ma->silent_hops = ma->voiced_hops = 0;
    memset(ma->chroma, 0, sizeof(ma->chroma));
}

bool music_analyzer_process(music_analyzer_t *ma, const float *samples, music_frame_t *frame,
                            music_features_t *features)
{
    const float bin_hz = (float)ma->sample_rate / MA_FFT_SIZE;
    const int bass_end = (int)(250.0f / bin_hz);
    const int mid_end = (int)(2000.0f / bin_hz);

    memset(frame, 0, sizeof(*frame));

    /* Slide the buffer and compute the hop RMS */
    memmove(ma->buf, ma->buf + MA_HOP_SIZE, (MA_CHROMA_FFT - MA_HOP_SIZE) * sizeof(float));
    memcpy(ma->buf + MA_CHROMA_FFT - MA_HOP_SIZE, samples, MA_HOP_SIZE * sizeof(float));
    const float *recent = ma->buf + MA_CHROMA_FFT - MA_FFT_SIZE;
    float energy = 0;
    for (int i = 0; i < MA_HOP_SIZE; i++) {
        energy += samples[i] * samples[i];
    }
    float rms = sqrtf(energy / MA_HOP_SIZE);
    frame->level_db = 20.0f * log10f(rms + 1e-7f);
    frame->silent = frame->level_db < ma->silence_db;

    /* Auto gain: follow peaks quickly, release over a few seconds */
    if (rms > ma->peak_rms) {
        ma->peak_rms = rms;
    } else {
        ma->peak_rms = fmaxf(ma->peak_rms * 0.997f, 1e-4f);
    }
    frame->level = frame->silent ? 0 : fminf(rms / ma->peak_rms, 1.0f);

    /* Spectrum */
    for (int i = 0; i < MA_FFT_SIZE; i++) {
        ma->re[i] = recent[i] * ma->window[i];
        ma->im[i] = 0;
    }
    fft(ma, MA_FFT_SIZE);

    float e_bass = 0, e_mid = 0, e_high = 0, weighted = 0, flux = 0;
    for (int k = 1; k < BINS; k++) {
        float p = ma->re[k] * ma->re[k] + ma->im[k] * ma->im[k];
        float logmag = logf(1.0f + 100.0f * sqrtf(p));
        float d = logmag - ma->prev_logmag[k];
        if (d > 0) {
            flux += d;
        }
        ma->prev_logmag[k] = logmag;
        if (k <= bass_end) {
            e_bass += p;
        } else if (k <= mid_end) {
            e_mid += p;
        } else {
            e_high += p;
        }
        weighted += p * k * bin_hz;
    }
    float total = e_bass + e_mid + e_high + 1e-12f;
    frame->bass = e_bass / total;
    frame->mid = e_mid / total;
    frame->high = e_high / total;
    flux /= BINS;
    if (frame->silent) {
        flux = 0;
    }

    /* Onset: flux above an adaptive threshold, at most one per 250 ms */
    float mean = 0, var = 0;
    for (int i = 0; i < FLUX_HIST_LEN; i++) {
        mean += ma->flux_hist[i];
    }
    mean /= FLUX_HIST_LEN;
    for (int i = 0; i < FLUX_HIST_LEN; i++) {
        float d = ma->flux_hist[i] - mean;
        var += d * d;
    }
    /* The ratio term keeps steady tones with slight flux jitter from triggering */
    float threshold = fmaxf(mean + ONSET_K * sqrtf(var / FLUX_HIST_LEN), ONSET_RATIO * mean) + 1e-3f;
    ma->flux_hist[ma->flux_hist_pos] = flux;
    ma->flux_hist_pos = (ma->flux_hist_pos + 1) % FLUX_HIST_LEN;

    int min_gap = (int)(0.25f * ma->sample_rate / MA_HOP_SIZE);
    ma->hops_since_onset++;
    if (!frame->silent && flux > threshold && ma->hops_since_onset >= min_gap) {
        frame->beat = true;
        frame->beat_strength = fminf((flux - threshold) / threshold, 1.0f);
        ma->hops_since_onset = 0;
        ma->onsets++;
    }

    /* Window accumulation */
    ma->onset_env[ma->hop_in_window] = flux;
    ma->level_db[ma->hop_in_window] = frame->level_db;
    if (frame->silent) {
        ma->silent_hops++;
    } else {
        if (++ma->hop_count % MA_CHROMA_EVERY == 0) {
            accumulate_chroma(ma);
        }
        ma->voiced_hops++;
        ma->sum_bass += frame->bass;
        ma->sum_mid += frame->mid;
        ma->sum_high += frame->high;
        ma->sum_centroid += weighted / total;
        ma->sum_flux += flux;
    }
    if (++ma->hop_in_window == MA_WINDOW_HOPS) {
        finish_window(ma, features);
        return true;
    }
    return false;
}
