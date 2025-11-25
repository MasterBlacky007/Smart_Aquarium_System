#include <WiFi.h>
#include <Firebase_ESP_Client.h>

// WIFI 
#define WIFI_SSID "nishee"
#define WIFI_PASSWORD "7654321e"

// Firebase 
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

//DAILY DOSAGE 
#define DOSAGE_RELAY 22
int dosage_time_ms = 5000;   // Pump ON duration (5 sec)

//WEEKLY DOSAGE 
#define WEEKLY_DOSAGE_RELAY 23
float weekly_flowRate_mL_per_sec = 10.0;

bool weeklyDosageRunning = false;
unsigned long weeklyStartMillis = 0;
unsigned long weeklyDurationMillis = 0;


//WIFI CONNECT FUNCTION

void wifiConnect() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}


//DAILY DOSAGE FUNCTION

void startDosage() {
  Serial.println("Starting daily dosage...");
  Firebase.RTDB.setString(&fbdo, "/dose/status", "Dispensing...");

  digitalWrite(DOSAGE_RELAY, LOW);   // Relay ON
  delay(dosage_time_ms);
  digitalWrite(DOSAGE_RELAY, HIGH);  // Relay OFF

  Firebase.RTDB.setString(&fbdo, "/dose/status", "Done");
  Serial.println("Daily dosage complete!");
}

//WEEKLY DOSAGE FUNCTION

void handleWeeklyDosage() {
  bool startWeekly = false;
  Firebase.RTDB.getBool(&fbdo, "/weeklyDose/start", &startWeekly);

  bool alreadyPoured = false;
  Firebase.RTDB.getBool(&fbdo, "/weeklyDose/hasPoured", &alreadyPoured);

  if (startWeekly) {

    if (alreadyPoured) {
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false);
      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Already Poured! Wait until next week");
      return;
    }

    float mlAmount = 0;
    Firebase.RTDB.getFloat(&fbdo, "/weeklyDose/ml", &mlAmount);

    if (mlAmount <= 0) {
      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Enter valid mL");
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false);
      return;
    }

    weeklyDurationMillis = (unsigned long)((mlAmount / weekly_flowRate_mL_per_sec) * 1000);
    weeklyStartMillis = millis();

    digitalWrite(WEEKLY_DOSAGE_RELAY, LOW);  // Start pump
    Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Dispensing...");
    weeklyDosageRunning = true;

    Firebase.RTDB.setBool(&fbdo, "/weeklyDose/start", false);

    Serial.printf("Weekly dosage started: %.2f mL\n", mlAmount);
  }

  // Running weekly dosage timer 
  if (weeklyDosageRunning) {
    if (millis() - weeklyStartMillis >= weeklyDurationMillis) {
      digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH);  // Stop pump
      weeklyDosageRunning = false;

      Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Done. Poured");
      Firebase.RTDB.setBool(&fbdo, "/weeklyDose/hasPoured", true);

      Serial.println("Weekly dosage complete!");
    }
  }
}


//FIREBASE BUTTON LISTENER

void pollFirebaseButtons() {
  bool b;

  // Daily dosage button
  if (Firebase.RTDB.getBool(&fbdo, "/dose/start", &b) && b) {
    startDosage();
    Firebase.RTDB.setBool(&fbdo, "/dose/start", false);
  }
}


unsigned long lastFbPollMillis = 0;
unsigned long fbPollInterval = 600;

void setup() {
  Serial.begin(115200);

  pinMode(DOSAGE_RELAY, OUTPUT);
  digitalWrite(DOSAGE_RELAY, HIGH);

  pinMode(WEEKLY_DOSAGE_RELAY, OUTPUT);
  digitalWrite(WEEKLY_DOSAGE_RELAY, HIGH);

  wifiConnect();

  // Firebase Setup
  config.api_key = "AIzaSyASBX1WRqOyJea1fm4LXGIV1PHT_7GuCVo";
  auth.user.email = "shasaninisansala2003@gmail.com";
  auth.user.password = "sh12345";
  config.database_url = "https://exercise2-55d7c-default-rtdb.asia-southeast1.firebasedatabase.app/";

  Firebase.begin(&config, &auth);

  Firebase.RTDB.setString(&fbdo, "/dose/status", "Idle");
  Firebase.RTDB.setString(&fbdo, "/weeklyDose/status", "Time to Pour");
  Firebase.RTDB.setBool(&fbdo, "/weeklyDose/hasPoured", false);
}

void loop() {
  unsigned long now = millis();

  // Check Firebase triggers
  if (now - lastFbPollMillis >= fbPollInterval) {
    lastFbPollMillis = now;
    pollFirebaseButtons();
  }

  // Run weekly dosage if triggered
  handleWeeklyDosage();

  delay(10);
}
