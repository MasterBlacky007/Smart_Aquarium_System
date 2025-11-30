#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h> // for fabs

// ---------- WIFI ----------
#define WIFI_SSID "nishee"
#define WIFI_PASSWORD "7654321e"

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
#define HEATER_RELAY 21
#define COOLING_RELAY 25

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
float scale_factor = -23500.0; 
float weight_correction = 1.00;
float filteredWeight = 0.0;
const float target_weight_g = 100.0;

// ---------- DOSAGE SYSTEM (DAILY) ----------
#define DOSAGE_RELAY 22
int dosage_time_ms = 5000;

// ---------- WEEKLY DOSAGE SYSTEM ----------
#define WEEKLY_DOSAGE_RELAY 23
float weekly_flowRate_mL_per_sec = 10.0;
bool weeklyDosageRunning = false;
unsigned long weeklyStartMillis = 0;
unsigned long weeklyDurationMillis = 0;

// ---------- IR Sensor ----------
#define IR_FOOD_PIN 36 
bool foodAvailable = false;

// ---------- Ultrasonic Sensor ----------
#define ULTRA_TRIG 15
#define ULTRA_ECHO 2
const float EXPECTED_WATER_DIST_CM = 5.0;
float waterDistance = 0.0;

// ---------- Dosage Level Sensors ----------
#define DOSAGE1_PIN 32
#define DOSAGE2_PIN 33
int doseLevel1 = 0;
int doseLevel2 = 0;

// ---------- Timing ----------
unsigned long sensorInterval = 2000;
unsigned long fbPollInterval = 600;
unsigned long lastSensorMillis = 0;
unsigned long lastFbPollMillis = 0;

// ---------- Door States ----------
bool mainDoorOpen = false;
bool lowerDoorsOpen = false;

// -------------------- WiFi --------------------
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
  if(foodAvailable){ 
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
  servoLower2.write(180 - angle);  
  lowerDoorsOpen = true; 
  setFeedStatus("Lower Opened"); 
}

void closeLowerDoors() { 
  const int angle = 0;
  servoLower1.write(angle); 
  servoLower2.write(180 - angle);   
  lowerDoorsOpen = false; 
  setFeedStatus("Lower Closed"); 
}

// -------------------- Weight Read --------------------
float read_weight() {
  if (!scale.is_ready()) return 0.0;
  float w = scale.get_units(20);   
  if (fabs(w) < 0.05) w = 0.0;
  w = w * weight_correction;
  float alpha = 0.1;
  filteredWeight = (alpha * w) + (1 - alpha) * filteredWeight;
  return filteredWeight;
}

// -------------------- DOSAGE FUNCTION (DAILY) --------------------
void startDosage() {
  Serial.println("Starting daily dosage...");
  Firebase.RTDB.setString(&fbdo, "/dose/status", "Dispensing...");
  digitalWrite(DOSAGE_RELAY, LOW);  
  delay(dosage_time_ms);
  digitalWrite(DOSAGE_RELAY, HIGH);  
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

    digitalWrite(WEEKLY_DOSAGE_RELAY, LOW); 
    Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Dispensing...");
    weeklyDosageRunning = true;
    Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false); 
    Serial.printf("Weekly dosage started: %.2f mL\n", mlAmount);
  }

  if(weeklyDosageRunning){
    if(millis() - weeklyStartMillis >= weeklyDurationMillis){
      digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH); 
      weeklyDosageRunning = false;
      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Done. Poured");
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/hasPoured", true);
    }
  }
}

// -------------------- Ultrasonic Sensor --------------------
float readUltrasonic() {
  digitalWrite(ULTRA_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRA_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRA_TRIG, LOW);

  long duration = pulseIn(ULTRA_ECHO, HIGH, 30000); 
  if(duration == 0) return -1; 

  float distance_cm = (duration / 2.0) * 0.0343;
  return distance_cm;
}

