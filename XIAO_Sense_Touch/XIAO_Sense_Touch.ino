/*
 * =============================================================================
 *  XIAO ESP32S3 Sense — Memory Recorder (Photo + Audio) + WiFi File Browser
 * =============================================================================
 *
 *  Board  : Seeed Studio XIAO ESP32S3 Sense
 *  IDE    : Arduino IDE 2.x+
 *
 *  Concept
 *  -------
 *  A "Memory" is a photo + audio recording saved together.
 *
 *  • First touch  (GPIO 1 / D0) → Capture photo + Start audio recording
 *  • Second touch (GPIO 1 / D0) → Stop audio recording
 *
 *  Files are saved in folders:
 *    /memory_001/photo.jpg  +  /memory_001/audio.wav
 *    /memory_002/photo.jpg  +  /memory_002/audio.wav
 *
 *  WiFi Access Point
 *  -----------------
 *  The device creates a WiFi hotspot. Connect from your phone/laptop
 *  to browse, view, and download all saved memories.
 *
 *    SSID     : XIAO_Memory
 *    Password : 12345678
 *    URL      : http://192.168.4.1
 *
 *  Haptic feedback (vibration motor on GPIO 3 / D2):
 *    Start memory : buzz-buzz  (two short pulses)
 *    Stop memory  : buuuzz     (one long pulse)
 *
 *  Libraries used (all built-in with the ESP32-S3 board package)
 *  ---------------------------------------------------------------
 *  - esp_camera.h    — camera driver
 *  - driver/i2s.h    — I2S / PDM microphone driver
 *  - FS.h / SD.h     — file system & SD card (SPI mode)
 *  - SPI.h           — SPI bus for SD card
 *  - WiFi.h          — WiFi Access Point
 *  - WebServer.h     — HTTP web server
 *
 *  Board settings in Arduino IDE
 *  ------------------------------
 *  Board        : "XIAO_ESP32S3"
 *  PSRAM        : "OPI PSRAM"          ← required for camera
 *  Flash Mode   : "QIO 80 MHz"
 *  Upload Speed : 921600
 *  Partition    : "Default 4MB with spiffs (1.2MB APP / 1.5MB SPIFFS)"
 *
 *  SD card must be formatted FAT32 (≤ 32 GB recommended).
 * =============================================================================
 */

// ─── Includes ────────────────────────────────────────────────────────────────

#include "esp_camera.h"        // Camera driver
#include <driver/i2s.h>        // I2S / PDM microphone driver
#include "FS.h"                // File system abstraction
#include "SD.h"                // SD card library (SPI mode)
#include "SPI.h"               // SPI bus
#include <WiFi.h>              // WiFi Access Point
#include <WebServer.h>         // HTTP web server

// ─── WiFi Access Point Settings ──────────────────────────────────────────────

const char* AP_SSID     = "XIAO_Memory";   // WiFi network name
const char* AP_PASSWORD = "12345678";       // WiFi password (min 8 chars)

// ─── Pin Definitions ─────────────────────────────────────────────────────────

// Touch button (ESP32-S3 capacitive touch) — single button for memory
#define TOUCH_BUTTON_MEMORY   1   // GPIO 1  (D0) — start/stop memory

// Vibration motor (connected via transistor/MOSFET to this pin)
#define VIBRATION_MOTOR_PIN   3   // GPIO 3  (D2) — vibration motor
#define VIBRATION_PWM_CHANNEL 2   // LEDC channel (0 & 1 used by camera)
#define VIBRATION_PWM_FREQ    1000 // 1 kHz PWM frequency
#define VIBRATION_PWM_RES     8   // 8-bit resolution (0–255)

// SD card (SPI mode on Sense expansion board)
#define SD_CS_PIN             21  // Chip-select for onboard SD slot

// PDM Microphone (Sense expansion board)
#define MIC_I2S_PORT          I2S_NUM_0
#define MIC_CLK_PIN           42  // PDM clock
#define MIC_DATA_PIN          41  // PDM data

