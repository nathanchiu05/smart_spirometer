// ================================================================
// Smart Spirometer — ESP32 firmware
// VL53L0X piston tracking + web app (headless — no OLED)
//
// NETWORKING: Access Point ONLY. The device hosts its own WiFi
// network and is reachable only through it — it never joins a home
// or public network.
//
//   Network:  SmartSpirometer
//   Password: breathe123
//   URL:      http://192.168.4.1
//
// Hardware:
//   ESP32 dev board
//   VL53L0X ToF sensor, socketed directly into the board:
//     3V3, GND, GPIO 15, GPIO 2 (I2C orientation auto-detected)
//   AirLife 4000mL incentive spirometer
//
// NOTE: GPIO 2 is tied to the board's status LED, so the I2C bus is
// LOCKED to 100 kHz. Do NOT raise it to 400 kHz — higher speeds cause
// signal degradation and timeout errors.
//
// Libraries (Library Manager):
//   Adafruit_VL53L0X,
//   ESP Async WebServer (ESP32Async), Async TCP (ESP32Async), ArduinoJson
// ================================================================

#include <Wire.h>
#include "Adafruit_VL53L0X.h"

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "webpage.h"   // INDEX_HTML lives here

// ---- VL53L0X socketed pins (orientation auto-detected in setup) ----
#define PIN_A 15
#define PIN_B 2

Adafruit_VL53L0X lox = Adafruit_VL53L0X();

// Optional status LED (set to a free GPIO if you ever wire one).
const int LED_PIN    = -1;

void setLed(bool on) { if (LED_PIN >= 0) digitalWrite(LED_PIN, on ? HIGH : LOW); }

// ================================================================
// WiFi — Access Point only
// ================================================================
// The device hosts its own network and is reachable ONLY through it.
// There is no station/client mode: the ESP32 never joins a home or
// public WiFi, so it is never exposed to a shared network and the
// address is always the same.
//
//   Network:  SmartSpirometer
//   Password: breathe123
//   URL:      http://192.168.4.1

const char* AP_SSID = "SmartSpirometer";
const char* AP_PASS = "breathe123";     // min 8 chars, or "" for open

String deviceIP = "";

Preferences prefs;              // profile, counters, session history

String deviceId = "";           // stable ID (from MAC) for cloud sync records

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long lastBroadcast = 0;
const unsigned long BROADCAST_INTERVAL_MS = 100;   // ~10 Hz push to the phone

// ---- Sensor sampling (non-blocking, matches 20 ms timing budget ≈ 50 Hz) ----
unsigned long lastSample = 0;
const unsigned long SAMPLE_INTERVAL_MS = 20;

// ---- Reset requested from the web task (handled safely in loop()) ----
volatile bool resetRequested = false;

// ================================================================
// Profile & recovery tracking (Home-screen UI)
// ================================================================

// Profile (persisted in NVS, editable from the app's Profile tab)
String userName     = "Friend";
int    dailyGoal    = 10;    // breaths per day
int    recoveryDays = 21;    // length of the recovery program

// Day/streak tracking. The ESP32 has no clock, so the phone sends its
// local-time epoch over the WebSocket on connect; we keep an offset.
volatile uint32_t epochAtSync  = 0;   // client local epoch (s) at sync
volatile uint32_t millisAtSync = 0;   // millis() at sync
volatile bool     timeSynced   = false;

uint32_t startDay     = 0;   // epoch-day recovery began (0 = not set)
uint32_t breathsDay   = 0;   // epoch-day breathsToday belongs to
uint32_t breathsToday = 0;   // completed sessions today

// Next-session countdown (1 h between sessions)
const uint32_t SESSION_INTERVAL_S = 3600;
unsigned long  lastBreathMillis   = 0;   // 0 = none since power-on

// Profile save handed off from the async web task to loop()
volatile bool profilePending = false;
char pendingName[24]     = "";
int  pendingDailyGoal    = 10;
int  pendingRecoveryDays = 21;
bool pendingRestart      = false;

