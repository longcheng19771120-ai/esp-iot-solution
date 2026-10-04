# Music-Reactive Light Gateway

[中文版](README_CN.md)

An ESP32-S3 listens through a digital microphone to music played by an external speaker, detects beats and the music genre locally, drives an RGBW constant-current light with four PWM channels, and reports and takes commands over Wi-Fi/MQTT.

```
external speaker (AirPlay etc.) -> MEMS mic -> I2S -> beat / loudness (every 16 ms)
                                                   -> genre (every 5 s) -> light effects -> 4x PWM -> RGBW driver
                                                                       \-> MQTT
```

## How It Works

| Stage | Implementation |
|-------|----------------|
| Capture | Standard I2S, 16 kHz, 24-bit MEMS mic (INMP441, ICS-43434, ...) |
| Beats | 512-point FFT with a 256-sample hop (16 ms); log spectral flux with an adaptive threshold for onsets; autocorrelation for BPM |
| Loudness | Per-hop RMS with auto gain, so the full brightness range is used at any volume |
| Genre | Every 5.12 s the window is summarised: BPM, beat regularity, percussiveness, low/mid/high energy share and dynamic range. Classified as silence / ambient / classical / pop / rock / electronic / hip-hop; switches after two agreeing windows or one confident one |
| Effects | Genre picks the palette and how often it changes, beats flash the brightness, loudness sets the overall level; warm-white breathing when quiet |
| Output | LEDC 12-bit PWM at 19.5 kHz (above audible), gamma 2.2, configurable duty cap for the thermal budget |

**Note:** the genre classifier is a rule-based baseline. It passes on synthetic music but has not been calibrated on real room recordings yet. `genre_classify()` is designed to be swapped for a trained model, for example an ESP-DL network on log-mel features.

## Suggested Pins (ESP32-S3-WROOM-1 N16R8)

| Function | GPIO | Notes |
|----------|------|-------|
| Mic BCLK / WS / DIN | 4 / 5 / 6 | Kconfig |
| Light R / G / B / W PWM | 38 / 39 / 40 / 41 | To the DIM inputs of the constant-current drivers, Kconfig |
| Button | 0 | Click cycles music / static / off, 5 s long press reboots |
| Round display QSPI (reserved) | CS 10, CLK 12, D0 11, D1 13, D2 14, D3 9, RST 8, backlight 7 | Not driven by this firmware yet |
| Touch I2C (reserved) | SDA 15, SCL 16, INT 17, RST 18 | Not driven yet |
| Bluetooth module UART (reserved) | TX 42, RX 21 | Not driven yet |
| USB debug / flashing | 19 / 20 | Built-in USB-Serial-JTAG |
| Avoid | 3, 45, 46 (strapping); 35, 36, 37 (octal PSRAM) | |

GPIO 38–41 double as external JTAG pins; debugging over the built-in USB-JTAG avoids the conflict.

## MQTT Topics

`<base>` is `<prefix>/<gateway_id>`, the default prefix is `music-light`.

| Topic | Direction | Content |
|-------|-----------|---------|
| `<base>/status` | up | Retained `online` / `offline` (LWT) |
| `<base>/music` | up | Every 5 s: genre, confidence, BPM, regularity, level, band shares |
| `<base>/light/state` | up | Retained: mode, brightness, static color, current genre |
| `<base>/telemetry` | up | Uptime, heap, RSSI |
| `<base>/cmd` | down | See below |
| `<base>/resp` | up | Command replies |

| Command | Effect |
|---------|--------|
| `mode music` / `mode static` / `mode off` | Switch mode |
| `color 255 120 0 50` | Static R G B W (0–255), switches to static mode |
| `brightness 60` | Brightness 0–100 |
| `ping` / `info` / `reboot` | Diagnostics |

## Build

Requires ESP-IDF v5.3 or later.

```bash
idf.py set-target esp32s3
idf.py menuconfig   # Music Light Gateway: Wi-Fi, broker, mic and light pins, power cap
idf.py build flash monitor
```

The log prints one analysis line every 5 s for tuning against what you hear.

## Tuning on a PC

The analysis code has no ESP-IDF dependencies and builds on a PC:

```bash
cd host_test
make test                       # synthetic self test: tempo and genre
ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav
make analyze_wav && ./analyze_wav song.wav   # same per-window output as the device
```

Calibrate with music recorded through a microphone in a real room, not only with the original audio files.

## Next Steps

- Round touch display UI (LVGL)
- Train an ESP-DL genre model on real recordings to replace the rules
- Measure CPU load and power, and verify thermals with the 8 W light