// Camera (OV2640 on Sense expansion board — directly wired via B2B connector)
#define CAMERA_PWDN_PIN      -1   // Not used on XIAO
#define CAMERA_RESET_PIN     -1   // Not used on XIAO
#define CAMERA_XCLK_PIN      10
#define CAMERA_SIOD_PIN      40   // I2C SDA
#define CAMERA_SIOC_PIN      39   // I2C SCL
#define CAMERA_Y9_PIN        48
#define CAMERA_Y8_PIN        11
#define CAMERA_Y7_PIN        12
#define CAMERA_Y6_PIN        14
#define CAMERA_Y5_PIN        16
#define CAMERA_Y4_PIN        18
#define CAMERA_Y3_PIN        17
#define CAMERA_Y2_PIN        15
#define CAMERA_VSYNC_PIN      38
#define CAMERA_HREF_PIN       47
#define CAMERA_PCLK_PIN       13

// ─── Audio Recording Parameters ─────────────────────────────────────────────

#define SAMPLE_RATE           16000     // 16 kHz
#define BITS_PER_SAMPLE       16
#define CHANNELS              1         // Mono
#define I2S_DMA_BUF_COUNT     8
#define I2S_DMA_BUF_LEN       512
#define I2S_READ_BUF_SIZE     1024      // Bytes per I2S read chunk
#define MAX_RECORD_SEC        60        // Auto-stop after this many seconds

// ─── Vibration Motor Parameters ──────────────────────────────────────────────

// Vibration intensity: 0 (off) to 255 (max). Change this to adjust strength.
// Suggested values:  64 = light,  128 = medium,  200 = strong,  255 = max
static uint8_t vibrationIntensity = 128;   // ← DEFAULT: medium

// ─── Touch Sensor Parameters ─────────────────────────────────────────────────

// On ESP32-S3, touchRead() values INCREASE when touched.
// Adjust this threshold after checking Serial output during first boot.
#define TOUCH_THRESHOLD       40000
#define DEBOUNCE_MS           300       // Milliseconds between re-reads

// ─── Global State ────────────────────────────────────────────────────────────

static int  memoryIndex     = 0;      // Next index for memory_NNN files
static bool sdReady         = false;
static bool cameraReady     = false;
static bool micReady        = false;
static bool isRecording     = false;  // True while a memory is being recorded

// Recording state (used while isRecording == true)
static File    recordFile;            // Open WAV file handle
static uint32_t totalBytesWritten = 0;
static unsigned long recordStartMs = 0; // millis() when recording started

// Web server instance
WebServer server(80);

// ─── Forward Declarations ────────────────────────────────────────────────────

bool   initSDCard();
bool   initCamera();
bool   initMicrophone();
void   initVibrationMotor();
void   initWiFiAP();
void   setupWebServer();
void   startMemory();
void   stopMemory();
void   recordAudioChunk();
int    findNextMemoryIndex();
bool   savePhoto(const char* path);
void   writeWavHeader(File &file, uint32_t dataSize);
bool   isTouched(int pin);
void   vibrateOnce(unsigned long durationMs);
void   vibratePattern(int pulses, unsigned long onMs, unsigned long offMs);

