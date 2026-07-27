// ================================================================
// Smart Spirometer — ESP32 firmware
// VL53L0X piston tracking + SSD1306 OLED + WiFi AP web dashboard
//
// Hardware:
//   ESP32 dev board
//   VL53L0X ToF sensor  (I2C: SDA=21, SCL=22)
//   SSD1306 128x64 OLED (same I2C bus, addr 0x3C)
//   AirLife 4000mL incentive spirometer
//
// Libraries (Library Manager):
//   VL53L0X (Pololu), Adafruit GFX, Adafruit SSD1306,
//   ESPAsyncWebServer, AsyncTCP, ArduinoJson
// ================================================================

#include <Wire.h>
#include <VL53L0X.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "webpage.h"   // INDEX_HTML lives here

VL53L0X sensor;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int LED_PIN = 4;
const int BUTTON_PIN = 0;

// ---- WiFi Access Point ----
const char* AP_SSID = "SmartSpirometer";
const char* AP_PASS = "breathe123";   // min 8 chars; use "" for an open network

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long lastBroadcast = 0;
const unsigned long BROADCAST_INTERVAL_MS = 100;   // ~10 Hz push to the phone

// ---- Calibration ----
float D_EMPTY = 187.0;   // distance (mm) when piston rests at 0 mL
float D_FULL  = 42.0;    // distance (mm) when piston is at 4000 mL

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

// ---- Breath-stop detection (patient stops inhaling, short of goal or not) ----
const float STOP_FLOW_THRESH = 0.0;   // flow below this counts as "not inhaling"
const int   STOP_NEG_COUNT   = 4;     // consecutive readings to confirm a real stop
int   consecutiveStop = 0;
float peakVolume      = 0;            // highest volume reached = the patient's score

// ---- Hold phase ----
const int HOLD_SECONDS = 5;

// ---- Latest values cached for the web dashboard ----
float  webVolume = 0;
float  webFlow   = 0;
String webState  = "Idle";

// ================================================================
// Helpers
// ================================================================

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

// ---- Push a JSON snapshot to every connected phone ----
void broadcastState() {
  StaticJsonDocument<220> doc;
  doc["volume"] = webVolume;
  doc["target"] = targetVolume;
  doc["flow"]   = webFlow;
  doc["score"]  = peakVolume;
  doc["state"]  = webState;
  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// ================================================================
// Screens (OLED)
// ================================================================

// Shown before a target volume is entered over Serial or the web page
void showWaitingForTarget() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Set target volume");
  display.setCursor(0, 36);
  display.println("via Serial or web.");
  display.display();
}

// Idle screen: target set, waiting for the patient to start inhaling
void showIdleScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(22, 6);
  display.println("Target Volume");

  display.setTextSize(3);
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", (int)targetVolume);
  int w = strlen(buf) * 18;  // textSize 3 = 18px/char
  display.setCursor(max(0, (SCREEN_WIDTH - w) / 2), 22);
  display.println(buf);

  display.setTextSize(1);
  display.setCursor(22, 52);
  display.println("Inhale to start");
  display.display();
}

// Live screen during the breath: volume vs target, flow + time,
// progress bar, and a real-time hint line
void updateDisplay(float volume, float flow, float elapsed) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("-- Spirometer --");

  display.setCursor(0, 12);
  display.print("Vol:  ");
  display.print((int)volume);
  display.print(" / ");
  display.print((int)targetVolume);
  display.println(" mL");

  display.setCursor(0, 24);
  display.print("Flow: ");
  display.print((int)flow);
  display.print(" mL/s  ");
  display.print(elapsed, 1);
  display.println("s");

  // Progress bar (fills as volume approaches target)
  display.drawRect(0, 36, 128, 10, SSD1306_WHITE);
  float frac = (targetVolume > 0) ? volume / targetVolume : 0;
  frac = constrain(frac, 0.0f, 1.0f);
  int fillW = (int)(126 * frac);
  if (fillW > 0) display.fillRect(1, 37, fillW, 8, SSD1306_WHITE);

  // Live hint
  display.setCursor(0, 52);
  display.print(">> ");
  display.println(liveHint(flow, elapsed));

  display.display();
}

// Hold phase: count down 5..1 while showing the score they locked in
void holdCountdown(float score) {
  webState = "Hold";
  for (int n = HOLD_SECONDS; n >= 1; n--) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(34, 2);
    display.println("Hold it!");

    display.setTextSize(4);            // big centered number
    display.setCursor(54, 18);
    display.println(n);

    display.setTextSize(1);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d mL", (int)score);
    int w = strlen(buf) * 6;
    display.setCursor((SCREEN_WIDTH - w) / 2, 54);
    display.println(buf);

    display.display();
    broadcastState();   // keep the phone in sync during the countdown
    delay(1000);
  }
}

