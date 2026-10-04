/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "gateway.h"

#define UART_PORT       CONFIG_GATEWAY_UART_PORT
#define UART_RX_BUF     2048
#define UART_READ_CHUNK 128
#define UART_LINE_MAX   CONFIG_GATEWAY_UART_LINE_MAX

static const char *TAG = "gw_uart";

static void flush_line(char *line, size_t *len)
{
    /* Strip a trailing '\r' from CRLF-terminated frames */
    if (*len > 0 && line[*len - 1] == '\r') {
        (*len)--;
    }
    if (*len > 0) {
        if (gateway_mqtt_publish_uplink(line, *len) != ESP_OK) {
            ESP_LOGW(TAG, "Uplink dropped (%u bytes), broker offline", (unsigned)*len);
        }
    }
    *len = 0;
}

static void uart_rx_task(void *arg)
{
    static char line[UART_LINE_MAX];
    uint8_t chunk[UART_READ_CHUNK];
    size_t line_len = 0;

    while (1) {
        int n = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                flush_line(line, &line_len);
                continue;
            }
            line[line_len++] = (char)chunk[i];
            if (line_len == UART_LINE_MAX) {
                flush_line(line, &line_len);
            }
        }
    }
}

esp_err_t gateway_uart_send(const char *data, size_t len)
{
    if (uart_write_bytes(UART_PORT, data, len) < 0 || uart_write_bytes(UART_PORT, "\n", 1) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t gateway_uart_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_GATEWAY_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, CONFIG_GATEWAY_UART_TX_GPIO, CONFIG_GATEWAY_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "Sub-device UART%d ready (TX %d, RX %d, %d baud)", UART_PORT,
             CONFIG_GATEWAY_UART_TX_GPIO, CONFIG_GATEWAY_UART_RX_GPIO, CONFIG_GATEWAY_UART_BAUD_RATE);

    if (xTaskCreate(uart_rx_task, "gw_uart_rx", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