// Web server handlers
void   handleRoot();
void   handleListMemories();
void   handleFile();
void   handleDelete();

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);  // Give Serial time to connect
  Serial.println();
  Serial.println("==========================================");
  Serial.println("  XIAO ESP32S3 Sense — Memory Recorder");
  Serial.println("==========================================");

  // Initialise peripherals — each can fail independently
  sdReady     = initSDCard();
  cameraReady = initCamera();
  micReady    = initMicrophone();
  initVibrationMotor();
  initWiFiAP();
  setupWebServer();

  Serial.println();
  Serial.println("--- Status Summary ---");
  Serial.printf("  SD Card   : %s\n", sdReady     ? "OK" : "FAIL");
  Serial.printf("  Camera    : %s\n", cameraReady ? "OK" : "FAIL");
  Serial.printf("  Microphone: %s\n", micReady    ? "OK" : "FAIL");
  Serial.printf("  Vibration : intensity %d/255\n", vibrationIntensity);
  Serial.printf("  WiFi AP   : %s\n", AP_SSID);
  Serial.println("  Web UI    : http://192.168.4.1");
  Serial.println("----------------------");
  Serial.println();
  Serial.println("Touch GPIO1 → Start a new Memory (photo + audio)");
  Serial.println("Touch GPIO1 again → Stop recording & save Memory");
  Serial.println();
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
  // Always handle web server requests
  server.handleClient();

  // ── While recording: stream audio data and check for stop ──
  if (isRecording) {
    // Keep writing audio chunks to the SD card
    recordAudioChunk();

    // Auto-stop if max duration reached
    if ((millis() - recordStartMs) >= (unsigned long)MAX_RECORD_SEC * 1000UL) {
      Serial.println("[Auto] Max recording time reached.");
      stopMemory();
    }
    // Check if user wants to stop manually
    else if (isTouched(TOUCH_BUTTON_MEMORY)) {
      stopMemory();
    }
    return;  // Don't check for new memory touch while recording
  }

  // ── Idle: wait for touch to start a new Memory ──
  if (isTouched(TOUCH_BUTTON_MEMORY)) {
    Serial.println("[Touch] Detected — Starting new Memory...");
    if (!sdReady) {
      Serial.println("[Error] SD card not available.");
    } else if (!cameraReady) {
      Serial.println("[Error] Camera not available.");
    } else if (!micReady) {
      Serial.println("[Error] Microphone not available.");
    } else {
      startMemory();
    }
  }

  delay(50);  // Small delay to keep loop responsive without hammering CPU
}

// =============================================================================
//  TOUCH DETECTION  (with debounce)
// =============================================================================

/**
 * Returns true once per touch event.
 * On ESP32-S3 the touch value rises when the pad is touched.
 * A simple re-read after DEBOUNCE_MS filters accidental triggers.
 */
bool isTouched(int pin) {
  uint32_t val = touchRead(pin);
  if (val > TOUCH_THRESHOLD) {
    delay(DEBOUNCE_MS);
    val = touchRead(pin);
    if (val > TOUCH_THRESHOLD) {
      // Wait for release so one touch = one action
      while (touchRead(pin) > TOUCH_THRESHOLD) {
        delay(50);
      }
      return true;
    }
  }
  return false;
}

// =============================================================================
//  SD CARD INITIALISATION
// =============================================================================

bool initSDCard() {
  Serial.println("Initializing SD card...");

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[Error] SD card mount failed!");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[Error] No SD card detected.");
    return false;
  }

  Serial.printf("SD card OK — type: ");
  switch (cardType) {
    case CARD_MMC:  Serial.println("MMC");   break;
    case CARD_SD:   Serial.println("SD");    break;
    case CARD_SDHC: Serial.println("SDHC");  break;
    default:        Serial.println("Unknown"); break;
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD card size: %llu MB\n", cardSize);
  return true;
}

// =============================================================================
//  CAMERA INITIALISATION
// =============================================================================

bool initCamera() {
  Serial.println("Initializing camera...");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = CAMERA_Y2_PIN;
  config.pin_d1       = CAMERA_Y3_PIN;
  config.pin_d2       = CAMERA_Y4_PIN;
  config.pin_d3       = CAMERA_Y5_PIN;
  config.pin_d4       = CAMERA_Y6_PIN;
  config.pin_d5       = CAMERA_Y7_PIN;
  config.pin_d6       = CAMERA_Y8_PIN;
  config.pin_d7       = CAMERA_Y9_PIN;
  config.pin_xclk     = CAMERA_XCLK_PIN;
  config.pin_pclk     = CAMERA_PCLK_PIN;
  config.pin_vsync    = CAMERA_VSYNC_PIN;
  config.pin_href     = CAMERA_HREF_PIN;
  config.pin_sccb_sda = CAMERA_SIOD_PIN;
  config.pin_sccb_scl = CAMERA_SIOC_PIN;
  config.pin_pwdn     = CAMERA_PWDN_PIN;
  config.pin_reset    = CAMERA_RESET_PIN;
  config.xclk_freq_hz = 20000000;           // 20 MHz XCLK
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_UXGA;     // 1600×1200
  config.jpeg_quality = 12;                 // 0-63 (lower = better quality)
  config.fb_count     = 1;                  // Single frame buffer
  config.fb_location  = CAMERA_FB_IN_PSRAM; // Frame buffer in PSRAM
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Error] Camera init failed: 0x%x\n", err);
    return false;
  }

  Serial.println("Camera ready.");
  return true;
}

