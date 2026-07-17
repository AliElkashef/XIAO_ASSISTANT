# XIAO ESP32S3 Sense — Touch-Triggered Audio & Camera

A simple Arduino project for the **Seeed Studio XIAO ESP32S3 Sense** that uses the built-in capacitive touch pins to record audio and capture photos, saving them to a microSD card.

---

## Features

| Touch Button | GPIO | Action |
|---|---|---|
| Button 1 | GPIO 1 (D0) | Start/Stop audio recording → `audio_NNN.wav` |
| Button 2 | GPIO 2 (D1) | Capture photo → `image_NNN.jpg` |
| Vibration Motor | GPIO 3 (D2) | Haptic feedback on every touch action |

- Sequential filenames — never overwrites existing files
- Proper WAV header for playback compatibility
- Software debounce — one touch = one action
- Haptic feedback with distinct vibration patterns per action
- Configurable vibration intensity via a single variable
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

### Touch Buttons
| Function | GPIO |
|---|---|
| Record Audio | GPIO 1 (D0) |
| Capture Photo | GPIO 2 (D1) |

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

### Vibration Motor
| Function | GPIO |
|---|---|
| Motor PWM output | GPIO 3 (D2) |

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
| `setup()` | Initialises Serial, SD card, camera, microphone, and vibration motor. |
| `loop()` | Streams audio while recording; polls touch buttons when idle. |
| `initSDCard()` | Mounts the SD card via SPI. Returns `true` on success. |
| `initCamera()` | Configures and starts the OV2640 camera. Returns `true` on success. |
| `initMicrophone()` | Installs the I2S driver in PDM-RX mode. Returns `true` on success. |
| `initVibrationMotor()` | Configures GPIO 3 with LEDC PWM for motor control. |
| `isTouched(pin)` | Reads capacitive touch value, applies debounce, waits for release. |
| `startRecording()` | Opens WAV file, sets recording flag, vibrates twice (buzz-buzz). |
| `recordAudioChunk()` | Reads one I2S buffer and writes it to the open WAV file (called from loop). |
| `stopRecording()` | Patches WAV header, closes file, vibrates once long (buuuzz). |
| `captureImage()` | Captures photo, vibrates once medium (buzz). |
| `writeWavHeader(file, dataSize)` | Writes a standard 44-byte RIFF/WAV header for 16-bit mono PCM. |
| `saveJpeg(path)` | Grabs camera frame buffer, writes JPEG bytes to file, releases buffer. |
| `generateNextFilename(prefix, ext, index)` | Creates sequential filenames, skipping any that already exist on SD. |
| `vibrateOnce(durationMs)` | Single vibration pulse at configured intensity. |
| `vibratePattern(pulses, onMs, offMs)` | Multiple vibration pulses with configurable timing. |

---

## Audio Format

| Parameter | Value |
|---|---|
| Sample Rate | 16 000 Hz |
| Bit Depth | 16-bit |
| Channels | 1 (Mono) |
| Format | PCM (uncompressed) |
| Container | WAV (RIFF) |
| Duration | Variable (touch to start, touch again to stop) |
| File Size | ~32 KB per second of recording |

---

## Hardware Setup

1. Attach the **Sense expansion board** to the XIAO ESP32S3 via the B2B connector.
2. Insert a **FAT32-formatted** microSD card (≤ 32 GB recommended).
3. Connect the board via USB-C.
4. If the board does not appear as a COM port, hold **BOOT** while plugging in.

---

## Touch Threshold Calibration

The default threshold is `40000`. To calibrate for your setup:

1. Upload the sketch and open the Serial Monitor at **115200 baud**.
2. Add a temporary `Serial.println(touchRead(1));` inside `loop()`.
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

---

## License

MIT — use freely in your projects.
