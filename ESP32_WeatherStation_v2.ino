/*
 * ESP32 Weather Station v2.1.0
 * 
 * Features:
 * - Real-time BME280 sensor monitoring
 * - MQ-135 Gas sensor with remote calibration
 * - Firebase Realtime Database integration
 * - Authenticated REST API support
 */

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SSD1306.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <HTTPClient.h>

#include "secrets.h"

// Hardware Configuration
// Hardware
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define MQ135_PIN 34
#define LED_STATUS 2          // Built-in LED for status
#define WATCHDOG_TIMEOUT 30   // 30 seconds

// Timing
#define UPLOAD_INTERVAL 10000      // 10 seconds for live data
#define HISTORY_INTERVAL 300000    // 5 minutes for historical data
#define DISPLAY_UPDATE 2000        // 2 seconds display refresh
#define WIFI_TIMEOUT 20000         // 20 seconds WiFi connection timeout
#define FIREBASE_TIMEOUT 15000     // 15 seconds Firebase timeout

// MQ-135 Calibration (Optimized for production)
#define RL_VALUE 10                // Load resistance in kilo ohms
#define RO_CLEAN_AIR_FACTOR 3.6    // RO in clean air / RS in clean air
#define CALIBRATION_SAMPLE_TIMES 20  // Reduced from 50 for faster calibration
#define CALIBRATION_SAMPLE_INTERVAL 250  // Reduced from 500ms

// Alert Thresholds
#define TEMP_HIGH_ALERT 35.0       // °C
#define TEMP_LOW_ALERT 10.0        // °C
#define HUMIDITY_HIGH_ALERT 80.0   // %
#define HUMIDITY_LOW_ALERT 30.0    // %
#define GAS_ALERT_THRESHOLD 300    // PPM (adjust for your environment)
#define PRESSURE_LOW_ALERT 980.0   // hPa (storm warning)


// Credentials are now in secrets.h (not tracked in git)
// The secrets.h file should define:
// - WIFI_SSID
// - WIFI_PASSWORD  
// - FIREBASE_API_KEY
// - FIREBASE_DATABASE_URL
// - FIREBASE_USER_EMAIL
// - FIREBASE_USER_PASSWORD
// - DEVICE_ID

// Firmware Info
const char* FIRMWARE_VERSION = "2.1.1";


Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_BME280 bme;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
Preferences preferences;

// Global Variables
// Timing
unsigned long lastUpload = 0;
unsigned long lastHistory = 0;
unsigned long lastDisplay = 0;
unsigned long wifiReconnectTime = 0;
unsigned long lastCommandCheck = 0;

// Sensor Data
float temperature = 0.0;
float humidity = 0.0;
float pressure = 0.0;
int gasRaw = 0;
float gasPPM = 0.0;
float Ro = 10.0;  // Sensor resistance in clean air

// Status Flags
bool wifiConnected = false;
bool firebaseReady = false;
bool sensorError = false;
bool hasTestedApi = false;
int reconnectAttempts = 0;

// Alert Flags
bool gasAlertActive = false;
bool tempAlertActive = false;

// Functions
void initHardware();
void connectWiFi();
void handleWiFiDisconnect();
void setupOTA();
void initFirebase();
void setupPresenceDetection();
void readSensors();
void calibrateMQ135();
float calculateGasPPM(int adcValue);
void updateDisplay();
void uploadLiveData();
void storeHistoricalData();
void sendBootNotification();
void checkAlerts();
void sendAlert(String alertType, String message);
void checkFirebaseCommands();
void testAuthenticatedRequest(); // Added prototype

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n\n=== ESP32 Weather Station v2.1.1 ==="));
  
  // Initialize Watchdog (compatible with both old and new ESP32 core versions)
  // Deinitialize first to prevent "already initialized" error on reboot
  esp_task_wdt_deinit();
  delay(100);
  
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    // ESP32 core v3.x and above
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WATCHDOG_TIMEOUT * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
  #else
    // ESP32 core v2.x
    esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
  #endif
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog initialized (30s timeout)");
  
  // Status LED
  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);
  
  // Preferences (for storing calibration)
  preferences.begin("weather", false);
  Ro = preferences.getFloat("mq135_ro", 10.0);
  
  // Initialize Hardware
  initHardware();
  esp_task_wdt_reset();
  
  // Connect WiFi
  connectWiFi();
  esp_task_wdt_reset();
  
  // Setup OTA
  setupOTA();
  
  // Initialize Firebase
  initFirebase();
  esp_task_wdt_reset();
  
  // Calibration Strategy: Only on first boot or manual trigger
  // This prevents watchdog timeout on every reboot
  if (preferences.getBool("force_calib", false)) {
    Serial.println("Forced calibration requested...");
    calibrateMQ135();
    preferences.putBool("force_calib", false);
  } else if (!preferences.isKey("mq135_ro")) {
    // First time boot - needs calibration
    Serial.println("First boot detected - calibrating...");
    calibrateMQ135();
  } else {
    Serial.println("Using stored calibration. Ro = " + String(Ro, 2));
    Serial.println("To recalibrate, use Firebase command or reset preferences");
  }
  esp_task_wdt_reset();
  
  // Send boot notification
  sendBootNotification();
  
  Serial.println("Setup complete! System ready.");
  Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
  esp_task_wdt_reset();
}

