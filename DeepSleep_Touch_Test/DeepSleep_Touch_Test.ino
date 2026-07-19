/*
 * =============================================================================
 *  Deep Sleep + Touch Wakeup — Validation Test
 * =============================================================================
 *
 *  Board  : Seeed Studio XIAO ESP32S3 Sense
 *  IDE    : Arduino IDE 2.x+
 *  Purpose: Prove that event-driven deep sleep with touch wakeup works
 *           reliably on this specific board before refactoring the main project.
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
#define WAKEUP_TOUCH_PIN       1      // GPIO 1 (D0) — same as main project

// Touch threshold for deep sleep wakeup.
// On ESP32-S3, touch values INCREASE when touched.
// This threshold tells the RTC controller: "wake up if touch > threshold"
// Start with a low value; increase if the board wakes up by itself.
// Use the printed calibration values to fine-tune.
#define WAKEUP_TOUCH_THRESHOLD 22000

// Built-in LED on XIAO ESP32S3
// GPIO 21 — ACTIVE LOW (LOW = ON, HIGH = OFF)
// Note: GPIO 21 is also the SD card CS on the Sense expansion board,
// but for this test we're not using the SD card.
#define LED_PIN                21
#define LED_ON                 LOW   // Active-low LED
#define LED_OFF                HIGH

// How many additional touch pins to read for calibration display
// These are read-only for diagnostics, not configured as wakeup sources
#define NUM_TOUCH_PINS         6

// Additional touch pins to read (for diagnostics only)
const int touchPins[NUM_TOUCH_PINS] = {1, 2, 3, 4, 5, 6};
const char* touchLabels[NUM_TOUCH_PINS] = {
  "GPIO1 (D0)", "GPIO2 (D1)", "GPIO3 (D2)",
  "GPIO4 (D3)", "GPIO5 (D4)", "GPIO6 (D5)"
};

// =============================================================================
//  PERSISTENT COUNTER — Survives deep sleep (stored in RTC memory)
// =============================================================================

RTC_DATA_ATTR int bootCount = 0;

// =============================================================================
//  HELPER FUNCTIONS
// =============================================================================

/**
 * Returns a human-readable string for the wakeup cause.
 */
const char* getWakeupReasonString(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:    return "Not from deep sleep (power-on / reset)";
    case ESP_SLEEP_WAKEUP_EXT0:         return "External signal (RTC_IO) — ext0";
    case ESP_SLEEP_WAKEUP_EXT1:         return "External signal (RTC_CNTL) — ext1";
    case ESP_SLEEP_WAKEUP_TIMER:        return "Timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:     return "★ Touchpad ★";
    case ESP_SLEEP_WAKEUP_ULP:          return "ULP program";
    case ESP_SLEEP_WAKEUP_GPIO:         return "GPIO (light sleep only)";
    case ESP_SLEEP_WAKEUP_UART:         return "UART (light sleep only)";
    default:                            return "Unknown";
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
  digitalWrite(LED_PIN, LED_OFF);  // Start with LED off

  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LED_ON);
    delay(onMs);
    digitalWrite(LED_PIN, LED_OFF);
    delay(offMs);
  }
}

/**
 * Reads and prints current touch values for all configured pins.
 * This helps you calibrate the wakeup threshold.
 */
void printTouchCalibration() {
  Serial.println("  ┌──────────────────────────────────────────┐");
  Serial.println("  │         Current Touch Readings            │");
  Serial.println("  ├──────────────┬────────────┬──────────────┤");
  Serial.println("  │ Pin          │ Value      │ Status       │");
  Serial.println("  ├──────────────┼────────────┼──────────────┤");

  for (int i = 0; i < NUM_TOUCH_PINS; i++) {
    uint32_t val = touchRead(touchPins[i]);
    const char* status = (val > WAKEUP_TOUCH_THRESHOLD) ? "TOUCHED!" : "idle";
    Serial.printf("  │ %-12s │ %10lu │ %-12s │\n", touchLabels[i], val, status);
  }

  Serial.println("  └──────────────┴────────────┴──────────────┘");
  Serial.printf("  Wakeup threshold: %d\n", WAKEUP_TOUCH_THRESHOLD);
  Serial.println("  (Wake triggers when touch value > threshold)");
}

