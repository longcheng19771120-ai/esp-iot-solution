# 音乐识别氛围灯网关

[English](README.md)

ESP32-S3 通过数字麦克风听取外部音箱播放的音乐，在本地做节奏检测，并估算音乐的情绪（愉悦度）和能量，用四路 PWM 驱动 RGBW 恒流灯，并通过 Wi-Fi/MQTT 上报和接受控制。

```
外部音箱（AirPlay 等）→ 数字 MEMS 麦克风 → I2S → 节奏/响度（每 16 ms）
                                                 → 情绪 + 能量（每 5 s）→ 灯效引擎 → 4 路 PWM → RGBW 恒流驱动
                                                                     ↘ MQTT 上报
```

## 工作原理

| 环节 | 实现 |
|------|------|
| 采集 | I2S 标准模式，16 kHz，24 位麦克风（INMP441、ICS-43434 等） |
| 节奏 | 512 点 FFT，步长 256（16 ms）；对数谱通量 + 自适应门限检测鼓点；自相关估算 BPM |
| 响度 | 每帧 RMS，带自动增益，音量大小不同都能用满亮度范围 |
| 调性 | 每 64 ms 做一次 2048 点 FFT，折算成 12 个半音的色度分布；每个窗口结束时与大调、小调模板（Krumhansl-Kessler）比对，得到调、偏大调还是偏小调，以及调性是否明确 |
| 情绪 | 每 5.12 s 汇总 11 个特征：速度、节拍规律性、打击感、鼓点密度、低/中/高频占比、动态范围、明亮度、大小调、调性强度。输出两个 0–1 的连续值：愉悦度（伤感到欢快）和能量（平静到激烈） |
| 灯效 | 按愉悦度和能量在四个角的配色之间混合（平静：暖白和琥珀，欢快：橙和粉，紧张：红和紫，伤感：蓝和青）。能量同时决定鼓点闪烁的力度、颜色渐变的快慢和点缀色切换的频率。情绪变化会在几秒内平滑过渡；安静时暖白呼吸 |
| 显示 | 圆形 360×360 触摸屏（LVGL）：显示情绪、BPM，中间的光盘跟随灯的颜色和亮度，圆点标出音乐在“愉悦度 × 能量”平面上的位置。拖动外圈调亮度，点按钮切换模式 |
| 输出 | LEDC 12 位 19.5 kHz PWM（高于可听频率），伽马 2.2，可设最大占空比限制发热 |

**注意：** 默认的情绪估计是基于规则的基线版本。大小调检测在真实录音上可用；但在 8 首真实曲目（模拟房间拾音）上，四象限（平静/欢快/紧张/伤感）与主观标注一致的 5 秒片段约占一半，每个维度平均误差约 0.2。典型的误判是没有鼓的激烈管弦乐（比如匈牙利舞曲），会被判成伤感而不是紧张。情绪本身很主观，建议用你自己房间的录音和你自己的标注做一次校准（见下文）。

## 引脚分配（ESP32-S3-WROOM-1 N16R8 建议）

| 功能 | GPIO | 说明 |
|------|------|------|
| 麦克风 BCLK / WS / DIN | 4 / 5 / 6 | Kconfig 可改 |
| 灯 R / G / B / W PWM | 38 / 39 / 40 / 41 | 接恒流驱动的 DIM 脚，Kconfig 可改 |
| 按键 | 0 | 单击切换 音乐/固定色/关，长按 5 s 重启 |
| 圆形屏 QSPI | CS 10, CLK 12, D0 11, D1 13, D2 14, D3 9, RST 8, 背光 7 | ST77916，Kconfig 可改 |
| 触摸 I2C | SDA 15, SCL 16, INT 17, RST 18 | CST816S，Kconfig 可改 |
| 蓝牙模组 UART（预留） | TX 42, RX 21 | 本版固件未驱动 |
| USB 调试 / 下载 | 19 / 20 | 内置 USB-Serial-JTAG |
| 不要使用 | 3, 45, 46（启动配置脚）；35, 36, 37（八线 PSRAM 占用） | |

38–41 同时是外部 JTAG 引脚；调试走内置 USB-JTAG 就不冲突。

## MQTT 主题

前缀 `<base>` = `<prefix>/<gateway_id>`，默认 prefix 为 `music-light`。

| 主题 | 方向 | 内容 |
|------|------|------|
| `<base>/status` | 上报 | 保留消息 `online` / `offline`（遗嘱） |
| `<base>/music` | 上报 | 每 5 s：情绪象限、愉悦度、能量、BPM、规律性、大小调、调性强度、响度、频段占比、特征向量 |
| `<base>/light/state` | 上报 | 保留消息：模式、亮度、固定色 |
| `<base>/telemetry` | 上报 | 运行时间、内存、RSSI |
| `<base>/cmd` | 下发 | 见下表 |
| `<base>/resp` | 上报 | 命令回复 |