void loop() {
  esp_task_wdt_reset();  // Reset watchdog
  
  // Handle OTA
  ArduinoOTA.handle();
  
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    handleWiFiDisconnect();
  } else {
    wifiConnected = true;
    reconnectAttempts = 0;
  }
  
  // Check for remote commands (every 5 seconds)
  if (millis() - lastCommandCheck > 5000 && wifiConnected && firebaseReady) {
    checkFirebaseCommands();
    lastCommandCheck = millis();
    
    // Run API test once
    if (!hasTestedApi) {
      testAuthenticatedRequest();
      hasTestedApi = true;
    }
  }
  
  // Read Sensors
  readSensors();
  
  // Update Display
  if (millis() - lastDisplay > DISPLAY_UPDATE) {
    updateDisplay();
    lastDisplay = millis();
  }
  
  // Upload Live Data
  if (millis() - lastUpload > UPLOAD_INTERVAL && wifiConnected) {
    uploadLiveData();
    lastUpload = millis();
  }
  
  // Store Historical Data
  if (millis() - lastHistory > HISTORY_INTERVAL && wifiConnected) {
    storeHistoricalData();
    lastHistory = millis();
  }
  
  // Check for Alerts
  checkAlerts();
  
  // Blink status LED to show alive
  static unsigned long ledBlink = 0;
  if (millis() - ledBlink > 1000) {
    digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
    ledBlink = millis();
  }
  
  delay(100);  // Small delay to prevent tight loop
}

void initHardware() {
  Serial.println("Initializing hardware...");
  
  // I2C
  Wire.begin(21, 22);
  
  // OLED Display
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("ERROR: SSD1306 failed!");
    // Continue without display
  } else {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Weather Station");
    display.println("v" + String(FIRMWARE_VERSION));
    display.println("Initializing...");
    display.display();
  }
  
  // BME280 Sensor
  if (!bme.begin(0x76) && !bme.begin(0x77)) {
    Serial.println("ERROR: BME280 not found!");
    sensorError = true;
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("SENSOR ERROR!");
    display.println("Check BME280");
    display.display();
    delay(3000);
  } else {
    Serial.println("BME280 initialized");
    // Configure BME280 for weather monitoring
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1, // temperature
                    Adafruit_BME280::SAMPLING_X1, // pressure
                    Adafruit_BME280::SAMPLING_X1, // humidity
                    Adafruit_BME280::FILTER_OFF);
  }
  
  // MQ-135 Pin
  pinMode(MQ135_PIN, INPUT);
  Serial.println("MQ-135 pin configured");
}

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Connecting WiFi...");
  display.display();
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();  // Reset watchdog during connection
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal: ");
    Serial.println(WiFi.RSSI());
    wifiConnected = true;
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("WiFi Connected!");
    display.print("IP: ");
    display.println(WiFi.localIP());
    display.display();
    delay(2000);
  } else {
    Serial.println("\nWiFi Connection Failed!");
    wifiConnected = false;
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("WiFi Failed!");
    display.println("Retrying...");
    display.display();
  }
}

