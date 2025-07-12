#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "env.h"

#define RXD2 20
#define TXD2 21

#define LED_PIN 10  // Onboard LED pin

IPAddress local_IP(192, 168, 10, 5);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);

extern const char* ssid;
extern const char* password;

extern const char* serverURL;
extern const char* apiKey;
extern const char* OTAPassword;

String fw_version = "0.0.1a";

// Circular buffer
const int bufferSize = 200;
byte buffer[bufferSize];
int writeIndex = 0;
int readIndex = 0;

// Advance index in a circular fashion
int advance(int index) {
  return (index + 1) % bufferSize;
}

// Check if buffer has a complete line ending in '\n'
bool findNewlineInBuffer() {
  int idx = readIndex;
  while (idx != writeIndex) {
    if (buffer[idx] == '\n') return true;
    idx = advance(idx);
  }
  return false;
}

// Extract a full line from the buffer into a String (up to and including '\n')
String extractLineFromBuffer() {
  String line = "";
  while (readIndex != writeIndex) {
    char c = buffer[readIndex];
    readIndex = advance(readIndex);
    line += c;
    if (c == '\n') break;
  }
  line.trim();  // removes \r or \n whitespace
  return line;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // Adjust if needed depending on your board

  Serial.begin(9600);
  Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2, true);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("Failed to configure static IP");
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...\n");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(500);
    Serial.print("Waiting for WiFi...\n");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed. Restarting...");
    delay(3000);
    ESP.restart();
  }

  digitalWrite(LED_PIN, HIGH);  // Connected

  Serial.println("\nWiFi connected. IP address: " + WiFi.localIP().toString());

  // OTA Setup
  ArduinoOTA.setHostname("esp32-ness2web-bridge");
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA Start: Updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.println("IP address: " + WiFi.localIP().toString());
}

void loop() {
  // Handle OTA updates
  ArduinoOTA.handle();

  // Step 1: Read all available Serial1 data into ring buffer
  while (Serial1.available()) {
    buffer[writeIndex] = Serial1.read();
    writeIndex = advance(writeIndex);

    // Prevent buffer overwrite (discard oldest byte)
    if (writeIndex == readIndex) {
      readIndex = advance(readIndex);
      Serial.println("Buffer overflow: oldest byte discarded");
    }
  }

  // Step 2: If there is a full line (ending in '\n'), extract it
  while (findNewlineInBuffer()) {
    String message = extractLineFromBuffer();
    Serial.print("Received message: ");
    Serial.println(message);

    sendToServer(message);
  }
}

void sendToServer(String rawData) {

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Api-Key " + String(apiKey));

    StaticJsonDocument<250> jsonDoc;
    jsonDoc["raw_data"] = rawData;

    jsonDoc["ip"] = WiFi.localIP().toString();
    jsonDoc["fw"] = fw_version;

    String jsonPayload;
    serializeJson(jsonDoc, jsonPayload);

    int httpCode = http.POST(jsonPayload);
    if (httpCode > 0) {
      Serial.println("POST success, code: " + String(httpCode));
      Serial.println("Response: " + http.getString());
    } else {
      Serial.println("POST failed, error: " + http.errorToString(httpCode));
    }

    http.end();
  } 
  else {
    Serial.println("WiFi disconnected");
  }

}
