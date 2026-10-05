/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "gateway.h"

#if CONFIG_GATEWAY_BUTTON_GPIO >= 0
static const char *TAG = "gateway";

static void button_click_cb(void *button, void *user_data)
{
    light_next_mode();
    gateway_mqtt_publish_light_state();
}

static void button_long_press_cb(void *button, void *user_data)
{
    ESP_LOGW(TAG, "Long press, rebooting");
    esp_restart();
}
#endif

static void button_init(void)
{
#if CONFIG_GATEWAY_BUTTON_GPIO >= 0
    const button_config_t btn_cfg = {
        .long_press_time = 5000,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = CONFIG_GATEWAY_BUTTON_GPIO,
        .active_level = 0,
    };
    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_click_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_long_press_cb, NULL));
#endif
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Light first so the room is lit even before the network comes up */
    ESP_ERROR_CHECK(light_rgbw_start());
    ESP_ERROR_CHECK(audio_input_start());
    /* Satellite lights are extra: the gateway's own light runs without them */
    if (light_sync_start() != ESP_OK) {
        ESP_LOGW("gateway", "Bluetooth light sync not started");
    }
    /* Without the receiver board the analysis simply stays on the microphone */
    if (bt_link_start() != ESP_OK) {
        ESP_LOGW("gateway", "Bluetooth receiver link not started");
    }
    /* A missing or faulty screen must not stop the light */
    if (display_start() != ESP_OK) {
        ESP_LOGW("gateway", "Display not started, continuing without it");
    }
    button_init();
    /* MQTT is started from the Wi-Fi event handler once an IP is assigned */
    ESP_ERROR_CHECK(gateway_wifi_start());
}
