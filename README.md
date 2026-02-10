# ESP32 Weather Station v2.1.0

A production-ready IoT weather monitoring system with Firebase Cloud Functions middleware, real-time data streaming, and presence detection.

## 🌡️ Features

### Hardware

- **ESP32** microcontroller
- **BME280** sensor (temperature, humidity, pressure)
- **MQ-135** gas sensor (air quality monitoring)
- **SSD1306 OLED** display (128x64)

### Firmware Capabilities

- ✅ Real-time sensor data upload (every 10 seconds)
- ✅ Historical data storage (every 5 minutes)
- ✅ OTA (Over-The-Air) firmware updates
- ✅ Watchdog timer & error recovery
- ✅ Remote command support (reboot, calibrate, etc.)
- ✅ Alert system (gas leak, extreme weather)
- ✅ Presence detection via `last_seen` timestamp
- ✅ WiFi auto-reconnect

### Cloud Functions Middleware

- ✅ **GET /live** — Latest sensor data + derived air quality status
- ✅ **GET /history** — Last 50 historical records
- ✅ **POST /command** — Send commands to the device
- ✅ **Scheduled cleanup** — Auto-delete history older than 30 days (every Sunday)
- ✅ **Presence detection** — Accurate online/offline status based on data freshness

## 🚀 Quick Start

### 1. Hardware Setup

Connect the sensors to your ESP32:

- BME280: I2C (SDA: GPIO21, SCL: GPIO22)
- MQ-135: Analog (GPIO34)
- OLED: I2C (0x3C)

### 2. Flash Firmware

1. Open `ESP32_WeatherStation_v2.ino` in Arduino IDE
2. Update WiFi credentials (lines 65-66)
3. Update Firebase credentials (lines 69-72)
4. Upload to ESP32

### 3. Deploy Cloud Functions

```bash
cd d:\ESP32_WeatherStation_v2
npm install -g firebase-tools
npx firebase login
npx firebase deploy --only functions
```

## 📡 API Endpoints

**Base URL:** `https://us-central1-esp32-weather-station-2508e.cloudfunctions.net/api`

### Get Live Data

```bash
curl https://us-central1-esp32-weather-station-2508e.cloudfunctions.net/api/live
```

### Get History

```bash
curl https://us-central1-esp32-weather-station-2508e.cloudfunctions.net/api/history
```

### Send Command (PowerShell)

```powershell
curl -X POST https://us-central1-esp32-weather-station-2508e.cloudfunctions.net/api/command `
  -H "Content-Type: application/json" `
  -Body '{"action": "reboot"}'
```

**Available commands:** `calibrate`, `reboot`, `reset_preferences`, `test_display`

## 🗂️ Project Structure

```
ESP32_WeatherStation_v2/
├── ESP32_WeatherStation_v2.ino    # Main firmware
├── functions/
│   ├── index.js                    # Cloud Functions (Express API + scheduler)
│   ├── package.json
│   └── .eslintrc.js
├── firebase.json                   # Firebase config
└── .firebaserc                     # Firebase project alias
```

## 🔧 Configuration

### Firmware Constants

- `UPLOAD_INTERVAL`: 10 seconds (live data)
- `HISTORY_INTERVAL`: 5 minutes (historical data)
- `GAS_ALERT_THRESHOLD`: 300 ppm
- `WATCHDOG_TIMEOUT`: 30 seconds

### Middleware

- **Presence threshold**: 30 seconds (data older than this = offline)
- **History retention**: 30 days
- **Cleanup schedule**: Every Sunday at midnight UTC

## 📊 Firebase RTDB Structure

```
/weather_station/ESP32_WS_001/
├── live/                    # Latest sensor readings
├── meta/                    # Device metadata + last_seen
├── history/                 # Historical data (push entries)
├── alerts/                  # Alert events
├── notifications/           # Boot/calibration events
└── commands/                # Remote commands (polled every 5s)
```

## 🛡️ Security Notes

> **⚠️ IMPORTANT:** The firmware currently contains hardcoded credentials. For production:
>
> 1. Move WiFi/Firebase credentials to a separate config file
> 2. Add `.gitignore` entry for credentials
> 3. Use Firebase App Check for API security
> 4. Restrict CORS origins in Cloud Functions

## 📝 License

MIT

## 👤 Author

**Mintu** (mintu12890551@gmail.com)

---

**Firmware Version:** 2.1.0  
**Last Updated:** February 10, 2026