// ---- Session history: ring buffer of the last HIST_MAX sessions in
// NVS (keys h0..h19 + hHead/hCnt). Records are compact JSON built by
// the web handlers into these buffers; loop() commits them to NVS. ----
const int HIST_MAX = 20;
volatile bool sessionPending = false;
volatile bool checkinPending = false;
char pendingSessionBuf[512] = "";
char pendingCheckinBuf[768] = "";

// ================================================================
// Calibration & signal processing
// ================================================================

float D_EMPTY = 187.0;   // distance (mm) when piston rests at 0 mL
float D_FULL  = 42.0;    // distance (mm) when piston is at 4000 mL

// ---- Idle deadzone: readings below this (pre-breath) display as 0 ----
const float IDLE_DEADZONE_ML = 100.0;

// ---- Rolling average: volume ----
const int ROLLING_N = 5;
float volBuffer[ROLLING_N];
int   bufIdx  = 0;
bool  bufFull = false;

// ---- Rolling average: flow (larger window = smoother readings) ----
const int FLOW_N = 15;
float flowBuffer[FLOW_N];
int   flowIdx  = 0;
bool  flowFull = false;

float lastVolume = 0;
unsigned long lastTime = 0;

// ---- Target & session state ----
float targetVolume = -1;
bool  targetReached = false;   // informational flag only — no longer ends the breath
bool  sessionEnded  = false;   // freezes the loop once a result is shown

// ---- Breath timing ----
unsigned long breathStartTime = 0;
bool  breathStarted = false;
const float BREATH_START_THRESHOLD = 179.0;  // piston distance (mm) that signals start
const float BREATH_TIMEOUT_S       = 15.0;   // safety stop if breath runs too long
const float BREATH_WARN_S          = 12.0;   // "almost there" warning kicks in

// ---- Flow classification thresholds (mL/s, from real device logs) ----
const float FLOW_TOO_SLOW   = 250.0;
const float FLOW_TOO_FAST   = 700.0;
const float FLOW_EXHALE_NEG = -100.0;  // strong negative = exhale (misuse)
const float FLOW_RAMP_SKIP  = 1.0;     // ignore the first second (ramp-up noise)

// ---- Flow accumulation for classification (post ramp-up, positive only) ----
float breathFlowSum     = 0;
int   breathFlowSamples = 0;

// ---- Exhale detection ----
bool exhaleDetected = false;

// ---- Breath-stop detection ----
const float STOP_FLOW_THRESH = 0.0;   // flow below this counts as "not inhaling"
const int   STOP_NEG_COUNT   = 4;     // consecutive readings to confirm a real stop
int   consecutiveStop = 0;
float peakVolume      = 0;            // highest volume reached during the breath

// ---- Final result: THE single source of truth for OLED + app ----
float sessionScore = 0;               // set once at breath end

// ---- Hold phase ----
const int HOLD_SECONDS = 5;

// ---- Latest values cached for the web dashboard ----
float  webVolume = 0;
float  webFlow   = 0;
String webState  = "Idle";

// ================================================================
// Helpers
// ================================================================

// One rounding rule for every readout. The old code truncated on the
// OLED ((int)v) but rounded in the browser (Math.round) — that was the
// off-by-one-mL mismatch. Everything now goes through here.
int toMl(float v) { return (int)lroundf(v); }

float getRollingAvg(float newVal) {
  volBuffer[bufIdx] = newVal;
  bufIdx = (bufIdx + 1) % ROLLING_N;
  if (bufIdx == 0) bufFull = true;
  int count = bufFull ? ROLLING_N : bufIdx;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += volBuffer[i];
  return sum / count;
}

float getRollingFlowAvg(float newVal) {
  flowBuffer[flowIdx] = newVal;
  flowIdx = (flowIdx + 1) % FLOW_N;
  if (flowIdx == 0) flowFull = true;
  int count = flowFull ? FLOW_N : flowIdx;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += flowBuffer[i];
  return sum / count;
}

float distToVolume(float d) {
  float vol = (D_EMPTY - d) * 4000.0 / (D_EMPTY - D_FULL);
  return max(0.0f, min(4000.0f, vol));
}

