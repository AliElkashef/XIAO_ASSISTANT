/*
 * =============================================================================
 *  XIAO ESP32S3 Sense — Memory Recorder (Event-Driven Deep Sleep)
 * =============================================================================
 *
 *  Board  : Seeed Studio XIAO ESP32S3 Sense
 *  IDE    : Arduino IDE 2.x+
 *
 *  Architecture
 *  ------------
 *  The device stays in DEEP SLEEP most of the time (~10 µA).
 *  It wakes ONLY when the user touches the capacitive touch sensor.
 *
 *  Two operating modes are determined by touch duration after wakeup:
 *
 *  Mode 1 — SHORT TOUCH → Capture Memory
 *    Wake → Photo + Audio → Save to SD → Sleep
 *
 *  Mode 2 — LONG TOUCH → Web Server
 *    Wake → Start WiFi AP + Web UI → Browse files → Idle timeout → Sleep
 *
 *  Files are saved in folders:
 *    /memory_001/photo.jpg  +  /memory_001/audio.wav
 *    /memory_002/photo.jpg  +  /memory_002/audio.wav
 *
 *  WiFi Access Point (Mode 2 only)
 *  --------------------------------
 *    SSID     : XIAO_Memory
 *    Password : 12345678
 *    URL      : http://192.168.4.1
 *
 *  Haptic Feedback (vibration motor on GPIO 3 / D2):
 *    Wakeup confirm     : buzz             (one short pulse)
 *    Memory started     : buzz-buzz        (two short pulses)
 *    Memory saved       : buuuzz           (one long pulse)
 *    Web server started : buzz-buzz-buzz   (three short pulses)
 *    Going to sleep     : bz-bz-bz        (three quick pulses)
 *
 *  Libraries (all built-in with the ESP32-S3 board package)
 *  ---------------------------------------------------------
 *  - esp_camera.h / driver/i2s.h / FS.h / SD.h / SPI.h
 *  - WiFi.h / WebServer.h / esp_sleep.h
 *
 *  Board Settings
 *  ---------------
 *  Board        : "XIAO_ESP32S3"
 *  PSRAM        : "OPI PSRAM"          ← required for camera
 *  USB CDC      : Enabled              ← required for Serial
 *  Flash Mode   : "QIO 80 MHz"
 *  Upload Speed : 921600
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
#include <esp_sleep.h>         // Deep sleep API

// ─── WiFi Access Point Settings ──────────────────────────────────────────────

const char* AP_SSID     = "XIAO_Memory";   // WiFi network name
const char* AP_PASSWORD = "12345678";       // WiFi password (min 8 chars)

// ─── Pin Definitions ─────────────────────────────────────────────────────────

// Touch button (ESP32-S3 capacitive touch)
#define TOUCH_BUTTON_MEMORY   1   // GPIO 1  (D0) — single button

// Vibration motor (connected via transistor/MOSFET)
#define VIBRATION_MOTOR_PIN   3   // GPIO 3  (D2)
#define VIBRATION_PWM_CHANNEL 2   // LEDC channel (0 & 1 used by camera)
#define VIBRATION_PWM_FREQ    1000
#define VIBRATION_PWM_RES     8   // 8-bit resolution (0–255)

// SD card (SPI mode on Sense expansion board)
#define SD_CS_PIN             21  // Chip-select for onboard SD slot

// PDM Microphone (Sense expansion board)
#define MIC_I2S_PORT          I2S_NUM_0
#define MIC_CLK_PIN           42  // PDM clock
#define MIC_DATA_PIN          41  // PDM data

// Camera (OV2640 on Sense expansion board)
#define CAMERA_PWDN_PIN      -1
#define CAMERA_RESET_PIN     -1
#define CAMERA_XCLK_PIN      10
#define CAMERA_SIOD_PIN      40
#define CAMERA_SIOC_PIN      39
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

// ─── Touch & Mode Detection Parameters ──────────────────────────────────────

// On ESP32-S3, touchRead() values INCREASE when touched.
#define THRESHOLD_MULTIPLIER   1.8    // Wakeup threshold = Baseline * multiplier
#define NOISE_MARGIN_RATIO     1.3    // Threshold below which we consider the sensor untouched
#define EMA_ALPHA              0.15   // Adaptation rate (higher since sleep boots are infrequent)
#define CALIBRATION_SAMPLES    50     // Samples for initial boot calibration
#define CALIBRATION_DELAY_MS   10     // Delay between calibration samples
#define DEBOUNCE_MS            300    // Debounce delay

// Touch hold duration to distinguish short vs long touch.
// After wakeup, if the user holds for longer than this → Mode 2 (Web Server)
#define LONG_TOUCH_MS          2000   // 2 seconds = long touch

// ─── Web Server Timeout ──────────────────────────────────────────────────────

// In Mode 2, how many seconds of no web activity (and no WiFi clients)
// before the device returns to deep sleep.
#define WEB_TIMEOUT_SEC       120       // 2 minutes

// ─── Vibration Motor Parameters ──────────────────────────────────────────────

// Vibration intensity: 0 (off) to 255 (max).
// Suggested: 64=light, 128=medium, 200=strong, 255=max
static uint8_t vibrationIntensity = 200;

// ─── Touch Type Enum ─────────────────────────────────────────────────────────

enum TouchType {
  TOUCH_NONE,       // Cold boot or unknown wakeup
  TOUCH_SHORT,      // Short touch → Mode 1 (Capture Memory)
  TOUCH_LONG        // Long touch  → Mode 2 (Web Server)
};

// ─── Global State ────────────────────────────────────────────────────────────

// Persists across deep sleep cycles (stored in RTC slow memory)
RTC_DATA_ATTR int   memoryIndex = 0;
RTC_DATA_ATTR float rtcBaseline = 0.0; // Dynamic baseline stored in RTC memory

// Runtime state
static bool sdReady          = false;  // Needed by web handlers
static bool webServerMode    = false;  // True only in Mode 2
static unsigned long lastActivityMs = 0;

// Web server instance (used only in Mode 2)
WebServer server(80);

// ─── Forward Declarations ────────────────────────────────────────────────────

// Core architecture
TouchType detectTouchType();
void      captureMemory();
void      startWebServerMode();
void      checkWebTimeout();
void      goToSleep();
void      waitForSerial(unsigned long maxWaitMs);

// Peripheral init (EXISTING — unchanged)
bool   initSDCard();
bool   initCamera();
bool   initMicrophone();
void   initVibrationMotor();
void   initWiFiAP();
void   setupWebServer();

// Peripheral deinit (NEW)
void   deinitCamera();
void   deinitMicrophone();

// Memory capture helpers (EXISTING — unchanged)
bool   savePhoto(const char* path);
void   recordAudioChunk();
void   writeWavHeader(File &file, uint32_t dataSize);
int    findNextMemoryIndex();

// Vibration (EXISTING — unchanged)
void   vibrateOnce(unsigned long durationMs);
void   vibratePattern(int pulses, unsigned long onMs, unsigned long offMs);

// Web server handlers (EXISTING — unchanged)
void   handleRoot();
void   handleListMemories();
void   handleFile();
void   handleDelete();

// =============================================================================
//  SETUP — Event-driven entry point
// =============================================================================

void setup() {
  Serial.begin(115200);
  waitForSerial(4000);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("  XIAO ESP32S3 Sense — Memory Recorder");
  Serial.println("       (Event-Driven Deep Sleep)");
  Serial.println("==========================================");
  delay(5000);
  // Always init vibration motor first (needed for all modes)
  initVibrationMotor();

  // Establish Baseline on First Boot
  if (rtcBaseline < 10.0) {
    Serial.println("  [Calibrating] First boot baseline calibration...");
    uint64_t sum = 0;
    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
      sum += touchRead(TOUCH_BUTTON_MEMORY);
      delay(CALIBRATION_DELAY_MS);
    }
    rtcBaseline = (float)sum / CALIBRATION_SAMPLES;
    Serial.printf("  [Calibrated] Initial baseline: %.2f\n", rtcBaseline);
  }

  // Detect what kind of touch woke us up
  TouchType touchType = detectTouchType();

  switch (touchType) {

    case TOUCH_SHORT:
      // ── Mode 1: Capture Memory ──
      Serial.println();
      Serial.println(">>> MODE 1: Capture Memory (short touch)");
      Serial.println();
      vibrateOnce(100);      // Confirm wakeup
      captureMemory();       // Full pipeline: init → capture → deinit
      goToSleep();           // Sleep immediately after
      break;

    case TOUCH_LONG:
      // ── Mode 2: Web Server ──
      Serial.println();
      Serial.println(">>> MODE 2: Web Server (long touch)");
      Serial.println();
      vibratePattern(3, 80, 80);  // Triple buzz = web mode
      startWebServerMode();       // Init SD + WiFi + server
      // Falls through to loop() which handles web requests
      break;

    default:
      // Cold boot, reset, or unknown wakeup
      Serial.println();
      Serial.println("Cold boot — no touch detected.");
      Serial.println("Going to sleep. Touch GPIO1 to wake.");
      Serial.println();
      vibrateOnce(50);
      goToSleep();
      break;
  }
}

// =============================================================================
//  LOOP — Minimal (only runs in Mode 2)
// =============================================================================

void loop() {
  if (webServerMode) {
    server.handleClient();
    checkWebTimeout();
  }
  // In all other cases, the device should have entered deep sleep
  // from setup() and never reach here.
}

// =============================================================================
//  TOUCH TYPE DETECTION (after wakeup)
// =============================================================================

/**
 * After waking from deep sleep, the user's finger is still on the pad.
 * We measure how long they hold it to decide the mode:
 *
 *   Released within LONG_TOUCH_MS  → SHORT_TOUCH (Mode 1)
 *   Held longer than LONG_TOUCH_MS → LONG_TOUCH  (Mode 2)
 *   Not a touch wakeup             → TOUCH_NONE
 */
