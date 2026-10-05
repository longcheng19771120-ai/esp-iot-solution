/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
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
 *   <base>/status       retained "online"/"offline" (offline is the LWT)
 *   <base>/telemetry    periodic JSON with uptime, heap and RSSI
 *   <base>/music        mood, tempo and features after every analysis window
 *   <base>/light/state  retained light mode, brightness and static color
 *   <base>/bt           retained Bluetooth receiver state and current track
 *   <base>/cmd          subscribed, see handle_cmd()
 *   <base>/resp         replies to commands
 */
#define TOPIC_LEN 96

static const char *TAG = "gw_mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_base[TOPIC_LEN - 16];
static char s_topic_status[TOPIC_LEN];
static char s_topic_telemetry[TOPIC_LEN];
static char s_topic_music[TOPIC_LEN];
static char s_topic_light[TOPIC_LEN];
static char s_topic_bt[TOPIC_LEN];
static char s_topic_cmd[TOPIC_LEN];
static char s_topic_resp[TOPIC_LEN];

/* Calibration: label attached to /music messages, see host_test/collect_mqtt.py */
static bool s_labelled;                 /* mood label set */
static float s_label_valence, s_label_energy;
static music_genre_t s_label_genre = MUSIC_GENRE_MAX;   /* MAX = no genre label */
static int s_label_session;
static portMUX_TYPE s_label_lock = portMUX_INITIALIZER_UNLOCKED;

bool gateway_mqtt_is_connected(void)
{
    return s_connected;
}

static esp_err_t publish(const char *topic, const char *data, size_t len, int qos, bool retain)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Enqueue so callers never block on the network, even from the audio task */
    int msg_id = esp_mqtt_client_enqueue(s_client, topic, data, len, qos, retain, true);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

static int append_vector(char *buf, int n, int size, const char *key, const float *v, int count)
{
    n += snprintf(buf + n, size - n, ",\"%s\":[", key);
    for (int i = 0; i < count && n < size; i++) {
        n += snprintf(buf + n, size - n, "%s%.4f", i ? "," : "", v[i]);
    }
    if (n < size) {
        n += snprintf(buf + n, size - n, "]");
    }
    return n;
}

esp_err_t gateway_mqtt_publish_music(music_genre_t genre, float genre_confidence, const mood_t *mood,
                                     const music_features_t *f)
{
    char buf[768];
    bool labelled;
    float label_v, label_e;
    music_genre_t label_genre;
    int session;
    float v[MOOD_FEATURE_COUNT], gv[GENRE_FEATURE_COUNT];

    portENTER_CRITICAL(&s_label_lock);
    labelled = s_labelled;
    label_genre = s_label_genre;
    label_v = s_label_valence;
    label_e = s_label_energy;
    session = s_label_session;
    portEXIT_CRITICAL(&s_label_lock);

    int n = snprintf(buf, sizeof(buf),
                     "{\"genre\":\"%s\",\"genre_conf\":%.2f,\"mood\":\"%s\",\"valence\":%.2f,\"energy\":%.2f,\"bpm\":%.1f,\"regularity\":%.2f,"
                     "\"mode\":%.2f,\"key_strength\":%.2f,\"level_db\":%.1f,\"onsets_per_s\":%.2f,"
                     "\"bass\":%.2f,\"mid\":%.2f,\"high\":%.2f,\"silent\":%.2f,\"source\":\"%s\"",
                     genre_name(genre), genre_confidence, mood_quadrant_name(mood_quadrant(mood)), mood->valence, mood->energy, f->bpm, f->regularity,
                     f->mode, f->key_strength, f->level_db_mean, f->onset_rate, f->bass_ratio, f->mid_ratio,
                     f->high_ratio, f->silent_ratio, bt_link_active() ? "bt" : "mic");
    mood_feature_vector(f, v);
    genre_feature_vector(f, gv);
    n = append_vector(buf, n, sizeof(buf), "vec", v, MOOD_FEATURE_COUNT);
    n = append_vector(buf, n, sizeof(buf), "gvec", gv, GENRE_FEATURE_COUNT);
    if (n < (int)sizeof(buf) && labelled) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"label\":[%.2f,%.2f]", label_v, label_e);
    }
    if (n < (int)sizeof(buf) && label_genre < MUSIC_GENRE_MAX) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"genre_label\":\"%s\"", genre_name(label_genre));
    }
    if (n < (int)sizeof(buf) && (labelled || label_genre < MUSIC_GENRE_MAX)) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"session\":%d", session);
    }
    if (n < (int)sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - n, "}");
    }
    if (n >= (int)sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return publish(s_topic_music, buf, n, 0, false);
}