// Average flow over the breath (negatives + ramp-up already excluded) -> bin
String classifyFlow() {
  if (breathFlowSamples == 0) return "TOO SLOW";   // never really got going
  float avg = breathFlowSum / breathFlowSamples;
  if (avg > FLOW_TOO_FAST) return "TOO FAST";
  if (avg < FLOW_TOO_SLOW) return "TOO SLOW";
  return "GOOD";
}

// Real-time hint shown on the bottom line during the breath
String liveHint(float flow, float elapsed) {
  if (flow < FLOW_EXHALE_NEG)            return "Do not exhale!";
  if (elapsed >= BREATH_WARN_S)          return "Almost there!";
  if (elapsed <  FLOW_RAMP_SKIP)         return "Keep going";
  if (flow > FLOW_TOO_FAST)              return "Slow down!";
  if (flow > 0 && flow < FLOW_TOO_SLOW)  return "Breathe deeper";
  return "Good pace!";
}

// ================================================================
// Time & recovery-tracking helpers
// ================================================================

// Client-local epoch seconds (0 if no phone has synced since boot)
uint32_t epochNow() {
  if (!timeSynced) return 0;
  return epochAtSync + (millis() - millisAtSync) / 1000;
}

// Days since the (client-local) Unix epoch — rolls over at local midnight
uint32_t currentEpochDay() {
  uint32_t e = epochNow();
  return e ? e / 86400 : 0;
}

// "Day N of recoveryDays" (0 = unknown: no time sync yet)
int recoveryDayNumber() {
  uint32_t today = currentEpochDay();
  if (today == 0) return 0;
  if (startDay == 0 || startDay > today) {   // first run: start today
    startDay = today;
    prefs.putULong("sDay", startDay);
  }
  int day = (int)(today - startDay) + 1;
  return min(day, recoveryDays);
}

// Breaths completed today, honoring midnight rollover for display
uint32_t breathsTodayNow() {
  uint32_t today = currentEpochDay();
  if (today != 0 && today != breathsDay) return 0;
  return breathsToday;
}

// Seconds until the next session is due (-1 = no breath yet since boot)
int32_t nextSessionRemainingS() {
  if (lastBreathMillis == 0) return -1;
  uint32_t elapsed = (millis() - lastBreathMillis) / 1000;
  return (elapsed >= SESSION_INTERVAL_S) ? 0 : (int32_t)(SESSION_INTERVAL_S - elapsed);
}

// Called (from loop context) whenever a session completes — i.e. the
// patient reached the results screen. Counts toward today's goal
// whether or not the volume target was hit: effort counts.
void recordBreathCompletion() {
  uint32_t today = currentEpochDay();
  if (today != 0 && today != breathsDay) {
    breathsToday = 0;
    breathsDay   = today;
  }
  breathsToday++;
  lastBreathMillis = millis();
  prefs.putUInt("bToday", breathsToday);
  prefs.putULong("bDay", breathsDay);
  Serial.print(">> Session recorded. Breaths today: ");
  Serial.println(breathsToday);
}

// Load profile + counters from NVS (call after prefs.begin)
void loadProfile() {
  userName     = prefs.getString("uName", "Friend");
  dailyGoal    = prefs.getInt("dGoal", 10);
  recoveryDays = prefs.getInt("rDays", 21);
  startDay     = prefs.getULong("sDay", 0);
  breathsToday = prefs.getUInt("bToday", 0);
  breathsDay   = prefs.getULong("bDay", 0);
}

