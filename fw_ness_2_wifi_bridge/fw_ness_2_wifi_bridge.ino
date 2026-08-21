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

// Debug (USB Serial) output toggle, server-controlled via
// ness2wifi_debug_prints_enabled on the /system-status/ poll, same pattern
// as otaEnabled. Starts true so setup()'s WiFi-connect diagnostics are
// visible before the ESP has ever talked to the server; getSystemStatus()
// then takes over (defaulting to off if the server doesn't send the field,
// so a stale/undeployed server can never crash the parse — see there).
bool debugPrintsEnabled = true;

// Gate all debug output behind debugPrintsEnabled. Macros (not wrapper
// functions) so every existing call site's argument types — String,
// literals, concatenated expressions — keep working unchanged. Wrapped in
// do{...}while(0) so each expands to a single statement — without it, a
// bare `if (debugPrintsEnabled) Serial.println(x)` used as the body of an
// outer if/else (e.g. `if (cond) DEBUG_PRINTLN(...); else ...`) would let
// the outer `else` bind to this macro's own internal `if` instead.
#define DEBUG_PRINTLN(x) do { if (debugPrintsEnabled) Serial.println(x); } while (0)
#define DEBUG_PRINT(x)   do { if (debugPrintsEnabled) Serial.print(x); } while (0)
#define DEBUG_PRINTF(...) do { if (debugPrintsEnabled) Serial.printf(__VA_ARGS__); } while (0)

String fw_version = "0.1.0";

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

// Recently-executed user-input commands, keyed by (id, timestamp). If an ack
// POST fails, the server keeps offering the same command as unacknowledged
// on the next poll — this history lets that retry re-send just the ack
// instead of re-running the command on the panel a second time.
//
// id alone is NOT enough: the server reuses the same UserInput row (same id)
// for every instance of an identical command text via get_or_create() — e.g.
// every "Disarm" click resolves to the same id, since the command string is
// always the same for a given user. Only `timestamp` changes on each new
// click. Keying on id alone would make every repeat of the same command
// silently no-op after the first one ever ran.
const int executedCommandHistorySize = 20;
int executedCommandIds[executedCommandHistorySize];
String executedCommandTimestamps[executedCommandHistorySize];
int executedCommandHistoryNext = 0;  // next slot to overwrite (oldest)
int executedCommandHistoryCount = 0;

unsigned long lastUserInputCheck = 0;
const unsigned long userInputInterval = 1000; // 1 second — command responsiveness matters

unsigned long lastSystemStatusCheck = 0;
const unsigned long systemStatusInterval = 5000; // 5 seconds — ota/debug flags rarely change

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
    DEBUG_PRINTLN("⚠️ Pending queue full: oldest unsent message discarded");
  }
  pendingQueue[pendingQueueTail] = rawData;
  pendingQueueTail = (pendingQueueTail + 1) % pendingQueueCapacity;
  pendingQueueCount++;
  DEBUG_PRINTLN("📥 Queued message for retry (WiFi down or send failed)");
}

// Has this exact command instance (same id AND same timestamp) already
// been run on the panel?
bool wasCommandExecuted(int id, const String& timestamp) {
  for (int i = 0; i < executedCommandHistoryCount; i++) {
    if (executedCommandIds[i] == id && executedCommandTimestamps[i] == timestamp) return true;
  }
  return false;
}

// Record a command instance as executed, evicting the oldest entry once full.
void markCommandExecuted(int id, const String& timestamp) {
  executedCommandIds[executedCommandHistoryNext] = id;
  executedCommandTimestamps[executedCommandHistoryNext] = timestamp;
  executedCommandHistoryNext = (executedCommandHistoryNext + 1) % executedCommandHistorySize;
  if (executedCommandHistoryCount < executedCommandHistorySize) executedCommandHistoryCount++;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // Adjust if needed depending on your board

  Serial.begin(115200);  // USB debug console only — Serial1 (the panel UART) stays at 9600, fixed by the panel's own protocol
  Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2, true);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    DEBUG_PRINTLN("❌ Failed to configure static IP");
  }

  WiFi.setHostname("Ness2Web_Bridge");
  WiFi.begin(ssid, password);
  DEBUG_PRINT("🚀 Connecting to WiFi...\n");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(500);
    DEBUG_PRINT("Waiting for WiFi...\n");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("❌ WiFi connection failed. Restarting...");
    delay(3000);
    ESP.restart();
  }

  digitalWrite(LED_PIN, HIGH);  // Connected

  DEBUG_PRINTLN("\nWiFi connected. IP address: " + WiFi.localIP().toString());

  // OTA Setup
  ArduinoOTA.setHostname("esp32-ness2web-bridge");
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    DEBUG_PRINTLN("OTA Start: Updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    DEBUG_PRINTLN("\nOTA End");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    DEBUG_PRINTF("OTA Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    DEBUG_PRINTF("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) DEBUG_PRINTLN("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) DEBUG_PRINTLN("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) DEBUG_PRINTLN("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) DEBUG_PRINTLN("Receive Failed");
    else if (error == OTA_END_ERROR) DEBUG_PRINTLN("End Failed");
  });

  ArduinoOTA.begin();
  DEBUG_PRINTLN("OTA Ready");
  DEBUG_PRINTLN("IP address: " + WiFi.localIP().toString());
}

