# Wi-Fi to MQTT Gateway Example

[中文版](README_CN.md)

A minimal IoT gateway built on ESP-IDF and esp-iot-solution components. The gateway joins a Wi-Fi network, keeps a connection to an MQTT broker, and bridges sub-devices attached to a UART bus to the cloud.

```
 sub-devices ──UART──▶ ESP32 gateway ──Wi-Fi──▶ MQTT broker / cloud
             ◀──────               ◀──────
```

## Features

- Wi-Fi station with exponential reconnect back-off
- MQTT client with Last Will (`offline`) and retained `online` status
- UART bridge: newline-terminated frames from sub-devices are published upstream, MQTT downlink messages are written to the UART
- Remote commands: `ping`, `info`, `reboot`
- Periodic telemetry: uptime, free heap, RSSI, firmware version
- Status LED via [`led_indicator`](../../../components/led/led_indicator) and a button via [`button`](../../../components/button)

## Topics

All topics use the base `<prefix>/<gateway_id>`, where the prefix is set in menuconfig (default `esp-gateway`) and the gateway id is the station MAC address in lowercase hex.

| Topic | Direction | Description |
|-------|-----------|-------------|
| `<base>/status` | gateway → cloud | Retained `online` / `offline` (LWT) |
| `<base>/telemetry` | gateway → cloud | JSON telemetry every `GATEWAY_TELEMETRY_INTERVAL_S` seconds |
| `<base>/up` | gateway → cloud | One message per line received on the UART |
| `<base>/down` | cloud → gateway | Payload is written to the UART followed by `\n` |
| `<base>/cmd` | cloud → gateway | `ping`, `info` or `reboot` |
| `<base>/resp` | gateway → cloud | Replies to commands |
| `<base>/event` | gateway → cloud | Local events, e.g. `{"event":"button_click"}` |

## Hardware

| Signal | Default GPIO | Kconfig |
|--------|--------------|---------|
| UART TX to sub-devices | 4 | `GATEWAY_UART_TX_GPIO` |
| UART RX from sub-devices | 5 | `GATEWAY_UART_RX_GPIO` |
| Status LED | 2 | `GATEWAY_LED_GPIO` (`-1` disables) |
| Button | 0 | `GATEWAY_BUTTON_GPIO` (`-1` disables) |

LED patterns: fast blink while connecting to Wi-Fi, slow blink while connecting to the broker, solid on when online. A single click publishes a button event, a 5 s long press reboots the gateway.

## Build and Flash

Requires ESP-IDF v5.3 or later.

```bash
idf.py set-target esp32
idf.py menuconfig   # Gateway Configuration: Wi-Fi, broker URI, GPIOs
idf.py build flash monitor
```

## Try It

With the default public broker, subscribe to everything the gateway sends:

```bash
mosquitto_sub -h broker.emqx.io -t 'esp-gateway/+/#' -v
```

Send a downlink frame and a command (replace `<id>` with the id printed in the log):

```bash
mosquitto_pub -h broker.emqx.io -t 'esp-gateway/<id>/down' -m '{"led":1}'
mosquitto_pub -h broker.emqx.io -t 'esp-gateway/<id>/cmd' -m 'info'
```

Use a private broker with credentials (and `mqtts://`) for anything beyond testing.
