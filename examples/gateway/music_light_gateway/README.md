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

**Note:** the default genre classifier is a rule-based baseline. On 8 real tracks (including simulated room pickup) it gets about half of the 5 s windows right; most confusions are between styles with similar lighting, such as classical and ambient, or hip-hop and electronic. For usable accuracy, calibrate it with recordings from your own room (see below), which switches it to a small model trained on that data.

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
| `label rock` / `label none` | Tag the following `/music` messages with a genre, for calibration |
| `ping` / `info` / `reboot` | Diagnostics |

## Build

Requires ESP-IDF v5.3 or later.

```bash
idf.py set-target esp32s3
idf.py menuconfig   # Music Light Gateway: Wi-Fi, broker, mic and light pins, power cap
idf.py build flash monitor
```

The log prints one analysis line every 5 s for tuning against what you hear.

## Silence Threshold

A typical MEMS microphone reads -26 dBFS at 94 dB SPL, so music at 60–80 dB SPL arrives at about -60 to -40 dBFS and a quiet room is below -80 dBFS. The default threshold is -65 dBFS. Check `level_db` in the `/music` messages: it should sit clearly above the threshold while music plays and below it otherwise; adjust `AUDIO_SILENCE_DB` in menuconfig if not.

## Calibrating Genre Detection

The analysis code has no ESP-IDF dependencies and builds on a PC.

1. **Collect.** Recording through the gateway's own microphone in the real room works best. Play 3–5 different songs per genre you want to detect, at least a minute each:

   ```bash
   cd host_test
   pip install paho-mqtt
   python3 collect_mqtt.py --broker <broker> --id <gateway_id> -o room.csv
   # send for every new song, even within the same genre, so songs can be told apart:
   mosquitto_pub -h <broker> -t 'music-light/<gateway_id>/cmd' -m 'label rock'
   # when done:
   mosquitto_pub -h <broker> -t 'music-light/<gateway_id>/cmd' -m 'label none'
   ```

   WAV files work too: `./analyze_wav --csv rock song.wav >> room.csv` (convert first with `ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav`).

2. **Fit and evaluate.** The script reports leave-one-song-out accuracy, so every song is judged by a model that never heard it:

   ```bash
   python3 fit_genre_model.py room.csv
   ```

3. **Build it in.** When the numbers look right, generate the model header and rebuild; the firmware switches to the trained model automatically:

   ```bash
   python3 fit_genre_model.py room.csv -o ../main/genre_model.h
   ```

If only some genres were collected, the model only chooses between those. Also available: `make test` runs the synthetic self test, and `./analyze_wav song.wav` prints the same per-window analysis as the device.

## Next Steps

- Round touch display UI (LVGL)
- With enough data, move to an ESP-DL convolutional model on log-mel features
- Measure CPU load and power, and verify thermals with the 8 W light
