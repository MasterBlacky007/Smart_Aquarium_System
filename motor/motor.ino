#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h> // for fabs

// ---------- WIFI ----------
#define WIFI_SSID "sethika"
#define WIFI_PASSWORD "sethika023"

// ---------- Firebase ----------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------- Sensors ----------
#define PH_PIN 34
#define MQ135_PIN 35
#define LED_SAFE 18
#define LED_HARM 19

// ---------- Temperature + Relays ----------
#define ONE_WIRE_BUS 4
#define HEATER_RELAY 25
#define COOLING_RELAY 21

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float waterTemp = 0.0;
const float MIN_TEMP = 29.0;
const float MAX_TEMP = 30.0;

// ---------- Servos ----------
#define SERVO_MAIN_PIN 13
#define SERVO_LOWER1_PIN 12
#define SERVO_LOWER2_PIN 14

Servo servoMain;
Servo servoLower1;
Servo servoLower2;

// ---------- Load cell for feeding ONLY ----------
HX711 scale;
#define LOADCELL_DOUT 26
#define LOADCELL_SCK 27
float scale_factor = -23500.0; // keep your existing factor, fine-tune with weight_correction if needed

// Fine-tuning multiplier — set to 1.00 initially, change after calibration
float weight_correction = 1.00;

// Filtered weight variable (DECLARED)
float filteredWeight = 0.0;

const float target_weight_g = 100.0;
unsigned long sensorInterval = 2000;
unsigned long fbPollInterval = 600;

bool mainDoorOpen = false;
bool lowerDoorsOpen = false;
unsigned long lastSensorMillis = 0;
unsigned long lastFbPollMillis = 0;

// ---------- DOSAGE SYSTEM (DAILY) ----------
#define DOSAGE_RELAY 22          // Relay pump control
int dosage_time_ms = 5000;       // PUMP RUN TIME (5 seconds)
 
// ---------- WEEKLY DOSAGE SYSTEM ----------
#define WEEKLY_DOSAGE_RELAY 23
float weekly_flowRate_mL_per_sec = 10.0;
bool weeklyDosageRunning = false;
unsigned long weeklyStartMillis = 0;
unsigned long weeklyDurationMillis = 0;

// ---------- IR Sensor for main food chamber ----------
#define IR_FOOD_PIN 36 // Change if needed
bool foodAvailable = false;

// -------------------- WiFi Connect --------------------
void wifiConnect() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(250);
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// -------------------- Feeding status --------------------
void setFeedStatus(const char *txt) {
  Firebase.RTDB.setString(&fbdo, "/feed/status", txt);
}

// -------------------- Servo Functions --------------------
void openMainDoor() {
  if(foodAvailable){ // Only open if food is detected
    servoMain.write(90);
    mainDoorOpen = true;
    setFeedStatus("Main Opened - Food Dispensed");
  } else {
    setFeedStatus("Cannot open - No Food!");
  }
}

void closeMainDoor() { 
  servoMain.write(0); 
  mainDoorOpen = false; 
  setFeedStatus("Main Closed"); 
}

void openLowerDoors() { 
  const int angle = 90;
  servoLower1.write(angle); 
  servoLower2.write(180 - angle);  // Inverted to match physical direction
  lowerDoorsOpen = true; 
  setFeedStatus("Lower Opened"); 
}

void closeLowerDoors() { 
  const int angle = 0;
  servoLower1.write(angle); 
  servoLower2.write(180 - angle);   // Inverted to match physical direction
  lowerDoorsOpen = false; 
  setFeedStatus("Lower Closed"); 
}

// -------------------- Weight Read --------------------
float read_weight() {
  if (!scale.is_ready()) return 0.0;

  // Take multiple units for better stability
  float w = scale.get_units(20);   // increased samples for smoothing

  // Noise floor: treat tiny jitter as zero
  if (fabs(w) < 0.05) w = 0.0;

  // Apply fine-tuning multiplier (calibration)
  w = w * weight_correction;

  // Smooth filter to avoid jumps (simple exponential)
  float alpha = 0.1;
  filteredWeight = (alpha * w) + (1 - alpha) * filteredWeight;

  return filteredWeight;
}