// Apply a profile save handed over from the web task (loop context)
void applyPendingProfile() {
  userName = String(pendingName);
  userName.trim();
  if (userName.length() == 0) userName = "Friend";
  dailyGoal    = constrain(pendingDailyGoal, 1, 50);
  recoveryDays = constrain(pendingRecoveryDays, 1, 365);
  prefs.putString("uName", userName);
  prefs.putInt("dGoal", dailyGoal);
  prefs.putInt("rDays", recoveryDays);
  if (pendingRestart) {
    uint32_t today = currentEpochDay();
    startDay = today;                    // 0 if not synced -> re-inits on sync
    breathsToday = 0;
    breathsDay   = today;
    prefs.putULong("sDay", startDay);
    prefs.putUInt("bToday", breathsToday);
    prefs.putULong("bDay", breathsDay);
  }
  Serial.print(">> Profile saved: ");
  Serial.print(userName);
  Serial.print(", ");
  Serial.print(dailyGoal);
  Serial.print(" breaths/day, ");
  Serial.print(recoveryDays);
  Serial.println("-day program");
}

// ================================================================
// Session history (NVS ring buffer)
// ================================================================

// Append a finished-session record (called from loop context)
void historyAppend(const char* rec) {
  uint8_t head  = prefs.getUChar("hHead", 0);
  uint8_t count = prefs.getUChar("hCnt", 0);
  char key[6];
  snprintf(key, sizeof(key), "h%u", head);
  prefs.putString(key, rec);
  prefs.putUChar("hHead", (uint8_t)((head + 1) % HIST_MAX));
  if (count < HIST_MAX) prefs.putUChar("hCnt", count + 1);
  Serial.print(">> Session saved to history slot ");
  Serial.println(head);
}

// Attach a check-in survey to the most recent session record
// (called from loop context)
void checkinMergeIntoLatest(const char* ciJson) {
  StaticJsonDocument<768> ci;
  if (deserializeJson(ci, ciJson) != DeserializationError::Ok) return;

  uint8_t head  = prefs.getUChar("hHead", 0);
  uint8_t count = prefs.getUChar("hCnt", 0);

  if (count == 0) {
    // Edge: check-in with no session record — store it as its own entry
    StaticJsonDocument<1024> rec;
    rec["e"]  = ci["e"] | 0;
    ci.remove("e");
    rec["ci"] = ci;
    String out;
    serializeJson(rec, out);
    historyAppend(out.c_str());
    return;
  }

  uint8_t last = (uint8_t)((head + HIST_MAX - 1) % HIST_MAX);
  char key[6];
  snprintf(key, sizeof(key), "h%u", last);
  String recStr = prefs.getString(key, "");
  StaticJsonDocument<1536> rec;
  if (recStr.length() == 0 ||
      deserializeJson(rec, recStr) != DeserializationError::Ok) return;
  ci.remove("e");
  rec["ci"] = ci;
  String out;
  serializeJson(rec, out);
  prefs.putString(key, out.c_str());
  Serial.println(">> Check-in attached to latest session.");
}

// Full history as a JSON array, newest first (NVS reads are
// thread-safe, so this may be called from the web task)
String buildHistoryJson() {
  uint8_t head  = prefs.getUChar("hHead", 0);
  uint8_t count = prefs.getUChar("hCnt", 0);
  String out;
  out.reserve(count * 600 + 4);
  out = "[";
  for (uint8_t i = 0; i < count; i++) {
    uint8_t idx = (uint8_t)((head + HIST_MAX - 1 - i) % HIST_MAX);
    char key[6];
    snprintf(key, sizeof(key), "h%u", idx);
    String rec = prefs.getString(key, "");
    if (rec.length() == 0) continue;
    if (out.length() > 1) out += ",";
    out += rec;
  }
  out += "]";
  return out;
}

// ---- Push a JSON snapshot to every connected phone ----
// All mL values are pre-rounded integers so the browser does no math
// of its own — guaranteeing it matches the OLED digit for digit.
void broadcastState() {
  StaticJsonDocument<512> doc;
  doc["volume"] = toMl(webVolume);
  doc["target"] = toMl(targetVolume);
  doc["flow"]   = toMl(webFlow);
  doc["score"]  = toMl(sessionEnded ? sessionScore : peakVolume);
  doc["state"]  = webState;
  doc["mode"]   = "AP";
  doc["ip"]     = deviceIP;
  doc["dev"]    = deviceId;
  // Home-screen / profile fields
  doc["name"]   = userName;
  doc["bToday"] = breathsTodayNow();
  doc["bGoal"]  = dailyGoal;
  doc["day"]    = recoveryDayNumber();   // 0 = unknown (no time sync yet)
  doc["rDays"]  = recoveryDays;
  doc["nextS"]  = nextSessionRemainingS();
  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// ---- Incoming WebSocket messages: the page sends its local time on
// connect ({"epoch": <local epoch s>}). Runs on the async task, so it
// only writes plain variables — no I2C, no display, no session state.
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (!(info->final && info->index == 0 && info->len == len &&
        info->opcode == WS_TEXT)) return;

  StaticJsonDocument<96> doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;
  if (doc["epoch"].is<uint32_t>()) {
    millisAtSync = millis();
    epochAtSync  = doc["epoch"].as<uint32_t>();
    timeSynced   = true;
  }
}

