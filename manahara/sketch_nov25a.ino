#include <WiFi.h>
#include <Firebase_ESP_Client.h>

// ---------- WIFI ----------
#define WIFI_SSID "nigeeth"
#define WIFI_PASSWORD "aaaaaaa1"

// ---------- Firebase ----------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------- Sensors ----------
#define PH_PIN 34
#define MQ135_PIN 35
#define LED_SAFE 18
#define LED_HARM 19

unsigned long lastSensorMillis = 0;
unsigned long sensorInterval = 2000;  // read every 2 seconds

// -------------------- WiFi Connect --------------------
void wifiConnect() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(250);
  }
  Serial.println("\nWiFi Connected!");
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_SAFE, OUTPUT);
  pinMode(LED_HARM, OUTPUT);

  wifiConnect();

  config.api_key = "AIzaSyASBX1WRqOyJea1fm4LXGIV1PHT_7GuCVo";
  auth.user.email = "shasaninisansala2003@gmail.com";
  auth.user.password = "sh12345";
  config.database_url = "https://exercise2-55d7c-default-rtdb.asia-southeast1.firebasedatabase.app/";

  Firebase.begin(&config, &auth);

  Firebase.RTDB.setString(&fbdo, "/sensor/status", "pH + Ammonia monitoring active");
}

// -------------------- Loop --------------------
void loop() {
  unsigned long now = millis();

  if (now - lastSensorMillis >= sensorInterval) {
    lastSensorMillis = now;

    // ---------- pH Reading ----------
    int ph_raw = analogRead(PH_PIN);
    float voltage = ph_raw * (3.3 / 4095.0);
    float ph_value = 7.0 - ((voltage - 2.5) / 0.18);  // rough formula

    // ---------- Ammonia Reading ----------
    int mq_raw = analogRead(MQ135_PIN);
    float ammonia = (mq_raw * 100.0) / 4095.0;  // scale % (0–100)

    // ---------- LED Indicators ----------
    if (ph_value >= 6.5 && ph_value <= 8.5) {
      digitalWrite(LED_SAFE, HIGH);
      digitalWrite(LED_HARM, LOW);
    } else {
      digitalWrite(LED_SAFE, LOW);
      digitalWrite(LED_HARM, HIGH);
    }

    // ---------- Firebase Upload ----------
    Firebase.RTDB.setFloat(&fbdo, "/sensor/ph", ph_value);
    Firebase.RTDB.setFloat(&fbdo, "/sensor/ammonia", ammonia);

    // Debug Print
    Serial.print("pH: ");
    Serial.print(ph_value);
    Serial.print(" | Ammonia: ");
    Serial.println(ammonia);
  }

  delay(10);
}
