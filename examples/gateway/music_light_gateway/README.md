# Music-Reactive Light Gateway

[中文版](README_CN.md)

An ESP32-S3 listens through a digital microphone to music played by an external speaker, or takes the music directly from an optional Bluetooth receiver board, detects beats, the genre and the mood (valence and energy) of the music locally, drives an RGBW constant-current light with four PWM channels, and reports and takes commands over Wi-Fi/MQTT.

```
external speaker (AirPlay etc.) -> MEMS mic -> I2S -> beat / loudness (every 16 ms)
phone -> Bluetooth board -> I2S ---------------/       -> genre + mood (every 5 s) -> light effects -> 4x PWM -> RGBW driver
                         \-> amplifier -> speaker                              \-> MQTT
```

## How It Works

| Stage | Implementation |
|-------|----------------|
| Capture | Standard I2S, 16 kHz, 24-bit MEMS mic (INMP441, ICS-43434, ...). While the Bluetooth receiver board plays, its digital stream is used instead, resampled from 44.1 or 48 kHz |
| Beats | 512-point FFT with a 256-sample hop (16 ms); log spectral flux with an adaptive threshold for onsets; autocorrelation for BPM |
| Loudness | Per-hop RMS with auto gain, so the full brightness range is used at any volume |
| Key | Every 64 ms a 2048-point FFT is folded into a 12-tone chroma profile; at the end of each window it is matched against major and minor key profiles (Krumhansl-Kessler), giving the key, how major or minor it sounds, and how tonal the music is |
| Genre | Every 5.12 s the window is classified as silence / ambient / classical / pop / rock / electronic / hip-hop from tempo, beat regularity, percussiveness, onset rate, band shares, dynamic range and brightness. A new genre takes over after two agreeing windows or one confident one |
| Mood | Every 5.12 s the window is summarised into 11 features: tempo, beat regularity, percussiveness, onset rate, low/mid/high energy share, dynamic range, brightness, major/minor and key strength. These map to two continuous values from 0 to 1: valence (sad to happy) and energy (calm to intense) |
| Effects | The genre picks the palette and its rhythm: blues and violet drifting slowly for ambient, warm white and amber for classical, bright multi-color on every other beat for pop, red and orange for rock, neon colors on every beat for electronic, violet, gold and red for hip-hop. Mood adjusts it: happy music turns warm hues toward gold and adds warm white, sad music turns cool hues toward blue, mutes warm ones and removes white, with the shift capped so each genre keeps its colors. Energy speeds up or slows down the genre's color changes and fades and scales its beat flashes. Warm-white breathing when quiet |
| Themes | `vivid` uses the saturated palettes above. `song` uses muted Chinese traditional colors in the Song dynasty style, with slower fades and softer beat flashes: celadon 天青, moon white 月白 and ink blue 黛蓝 for ambient; old-silk yellow 缃色, ivory 牙色 and sandalwood 檀色 for classical; carmine 胭脂, lotus mauve 藕荷 and gosling yellow 鹅黄 for pop; cinnabar 丹砂, ochre 赭石 and amber 黄栌 for rock; the azurite, malachite and gold of *A Thousand Li of Rivers and Mountains* for electronic; ink violet 黛紫, gold 赤金 and vermilion 朱红 for hip-hop. Switch with the `theme` command or by tapping the corona on the screen; the choice is kept across reboots |
| Display | Round 360×360 touch screen (LVGL) on black: a solar corona in the light's color, whose rays stretch with loudness, ripple with the bass and treble and flare on each beat, around a dark moon showing the genre, mood and BPM. Drag the ring to set brightness, tap the corona to switch the color theme, tap the button to cycle the mode. A Bluetooth icon appears while a phone is connected, blue while the music comes from it |
| Output | LEDC 12-bit PWM at 19.5 kHz (above audible), gamma 2.2, configurable duty cap for the thermal budget |