// ================================================================
// Sensor init: socketed VL53L0X with pin-orientation auto-fallback
// ================================================================

bool initToFSensor() {
  // Orientation 1: SDA = 15, SCL = 2
  Wire.begin(PIN_A, PIN_B);
  Wire.setClock(100000);   // LOCKED at 100 kHz — GPIO 2 shares the status LED

  if (!lox.begin()) {
    // Orientation 2: SDA = 2, SCL = 15
    Wire.end();
    Wire.begin(PIN_B, PIN_A);
    Wire.setClock(100000);

    if (!lox.begin()) {
      return false;
    }
  }

  // High-speed refresh: 20,000 us timing budget -> ~50 Hz sampling
  lox.setMeasurementTimingBudgetMicroSeconds(20000);
  return true;
}

// ================================================================
// Session phases (previously OLED screens — now Serial + web state)
// ================================================================

// Hold phase: count down 5..1 while the app shows the locked-in score
void holdCountdown(float score) {
  webState = "Hold";
  for (int n = HOLD_SECONDS; n >= 1; n--) {
    Serial.print(">> Hold it! "); Serial.print(n);
    Serial.print("  ("); Serial.print(toMl(score)); Serial.println(" mL)");
    broadcastState();   // keep the phone in sync during the countdown
    delay(1000);
  }
}

// Results: goal vs score, encouragement, and flow next-step
void showResultsScreen(float score, float goal) {
  setLed(false);
  sessionEnded = true;
  webState = "Done";

  bool hitGoal = (score >= goal);
  String flowClass = classifyFlow();

  Serial.println(hitGoal ? ">> Great job!" : ">> Good effort!");
  Serial.print("Results -> goal "); Serial.print(toMl(goal));
  Serial.print("  score "); Serial.print(toMl(score));
  Serial.print("  flow "); Serial.println(flowClass);

  broadcastState();   // push the final Done state immediately
}

// Exhale (misuse): skips the hold/score entirely, just corrects them
void showExhaleScreen() {
  setLed(false);
  sessionEnded = true;
  webState = "Done";

  Serial.println(">> Inhale only.");
  broadcastState();
}

// ================================================================
// Session reset (feature 3)
// ================================================================

// Called from loop() only — never directly from the async web task,
// to avoid two tasks touching the I2C bus / session state at once.
void resetSession() {
  breathStarted   = false;
  sessionEnded    = false;
  targetReached   = false;
  exhaleDetected  = false;
  peakVolume      = 0;
  sessionScore    = 0;
  consecutiveStop = 0;
  breathFlowSum     = 0;
  breathFlowSamples = 0;
  bufIdx  = 0; bufFull  = false;
  flowIdx = 0; flowFull = false;
  lastVolume = 0;
  lastTime   = 0;
  breathStartTime = 0;
  webVolume = 0;
  webFlow   = 0;
  webState  = "Idle";

  setLed(true);
  Serial.println(">> Session reset — ready for a new breath.");
  if (targetVolume > 0) Serial.println("Inhale to start...");
  else                  Serial.println("Set target volume via Serial or web.");
  broadcastState();
}

// ================================================================
// WiFi mode management (feature 4)
// ================================================================

