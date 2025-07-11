#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>  // Add this line

#define RXD2 20
#define TXD2 21

#define LED_PIN 10  // Onboard LED pin

const char* ssid = "iwi-perez";
const char* password = "$Andre$Naoelle$";

IPAddress local_IP(192, 168, 10, 5);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);

const char* serverURL = "http://192.168.10.70:8000/api/ness_comms-raw-data/";
const char* apiKey = "MgceyBgD.uNMNhAMcKHwHTPWcdtQfjaOWxaTG5rYq";  // Replace with your actual API key

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
    if (c == '\n') break;  // message complete
  }
  line.trim();  // removes \r or \n whitespace
  return line;
}

void setup() {

  pinMode(LED_PIN, OUTPUT);  // Set pin 10 as output
  digitalWrite(LED_PIN, LOW); // Turn on the LED

  Serial.begin(9600);
  
  Serial1.begin(9600,SERIAL_8N1, RXD2, TXD2, true);  

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("Failed to configure static IP");
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...\n");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print("Waiting wor WIFI...\n");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Turn on the LED
  }

  digitalWrite(LED_PIN, HIGH); // Turn on the LED
  Serial.println("\nWiFi connected. IP address: " + WiFi.localIP().toString());

}

void loop() {

 // Step 1: Read all available Serial1 data into ring buffer
  while (Serial1.available()) {
    
    buffer[writeIndex] = Serial1.read();
    writeIndex = advance(writeIndex);

    // Prevent buffer overwrite (discard oldest byte)
    if (writeIndex == readIndex) {
      readIndex = advance(readIndex);  // drop oldest byte
      Serial.print("dropped bytes...\n");
    }

  }

  // Step 2: If there is a full line (ending in '\n'), extract it
  while (findNewlineInBuffer()) {
    String message = extractLineFromBuffer();  // Steps 3, 4
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

    // Prepare JSON
    StaticJsonDocument<250> jsonDoc;
    jsonDoc["raw_data"] = rawData;
    jsonDoc["ip"] = local_IP.toString();
    jsonDoc["fw"] = fw_version;

    String jsonPayload;
    serializeJson(jsonDoc, jsonPayload);

    // Send request
    int httpCode = http.POST(jsonPayload);
    if (httpCode > 0) {
      Serial.println("POST success, code: " + String(httpCode));
      Serial.println("Response: " + http.getString());
    } else {
      Serial.println("POST failed, error: " + String(http.errorToString(httpCode)));
    }

    http.end();
  } else {
    Serial.println("WiFi disconnected");
  }
}