TouchType detectTouchType() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  Serial.printf("  Wakeup cause: %d", cause);
  if (cause == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    Serial.println(" (Touch)");
  } else if (cause == 0) {
    Serial.println(" (Power-on / Reset)");
  } else {
    Serial.printf(" (Other: %d)\n", cause);
  }

  // Only proceed if woken by touch
  if (cause != ESP_SLEEP_WAKEUP_TOUCHPAD) {
    return TOUCH_NONE;
  }

  // Small stabilization delay after wakeup
  delay(50);

  // Measure how long the user holds the touch after wakeup
  unsigned long holdStart = millis();
  float currentThreshold = rtcBaseline * THRESHOLD_MULTIPLIER;

  while (touchRead(TOUCH_BUTTON_MEMORY) > currentThreshold) {
    // Still holding...
    if ((millis() - holdStart) >= LONG_TOUCH_MS) {
      Serial.println("  → LONG TOUCH detected (held > 2 sec)");
      // Wait for release before continuing
      while (touchRead(TOUCH_BUTTON_MEMORY) > currentThreshold) {
        delay(50);
      }
      delay(DEBOUNCE_MS);
      return TOUCH_LONG;
    }
    delay(20);
  }

  // Released before timeout → short touch
  delay(DEBOUNCE_MS);
  Serial.println("  → SHORT TOUCH detected (quick tap)");
  return TOUCH_SHORT;
}