// =============================================================================
//  MICROPHONE INITIALISATION  (I2S / PDM)
// =============================================================================

bool initMicrophone() {
  Serial.println("Initializing microphone...");

  // I2S configuration for PDM microphone
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = I2S_DMA_BUF_COUNT,
    .dma_buf_len          = I2S_DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  // Pin configuration
  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_PIN_NO_CHANGE,   // Not used in PDM mode
    .ws_io_num    = MIC_CLK_PIN,          // GPIO 42 — PDM clock
    .data_out_num = I2S_PIN_NO_CHANGE,    // Not used (RX only)
    .data_in_num  = MIC_DATA_PIN          // GPIO 41 — PDM data
  };

  // Install I2S driver
  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[Error] I2S driver install failed: 0x%x\n", err);
    return false;
  }

  // Set pins
  err = i2s_set_pin(MIC_I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[Error] I2S set pin failed: 0x%x\n", err);
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }

  Serial.println("Microphone ready.");
  return true;
}

// =============================================================================
//  WiFi ACCESS POINT
// =============================================================================

/**
 * Creates a WiFi hotspot so you can connect from a phone or laptop
 * to browse the saved memories via a web browser.
 */
void initWiFiAP() {
  Serial.println("Starting WiFi Access Point...");
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("WiFi AP ready — SSID: %s\n", AP_SSID);
  Serial.printf("Connect and open: http://%s\n", ip.toString().c_str());
}

// =============================================================================
//  WEB SERVER SETUP
// =============================================================================

void setupWebServer() {
  server.on("/",          HTTP_GET, handleRoot);
  server.on("/api/list",  HTTP_GET, handleListMemories);
  server.on("/file",      HTTP_GET, handleFile);
  server.on("/api/delete",HTTP_GET, handleDelete);
  server.begin();
  Serial.println("Web server started on port 80.");
}

// =============================================================================
//  MEMORY — START  (Photo + Audio Recording)
// =============================================================================

/**
 * Starts a new "Memory":
 *  1. Finds the next available index (e.g. 005)
 *  2. Creates a folder  → /memory_005/
 *  3. Captures a photo  → /memory_005/photo.jpg
 *  4. Opens a WAV file  → /memory_005/audio.wav  and starts recording
 *
 * If the photo fails, the memory is aborted (no audio file is created).
 */
void startMemory() {
  // Find next available index (folder doesn't exist yet)
  int idx = findNextMemoryIndex();

  // Build folder and file paths
  char folderPath[32];
  char jpgPath[48];
  char wavPath[48];
  snprintf(folderPath, sizeof(folderPath), "/memory_%03d", idx);
  snprintf(jpgPath,    sizeof(jpgPath),    "/memory_%03d/photo.jpg", idx);
  snprintf(wavPath,    sizeof(wavPath),    "/memory_%03d/audio.wav", idx);

  Serial.println("────────────────────────────────");
  Serial.printf("  Memory #%03d — Starting\n", idx);
  Serial.println("────────────────────────────────");

  // ── Step 1: Create folder ──
  if (!SD.mkdir(folderPath)) {
    Serial.printf("[Error] Could not create folder %s — Memory aborted.\n", folderPath);
    return;
  }
  Serial.printf("  📁 Folder created: %s\n", folderPath);

  // ── Step 2: Capture photo ──
  Serial.printf("  📷 Capturing photo → %s\n", jpgPath);
  if (!savePhoto(jpgPath)) {
    Serial.println("[Error] Photo capture failed — Memory aborted.");
    return;
  }

  // ── Step 3: Start audio recording ──
  Serial.printf("  🎙️ Recording audio → %s\n", wavPath);
  recordFile = SD.open(wavPath, FILE_WRITE);
  if (!recordFile) {
    Serial.println("[Error] Could not create WAV file — Memory aborted.");
    return;
  }

  // Write a placeholder WAV header (44 bytes) — patched on stop
  writeWavHeader(recordFile, 0);

  totalBytesWritten = 0;
  recordStartMs = millis();
  isRecording = true;

  // Haptic feedback: two short pulses = "memory started"
  vibratePattern(2, 80, 80);

  Serial.println("  Touch again to stop recording.");
  Serial.println("────────────────────────────────");
}