void handleWiFiDisconnect() {
  if (millis() - wifiReconnectTime > 30000) {  // Try reconnect every 30s
    Serial.println("WiFi disconnected. Reconnecting...");
    wifiConnected = false;
    reconnectAttempts++;
    
    if (reconnectAttempts > 5) {
      Serial.println("Too many reconnect attempts. Rebooting...");
      delay(1000);
      ESP.restart();
    }
    
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiReconnectTime = millis();
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword("weather2024");  // Change this!
  
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA Update: " + type);
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("OTA UPDATE");
    display.println("DO NOT POWER OFF");
    display.display();
    
    // Disable watchdog during OTA
    esp_task_wdt_delete(NULL);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Complete!");
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Update Complete!");
    display.println("Rebooting...");
    display.display();
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    unsigned int percent = (progress / (total / 100));
    Serial.printf("Progress: %u%%\r", percent);
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Updating...");
    display.print(percent);
    display.println("%");
    display.display();
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    
    // Re-enable watchdog
    esp_task_wdt_add(NULL);
  });
  
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
}

void initFirebase() {
  Serial.println("Initializing Firebase...");
  
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Connecting to");
  display.println("Firebase...");
  display.display();
  
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;
  auth.user.email = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASSWORD;
  
  config.token_status_callback = tokenStatusCallback;
  config.timeout.serverResponse = FIREBASE_TIMEOUT;
  config.max_token_generation_retry = 5;
  
  fbdo.setResponseSize(4096);
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  // Wait for authentication
  unsigned long startWait = millis();
  while (!Firebase.ready() && millis() - startWait < 30000) {
    delay(100);
    esp_task_wdt_reset();  // Reset watchdog during Firebase init
  }
  
  // Check if Firebase is ready and run tasks
  if (Firebase.ready()) {
    firebaseReady = true;

    // Run API test once
    if (!hasTestedApi) {
      testAuthenticatedRequest();
      hasTestedApi = true;
    }
    
    Serial.println("Firebase Connected!");
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Firebase Ready!");
    display.display();
    delay(2000);
    

    
    // Setup presence detection (auto-offline on disconnect)
    setupPresenceDetection();
  } else {
    Serial.println("Firebase Connection Failed!");
    firebaseReady = false;
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Firebase Failed!");
    display.display();
    delay(2000);
  }
}

void setupPresenceDetection() {
  // Set initial status to "online"
  String statusPath = "/weather_station/" + String(DEVICE_ID) + "/meta/status";
  String lastSeenPath = "/weather_station/" + String(DEVICE_ID) + "/meta/last_seen";
  
  if (Firebase.RTDB.setString(&fbdo, statusPath.c_str(), "online")) {
    Serial.println("✓ Status set to: online");
    
    // Set initial last_seen timestamp
    if (Firebase.RTDB.setTimestamp(&fbdo, lastSeenPath.c_str())) {
      Serial.println("✓ Presence tracking enabled (last_seen timestamp)");
      Serial.println("  Note: Middleware will check timestamp freshness for online/offline status");
    }
  } else {
    Serial.println("✗ Failed to set online status");
  }
}

void readSensors() {
  if (sensorError) return;
  
  // Force BME280 reading
  bme.takeForcedMeasurement();
  
  // Read BME280
  float newTemp = bme.readTemperature();
  float newHum = bme.readHumidity();
  float newPres = bme.readPressure() / 100.0F;
  
  // Validate readings
  if (!isnan(newTemp) && newTemp > -40 && newTemp < 85) {
    temperature = newTemp;
  }
  if (!isnan(newHum) && newHum >= 0 && newHum <= 100) {
    humidity = newHum;
  }
  if (!isnan(newPres) && newPres > 800 && newPres < 1200) {
    pressure = newPres;
  }
  
  // Read MQ-135
  gasRaw = analogRead(MQ135_PIN);
  gasPPM = calculateGasPPM(gasRaw);
}

