/*
 * =============================================================================
 *  XIAO ESP32S3 Sense — Touch-Triggered Audio Recorder & Camera
 * =============================================================================
 *
 *  Board  : Seeed Studio XIAO ESP32S3 Sense
 *  IDE    : Arduino IDE 2.x+
 *
 *  Features
 *  --------
 *  • Touch Button 1 (GPIO 1 / D0)  → Start/Stop audio recording → WAV on SD
 *  • Touch Button 2 (GPIO 2 / D1)  → Capture a JPEG photo       → JPG on SD
 *
 *  Libraries used (all built-in with the ESP32-S3 board package)
 *  ---------------------------------------------------------------
 *  - esp_camera.h    — camera driver
 *  - driver/i2s.h    — I2S / PDM microphone driver
 *  - FS.h / SD.h     — file system & SD card (SPI mode)
 *  - SPI.h           — SPI bus for SD card
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

// ─── Pin Definitions ─────────────────────────────────────────────────────────

// Touch buttons (ESP32-S3 capacitive touch)
#define TOUCH_BUTTON_RECORD   1   // GPIO 1  (D0) — record audio
#define TOUCH_BUTTON_PHOTO    2   // GPIO 2  (D1) — capture photo

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

// ─── Touch Sensor Parameters ─────────────────────────────────────────────────

// On ESP32-S3, touchRead() values INCREASE when touched.
// Adjust this threshold after checking Serial output during first boot.
#define TOUCH_THRESHOLD       40000
#define DEBOUNCE_MS           300       // Milliseconds between re-reads

// ─── Global State ────────────────────────────────────────────────────────────

static int audioFileIndex  = 0;      // Next index for audio_NNN.wav
static int imageFileIndex  = 0;      // Next index for image_NNN.jpg
static bool sdReady        = false;
static bool cameraReady    = false;
static bool micReady       = false;
static bool isRecording    = false;  // True while audio recording is active

// Recording state (used while isRecording == true)
static File    recordFile;           // Open WAV file handle
static uint32_t totalBytesWritten = 0;

// ─── Forward Declarations ────────────────────────────────────────────────────

bool   initSDCard();
bool   initCamera();
bool   initMicrophone();
void   startRecording();
void   stopRecording();
void   recordAudioChunk();
void   captureImage();
String generateNextFilename(const char* prefix, const char* extension, int &index);
void   writeWavHeader(File &file, uint32_t dataSize);
void   saveJpeg(const char* path);
bool   isTouched(int pin);

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);  // Give Serial time to connect
  Serial.println();
  Serial.println("========================================");
  Serial.println("  XIAO ESP32S3 Sense — Touch Recorder");
  Serial.println("========================================");

  // Initialise peripherals — each can fail independently
  sdReady     = initSDCard();
  cameraReady = initCamera();
  micReady    = initMicrophone();

  Serial.println();
  Serial.println("--- Status Summary ---");
  Serial.printf("  SD Card   : %s\n", sdReady     ? "OK" : "FAIL");
  Serial.printf("  Camera    : %s\n", cameraReady ? "OK" : "FAIL");
  Serial.printf("  Microphone: %s\n", micReady    ? "OK" : "FAIL");
  Serial.println("----------------------");
  Serial.println("Touch GPIO1 → Start / Stop audio recording");
  Serial.println("Touch GPIO2 → Capture a photo");
  Serial.println();
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
  // ── While recording: stream audio data and check for stop touch ──
  if (isRecording) {
    // Keep writing audio chunks to the SD card
    recordAudioChunk();

    // Check if user wants to stop
    if (isTouched(TOUCH_BUTTON_RECORD)) {
      stopRecording();
    }
    return;  // Don't process other buttons while recording
  }

  // ── Touch Button 1 — Start Audio Recording ──
  if (isTouched(TOUCH_BUTTON_RECORD)) {
    Serial.println("[Touch] Button 1 detected — Start Recording");
    if (!sdReady) {
      Serial.println("[Error] SD card not available — cannot record.");
    } else if (!micReady) {
      Serial.println("[Error] Microphone not available — cannot record.");
    } else {
      startRecording();
    }
  }

  // ── Touch Button 2 — Photo Capture ──
  if (isTouched(TOUCH_BUTTON_PHOTO)) {
    Serial.println("[Touch] Button 2 detected — Photo");
    if (!sdReady) {
      Serial.println("[Error] SD card not available — cannot save image.");
    } else if (!cameraReady) {
      Serial.println("[Error] Camera not available — cannot capture.");
    } else {
      captureImage();
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
//  AUDIO RECORDING  (Start / Stop toggle)
// =============================================================================

/**
 * Opens a new WAV file, writes a placeholder header, and sets the recording
 * flag. From this point, loop() calls recordAudioChunk() every iteration.
 */
