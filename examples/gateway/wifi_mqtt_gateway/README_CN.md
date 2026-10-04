# Wi-Fi 转 MQTT 网关示例

[English](README.md)

基于 ESP-IDF 和 esp-iot-solution 组件的最小物联网网关。网关连接 Wi-Fi，保持与 MQTT 服务器的连接，并把挂在 UART 总线上的子设备桥接到云端。

```
 子设备 ──UART──▶ ESP32 网关 ──Wi-Fi──▶ MQTT 服务器 / 云
        ◀──────             ◀──────
```

## 功能

- Wi-Fi STA，断线后按指数退避自动重连
- MQTT 客户端，带遗嘱消息（`offline`）和保留的 `online` 状态
- UART 桥接：子设备发来的每一行（以 `\n` 结尾）上报到云端，云端下发的消息写入 UART
- 远程命令：`ping`、`info`、`reboot`
- 周期遥测：运行时间、剩余内存、RSSI、固件版本
- 使用 [`led_indicator`](../../../components/led/led_indicator) 组件做状态灯，[`button`](../../../components/button) 组件做按键

## 主题

所有主题以 `<prefix>/<gateway_id>` 为前缀。prefix 在 menuconfig 中设置（默认 `esp-gateway`），gateway_id 为 STA MAC 地址的小写十六进制。

| 主题 | 方向 | 说明 |
|------|------|------|
| `<base>/status` | 网关 → 云 | 保留消息 `online` / `offline`（遗嘱） |
| `<base>/telemetry` | 网关 → 云 | 每 `GATEWAY_TELEMETRY_INTERVAL_S` 秒上报一次 JSON 遥测 |
| `<base>/up` | 网关 → 云 | UART 收到的每一行发一条消息 |
| `<base>/down` | 云 → 网关 | 负载加 `\n` 后写入 UART |
| `<base>/cmd` | 云 → 网关 | `ping`、`info` 或 `reboot` |
| `<base>/resp` | 网关 → 云 | 命令回复 |
| `<base>/event` | 网关 → 云 | 本地事件，如 `{"event":"button_click"}` |

## 硬件

| 信号 | 默认 GPIO | Kconfig |
|------|-----------|---------|
| 发往子设备的 UART TX | 4 | `GATEWAY_UART_TX_GPIO` |
| 来自子设备的 UART RX | 5 | `GATEWAY_UART_RX_GPIO` |
| 状态灯 | 2 | `GATEWAY_LED_GPIO`（`-1` 关闭） |
| 按键 | 0 | `GATEWAY_BUTTON_GPIO`（`-1` 关闭） |

状态灯：连接 Wi-Fi 时快闪，连接 MQTT 服务器时慢闪，在线时常亮。单击按键上报按键事件，长按 5 秒重启网关。

## 编译和烧录

需要 ESP-IDF v5.3 或更高版本。

```bash
idf.py set-target esp32
idf.py menuconfig   # Gateway Configuration：Wi-Fi、服务器地址、GPIO
idf.py build flash monitor
```

## 测试

使用默认的公共服务器时，订阅网关发出的所有消息：

```bash
mosquitto_sub -h broker.emqx.io -t 'esp-gateway/+/#' -v
```

下发数据和命令（`<id>` 替换为日志里打印的网关 id）：

```bash
mosquitto_pub -h broker.emqx.io -t 'esp-gateway/<id>/down' -m '{"led":1}'
mosquitto_pub -h broker.emqx.io -t 'esp-gateway/<id>/cmd' -m 'info'
```

正式使用请换成带账号密码（以及 `mqtts://`）的私有服务器。
