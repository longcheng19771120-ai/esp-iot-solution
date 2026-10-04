/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "gateway.h"

#define WIFI_RETRY_MIN_MS 1000

static const char *TAG = "gw_wifi";

static char s_gateway_id[13];
static bool s_connected;
static bool s_mqtt_started;
static uint32_t s_retry_ms = WIFI_RETRY_MIN_MS;
static TimerHandle_t s_retry_timer;

const char *gateway_get_id(void)
{
    return s_gateway_id;
}

bool gateway_wifi_is_connected(void)
{
    return s_connected;
}

static void retry_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Reconnecting to AP...");
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected (reason %d), retry in %" PRIu32 " ms", evt->reason, s_retry_ms);
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        s_retry_ms *= 2;
        if (s_retry_ms > CONFIG_GATEWAY_WIFI_MAX_RETRY_INTERVAL_MS) {
            s_retry_ms = CONFIG_GATEWAY_WIFI_MAX_RETRY_INTERVAL_MS;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        s_retry_ms = WIFI_RETRY_MIN_MS;
        if (!s_mqtt_started) {
            /* esp-mqtt reconnects on its own afterwards, start it only once */
            s_mqtt_started = true;
            ESP_ERROR_CHECK(gateway_mqtt_start());
        }
    }
}

esp_err_t gateway_wifi_start(void)
{
    uint8_t mac[6];

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_gateway_id, sizeof(s_gateway_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Gateway id: %s", s_gateway_id);

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(WIFI_RETRY_MIN_MS), pdFALSE, NULL, retry_timer_cb);
    if (s_retry_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_GATEWAY_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_GATEWAY_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    if (strlen(CONFIG_GATEWAY_WIFI_PASSWORD) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to SSID \"%s\"", CONFIG_GATEWAY_WIFI_SSID);
    return ESP_OK;
}
