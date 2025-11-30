// ---------- Water Level (Ultrasonic) ----------
#define TRIG_PIN 5
#define ECHO_PIN 15

float waterLevel = 0.0;
float tankHeightCM = 30.0;   // Tank height in cm

// Read water level function
float readWaterLevel() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH);

  // Distance from sensor → water (cm)
  float distance = duration * 0.034 / 2;

  // Convert to actual water level height
  float level = tankHeightCM - distance;

  if (level < 0) level = 0;
  if (level > tankHeightCM) level = tankHeightCM;

  return level;
}
