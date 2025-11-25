#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ---------- WIFI ----------
#define WIFI_SSID "nigeeth"
#define WIFI_PASSWORD "aaaaaaa1"

// ---------- Firebase ----------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// (other defines same as yours)
#define PH_PIN 34
#define MQ135_PIN 35
#define LED_SAFE 18
#define LED_HARM 19
#define ONE_WIRE_BUS 4
#define HEATER_RELAY 25
#define COOLING_RELAY 21
#define SERVO_MAIN_PIN 13
#define SERVO_LOWER1_PIN 12
#define SERVO_LOWER2_PIN 14
#define LOADCELL_DOUT 26
#define LOADCELL_SCK 27
#define DOSAGE_RELAY 22
#define WEEKLY_DOSAGE_RELAY 23
#define IR_FOOD_PIN 36

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

Servo servoMain, servoLower1, servoLower2;
HX711 scale;

float scale_factor = -23500.0;
float weight_correction = 1.00;
float filteredWeight = 0.0;
const float target_weight_g = 100.0;

unsigned long sensorInterval = 2000;
unsigned long fbPollInterval = 600;
unsigned long lastSensorMillis = 0;
unsigned long lastFbPollMillis = 0;

bool mainDoorOpen = false;
bool lowerDoorsOpen = false;
bool weeklyDosageRunning = false;
unsigned long weeklyStartMillis = 0;
unsigned long weeklyDurationMillis = 0;
float weekly_flowRate_mL_per_sec = 10.0;
unsigned long weeklyStartRequest = 0;

// -------------------- Helper: debug-print Firebase result --------------------
void fbPrintResult(const char* action, bool ok) {
  if (ok) {
    Serial.printf("Firebase %s -> OK\n", action);
  } else {
    Serial.printf("Firebase %s -> FAILED: %s (HTTP: %d)\n", action, fbdo.errorReason().c_str(), fbdo.httpCode());
  }
}

// -------------------- WiFi Connect (improved) --------------------
void wifiConnect() {
  Serial.printf("WiFi: Connecting to SSID '%s'\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  const unsigned long timeout = 20000; // 20s
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(400);
    if (millis() - start > timeout) {
      Serial.println();
      Serial.println("WiFi connect timeout. Will retry in 5s...");
      delay(5000);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }
  Serial.println();
  Serial.print("WiFi Connected! IP: ");
  Serial.println(WiFi.localIP());
}

// -------------------- Safe wrappers for RTDB writes --------------------
bool fbSetString(const char* path, const char* val) {
  bool ok = Firebase.RTDB.setString(&fbdo, path, val);
  if(!ok) Serial.printf("setString('%s') failed -> %s\n", path, fbdo.errorReason().c_str());
  return ok;
}
bool fbSetFloat(const char* path, float v) {
  bool ok = Firebase.RTDB.setFloat(&fbdo, path, v);
  if(!ok) Serial.printf("setFloat('%s') failed -> %s\n", path, fbdo.errorReason().c_str());
  return ok;
}
bool fbSetBool(const char* path, bool b) {
  bool ok = Firebase.RTDB.setBool(&fbdo, path, b);
  if(!ok) Serial.printf("setBool('%s') failed -> %s\n", path, fbdo.errorReason().c_str());
  return ok;
}

// -------------------- Feeding status --------------------
void setFeedStatus(const char *txt) {
  fbSetString("/feed/status", txt);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_SAFE, OUTPUT);
  pinMode(LED_HARM, OUTPUT);
  pinMode(HEATER_RELAY, OUTPUT);
  pinMode(COOLING_RELAY, OUTPUT);
  pinMode(DOSAGE_RELAY, OUTPUT);
  digitalWrite(DOSAGE_RELAY, HIGH);
  pinMode(WEEKLY_DOSAGE_RELAY, OUTPUT);
  digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH);
  pinMode(IR_FOOD_PIN, INPUT);

  sensors.begin();
  servoMain.attach(SERVO_MAIN_PIN);
  servoLower1.attach(SERVO_LOWER1_PIN);
  servoLower2.attach(SERVO_LOWER2_PIN);

  // Servos to closed
  servoMain.write(0);
  servoLower1.write(0);
  servoLower2.write(180);

  // HX711
  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(scale_factor);
  delay(500);
  scale.tare(20);
  if (!scale.is_ready()) {
    Serial.println("HX711 NOT ready!");
  } else {
    Serial.println("HX711 OK");
  }

  wifiConnect();

  // ---------- Firebase config ----------
  config.api_key = "AIzaSyANvMOIRcRdOjOIJ-GglO-iGyeel64ZG_U";
  auth.user.email = "shasaninisansala2003@gmail.com";
  auth.user.password = "sn123456";
  config.database_url = "https://smart-aquarium-system-3a19d-default-rtdb.asia-southeast1.firebasedatabase.app";

  Serial.println("Initializing Firebase...");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Quick test writes to confirm auth & DB connectivity
  bool ok = Firebase.RTDB.setString(&fbdo, "/__sys/test", "hello_from_esp32");
  fbPrintResult("initial write /__sys/test", ok);

  // Initialize a few paths
  fbSetString("/feed/status", "Idle");
  fbSetString("/dose/status", "Idle");
  fbSetString("/weeklyDose/status", "Time to Pour");
  fbSetBool("/weeklyDose/hasPoured", false);

  // Init filteredWeight
  if (scale.is_ready()) {
    float init = scale.get_units(10) * weight_correction;
    filteredWeight = (fabs(init) < 0.05) ? 0.0 : init;
  }
  Serial.println("Setup complete.");
}