// Results screen: goal vs score, encouragement, and flow next-step
void showResultsScreen(float score, float goal) {
  digitalWrite(LED_PIN, LOW);
  sessionEnded = true;
  webState = "Done";

  bool hitGoal = (score >= goal);

  // Encouragement — "Great job!" fits one big line; the miss case splits in two
  String enc1, enc2;
  if (hitGoal) { enc1 = "Great job!"; enc2 = ""; }
  else         { enc1 = "Good";       enc2 = "effort!"; }

  // Flow-based next step (from the flow accumulated during the inhale)
  String flowClass = classifyFlow();
  String flowMsg = "";
  if      (flowClass == "TOO FAST") flowMsg = "Too fast!";
  else if (flowClass == "TOO SLOW") flowMsg = "Too slow!";
  // GOOD pace -> no correction line

  Serial.print("Results -> goal "); Serial.print((int)goal);
  Serial.print("  score "); Serial.print((int)score);
  Serial.print("  flow "); Serial.println(flowClass);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Encouragement (textSize 2)
  display.setTextSize(2);
  display.setCursor(max(0, (SCREEN_WIDTH - (int)enc1.length() * 12) / 2), 0);
  display.println(enc1);
  if (enc2.length() > 0) {
    display.setCursor(max(0, (SCREEN_WIDTH - (int)enc2.length() * 12) / 2), 16);
    display.println(enc2);
  }

  // Goal / Score (textSize 1)
  display.setTextSize(1);
  char buf[24];
  snprintf(buf, sizeof(buf), "GOAL:  %d mL", (int)goal);
  display.setCursor(4, 34);
  display.println(buf);

  snprintf(buf, sizeof(buf), "SCORE: %d mL", (int)score);
  display.setCursor(4, 44);
  display.println(buf);

  // Flow feedback (textSize 1, centered)
  if (flowMsg.length() > 0) {
    display.setCursor(max(0, (SCREEN_WIDTH - (int)flowMsg.length() * 6) / 2), 54);
    display.println(flowMsg);
  }

  display.display();
  broadcastState();   // push the final Done state immediately
}

// Exhale (misuse) screen: skips the hold/score entirely, just corrects them
void showExhaleScreen() {
  digitalWrite(LED_PIN, LOW);
  sessionEnded = true;
  webState = "Done";

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 18);
  display.println("Inhale");
  display.setCursor(34, 38);
  display.println("only.");
  display.display();
  broadcastState();
}

// ================================================================
// Setup
// ================================================================

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 not found.");
    while (1);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 28);
  display.println("Initializing...");
  display.display();

  if (!sensor.init()) {
    display.clearDisplay();
    display.setCursor(0, 28);
    display.println("Sensor not found!");
    display.display();
    Serial.println("Sensor not found — check wiring.");
    while (1);
  }

  // ---- WiFi Access Point + web server ----
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP started. Connect to '");
  Serial.print(AP_SSID);
  Serial.print("' (pass: ");
  Serial.print(AP_PASS);
  Serial.println("), then browse to http://192.168.4.1");

  // Serve the dashboard page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  // Goal setting from the phone (same validation as the Serial path)
  server.on("/setGoal", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value", true)) {
      float val = request->getParam("value", true)->value().toFloat();
      if (val > 0 && val <= 4000) {
        targetVolume = val;
        Serial.print("Target set via web to "); Serial.print(val, 0);
        Serial.println(" mL. Inhale to start...");
        showIdleScreen();
        request->send(200, "text/plain", "OK");
        return;
      }
    }
    request->send(400, "text/plain", "Enter a number between 1 and 4000");
  });

  server.addHandler(&ws);
  server.begin();

  delay(1000);
  sensor.startContinuous();
  digitalWrite(LED_PIN, HIGH);

  Serial.println("Enter target volume in mL (e.g. 2000):");
  showWaitingForTarget();
}

// ================================================================
// Main loop
// ================================================================

void loop() {
  // ---- Push live data to any connected phone (runs in every state) ----
  if (millis() - lastBroadcast >= BROADCAST_INTERVAL_MS) {
    lastBroadcast = millis();
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
        showIdleScreen();
      } else {
        Serial.println("Invalid. Enter a number between 1 and 4000:");
      }
    }
    return;
  }

  // ---- Session finished: freeze on the result until hardware reset ----
  if (sessionEnded) return;

  // ---- Manual stop button ----
  if (digitalRead(BUTTON_PIN) == LOW) {
    digitalWrite(LED_PIN, LOW);
    sensor.stopContinuous();
    webState = "Stopped";
    broadcastState();
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(35, 28);
    display.println("Stopped.");
    display.display();
    Serial.println("Stopped.");
    while (1);
  }

  // ---- Read sensor ----
  int rawDist = sensor.readRangeContinuousMillimeters();
  if (sensor.timeoutOccurred() || rawDist >= 8190) return;

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

  // ---- Cache values for the web dashboard ----
  webVolume = volume;
  webFlow   = flow;
  webState  = breathStarted ? "Breathing" : "Idle";

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
      Serial.print(">> Breath stopped at "); Serial.print((int)peakVolume); Serial.println(" mL");
      holdCountdown(peakVolume);
      showResultsScreen(peakVolume, targetVolume);
      return;
    }
  }

  // ---- Live display ----
  if (!breathStarted) {
    showIdleScreen();
  } else {
    updateDisplay(volume, flow, elapsed);
  }

  // ---- Serial log (for empirical tuning) ----
  Serial.print(rawDist); Serial.print(" mm\t| ");
  Serial.print(volume, 0); Serial.print(" mL\t| ");
  Serial.print(flow, 0); Serial.print(" mL/s\t| ");
  if (breathStarted) {
    Serial.print(elapsed, 1); Serial.println(" s");
  } else {
    Serial.println("--");
  }

  // ---- Timeout safety: too long, end it anyway ----
  if (breathStarted && elapsed >= BREATH_TIMEOUT_S) {
    Serial.println(">> Timeout.");
    holdCountdown(peakVolume);
    showResultsScreen(peakVolume, targetVolume);
    return;
  }

  delay(20);
}