// -------------------- Firebase button handler --------------------
void pollFirebaseButtons() {
  bool b;
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_open_main", &b) && b){ openMainDoor(); Firebase.RTDB.setBool(&fbdo, "/feed/button_open_main", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_close_main", &b) && b){ closeMainDoor(); Firebase.RTDB.setBool(&fbdo, "/feed/button_close_main", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_open_lower", &b) && b){ openLowerDoors(); Firebase.RTDB.setBool(&fbdo, "/feed/button_open_lower", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/feed/button_close_lower", &b) && b){ closeLowerDoors(); Firebase.RTDB.setBool(&fbdo, "/feed/button_close_lower", false); }
  if (Firebase.RTDB.getBool(&fbdo, "/dose/start", &b) && b){ startDosage(); Firebase.RTDB.setBool(&fbdo, "/dose/start", false); }
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_SAFE, OUTPUT);
  pinMode(LED_HARM, OUTPUT);
  pinMode(HEATER_RELAY, OUTPUT);
  pinMode(COOLING_RELAY, OUTPUT);
  pinMode(DOSAGE_RELAY, OUTPUT); digitalWrite(DOSAGE_RELAY, HIGH);
  pinMode(WEEKLY_DOSAGE_RELAY, OUTPUT); digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH);
  pinMode(IR_FOOD_PIN, INPUT);
  pinMode(ULTRA_TRIG, OUTPUT);
  pinMode(ULTRA_ECHO, INPUT);
  
  pinMode(DOSAGE1_PIN, INPUT);
  pinMode(DOSAGE2_PIN, INPUT);

  sensors.begin();
  servoMain.attach(SERVO_MAIN_PIN);
  servoLower1.attach(SERVO_LOWER1_PIN);
  servoLower2.attach(SERVO_LOWER2_PIN);
  closeMainDoor();
  closeLowerDoors();

  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(scale_factor);
  delay(500);
  scale.tare(20);
  
  wifiConnect();

  config.api_key = "AIzaSyASBX1WRqOyJea1fm4LXGIV1PHT_7GuCVo";
  auth.user.email = "shasaninisansala2003@gmail.com";
  auth.user.password = "sh12345";
  config.database_url = "https://exercise2-55d7c-default-rtdb.asia-southeast1.firebasedatabase.app/";
  Firebase.begin(&config, &auth);

  Firebase.RTDB.setString(&fbdo, "/feed/status", "Idle");
  Firebase.RTDB.setString(&fbdo, "/dose/status", "Idle");
  Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Time to Pour");
  Firebase.RTDB.setBool(&fbdo, "/weeklyDose/hasPoured", false);

  if (scale.is_ready()) {
    float init = scale.get_units(10) * weight_correction;
    filteredWeight = (fabs(init) < 0.05) ? 0.0 : init;
  }
}

// -------------------- Loop --------------------
void loop() {
  unsigned long now = millis();

  // IR Sensor
  foodAvailable = digitalRead(IR_FOOD_PIN) == LOW; 
  Firebase.RTDB.setString(&fbdo, "/feed/foodStatus", foodAvailable?"Food Available":"No Food");

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

  // Ultrasonic water level
  float ultraDistance = readUltrasonic();
  if(ultraDistance > 0){
      waterDistance = ultraDistance;
      Firebase.RTDB.setFloat(&fbdo, "/sensor/waterLevel", waterDistance);
      if(waterDistance > EXPECTED_WATER_DIST_CM + 0.5){
          Firebase.RTDB.setString(&fbdo, "/sensor/waterAlert", "Water Low!");
      } else {
          Firebase.RTDB.setString(&fbdo, "/sensor/waterAlert", "Water OK");
      }
  }

  // Dosage tank levels
  doseLevel1 = digitalRead(DOSAGE1_PIN) == HIGH ? 100 : 0; // HIGH = Full
  doseLevel2 = digitalRead(DOSAGE2_PIN) == HIGH ? 100 : 0;
  Firebase.RTDB.setFloat(&fbdo, "/dose/tank1", doseLevel1);
  Firebase.RTDB.setFloat(&fbdo, "/dose/tank2", doseLevel2);

  delay(10);
}
