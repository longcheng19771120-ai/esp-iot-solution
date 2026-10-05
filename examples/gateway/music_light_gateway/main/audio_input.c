/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "gateway.h"

/*
 * Digital MEMS microphone (INMP441, ICS-43434, SPH0645 ...) on standard I2S.
 * These output 24-bit samples left aligned in a 32-bit slot. While the
 * Bluetooth receiver board plays music, its samples are analysed instead.
 */
#define SAMPLE_RATE CONFIG_AUDIO_SAMPLE_RATE

/* A new genre takes over after two agreeing windows, or one confident one */
#define GENRE_STABLE_WINDOWS 2
#define GENRE_FAST_CONFIDENCE 0.8f

static const char *TAG = "audio";

static i2s_chan_handle_t s_rx;
static music_analyzer_t s_ma;

static void audio_task(void *arg)
{
    static int32_t raw[MA_HOP_SIZE];
    static float samples[MA_HOP_SIZE];
    music_frame_t frame;
    music_features_t features;
    music_genre_t current = MUSIC_GENRE_SILENCE, candidate = MUSIC_GENRE_SILENCE;
    int candidate_count = 0;
    bool use_bt = false;

    while (1) {
        bool bt = bt_link_active();
        if (bt != use_bt) {
            /* Tempo and loudness history from the other source would only mislead */
            ESP_LOGI(TAG, "Listening to %s", bt ? "Bluetooth" : "the microphone");
            use_bt = bt;
            bt_link_set_consuming(bt);
            music_analyzer_init(&s_ma, SAMPLE_RATE, CONFIG_AUDIO_SILENCE_DB);
        }

        if (use_bt) {
            /* The microphone DMA simply overruns meanwhile; its oldest data is dropped */
            if (!bt_link_read(samples, MA_HOP_SIZE, 100)) {
                continue;
            }
        } else {
            size_t got = 0;
            if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, portMAX_DELAY) != ESP_OK || got != sizeof(raw)) {
                continue;
            }
            for (int i = 0; i < MA_HOP_SIZE; i++) {
                samples[i] = (float)(raw[i] >> 8) / 8388608.0f;
            }
        }

        bool window_done = music_analyzer_process(&s_ma, samples, &frame, &features);
        light_on_frame(&frame);
        display_on_frame(&frame);
        if (!window_done) {
            continue;
        }

        /* The light smooths the mood itself, so every window is passed on */
        mood_t mood = mood_estimate(&features);
        light_set_mood(&mood);
        genre_result_t res = genre_classify(&features);
        ESP_LOGI(TAG, "%-10s conf %.2f | %-7s valence %.2f energy %.2f | mode %+.2f key %.2f | bpm %5.1f reg %.2f | "
                 "onsets %.1f/s | %5.1f dBFS", genre_name(res.genre), res.confidence,
                 mood_quadrant_name(mood_quadrant(&mood)), mood.valence, mood.energy, features.mode,
                 features.key_strength, features.bpm, features.regularity, features.onset_rate,
                 features.level_db_mean);

        if (res.genre == candidate) {
            candidate_count++;
        } else {
            candidate = res.genre;
            candidate_count = 1;
        }
        if (candidate != current &&
                (candidate_count >= GENRE_STABLE_WINDOWS || res.confidence >= GENRE_FAST_CONFIDENCE)) {
            ESP_LOGI(TAG, "Genre: %s -> %s", genre_name(current), genre_name(candidate));
            current = candidate;
            light_set_genre(current);
            gateway_mqtt_publish_light_state();
        }
        display_show_music(&mood, &features);
        gateway_mqtt_publish_music(current, res.confidence, &mood, &features);
    }
}

esp_err_t audio_input_start(void)
{
    music_analyzer_init(&s_ma, SAMPLE_RATE, CONFIG_AUDIO_SILENCE_DB);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_AUDIO_I2S_BCLK_GPIO,
            .ws = CONFIG_AUDIO_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_AUDIO_I2S_DIN_GPIO,
        },
    };
#if CONFIG_AUDIO_MIC_SLOT_RIGHT
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    ESP_LOGI(TAG, "Mic on BCLK %d WS %d DIN %d, %d Hz", CONFIG_AUDIO_I2S_BCLK_GPIO, CONFIG_AUDIO_I2S_WS_GPIO,
             CONFIG_AUDIO_I2S_DIN_GPIO, SAMPLE_RATE);

    /* FFT, tempo and key estimation need a few KB of stack */
    if (xTaskCreate(audio_task, "audio", 8192, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
