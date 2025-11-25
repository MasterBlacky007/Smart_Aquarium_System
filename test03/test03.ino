#include <OneWire.h>
#include <DallasTemperature.h>
#include <Firebase_ESP_Client.h>


#define ONE_WIRE_BUS 4
#define HEATER_RELAY 25
#define COOLING_RELAY 21

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

extern FirebaseData fbdo;   
extern bool fbSetFloat(const char* path, float v);
extern bool fbSetBool(const char* path, bool v);

float getWaterTemperature() {
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  if (tempC == DEVICE_DISCONNECTED_C || tempC < -20 || tempC > 80) {
    Serial.println("Error: Temperature sensor not detected!");
    return -127.0;
  }

  return tempC;
}

void updateHeaterCooler(float tempC) {
  bool heaterOn = false;
  bool coolerOn = false;

  if (tempC < 29.0) {
    heaterOn = true;
    coolerOn = false;
  } 
  else if (tempC > 30.0) {
    heaterOn = false;
    coolerOn = true;
  } 
  else {
    heaterOn = false;
    coolerOn = false;
  }

  digitalWrite(HEATER_RELAY, heaterOn);
  digitalWrite(COOLING_RELAY, coolerOn);

  // Firebase update
  fbSetBool("/control/heater", heaterOn);
  fbSetBool("/control/cooler", coolerOn);

  Serial.print("Heater: ");
  Serial.print(heaterOn ? "ON" : "OFF");
  Serial.print("  |  Cooler: ");
  Serial.println(coolerOn ? "ON" : "OFF");
}

void updateTemperatureFirebase(float tempC) {
  fbSetFloat("/sensor/temperature", tempC);
}

void handleTemperatureSystem() {
  float temp = getWaterTemperature();

  if (temp != -127.0) {
    Serial.print("Water Temperature: ");
    Serial.print(temp);
    Serial.println(" °C");

    updateTemperatureFirebase(temp);
    updateHeaterCooler(temp);
  }
}