void startRecording() {
  String filename = generateNextFilename("/audio_", ".wav", audioFileIndex);
  Serial.printf("Recording started → %s\n", filename.c_str());
  Serial.println("Touch Button 1 again to stop.");

  recordFile = SD.open(filename.c_str(), FILE_WRITE);
  if (!recordFile) {
    Serial.println("[Error] Could not create WAV file.");
    return;
  }

  // Write a placeholder WAV header (44 bytes) — patched on stop
  writeWavHeader(recordFile, 0);

  totalBytesWritten = 0;
  isRecording = true;
}

/**
 * Reads one chunk of audio data from the I2S microphone and writes it
 * to the open WAV file. Called repeatedly from loop() while recording.
 */
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

/**
 * Stops recording: patches the WAV header with the actual data size,
 * closes the file, and prints a summary.
 */
void stopRecording() {
  isRecording = false;

  // Patch the WAV header with the real data size
  recordFile.seek(0);
  writeWavHeader(recordFile, totalBytesWritten);
  recordFile.close();

  // Calculate duration for the user
  float durationSec = (float)totalBytesWritten
                      / (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));

  Serial.println("Recording stopped.");
  Serial.printf("Duration: %.1f seconds  |  Size: %u bytes\n\n",
                durationSec, totalBytesWritten + 44);
}

// =============================================================================
//  IMAGE CAPTURE
// =============================================================================

/**
 * Captures a single JPEG frame from the camera and saves it to the SD card.
 */
void captureImage() {
  String filename = generateNextFilename("/image_", ".jpg", imageFileIndex);
  Serial.printf("Capturing image → %s\n", filename.c_str());

  saveJpeg(filename.c_str());
}

// =============================================================================
//  WAV HEADER
// =============================================================================

/**
 * Writes a standard 44-byte RIFF/WAV header for 16-bit mono PCM.
 * Call once before writing data, then seek(0) and call again to patch sizes.
 */
void writeWavHeader(File &file, uint32_t dataSize) {
  uint32_t byteRate   = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);

  // RIFF chunk
  file.write((const uint8_t*)"RIFF", 4);
  uint32_t chunkSize = 36 + dataSize;
  file.write((const uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);

  // fmt sub-chunk
  file.write((const uint8_t*)"fmt ", 4);
  uint32_t subchunk1Size = 16;                  // PCM
  file.write((const uint8_t*)&subchunk1Size, 4);
  uint16_t audioFormat = 1;                     // PCM = 1
  file.write((const uint8_t*)&audioFormat, 2);
  uint16_t numChannels = CHANNELS;
  file.write((const uint8_t*)&numChannels, 2);
  uint32_t sampleRate = SAMPLE_RATE;
  file.write((const uint8_t*)&sampleRate, 4);
  file.write((const uint8_t*)&byteRate, 4);
  file.write((const uint8_t*)&blockAlign, 2);
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  file.write((const uint8_t*)&bitsPerSample, 2);

  // data sub-chunk
  file.write((const uint8_t*)"data", 4);
  file.write((const uint8_t*)&dataSize, 4);
}

// =============================================================================
//  SAVE JPEG
// =============================================================================

/**
 * Grabs the latest JPEG frame from the camera frame buffer and writes it
 * to the SD card. Releases the frame buffer when done.
 */
void saveJpeg(const char* path) {
  // Grab a frame
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Error] Camera capture failed.");
    return;
  }

  // Open file and write
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("[Error] Could not create image file.");
    esp_camera_fb_return(fb);
    return;
  }

  file.write(fb->buf, fb->len);
  file.close();

  Serial.printf("Image saved: %s (%u bytes)\n\n", path, fb->len);

  // Release the frame buffer back to the driver
  esp_camera_fb_return(fb);
}

// =============================================================================
//  FILENAME GENERATOR
// =============================================================================

/**
 * Generates sequential filenames like /audio_001.wav, /audio_002.wav, ...
 * Skips any index that already exists on the SD card so files are never
 * overwritten. Updates `index` in-place for the next call.
 */
String generateNextFilename(const char* prefix, const char* extension, int &index) {
  String filename;
  do {
    index++;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%03d%s", prefix, index, extension);
    filename = String(buf);
  } while (SD.exists(filename));

  return filename;
}