// =============================================================================
//  SERIAL WAIT (safe for USB CDC)
// =============================================================================

/**
 * Waits for USB CDC Serial to reconnect after deep sleep wakeup.
 * Times out to avoid hanging if no USB host is connected.
 */
void waitForSerial(unsigned long maxWaitMs) {
  unsigned long start = millis();
  while (!Serial && (millis() - start < maxWaitMs)) {
    delay(10);
  }
  delay(300);  // Extra settle time for USB host
}

// =============================================================================
//  MODE 1: CAPTURE MEMORY (self-contained pipeline)
// =============================================================================

/**
 * Complete memory capture pipeline:
 *   Init SD → Init Camera → Photo → Deinit Camera
 *   → Init Mic → Record (blocking) → Stop → Deinit Mic → Close SD
 *
 * The camera is deinitialized before recording to free ~400KB PSRAM.
 * Audio recording is a blocking loop that exits on touch or timeout.
 */
void captureMemory() {
  // ── Step 1: Init SD card ──
  sdReady = initSDCard();
  if (!sdReady) {
    Serial.println("[Error] SD card not available — aborting.");
    return;
  }

  // ── Step 2: Init Camera ──
  bool cameraReady = initCamera();
  if (!cameraReady) {
    Serial.println("[Error] Camera not available — aborting.");
    SD.end();
    return;
  }

  // ── Step 3: Find next memory index and create folder ──
  int idx = findNextMemoryIndex();

  char folderPath[32];
  char jpgPath[48];
  char wavPath[48];
  snprintf(folderPath, sizeof(folderPath), "/memory_%03d", idx);
  snprintf(jpgPath,    sizeof(jpgPath),    "/memory_%03d/photo.jpg", idx);
  snprintf(wavPath,    sizeof(wavPath),    "/memory_%03d/audio.wav", idx);

  Serial.println("────────────────────────────────");
  Serial.printf("  Memory #%03d — Starting\n", idx);
  Serial.println("────────────────────────────────");

  if (!SD.mkdir(folderPath)) {
    Serial.printf("[Error] Could not create folder %s — aborting.\n", folderPath);
    deinitCamera();
    SD.end();
    return;
  }
  Serial.printf("  📁 Folder created: %s\n", folderPath);

  // ── Step 4: Capture photo ──
  Serial.printf("  📷 Capturing photo → %s\n", jpgPath);
  if (!savePhoto(jpgPath)) {
    Serial.println("[Error] Photo capture failed — aborting.");
    deinitCamera();
    SD.end();
    return;
  }

  // ── Step 5: Deinit camera (free PSRAM before recording) ──
  deinitCamera();

  // ── Step 6: Init microphone ──
  bool micReady = initMicrophone();
  if (!micReady) {
    Serial.println("[Error] Microphone not available — photo saved but no audio.");
    SD.end();
    return;
  }

  // ── Step 7: Open WAV file and start recording ──
  Serial.printf("  🎙️ Recording audio → %s\n", wavPath);
  File recordFile = SD.open(wavPath, FILE_WRITE);
  if (!recordFile) {
    Serial.println("[Error] Could not create WAV file — aborting.");
    deinitMicrophone();
    SD.end();
    return;
  }

  // Write placeholder WAV header (patched later)
  writeWavHeader(recordFile, 0);
  uint32_t totalBytesWritten = 0;
  unsigned long recordStartMs = millis();

  // Haptic feedback: memory capture started
  vibratePattern(2, 80, 80);

  Serial.println("  ⏺ Recording... Touch again to stop.");
  Serial.println("────────────────────────────────");

  // ── Step 8: Blocking recording loop ──
  bool recording = true;
  float currentThreshold = rtcBaseline * THRESHOLD_MULTIPLIER;
  while (recording) {
    // Read audio chunk and write to SD
    uint8_t buffer[I2S_READ_BUF_SIZE];
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(MIC_I2S_PORT, buffer, I2S_READ_BUF_SIZE,
                                &bytesRead, portMAX_DELAY);
    if (result == ESP_OK && bytesRead > 0) {
      recordFile.write(buffer, bytesRead);
      totalBytesWritten += bytesRead;
    }

    // Check for stop: user touches the sensor again
    if (touchRead(TOUCH_BUTTON_MEMORY) > currentThreshold) {
      delay(DEBOUNCE_MS);
      if (touchRead(TOUCH_BUTTON_MEMORY) > currentThreshold) {
        Serial.println("  [Touch] Stop recording.");
        // Wait for release
        while (touchRead(TOUCH_BUTTON_MEMORY) > currentThreshold) {
          delay(50);
        }
        recording = false;
      }
    }

    // Check for auto-stop: max duration
    if ((millis() - recordStartMs) >= (unsigned long)MAX_RECORD_SEC * 1000UL) {
      Serial.println("  [Auto] Max recording time reached.");
      recording = false;
    }
  }

  // ── Step 9: Finalize WAV file ──
  recordFile.seek(0);
  writeWavHeader(recordFile, totalBytesWritten);
  recordFile.close();

  float durationSec = (float)totalBytesWritten
                      / (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));

  Serial.println("────────────────────────────────");
  Serial.println("  ✅ Memory Saved!");
  Serial.printf("  Audio duration: %.1f seconds\n", durationSec);
  Serial.printf("  Audio size: %u bytes\n", totalBytesWritten + 44);
  Serial.println("────────────────────────────────");
  Serial.println();

  // ── Step 10: Cleanup ──
  deinitMicrophone();
  SD.end();

  // Haptic feedback: memory saved
  vibrateOnce(300);
}