// Bring up the device's own Access Point. This is the only network
// mode: the ESP32 never joins an external WiFi.
void startWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  deviceIP = WiFi.softAPIP().toString();
  Serial.print("AP '"); Serial.print(AP_SSID);
  Serial.print("' up. Browse to http://"); Serial.println(deviceIP);
}

// ================================================================
// Setup
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);   // allow hardware & power to settle

  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);

  // ---- VL53L0X (socketed, orientation auto-fallback, 100 kHz, ~50 Hz) ----
  if (!initToFSensor()) {
    Serial.println("[ERROR] Could not communicate with VL53L0X — check socket seating.");
    while (1) { delay(100); }
  }
  Serial.println("VL53L0X ready — high-speed sampling enabled (~50 Hz).");

// ---- WiFi FIRST: bring up the network stack before the server binds ----
  prefs.begin("spiro", false);
  loadProfile();      // name, goals, streak counters from NVS

  // Stable device identity for cloud sync (MAC without colons)
  deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  deviceId = "spiro-" + deviceId;

  startWiFi();   // device hosts its own network; no client mode

  // ---- Web routes (registered AFTER WiFi is up) ----
  // NOTE: send_P() is deprecated in the ESP32Async fork of
  // ESPAsyncWebServer and can serve an empty body (Content-Length: 0)
  // for large PROGMEM pages. On ESP32, PROGMEM is memory-mapped flash,
  // so a plain response with an explicit length works reliably.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *res = request->beginResponse(
        200, "text/html", (const uint8_t*)INDEX_HTML, strlen(INDEX_HTML));
    request->send(res);
  });

  // Goal setting from the phone (same validation as the Serial path).
  // Note: no display call here — this handler runs on the async task,
  // and loop() repaints the idle screen on its next pass anyway.
  server.on("/setGoal", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value", true)) {
      float val = request->getParam("value", true)->value().toFloat();
      if (val > 0 && val <= 4000) {
        targetVolume = val;
        Serial.print("Target set via web to "); Serial.print(val, 0);
        Serial.println(" mL. Inhale to start...");
        request->send(200, "text/plain", "OK");
        return;
      }
    }
    request->send(400, "text/plain", "Enter a number between 1 and 4000");
  });

  // New Breath: just raise a flag — loop() performs the reset safely
  server.on("/newBreath", HTTP_POST, [](AsyncWebServerRequest *request) {
    resetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  // Profile save from the app (name, daily goal, program length).
  // Same flag pattern as /newBreath: stash values, loop() applies them.
  server.on("/profile", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("name", true)) {
      String n = request->getParam("name", true)->value();
      n.toCharArray(pendingName, sizeof(pendingName));
    }
    if (request->hasParam("dailyGoal", true))
      pendingDailyGoal = request->getParam("dailyGoal", true)->value().toInt();
    if (request->hasParam("recoveryDays", true))
      pendingRecoveryDays = request->getParam("recoveryDays", true)->value().toInt();
    pendingRestart = request->hasParam("restart", true) &&
                     request->getParam("restart", true)->value() == "1";
    profilePending = true;
    request->send(200, "text/plain", "OK");
  });

  // Post-session check-in from the app (Stage 6). No storage backend
  // yet — logs the submitted fields to Serial and acknowledges.
  server.on("/checkin", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(">> Post-session check-in received:");
    int n = request->params();
    for (int i = 0; i < n; i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (p->isPost()) {
        Serial.print("   ");
        Serial.print(p->name());
        Serial.print(": ");
        Serial.println(p->value());
      }
    }
    request->send(200, "text/plain", "OK");
  });

  // Session summary record: handler builds compact JSON, loop() commits
  server.on("/session", HTTP_POST, [](AsyncWebServerRequest *request) {
    auto P = [&](const char* n) -> String {
      return request->hasParam(n, true) ? request->getParam(n, true)->value() : String("");
    };
    StaticJsonDocument<384> doc;
    doc["e"] = (uint32_t)P("epoch").toInt();   // real epoch (for date/time display)
    doc["n"] = P("n").toInt();                 // breaths completed
    doc["N"] = P("N").toInt();                 // breaths target
    doc["o"] = P("outcome");                   // Perfect / Too fast / Too slow
    doc["c"] = P("consist").toInt();           // consistency %
    doc["v"] = P("vol").toInt();               // avg volume, mL
    doc["d"] = P("dur").toFloat();             // avg duration, s
    doc["p"] = P("peak").toInt();              // avg peak flow, mL/s
    serializeJson(doc, pendingSessionBuf, sizeof(pendingSessionBuf));
    sessionPending = true;
    request->send(200, "text/plain", "OK");
  });

  // Post-session check-in: logged to Serial + attached to the latest
  // history record (JSON built here, committed by loop())
  server.on("/checkin", HTTP_POST, [](AsyncWebServerRequest *request) {
    auto P = [&](const char* n) -> String {
      return request->hasParam(n, true) ? request->getParam(n, true)->value() : String("");
    };
    Serial.println(">> Post-session check-in:");
    int n = request->params();
    for (int i = 0; i < n; i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (p->isPost()) {
        Serial.print("   ");
        Serial.print(p->name());
        Serial.print(": ");
        Serial.println(p->value());
      }
    }
    StaticJsonDocument<768> doc;
    doc["e"]      = (uint32_t)P("epoch").toInt();
    doc["pain"]   = P("pain").toInt();
    doc["tired"]  = P("tiredness").toInt();
    doc["chest"]  = P("chestTightness").toInt();
    doc["dry"]    = P("dryMouth");
    doc["cough"]  = P("cough");
    if (P("cough") == "Yes") {
      doc["mucus"] = P("mucus");
      if (P("mucus") == "Yes") {
        doc["colour"] = P("mucusColour");
        doc["thick"]  = P("thickness");
        doc["amt"]    = P("amount");
      }
      doc["clear"] = P("coughClear");
      doc["hard"]  = P("coughHard");
    }
    if (P("notes").length() > 0) doc["notes"] = P("notes");
    serializeJson(doc, pendingCheckinBuf, sizeof(pendingCheckinBuf));
    checkinPending = true;
    request->send(200, "text/plain", "OK");
  });

  // Full session history, newest first
  server.on("/history", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildHistoryJson());
  });

  ws.onEvent(onWsEvent);   // receives the phone's time sync
  server.addHandler(&ws);
  server.begin();

  setLed(true);

  Serial.println("Enter target volume in mL (e.g. 2000):");
}