void calibrateMQ135() {
  Serial.println("Calibrating MQ-135... Please ensure clean air!");
  
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("CALIBRATING");
  display.println("MQ-135");
  display.println("");
  display.println("Ensure clean air!");
  display.display();
  
  delay(5000);  // Wait for sensor to stabilize
  esp_task_wdt_reset();  // Reset watchdog after delay
  
  float rs = 0;
  for (int i = 0; i < CALIBRATION_SAMPLE_TIMES; i++) {
    int adcValue = analogRead(MQ135_PIN);
    float voltage = adcValue * (3.3 / 4095.0);
    float rsTemp = ((3.3 * RL_VALUE) / voltage) - RL_VALUE;
    rs += rsTemp;
    
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("CALIBRATING");
    display.print("Sample: ");
    display.print(i + 1);
    display.print("/");
    display.println(CALIBRATION_SAMPLE_TIMES);
    
    // Show progress bar
    int barWidth = 100;
    int progress = (i * barWidth) / CALIBRATION_SAMPLE_TIMES;
    display.setCursor(0, 40);
    display.print("[");
    for (int j = 0; j < barWidth / 10; j++) {
      if (j < progress / 10) display.print("=");
      else display.print(" ");
    }
    display.print("]");
    display.display();
    
    delay(CALIBRATION_SAMPLE_INTERVAL);
    
    // Reset watchdog every 5 samples to prevent timeout
    if (i % 5 == 0) {
      esp_task_wdt_reset();
      Serial.print(".");
    }
  }
  Serial.println();
  
  rs = rs / CALIBRATION_SAMPLE_TIMES;
  Ro = rs / RO_CLEAN_AIR_FACTOR;
  
  preferences.putFloat("mq135_ro", Ro);
  
  Serial.print("Calibration complete! Ro = ");
  Serial.println(Ro);
  
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Calibration Done!");
  display.print("Ro = ");
  display.println(Ro, 2);
  display.display();
  delay(3000);
  
  esp_task_wdt_reset();  // Final reset after calibration
}

float calculateGasPPM(int adcValue) {
  float voltage = adcValue * (3.3 / 4095.0);
  
  if (voltage == 0) return 0;
  
  float rs = ((3.3 * RL_VALUE) / voltage) - RL_VALUE;
  
  if (rs < 0) rs = 0;
  
  float ratio = rs / Ro;
  
  // MQ-135 approximation for CO2 (adjust curve for other gases)
  // PPM = a * ratio^b
  // For CO2: a = 116.6020682, b = -2.769034857
  float ppm = 116.6020682 * pow(ratio, -2.769034857);
  
  return ppm;
}

void updateDisplay() {
  display.clearDisplay();
  
  // Status Bar
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  if (firebaseReady && wifiConnected) {
    display.print("ONLINE");
  } else if (wifiConnected) {
    display.print("WiFi OK");
  } else {
    display.print("OFFLINE");
  }
  
  // WiFi Signal
  display.setCursor(80, 0);
  int rssi = WiFi.RSSI();
  if (rssi > -60) display.print("||||");
  else if (rssi > -70) display.print("|||");
  else if (rssi > -80) display.print("||");
  else if (rssi > -90) display.print("|");
  else display.print("X");
  
  // Temperature (Large)
  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print(temperature, 1);
  display.setTextSize(1);
  display.print("C");
  
  // Alert Indicator
  if (gasAlertActive || tempAlertActive) {
    display.setTextSize(2);
    display.setCursor(100, 15);
    display.print("!");
  }
  
  // Details
  display.setTextSize(1);
  display.setCursor(0, 38);
  display.print("Hum: ");
  display.print(humidity, 0);
  display.println("%");
  
  display.setCursor(0, 48);
  display.print("Air: ");
  display.print(gasPPM, 0);
  display.println(" ppm");
  
  display.setCursor(0, 56);
  display.print("P:");
  display.print(pressure, 0);
  display.print("hPa");
  
  display.display();
}

void uploadLiveData() {
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready");
    firebaseReady = false;
    return;
  }
  
  firebaseReady = true;
  
  FirebaseJson json;
  String basePath = "/weather_station/" + String(DEVICE_ID);
  
  // Live sensor data
  json.set("live/temperature", temperature);
  json.set("live/humidity", humidity);
  json.set("live/pressure", pressure);
  json.set("live/gas_raw", gasRaw);
  json.set("live/gas_ppm", gasPPM);
  json.set("live/timestamp/.sv", "timestamp");  // Firebase server timestamp
  
  // Device metadata
  json.set("meta/device_id", DEVICE_ID);
  json.set("meta/firmware_version", FIRMWARE_VERSION);
  json.set("meta/status", "online");
  json.set("meta/signal_strength", WiFi.RSSI());
  json.set("meta/uptime_seconds", millis() / 1000);
  json.set("meta/free_heap", ESP.getFreeHeap());
  json.set("meta/sensor_ro", Ro);
  json.set("meta/last_seen/.sv", "timestamp");  // Update last_seen for presence detection
  
  // Alert status
  json.set("alerts/gas_alert", gasAlertActive);
  json.set("alerts/temp_alert", tempAlertActive);
  
  if (Firebase.RTDB.updateNode(&fbdo, basePath.c_str(), &json)) {
    Serial.println("✓ Live data uploaded");
  } else {
    Serial.print("✗ Upload failed: ");
    Serial.println(fbdo.errorReason());
  }
}