// =============================================================================
//  SETUP — All logic runs here, then device sleeps
// =============================================================================

void setup() {
  // ── 1. Initialize LED immediately (visual feedback even without Serial) ──
  blinkLED(1, 100, 0);  // Quick single blink = "I'm alive"

  // ── 2. Initialize Serial and wait for USB CDC reconnection ──
  Serial.begin(115200);
  waitForSerial(3000);  // Wait up to 3 seconds for USB

  // ── 3. Increment and display boot count ──
  bootCount++;

  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║  Deep Sleep + Touch Wakeup — Validation Test ║");
  Serial.println("║  Board: XIAO ESP32S3 Sense                   ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.printf("  Boot count: %d (survives deep sleep via RTC memory)\n", bootCount);
  Serial.println();

  // ── 4. Determine and print wakeup reason ──
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  Serial.printf("  Wakeup cause: %s\n", getWakeupReasonString(wakeupCause));

  if (wakeupCause == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    // Try to identify which touch pad triggered the wakeup
    touch_pad_t touchPin = esp_sleep_get_touchpad_wakeup_status();
    Serial.printf("  Touch pad #: %d\n", touchPin);
    Serial.println();

    // ── 5. BLINK LED 3 times to confirm touch wakeup ──
    Serial.println("  >>> Blinking LED 3 times (confirming touch wakeup)...");
    blinkLED(3, 300, 300);
    Serial.println("  >>> LED blink complete.");

    // ── 6. Wait 2 seconds (as required by spec) ──
    Serial.println("  >>> Waiting 2 seconds before returning to sleep...");
    delay(2000);

  } else {
    // First boot or non-touch wakeup
    Serial.println("  (This is a cold boot or non-touch wakeup)");
    Serial.println();
  }

  // ── 7. Print touch calibration data ──
  Serial.println();
  printTouchCalibration();
  Serial.println();

  // ── 8. Configure touch wakeup ──
  Serial.printf("  Configuring touch wakeup: GPIO %d, threshold %d\n",
                WAKEUP_TOUCH_PIN, WAKEUP_TOUCH_THRESHOLD);

  touchSleepWakeUpEnable(WAKEUP_TOUCH_PIN, WAKEUP_TOUCH_THRESHOLD);

  Serial.println();
  Serial.println("  ┌─────────────────────────────────────────┐");
  Serial.println("  │  💤 Entering DEEP SLEEP now...           │");
  Serial.println("  │                                          │");
  Serial.printf( "  │  Touch GPIO %d (D0) to wake up.          │\n", WAKEUP_TOUCH_PIN);
  Serial.println("  │                                          │");
  Serial.println("  │  The USB connection will disconnect.     │");
  Serial.println("  │  Keep the Serial Monitor open.           │");
  Serial.println("  │  LED will blink 3x on successful wakeup.│");
  Serial.println("  └─────────────────────────────────────────┘");
  Serial.println();

  // ── 9. Flush Serial before sleeping ──
  // Note: Serial.flush() can block on ESP32-S3 USB CDC if host isn't
  // listening. We use a small delay instead for safety.
  if (Serial) {
    Serial.flush();
  }
  delay(100);

  // ── 10. Enter deep sleep ──
  esp_deep_sleep_start();

  // ── This line should NEVER be reached ──
  Serial.println("[ERROR] Deep sleep failed!");
}

// =============================================================================
//  LOOP — Intentionally empty
// =============================================================================

/**
 * The device should never reach loop() because it enters deep sleep
 * at the end of setup(). If this function runs, something went wrong.
 */
void loop() {
  // If we somehow reach here, print an error and try to sleep again
  Serial.println("[ERROR] loop() reached — deep sleep did not engage!");
  delay(5000);
  esp_deep_sleep_start();
}
