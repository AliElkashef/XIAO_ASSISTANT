/*
 * =============================================================================
 *  Deep Sleep + Touch Wakeup — Validation Test
 * =============================================================================
 *
 *  Board  : Seeed Studio XIAO ESP32S3 Sense
 *  IDE    : Arduino IDE 2.x+
 *  Purpose: Prove that event-driven deep sleep with touch wakeup works
 *           reliably on this specific board before refactoring the main
 * project.
 *
 *  Behavior
 *  --------
 *  1. Boot → print diagnostics → blink LED (if woken by touch)
 *  2. Read current touch values for calibration reference
 *  3. Configure touch wakeup on GPIO 1 (D0)
 *  4. Enter deep sleep
 *  5. Wait for touch → wake → repeat from step 1
 *
 *  The loop() function is intentionally EMPTY.
 *  All logic runs inside setup() and then the device sleeps.
 *
 *  Board Settings
 *  --------------
 *  Board        : "XIAO_ESP32S3"
 *  PSRAM        : "OPI PSRAM"
 *  USB CDC On Boot : Enabled     ← IMPORTANT for Serial output
 *  Flash Mode   : QIO 80 MHz
 *  Upload Speed : 921600
 *
 *  IMPORTANT: USB CDC Behavior During Deep Sleep
 *  ----------------------------------------------
 *  When the ESP32-S3 enters deep sleep, the USB connection is PHYSICALLY
 *  disconnected. Your Serial Monitor will lose the COM port.
 *
 *  After waking up, the USB must re-enumerate with the PC. This takes
 *  ~1-3 seconds. The sketch waits for this reconnection before printing.
 *
 *  Tips:
 *  - Do NOT close the Serial Monitor when the device sleeps.
 *  - Some Serial Monitors auto-reconnect (PlatformIO, VS Code).
 *  - Arduino IDE Serial Monitor may require manual reconnection.
 *  - If you miss output, check if the LED blinked — that confirms wakeup.
 *
 * =============================================================================
 */

#include <esp_sleep.h>

// =============================================================================
//  CONFIGURATION — Adjust these for your setup
// =============================================================================

// Touch pin to use as wakeup source
// On XIAO ESP32S3: D0=GPIO1, D1=GPIO2, D2=GPIO3, D3=GPIO4, D4=GPIO5, D5=GPIO6
// All of GPIO 1-9 support capacitive touch on ESP32-S3.
#define WAKEUP_TOUCH_PIN 1 // GPIO 1 (D0) — same as main project

// ─── Dynamic Calibration Configuration ───────────────────────────────────────

// On ESP32-S3, touchSleepWakeUpEnable expects a DELTA value (change in
// capacitance). It triggers wakeup when: (Touched_Value - Baseline_Value) >
// Delta_Threshold We calculate this delta dynamically as: Delta_Threshold =
// Baseline * DELTA_RATIO.
//
// If the board doesn't wake up: DECREASE DELTA_RATIO (e.g. to 0.4 or 0.3) for
// more sensitivity. If the board wakes up by itself: INCREASE DELTA_RATIO (e.g.
// to 0.7 or 0.8) for less sensitivity.
#define DELTA_RATIO  0.1 // Wakeup threshold delta = 50% of baseline (more sensitive & reliable)
#define NOISE_MARGIN_RATIO 1.3 // Threshold below which we consider the sensor untouched
#define EMA_ALPHA 0.15 // Adaptation rate (higher since sleep boots are infrequent)
#define CALIBRATION_SAMPLES 50  // Samples for initial boot calibration
#define CALIBRATION_DELAY_MS 10 // Delay between calibration samples

// Built-in LED on XIAO ESP32S3
// GPIO 21 — ACTIVE LOW (LOW = ON, HIGH = OFF)
// Note: GPIO 21 is also the SD card CS on the Sense expansion board,
// but for this test we're not using the SD card.
#define LED_PIN 21
#define LED_ON LOW // Active-low LED
#define LED_OFF HIGH

// How many additional touch pins to read for calibration display
// These are read-only for diagnostics, not configured as wakeup sources
#define NUM_TOUCH_PINS 6