// =============================================================================
//  MODE 2: WEB SERVER MODE
// =============================================================================

/**
 * Initializes SD + WiFi + Web Server for file browsing.
 * Sets webServerMode = true so loop() handles requests.
 * The device stays awake until the inactivity timeout triggers.
 */
void startWebServerMode() {
  // Init SD card (needed for file serving)
  sdReady = initSDCard();
  if (!sdReady) {
    Serial.println("[Error] SD card not available — cannot serve files.");
    Serial.println("Going to sleep.");
    goToSleep();
    return;
  }

  // Start WiFi Access Point
  initWiFiAP();

  // Start web server
  setupWebServer();

  // Start inactivity timer
  lastActivityMs = millis();
  webServerMode = true;

  Serial.println();
  Serial.println("--- Web Server Active ---");
  Serial.printf("  SSID     : %s\n", AP_SSID);
  Serial.println("  Password : 12345678");
  Serial.println("  URL      : http://192.168.4.1");
  Serial.printf("  Timeout  : %d sec inactivity\n", WEB_TIMEOUT_SEC);
  Serial.println("-------------------------");
  Serial.println();
}

// =============================================================================
//  WEB SERVER TIMEOUT CHECK (runs in loop during Mode 2)
// =============================================================================

/**
 * Checks if the web server should shut down due to inactivity.
 * Won't sleep if a WiFi client is still connected.
 */
