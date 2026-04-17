//=====THESIS ARDUINO CODE======

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TinyGPS++.h>
#include <ArduinoJson.h>

// ========== HARDWARE CONFIGURATION ==========
#define BUTTON_PIN       5
#define LONG_PRESS_MS    3000UL
#define GPS_RX_PIN       20
#define GPS_TX_PIN       21

// ========== LED CONFIGURATION - COMPLETELY DISABLED ==========
#define BLUE_LED_PIN     8

// ========== NETWORK CONFIGURATION ==========
const char* ssid     = "realme8";
const char* password = "12345678";
const char* projectId     = "alert-on";
const char* sendCollection = "DeviceLoc";
const char* sendDocument   = "Device-A";
const char* startCollection = "DeviceStart";
const char* startDocument   = "Device-A";

// ========== TIMING CONFIGURATION ==========
const unsigned long UPDATE_INTERVAL = 2000;
const unsigned long CANCEL_WINDOW = 20000;
const unsigned long DOUBLE_PRESS_TIMEOUT = 800;      // Increased to 800ms for easier detection
const unsigned long DEBOUNCE_DELAY = 50;              // Debounce delay
const unsigned long ACTIVE_CHECK_INTERVAL = 10000;
const unsigned long REACTIVATION_COOLDOWN_MS = 30000;
const int MIN_SATELLITES = 4;

// ========== GPS OBJECT ==========
HardwareSerial GPSSerial(1);
TinyGPSPlus gps;

// ========== FLAGS ==========
struct {
  bool emergencyTriggered : 1;
  bool isTracking : 1;
  bool cancelled : 1;
  bool wifiConnected : 1;
  bool wifiConnecting : 1;
  bool hasValidFix : 1;
  bool waitingDoublePress : 1;
  bool deviceActive : 1;
  bool reactivationCooldown : 1;
  bool firstFixSent : 1;
} flags = {false, false, false, false, false, false, false, true, false, false};

// ========== TIMING VARIABLES ==========
unsigned long triggerTime = 0;
unsigned long lastUpdate = 0;
unsigned long lastStatusDisplay = 0;
unsigned long lastButtonPress = 0;
unsigned long lastActiveCheck = 0;
unsigned long reactivationCooldownEnd = 0;
unsigned long lastDebounceTime = 0;
uint8_t pressCount = 0;
bool buttonState = HIGH;
bool lastButtonState = HIGH;

// ========== FORWARD DECLARATIONS ==========
void connectWiFi();
void handleWiFiConnection();
void checkButton();
void checkForLongPress();
void checkForDoublePress();
void sendLocation();
void sendFirstLocationToStart();
void readGPS();
void updateFixStatus();
void checkEmergencyStatus();
void setEmergencyActiveTrue();
void handleEmergencyCancellationWindow();

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Disable blue LED
  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(BLUE_LED_PIN, LOW);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  
  Serial.println(F("\n========================================"));
  Serial.println(F("   EMERGENCY DEVICE - READY"));
  Serial.println(F("   Hold button 3s to trigger emergency"));
  Serial.println(F("   Double-press within 20s to cancel (800ms window)"));
  Serial.println(F("========================================\n"));
  
  connectWiFi();
  checkEmergencyStatus();
  lastActiveCheck = millis();
}