void loop() {

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWifiRetryAttempt >= wifiRetryInterval) {
      lastWifiRetryAttempt = now;
      wifiRetryCount++;

      if (wifiRetryCount > maxWifiRetryAttempts) {
        DEBUG_PRINTLN("❌ WiFi still down after max reconnect attempts. Restarting...");
        ESP.restart();
      } else {
        DEBUG_PRINTF("⚠️ WiFi disconnected. Reconnect attempt %d/%d...\n", wifiRetryCount, maxWifiRetryAttempts);
        WiFi.reconnect();
      }
    }
  } else if (wifiRetryCount > 0) {
    DEBUG_PRINTLN("✅ WiFi reconnected.");
    wifiRetryCount = 0;
  }

  if( otaEnabled ){
    // Handle OTA updates
    ArduinoOTA.handle();
  }

  unsigned long now = millis();

  // Periodic user input check — kept at 1s, command responsiveness matters
  if (now - lastUserInputCheck >= userInputInterval) {
    lastUserInputCheck = now;
    getUserInputs();
    flushPendingQueue();
  }

  // Periodic system status check (OTA/debug flags) — these rarely change,
  // so this runs on its own slower interval instead of paying for a second
  // blocking HTTP round-trip every single second.
  if (now - lastSystemStatusCheck >= systemStatusInterval) {
    lastSystemStatusCheck = now;
    getSystemStatus();
  }

  // Step 1: Read all available Serial1 data into ring buffer
  while (Serial1.available()) {
    buffer[writeIndex] = Serial1.read();
    writeIndex = advance(writeIndex);

    // Prevent buffer overwrite (discard oldest byte)
    if (writeIndex == readIndex) {
      readIndex = advance(readIndex);
      DEBUG_PRINTLN("Buffer overflow: oldest byte discarded");
    }
  }

  // Step 2: If there is a full line (ending in '\n'), extract it
  while (findNewlineInBuffer()) {
    String message = extractLineFromBuffer();
    DEBUG_PRINT("Received message: ");
    DEBUG_PRINTLN(message);

    sendToServer(message);
  }
}


bool sendPostRequest(String jsonPayload, String ApiEndpoint){
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("❌ WiFi disconnected");
    return false;
  }

  HTTPClient http;

  http.begin(serverRootURL + ApiEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Api-Key " + String(apiKey));

  int httpCode = http.POST(jsonPayload);
  bool success = (httpCode >= 200 && httpCode < 300);

  if (success) {
    DEBUG_PRINTLN("POST success, code: " + String(httpCode));
    DEBUG_PRINTLN("Response: " + http.getString());
  } else if (httpCode > 0) {
    DEBUG_PRINTLN("POST failed, HTTP code: " + String(httpCode));
  } else {
    DEBUG_PRINTLN("POST failed, error: " + http.errorToString(httpCode));
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

    DEBUG_PRINTLN("📤 Delivered previously queued message");
    pendingQueueHead = (pendingQueueHead + 1) % pendingQueueCapacity;
    pendingQueueCount--;
  }
}


bool performGETRequest(const String& endpoint, String& responsePayload) {
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("❌ WiFi disconnected");
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
    DEBUG_PRINT("❌ GET failed: ");
    DEBUG_PRINTLN(http.errorToString(httpCode));
    http.end();
    return false;
  }
}


void getUserInputs() {
  String payload;
  if (!performGETRequest(APIUserInputEndpoint, payload)) return;

  DEBUG_PRINTLN("📥 API User Inputs Response:");
  DEBUG_PRINTLN(payload);

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    DEBUG_PRINT("❌ JSON parse failed: ");
    DEBUG_PRINTLN(error.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject jsonDoc : arr) {
    int id = jsonDoc["id"] | -1;
    String timestamp = jsonDoc["timestamp"] | "";
    String raw_data = jsonDoc["raw_data"] | "";
    bool received = jsonDoc["input_command_received"] | false;

    if (!received && raw_data.length() > 0) {
      if (wasCommandExecuted(id, timestamp)) {
        // Already ran this exact command instance — the ack just didn't
        // land last time. Re-send the ack below without executing it again.
        DEBUG_PRINTLN("ℹ️ Command already executed; re-sending ack only");
      } else {
        DEBUG_PRINTLN("🚀 Unprocessed command received:");
        DEBUG_PRINTLN(raw_data);

        Serial1.println(raw_data);
        Serial1.flush();
        delay(100);

        markCommandExecuted(id, timestamp);
      }

      jsonDoc["input_command_received"] = true;
      jsonDoc["ness2wifi_ack"] = true;

      String ackPayload;
      serializeJson(jsonDoc, ackPayload);
      if (!sendPostRequest(ackPayload, APIUserInputEndpoint)) {
        DEBUG_PRINTLN("⚠️ Ack failed to send; server will re-offer this command next poll");
      }
    } else {
      DEBUG_PRINTLN("ℹ️ Already processed or no valid raw_data");
    }
  }
}


void getSystemStatus() {
  String payload;
  if (!performGETRequest(APISystemStatusEndpoint, payload)) return;

  DEBUG_PRINTLN("🔄 System Status Payload:");
  DEBUG_PRINTLN(payload);

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    DEBUG_PRINT("❌ JSON parse failed: ");
    DEBUG_PRINTLN(error.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject jsonDoc : arr) {
    otaEnabled = jsonDoc["ness2wifi_ota_enabled"];
    DEBUG_PRINT("✅ OTA Enabled: ");
    DEBUG_PRINTLN(otaEnabled ? "true" : "false");

    // Explicit | false default: if the server hasn't been redeployed with
    // this field yet (or ever drops it), this must never crash — it just
    // converges to "debug prints off" instead, which is also the desired
    // steady-state default for normal operation.
    debugPrintsEnabled = jsonDoc["ness2wifi_debug_prints_enabled"] | false;
  }
}

