/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gateway.h"

#if CONFIG_BT_LINK_ENABLE

/*
 * Bluetooth receiver board (see ../bt_receiver). The ESP32 on that board is
 * the I2S master and plays the phone's audio to the speaker amplifier; this
 * chip listens on the same three wires as an I2S slave, so the analysis gets
 * the music before it reaches the room. The board also reports connection,
 * play state and track over UART, one line per event.
 *
 * Phones send 44.1 or 48 kHz. The rate is measured from the incoming frames,
 * and the stereo stream is mixed to mono and resampled to the analysis rate
 * with a windowed-sinc filter that also removes what that rate cannot hold.
 */
#define OUT_RATE            CONFIG_AUDIO_SAMPLE_RATE
#define BLOCK_FRAMES        256
#define RS_TAPS             64
#define RS_PHASES           32
/* 7.3 kHz at 16 kHz: flat to 6 kHz, aliases from above 8.5 kHz down 30 dB or more */
#define RS_CUTOFF_HZ        (0.456f * OUT_RATE)
#define ACTIVE_DB           -70.0f      /* below this the stream counts as paused */
#define ACTIVE_HOLD_US      3000000     /* keep using it through short gaps between songs */
#define STREAM_SAMPLES      (4 * MA_HOP_SIZE)
#define UART_LINE_MAX       160
#define TEXT_MAX            64

static const char *TAG = "bt_link";

static i2s_chan_handle_t s_rx;
static StreamBufferHandle_t s_stream;
static volatile bool s_consuming;
static volatile int64_t s_active_until;

static bt_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Resampler state, only touched by the I2S task */
static float s_taps[RS_PHASES + 1][RS_TAPS];
static float s_hist[RS_TAPS + BLOCK_FRAMES];
static int s_hist_len;
static double s_pos;
static int s_in_rate;

static void resampler_init(int in_rate)
{
    const float fc = RS_CUTOFF_HZ / in_rate;
    for (int p = 0; p <= RS_PHASES; p++) {
        float sum = 0;
        for (int k = 0; k < RS_TAPS; k++) {
            /* Distance of tap k from the output instant, in input samples */
            float d = (k - (RS_TAPS / 2 - 1)) - (float)p / RS_PHASES;
            float x = 2.0f * fc * d;
            float sinc = fabsf(x) < 1e-6f ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
            float w = 0.42f + 0.5f * cosf((float)M_PI * d / (RS_TAPS / 2)) + 0.08f * cosf(2.0f * (float)M_PI * d / (RS_TAPS / 2));
            s_taps[p][k] = fabsf(d) < RS_TAPS / 2 ? sinc * w : 0;
            sum += s_taps[p][k];
        }
        for (int k = 0; k < RS_TAPS; k++) {
            s_taps[p][k] /= sum;
        }
    }
    memset(s_hist, 0, sizeof(s_hist));
    s_hist_len = RS_TAPS;
    s_pos = RS_TAPS / 2 - 1;
    s_in_rate = in_rate;
}

/* Appends n mono input samples, writes the resampled output, returns its length */
static int resample(const float *in, int n, float *out)
{
    memcpy(s_hist + s_hist_len, in, n * sizeof(float));
    s_hist_len += n;

    const double step = (double)s_in_rate / OUT_RATE;
    int count = 0;
    while (s_pos + RS_TAPS / 2 < s_hist_len) {
        int i = (int)s_pos;
        int p = (int)lround((s_pos - i) * RS_PHASES);
        const float *x = s_hist + i - (RS_TAPS / 2 - 1);
        float y = 0;
        for (int k = 0; k < RS_TAPS; k++) {
            y += x[k] * s_taps[p][k];
        }
        out[count++] = y;
        s_pos += step;
    }

    /* Keep the history the next output still needs */
    int drop = (int)s_pos - (RS_TAPS / 2 - 1);
    memmove(s_hist, s_hist + drop, (s_hist_len - drop) * sizeof(float));
    s_hist_len -= drop;
    s_pos -= drop;
    return count;
}

static int snap_rate(float measured)
{
    static const int rates[] = {16000, 32000, 44100, 48000};
    int best = rates[0];
    for (int i = 1; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (fabsf(measured - rates[i]) < fabsf(measured - best)) {
            best = rates[i];
        }
    }
    return best;
}

static void set_rate(int rate)
{
    portENTER_CRITICAL(&s_lock);
    s_status.sample_rate = rate;
    portEXIT_CRITICAL(&s_lock);
}

static void i2s_task(void *arg)
{
    static int16_t raw[BLOCK_FRAMES * 2];
    static float mono[BLOCK_FRAMES];
    static float out[BLOCK_FRAMES];
    int64_t count_start = esp_timer_get_time();
    int frames = 0;

    resampler_init(44100);
    while (1) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(200)) != ESP_OK || got != sizeof(raw)) {
            /* No bit clock: the board is off or not wired */
            if (s_status.sample_rate) {
                set_rate(0);
            }
            frames = 0;
            count_start = esp_timer_get_time();
            continue;
        }

        /* The rate follows the phone, so measure it from the bit clock */
        frames += BLOCK_FRAMES;
        int64_t now = esp_timer_get_time();
        if (now - count_start >= 1000000) {
            int rate = snap_rate(frames * 1e6f / (now - count_start));
            if (rate != s_in_rate) {
                ESP_LOGI(TAG, "I2S input at %d Hz", rate);
                resampler_init(rate);
            }
            if (rate != s_status.sample_rate) {
                set_rate(rate);
            }
            frames = 0;
            count_start = now;
        }

        float energy = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            mono[i] = (raw[2 * i] + raw[2 * i + 1]) * (0.5f / 32768.0f);
            energy += mono[i] * mono[i];
        }
        if (10.0f * log10f(energy / BLOCK_FRAMES + 1e-12f) > ACTIVE_DB) {
            s_active_until = now + ACTIVE_HOLD_US;
        }

        int n = resample(mono, BLOCK_FRAMES, out);
        if (s_consuming) {
            /* Never block: if the analysis falls behind, the newest samples are dropped */
            xStreamBufferSend(s_stream, out, n * sizeof(float), 0);
        }
    }
}

