/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "gateway.h"

/*
 * Topic layout, <base> = <prefix>/<gateway_id>:
 *   <base>/status     retained "online"/"offline" (offline is the LWT)
 *   <base>/telemetry  periodic JSON with uptime, heap and RSSI
 *   <base>/up         frames received from sub-devices on UART
 *   <base>/down       subscribed, payload is written to UART
 *   <base>/cmd        subscribed, gateway commands: ping, info, reboot
 *   <base>/resp       replies to commands
 *   <base>/event      local gateway events (button)
 */
#define TOPIC_LEN 96

static const char *TAG = "gw_mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_base[TOPIC_LEN - 16];
static char s_topic_status[TOPIC_LEN];
static char s_topic_telemetry[TOPIC_LEN];
static char s_topic_up[TOPIC_LEN];
static char s_topic_down[TOPIC_LEN];
static char s_topic_cmd[TOPIC_LEN];
static char s_topic_resp[TOPIC_LEN];
static char s_topic_event[TOPIC_LEN];

bool gateway_mqtt_is_connected(void)
{
    return s_connected;
}

static esp_err_t publish(const char *topic, const char *data, size_t len, int qos, bool retain)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Enqueue so callers never block on the network, even from the UART task */
    int msg_id = esp_mqtt_client_enqueue(s_client, topic, data, len, qos, retain, true);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t gateway_mqtt_publish_uplink(const char *data, size_t len)
{
    return publish(s_topic_up, data, len, 1, false);
}

esp_err_t gateway_mqtt_publish_event(const char *event)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"event\":\"%s\"}", event);
    return publish(s_topic_event, buf, n, 1, false);
}

static int build_telemetry(char *buf, size_t size)
{
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    return snprintf(buf, size,
                    "{\"id\":\"%s\",\"fw\":\"%s\",\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32
                    ",\"min_free_heap\":%" PRIu32 ",\"rssi\":%d}",
                    gateway_get_id(), esp_app_get_description()->version,
                    esp_timer_get_time() / 1000000, esp_get_free_heap_size(),
                    esp_get_minimum_free_heap_size(), rssi);
}

static void handle_cmd(const char *cmd, int len)
{
    char buf[256];

    if (len == 4 && strncmp(cmd, "ping", 4) == 0) {
        publish(s_topic_resp, "pong", 4, 1, false);
    } else if (len == 4 && strncmp(cmd, "info", 4) == 0) {
        int n = build_telemetry(buf, sizeof(buf));
        publish(s_topic_resp, buf, n, 1, false);
    } else if (len == 6 && strncmp(cmd, "reboot", 6) == 0) {
        ESP_LOGW(TAG, "Reboot requested over MQTT");
        /* Publish synchronously so the reply goes out before restarting */
        esp_mqtt_client_publish(s_client, s_topic_resp, "rebooting", 0, 1, false);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        int n = snprintf(buf, sizeof(buf), "unknown command: %.*s", len > 64 ? 64 : len, cmd);
        publish(s_topic_resp, buf, n, 1, false);
    }
}

static bool topic_is(const esp_mqtt_event_t *event, const char *topic)
{
    return event->topic_len == (int)strlen(topic) && strncmp(event->topic, topic, event->topic_len) == 0;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, s_topic_down, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 1, true);
        gateway_set_state(GATEWAY_STATE_ONLINE);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        gateway_set_state(gateway_wifi_is_connected() ? GATEWAY_STATE_MQTT_CONNECTING : GATEWAY_STATE_WIFI_CONNECTING);
        break;
    case MQTT_EVENT_DATA:
        /* Fragmented messages (larger than the MQTT buffer) are ignored */
        if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
            ESP_LOGW(TAG, "Dropping fragmented message (%d bytes)", event->total_data_len);
            break;
        }
        if (topic_is(event, s_topic_down)) {
            gateway_uart_send(event->data, event->data_len);
        } else if (topic_is(event, s_topic_cmd)) {
            handle_cmd(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Transport error, errno %d", event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "MQTT error type %d", event->error_handle->error_type);
        }
        break;
    default:
        break;
    }
}

static void telemetry_task(void *arg)
{
    char buf[256];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_GATEWAY_TELEMETRY_INTERVAL_S * 1000));
        if (s_connected) {
            int n = build_telemetry(buf, sizeof(buf));
            publish(s_topic_telemetry, buf, n, 0, false);
        }
    }
}

esp_err_t gateway_mqtt_start(void)
{
    snprintf(s_base, sizeof(s_base), "%s/%s", CONFIG_GATEWAY_MQTT_TOPIC_PREFIX, gateway_get_id());
    snprintf(s_topic_status, TOPIC_LEN, "%s/status", s_base);
    snprintf(s_topic_telemetry, TOPIC_LEN, "%s/telemetry", s_base);
    snprintf(s_topic_up, TOPIC_LEN, "%s/up", s_base);
    snprintf(s_topic_down, TOPIC_LEN, "%s/down", s_base);
    snprintf(s_topic_cmd, TOPIC_LEN, "%s/cmd", s_base);
    snprintf(s_topic_resp, TOPIC_LEN, "%s/resp", s_base);
    snprintf(s_topic_event, TOPIC_LEN, "%s/event", s_base);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_GATEWAY_MQTT_BROKER_URI,
        .credentials.client_id = gateway_get_id(),
                    .session.keepalive = 60,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "offline",
            .qos = 1,
            .retain = true,
        },
    };
    if (strlen(CONFIG_GATEWAY_MQTT_USERNAME) > 0) {
        cfg.credentials.username = CONFIG_GATEWAY_MQTT_USERNAME;
        cfg.credentials.authentication.password = CONFIG_GATEWAY_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_LOGI(TAG, "Connecting to %s, base topic %s", CONFIG_GATEWAY_MQTT_BROKER_URI, s_base);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    if (xTaskCreate(telemetry_task, "gw_telemetry", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