void storeHistoricalData() {
  if (!Firebase.ready()) return;
  
  // Store historical data with timestamp
  String historyPath = "/weather_station/" + String(DEVICE_ID) + "/history";
  
  FirebaseJson json;
  json.set("temperature", temperature);
  json.set("humidity", humidity);
  json.set("pressure", pressure);
  json.set("gas_ppm", gasPPM);
  json.set("timestamp/.sv", "timestamp");  // Firebase server timestamp
  
  // Push to history (Firebase auto-generates unique key)
  if (Firebase.RTDB.pushJSON(&fbdo, historyPath.c_str(), &json)) {
    Serial.println("✓ Historical data stored");
    
    // Optional: Clean old history (keep last 100 records)
    // This prevents unlimited growth
    cleanOldHistory();
  } else {
    Serial.print("✗ History failed: ");
    Serial.println(fbdo.errorReason());
  }
}

void cleanOldHistory() {
  // This runs once per day to clean old records
  static unsigned long lastClean = 0;
  if (millis() - lastClean < 86400000) return;  // 24 hours
  
  String historyPath = "/weather_station/" + String(DEVICE_ID) + "/history";
  
  // Query to get count
  if (Firebase.RTDB.getJSON(&fbdo, historyPath.c_str())) {
    FirebaseJson &json = fbdo.jsonObject();
    size_t count = json.iteratorBegin();
    
    // If more than 500 records, delete oldest 100
    if (count > 500) {
      Serial.println("Cleaning old history records...");
      // Implement deletion logic here if needed
    }
    
    json.iteratorEnd();
  }
  
  lastClean = millis();
}

void sendBootNotification() {
  if (!Firebase.ready()) return;
  
  String notifPath = "/weather_station/" + String(DEVICE_ID) + "/notifications";
  
  FirebaseJson json;
  json.set("type", "boot");
  json.set("message", "Device started");
  json.set("firmware", FIRMWARE_VERSION);
  json.set("timestamp/.sv", "timestamp");
  json.set("ip", WiFi.localIP().toString());
  
  if (Firebase.RTDB.pushJSON(&fbdo, notifPath.c_str(), &json)) {
    Serial.println("✓ Boot notification sent");
  }
}

void checkAlerts() {
  // Gas Leak Alert
  if (gasPPM > GAS_ALERT_THRESHOLD) {
    if (!gasAlertActive) {
      gasAlertActive = true;
      sendAlert("gas_leak", "High gas concentration detected: " + String(gasPPM, 0) + " ppm");
      Serial.println("⚠ GAS ALERT!");
    }
  } else {
    gasAlertActive = false;
  }
  
  // Temperature Alerts
  if (temperature > TEMP_HIGH_ALERT || temperature < TEMP_LOW_ALERT) {
    if (!tempAlertActive) {
      tempAlertActive = true;
      String msg = (temperature > TEMP_HIGH_ALERT) ? "High temperature" : "Low temperature";
      sendAlert("temperature", msg + ": " + String(temperature, 1) + "°C");
      Serial.println("⚠ TEMP ALERT!");
    }
  } else {
    tempAlertActive = false;
  }
  
  // Humidity Alert
  static bool humidityAlertSent = false;
  if (humidity > HUMIDITY_HIGH_ALERT || humidity < HUMIDITY_LOW_ALERT) {
    if (!humidityAlertSent) {
      String msg = (humidity > HUMIDITY_HIGH_ALERT) ? "High humidity" : "Low humidity";
      sendAlert("humidity", msg + ": " + String(humidity, 0) + "%");
      humidityAlertSent = true;
    }
  } else {
    humidityAlertSent = false;
  }
  
  // Low Pressure (Storm Warning)
  static bool pressureAlertSent = false;
  if (pressure < PRESSURE_LOW_ALERT) {
    if (!pressureAlertSent) {
      sendAlert("pressure", "Low pressure detected: " + String(pressure, 1) + " hPa - Storm warning");
      pressureAlertSent = true;
    }
  } else {
    pressureAlertSent = false;
  }
}

void sendAlert(String alertType, String message) {
  if (!Firebase.ready()) return;
  
  String alertPath = "/weather_station/" + String(DEVICE_ID) + "/alerts";
  
  FirebaseJson json;
  json.set("type", alertType);
  json.set("message", message);
  json.set("severity", "high");
  json.set("timestamp/.sv", "timestamp");
  json.set("device_id", DEVICE_ID);
  json.set("acknowledged", false);
  
  if (Firebase.RTDB.pushJSON(&fbdo, alertPath.c_str(), &json)) {
    Serial.println("✓ Alert sent: " + alertType);
  }
}

