/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

/*
 * Bluetooth receiver for the music light gateway. A phone connects over
 * Bluetooth (A2DP); the audio goes out on I2S to a speaker amplifier such as
 * the MAX98357A, and the gateway listens on the same wires. Connection, play
 * state and track title are sent to the gateway over UART as text lines:
 *   conn | disc | play | stop | title <text> | artist <text>
 */
#define LINK_UART           UART_NUM_2
#define RING_SIZE           (32 * 1024)     /* about 190 ms at 44.1 kHz stereo */
#define PREFILL             (RING_SIZE / 2) /* buffer this much before playing, to ride out radio gaps */

/* AVRCP transaction labels */
enum {
    TL_CAPS = 0,
    TL_METADATA,
    TL_RN_TRACK,
    TL_RN_PLAY,
};

static const char *TAG = "bt_rx";

static i2s_chan_handle_t s_tx;
static RingbufHandle_t s_ring;
static volatile bool s_mono;
static esp_avrc_rn_evt_cap_mask_t s_peer_caps;

static void link_send(const char *fmt, ...)
{
    char line[160];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    n = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 2;
    line[n++] = '\n';
    uart_write_bytes(LINK_UART, line, n);
    ESP_LOGI(TAG, "-> %.*s", n - 1, line);
}

static void i2s_set_rate(int rate)
{
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    /* The audio PLL hits 44.1 kHz exactly */
    clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    i2s_channel_disable(s_tx);
    i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
    i2s_channel_enable(s_tx);
}

static void i2s_task(void *arg)
{
    bool buffering = true;
    while (1) {
        if (buffering) {
            /* Wait for a cushion so short radio gaps do not click */
            if (RING_SIZE - xRingbufferGetCurFreeSize(s_ring) < PREFILL) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            buffering = false;
        }
        size_t size = 0;
        void *data = xRingbufferReceiveUpTo(s_ring, &size, pdMS_TO_TICKS(50), 4096);
        if (!data) {
            /* Underrun or paused: the DMA sends silence (auto_clear) until data returns */
            buffering = true;
            continue;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, data, size, &written, portMAX_DELAY);
        vRingbufferReturnItem(s_ring, data);
    }
}

/* Decoded PCM from the Bluetooth stack: 16-bit, stereo unless the phone sends mono */
static void a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (!s_mono) {
        xRingbufferSend(s_ring, data, len, 0);
        return;
    }
    static int16_t stereo[512];
    const int16_t *in = (const int16_t *)data;
    uint32_t count = len / 2;
    while (count) {
        uint32_t n = count < 256 ? count : 256;
        for (uint32_t i = 0; i < n; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = in[i];
        }
        xRingbufferSend(s_ring, stereo, n * 4, 0);
        in += n;
        count -= n;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            link_send("conn");
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            link_send("disc");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        link_send(param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED ? "play" : "stop");
        break;
    case ESP_A2D_AUDIO_CFG_EVT: {
        if (param->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) {
            break;
        }
        uint8_t oct0 = param->audio_cfg.mcc.cie.sbc[0];
        int rate = 16000;
        if (oct0 & (1 << 6)) {
            rate = 32000;
        } else if (oct0 & (1 << 5)) {
            rate = 44100;
        } else if (oct0 & (1 << 4)) {
            rate = 48000;
        }
        s_mono = oct0 & (1 << 3);
        ESP_LOGI(TAG, "SBC %d Hz %s", rate, s_mono ? "mono" : "stereo");
        i2s_set_rate(rate);
        break;
    }
    default:
        break;
    }
}

static void request_track(void)
{
    esp_avrc_ct_send_metadata_cmd(TL_METADATA, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
}

static void register_notification(uint8_t tl, uint8_t event_id)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_peer_caps, event_id)) {
        esp_avrc_ct_send_register_notification_cmd(tl, event_id, 0);
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(TL_CAPS);
        } else {
            s_peer_caps.bits = 0;
        }
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_peer_caps = param->get_rn_caps_rsp.evt_set;
        request_track();
        register_notification(TL_RN_TRACK, ESP_AVRC_RN_TRACK_CHANGE);
        register_notification(TL_RN_PLAY, ESP_AVRC_RN_PLAY_STATUS_CHANGE);
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        /* The text is not terminated, and a title must stay on one line */
        char text[128];
        int n = param->meta_rsp.attr_length < (int)sizeof(text) - 1 ? param->meta_rsp.attr_length : (int)sizeof(text) - 1;
        memcpy(text, param->meta_rsp.attr_text, n);
        text[n] = '\0';
        for (char *c = text; *c; c++) {
            if (*c == '\n' || *c == '\r') {
                *c = ' ';
            }
        }
        if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE) {
            link_send("title %s", text);
        } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ARTIST) {
            link_send("artist %s", text);
        }
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        /* Notifications fire once; register again for the next change */
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            request_track();
            register_notification(TL_RN_TRACK, ESP_AVRC_RN_TRACK_CHANGE);
        } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            link_send(param->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING ? "play" : "stop");
            register_notification(TL_RN_PLAY, ESP_AVRC_RN_PLAY_STATUS_CHANGE);
        }
        break;
    default:
        break;
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "Pairing failed: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* No display or keyboard on this board: accept the phone's pairing request */
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

static void output_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BT_RX_I2S_BCLK_GPIO,
            .ws = CONFIG_BT_RX_I2S_WS_GPIO,
            .dout = CONFIG_BT_RX_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    /* The clock runs all the time so the gateway always sees a valid stream */
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    s_ring = xRingbufferCreate(RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    assert(s_ring);
    xTaskCreate(i2s_task, "i2s_out", 3072, NULL, configMAX_PRIORITIES - 3, NULL);

    const uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 256, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_BT_RX_UART_TX_GPIO, CONFIG_BT_RX_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    output_init();

    /* Classic Bluetooth only */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Secure Simple Pairing without input or output, PIN 1234 for old phones */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);

    esp_bt_gap_set_device_name(CONFIG_BT_RX_DEVICE_NAME);
    esp_bt_gap_register_callback(gap_cb);
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    esp_avrc_ct_register_callback(avrc_ct_cb);
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(a2d_data_cb);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    link_send("disc");
    ESP_LOGI(TAG, "Ready, pair with \"%s\"", CONFIG_BT_RX_DEVICE_NAME);
}
