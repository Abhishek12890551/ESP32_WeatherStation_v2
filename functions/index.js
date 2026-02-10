/**
 * Firebase Cloud Functions — ESP32 Weather Station Middleware
 *
 * Exposes a secure REST API so clients never touch RTDB directly.
 * Also runs a weekly scheduled cleanup of stale history records.
 *
 * Endpoints:
 *   GET  /live      → latest sensor data + derived air_quality_status
 *   GET  /history   → last 50 historical records (newest first)
 *   POST /command   → write a validated command to the device
 */

const functions = require("firebase-functions");
const admin = require("firebase-admin");
const express = require("express");
const cors = require("cors");

// ─── Firebase Admin SDK ──────────────────────────────────────────────
admin.initializeApp();
const db = admin.database();

// ─── Constants ───────────────────────────────────────────────────────
const DEVICE_ID = "ESP32_WS_001";
const BASE_PATH = `/weather_station/${DEVICE_ID}`;
const GAS_PPM_THRESHOLD = 300; // same as firmware GAS_ALERT_THRESHOLD
const ALLOWED_COMMANDS = ["calibrate", "reboot", "reset_preferences", "test_display"];
const HISTORY_RETENTION_DAYS = 30;

// ─── Express App ─────────────────────────────────────────────────────
const app = express();
app.use(cors({ origin: true })); // restrict origin in production
app.use(express.json());

// ──────────────────────────────────────────────────────────────────────
// GET /live
// Returns the latest sensor data with a derived air_quality_status field.
// ──────────────────────────────────────────────────────────────────────
app.get("/live", async (req, res) => {
  try {
    const [liveSnap, metaSnap] = await Promise.all([
      db.ref(`${BASE_PATH}/live`).once("value"),
      db.ref(`${BASE_PATH}/meta`).once("value"),
    ]);

    const live = liveSnap.val();
    const meta = metaSnap.val();

    if (!live) {
      return res.status(404).json({ error: "No live data available" });
    }

    // Derive air quality status from gas_ppm (matches firmware threshold)
    const gasPpm = live.gas_ppm || 0;
    const airQualityStatus = gasPpm > GAS_PPM_THRESHOLD ? "POOR" : "GOOD";

    // Check data freshness for presence detection
    const lastSeen = meta?.last_seen || 0;
    const dataAge = Date.now() - lastSeen;
    const isOnline = dataAge < 30000; // 30 seconds threshold

    return res.status(200).json({
      data: {
        ...live,
        air_quality_status: airQualityStatus,
      },
      meta: {
        ...meta,
        status: isOnline ? "online" : "offline", // Override based on freshness
        data_age_seconds: Math.floor(dataAge / 1000),
      },
    });
  } catch (err) {
    console.error("GET /live error:", err);
    return res.status(500).json({ error: "Internal server error" });
  }
});

// ──────────────────────────────────────────────────────────────────────
// GET /history
// Returns the last 50 historical records, sorted newest-first.
// ──────────────────────────────────────────────────────────────────────
app.get("/history", async (req, res) => {
  try {
    const snap = await db
      .ref(`${BASE_PATH}/history`)
      .orderByChild("timestamp")
      .limitToLast(50)
      .once("value");

    const raw = snap.val();

    if (!raw) {
      return res.status(200).json({ data: [], count: 0 });
    }

    // Convert Firebase object to array and sort newest-first
    const records = Object.entries(raw)
      .map(([key, value]) => ({ id: key, ...value }))
      .sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));

    return res.status(200).json({ data: records, count: records.length });
  } catch (err) {
    console.error("GET /history error:", err);
    return res.status(500).json({ error: "Internal server error" });
  }
});

// ──────────────────────────────────────────────────────────────────────
// POST /command
// Writes a validated command to the device's commands node.
// Body: { "action": "reboot" }
// ──────────────────────────────────────────────────────────────────────
app.post("/command", async (req, res) => {
  try {
    const { action } = req.body;

    // Validate presence
    if (!action) {
      return res.status(400).json({
        error: "Missing 'action' field in request body",
        allowed: ALLOWED_COMMANDS,
      });
    }

    // Validate against allowlist
    if (!ALLOWED_COMMANDS.includes(action)) {
      return res.status(400).json({
        error: `Invalid action: '${action}'`,
        allowed: ALLOWED_COMMANDS,
      });
    }

    // Write command to RTDB — the ESP32 polls this node every 5 seconds
    await db.ref(`${BASE_PATH}/commands`).update({ [action]: true });

    console.log(`Command '${action}' sent to device ${DEVICE_ID}`);

    return res.status(200).json({
      success: true,
      message: `Command '${action}' sent to device ${DEVICE_ID}`,
      device_id: DEVICE_ID,
      action: action,
      timestamp: new Date().toISOString(),
    });
  } catch (err) {
    console.error("POST /command error:", err);
    return res.status(500).json({ error: "Internal server error" });
  }
});

// ─── Export Express app as a Cloud Function ──────────────────────────
exports.api = functions.https.onRequest(app);

// ──────────────────────────────────────────────────────────────────────
// SCHEDULED: cleanupHistory
// Runs every Sunday at midnight UTC.
// Deletes history entries older than 30 days.
// ──────────────────────────────────────────────────────────────────────
exports.cleanupHistory = functions.pubsub
  .schedule("every sunday 00:00")
  .timeZone("UTC")
  .onRun(async () => {
    const cutoff = Date.now() - HISTORY_RETENTION_DAYS * 24 * 60 * 60 * 1000;

    console.log(
      `[cleanupHistory] Running cleanup. Deleting records older than ${new Date(cutoff).toISOString()}`
    );

    try {
      const snap = await db
        .ref(`${BASE_PATH}/history`)
        .orderByChild("timestamp")
        .endAt(cutoff)
        .once("value");

      const staleEntries = snap.val();

      if (!staleEntries) {
        console.log("[cleanupHistory] No stale records found.");
        return null;
      }

      // Build a single batched update that nullifies each stale key
      const updates = {};
      Object.keys(staleEntries).forEach((key) => {
        updates[`${BASE_PATH}/history/${key}`] = null;
      });

      const deletedCount = Object.keys(updates).length;
      await db.ref().update(updates);

      console.log(`[cleanupHistory] Deleted ${deletedCount} records.`);
      return null;
    } catch (err) {
      console.error("[cleanupHistory] Error:", err);
      return null;
    }
  });