void checkFirebaseCommands() {
  if (!Firebase.ready()) return;
  
  String commandPath = "/weather_station/" + String(DEVICE_ID) + "/commands";
  
  // Read commands
  if (Firebase.RTDB.getJSON(&fbdo, commandPath.c_str())) {
    if (fbdo.dataType() == "json") {
      FirebaseJson &json = fbdo.jsonObject();
      FirebaseJsonData result;
      
      // Check for calibration command
      if (json.get(result, "calibrate")) {
        if ((result.type == "bool" || result.type == "boolean") && result.boolValue == true) {
          Serial.println("📡 Remote calibration requested");
          
          display.clearDisplay();
          display.setCursor(0, 20);
          display.println("Remote Command:");
          display.println("Calibrating...");
          display.display();
          
          calibrateMQ135();
          
          // Clear the command
          Firebase.RTDB.setBool(&fbdo, (commandPath + "/calibrate").c_str(), false);
          
          // Send confirmation
          FirebaseJson confirmJson;
          confirmJson.set("type", "calibration_complete");
          confirmJson.set("message", "Calibration completed. Ro = " + String(Ro, 2));
          confirmJson.set("timestamp/.sv", "timestamp");
          Firebase.RTDB.pushJSON(&fbdo, 
            ("/weather_station/" + String(DEVICE_ID) + "/notifications").c_str(), 
            &confirmJson);
        }
      }
      
      // Check for reboot command
      if (json.get(result, "reboot")) {
        if ((result.type == "bool" || result.type == "boolean") && result.boolValue == true) {
          Serial.println("📡 Remote reboot requested");
          
          display.clearDisplay();
          display.setCursor(0, 20);
          display.println("Remote Command:");
          display.println("Rebooting...");
          display.display();
        
        // Clear the command first
        Firebase.RTDB.setBool(&fbdo, (commandPath + "/reboot").c_str(), false);
        
        delay(1000);
        ESP.restart();
      }
    }
    
      // Check for reset command (clear preferences)
      if (json.get(result, "reset_preferences")) {
        if ((result.type == "bool" || result.type == "boolean") && result.boolValue == true) {
          Serial.println("📡 Reset preferences requested");
          
          preferences.clear();
          preferences.putBool("force_calib", true);
          
          // Clear the command
          Firebase.RTDB.setBool(&fbdo, (commandPath + "/reset_preferences").c_str(), false);
          
          Serial.println("Preferences cleared. Rebooting...");
          delay(1000);
          ESP.restart();
        }
      }
      
      // Check for display test command
      if (json.get(result, "test_display")) {
        if ((result.type == "bool" || result.type == "boolean") && result.boolValue == true) {
          Serial.println("📡 Display test requested");
          
          display.clearDisplay();
          display.setTextSize(2);
          display.setCursor(10, 20);
          display.println("TEST OK!");
          display.display();
          delay(3000);
          // Clear the command
          Firebase.RTDB.setBool(&fbdo, (commandPath + "/test_display").c_str(), false);
        }
      }
    }
  } else {
    Serial.print("Error reading commands: ");
    Serial.println(fbdo.errorReason());
  }
}

void testAuthenticatedRequest() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    Serial.println("Cannot test API: WiFi or Firebase not ready");
    return;
  }

  Serial.println("\n--- Testing Authenticated API Request ---");
  
  // Get the current ID token from the Firebase object
  // Note: This token is automatically refreshed by the library
  String idToken = Firebase.getToken();
  
  if (idToken.length() == 0) {
    Serial.println("Error: No ID token available yet");
    return;
  }
  
  HTTPClient http;
  
  Serial.print("Requesting: ");
  Serial.println(API_URL);
  
  http.begin(API_URL);
  http.addHeader("Authorization", "Bearer " + idToken);
  
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    Serial.printf("HTTP Code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Response: " + payload);
      Serial.println("✓ Authenticated API access successful!");
    } else {
      Serial.printf("✗ API request failed: %s\n", http.errorToString(httpCode).c_str());
    }
  } else {
    Serial.printf("✗ Connection failed: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
  Serial.println("-----------------------------------------\n");
}