// ========== MAIN LOOP ==========
void loop() {
  readGPS();
  updateFixStatus();
  
  // Handle WiFi connection
  if (flags.wifiConnecting && !flags.wifiConnected) {
    handleWiFiConnection();
  }
  
  // Periodically check active status, but NOT during cooldown
  if (flags.wifiConnected && !flags.reactivationCooldown && 
      (millis() - lastActiveCheck >= ACTIVE_CHECK_INTERVAL)) {
    checkEmergencyStatus();
    lastActiveCheck = millis();
  }
  
  // End cooldown if time elapsed
  if (flags.reactivationCooldown && millis() >= reactivationCooldownEnd) {
    flags.reactivationCooldown = false;
    Serial.println(F("[COOLDOWN ENDED] Remote deactivation checks resumed"));
  }
  
  // If device is deactivated, still listen for button to reactivate
  if (!flags.deviceActive) {
    checkButton();
    delay(100);
    return;
  }
  
  // Handle emergency states
  if (flags.emergencyTriggered && !flags.isTracking && !flags.cancelled) {
    handleEmergencyCancellationWindow();
  } else if (!flags.emergencyTriggered && !flags.isTracking) {
    checkButton();
  }
  
  // Send location updates when tracking
  if (flags.isTracking && (millis() - lastUpdate >= UPDATE_INTERVAL)) {
    if (flags.wifiConnected && flags.hasValidFix) {
      sendLocation();
    } else if (!flags.wifiConnected && !flags.wifiConnecting) {
      connectWiFi();
    }
    lastUpdate = millis();
  }
  
  delay(10);  // Smaller delay for better button response
}

// ========== EMERGENCY HANDLING ==========
void handleEmergencyCancellationWindow() {
  unsigned long elapsed = millis() - triggerTime;
  
  if (elapsed >= CANCEL_WINDOW) {
    // START TRACKING - Cancellation window expired
    flags.isTracking = true;
    flags.waitingDoublePress = false;
    lastUpdate = millis() - UPDATE_INTERVAL;
    
    // Send first coordinates to DeviceStart NOW (after cancellation window)
    if (flags.wifiConnected && flags.hasValidFix && !flags.firstFixSent) {
      sendFirstLocationToStart();
      flags.firstFixSent = true;
    }
    
    Serial.println(F("\n🚨 [EMERGENCY ACTIVE] Tracking started"));
    Serial.println(F("   Sending location every 2 seconds"));
  } else {
    checkButton();  // Check for double-press cancellation
    
    if (millis() - lastStatusDisplay >= 1000) {
      int secondsLeft = (CANCEL_WINDOW - elapsed) / 1000;
      Serial.print(F("⏱️ Cancel in: "));
      Serial.print(secondsLeft);
      Serial.print(F("s | GPS: "));
      Serial.print(flags.hasValidFix ? F("✅") : F("❌"));
      Serial.print(F(" | SAT: "));
      Serial.println(gps.satellites.value());
      lastStatusDisplay = millis();
    }
  }
}

// ========== IMPROVED BUTTON HANDLING WITH DEBOUNCE ==========
void checkButton() {
  // Read button state with debounce
  int reading = digitalRead(BUTTON_PIN);
  
  // If the button state changed, reset debounce timer
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  // If enough time passed, accept the button state
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;
      
      // Button is pressed (LOW because INPUT_PULLUP)
      if (buttonState == LOW) {
        onButtonPress();
      }
    }
  }
  
  lastButtonState = reading;
}

void onButtonPress() {
  unsigned long currentPressTime = millis();
  
  // Check for long press (emergency trigger)
  if (!flags.emergencyTriggered && !flags.isTracking) {
    // Start tracking potential long press
    unsigned long pressStart = currentPressTime;
    while (digitalRead(BUTTON_PIN) == LOW) {
      if (millis() - pressStart >= LONG_PRESS_MS) {
        // LONG PRESS DETECTED
        triggerEmergency();
        return;
      }
      delay(10);
    }
    
    // If we exit the loop without long press, it's a short press
    Serial.println(F("   Short press - ignored"));
    return;
  }
  
  // Check for double press during cancellation window
  if (flags.waitingDoublePress && flags.emergencyTriggered && !flags.isTracking) {
    if (pressCount == 0) {
      // First press
      pressCount = 1;
      lastButtonPress = currentPressTime;
      Serial.println(F("🔵 First press detected... waiting for second press"));
      
      // Set timeout to reset if no second press
      unsigned long timeoutStart = currentPressTime;
      while (millis() - timeoutStart < DOUBLE_PRESS_TIMEOUT) {
        // Check for second press
        if (digitalRead(BUTTON_PIN) == LOW) {
          delay(DEBOUNCE_DELAY);
          if (digitalRead(BUTTON_PIN) == LOW) {
            // Wait for release
            while (digitalRead(BUTTON_PIN) == LOW) delay(10);
            
            // SECOND PRESS DETECTED!
            Serial.println(F("\n🔴🔴🔴 DOUBLE PRESS DETECTED! 🔴🔴🔴"));
            cancelEmergency();
            pressCount = 0;
            return;
          }
        }
        delay(10);
      }
      
      // Timeout - no second press
      Serial.println(F("⏰ Double-press timeout - no cancellation"));
      pressCount = 0;
    }
  }
}