/* Truncates at a character boundary so a UTF-8 title is never cut mid-character */
static void copy_text(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= TEXT_MAX) {
        n = TEXT_MAX - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void handle_line(const char *line)
{
    bt_status_t old;
    portENTER_CRITICAL(&s_lock);
    old = s_status;
    if (strcmp(line, "conn") == 0) {
        s_status.connected = true;
    } else if (strcmp(line, "disc") == 0) {
        s_status.connected = false;
        s_status.playing = false;
        s_status.title[0] = s_status.artist[0] = '\0';
    } else if (strcmp(line, "play") == 0) {
        s_status.connected = true;
        s_status.playing = true;
    } else if (strcmp(line, "stop") == 0) {
        s_status.playing = false;
    } else if (strncmp(line, "title ", 6) == 0) {
        copy_text(s_status.title, line + 6);
    } else if (strncmp(line, "artist ", 7) == 0) {
        copy_text(s_status.artist, line + 7);
    }
    bool changed = memcmp(&old, &s_status, sizeof(old)) != 0;
    portEXIT_CRITICAL(&s_lock);

    if (changed) {
        ESP_LOGI(TAG, "%s", line);
        gateway_mqtt_publish_bt();
    }
}

static void uart_task(void *arg)
{
    static char line[UART_LINE_MAX];
    int len = 0;
    uint8_t c;
    while (1) {
        if (uart_read_bytes(CONFIG_BT_LINK_UART_NUM, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (len) {
                line[len] = '\0';
                handle_line(line);
                len = 0;
            }
        } else if (len < UART_LINE_MAX - 1) {
            line[len++] = (char)c;
        }
    }
}

esp_err_t bt_link_start(void)
{
    s_stream = xStreamBufferCreate(STREAM_SAMPLES * sizeof(float), MA_HOP_SIZE * sizeof(float));
    if (!s_stream) {
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_SLAVE);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "I2S channel");
    i2s_std_config_t std_cfg = {
        /* As a slave the rate only sizes the internal clock; the data follows the board's clock */
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BT_LINK_I2S_BCLK_GPIO,
            .ws = CONFIG_BT_LINK_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_BT_LINK_I2S_DIN_GPIO,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "I2S init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "I2S enable");

    const uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_BT_LINK_UART_NUM, 1024, 0, 0, NULL, 0), TAG, "UART");
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_BT_LINK_UART_NUM, &uart_cfg), TAG, "UART config");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_BT_LINK_UART_NUM, CONFIG_BT_LINK_UART_TX_GPIO, CONFIG_BT_LINK_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "UART pins");

    ESP_LOGI(TAG, "Receiver board on BCLK %d WS %d DIN %d, UART TX %d RX %d", CONFIG_BT_LINK_I2S_BCLK_GPIO,
             CONFIG_BT_LINK_I2S_WS_GPIO, CONFIG_BT_LINK_I2S_DIN_GPIO, CONFIG_BT_LINK_UART_TX_GPIO,
             CONFIG_BT_LINK_UART_RX_GPIO);

    /* Just below the analysis task, which waits on these samples */
    if (xTaskCreate(i2s_task, "bt_i2s", 4096, NULL, 6, NULL) != pdPASS ||
            xTaskCreate(uart_task, "bt_uart", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool bt_link_active(void)
{
    /* The play flag alone is not enough: without a bit clock no samples arrive */
    return (s_status.playing && s_status.sample_rate) || esp_timer_get_time() < s_active_until;
}

void bt_link_set_consuming(bool on)
{
    if (on && !s_consuming) {
        /* Start from fresh samples; safe because the I2S task never blocks on the buffer */
        xStreamBufferReset(s_stream);
    }
    s_consuming = on;
}

bool bt_link_read(float *samples, int count, int timeout_ms)
{
    size_t want = count * sizeof(float), got = 0;
    TickType_t start = xTaskGetTickCount(), wait = pdMS_TO_TICKS(timeout_ms);
    while (got < want) {
        TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= wait) {
            return false;
        }
        got += xStreamBufferReceive(s_stream, (uint8_t *)samples + got, want - got, wait - spent);
    }
    return true;
}

void bt_link_get_status(bt_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

#else

esp_err_t bt_link_start(void)
{
    return ESP_OK;
}

bool bt_link_active(void)
{
    return false;
}

void bt_link_set_consuming(bool on)
{
}

bool bt_link_read(float *samples, int count, int timeout_ms)
{
    return false;
}

void bt_link_get_status(bt_status_t *status)
{
    memset(status, 0, sizeof(*status));
}

#endif