// -------------------- read weight --------------------
float read_weight() {
  if (!scale.is_ready()) return 0.0;
  float w = scale.get_units(20);
  if (fabs(w) < 0.05) w = 0.0;
  w = w * weight_correction;
  float alpha = 0.1;
  filteredWeight = (alpha * w) + (1 - alpha) * filteredWeight;
  return filteredWeight;
}

// (other functions: open/close doors etc. reuse your code but call setFeedStatus which now logs)

// -------------------- pollFirebaseButtons (improved) --------------------
void pollFirebaseButtons() {
  bool b;
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_open_main", &b) && b) {
    // your action...
    Firebase.RTDB.setBool(&fbdo, "/feed/button_open_main", false);
    Serial.println("Button open_main pressed (from Firebase).");
  }
  // ... repeat for others, plus print fbdo.errorReason() if getBool() fails
  if (!fbdo.httpCode() == 200) {
    // optional: print if needed
  }
}

// -------------------- loop --------------------
void loop() {
  unsigned long now = millis();

  // Reconnect wifi if needed
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi LOST - reconnecting...");
    wifiConnect();
  }

  // update sensors at intervals
  // food status
  bool foodAvailable = digitalRead(IR_FOOD_PIN) == LOW;
  fbSetString("/feed/foodStatus", foodAvailable ? "Food Available" : "No Food");

  // temperature
  sensors.requestTemperatures();
  float waterTemp = sensors.getTempCByIndex(0);
  fbSetFloat("/sensor/temperature", waterTemp);

  // control relays and log
  bool heaterOn = (waterTemp < 29.0);
  bool coolerOn = (waterTemp > 30.0);
  digitalWrite(HEATER_RELAY, heaterOn);
  digitalWrite(COOLING_RELAY, coolerOn);
  fbSetBool("/control/heater", heaterOn);
  fbSetBool("/control/cooler", coolerOn);

  // PH + MQ135 (only run every sensorInterval)
  if (now - lastSensorMillis >= sensorInterval) {
    lastSensorMillis = now;
    int ph_raw = analogRead(PH_PIN);
    float voltage = ph_raw * (3.3 / 4095.0);
    float ph_value = 7.0 - ((voltage - 2.5) / 0.18);
    int mq_raw = analogRead(MQ135_PIN);
    float ammonia = (mq_raw * 100.0) / 4095.0;
    digitalWrite(LED_SAFE, (ph_value >= 6.5 && ph_value <= 8.5));
    digitalWrite(LED_HARM, !(ph_value >= 6.5 && ph_value <= 8.5));
    fbSetFloat("/sensor/ph", ph_value);
    fbSetFloat("/sensor/ammonia", ammonia);
  }

  // weight
  float liveWeight = read_weight();
  fbSetFloat("/feed/weight", liveWeight);
  if (mainDoorOpen && liveWeight >= target_weight_g) {
    // closeMainDoor(); // your function
    fbSetString("/feed/status", "Auto closed (target reached)");
    Serial.println("Auto-closed main door (target reached).");
  }

  // poll firebase commands periodically
  if (now - lastFbPollMillis >= fbPollInterval) {
    lastFbPollMillis = now;
    pollFirebaseButtons();
  }

  // weekly dosage handling (your existing logic)
  // ...

  delay(10);
}
