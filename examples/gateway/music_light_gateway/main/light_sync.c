/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "gateway.h"
#include "light_sync_proto.h"

#if CONFIG_LIGHT_SYNC_ENABLE

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

/*
 * Bluetooth LE light sync: the gateway broadcasts what its own LEDs show, so
 * any number of satellite lights (../sync_light) follow the music in step.
 * There is no pairing or connection: the color rides in non-connectable
 * advertising packets, sent every 20 ms and refreshed every 30 ms, which
 * every satellite in range hears at once.
 */
#define ADV_INTERVAL_MS     20
#define UPDATE_MS           30

static const char *TAG = "light_sync";

static volatile bool s_ready;
static uint8_t s_seq;

static void build_packet(const rgbw_t *c, uint8_t *adv, int *len)
{
    const float ch[4] = {c->r, c->g, c->b, c->w};
    light_sync_packet_t pkt = {
        .company = LIGHT_SYNC_COMPANY_ID,
        .magic = {LIGHT_SYNC_MAGIC_0, LIGHT_SYNC_MAGIC_1},
        .version = LIGHT_SYNC_VERSION,
        .group = CONFIG_LIGHT_SYNC_GROUP,
        .seq = s_seq,
    };
    for (int i = 0; i < 4; i++) {
        float v = ch[i] < 0 ? 0 : ch[i] > 1 ? 1 : ch[i];
        pkt.level[i] = (uint16_t)(v * 65535.0f + 0.5f);
    }
    adv[0] = sizeof(pkt) + 1;
    adv[1] = BLE_HS_ADV_TYPE_MFG_DATA;
    memcpy(adv + 2, &pkt, sizeof(pkt));
    *len = sizeof(pkt) + 2;
}

static void start_advertising(void)
{
    rgbw_t dark = {0};
    uint8_t adv[BLE_HS_ADV_MAX_SZ];
    int len;
    build_packet(&dark, adv, &len);
    ble_gap_adv_set_data(adv, len);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_NON,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS),
    };
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc == 0) {
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising failed: %d", rc);
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "Broadcasting the light to group %d", CONFIG_LIGHT_SYNC_GROUP);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    start_advertising();
}

static void on_reset(int reason)
{
    s_ready = false;
    ESP_LOGW(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void update_task(void *arg)
{
    rgbw_t last = {-1, -1, -1, -1};
    TickType_t wake = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(UPDATE_MS));
        if (!s_ready) {
            continue;
        }
        light_status_t st;
        light_get_status(&st);
        /* The controller repeats the last packet by itself, so only changes need sending */
        if (memcmp(&st.out, &last, sizeof(last)) == 0) {
            continue;
        }
        last = st.out;
        s_seq++;
        uint8_t adv[BLE_HS_ADV_MAX_SZ];
        int len;
        build_packet(&st.out, adv, &len);
        ble_gap_adv_set_data(adv, len);
    }
}

esp_err_t light_sync_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth init failed: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);

    /* Below the light and audio tasks: a late packet only delays the satellites */
    if (xTaskCreate(update_task, "light_sync", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else

esp_err_t light_sync_start(void)
{
    return ESP_OK;
}

#endif