**Note:** both estimates are rule-based baselines. The genre rules get 89 of 200 five-second windows right on 8 real tracks, clean and with simulated room pickup; most confusions are between genres with similar lighting, such as classical and ambient, or hip-hop and electronic. Major/minor detection works on real recordings, but on 8 real tracks with simulated room pickup the quadrant (calm / happy / tense / sad) matched a subjective label in about half of the 5 s windows, with an average error of about 0.2 on each axis. A typical miss is energetic orchestral music without drums, such as a Hungarian Dance, which reads as sad rather than tense. Calibrate both with recordings from your own room and your own labels (see below).

## Suggested Pins (ESP32-S3-WROOM-1 N16R8)

| Function | GPIO | Notes |
|----------|------|-------|
| Mic BCLK / WS / DIN | 4 / 5 / 6 | Kconfig |
| Light R / G / B / W PWM | 38 / 39 / 40 / 41 | To the DIM inputs of the constant-current drivers, Kconfig |
| Button | 0 | Click cycles music / static / off, 5 s long press reboots |
| Round display QSPI | CS 10, CLK 12, D0 11, D1 13, D2 14, D3 9, RST 8, backlight 7 | ST77916, Kconfig |
| Touch I2C | SDA 15, SCL 16, INT 17, RST 18 | CST816S, Kconfig |
| Bluetooth receiver board I2S in | BCLK 1, WS 2, DATA 47 | From the board's I2S output, Kconfig |
| Bluetooth receiver board UART | TX 42, RX 21 | Connection and track, Kconfig |
| USB debug / flashing | 19 / 20 | Built-in USB-Serial-JTAG |
| Avoid | 3, 45, 46 (strapping); 35, 36, 37 (octal PSRAM) | |

GPIO 38–41 double as external JTAG pins; debugging over the built-in USB-JTAG avoids the conflict.

## MQTT Topics

`<base>` is `<prefix>/<gateway_id>`, the default prefix is `music-light`.

| Topic | Direction | Content |
|-------|-----------|---------|
| `<base>/status` | up | Retained `online` / `offline` (LWT) |
| `<base>/music` | up | Every 5 s: genre and confidence, mood quadrant, valence, energy, BPM, regularity, mode, key strength, level, band shares, feature vectors, and the source (`mic` or `bt`) |
| `<base>/light/state` | up | Retained: mode, color theme, current genre, brightness, static color |
| `<base>/bt` | up | Retained: Bluetooth receiver connected, playing, sample rate, title, artist |
| `<base>/telemetry` | up | Uptime, heap, RSSI |
| `<base>/cmd` | down | See below |
| `<base>/resp` | up | Command replies |

| Command | Effect |
|---------|--------|
| `mode music` / `mode static` / `mode off` | Switch mode |
| `color 255 120 0 50` | Static R G B W (0–255), switches to static mode |
| `brightness 60` | Brightness 0–100 |
| `theme song` / `theme vivid` | Color theme, kept across reboots |
| `label rock happy` / `label pop 0.3 0.9` / `label none` | Tag the following `/music` messages for calibration with a genre (`ambient`, `classical`, `pop`, `rock`, `electronic`, `hiphop`), a mood (`calm`, `happy`, `tense`, `sad`, or valence and energy from 0 to 1), or both |
| `ping` / `info` / `reboot` | Diagnostics |

## Build

Requires ESP-IDF v5.4 or later.

```bash
idf.py set-target esp32s3
idf.py menuconfig   # Music Light Gateway: Wi-Fi, broker, mic, light and display pins, power cap
idf.py build flash monitor
```

The log prints one analysis line every 5 s for tuning against what you hear.

## Round Display

The default panel is a 1.8" 360×360 ST77916 LCD on QSPI with a CST816S touch controller, a common round module. The display runs at a lower priority than audio analysis and the light, so drawing never delays a beat. If the touch controller does not answer, the screen still works without touch; if the screen itself fails to start, the light keeps running. Turn the display off under *Round touch display* in menuconfig to build without it.

ST77916 panels from different vendors sometimes need their own initialization commands; if the screen stays blank or shows wrong colors, pass the vendor's sequence through `st77916_vendor_config_t.init_cmds` in `display_ui.c`. If touches land mirrored, toggle the mirror options in menuconfig.

The firmware enables octal PSRAM (as on the N16R8 module) and still boots if none is fitted. The app partition is 3 MB, which fits a 4 MB flash.

## Bluetooth Receiver Board

