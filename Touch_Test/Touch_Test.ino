/*
 * =============================================================================
 *  XIAO ESP32S3 Sense — Touch Calibration / Test Utility
 * =============================================================================
 * 
 *  This code reads the capacitive touch values on GPIO 1 (D0) and GPIO 2 (D1).
 *  Use this to find the baseline (untouched) and active (touched) values
 *  to set the perfect threshold in your main project.
 * 
 *  Instructions:
 *  1. Upload this sketch to your XIAO ESP32S3.
 *  2. Open the Serial Monitor (Ctrl+Shift+M or Cmd+Shift+M) at 115200 baud.
 *  3. (Optional) Open the Serial Plotter (Ctrl+Shift+L or Cmd+Shift+L) to see a graph.
 *  4. Observe the values when:
 *     - You are NOT touching the pins.
 *     - You ARE touching Pin 1.
 *     - You ARE touching Pin 2.
 * =============================================================================
 */

#define TOUCH_PIN_1  1   // GPIO 1  (D0)
#define TOUCH_PIN_2  2   // GPIO 2  (D1)

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10); // Wait for Serial to initialize
  }
  
  Serial.println("\n========================================");
  Serial.println("   XIAO ESP32S3 Touch Test & Calibration");
  Serial.println("========================================");
  Serial.println("Reading pins: GPIO 1 (D0) and GPIO 2 (D1)");
  Serial.println("Format: Pin1_Value , Pin2_Value");
  Serial.println("----------------------------------------\n");
}

void loop() {
  // Read the touch values (on ESP32-S3, values rise when touched)
  uint32_t val1 = touchRead(TOUCH_PIN_1);
  uint32_t val2 = touchRead(TOUCH_PIN_2);

  // Print in a format compatible with Serial Monitor and Serial Plotter
  Serial.print("Pin1 (D0): ");
  Serial.print(val1);
  Serial.print(" \t | \t Pin2 (D1): ");
  Serial.println(val2);

  // Wait 200ms before next reading to make it easy to read
  delay(200);
}
