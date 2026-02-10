# ESP32 Weather Station v2.1.1

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

### 2. Configure Credentials

1. Create `secrets.h` in the project root (this file is gitignored)
2. Add your credentials:

```cpp
// secrets.h
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
#define FIREBASE_API_KEY "your_firebase_api_key"
#define FIREBASE_DATABASE_URL "your_database_url"
#define FIREBASE_USER_EMAIL "your_email"
#define FIREBASE_USER_PASSWORD "your_password"
```

### 3. Flash Firmware

1. Open `ESP32_WeatherStation_v2.ino` in Arduino IDE
2. Include `secrets.h` at the top of the file
3. Upload to ESP32

### 4. Deploy Cloud Functions

```bash
npm install -g firebase-tools
npx firebase login
npx firebase deploy --only functions
```

## 📡 API Endpoints

**Base URL:** `https://YOUR_REGION-YOUR_PROJECT_ID.cloudfunctions.net/api`

> **🔒 Note:** All endpoints require Firebase Authentication. See Security section below.

### Get Live Data

```bash
curl https://YOUR_REGION-YOUR_PROJECT_ID.cloudfunctions.net/api/live \
  -H "Authorization: Bearer YOUR_ID_TOKEN"
```

### Get History

```bash
curl https://YOUR_REGION-YOUR_PROJECT_ID.cloudfunctions.net/api/history \
  -H "Authorization: Bearer YOUR_ID_TOKEN"
```

### Send Command

```bash
curl -X POST https://YOUR_REGION-YOUR_PROJECT_ID.cloudfunctions.net/api/command \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ID_TOKEN" \
  -d '{"action": "reboot"}'
```

**Available commands:** `calibrate`, `reboot`, `reset_preferences`, `test_display`

## 🗂️ Project Structure

```
ESP32_WeatherStation_v2/
├── ESP32_WeatherStation_v2.ino    # Main firmware
├── secrets.h                       # Credentials (gitignored)
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
/weather_station/DEVICE_ID/
├── live/                    # Latest sensor readings
├── meta/                    # Device metadata + last_seen
├── history/                 # Historical data (push entries)
├── alerts/                  # Alert events
├── notifications/           # Boot/calibration events
└── commands/                # Remote commands (polled every 5s)
```

## 🛡️ Security Implementation

### 1. Protect Credentials

**Create `secrets.h`** (already in `.gitignore`):

```cpp
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
#define FIREBASE_API_KEY "your_api_key"
#define FIREBASE_DATABASE_URL "https://your-project.firebasedatabase.app"
#define FIREBASE_USER_EMAIL "device@yourproject.com"
#define FIREBASE_USER_PASSWORD "secure_password"
```

### 2. Add API Authentication (Optional but Recommended)

To secure the Cloud Functions API, add Firebase Auth middleware:

```javascript
// functions/index.js - Add before route definitions
const authenticate = async (req, res, next) => {
  const authHeader = req.headers.authorization;

  if (!authHeader?.startsWith("Bearer ")) {
    return res.status(401).json({ error: "Unauthorized" });
  }

  try {
    const idToken = authHeader.split("Bearer ")[1];
    const decodedToken = await admin.auth().verifyIdToken(idToken);
    req.user = decodedToken;
    next();
  } catch (error) {
    return res.status(401).json({ error: "Invalid token" });
  }
};

// Apply to protected routes
app.get("/live", authenticate, async (req, res) => {
  /* ... */
});
```

### 3. Restrict CORS Origins

Replace in `functions/index.js`:

```javascript
app.use(
  cors({
    origin: ["https://yourdomain.com"],
    credentials: true,
  }),
);
```

### 4. Enable Firebase App Check

1. Go to Firebase Console → App Check
2. Enable for your project
3. Add enforcement for Cloud Functions

## 📝 License

MIT

## 👤 Author

**Abhishek Kumar**

---

**Firmware Version:** 2.1.1  
**Last Updated:** February 10, 2026