The ESP32-S3 has no Classic Bluetooth, so phones cannot stream music to it directly. An ESP32 board (ESP32-WROOM-32 or similar) running the firmware in [bt_receiver](bt_receiver) does that part: phones pair with it as "Music Light" and play through an I2S amplifier such as the MAX98357A. The gateway listens on the same three I2S wires and gets the music as the phone sent it, without room echo, and the board reports the connection and the track over UART.

| ESP32 board | Amplifier | ESP32-S3 gateway |
|-------------|-----------|------------------|
| GPIO 26 BCLK | BCLK | GPIO 1 |
| GPIO 25 WS | LRC | GPIO 2 |
| GPIO 22 DOUT | DIN | GPIO 47 |
| GPIO 17 TX | | GPIO 21 (RX) |
| GPIO 16 RX | | GPIO 42 (TX) |
| GND | GND | GND |

```bash
cd bt_receiver
idf.py set-target esp32
idf.py build flash monitor
```

While the board delivers sound, or reports that it is playing, the analysis uses it instead of the microphone, and returns to the microphone 3 s after the music stops. Phones send 44.1 or 48 kHz; the gateway measures the rate from the bit clock and resamples to 16 kHz. Without the board the gateway works as before; turn the link off under *Bluetooth receiver board* in menuconfig to free the pins.

## Silence Threshold

A typical MEMS microphone reads -26 dBFS at 94 dB SPL, so music at 60–80 dB SPL arrives at about -60 to -40 dBFS and a quiet room is below -80 dBFS. The default threshold is -65 dBFS. Check `level_db` in the `/music` messages: it should sit clearly above the threshold while music plays and below it otherwise; adjust `AUDIO_SILENCE_DB` in menuconfig if not.

## Calibrating Genre and Mood Detection

The analysis code has no ESP-IDF dependencies and builds on a PC.

1. **Collect.** Recording through the gateway's own microphone in the real room works best. Play at least 3 different songs per genre and per mood quadrant you care about, at least a minute each, and label each song with its genre and how it feels to you:

   ```bash
   cd host_test
   pip install paho-mqtt
   python3 collect_mqtt.py --broker <broker> --id <gateway_id> -g genre.csv -m mood.csv
   # send for every new song, even with the same label, so songs can be told apart:
   mosquitto_pub -h <broker> -t 'music-light/<gateway_id>/cmd' -m 'label rock happy'
   # genre only, mood only, or exact mood values (valence then energy) work too:
   mosquitto_pub -h <broker> -t 'music-light/<gateway_id>/cmd' -m 'label pop 0.3 0.9'
   # when done:
   mosquitto_pub -h <broker> -t 'music-light/<gateway_id>/cmd' -m 'label none'
   ```

   WAV files work too: `./analyze_wav --csv rock song.wav >> genre.csv`, `./analyze_wav --csv happy song.wav >> mood.csv` or `--csv 0.3,0.9` (convert first with `ffmpeg -i song.mp3 -ac 1 -ar 16000 -sample_fmt s16 song.wav`).

2. **Fit and evaluate.** Both scripts score leave-one-song-out, so every song is judged by a model that never heard it; the mood script also shows the error of the built-in estimate on the same data:

   ```bash
   python3 fit_genre_model.py genre.csv
   python3 fit_mood_model.py mood.csv
   ```

3. **Build it in.** Only when a fitted model beats the built-in rules, generate its header and rebuild; the firmware switches to the trained model automatically:

   ```bash
   python3 fit_genre_model.py genre.csv -o ../main/genre_model.h
   python3 fit_mood_model.py mood.csv -o ../main/mood_model.h
   ```

If only some genres were collected, the genre model only chooses between those. With only a handful of songs a fitted mood model usually does worse than the built-in rules, and the script warns when a quadrant has fewer than 3 songs. Also available: `make test` runs the synthetic self test, and `./analyze_wav song.wav` prints the same per-window analysis as the device.

## Next Steps

- With enough data, move to an ESP-DL convolutional model on log-mel features
- Bluetooth volume control (AVRCP absolute volume) on the receiver board; for now the phone scales the audio itself
- Measure CPU load and power, and verify thermals with the 8 W light
