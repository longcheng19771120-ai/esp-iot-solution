/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs.h"
#include "gateway.h"

#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_RES        LEDC_TIMER_12_BIT
#define DUTY_MAX        ((1 << 12) - 1)
#define RENDER_HZ       100
#define GAMMA           2.2f
#define NVS_NAMESPACE   "light"
#define NVS_KEY_THEME   "theme"

static const char *TAG = "light";

static const int s_gpio[4] = {
    CONFIG_LIGHT_R_GPIO, CONFIG_LIGHT_G_GPIO, CONFIG_LIGHT_B_GPIO, CONFIG_LIGHT_W_GPIO,
};

static light_effect_t s_fx;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t to_duty(float v)
{
    /* Perceptual gamma, then the power limit that keeps the board within its thermal budget */
    float d = powf(v, GAMMA) * (CONFIG_LIGHT_MAX_POWER_PERCENT / 100.0f);
#if !CONFIG_LIGHT_ACTIVE_HIGH
    d = 1.0f - d;
#endif
    return (uint32_t)lroundf(d * DUTY_MAX);
}

static void render_task(void *arg)
{
    const float dt = 1.0f / RENDER_HZ;
    TickType_t last = xTaskGetTickCount();
    rgbw_t out;

    while (1) {
        portENTER_CRITICAL(&s_lock);
        light_effect_render(&s_fx, dt, &out);
        portEXIT_CRITICAL(&s_lock);

        const float ch[4] = {out.r, out.g, out.b, out.w};
        for (int i = 0; i < 4; i++) {
            ledc_set_duty(LEDC_MODE, (ledc_channel_t)i, to_duty(ch[i]));
            ledc_update_duty(LEDC_MODE, (ledc_channel_t)i);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / RENDER_HZ));
    }
}

esp_err_t light_rgbw_start(void)
{
    light_effect_init(&s_fx, (float)MA_HOP_SIZE / CONFIG_AUDIO_SAMPLE_RATE);
    nvs_handle_t nvs;
    uint8_t theme;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u8(nvs, NVS_KEY_THEME, &theme) == ESP_OK) {
            light_effect_set_theme(&s_fx, (light_theme_t)theme);
        }
        nvs_close(nvs);
    }

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_RES,
        .freq_hz = CONFIG_LIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int i = 0; i < 4; i++) {
        const ledc_channel_config_t ch = {
            .gpio_num = s_gpio[i],
            .speed_mode = LEDC_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER,
            .duty = to_duty(0),
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
    ESP_LOGI(TAG, "RGBW on GPIO %d/%d/%d/%d, %d Hz PWM, max power %d%%", s_gpio[0], s_gpio[1], s_gpio[2],
             s_gpio[3], CONFIG_LIGHT_PWM_FREQ_HZ, CONFIG_LIGHT_MAX_POWER_PERCENT);

    if (xTaskCreate(render_task, "light", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void light_on_frame(const music_frame_t *frame)
{
    portENTER_CRITICAL(&s_lock);
    light_effect_on_frame(&s_fx, frame);
    portEXIT_CRITICAL(&s_lock);
}

void light_set_mood(const mood_t *mood)
{
    portENTER_CRITICAL(&s_lock);
    light_effect_set_mood(&s_fx, mood);
    portEXIT_CRITICAL(&s_lock);
}

void light_set_genre(music_genre_t genre)
{
    portENTER_CRITICAL(&s_lock);
    light_effect_set_genre(&s_fx, genre);
    portEXIT_CRITICAL(&s_lock);
}

void light_set_theme(light_theme_t theme)
{
    if (theme >= LIGHT_THEME_MAX) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    light_effect_set_theme(&s_fx, theme);
    portEXIT_CRITICAL(&s_lock);

    /* Remembered across reboots */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_THEME, (uint8_t)theme);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void light_next_theme(void)
{
    portENTER_CRITICAL(&s_lock);
    light_theme_t theme = (light_theme_t)((s_fx.theme + 1) % LIGHT_THEME_MAX);
    portEXIT_CRITICAL(&s_lock);
    light_set_theme(theme);
}

void light_set_mode(light_mode_t mode)
{
    if (mode >= LIGHT_MODE_MAX) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_fx.mode = mode;
    portEXIT_CRITICAL(&s_lock);
}

void light_next_mode(void)
{
    portENTER_CRITICAL(&s_lock);
    s_fx.mode = (light_mode_t)((s_fx.mode + 1) % LIGHT_MODE_MAX);
    portEXIT_CRITICAL(&s_lock);
}

void light_set_color(rgbw_t color)
{
    portENTER_CRITICAL(&s_lock);
    s_fx.static_color = color;
    s_fx.mode = LIGHT_MODE_STATIC;
    portEXIT_CRITICAL(&s_lock);
}

void light_set_brightness(float brightness)
{
    portENTER_CRITICAL(&s_lock);
    s_fx.brightness = brightness < 0 ? 0 : (brightness > 1 ? 1 : brightness);
    portEXIT_CRITICAL(&s_lock);
}

int light_get_state_json(char *buf, size_t size)
{
    portENTER_CRITICAL(&s_lock);
    light_mode_t mode = s_fx.mode;
    rgbw_t c = s_fx.static_color;
    float bri = s_fx.brightness;
    music_genre_t genre = s_fx.genre;
    light_theme_t theme = s_fx.theme;
    portEXIT_CRITICAL(&s_lock);

    return snprintf(buf, size, "{\"mode\":\"%s\",\"theme\":\"%s\",\"genre\":\"%s\",\"brightness\":%d,\"color\":[%d,%d,%d,%d]}",
                    light_mode_name(mode), light_theme_name(theme), genre_name(genre), (int)lroundf(bri * 100), (int)lroundf(c.r * 255), (int)lroundf(c.g * 255),
                    (int)lroundf(c.b * 255), (int)lroundf(c.w * 255));
}

void light_get_status(light_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    status->mode = s_fx.mode;
    status->brightness = s_fx.brightness;
    status->genre = s_fx.genre;
    status->theme = s_fx.theme;
    status->out = s_fx.out;
    status->mood.silent = s_fx.silence > 0.5f;
    status->mood.valence = s_fx.valence;
    status->mood.energy = s_fx.energy;
    portEXIT_CRITICAL(&s_lock);
}