esp_err_t gateway_mqtt_publish_light_state(void)
{
    char buf[160];
    int n = light_get_state_json(buf, sizeof(buf));
    return publish(s_topic_light, buf, n, 1, true);
}

#if CONFIG_BT_LINK_ENABLE
/* Copies src into a JSON string body, escaping quotes, backslashes and control characters */
static void json_escape(char *dst, int size, const char *src)
{
    int n = 0;
    for (; *src && n < size - 7; src++) {
        unsigned char c = (unsigned char) * src;
        if (c == '"' || c == '\\') {
            dst[n++] = '\\';
            dst[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(dst + n, size - n, "\\u%04x", c);
        } else {
            dst[n++] = c;
        }
    }
    dst[n] = '\0';
}

esp_err_t gateway_mqtt_publish_bt(void)
{
    bt_status_t st;
    char title[2 * sizeof(st.title)], artist[2 * sizeof(st.artist)], buf[384];
    bt_link_get_status(&st);
    json_escape(title, sizeof(title), st.title);
    json_escape(artist, sizeof(artist), st.artist);
    int n = snprintf(buf, sizeof(buf), "{\"connected\":%s,\"playing\":%s,\"sample_rate\":%d,\"title\":\"%s\",\"artist\":\"%s\"}",
                     st.connected ? "true" : "false", st.playing ? "true" : "false", st.sample_rate, title, artist);
    return publish(s_topic_bt, buf, n, 1, true);
}
#else
esp_err_t gateway_mqtt_publish_bt(void)
{
    return ESP_OK;
}
#endif

static void reply(const char *text)
{
    publish(s_topic_resp, text, strlen(text), 1, false);
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

/*
 * Commands (plain text):
 *   ping | info | reboot
 *   mode music|static|off
 *   color <r> <g> <b> <w>     0..255 each, switches to static mode
 *   brightness <0..100>
 *   theme vivid|song         color theme, kept across reboots
 *   label [<genre>] [<calm|happy|tense|sad>|<valence> <energy>] | none
 *                             tag /music messages for calibration, e.g. "label rock", "label pop happy"
 */
static music_genre_t genre_from_name(const char *name)
{
    for (int g = 0; g < MUSIC_GENRE_MAX; g++) {
        if (strcmp(name, genre_name((music_genre_t)g)) == 0) {
            return (music_genre_t)g;
        }
    }
    return MUSIC_GENRE_MAX;
}

/* "rock", "happy", "rock happy", "pop 0.8 0.7" or "none" */
static bool parse_label(char *args)
{
    music_genre_t genre = MUSIC_GENRE_MAX;
    bool has_mood = false;
    float lv = 0, le = 0, nums[2];
    int num_count = 0;

    if (strcmp(args, "none") != 0) {
        for (char *save = NULL, *tok = strtok_r(args, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            char *end;
            float x = strtof(tok, &end);
            if (*end == '\0' && num_count < 2) {
                nums[num_count++] = x;
            } else if (genre == MUSIC_GENRE_MAX && genre_from_name(tok) < MUSIC_GENRE_MAX) {
                genre = genre_from_name(tok);
            } else if (!has_mood && mood_quadrant_target(tok, &lv, &le)) {
                has_mood = true;
            } else {
                return false;
            }
        }
        if (num_count == 2 && !has_mood) {
            has_mood = true;
            lv = nums[0];
            le = nums[1];
        } else if (num_count != 0 || (genre == MUSIC_GENRE_MAX && !has_mood)) {
            return false;
        }
    }

    portENTER_CRITICAL(&s_label_lock);
    s_labelled = has_mood;
    s_label_valence = lv;
    s_label_energy = le;
    s_label_genre = genre;
    /* Every label command starts a new session, so each song can be told apart */
    s_label_session++;
    portEXIT_CRITICAL(&s_label_lock);
    return true;
}

static void handle_cmd(const char *data, int len)
{
    char cmd[64], buf[256], arg[16];
    int r, g, b, w, v;

    if (len <= 0 || len >= (int)sizeof(cmd)) {
        reply("error: bad length");
        return;
    }
    memcpy(cmd, data, len);
    cmd[len] = '\0';

    if (strcmp(cmd, "ping") == 0) {
        reply("pong");
        return;
    } else if (strcmp(cmd, "info") == 0) {
        build_telemetry(buf, sizeof(buf));
        reply(buf);
        return;
    } else if (strcmp(cmd, "reboot") == 0) {
        ESP_LOGW(TAG, "Reboot requested over MQTT");
        /* Publish synchronously so the reply goes out before restarting */
        esp_mqtt_client_publish(s_client, s_topic_resp, "rebooting", 0, 1, false);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (sscanf(cmd, "mode %15s", arg) == 1) {
        light_mode_t mode = LIGHT_MODE_MAX;
        for (int m = 0; m < LIGHT_MODE_MAX; m++) {
            if (strcmp(arg, light_mode_name((light_mode_t)m)) == 0) {
                mode = (light_mode_t)m;
            }
        }
        if (mode == LIGHT_MODE_MAX) {
            reply("error: mode must be music, static or off");
            return;
        }
        light_set_mode(mode);
    } else if (sscanf(cmd, "theme %15s", arg) == 1) {
        light_theme_t theme = LIGHT_THEME_MAX;
        for (int t = 0; t < LIGHT_THEME_MAX; t++) {
            if (strcmp(arg, light_theme_name((light_theme_t)t)) == 0) {
                theme = (light_theme_t)t;
            }
        }
        if (theme == LIGHT_THEME_MAX) {
            reply("error: theme must be vivid or song");
            return;
        }
        light_set_theme(theme);
    } else if (sscanf(cmd, "color %d %d %d %d", &r, &g, &b, &w) == 4) {
        light_set_color((rgbw_t) {
            r / 255.0f, g / 255.0f, b / 255.0f, w / 255.0f
        });
    } else if (sscanf(cmd, "brightness %d", &v) == 1) {
        light_set_brightness(v / 100.0f);
    } else if (strncmp(cmd, "label ", 6) == 0) {
        if (!parse_label(cmd + 6)) {
            reply("error: label takes a genre, a mood (calm, happy, tense, sad or '<valence> <energy>'), "
                  "both, or none");
            return;
        }
    } else {
        snprintf(buf, sizeof(buf), "error: unknown command: %s", cmd);
        reply(buf);
        return;
    }
    reply("ok");
    gateway_mqtt_publish_light_state();
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 1, true);
        gateway_mqtt_publish_light_state();
        gateway_mqtt_publish_bt();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
            ESP_LOGW(TAG, "Dropping fragmented message (%d bytes)", event->total_data_len);
            break;
        }
        if (event->topic_len == (int)strlen(s_topic_cmd) &&
                strncmp(event->topic, s_topic_cmd, event->topic_len) == 0) {
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
    snprintf(s_topic_music, TOPIC_LEN, "%s/music", s_base);
    snprintf(s_topic_light, TOPIC_LEN, "%s/light/state", s_base);
    snprintf(s_topic_bt, TOPIC_LEN, "%s/bt", s_base);
    snprintf(s_topic_cmd, TOPIC_LEN, "%s/cmd", s_base);
    snprintf(s_topic_resp, TOPIC_LEN, "%s/resp", s_base);

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