// Additional touch pins to read (for diagnostics only)
const int touchPins[NUM_TOUCH_PINS] = {1, 2, 3, 4, 5, 6};
const char *touchLabels[NUM_TOUCH_PINS] = {"GPIO1 (D0)", "GPIO2 (D1)",
                                           "GPIO3 (D2)", "GPIO4 (D3)",
                                           "GPIO5 (D4)", "GPIO6 (D5)"};

// =============================================================================
//  PERSISTENT COUNTERS & STATE — Survive deep sleep (RTC memory)
// =============================================================================

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR float rtcBaseline = 0.0; // Dynamic baseline stored in RTC memory

// =============================================================================
//  HELPER FUNCTIONS
// =============================================================================

/**
 * Returns a human-readable string for the wakeup cause.
 */
const char *getWakeupReasonString(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
  case ESP_SLEEP_WAKEUP_UNDEFINED:
    return "Not from deep sleep (power-on / reset)";
  case ESP_SLEEP_WAKEUP_EXT0:
    return "External signal (RTC_IO) — ext0";
  case ESP_SLEEP_WAKEUP_EXT1:
    return "External signal (RTC_CNTL) — ext1";
  case ESP_SLEEP_WAKEUP_TIMER:
    return "Timer";
  case ESP_SLEEP_WAKEUP_TOUCHPAD:
    return "★ Touchpad ★";
  case ESP_SLEEP_WAKEUP_ULP:
    return "ULP program";
  case ESP_SLEEP_WAKEUP_GPIO:
    return "GPIO (light sleep only)";
  case ESP_SLEEP_WAKEUP_UART:
    return "UART (light sleep only)";
  default:
    return "Unknown";
  }
}

/**
 * Waits for USB CDC Serial to reconnect after deep sleep wakeup.
 * Times out after maxWaitMs to avoid hanging if no USB is connected.
 */
void waitForSerial(unsigned long maxWaitMs) {
  unsigned long start = millis();
  while (!Serial && (millis() - start < maxWaitMs)) {
    delay(10);
  }
  // Extra delay for USB host to stabilize
  delay(500);
}

/**
 * Blinks the built-in LED a specified number of times.
 */
void blinkLED(int times, int onMs, int offMs) {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF); // Start with LED off

  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LED_ON);
    delay(onMs);
    digitalWrite(LED_PIN, LED_OFF);
    delay(offMs);
  }
}

/**
 * Reads and prints current touch values for all configured pins.
 */
void printTouchCalibration(float currentDelta) {
  Serial.println("  ┌──────────────────────────────────────────┐");
  Serial.println("  │         Current Touch Readings            │");
  Serial.println("  ├──────────────┬────────────┬──────────────┤");
  Serial.println("  │ Pin          │ Value      │ Status       │");
  Serial.println("  ├──────────────┼────────────┼──────────────┤");

  for (int i = 0; i < NUM_TOUCH_PINS; i++) {
    uint32_t val = touchRead(touchPins[i]);
    float pinBaseline = (touchPins[i] == WAKEUP_TOUCH_PIN)
                            ? rtcBaseline
                            : (float)val; // Fallback for other pins
    float pinDelta = (touchPins[i] == WAKEUP_TOUCH_PIN)
                         ? currentDelta
                         : (pinBaseline * DELTA_RATIO);
    bool touched = (val > (pinBaseline + pinDelta));
    const char *status = touched ? "TOUCHED!" : "idle";
    Serial.printf("  │ %-12s │ %10lu │ %-12s │\n", touchLabels[i], val, status);
  }

  Serial.println("  └──────────────┴────────────┴──────────────┘");
  Serial.printf("  RTC Baseline     : %.2f\n", rtcBaseline);
  Serial.printf("  Wake threshold   : %.2f (Baseline + Delta)\n",
                rtcBaseline + currentDelta);
  Serial.printf("  Wakeup Delta (D) : %.2f (Expected trigger change)\n",
                currentDelta);
}

// =============================================================================
//  SETUP — All logic runs here, then device sleeps
// =============================================================================