void checkWebTimeout() {
  // Don't sleep if a WiFi client is connected
  if (WiFi.softAPgetStationNum() > 0) {
    lastActivityMs = millis();  // Client present = active
    return;
  }

  // Check if timeout has been reached
  unsigned long elapsed = millis() - lastActivityMs;
  if (elapsed >= (unsigned long)WEB_TIMEOUT_SEC * 1000UL) {
    Serial.println();
    Serial.println("[Timeout] No web activity — shutting down.");
    webServerMode = false;
    goToSleep();
  }
}

// =============================================================================
//  GO TO SLEEP (clean shutdown + deep sleep)
// =============================================================================

/**
 * Cleanly shuts down all peripherals and enters deep sleep.
 * Configures touch pin as the wakeup source.
 * On wake, the device reboots and runs setup() again.
 */
void goToSleep() {
  Serial.println();
  Serial.println("💤 Entering deep sleep...");

  // Wait for touch release before sleeping to update baseline and prevent immediate wakeup
  float releaseThreshold = rtcBaseline * NOISE_MARGIN_RATIO;
  unsigned long releaseStart = millis();
  while (touchRead(TOUCH_BUTTON_MEMORY) > releaseThreshold && (millis() - releaseStart < 4000)) {
    delay(50); // Timeout after 4s
  }
  delay(200); // Settle time

  // Dynamic Baseline Tracking: Take 10 idle samples to update our baseline
  uint64_t sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += touchRead(TOUCH_BUTTON_MEMORY);
    delay(10);
  }
  float idleAverage = (float)sum / 10.0;

  // Only update baseline if the values are in the untouched region
  if (idleAverage < rtcBaseline * NOISE_MARGIN_RATIO) {
    float oldBaseline = rtcBaseline;
    rtcBaseline = (rtcBaseline * (1.0 - EMA_ALPHA)) + (idleAverage * EMA_ALPHA);
    Serial.printf("  [EMA Update] Baseline drifted: %.2f -> %.2f\n", oldBaseline, rtcBaseline);
  } else {
    Serial.println("  [EMA Skip] Sensor active or noisy; skipping baseline update.");
  }

  float currentThreshold = rtcBaseline * THRESHOLD_MULTIPLIER;
  Serial.printf("   Touch GPIO1 to wake up (Wake threshold: %.0f).\n", currentThreshold);
  Serial.println();

  // Flush serial before sleeping (safe check for USB CDC)
  if (Serial) {
    Serial.flush();
  }
  delay(100);

  // Vibrate: going to sleep
  vibratePattern(3, 50, 50);
  delay(200);

  // Stop web server if running
  if (webServerMode) {
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webServerMode = false;
  }

  // Close SD card if mounted
  SD.end();

  // Configure touch wakeup with dynamic threshold
  touchSleepWakeUpEnable(TOUCH_BUTTON_MEMORY, (uint32_t)currentThreshold);

  // Enter deep sleep (device reboots on wake)
  esp_deep_sleep_start();

  // Should never reach here
  Serial.println("[ERROR] Deep sleep failed!");
}

