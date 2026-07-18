# XIAO ESP32S3 Sense — Memory Recorder + WiFi Browser

A simple Arduino project for the **Seeed Studio XIAO ESP32S3 Sense** that records "Memories" — a **photo + audio recording** saved together with a single touch — and lets you **browse them from your phone** via WiFi.

---

## How It Works

| Touch | Action |
|---|---|
| **First touch** (GPIO 1) | 📷 Capture photo + 🎙️ Start audio recording |
| **Second touch** (GPIO 1) | ⏹️ Stop audio recording & save Memory |

Each Memory is saved in its own folder:
```
📁 memory_001/
   ├── photo.jpg   ← photo captured at the moment of touch
   └── audio.wav   ← audio recorded until you touch again

📁 memory_002/
   ├── photo.jpg
   └── audio.wav
```

---

## WiFi File Browser 📱

The device creates a **WiFi hotspot**. Connect from your phone or laptop to browse, view, and download all saved memories.

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

## Features

- **One-button operation** — single touch pin does everything
- **WiFi Access Point** — browse files from any device
- Sequential filenames — never overwrites existing files
- Proper WAV header for playback compatibility
- Haptic feedback with distinct vibration patterns
- Configurable vibration intensity via a single variable
- Auto-stop recording after 60 seconds
- Graceful error handling if any peripheral fails

---

## Required Libraries

All libraries are **built-in** with the ESP32-S3 Arduino board package. No extra installs needed.

| Library | Purpose |
|---|---|
| `esp_camera.h` | OV2640 camera driver |
| `driver/i2s.h` | I2S/PDM microphone driver |
| `FS.h` | Filesystem abstraction |
| `SD.h` | SD card access (SPI mode) |
| `SPI.h` | SPI bus |
| `WiFi.h` | WiFi Access Point |
| `WebServer.h` | HTTP web server |

---

## Board Configuration (Arduino IDE)

1. **Install the board package**  
   `File → Preferences → Additional Board Manager URLs` → add:  
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   Then `Tools → Board → Board Manager` → search "esp32" → install.

2. **Select the board**  
   `Tools → Board → esp32 → XIAO_ESP32S3`

3. **Required settings**

| Setting | Value |
|---|---|
| Board | `XIAO_ESP32S3` |
| PSRAM | **OPI PSRAM** ← _required for camera_ |
| Flash Mode | QIO 80 MHz |
| Upload Speed | 921600 |
| USB CDC On Boot | Enabled |
| Partition Scheme | Default 4MB with spiffs |

---

## Pin Definitions

### Touch Button
| Function | GPIO |
|---|---|
| Start/Stop Memory | GPIO 1 (D0) |

### Vibration Motor
| Function | GPIO |
|---|---|
| Motor PWM output | GPIO 3 (D2) |

### PDM Microphone (Sense expansion board)
| Function | GPIO |
|---|---|
| PDM Clock | GPIO 42 |
| PDM Data | GPIO 41 |

### SD Card (SPI mode)
| Function | GPIO |
|---|---|
| CS (Chip Select) | GPIO 21 |
| SCK | GPIO 7 |
| MISO | GPIO 8 |
| MOSI | GPIO 9 |

### OV2640 Camera (Sense expansion board)
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

## Function Reference

| Function | Description |
|---|---|
| `setup()` | Initialises all peripherals, WiFi AP, and web server. |
| `loop()` | Handles web requests, streams audio while recording, polls touch. |
| `initSDCard()` | Mounts the SD card via SPI. |
| `initCamera()` | Configures and starts the OV2640 camera. |
| `initMicrophone()` | Installs the I2S driver in PDM-RX mode. |
| `initVibrationMotor()` | Configures GPIO 3 with LEDC PWM. |
| `initWiFiAP()` | Creates a WiFi Access Point hotspot. |
| `setupWebServer()` | Registers URL routes and starts the HTTP server. |
| `startMemory()` | Creates folder, captures photo, starts audio recording. |
| `stopMemory()` | Patches WAV header, closes file, vibrates. |
| `recordAudioChunk()` | Reads one I2S buffer and writes to WAV file. |
| `findNextMemoryIndex()` | Finds next unused folder index on SD. |
| `savePhoto(path)` | Captures JPEG and writes to SD. |
| `handleRoot()` | Serves the main HTML/CSS/JS web UI page. |
| `handleListMemories()` | Returns JSON list of all memory folders. |
| `handleFile()` | Streams a file from SD card to the browser. |
| `handleDelete()` | Deletes a memory folder and its contents. |

---

## Haptic Feedback Patterns

| Event | Pattern | Timing |
|---|---|---|
| Start Memory | `buzz-buzz` (two short pulses) | 80ms on, 80ms off, 80ms on |
| Stop Memory | `buuuzz` (one long pulse) | 300ms on |

### Vibration Intensity

Adjust `vibrationIntensity` at the top of the sketch:

| Value | Strength |
|---|---|
| `64` | Light |
| `128` | **Medium (default)** |
| `200` | Strong |
| `255` | Maximum |

---

## Audio Format

| Parameter | Value |
|---|---|
| Sample Rate | 16 000 Hz |
| Bit Depth | 16-bit |
| Channels | 1 (Mono) |
| Format | PCM (uncompressed) |
| Container | WAV (RIFF) |
| Duration | Variable (max 60 seconds) |
| File Size | ~32 KB per second of recording |

---

## Hardware Setup

1. Attach the **Sense expansion board** to the XIAO ESP32S3 via the B2B connector.
2. Insert a **FAT32-formatted** microSD card (≤ 32 GB recommended).
3. Connect vibration motor to **GPIO 3 (D2)** via a transistor/MOSFET.
4. Connect the board via USB-C.
5. If the board does not appear as a COM port, hold **BOOT** while plugging in.

---

## Touch Threshold Calibration

The default threshold is `40000`. To calibrate for your setup:

1. Upload the `Touch_Test` sketch first (included in this repo).
2. Open the Serial Monitor at **115200 baud**.
3. Note the **idle** value (not touching) and the **touched** value.
4. Set `TOUCH_THRESHOLD` to roughly halfway between the two.

> **Note:** On ESP32-S3, touch values **increase** when touched (opposite of original ESP32).

---

## Troubleshooting

| Problem | Solution |
|---|---|
| "SD card mount failed" | Check card is FAT32, firmly seated, ≤ 32 GB |
| "Camera init failed: 0x…" | Ensure PSRAM is set to **OPI PSRAM** in IDE |
| Touch triggers immediately | Calibrate `TOUCH_THRESHOLD` (see above) |
| Audio sounds like static | Verify PDM mode is enabled (`I2S_MODE_PDM`) |
| Board not detected | Hold BOOT button while connecting USB |
| Can't connect to WiFi | Look for `XIAO_Memory` in WiFi list, password: `12345678` |
| Web page not loading | Make sure you're connected to the XIAO WiFi, open `http://192.168.4.1` |

---

## License

MIT — use freely in your projects.