void triggerEmergency() {
  Serial.println(F("\n⚠️ [EMERGENCY TRIGGERED]"));
  
  // Force WiFi connection if needed
  if (!flags.wifiConnected && !flags.wifiConnecting) {
    connectWiFi();
    delay(2000);
  }
  
  // Update Firestore (set emergency_active = true)
  if (flags.wifiConnected) {
    setEmergencyActiveTrue();
  } else {
    Serial.println(F("   ⚠️ No WiFi - will retry later"));
  }
  
  // Force local activation
  flags.deviceActive = true;
  flags.emergencyTriggered = true;
  triggerTime = millis();
  flags.cancelled = false;
  flags.waitingDoublePress = true;
  pressCount = 0;
  
  // Reset first fix flag
  flags.firstFixSent = false;
  
  // Start cooldown to prevent immediate deactivation
  flags.reactivationCooldown = true;
  reactivationCooldownEnd = millis() + REACTIVATION_COOLDOWN_MS;
  
  Serial.println(F("   20 seconds to cancel (double-press within 800ms)"));
  Serial.println(F("   🔄 Reactivation cooldown active (30s)"));
}

void cancelEmergency() {
  flags.emergencyTriggered = false;
  flags.isTracking = false;
  flags.cancelled = true;
  flags.waitingDoublePress = false;
  
  Serial.println(F("✅ [CANCELLED] Emergency aborted"));
  Serial.println(F("   Device returning to standby mode"));
}

// ========== GPS FUNCTIONS ==========
void readGPS() {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }
}

void updateFixStatus() {
  bool wasValid = flags.hasValidFix;
  flags.hasValidFix = (gps.location.isValid() && gps.satellites.value() >= MIN_SATELLITES);
  if (!wasValid && flags.hasValidFix) {
    Serial.println(F("📍 GPS FIX ACQUIRED!"));
  }
}

// ========== WIFI FUNCTIONS ==========
void connectWiFi() {
  if (flags.wifiConnected || flags.wifiConnecting) return;
  flags.wifiConnecting = true;
  WiFi.begin(ssid, password);
  Serial.print(F("📶 WiFi connecting"));
  unsigned long start = millis();
  while (millis() - start < 10000) {
    if (WiFi.status() == WL_CONNECTED) {
      flags.wifiConnected = true;
      flags.wifiConnecting = false;
      Serial.println(F(" ✅"));
      Serial.print(F("   IP: "));
      Serial.println(WiFi.localIP());
      return;
    }
    delay(100);
    Serial.print(F("."));
  }
  flags.wifiConnecting = false;
  Serial.println(F(" ❌ Failed"));
}

void handleWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    flags.wifiConnected = true;
    flags.wifiConnecting = false;
  } else if (millis() - lastActiveCheck > 15000) {
    flags.wifiConnecting = false;
    flags.wifiConnected = false;
  }
}

// ========== FIRESTORE FUNCTIONS ==========