// =============================================================================
//  MEMORY — STOP  (Finalize Audio)
// =============================================================================

/**
 * Stops the audio recording, patches the WAV header, closes the file,
 * and prints a summary of the completed Memory.
 */
void stopMemory() {
  isRecording = false;

  // Patch the WAV header with the real data size
  recordFile.seek(0);
  writeWavHeader(recordFile, totalBytesWritten);
  recordFile.close();

  // Haptic feedback: one long pulse = "memory saved"
  vibrateOnce(300);

  // Calculate duration for the user
  float durationSec = (float)totalBytesWritten
                      / (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));

  Serial.println("────────────────────────────────");
  Serial.println("  ✅ Memory Saved!");
  Serial.printf("  Audio duration: %.1f seconds\n", durationSec);
  Serial.printf("  Audio size: %u bytes\n", totalBytesWritten + 44);
  Serial.println("────────────────────────────────\n");
}

// =============================================================================
//  AUDIO CHUNK (called from loop while recording)
// =============================================================================

void recordAudioChunk() {
  uint8_t buffer[I2S_READ_BUF_SIZE];
  size_t bytesRead = 0;

  esp_err_t result = i2s_read(MIC_I2S_PORT, buffer, I2S_READ_BUF_SIZE,
                              &bytesRead, portMAX_DELAY);
  if (result == ESP_OK && bytesRead > 0) {
    recordFile.write(buffer, bytesRead);
    totalBytesWritten += bytesRead;
  }
}

// =============================================================================
//  SAVE PHOTO
// =============================================================================

bool savePhoto(const char* path) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Error] Camera capture failed.");
    return false;
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("[Error] Could not create image file.");
    esp_camera_fb_return(fb);
    return false;
  }

  file.write(fb->buf, fb->len);
  file.close();
  Serial.printf("  Photo saved (%u bytes)\n", fb->len);
  esp_camera_fb_return(fb);
  return true;
}

// =============================================================================
//  WAV HEADER
// =============================================================================

void writeWavHeader(File &file, uint32_t dataSize) {
  uint32_t byteRate   = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);

  file.write((const uint8_t*)"RIFF", 4);
  uint32_t chunkSize = 36 + dataSize;
  file.write((const uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);

  file.write((const uint8_t*)"fmt ", 4);
  uint32_t subchunk1Size = 16;
  file.write((const uint8_t*)&subchunk1Size, 4);
  uint16_t audioFormat = 1;
  file.write((const uint8_t*)&audioFormat, 2);
  uint16_t numChannels = CHANNELS;
  file.write((const uint8_t*)&numChannels, 2);
  uint32_t sampleRate = SAMPLE_RATE;
  file.write((const uint8_t*)&sampleRate, 4);
  file.write((const uint8_t*)&byteRate, 4);
  file.write((const uint8_t*)&blockAlign, 2);
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  file.write((const uint8_t*)&bitsPerSample, 2);

  file.write((const uint8_t*)"data", 4);
  file.write((const uint8_t*)&dataSize, 4);
}

// =============================================================================
//  FILENAME INDEX GENERATOR
// =============================================================================

int findNextMemoryIndex() {
  char folderPath[32];
  do {
    memoryIndex++;
    snprintf(folderPath, sizeof(folderPath), "/memory_%03d", memoryIndex);
  } while (SD.exists(folderPath));
  return memoryIndex;
}

// =============================================================================
//  VIBRATION MOTOR
// =============================================================================

void initVibrationMotor() {
  Serial.println("Initializing vibration motor...");
  ledcSetup(VIBRATION_PWM_CHANNEL, VIBRATION_PWM_FREQ, VIBRATION_PWM_RES);
  ledcAttachPin(VIBRATION_MOTOR_PIN, VIBRATION_PWM_CHANNEL);
  ledcWrite(VIBRATION_PWM_CHANNEL, 0);
  Serial.println("Vibration motor ready.");
}

