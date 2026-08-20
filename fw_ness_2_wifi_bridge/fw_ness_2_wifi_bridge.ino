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

extern String serverRootURL;
extern String APIRawDataEndpoint;
extern String APIUserInputEndpoint;

extern const char* apiKey;
extern const char* OTAPassword;

extern int maxWifiRetryAttempts;

bool otaEnabled = true;

String fw_version = "0.0.1a";

// Circular buffer
const int bufferSize = 200;
byte buffer[bufferSize];
int writeIndex = 0;
int readIndex = 0;

// Pending outbound message queue — holds raw_data lines that failed to
// deliver (e.g. WiFi down) so they're retried instead of silently dropped.
const int pendingQueueCapacity = 20;
String pendingQueue[pendingQueueCapacity];
int pendingQueueHead = 0;   // index of the oldest undelivered message
int pendingQueueTail = 0;   // index of the next free slot
int pendingQueueCount = 0;

// Recently-executed user-input command IDs. If an ack POST fails, the
// server keeps offering the same command as unacknowledged on the next
// poll — this history lets that retry re-send just the ack instead of
// re-running the command on the panel a second time.
const int executedCommandHistorySize = 20;
int executedCommandIds[executedCommandHistorySize];
int executedCommandHistoryNext = 0;  // next slot to overwrite (oldest)
int executedCommandHistoryCount = 0;

unsigned long lastUserInputCheck = 0;
const unsigned long userInputInterval = 1000; // 1 second

unsigned long lastWifiRetryAttempt = 0;
const unsigned long wifiRetryInterval = 3000; // wait between reconnect attempts
int wifiRetryCount = 0;


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

// Queue an undelivered raw_data message for retry. Drops the oldest queued
// message to make room when full, mirroring the ring buffer's overflow
// policy above.
void enqueuePendingMessage(const String& rawData) {
  if (pendingQueueCount == pendingQueueCapacity) {
    pendingQueueHead = (pendingQueueHead + 1) % pendingQueueCapacity;
    pendingQueueCount--;
    Serial.println("⚠️ Pending queue full: oldest unsent message discarded");
  }
  pendingQueue[pendingQueueTail] = rawData;
  pendingQueueTail = (pendingQueueTail + 1) % pendingQueueCapacity;
  pendingQueueCount++;
  Serial.println("📥 Queued message for retry (WiFi down or send failed)");
}

// Has this user-input command ID already been run on the panel?
bool wasCommandExecuted(int id) {
  for (int i = 0; i < executedCommandHistoryCount; i++) {
    if (executedCommandIds[i] == id) return true;
  }
  return false;
}

// Record a command ID as executed, evicting the oldest entry once full.
void markCommandExecuted(int id) {
  executedCommandIds[executedCommandHistoryNext] = id;
  executedCommandHistoryNext = (executedCommandHistoryNext + 1) % executedCommandHistorySize;
  if (executedCommandHistoryCount < executedCommandHistorySize) executedCommandHistoryCount++;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // Adjust if needed depending on your board

  Serial.begin(9600);
  Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2, true);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("❌ Failed to configure static IP");
  }

  WiFi.setHostname("Ness2Web_Bridge");
  WiFi.begin(ssid, password);
  Serial.print("🚀 Connecting to WiFi...\n");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(500);
    Serial.print("Waiting for WiFi...\n");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi connection failed. Restarting...");
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

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWifiRetryAttempt >= wifiRetryInterval) {
      lastWifiRetryAttempt = now;
      wifiRetryCount++;

      if (wifiRetryCount > maxWifiRetryAttempts) {
        Serial.println("❌ WiFi still down after max reconnect attempts. Restarting...");
        ESP.restart();
      } else {
        Serial.printf("⚠️ WiFi disconnected. Reconnect attempt %d/%d...\n", wifiRetryCount, maxWifiRetryAttempts);
        WiFi.reconnect();
      }
    }
  } else if (wifiRetryCount > 0) {
    Serial.println("✅ WiFi reconnected.");
    wifiRetryCount = 0;
  }

  if( otaEnabled ){
    // Handle OTA updates
    ArduinoOTA.handle();
  }

  // Periodic user input check
  unsigned long now = millis();
  if (now - lastUserInputCheck >= userInputInterval) {
    lastUserInputCheck = now;
    getUserInputs();  // Call every 1 second
    getSystemStatus();
    flushPendingQueue();
  }

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