// =============================================================================
//  PERIPHERAL DEINITIALIZATION
// =============================================================================

/**
 * Releases the camera driver and frees PSRAM frame buffers.
 */
void deinitCamera() {
  esp_camera_deinit();
  Serial.println("Camera deinitialized.");
}

/**
 * Uninstalls the I2S driver to free resources.
 */
void deinitMicrophone() {
  i2s_driver_uninstall(MIC_I2S_PORT);
  Serial.println("Microphone deinitialized.");
}

// =============================================================================
//  SD CARD INITIALISATION (existing — unchanged)
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
//  CAMERA INITIALISATION (existing — unchanged)
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
//  MICROPHONE INITIALISATION (existing — unchanged)
// =============================================================================

bool initMicrophone() {
  Serial.println("Initializing microphone...");

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

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_PIN_NO_CHANGE,
    .ws_io_num    = MIC_CLK_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_DATA_PIN
  };

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[Error] I2S driver install failed: 0x%x\n", err);
    return false;
  }

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
//  WiFi ACCESS POINT (existing — unchanged)
// =============================================================================

void initWiFiAP() {
  Serial.println("Starting WiFi Access Point...");
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("WiFi AP ready — SSID: %s\n", AP_SSID);
  Serial.printf("Connect and open: http://%s\n", ip.toString().c_str());
}

// =============================================================================
//  WEB SERVER SETUP (existing — unchanged)
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
//  SAVE PHOTO (existing — unchanged)
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
//  WAV HEADER (existing — unchanged)
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
//  FILENAME INDEX GENERATOR (existing — unchanged)
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
//  VIBRATION MOTOR (existing — unchanged)
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
//  WEB SERVER — HTML PAGE (existing — resetActivityTimer removed)
// =============================================================================

void handleRoot() {
  lastActivityMs = millis();
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
//  WEB SERVER — LIST MEMORIES API (existing — resetActivityTimer removed)
// =============================================================================

void handleListMemories() {
  lastActivityMs = millis();
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
//  WEB SERVER — SERVE FILE (existing — resetActivityTimer removed)
// =============================================================================

void handleFile() {
  lastActivityMs = millis();
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

  String contentType = "application/octet-stream";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    contentType = "image/jpeg";
  } else if (path.endsWith(".wav")) {
    contentType = "audio/wav";
  } else if (path.endsWith(".png")) {
    contentType = "image/png";
  }

  server.streamFile(file, contentType);
  file.close();
}

// =============================================================================
//  WEB SERVER — DELETE MEMORY (existing — resetActivityTimer removed)
// =============================================================================

void handleDelete() {
  lastActivityMs = millis();
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing 'name' parameter");
    return;
  }

  String name = server.arg("name");
  String folderPath = "/" + name;

  if (!name.startsWith("memory_")) {
    server.send(403, "text/plain", "Can only delete memory folders");
    return;
  }

  if (!SD.exists(folderPath)) {
    server.send(404, "text/plain", "Folder not found");
    return;
  }

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

  SD.rmdir(folderPath);

  Serial.printf("[Web] Deleted: %s\n", folderPath.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}