// -------------------- DOSAGE FUNCTION (DAILY) --------------------
void startDosage() {
  Serial.println("Starting daily dosage...");
  Firebase.RTDB.setString(&fbdo, "/dose/status", "Dispensing...");
  digitalWrite(DOSAGE_RELAY, LOW);  // Pump ON (active LOW)
  delay(dosage_time_ms);
  digitalWrite(DOSAGE_RELAY, HIGH);   // Pump OFF
  Firebase.RTDB.setString(&fbdo, "/dose/status", "Done");
  Serial.println("Daily dosage complete!");
}

// -------------------- WEEKLY DOSAGE FUNCTION --------------------
void handleWeeklyDosage() {
  bool startWeekly = false;
  Firebase.RTDB.getBool(&fbdo, "/weeklyDose/start", &startWeekly);

  bool alreadyPoured = false;
  Firebase.RTDB.getBool(&fbdo, "/weeklyDose/hasPoured", &alreadyPoured);

  if(startWeekly){
    if(alreadyPoured){
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false);
      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Already Poured! Wait until next week");
      return;
    }

    float mlAmount = 0;
    Firebase.RTDB.getFloat(&fbdo, "/weeklyDose/ml", &mlAmount);

    if(mlAmount <= 0){
      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Enter valid mL");
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false);
      return;
    }

    weeklyDurationMillis = (unsigned long)((mlAmount / weekly_flowRate_mL_per_sec) * 1000);
    weeklyStartMillis = millis();

    digitalWrite(WEEKLY_DOSAGE_RELAY, LOW); // Pump ON (active LOW)
    Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Dispensing...");
    weeklyDosageRunning = true;
    Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false); // reset trigger
    Serial.printf("Weekly dosage started: %.2f mL\n", mlAmount);
  }

  if(weeklyDosageRunning){
    if(millis() - weeklyStartMillis >= weeklyDurationMillis){
      digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH); // Pump OFF
      weeklyDosageRunning = false;
      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Done. Poured");
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/hasPoured", true);
    }
  }
}

// -------------------- Firebase button handler --------------------
void pollFirebaseButtons() {
  bool b;

  // Feeding control
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_open_main", &b) && b){ openMainDoor(); Firebase.RTDB.setBool(&fbdo, "/feed/button_open_main", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_close_main", &b) && b){ closeMainDoor(); Firebase.RTDB.setBool(&fbdo, "/feed/button_close_main", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_open_lower", &b) && b){ openLowerDoors(); Firebase.RTDB.setBool(&fbdo, "/feed/button_open_lower", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_close_lower", &b) && b){ closeLowerDoors(); Firebase.RTDB.setBool(&fbdo, "/feed/button_close_lower", false); }

  // Daily Dosage
  if (Firebase.RTDB.getBool(&fbdo, "/dose/start", &b) && b){ startDosage(); Firebase.RTDB.setBool(&fbdo, "/dose/start", false); }
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_SAFE, OUTPUT);
  pinMode(LED_HARM, OUTPUT);
  pinMode(HEATER_RELAY, OUTPUT);
  pinMode(COOLING_RELAY, OUTPUT);

  pinMode(DOSAGE_RELAY, OUTPUT);
  digitalWrite(DOSAGE_RELAY, HIGH);

  pinMode(WEEKLY_DOSAGE_RELAY, OUTPUT);
  digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH);

  pinMode(IR_FOOD_PIN, INPUT); // IR sensor input

  sensors.begin();
  servoMain.attach(SERVO_MAIN_PIN);
  servoLower1.attach(SERVO_LOWER1_PIN);
  servoLower2.attach(SERVO_LOWER2_PIN);
  closeMainDoor();
  closeLowerDoors();

  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(scale_factor);
  delay(500);           // allow HX711 settle after set_scale
  scale.tare(20);       // more accurate tare using multiple samples

  wifiConnect();

  config.api_key = "AIzaSyANvMOIRcRdOjOIJ-GglO-iGyeel64ZG_U";
  auth.user.email = "shasaninisansala2003@gmail.com";
  auth.user.password = "sn123456";
  config.database_url = "https://smart-aquarium-system-3a19d-default-rtdb.asia-southeast1.firebasedatabase.app/";
  Firebase.begin(&config, &auth);

  Firebase.RTDB.setString(&fbdo, "/feed/status", "Idle");
  Firebase.RTDB.setString(&fbdo, "/dose/status", "Idle");
  Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Time to Pour");
  Firebase.RTDB.setBool(&fbdo, "/weeklyDose/hasPoured", false);

  // Initialize filteredWeight to current reading to avoid jump
  if (scale.is_ready()) {
    float init = scale.get_units(10) * weight_correction;
    filteredWeight = (fabs(init) < 0.05) ? 0.0 : init;
  }
}