bool sendPostRequest(String jsonPayload, String ApiEndpoint){
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi disconnected");
    return false;
  }

  HTTPClient http;

  http.begin(serverRootURL + ApiEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Api-Key " + String(apiKey));

  int httpCode = http.POST(jsonPayload);
  bool success = (httpCode >= 200 && httpCode < 300);

  if (success) {
    Serial.println("POST success, code: " + String(httpCode));
    Serial.println("Response: " + http.getString());
  } else if (httpCode > 0) {
    Serial.println("POST failed, HTTP code: " + String(httpCode));
  } else {
    Serial.println("POST failed, error: " + http.errorToString(httpCode));
  }

  http.end();
  return success;
}


// Builds the raw-data JSON payload for a single panel message and attempts
// to deliver it. Shared by sendToServer() (new messages) and
// flushPendingQueue() (retries) so both use identical delivery logic.
bool sendRawDataMessage(const String& rawData) {
  StaticJsonDocument<250> jsonDoc;

  jsonDoc["raw_data"] = rawData;
  jsonDoc["ip"] = WiFi.localIP().toString();
  jsonDoc["fw"] = fw_version;
  jsonDoc["otaEnabled"] = otaEnabled;

  String jsonPayload;
  serializeJson(jsonDoc, jsonPayload);
  return sendPostRequest(jsonPayload, APIRawDataEndpoint);
}


void sendToServer(String rawData) {
  // If there's already a backlog, queue behind it instead of attempting an
  // out-of-order immediate send — keeps delivery order intact.
  if (pendingQueueCount > 0 || !sendRawDataMessage(rawData)) {
    enqueuePendingMessage(rawData);
  }
}


// Retries queued messages in order, oldest first. Stops at the first
// failure so a later message can't be delivered ahead of one still
// waiting behind it; whatever remains stays queued for the next call.
void flushPendingQueue() {
  while (pendingQueueCount > 0) {
    if (!sendRawDataMessage(pendingQueue[pendingQueueHead])) break;

    Serial.println("📤 Delivered previously queued message");
    pendingQueueHead = (pendingQueueHead + 1) % pendingQueueCapacity;
    pendingQueueCount--;
  }
}


bool performGETRequest(const String& endpoint, String& responsePayload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi disconnected");
    return false;
  }

  HTTPClient http;
  http.begin(serverRootURL + endpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Api-Key " + String(apiKey));

  int httpCode = http.GET();

  if (httpCode > 0) {
    responsePayload = http.getString();
    http.end();
    return true;
  } else {
    Serial.print("❌ GET failed: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return false;
  }
}


void getUserInputs() {
  String payload;
  if (!performGETRequest(APIUserInputEndpoint, payload)) return;

  Serial.println("📥 API User Inputs Response:");
  Serial.println(payload);

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("❌ JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject jsonDoc : arr) {
    int id = jsonDoc["id"] | -1;
    String raw_data = jsonDoc["raw_data"] | "";
    bool received = jsonDoc["input_command_received"] | false;

    if (!received && raw_data.length() > 0) {
      if (wasCommandExecuted(id)) {
        // Already ran this on the panel — the ack just didn't land last
        // time. Re-send the ack below without executing it again.
        Serial.println("ℹ️ Command already executed; re-sending ack only");
      } else {
        Serial.println("🚀 Unprocessed command received:");
        Serial.println(raw_data);

        Serial1.println(raw_data);
        Serial1.flush();
        delay(100);

        markCommandExecuted(id);
      }

      jsonDoc["input_command_received"] = true;
      jsonDoc["ness2wifi_ack"] = true;

      String ackPayload;
      serializeJson(jsonDoc, ackPayload);
      if (!sendPostRequest(ackPayload, APIUserInputEndpoint)) {
        Serial.println("⚠️ Ack failed to send; server will re-offer this command next poll");
      }
    } else {
      Serial.println("ℹ️ Already processed or no valid raw_data");
    }
  }
}


void getSystemStatus() {
  String payload;
  if (!performGETRequest(APISystemStatusEndpoint, payload)) return;

  Serial.println("🔄 System Status Payload:");
  Serial.println(payload);

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("❌ JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject jsonDoc : arr) {
    otaEnabled = jsonDoc["ness2wifi_ota_enabled"];
    Serial.print("✅ OTA Enabled: ");
    Serial.println(otaEnabled ? "true" : "false");
  }
}