| 命令 | 作用 |
|------|------|
| `mode music` / `mode static` / `mode off` | 切换模式 |
| `color 255 120 0 50` | 固定色 R G B W（0–255），自动切到固定色模式 |
| `brightness 60` | 亮度 0–100 |
| `label happy` / `label 0.3 0.9` / `label none` | 给接下来的 `/music` 消息打上情绪标签（`calm`、`happy`、`tense`、`sad`，或 0–1 的愉悦度和能量），用于校准 |
| `ping` / `info` / `reboot` | 诊断 |

## 编译

需要 ESP-IDF v5.4 及以上。

```bash
idf.py set-target esp32s3
idf.py menuconfig   # Music Light Gateway：Wi-Fi、服务器、麦克风、灯和屏幕的引脚、功率上限
idf.py build flash monitor
```

日志每 5 s 打印一行分析结果，可以直接用来对照现场效果调参。

## 圆形屏

默认屏幕是常见的 1.8 寸 360×360 圆形屏：ST77916 主控走 QSPI，触摸芯片 CST816S。屏幕刷新的优先级低于音频分析和灯光，画面再忙也不会耽误鼓点。触摸芯片没有响应时屏幕照常显示，只是不能触摸；屏幕本身起不来时，灯照常工作。不接屏幕时可以在 menuconfig 的 *Round touch display* 里关掉。

不同厂家的 ST77916 屏有时需要各自的初始化命令；如果屏幕黑屏或颜色不对，把厂家给的初始化序列填到 `display_ui.c` 里的 `st77916_vendor_config_t.init_cmds`。如果触摸位置左右或上下反了，在 menuconfig 里打开对应的镜像选项。

固件开启了八线 PSRAM（N16R8 模组自带），模组没有 PSRAM 时也能正常启动。应用分区为 3 MB，4 MB Flash 即可放下。

## 静音门限

常见 MEMS 麦克风灵敏度为 -26 dBFS（94 dB SPL），房间里 60–80 dB SPL 的音乐大约是 -60 到 -40 dBFS，安静房间在 -80 dBFS 以下，所以默认静音门限设为 -65 dBFS。装好后看 `/music` 消息里的 `level_db`：音乐播放时应明显高于门限，不放音乐时应低于门限，否则在 menuconfig 里调整 `AUDIO_SILENCE_DB`。

## 校准情绪判断

分析代码不依赖 ESP-IDF，可以在电脑上编译运行。校准步骤：

1. **采集。** 用网关自己的麦克风在实际房间里采集最准确。每个情绪象限至少放 3 首不同的歌，每首放 1 分钟以上，按你自己的感受打标签：

   ```bash
   cd host_test
   pip install paho-mqtt
   python3 collect_mqtt.py --broker <服务器> --id <网关id> -o room.csv
   # 每换一首歌发一次（标签相同也要重发，用来区分不同的歌）：
   mosquitto_pub -h <服务器> -t 'music-light/<网关id>/cmd' -m 'label happy'
   # 也可以直接给数值，先愉悦度后能量：
   mosquitto_pub -h <服务器> -t 'music-light/<网关id>/cmd' -m 'label 0.3 0.9'
   # 结束后：
   mosquitto_pub -h <服务器> -t 'music-light/<网关id>/cmd' -m 'label none'
   ```

   也可以用录音文件：`./analyze_wav --csv happy song.wav >> room.csv` 或 `--csv 0.3,0.9`（先用 `ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav` 转格式）。

2. **训练和评估。** 脚本按“每次留出一首歌”评估误差，没训练过的歌也要判对才算数，并在同一批数据上列出内置估计的误差作对比：

   ```bash
   python3 fit_mood_model.py room.csv
   ```

3. **写入固件。** 只有训练出的模型比内置估计更好时，才生成模型头文件，重新编译烧录，设备会自动改用训练好的模型：

   ```bash
   python3 fit_mood_model.py room.csv -o ../main/mood_model.h
   ```

歌太少时训练出的模型通常不如内置规则，某个象限少于 3 首歌时脚本会提示。其他工具：`make test` 跑合成音乐自测，`./analyze_wav song.wav` 打印与设备相同的逐段分析。

## 后续

- 数据足够多以后，可换成 ESP-DL 卷积模型（输入 log-mel 特征）
- 实测 CPU 占用和功耗，确认 8 W 灯光下的散热
