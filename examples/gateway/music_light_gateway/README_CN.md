# 音乐识别氛围灯网关

[English](README.md)

ESP32-S3 通过数字麦克风听取外部音箱播放的音乐，在本地做节奏检测和音乐风格判断，用四路 PWM 驱动 RGBW 恒流灯，并通过 Wi-Fi/MQTT 上报和接受控制。

```
外部音箱（AirPlay 等）→ 数字 MEMS 麦克风 → I2S → 节奏/响度（每 16 ms）
                                                 → 风格判断（每 5 s）→ 灯效引擎 → 4 路 PWM → RGBW 恒流驱动
                                                                     ↘ MQTT 上报
```

## 工作原理

| 环节 | 实现 |
|------|------|
| 采集 | I2S 标准模式，16 kHz，24 位麦克风（INMP441、ICS-43434 等） |
| 节奏 | 512 点 FFT，步长 256（16 ms）；对数谱通量 + 自适应门限检测鼓点；自相关估算 BPM |
| 响度 | 每帧 RMS，带自动增益，音量大小不同都能用满亮度范围 |
| 风格 | 每 5.12 s 汇总特征：BPM、节拍规律性、打击感、低/中/高频占比、动态范围。判断为 安静 / 氛围 / 古典 / 流行 / 摇滚 / 电子 / 嘻哈 之一，连续两次一致或置信度很高时才切换 |
| 灯效 | 风格决定配色和切换节奏，鼓点触发亮度脉冲，响度控制整体亮度；安静时暖白呼吸 |
| 输出 | LEDC 12 位 19.5 kHz PWM（高于可听频率），伽马 2.2，可设最大占空比限制发热 |

**注意：** 当前风格判断是基于规则的基线版本，在合成音乐上测试通过，但还没有用真实房间录音校准。`genre_classify()` 的接口按“可替换为训练好的模型”设计，后续可换成 ESP-DL 模型（输入 log-mel 特征）。

## 引脚分配（ESP32-S3-WROOM-1 N16R8 建议）

| 功能 | GPIO | 说明 |
|------|------|------|
| 麦克风 BCLK / WS / DIN | 4 / 5 / 6 | Kconfig 可改 |
| 灯 R / G / B / W PWM | 38 / 39 / 40 / 41 | 接恒流驱动的 DIM 脚，Kconfig 可改 |
| 按键 | 0 | 单击切换 音乐/固定色/关，长按 5 s 重启 |
| 圆形屏 QSPI（预留） | CS 10, CLK 12, D0 11, D1 13, D2 14, D3 9, RST 8, 背光 7 | 本版固件未驱动 |
| 触摸 I2C（预留） | SDA 15, SCL 16, INT 17, RST 18 | 本版固件未驱动 |
| 蓝牙模组 UART（预留） | TX 42, RX 21 | 本版固件未驱动 |
| USB 调试 / 下载 | 19 / 20 | 内置 USB-Serial-JTAG |
| 不要使用 | 3, 45, 46（启动配置脚）；35, 36, 37（八线 PSRAM 占用） | |

38–41 同时是外部 JTAG 引脚；调试走内置 USB-JTAG 就不冲突。

## MQTT 主题

前缀 `<base>` = `<prefix>/<gateway_id>`，默认 prefix 为 `music-light`。

| 主题 | 方向 | 内容 |
|------|------|------|
| `<base>/status` | 上报 | 保留消息 `online` / `offline`（遗嘱） |
| `<base>/music` | 上报 | 每 5 s：风格、置信度、BPM、规律性、响度、频段占比 |
| `<base>/light/state` | 上报 | 保留消息：模式、亮度、固定色、当前风格 |
| `<base>/telemetry` | 上报 | 运行时间、内存、RSSI |
| `<base>/cmd` | 下发 | 见下表 |
| `<base>/resp` | 上报 | 命令回复 |

| 命令 | 作用 |
|------|------|
| `mode music` / `mode static` / `mode off` | 切换模式 |
| `color 255 120 0 50` | 固定色 R G B W（0–255），自动切到固定色模式 |
| `brightness 60` | 亮度 0–100 |
| `ping` / `info` / `reboot` | 诊断 |

## 编译

需要 ESP-IDF v5.3 及以上。

```bash
idf.py set-target esp32s3
idf.py menuconfig   # Music Light Gateway：Wi-Fi、服务器、麦克风、灯的引脚和功率上限
idf.py build flash monitor
```

日志每 5 s 打印一行分析结果，可以直接用来对照现场效果调参。

## 在电脑上调风格规则

分析算法不依赖 ESP-IDF，可以在电脑上跑：

```bash
cd host_test
make test                       # 合成音乐自测：BPM 和风格
ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav
make analyze_wav && ./analyze_wav song.wav   # 输出与设备上相同的逐窗分析
```

建议用麦克风在真实房间里录几段不同风格的音乐来校准，而不是只用原始音频文件。

## 后续

- 圆形触摸屏界面（LVGL）
- 用真实录音训练 ESP-DL 风格分类模型，替换规则版
- 实测 CPU 占用和功耗，确认 8 W 灯光下的散热