// -------------------- Loop --------------------
void loop() {
  unsigned long now = millis();

  // Update IR sensor status
  foodAvailable = digitalRead(IR_FOOD_PIN) == LOW; // adjust if sensor is active LOW
  if(foodAvailable){
    Firebase.RTDB.setString(&fbdo, "/feed/foodStatus", "Food Available");
  } else {
    Firebase.RTDB.setString(&fbdo, "/feed/foodStatus", "No Food");
  }

  // Temperature
  sensors.requestTemperatures();
  waterTemp = sensors.getTempCByIndex(0);
  Firebase.RTDB.setFloat(&fbdo, "/sensor/temperature", waterTemp);

  bool heaterOn = false;
  bool coolerOn = false;
  if (waterTemp < MIN_TEMP){ heaterOn=true; coolerOn=false; }
  else if (waterTemp > MAX_TEMP){ heaterOn=false; coolerOn=true; }
  else { heaterOn=true; coolerOn=true; }
  digitalWrite(HEATER_RELAY, heaterOn);
  digitalWrite(COOLING_RELAY, coolerOn);
  Firebase.RTDB.setBool(&fbdo, "/control/heater", heaterOn);
  Firebase.RTDB.setBool(&fbdo, "/control/cooler", coolerOn);

  // PH + MQ135
  if (now - lastSensorMillis >= sensorInterval){
    lastSensorMillis = now;
    int ph_raw = analogRead(PH_PIN);
    float voltage = ph_raw * (3.3 / 4095.0);
    float ph_value = 7.0 - ((voltage - 2.5) / 0.18);
    int mq_raw = analogRead(MQ135_PIN);
    float ammonia = (mq_raw * 100.0) / 4095.0;

    digitalWrite(LED_SAFE, (ph_value >= 6.5 && ph_value <= 8.5));
    digitalWrite(LED_HARM, !(ph_value >= 6.5 && ph_value <= 8.5));

    Firebase.RTDB.setFloat(&fbdo, "/sensor/ph", ph_value);
    Firebase.RTDB.setFloat(&fbdo, "/sensor/ammonia", ammonia);
  }

  // Feeding weight
  float liveWeight = read_weight();
  Firebase.RTDB.setFloat(&fbdo, "/feed/weight", liveWeight);
  if (mainDoorOpen && liveWeight >= target_weight_g){
    closeMainDoor();
    Firebase.RTDB.setString(&fbdo, "/feed/status", "Auto closed (target reached)");
  }

  // Firebase button check
  if (now - lastFbPollMillis >= fbPollInterval){
    lastFbPollMillis = now;
    pollFirebaseButtons();
  }

  // Weekly dosage
  handleWeeklyDosage();

  delay(10);
}
