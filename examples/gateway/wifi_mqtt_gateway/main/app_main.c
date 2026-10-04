/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "led_indicator.h"
#include "led_indicator_gpio.h"
#include "gateway.h"

static led_indicator_handle_t s_led;
static gateway_state_t s_led_state = GATEWAY_STATE_MAX;
static SemaphoreHandle_t s_led_lock;

#if CONFIG_GATEWAY_LED_GPIO >= 0
/* Wi-Fi connecting: fast blink */
static const blink_step_t s_blink_wifi[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_LOOP, 0, 0},
};

/* Broker connecting: slow blink */
static const blink_step_t s_blink_mqtt[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 500},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0},
};

/* Online: solid on */
static const blink_step_t s_blink_online[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 1000},
    {LED_BLINK_LOOP, 0, 0},
};

/* Indexed by gateway_state_t */
static blink_step_t const *s_blink_lists[] = {
    [GATEWAY_STATE_WIFI_CONNECTING] = s_blink_wifi,
    [GATEWAY_STATE_MQTT_CONNECTING] = s_blink_mqtt,
    [GATEWAY_STATE_ONLINE] = s_blink_online,
};
#endif

void gateway_set_state(gateway_state_t state)
{
    if (s_led == NULL) {
        return;
    }
    /* Called from both the Wi-Fi event task and the MQTT task */
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    if (state != s_led_state) {
        if (s_led_state != GATEWAY_STATE_MAX) {
            led_indicator_stop(s_led, s_led_state);
        }
        led_indicator_start(s_led, state);
        s_led_state = state;
    }
    xSemaphoreGive(s_led_lock);
}

static void led_init(void)
{
#if CONFIG_GATEWAY_LED_GPIO >= 0
    const led_indicator_gpio_config_t gpio_cfg = {
        .gpio_num = CONFIG_GATEWAY_LED_GPIO,
        .is_active_level_high = CONFIG_GATEWAY_LED_ACTIVE_HIGH,
    };
    const led_indicator_config_t cfg = {
        .blink_lists = s_blink_lists,
        .blink_list_num = GATEWAY_STATE_MAX,
    };
    s_led_lock = xSemaphoreCreateMutex();
    assert(s_led_lock != NULL);
    ESP_ERROR_CHECK(led_indicator_new_gpio_device(&cfg, &gpio_cfg, &s_led));
#endif
}

#if CONFIG_GATEWAY_BUTTON_GPIO >= 0
static const char *TAG = "gateway";

static void button_click_cb(void *button, void *user_data)
{
    if (gateway_mqtt_publish_event("button_click") != ESP_OK) {
        ESP_LOGW(TAG, "Button event not sent, broker offline");
    }
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
        .active_level = CONFIG_GATEWAY_BUTTON_ACTIVE_LEVEL,
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

    led_init();
    button_init();
    ESP_ERROR_CHECK(gateway_uart_start());
    /* MQTT is started from the Wi-Fi event handler once an IP is assigned */
    ESP_ERROR_CHECK(gateway_wifi_start());
}
