/*
 * ESP32 Weather Station v2.1.2
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

#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DoubleResetDetector.h>

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
// - OTA_PASSWORD

// Firmware Info
// Firmware Info
const char* FIRMWARE_VERSION = "3.0.0";


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

// NEW: v3.0.0 Global Infrastructure
TaskHandle_t NetworkTask;
TaskHandle_t SensorTask;
SemaphoreHandle_t sensorDataMutex;

// Safe Mode
#define DRD_TIMEOUT 2.0
#define DRD_ADDRESS 0
DoubleResetDetector* drd;
bool isSafeMode = false;

// WiFiManager & Config
WiFiManager wm;
char fb_api_key[64];
char tg_bot_token[64];
char tg_chat_id[20];

// NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 5.5 * 3600, 60000); // Default to +5:30

// Telegram
WiFiClientSecure tgClient;
UniversalTelegramBot bot(tg_bot_token, tgClient);

// UI & Logic
bool isWarmingUp = true;
unsigned long warmupStart = 0;
int currentDisplayPage = 0;
#define MAX_PAGES 3
#define TOUCH_PIN 15
#define TOUCH_THRESHOLD 40
bool displayDimmed = false;
unsigned long lastActivity = 0;
bool sensorError = false;
bool hasTestedApi = false;
int reconnectAttempts = 0;

// Alert Flags
bool gasAlertActive = false;
bool tempAlertActive = false;

// Functions
void initHardware();
void setupWiFiManager();
void setupOTA();
void initFirebase();
void setupPresenceDetection();
void readSensors();
void calibrateMQ135();
float calculateGasPPM(int adcValue, float temp, float hum);
void updateDisplay();
void uploadLiveData();
void storeHistoricalData();
void sendBootNotification();
void checkAlerts();
void sendAlert(String alertType, String message);
void checkFirebaseCommands();
void testAuthenticatedRequest();
void logVersionHistory();
void networkTaskImpl(void* _);
void sensorTaskImpl(void* _);
void handleTelegramCommands();
void checkTouchSensor();
void updateWarmup();
void saveConfigCallback();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n\n=== ESP32 Weather Station v3.0.0 Mega Update ==="));
  
  // 1. Initialize Mutex for thread-safe sensor data access
  sensorDataMutex = xSemaphoreCreateBinary();
  if (sensorDataMutex != NULL) xSemaphoreGive(sensorDataMutex);
  
  // 2. Double Reset Detector (Safe Mode)
  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
  if (drd->detectDoubleReset()) {
    Serial.println("!!! SAFE MODE DETECTED !!!");
    isSafeMode = true;
  }
  
  // 3. Status LED
  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, HIGH); // On during setup
  
  // 4. Preferences
  preferences.begin("weather", false);
  Ro = preferences.getFloat("mq135_ro", 10.0);
  
  // 5. Initialize Hardware (OLED, Sensors)
  initHardware();
  
  // 6. WiFiManager (Non-blocking usually, blocking in Safe Mode)
  setupWiFiManager();
  
  // 7. Initialize Warmup Timer
  warmupStart = millis();
  isWarmingUp = true;
  
  // 8. Create FreeRTOS Tasks
  // Core 0: Networking (WiFi, Firebase, Telegram, NTP, OTA)
  xTaskCreatePinnedToCore(
    networkTaskImpl,
    "NetworkTask",
    10000,      // Stack size
    NULL,
    1,          // Priority
    &NetworkTask,
    0           // Core 0
  );

  // Core 1: Sensors and UI (Sampling, OLED, Touch)
  xTaskCreatePinnedToCore(
    sensorTaskImpl,
    "SensorTask",
    8000,       // Stack size
    NULL,
    1,          // Priority
    &SensorTask,
    1           // Core 1
  );

  Serial.println("Multitasking kernel started. Free heap: " + String(ESP.getFreeHeap()));
  digitalWrite(LED_STATUS, LOW);
}

void loop() {
  drd->loop();
  delay(10);
}

void saveConfigCallback() {
  Serial.println("WiFiManager config needs saving...");
  // Update internal variables from the parameters (handled in setupWiFiManager usually)
}

void setupWiFiManager() {
  // Load existing values from preferences
  preferences.begin("weather", false);
  preferences.getString("fb_api_key", fb_api_key, 64);
  preferences.getString("tg_token", tg_bot_token, 64);
  preferences.getString("tg_chat", tg_chat_id, 20);
  preferences.end();

  // Add custom parameters to WiFiManager
  WiFiManagerParameter custom_fb_key("fb_key", "Firebase API Key", fb_api_key, 64);
  WiFiManagerParameter custom_tg_token("tg_token", "Telegram Bot Token", tg_bot_token, 64);
  WiFiManagerParameter custom_tg_chat("tg_chat", "Telegram Chat ID", tg_chat_id, 20);

  wm.addParameter(&custom_fb_key);
  wm.addParameter(&custom_tg_token);
  wm.addParameter(&custom_tg_chat);
  
  wm.setSaveConfigCallback(saveConfigCallback);
  
  if (isSafeMode) {
    wm.setConfigPortalBlocking(true);
    wm.startConfigPortal("WS_Safe_Mode", "ota@admin");
  } else {
    wm.setConfigPortalBlocking(false);
    if (!wm.autoConnect("WeatherStation_Setup")) {
      Serial.println("Failed to connect, portal running...");
    }
  }

  // After portal exits (or connects), save parameters if updated
  if (wm.getConfigPortalActive()) {
     // If we were in safe mode or config mode, the params were updated
     strncpy(fb_api_key, custom_fb_key.getValue(), 64);
     strncpy(tg_bot_token, custom_tg_token.getValue(), 64);
     strncpy(tg_chat_id, custom_tg_chat.getValue(), 20);

     preferences.begin("weather", false);
     preferences.putString("fb_api_key", fb_api_key);
     preferences.putString("tg_token", tg_bot_token);
     preferences.putString("tg_chat", tg_chat_id);
     preferences.end();
     
     bot.updateToken(tg_bot_token);
  }
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
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
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
  
  config.api_key = (strlen(fb_api_key) > 10) ? fb_api_key : FIREBASE_API_KEY;
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
  
  // Read local variables first to minimize Mutex hold time
  float newTemp = bme.readTemperature();
  float newHum = bme.readHumidity();
  float newPres = bme.readPressure() / 100.0F;
  int newGasRaw = analogRead(MQ135_PIN);
  
  // Thread Safety: Protect shared data
  if (xSemaphoreTake(sensorDataMutex, portMAX_DELAY) == pdTRUE) {
    // Validate and update BME280 data
    if (!isnan(newTemp) && newTemp > -40 && newTemp < 85) temperature = newTemp;
    if (!isnan(newHum) && newHum >= 0 && newHum <= 100) humidity = newHum;
    if (!isnan(newPres) && newPres > 800 && newPres < 1200) pressure = newPres;
    
    gasRaw = newGasRaw;
    gasPPM = calculateGasPPM(gasRaw, temperature, humidity);
    
    xSemaphoreGive(sensorDataMutex);
  }
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

float calculateGasPPM(int adcValue, float temp, float hum) {
  float voltage = adcValue * (3.3 / 4095.0);
  if (voltage <= 0) return 0.0;
  
  float rs = ((3.3 * RL_VALUE) / voltage) - RL_VALUE;
  
  // Environmental Compensation Factor (approx. from MQ-135 datasheet)
  // Base is 20°C and 33% Humidity
  float correctionFactor = 1.0 + (temp - 20.0) * (-0.005) + (hum - 33.0) * (-0.002);
  float correctedRs = rs / correctionFactor;
  
  float ratio = correctedRs / Ro;
  
  // MQ-135 approximation for CO2 (adjust curve for other gases)
  // PPM = a * ratio^b
  // For CO2: a = 116.6020682, b = -2.769034857
  return 116.6020682 * pow(ratio, -2.769034857);
}

void updateDisplay() {
  display.clearDisplay();
  
  // Status Bar (Always visible)
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (isSafeMode) display.print("SAFE");
  else if (firebaseReady && wifiConnected) display.print("ONLINE");
  else if (wifiConnected) display.print("WiFi OK");
  else display.print("OFFLINE");
  
  display.setCursor(80, 0);
  int rssi = WiFi.RSSI();
  if (rssi > -60) display.print("||||");
  else if (rssi > -70) display.print("|||");
  else if (rssi > -80) display.print("||");
  else if (rssi > -90) display.print("|");
  else display.print("X");

  // OLED Dimming Control
  if (millis() - lastActivity > 300000) display.dim(true); 
  else display.dim(false);

  // MQ-135 Warmup State
  if (isWarmingUp) {
    display.setCursor(0, 15);
    display.print("SENSORS WARMING...");
    long elapsed = (millis() - warmupStart) / 1000;
    int remaining = 180 - elapsed;
    if (remaining < 0) remaining = 0;
    
    display.setCursor(0, 30);
    display.print("Wait: ");
    display.print(remaining);
    display.print("s");
    
    display.drawRect(0, 45, 128, 8, WHITE);
    display.fillRect(0, 45, (elapsed * 128) / 180, 8, WHITE);
    display.display();
    return;
  }

  // Shared Data Protection
  if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    switch(currentDisplayPage) {
      case 0: // Main Weather
        display.setTextSize(2);
        display.setCursor(0, 15);
        display.print(temperature, 1);
        display.setTextSize(1);
        display.print("C  ");
        display.setTextSize(2);
        display.print(humidity, 0);
        display.setTextSize(1);
        display.print("%");
        
        display.setCursor(0, 40);
        display.print("Air: ");
        display.print(gasPPM, 0);
        display.print(" ppm");
        break;
        
      case 1: // Detailed Air
        display.setCursor(0, 15);
        display.print("AIR QUALITY DATA");
        display.setCursor(0, 28);
        display.print("Raw AD: "); display.println(gasRaw);
        display.print("PPM:    "); display.println(gasPPM, 1);
        display.print("Ro:     "); display.println(Ro, 1);
        break;
        
      case 2: // System State
        display.setCursor(0, 15);
        display.print("SYSTEM: v"); display.println(FIRMWARE_VERSION);
        display.print("IP: "); display.println(WiFi.localIP().toString());
        display.print("UP: "); display.println(millis()/60000);
        display.print("Time: "); display.println(timeClient.getFormattedTime());
        break;
    }
    xSemaphoreGive(sensorDataMutex);
  }

  // Alert Indicator
  if (gasAlertActive || tempAlertActive) {
    display.setTextSize(2);
    display.setCursor(110, 15);
    display.print("!");
  }
  
  display.display();
}

void uploadLiveData() {
  if (!firebaseReady || !wifiConnected) return;
  
  FirebaseJson json;
  String basePath = "/weather_station/" + String(DEVICE_ID);
  
  if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    json.set("live/temperature", temperature);
    json.set("live/humidity", humidity);
    json.set("live/pressure", pressure);
    json.set("live/gas_raw", gasRaw);
    json.set("live/gas_ppm", gasPPM);
    xSemaphoreGive(sensorDataMutex);
  }
  
  json.set("live/timestamp/.sv", "timestamp");
  json.set("meta/device_id", DEVICE_ID);
  json.set("meta/firmware_version", FIRMWARE_VERSION);
  json.set("meta/signal_strength", WiFi.RSSI());
  json.set("meta/uptime_seconds", millis() / 1000);
  json.set("meta/free_heap", ESP.getFreeHeap());
  json.set("meta/warming_up", isWarmingUp);
  
  json.set("alerts/gas_alert", gasAlertActive);
  json.set("alerts/temp_alert", tempAlertActive);
  
  if (Firebase.RTDB.updateNode(&fbdo, basePath.c_str(), &json)) {
    Serial.println("✓ Live data uploaded");
  } else {
    Serial.println("✗ Upload failed: " + fbdo.errorReason());
  }
}

void storeHistoricalData() {
  if (!firebaseReady || !wifiConnected) return;
  
  String historyPath = "/weather_station/" + String(DEVICE_ID) + "/history";
  FirebaseJson json;
  
  if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    json.set("temperature", temperature);
    json.set("humidity", humidity);
    json.set("pressure", pressure);
    json.set("gas_ppm", gasPPM);
    xSemaphoreGive(sensorDataMutex);
  } else {
    return; // Skip if sensors busy
  }
  
  json.set("timestamp/.sv", "timestamp");
  
  if (Firebase.RTDB.pushJSON(&fbdo, historyPath.c_str(), &json)) {
    Serial.println("✓ Historical data stored");
    cleanOldHistory();
  } else {
    Serial.println("✗ History failed: " + fbdo.errorReason());
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
  if (!firebaseReady || !wifiConnected) return;
  
  String notifPath = "/weather_station/" + String(DEVICE_ID) + "/notifications";
  
  FirebaseJson json;
  json.set("type", "boot");
  json.set("message", isSafeMode ? "Device started in SAFE MODE" : "Device started normally");
  json.set("firmware", FIRMWARE_VERSION);
  json.set("timestamp/.sv", "timestamp");
  json.set("ip", WiFi.localIP().toString());
  
  if (Firebase.RTDB.pushJSON(&fbdo, notifPath.c_str(), &json)) {
    Serial.println("✓ Boot notification sent");
  } else {
    Serial.println("✗ Boot notification failed: " + fbdo.errorReason());
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

void logVersionHistory() {
  if (!Firebase.ready()) return;
  
  // Use underscore instead of dot for Firebase keys if needed, but here it's a path component
  String historyPath = "/weather_station/" + String(DEVICE_ID) + "/version_history/" + String(FIRMWARE_VERSION);
  historyPath.replace(".", "_");
  
  FirebaseJson json;
  if (String(FIRMWARE_VERSION) == "2.1.3") {
    json.set("added", "WiFi credentials restore logic, version history tracking in database");
    json.set("removed", "Hardcoded secrets in main sketch (moved to secrets.h)");
  } else {
    json.set("added", "General maintenance");
    json.set("removed", "None");
  }
  json.set("timestamp/.sv", "timestamp");
  
  if (Firebase.RTDB.setJSON(&fbdo, historyPath.c_str(), &json)) {
    Serial.println("✓ Version history logged to database");
  } else {
    Serial.print("✗ Version history failed: ");
    Serial.println(fbdo.errorReason());
  }
}
// --- FreeRTOS Tasks ---

void networkTaskImpl(void* _) {
  Serial.println("Network Task started on Core 0");
  
  tgClient.setInsecure(); // Use insecure for Telegram to simplify (or provide certificate)
  
  bool apiTested = false;
  bool verLogged = false;

  for (;;) {
    // 1. Process WiFiManager Portal (Non-blocking)
    wm.process();
    
    // 2. Handle OTA
    ArduinoOTA.handle();
    
    // 3. NTP & Time Update
    if (WiFi.status() == WL_CONNECTED) {
      if (!wifiConnected) {
        Serial.println("WiFi Connected! Initializing network services...");
        initFirebase();
        setupOTA();
        if (!timeClient.isTimeSet()) timeClient.begin();
      }
      timeClient.update();
      wifiConnected = true;
    } else {
      wifiConnected = false;
    }
    
    // 4. Firebase Logic
    if (Firebase.ready() && wifiConnected) {
      firebaseReady = true;
      
      // Initial tasks
      if (!verLogged) {
        logVersionHistory();
        verLogged = true;
      }
      if (!apiTested) {
        testAuthenticatedRequest();
        apiTested = true;
      }

      // Periodics
      static unsigned long lastCheck = 0;
      if (millis() - lastCheck > 5000) {
        checkFirebaseCommands();
        lastCheck = millis();
      }

      static unsigned long lastUp = 0;
      if (millis() - lastUp > UPLOAD_INTERVAL) {
        uploadLiveData();
        lastUp = millis();
      }

      static unsigned long lastHist = 0;
      if (millis() - lastHist > HISTORY_INTERVAL) {
        storeHistoricalData();
        lastHist = millis();
      }
    } else {
      firebaseReady = false;
    }
    
    // 5. Telegram
    handleTelegramCommands();

    vTaskDelay(pdMS_TO_TICKS(100)); // Yield to OS
  }
}

void sensorTaskImpl(void* _) {
  Serial.println("Sensor Task started on Core 1");
  
  for (;;) {
    // 1. Read Sensors (Mutex protected inside)
    readSensors();
    
    // 2. System Logic
    updateWarmup();
    checkTouchSensor();
    checkAlerts();
    
    // 3. Update UI
    static unsigned long lastDisp = 0;
    if (millis() - lastDisp > DISPLAY_UPDATE) {
      updateDisplay();
      lastDisp = millis();
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// --- Utility Functions ---

void handleTelegramCommands() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 2000) return; // Rate limit
  lastCheck = millis();

  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  while (numNewMessages) {
    String chat_id = String(bot.messages[0].chat_id);
    String text = bot.messages[0].text;
    String from_name = bot.messages[0].from_name;

    if (text == "/status") {
      String msg = "🌤 *ESP32 WS v3.0.0*\n";
      if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        msg += "Temp: " + String(temperature, 1) + "°C\n";
        msg += "Hum: " + String(humidity, 0) + "%\n";
        msg += "Air Q: " + String(gasPPM, 0) + " ppm\n";
        xSemaphoreGive(sensorDataMutex);
      }
      msg += "Uptime: " + String(millis()/60000) + " min";
      bot.sendMessage(chat_id, msg, "Markdown");
    } 
    else if (text == "/reboot") {
      bot.sendMessage(chat_id, "Rebooting system...", "");
      delay(1000);
      ESP.restart();
    }
    
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}

void checkTouchSensor() {
  // Using Capacitive Touch or Digital Input
  if (touchRead(TOUCH_PIN) < TOUCH_THRESHOLD) {
    currentDisplayPage = (currentDisplayPage + 1) % MAX_PAGES;
    lastActivity = millis();
    displayDimmed = false;
    Serial.print("Touch! Switching to page ");
    Serial.println(currentDisplayPage);
    delay(200); // Simple debounce
  }
}

void updateWarmup() {
  if (isWarmingUp) {
    if (millis() - warmupStart > 180000) {
      isWarmingUp = false;
      Serial.println("MQ-135 Warmup complete.");
    }
  }
}
