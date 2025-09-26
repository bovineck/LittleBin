/*
   The bin project aims to solve two problems:
   a) is the WiFi working?
   b) what bins do I put out this week

   The code is therefore doing this:
   1. The ESP32 checks the strength of the WiFi signal
   2. If the signal is out, the ESP32 attempts reconnection and flashes red
   3. The ESP32 can be told which bins are out this week (192.168.4.1)
   4. It saves the data then changes each "changeday" at 1am
   5. The ESP32 connects with an ntp server to keep track of time
   6. Data is saved to EEPROM in case of reset

   OneCircuit Fri 26 Sep 2025 15:56:07 AEST
   https://www.youtube.com/@onecircuit-as
   https://onecircuit.blogspot.com/

*/

// libraries used
#include "WiFi.h"
#include "WebServer.h"
#include "time.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <ESP32Ping.h>

#define EEPROM_SIZE 1

// Your WiFi credentials - **CHANGE THESE** as required
const char* ssid = "MySSID";
const char* password = "MyPASS";

// The Access Point credentials for the setup portal
const char* binssid = "BinXMonitor";
const char* binpassword = NULL;

// server for time, and the offsets for location
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 32400;
const int daylightOffset_sec = 3600;

// Pin allocations for XIAO ESP32S3
const uint8_t latchPin = 1;
const uint8_t clockPin = 2;
const uint8_t dataPin = 3;
const uint8_t GreenLed = 4;
const uint8_t YellLed = 5;

bool YellLedState = LOW;
bool debugit = true;

// the html page served up from the ESP32 to change bin status
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Bin Selection</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
</head>
<body>
<center>
  <h1>Select Bins for next week</h1>
    <form action="/" method="POST">
      <font size="+2">
      <input type="radio" name="bintype" value="Green Bin">
      <label for="GB">Green Bin only</label>
      <br>
      <input type="radio" name="bintype" value="Recycle Bin">
      <label for="RB">Recycle Bin too</label><br><br>
      <input type="submit" value="Enter Choice">
      </font>
    </form>
</center>
</body>
</html>
)rawliteral";

const char* PARAM_INPUT_1 = "bintype";
String bintype;
bool newRequest = false;
char Day[10];
int Hour;

unsigned long previousMillis_Polling = 0;
unsigned long previousMillis_NTP = 0;
unsigned long previousMillis_Blink = 0;
const int pollingtime = 2000;
const unsigned long timelapse_NTP = 14400000; // 4 hours in milliseconds
const int blinkInterval = 100;

bool isWifiConnected = false;
bool isNTPReady = false;
bool ledState = LOW;

const String changeday = "Wednesday";
const int changehour = 1;
uint8_t changecount = 0;
const uint8_t numchanges = 7;

// Start the standard web server
WebServer server(80);

const uint8_t ledpattern[9] = {
  0b00000000,  // 0
  0b00000001,  // 1
  0b00000011,  // 2
  0b00000111,  // 3
  0b00001111,  // 4
  0b00011111,  // 5
  0b00111111,  // 6
  0b01111111,  // 7
  0b11111111   // 8
};

// Non-blocking LED flash on failure
void signalFailure() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis_Blink >= blinkInterval) {
    previousMillis_Blink = currentMillis;
    ledState = !ledState;
    digitalWrite(latchPin, LOW);
    shiftOut(dataPin, clockPin, MSBFIRST, ledState ? ledpattern[0] : ledpattern[8]);
    digitalWrite(latchPin, HIGH);
  }
}

