# XIAO ESP32S3 Sense — Memory Recorder (Event-Driven)

<img width="1536" height="1024" alt="Memory Pin" src="https://github.com/user-attachments/assets/2ec47771-8417-43eb-a155-f868f5165a0a" />


A low-power Arduino firmware for the **Seeed Studio XIAO ESP32S3 Sense** that records "Memories" — a **photo + audio recording** — with a single touch, and lets you **browse them from your phone** via WiFi.

Designed for **battery-powered wearable** use: the device stays in deep sleep (~10 µA) and wakes only when touched.

---

## Two Operating Modes

The device distinguishes between **short touch** and **long touch** after waking from deep sleep:

### Mode 1: Capture Memory (Short Touch — tap and release)

```
Touch → Wake → Photo → Record Audio → Touch to Stop → Save → Sleep
```

| Step | What happens |
|---|---|
| 1 | Touch the sensor (quick tap) |
| 2 | Device wakes, vibrates once (confirmation) |
| 3 | Captures a photo, saves to SD |
| 4 | Starts recording audio (vibrates buzz-buzz) |
| 5 | Touch again to stop recording (or auto-stops at 60s) |
| 6 | Saves WAV file, vibrates (buuuzz), returns to deep sleep |

### Mode 2: Web Server (Long Touch — hold for 1+ second)

```
Hold Touch → Wake → WiFi AP + Web UI → Browse Files → Idle → Sleep
```

| Step | What happens |
|---|---|
| 1 | Touch and hold the sensor for 1+ second |
| 2 | Device wakes, vibrates three times (web mode) |
| 3 | Starts WiFi Access Point |
| 4 | Connect from phone to browse memories |
| 5 | After 2 min of inactivity (no clients), sleeps |

---

## WiFi File Browser 📱

| Setting | Value |
|---|---|
| SSID | `XIAO_Memory` |
| Password | `12345678` |
| URL | `http://192.168.4.1` |

### Web UI Features
- 🌙 Dark theme, mobile-friendly design
- 🖼️ Photo preview with full-screen lightbox
- 🔊 Built-in audio player
- ⬇️ Download buttons for photo and audio
- 🗑️ Delete memories from the browser
- 📊 Memory count display

---

## Memory Storage

Each memory is saved in its own folder on the SD card:
```
📁 memory_001/
   ├── photo.jpg   ← captured at the moment of touch
   └── audio.wav   ← recorded until you touch again

📁 memory_002/
   ├── photo.jpg
   └── audio.wav
```

---

## Power Architecture

```
           ┌──────────────────────────────────┐
           │         DEEP SLEEP (~10 µA)       │
           │     Device is OFF, waiting...     │
           └──────────────┬───────────────────┘
                          │ Touch wakeup
                          ▼
              ┌───────────────────────┐
              │  Detect touch type    │
              └───┬───────────────┬───┘
         Short    │               │    Long
                  ▼               ▼
        ┌─────────────┐  ┌──────────────────┐
        │ Mode 1      │  │ Mode 2           │
        │ Capture     │  │ WiFi + Web UI    │
        │ Photo+Audio │  │ Browse files     │
        └──────┬──────┘  └────────┬─────────┘
               │                  │ Idle timeout
               ▼                  ▼
           ┌──────────────────────────────────┐
           │         DEEP SLEEP (~10 µA)       │
           └──────────────────────────────────┘
```

### Power Consumption

| State | Current Draw | When |
|---|---|---|
| Deep Sleep | ~10-20 µA | 99%+ of the time |
| Mode 1 (Capture) | ~150-200 mA | Only during photo+audio |
| Mode 2 (WiFi) | ~200-300 mA | Only when browsing files |

> With a 100 mAh LiPo, the device can last **weeks to months** in standby.
> Battery charging is fast because the MCU is asleep (~10 µA) during charging.

---

<img width="2048" height="2048" alt="Product Design" src="https://github.com/user-attachments/assets/e798ee77-bc3e-4b1c-8dc6-34dca91d47af" />


## Features

- **Event-driven deep sleep** — wakes only when touched
- **Two modes via touch duration** — short=capture, long=browse
- **WiFi Access Point** — browse files from any device
- Sequential filenames — never overwrites existing files
- Proper WAV header for playback compatibility
- Haptic feedback with distinct vibration patterns
- Configurable vibration intensity
- Auto-stop recording after 60 seconds
- Camera deinitialized before recording (frees ~400KB PSRAM)
- Memory index persists across sleep cycles (RTC memory)
- Clean peripheral shutdown before every sleep

---

## Required Libraries

All libraries are **built-in** with the ESP32-S3 Arduino board package:

| Library | Purpose |
|---|---|
| `esp_camera.h` | OV2640 camera driver |
| `driver/i2s.h` | I2S/PDM microphone driver |
| `FS.h` | Filesystem abstraction |
| `SD.h` | SD card access (SPI mode) |
| `SPI.h` | SPI bus |
| `WiFi.h` | WiFi Access Point |
| `WebServer.h` | HTTP web server |
| `esp_sleep.h` | Deep sleep API |

---

## Board Configuration (Arduino IDE)