void sendFirstLocationToStart() {
  Serial.println(F("\n📍 [FIRST FIX] Storing first coordinates to DeviceStart/Device-A"));
  
  if (!flags.wifiConnected) {
    Serial.println(F("   ❌ WiFi not connected - cannot store"));
    return;
  }
  
  if (!flags.hasValidFix) {
    Serial.println(F("   ❌ No valid GPS fix"));
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) +
               "/databases/(default)/documents/" + String(startCollection) + "/" + String(startDocument);
  
  String payload = "{"
    "\"fields\": {"
      "\"lat\": {\"doubleValue\": " + String(gps.location.lat(), 6) + "},"
      "\"lon\": {\"doubleValue\": " + String(gps.location.lng(), 6) + "}"
    "}"
  "}";
  
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  int code = https.PATCH(payload);
  
  if (code == 200 || code == 201) {
    Serial.println(F("   ✅ First coordinates stored successfully!"));
    Serial.print(F("   📍 Lat: "));
    Serial.print(gps.location.lat(), 6);
    Serial.print(F(", Lon: "));
    Serial.println(gps.location.lng(), 6);
  } else {
    Serial.print(F("   ❌ Failed! HTTP: "));
    Serial.println(code);
  }
  
  https.end();
}

void setEmergencyActiveTrue() {
  Serial.println(F("   📤 Setting emergency_active: true"));
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) +
               "/databases/(default)/documents/" + String(sendCollection) + "/" + String(sendDocument);
  
  String payload = "{"
    "\"fields\": {"
      "\"emergency_active\": {\"booleanValue\": true},"
      "\"last_triggered\": {\"integerValue\": " + String(millis() / 1000) + "}"
    "}"
  "}";
  
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  int code = https.PATCH(payload);
  
  if (code == 200 || code == 201) {
    Serial.println(F("   ✓ Firestore updated: emergency_active = true"));
  } else {
    Serial.print(F("   ⚠️ Firestore update failed (HTTP "));
    Serial.print(code);
    Serial.println(F(")"));
  }
  https.end();
}

void checkEmergencyStatus() {
  if (!flags.wifiConnected) return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) +
               "/databases/(default)/documents/" + String(sendCollection) + "/" + String(sendDocument);
  
  https.begin(client, url);
  int code = https.GET();
  
  if (code == 200) {
    String response = https.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, response);
    
    JsonObject fields = doc["fields"];
    if (fields.containsKey("emergency_active")) {
      bool isActive = fields["emergency_active"]["booleanValue"];
      
      if (!isActive && flags.deviceActive && !flags.reactivationCooldown) {
        flags.deviceActive = false;
        flags.isTracking = false;
        flags.emergencyTriggered = false;
        Serial.println(F("\n🔴 [DEACTIVATED] Emergency responded - stopping"));
        Serial.println(F("   Press button 3s to start new emergency"));
      } 
      else if (isActive && !flags.deviceActive) {
        flags.deviceActive = true;
        Serial.println(F("\n🟢 [REACTIVATED] Device active again"));
      }
    }
  } else if (code == 404) {
    if (flags.deviceActive && !flags.reactivationCooldown) {
      flags.deviceActive = false;
      flags.isTracking = false;
      flags.emergencyTriggered = false;
      Serial.println(F("\n🔴 [NO DOCUMENT] Device inactive - press button to start"));
    }
  }
  https.end();
}

void sendLocation() {
  if (!flags.deviceActive) return;
  if (!flags.hasValidFix) {
    Serial.println(F("⚠️ No GPS fix - location not sent"));
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) +
               "/databases/(default)/documents/" + String(sendCollection) + "/" + String(sendDocument);
  
  String payload = "{"
    "\"fields\": {"
      "\"lat\": {\"doubleValue\": " + String(gps.location.lat(), 6) + "},"
      "\"lon\": {\"doubleValue\": " + String(gps.location.lng(), 6) + "},"
      "\"satellites\": {\"integerValue\": " + String(gps.satellites.value()) + "},"
      "\"timestamp\": {\"integerValue\": " + String(millis() / 1000) + "}"
    "}"
  "}";
  
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  int code = https.PATCH(payload);
  
  if (code == 200 || code == 201) {
    Serial.print(F("📡 ✓"));
  } else if (code == 404) {
    Serial.println(F("\n📡 Document not found - stopping"));
    flags.deviceActive = false;
  } else {
    Serial.print(F("📡 ✗("));
    Serial.print(code);
    Serial.print(F(")"));
  }
  https.end();
}