// get time from the ntp server (non-blocking)
void GetLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    if (debugit) Serial.println("Failed to obtain time. NTP sync error.");
    isNTPReady = false;
    return;
  }
  strftime(Day, 10, "%A", &timeinfo);
  Hour = timeinfo.tm_hour;
  isNTPReady = true;
  if (debugit) {
    Serial.printf("NTP Time Sync successful: %s %02d:%02d:%02d\n", Day, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
}

// Handle HTTP GET requests to the root
void handleRoot() {
  server.send(200, "text/html", index_html);
}

// Handle HTTP POST requests to the root
void handlePost() {
  if (server.hasArg(PARAM_INPUT_1)) {
    bintype = server.arg(PARAM_INPUT_1);
    if (debugit) {
      Serial.print("New bin selection: ");
      Serial.println(bintype);
    }
    newRequest = true;
  }
  server.send(200, "text/html", index_html);
}

void setup() {
  delay(200);
  if (debugit) Serial.begin(115200);

  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);
  pinMode(GreenLed, OUTPUT);
  digitalWrite(GreenLed, HIGH);
  pinMode(YellLed, OUTPUT);
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, ledpattern[0]);
  digitalWrite(latchPin, HIGH);

  EEPROM.begin(EEPROM_SIZE);
  YellLedState = EEPROM.read(0);
  digitalWrite(YellLed, YellLedState);

  // Set the Wi-Fi mode to AP and STA simultaneously
  WiFi.mode(WIFI_AP_STA);
  
  // Start the Access Point and attempt STA connection
  WiFi.softAP(binssid, binpassword);
  Serial.println("Attempting STA connection...");
  WiFi.begin(ssid, password);

  // Set up standard WebServer handlers
  server.on("/", HTTP_GET, handleRoot);
  server.on("/", HTTP_POST, handlePost);

  server.begin();
  if (debugit) Serial.println("Web Server Started.");
}

void loop() {
  // Check Wi-Fi connection status
  if (WiFi.status() == WL_CONNECTED) {
    if (!isWifiConnected) {
      isWifiConnected = true;
      Serial.println("STA connection successful!");
      Serial.print("STA IP Address: ");
      Serial.println(WiFi.localIP());
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    }
    // Also, ensure the Access Point is still running
    if (WiFi.softAPIP() == INADDR_NONE) {
      if (debugit) Serial.println("AP not found, restarting...");
      WiFi.softAP(binssid, binpassword);
      if (debugit) {
        Serial.print("AP re-established at: ");
        Serial.println(WiFi.softAPIP());
      }
    }
  } else {
    isWifiConnected = false;
    // Handle Wi-Fi disconnection if needed
  }
  
  server.handleClient(); // This is essential for the standard WebServer to work

  unsigned long currentMillis = millis();

  if (isWifiConnected) {
    if (currentMillis - previousMillis_Polling >= pollingtime) {
      previousMillis_Polling = currentMillis;
      if (Ping.ping("www.google.com", 1)) {
        int strength = abs(WiFi.RSSI());
        uint8_t lights = map(strength, 90, 30, 0, 8);
        digitalWrite(latchPin, LOW);
        shiftOut(dataPin, clockPin, MSBFIRST, ledpattern[lights]);
        digitalWrite(latchPin, HIGH);
      } else {
        signalFailure();
      }
    }
  } else {
    signalFailure();
  }

  if (isWifiConnected && currentMillis - previousMillis_NTP >= timelapse_NTP) {
    previousMillis_NTP = currentMillis;
    GetLocalTime();
    
    if (isNTPReady) {
      String today = String(Day);
      if ((today == changeday) && (Hour >= changehour)) {
        if (changecount == 0) {
          YellLedState = !YellLedState;
          digitalWrite(YellLed, YellLedState);
          EEPROM.write(0, YellLedState);
          EEPROM.commit();
        }
        changecount = changecount + 1;
        if (changecount >= numchanges) {
          changecount = 0;
        }
      }
    }
  }

  if (newRequest) {
    if (bintype == "Recycle Bin") {
      YellLedState = HIGH;
      digitalWrite(YellLed, YellLedState);
      EEPROM.write(0, YellLedState);
      EEPROM.commit();
    } else {
      YellLedState = LOW;
      digitalWrite(YellLed, YellLedState);
      EEPROM.write(0, YellLedState);
      EEPROM.commit();
    }
    newRequest = false;
  }
}