void vibrateOnce(unsigned long durationMs) {
  ledcWrite(VIBRATION_PWM_CHANNEL, vibrationIntensity);
  delay(durationMs);
  ledcWrite(VIBRATION_PWM_CHANNEL, 0);
}

void vibratePattern(int pulses, unsigned long onMs, unsigned long offMs) {
  for (int i = 0; i < pulses; i++) {
    ledcWrite(VIBRATION_PWM_CHANNEL, vibrationIntensity);
    delay(onMs);
    ledcWrite(VIBRATION_PWM_CHANNEL, 0);
    if (i < pulses - 1) delay(offMs);
  }
}

// =============================================================================
//  WEB SERVER — HTML PAGE
// =============================================================================

/**
 * Serves the main web UI — a beautiful, mobile-friendly dark-themed page
 * that lists all memories with photo previews and audio players.
 */
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>XIAO Memory Browser</title>
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }

    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: #0a0a0f;
      color: #e0e0e0;
      min-height: 100vh;
    }

    header {
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      padding: 20px;
      text-align: center;
      border-bottom: 1px solid #2a2a4a;
      position: sticky;
      top: 0;
      z-index: 100;
    }

    header h1 {
      font-size: 1.4em;
      font-weight: 600;
      background: linear-gradient(135deg, #667eea, #764ba2);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      margin-bottom: 4px;
    }

    header p {
      font-size: 0.8em;
      color: #888;
    }

    .status-bar {
      display: flex;
      justify-content: center;
      gap: 16px;
      margin-top: 10px;
      font-size: 0.75em;
    }

    .status-dot {
      width: 8px; height: 8px;
      border-radius: 50%;
      display: inline-block;
      margin-right: 4px;
    }

    .dot-green { background: #4ade80; box-shadow: 0 0 6px #4ade80; }
    .dot-red   { background: #f87171; box-shadow: 0 0 6px #f87171; }

    .container {
      max-width: 600px;
      margin: 0 auto;
      padding: 16px;
    }

    .empty-state {
      text-align: center;
      padding: 60px 20px;
      color: #666;
    }

    .empty-state .icon { font-size: 3em; margin-bottom: 12px; }

    .memory-card {
      background: linear-gradient(145deg, #151520, #1a1a2e);
      border: 1px solid #2a2a4a;
      border-radius: 16px;
      margin-bottom: 16px;
      overflow: hidden;
      transition: transform 0.2s, box-shadow 0.2s;
    }

    .memory-card:hover {
      transform: translateY(-2px);
      box-shadow: 0 8px 24px rgba(102, 126, 234, 0.15);
    }

    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 16px;
      border-bottom: 1px solid #2a2a4a;
    }

    .card-title {
      font-weight: 600;
      font-size: 0.95em;
      color: #b8b8d0;
    }

    .card-title span {
      background: linear-gradient(135deg, #667eea, #764ba2);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
    }

    .delete-btn {
      background: none;
      border: 1px solid #3a2020;
      color: #f87171;
      padding: 4px 10px;
      border-radius: 6px;
      font-size: 0.75em;
      cursor: pointer;
      transition: all 0.2s;
    }

    .delete-btn:hover { background: #3a2020; }

    .card-photo {
      width: 100%;
      aspect-ratio: 4/3;
      object-fit: cover;
      display: block;
      background: #111;
      cursor: pointer;
    }

    .card-audio {
      padding: 12px 16px;
      border-top: 1px solid #2a2a4a;
    }

    .card-audio audio {
      width: 100%;
      height: 36px;
      border-radius: 8px;
    }

    .card-actions {
      display: flex;
      gap: 8px;
      padding: 10px 16px;
      border-top: 1px solid #2a2a4a;
    }

    .dl-btn {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      padding: 8px;
      border-radius: 8px;
      border: 1px solid #2a2a4a;
      background: #151520;
      color: #b8b8d0;
      text-decoration: none;
      font-size: 0.8em;
      transition: all 0.2s;
    }

    .dl-btn:hover {
      background: #1a1a2e;
      border-color: #667eea;
      color: #667eea;
    }

    .loading {
      text-align: center;
      padding: 40px;
      color: #666;
    }

    .spinner {
      width: 32px; height: 32px;
      border: 3px solid #2a2a4a;
      border-top-color: #667eea;
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
      margin: 0 auto 12px;
    }

    @keyframes spin { to { transform: rotate(360deg); } }

    /* Lightbox */
    .lightbox {
      display: none;
      position: fixed;
      inset: 0;
      background: rgba(0,0,0,0.92);
      z-index: 200;
      justify-content: center;
      align-items: center;
    }

    .lightbox.active { display: flex; }

    .lightbox img {
      max-width: 95%;
      max-height: 90vh;
      border-radius: 8px;
    }

    .lightbox-close {
      position: absolute;
      top: 16px;
      right: 16px;
      background: rgba(255,255,255,0.1);
      border: none;
      color: white;
      width: 40px; height: 40px;
      border-radius: 50%;
      font-size: 1.2em;
      cursor: pointer;
    }

    .rec-badge {
      display: inline-block;
      background: #f87171;
      color: white;
      font-size: 0.65em;
      padding: 2px 8px;
      border-radius: 10px;
      animation: pulse 1.5s infinite;
      margin-left: 8px;
    }

    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
  </style>
</head>
<body>
  <header>
    <h1>📷 XIAO Memory Browser</h1>
    <p>Seeed Studio XIAO ESP32S3 Sense</p>
    <div class="status-bar">
      <span><span class="status-dot dot-green"></span> Connected</span>
      <span id="memCount">— memories</span>
    </div>
  </header>

  <div class="container" id="app">
    <div class="loading">
      <div class="spinner"></div>
      <p>Loading memories...</p>
    </div>
  </div>

  <div class="lightbox" id="lightbox" onclick="closeLightbox()">
    <button class="lightbox-close" onclick="closeLightbox()">✕</button>
    <img id="lightbox-img" src="" alt="Photo">
  </div>

  <script>
    async function loadMemories() {
      try {
        const res = await fetch('/api/list');
        const data = await res.json();
        const app = document.getElementById('app');
        const memCount = document.getElementById('memCount');

        if (data.memories.length === 0) {
          app.innerHTML = `
            <div class="empty-state">
              <div class="icon">📭</div>
              <p>No memories yet</p>
              <p style="margin-top:8px;font-size:0.85em">Touch the sensor to create your first memory</p>
            </div>`;
          memCount.textContent = '0 memories';
          return;
        }

        memCount.textContent = data.memories.length + ' memories';

        let html = '';
        data.memories.forEach(m => {
          const photoUrl = '/file?path=' + encodeURIComponent(m.photo);
          const audioUrl = '/file?path=' + encodeURIComponent(m.audio);

          html += `
          <div class="memory-card" id="card-${m.name}">
            <div class="card-header">
              <div class="card-title">🧠 <span>${m.name}</span></div>
              <button class="delete-btn" onclick="deleteMemory('${m.name}')">🗑 Delete</button>
            </div>
            ${m.hasPhoto ? `<img class="card-photo" src="${photoUrl}" alt="Photo" loading="lazy" onclick="openLightbox('${photoUrl}')">` : ''}
            ${m.hasAudio ? `
            <div class="card-audio">
              <audio controls preload="none">
                <source src="${audioUrl}" type="audio/wav">
              </audio>
            </div>` : ''}
            <div class="card-actions">
              ${m.hasPhoto ? `<a class="dl-btn" href="${photoUrl}" download="${m.name}_photo.jpg">📷 Download Photo</a>` : ''}
              ${m.hasAudio ? `<a class="dl-btn" href="${audioUrl}" download="${m.name}_audio.wav">🎙 Download Audio</a>` : ''}
            </div>
          </div>`;
        });
        app.innerHTML = html;

      } catch (err) {
        document.getElementById('app').innerHTML =
          '<div class="empty-state"><div class="icon">⚠️</div><p>Failed to load</p></div>';
      }
    }

    async function deleteMemory(name) {
      if (!confirm('Delete ' + name + '? This cannot be undone.')) return;
      try {
        const res = await fetch('/api/delete?name=' + encodeURIComponent(name));
        if (res.ok) {
          document.getElementById('card-' + name).remove();
          // Update count
          const cards = document.querySelectorAll('.memory-card');
          document.getElementById('memCount').textContent = cards.length + ' memories';
          if (cards.length === 0) loadMemories();
        }
      } catch (e) { alert('Delete failed'); }
    }

    function openLightbox(url) {
      document.getElementById('lightbox-img').src = url;
      document.getElementById('lightbox').classList.add('active');
    }

    function closeLightbox() {
      document.getElementById('lightbox').classList.remove('active');
    }

    loadMemories();
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// =============================================================================
//  WEB SERVER — LIST MEMORIES API
// =============================================================================

/**
 * Returns a JSON array of all memory folders with their contents.
 * Example: {"memories":[{"name":"memory_001","photo":"/memory_001/photo.jpg",
 *           "audio":"/memory_001/audio.wav","hasPhoto":true,"hasAudio":true}]}
 */
void handleListMemories() {
  if (!sdReady) {
    server.send(500, "application/json", "{\"error\":\"SD card not available\"}");
    return;
  }

  String json = "{\"memories\":[";
  File root = SD.open("/");
  bool first = true;

  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry) {
      String name = entry.name();
      // Only include memory_xxx directories
      if (entry.isDirectory() && name.startsWith("memory_")) {
        if (!first) json += ",";
        first = false;

        String photoPath = "/" + name + "/photo.jpg";
        String audioPath = "/" + name + "/audio.wav";
        bool hasPhoto = SD.exists(photoPath);
        bool hasAudio = SD.exists(audioPath);

        json += "{\"name\":\"" + name + "\"";
        json += ",\"photo\":\"" + photoPath + "\"";
        json += ",\"audio\":\"" + audioPath + "\"";
        json += ",\"hasPhoto\":" + String(hasPhoto ? "true" : "false");
        json += ",\"hasAudio\":" + String(hasAudio ? "true" : "false");
        json += "}";
      }
      entry.close();
      entry = root.openNextFile();
    }
    root.close();
  }

  json += "]}";
  server.send(200, "application/json", json);
}

// =============================================================================
//  WEB SERVER — SERVE FILE
// =============================================================================

/**
 * Streams a file from the SD card to the browser.
 * Usage: /file?path=/memory_001/photo.jpg
 */
void handleFile() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing 'path' parameter");
    return;
  }

  String path = server.arg("path");
  if (!SD.exists(path)) {
    server.send(404, "text/plain", "File not found: " + path);
    return;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Cannot open file");
    return;
  }

  // Determine content type from extension
  String contentType = "application/octet-stream";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    contentType = "image/jpeg";
  } else if (path.endsWith(".wav")) {
    contentType = "audio/wav";
  } else if (path.endsWith(".png")) {
    contentType = "image/png";
  }

  // Stream the file to the client
  server.streamFile(file, contentType);
  file.close();
}

// =============================================================================
//  WEB SERVER — DELETE MEMORY
// =============================================================================

/**
 * Deletes a memory folder and all its contents.
 * Usage: /api/delete?name=memory_001
 */
void handleDelete() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing 'name' parameter");
    return;
  }

  String name = server.arg("name");
  String folderPath = "/" + name;

  // Safety check: only allow deleting memory_xxx folders
  if (!name.startsWith("memory_")) {
    server.send(403, "text/plain", "Can only delete memory folders");
    return;
  }

  if (!SD.exists(folderPath)) {
    server.send(404, "text/plain", "Folder not found");
    return;
  }

  // Delete files inside the folder first
  File dir = SD.open(folderPath);
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      String filePath = folderPath + "/" + String(f.name());
      f.close();
      SD.remove(filePath);
      f = dir.openNextFile();
    }
    dir.close();
  }

  // Remove the empty folder
  SD.rmdir(folderPath);

  Serial.printf("[Web] Deleted: %s\n", folderPath.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}
