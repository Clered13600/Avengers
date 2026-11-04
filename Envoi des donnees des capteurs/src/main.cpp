#include <Arduino.h>
#include <WiFi.h>
#include "esp_wpa2.h"
#include "HTTPClient.h"
#include <ModbusMaster.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

const char* ssid = "Freebox-CBEE5C"; // Pour partage de connection ou Wifi simple
const char* PASSWORD = "46hx7q2w3nqmf2d6q7cv35"; // Mot de passe du Wifi simple
const char* WIFI_SSID = "eduroam";
const char* EAP_IDENTITY = "joris.bultel@etu.univ-amu.fr"; // identifiant ENT complet
const char* EAP_PASSWORD = "AMUPryska13130-Luffy360*";
const char* GROUP_KEY = "e81e6a27239bf334796bbbc4a9b1989a";
const char* URL = "http://api.aircarto.fr/iut-amu/push";

//Définition de l'objet BME280
Adafruit_BME280 bme;
unsigned status;

float temp;
float hum;

// Branchement du NextPM sur l'UART n°1 de l'ESP32 (cartes 2026 : ModuleAir et NebuleAir)
#define NEXTPM_RX 39   // l'ESP32 reçoit sur GPIO 39 (fil TX du capteur)
#define NEXTPM_TX 32   // l'ESP32 émet sur GPIO 32 (fil RX du capteur)

// Registres Modbus du NextPM
#define REG_STATUS 0x13   // état du capteur : codes d'erreur
#define REG_PM1    0x38   // moyennes 10 s (tests) ; en production, moyennes 60 s :
#define REG_PM25   0x3A   // PM1 = 0x44, PM2.5 = 0x46, PM10 = 0x48
#define REG_PM10   0x3C

// Signification des 8 bits du registre d'état (bit 0 en premier)
const char* STATUS_BITS[8] = {
  "veille", "mode degrade", "demarrage en cours", "humidite trop forte",
  "erreur capteur T/HR", "erreur ventilateur", "erreur memoire", "erreur laser"
};
// Bits qui rendent la mesure inutilisable : démarrage, humidité, ventilateur, laser
#define STATUS_CRITICAL 0xAC

HardwareSerial nextpmSerial(1);   // UART n°1 de l'ESP32
ModbusMaster nextpm;

// Une valeur PM occupe 2 registres de 16 bits (poids faible d'abord), en
// millièmes de µg/m³. Renvoie -1 si le capteur n'a pas répondu.
float readPM(uint16_t reg) {
  if (nextpm.readHoldingRegisters(reg, 2) != nextpm.ku8MBSuccess) {
    return -1;
  }
  uint32_t raw = ((uint32_t)nextpm.getResponseBuffer(1) << 16) | nextpm.getResponseBuffer(0);
  return raw / 1000.0;
}

void connectWiFi() {

  Serial.print("Connexion au réseau "); Serial.println(WIFI_SSID);
  //Déconnection du Wifi puis reconnection
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  // eduroam = WPA2 Entreprise (PEAP / MSCHAPv2) : identifiant + mot de passe ENT
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_wifi_sta_wpa2_ent_enable();
  WiFi.begin(WIFI_SSID);

  //20 tentatives de connection avec Eduoram
  int essais = 0;
  while ( WiFi.status() != WL_CONNECTED && essais != 20) {
    essais += 1;
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Echec de la connection à eduoram");

  //Tentative de connection à l'autre Wifi si eduoram n'a pas fonctionnée
  if (WiFi.status() != WL_CONNECTED) {
    esp_wifi_sta_wpa2_ent_disable();
    WiFi.begin(ssid,PASSWORD);
    Serial.print("Connection à "); Serial.println(ssid);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
  }
  // Connexion reussie
  Serial.println();
  Serial.print("\nConnecté au Wifi");
  Serial.print("Adresse IP locale attribuee : ");
  Serial.println(WiFi.localIP());
  Serial.print("Puissance du signal (RSSI) : ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

void sendMeasure() {
 bool PMValide = true;
 bool BMEValide = true;
 temp = bme.readTemperature();
 hum = bme.readHumidity();

 uint8_t result = nextpm.readHoldingRegisters(REG_STATUS, 1); //Mesure non fiable, on renvoit 0
  if (result != nextpm.ku8MBSuccess) {
    PMValide = false;
  }

 uint8_t status = nextpm.getResponseBuffer(0) & 0xFF;
 if (status & STATUS_CRITICAL) {
    PMValide = false;
  }

  if (isnan(temp) || isnan(hum) == true) {
    BMEValide = false;
  }
 // Création du Json
 String json = "{";
 json += "\"device_id\": \"" + WiFi.macAddress() + "\"";

 if (PMValide) {
  json += ", \"pm1\": " + String(readPM(REG_PM1)) + ", ";
  json += "\"pm25\": " + String(readPM(REG_PM25)) + ", ";
  json += "\"pm10\": " + String(readPM(REG_PM10));
 }

 if (BMEValide) {
  json += ", \"temp\": " + String(temp) + ", ";
  json += "\"hum\": " + String(hum);
 }
 json += "}";
 Serial.print("Envoi : ");
 Serial.println(json);

 //Connection HTTP
 HTTPClient http;
 http.begin(URL);
 http.addHeader("Content-Type", "application/json");
 http.addHeader("X-Group-Key", GROUP_KEY);
 int code = http.POST(json);
 if (code > 0) {
 // Code HTTP reçu : afficher aussi le corps, il explique les erreurs.
 Serial.print("Reponse HTTP ");
 Serial.println(code);
 Serial.println(http.getString());
 } else {
 // Pas de réponse du tout : Wi-Fi, URL ou serveur injoignable.
 Serial.print("Echec de l'envoi : ");
 Serial.println(http.errorToString(code));
 }
 http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  status = bme.begin(0x76); //Connection au BME280

  nextpmSerial.begin(115200, SERIAL_8E1, NEXTPM_RX, NEXTPM_TX);  // 8E1 : parité paire !
  nextpm.begin(1, nextpmSerial);  

  connectWiFi();
}

void loop() {

  // Verification reguliere de l'etat de la connexion
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("En ligne - IP : %s | RSSI : %d dBm\n", 
                  WiFi.localIP().toString().c_str(), 
                  WiFi.RSSI());
    Serial.println();
    sendMeasure();
  } else {
    Serial.println("Perte de signal WiFi !");
    connectWiFi();
  }
  delay(10000);
}