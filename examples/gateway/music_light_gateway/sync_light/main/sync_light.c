/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "light_sync_proto.h"

/*
 * Satellite light for the music light gateway. It listens for the gateway's
 * Bluetooth LE light sync broadcasts and shows the same color on its own
 * RGBW driver. Nothing to pair: every satellite of the same group in range
 * follows. Without packets for a while the light fades out.
 */
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_RES        LEDC_TIMER_12_BIT
#define DUTY_MAX        ((1 << 12) - 1)
#define RENDER_HZ       100
#define SMOOTH_S        0.03f   /* hides the 30 ms update steps without blunting beats */
#define FADE_OUT_S      1.0f
#define GAMMA           2.2f

static const char *TAG = "sync_light";

static const int s_gpio[4] = {
    CONFIG_SYNC_R_GPIO, CONFIG_SYNC_G_GPIO, CONFIG_SYNC_B_GPIO, CONFIG_SYNC_W_GPIO,
};

static float s_target[4];
static volatile int64_t s_last_rx_us;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t to_duty(float v)
{
    float d = powf(v, GAMMA) * (CONFIG_SYNC_MAX_POWER_PERCENT / 100.0f);
#if !CONFIG_SYNC_ACTIVE_HIGH
    d = 1.0f - d;
#endif
    return (uint32_t)lroundf(d * DUTY_MAX);
}

static void render_task(void *arg)
{
    const float dt = 1.0f / RENDER_HZ;
    float out[4] = {0};
    TickType_t wake = xTaskGetTickCount();
    bool lost = true;

    while (1) {
        float target[4];
        portENTER_CRITICAL(&s_lock);
        memcpy(target, s_target, sizeof(target));
        portEXIT_CRITICAL(&s_lock);

        bool stale = esp_timer_get_time() - s_last_rx_us > CONFIG_SYNC_TIMEOUT_MS * 1000LL;
        if (stale != lost) {
            ESP_LOGI(TAG, "%s", stale ? "Gateway lost, fading out" : "Following the gateway");
            lost = stale;
        }
        float tau = stale ? FADE_OUT_S : SMOOTH_S;
        float k = 1.0f - expf(-dt / tau);
        for (int i = 0; i < 4; i++) {
            out[i] += ((stale ? 0.0f : target[i]) - out[i]) * k;
            ledc_set_duty(LEDC_MODE, (ledc_channel_t)i, to_duty(out[i]));
            ledc_update_duty(LEDC_MODE, (ledc_channel_t)i);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / RENDER_HZ));
    }
}

/* Finds our packet in the advertising data, which is a list of length, type, value fields */
static const light_sync_packet_t *find_packet(const uint8_t *data, int len)
{
    int i = 0;
    while (i + 1 < len) {
        int field = data[i];
        if (field == 0 || i + 1 + field > len) {
            break;
        }
        if (data[i + 1] == BLE_HS_ADV_TYPE_MFG_DATA && field - 1 == sizeof(light_sync_packet_t)) {
            const light_sync_packet_t *p = (const light_sync_packet_t *)(data + i + 2);
            if (p->company == LIGHT_SYNC_COMPANY_ID && p->magic[0] == LIGHT_SYNC_MAGIC_0 &&
                    p->magic[1] == LIGHT_SYNC_MAGIC_1 && p->version == LIGHT_SYNC_VERSION) {
                return p;
            }
        }
        i += field + 1;
    }
    return NULL;
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_scan(void)
{
    /* Listen all the time and keep repeats: every packet is a fresh color */
    const struct ble_gap_disc_params params = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(30),
        .window = BLE_GAP_SCAN_WIN_MS(30),
        .passive = 1,
        .filter_duplicates = 0,
    };
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc == 0) {
        rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const light_sync_packet_t *p = find_packet(event->disc.data, event->disc.length_data);
        if (!p || p->group != CONFIG_SYNC_GROUP) {
            break;
        }
        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < 4; i++) {
            s_target[i] = p->level[i] / 65535.0f;
        }
        portEXIT_CRITICAL(&s_lock);
        s_last_rx_us = esp_timer_get_time();
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    start_scan();
    ESP_LOGI(TAG, "Listening for group %d", CONFIG_SYNC_GROUP);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void light_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_RES,
        .freq_hz = CONFIG_SYNC_PWM_FREQ_HZ,
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
    xTaskCreate(render_task, "render", 3072, NULL, 5, NULL);
}

void app_main(void)
{
    /* The Bluetooth controller keeps its calibration in NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_last_rx_us = -CONFIG_SYNC_TIMEOUT_MS * 1000LL;
    light_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