void setup() {
  // ── 1. Initialize LED immediately (visual feedback) ──
  blinkLED(1, 100, 0);

  // ── 2. Initialize Serial ──
  Serial.begin(115200);
  waitForSerial(3000);

  bootCount++;

  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║  Deep Sleep + Touch Wakeup — Validation Test ║");
  Serial.println("║         (With Dynamic Calibration)           ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.printf("  Boot count: %d\n", bootCount);
  Serial.println();

  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  Serial.printf("  Wakeup cause: %s\n", getWakeupReasonString(wakeupCause));

  // ── 3. Establish Baseline on First Boot ──
  if (rtcBaseline < 10.0) {
    Serial.println("  [Calibrating] First boot baseline calibration...");
    Serial.println("  Keep hands off the touch sensor!");

    uint64_t sum = 0;
    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
      sum += touchRead(WAKEUP_TOUCH_PIN);
      delay(CALIBRATION_DELAY_MS);
    }
    rtcBaseline = (float)sum / CALIBRATION_SAMPLES;
    Serial.printf("  [Calibrated] Initial baseline set to: %.2f\n",
                  rtcBaseline);
  }

  // Calculate current delta
  float currentDelta = rtcBaseline * DELTA_RATIO;

  if (wakeupCause == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    touch_pad_t touchPin = esp_sleep_get_touchpad_wakeup_status();
    Serial.printf("  Touch pad #: %d\n", touchPin);

    // ── 4. Wait for touch release to update baseline safely ──
    Serial.println("  >>> Waiting for touch release...");
    unsigned long releaseStart = millis();
    float releaseThreshold = rtcBaseline * NOISE_MARGIN_RATIO;
    while (touchRead(WAKEUP_TOUCH_PIN) > releaseThreshold &&
           (millis() - releaseStart < 4000)) {
      delay(50); // Timeout after 4s
    }
    delay(200); // Settle time

    // Blink LED 3 times to confirm wakeup
    blinkLED(3, 300, 300);
    delay(5000);

    // ── 5. Dynamic Baseline Tracking ──
    // Take 10 idle samples to update our drift baseline
    uint64_t sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += touchRead(WAKEUP_TOUCH_PIN);
      delay(10);
    }
    float idleAverage = (float)sum / 10.0;

    // Only update baseline if the values are in the untouched region
    if (idleAverage < rtcBaseline * NOISE_MARGIN_RATIO) {
      float oldBaseline = rtcBaseline;
      rtcBaseline =
          (rtcBaseline * (1.0 - EMA_ALPHA)) + (idleAverage * EMA_ALPHA);
      currentDelta = rtcBaseline * DELTA_RATIO;
      Serial.printf(
          "  [EMA Update] Baseline drifted: %.2f -> %.2f | New Delta: %.2f\n",
          oldBaseline, rtcBaseline, currentDelta);
    } else {
      Serial.println(
          "  [EMA Skip] Sensor still active; skipping baseline update.");
    }

    // Wait remaining time of the 2-second pause
    delay(1000);
  }

  // ── 6. Print diagnostics ──
  Serial.println();
  printTouchCalibration(currentDelta);
  Serial.println();

  // ── 7. Configure wakeup and sleep ──
  Serial.printf("  Configuring touch wakeup: GPIO %d, delta threshold %.0f\n",
                WAKEUP_TOUCH_PIN, currentDelta);

  touchSleepWakeUpEnable(WAKEUP_TOUCH_PIN, (uint32_t)currentDelta);

  Serial.println();
  Serial.println("  ┌─────────────────────────────────────────┐");
  Serial.println("  │  💤 Entering DEEP SLEEP now...           │");
  Serial.println("  │  Touch GPIO 1 (D0) to wake up.          │");
  Serial.println("  └─────────────────────────────────────────┘");
  Serial.println();

  if (Serial) {
    Serial.flush();
  }
  delay(100);

  esp_deep_sleep_start();
  Serial.println("[ERROR] Deep sleep failed!");
}

void loop() {
  Serial.println("[ERROR] loop() reached!");
  delay(5000);
  esp_deep_sleep_start();
}