// ================================================================
// Main loop
// ================================================================

void loop() {
  // ---- Web-requested reset (checked BEFORE the sessionEnded freeze,
  //      so "New Breath" works from the Done screen) ----
  if (resetRequested) {
    resetRequested = false;
    resetSession();
  }

  // ---- Web-requested profile save (same safe handoff pattern) ----
  if (profilePending) {
    profilePending = false;
    applyPendingProfile();
  }

  // ---- History writes handed off from the web task ----
  if (sessionPending) {
    sessionPending = false;
    historyAppend(pendingSessionBuf);
  }
  if (checkinPending) {
    checkinPending = false;
    checkinMergeIntoLatest(pendingCheckinBuf);
  }

  // ---- Push live data to any connected phone (runs in every state) ----
  if (millis() - lastBroadcast >= BROADCAST_INTERVAL_MS) {
    lastBroadcast = millis();
    ws.cleanupClients();
    broadcastState();
  }

  // ---- Wait for target volume (Serial or web — whichever comes first) ----
  if (targetVolume < 0) {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      float val = input.toFloat();
      if (val > 0 && val <= 4000) {
        targetVolume = val;
        Serial.print("Target set to "); Serial.print(targetVolume, 0);
        Serial.println(" mL. Inhale to start...");
      } else {
        Serial.println("Invalid. Enter a number between 1 and 4000:");
      }
    }
    return;
  }

  // ---- Session finished: freeze on the result until New Breath / reset ----
  if (sessionEnded) return;

  // ---- Non-blocking sample gate: aligns with the 20 ms timing budget ----
  if (millis() - lastSample < SAMPLE_INTERVAL_MS) return;
  lastSample = millis();

  // ---- Read sensor ----
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus == 4) return;   // out of range / invalid reading
  int rawDist = measure.RangeMilliMeter;
  if (rawDist >= 8190) return;

  // ---- Detect breath start ----
  if (!breathStarted && rawDist < BREATH_START_THRESHOLD) {
    breathStarted = true;
    breathStartTime = millis();
    Serial.println(">> Breath started.");
  }

  // ---- Volume + flow ----
  float volume = getRollingAvg(distToVolume(rawDist));
  unsigned long now = millis();
  float dt = (lastTime > 0) ? (now - lastTime) / 1000.0 : 0;
  float rawFlow = (dt > 0) ? (volume - lastVolume) / dt : 0;
  float flow = getRollingFlowAvg(rawFlow);

  lastVolume = volume;
  lastTime = now;

  float elapsed = breathStarted ? (millis() - breathStartTime) / 1000.0 : 0.0;

  // ---- Idle deadzone (feature 1): before a breath starts, sensor
  //      jitter (-30..100 mL band) is reported as a clean 0/idle.
  //      Internal pipeline still runs on real values. ----
  if (!breathStarted) {
    webVolume = (volume < IDLE_DEADZONE_ML) ? 0 : volume;
    webFlow   = 0;
    webState  = "Idle";
  } else {
    webVolume = volume;
    webFlow   = flow;
    webState  = "Breathing";
  }

  // ---- Track peak volume = the score ----
  if (breathStarted && volume > peakVolume) peakVolume = volume;

  // ---- Mark goal reached (flag only — does NOT end the breath) ----
  if (!targetReached && peakVolume >= targetVolume) {
    targetReached = true;
    Serial.println(">> Goal volume reached.");
  }

  // // ---- Strong exhale = misuse -> "Inhale only." (skips hold) ----
  // if (breathStarted && !exhaleDetected && flow < FLOW_EXHALE_NEG) {
  //   exhaleDetected = true;
  //   Serial.println(">> Exhale detected mid-breath!");
  //   showExhaleScreen();
  //   return;
  // }

  // ---- Accumulate flow for classification (skip ramp-up, skip negatives) ----
  if (breathStarted && elapsed >= FLOW_RAMP_SKIP && flow > 0) {
    breathFlowSum += flow;
    breathFlowSamples++;
  }

  // ---- Breath-stop detection: consecutive non-rising readings post ramp-up ----
  if (breathStarted && elapsed >= FLOW_RAMP_SKIP) {
    if (flow < STOP_FLOW_THRESH) consecutiveStop++;
    else                         consecutiveStop = 0;

    if (consecutiveStop >= STOP_NEG_COUNT) {
      // Feature 2: lock the final result into ONE variable, freeze the
      // app's live readout to it, and use it everywhere from here on.
      sessionScore = peakVolume;
      webVolume    = sessionScore;
      recordBreathCompletion();   // counts toward today's goal, resets countdown
      Serial.print(">> Breath stopped at "); Serial.print(toMl(sessionScore)); Serial.println(" mL");
      holdCountdown(sessionScore);
      showResultsScreen(sessionScore, targetVolume);
      return;
    }
  }

  // ---- Serial log (for empirical tuning; hint was the OLED bottom line) ----
  Serial.print(rawDist); Serial.print(" mm\t| ");
  Serial.print(volume, 0); Serial.print(" mL\t| ");
  Serial.print(flow, 0); Serial.print(" mL/s\t| ");
  if (breathStarted) {
    Serial.print(elapsed, 1); Serial.print(" s\t| ");
    Serial.println(liveHint(flow, elapsed));
  } else {
    Serial.println("--");
  }

  // ---- Timeout safety: too long, end it anyway ----
  if (breathStarted && elapsed >= BREATH_TIMEOUT_S) {
    Serial.println(">> Timeout.");
    sessionScore = peakVolume;
    webVolume    = sessionScore;
    recordBreathCompletion();   // counts toward today's goal, resets countdown
    holdCountdown(sessionScore);
    showResultsScreen(sessionScore, targetVolume);
    return;
  }
}