| Setting | Value | Notes |
|---|---|---|
| Board | `XIAO_ESP32S3` | |
| PSRAM | **OPI PSRAM** | Required for camera |
| USB CDC On Boot | **Enabled** | Required for Serial |
| Flash Mode | QIO 80 MHz | |
| Upload Speed | 921600 | |
| Partition Scheme | Default 4MB with spiffs | |

---

## Configurable Parameters

All parameters are defined at the top of the sketch:

| Parameter | Default | Description |
|---|---|---|
| `THRESHOLD_MULTIPLIER` | `1.8` | Touch threshold = Baseline * multiplier |
| `NOISE_MARGIN_RATIO` | `1.3` | Settle buffer margin above baseline for idle tracking |
| `EMA_ALPHA` | `0.15` | Adaptation rate of the baseline moving average |
| `LONG_TOUCH_MS` | `2000` | Hold duration for Mode 2 (ms) |
| `WEB_TIMEOUT_SEC` | `120` | Web server inactivity timeout (sec) |
| `MAX_RECORD_SEC` | `60` | Max audio recording duration (sec) |
| `vibrationIntensity` | `200` | Motor strength (0-255) |
| `AP_SSID` | `XIAO_Memory` | WiFi network name |
| `AP_PASSWORD` | `12345678` | WiFi password |

---

## Haptic Feedback Patterns

| Event | Pattern | Meaning |
|---|---|---|
| Wakeup (Mode 1) | `buzz` (1 short) | Memory mode confirmed |
| Recording started | `buzz-buzz` (2 short) | Now recording audio |
| Recording saved | `buuuzz` (1 long) | Memory saved |
| Web server started | `buzz-buzz-buzz` (3 short) | WiFi mode active |
| Going to sleep | `bz-bz-bz` (3 quick) | Shutting down |

---
<img width="1376" height="768" alt="Premium Hardware Showcase" src="https://github.com/user-attachments/assets/d1321489-425a-4e42-8a84-bfdf620c474f" />


## Pin Definitions

### Touch Button
| Function | GPIO |
|---|---|
| Start/Stop Memory, Mode Select | GPIO 1 (D0) |

### Vibration Motor
| Function | GPIO |
|---|---|
| Motor PWM output | GPIO 3 (D2) |

### PDM Microphone
| Function | GPIO |
|---|---|
| PDM Clock | GPIO 42 |
| PDM Data | GPIO 41 |

### SD Card (SPI mode)
| Function | GPIO |
|---|---|
| CS (Chip Select) | GPIO 21 |

### OV2640 Camera
| Function | GPIO |
|---|---|
| XCLK | 10 |
| PCLK | 13 |
| VSYNC | 38 |
| HREF | 47 |
| SIOD (SDA) | 40 |
| SIOC (SCL) | 39 |
| D0–D7 | 15, 17, 18, 16, 14, 12, 11, 48 |

---

## Hardware Setup

1. Attach the **Sense expansion board** via the B2B connector
2. Insert a **FAT32-formatted** microSD card (≤ 32 GB)
3. Connect vibration motor to **GPIO 3 (D2)** via transistor/MOSFET
4. Connect via USB-C
5. If the board doesn't appear, hold **BOOT** while plugging in

---

## Touch Threshold & Calibration

Instead of using a hardcoded threshold, the firmware calibrates itself dynamically:
1. **Initial Calibration:** At first boot, the sketch takes 50 samples to calculate the untouched baseline and stores it in RTC memory.
2. **Dynamic Baseline Drift Tracking:** Every time the device is about to enter sleep (and after the finger is released), it reads 10 idle samples. It slowly corrects the baseline via Exponential Moving Average (EMA) to adjust for temperature, humidity, or wire positioning.
3. **Threshold calculation:** The wakeup threshold is dynamically adjusted as `Baseline * THRESHOLD_MULTIPLIER`.

> **Plotter Check:** Use the `Touch_Test` sketch (included in this repo) and open the **Serial Plotter** to see how baseline tracking behaves dynamically in real-time.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| "SD card mount failed" | Check card is FAT32, firmly seated, ≤ 32 GB |
| "Camera init failed" | Ensure PSRAM is set to **OPI PSRAM** |
| Device won't wake from sleep | Decrease `THRESHOLD_MULTIPLIER` (e.g., `1.5`) |
| Device wakes up by itself | Increase `THRESHOLD_MULTIPLIER` (e.g., `2.0` or `2.2`) |
| Mode 2 triggers instead of Mode 1 | Release the touch faster (< 2 sec) |
| Mode 1 triggers instead of Mode 2 | Hold the touch longer (> 2 sec) |
| No Serial output after wake | Enable **USB CDC On Boot** in IDE settings |
| Serial monitor disconnects | Normal — USB is lost during deep sleep |
| Board not detected | Hold BOOT button while connecting USB |

---

## Project Structure

```
XIAO/
├── XIAO_Sense_Touch/
│   └── XIAO_Sense_Touch.ino      ← Main project (event-driven)
├── Touch_Test/
│   └── Touch_Test.ino             ← Touch calibration utility
├── DeepSleep_Touch_Test/
│   └── DeepSleep_Touch_Test.ino   ← Deep sleep validation test
├── README.md
└── .gitignore
```

---

## License

MIT — use freely in your projects.
