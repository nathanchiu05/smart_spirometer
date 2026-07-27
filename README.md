# Smart Spirometer — ESP32 Firmware + Mobile App UI (v3)

## What's new in v3 (Home-screen UI)

The web dashboard is now a multi-view app matching the Home-screen mock:

- **Home tab** — greeting ("Good morning, [name]"), Today's goal card with a
  gradient progress ring + lung icon, Next session countdown card, and a
  Recovery progress ("Day N of 21") card with sprout graphic.
- **Session view** — the live breath screen (volume, progress bar, flow,
  New Breath, volume goal). Opened by tapping the Next-session card;
  auto-opens when a breath starts.
- **Profile tab (functional)** — name, daily breath goal, and program length,
  saved to ESP32 NVS via `POST /profile`. Includes a "Restart recovery"
  option that makes today Day 1.
- **History / Trends tabs** — styled placeholders for now.

**How day tracking works on a clockless ESP32:** the page sends the phone's
local time over the WebSocket on connect (`{"epoch": ...}`); the firmware
keeps an offset and computes day rollovers at local midnight. Completed
sessions (any breath that reaches the results screen) increment today's
count, persist in NVS, and reset the 1-hour next-session countdown
(`Ready when you are` -> `In 59m` -> `Due now`).

---

# Previous: v2 notes

Add-on device for a standard AirLife 4000 mL incentive spirometer. A VL53L0X
time-of-flight sensor tracks piston displacement; the ESP32 computes volume and
flow in real time, drives an SSD1306 OLED for patient-facing feedback, and
hosts a mobile web dashboard.

## What's new in v2

1. **Idle deadzone** — pre-breath sensor jitter (the -30 to 100 mL band) is
   reported to the app as a clean 0 mL / Idle. Internal signal processing is
   unaffected.
2. **OLED/app consistency** — one rounding rule (`toMl()`) and one final-result
   variable (`sessionScore`) feed both displays; the firmware sends pre-rounded
   integers so the browser does no math. The two readouts now match to the mL.
3. **New Breath button** — resets the trial from the app. No more pressing
   EN/RST between breaths. Works from the Done screen or mid-breath.
4. **Access Point only** — the device hosts its own network at a fixed
   address; it never joins external WiFi.

> **Note:** GPIO 0 (BOOT) has no firmware function. It was briefly a WiFi
> mode toggle; with AP-only networking that toggle is gone.

## File structure

```
SmartSpirometer/
├── README.md                       <- you are here
└── SmartSpirometer/                <- Arduino sketch folder (name must match the .ino)
    ├── SmartSpirometer.ino         <- full firmware: sensing, OLED, WiFi, web server
    └── webpage.h                   <- embedded mobile dashboard (PROGMEM HTML/CSS/JS)
```

## Libraries required

| Library            | Notes                                          |
|--------------------|------------------------------------------------|
| VL53L0X            | Pololu version                                 |
| Adafruit GFX       |                                                |
| Adafruit SSD1306   |                                                |
| ESP Async WebServer| by **ESP32Async** (maintained fork)            |
| Async TCP          | by **ESP32Async** — NOT `ESPAsyncTCP` (ESP8266)|
| ArduinoJson        | v6+                                            |

`Preferences` ships with the ESP32 core — nothing to install.

## Wiring

| Signal | ESP32 pin |
|--------|-----------|
| SCL    | GPIO 15   |
| SDA    | GPIO 4    |
| LED    | GPIO 4    |
| BOOT   | GPIO 0    | (unused by firmware)

Three of the sensor's four pins line up with the `3V3 / GND / D15` run;
SDA jumps past D2 to D4 (see below).

**The OLED is optional.** If no SSD1306 is detected on the bus at boot,
the firmware logs `running headless (web app only)` and continues
normally — all session logic, WiFi, history, and the web app work
unchanged. Reattach the display any time and it lights up again on the
next reset.

### Why SDA skips GPIO 2

The sensor header (VIN / GND / SCL / SDA) lines up with `3V3 · GND · D15 · D2`,
but **GPIO 2 must not be used for I2C on this board**. It is a strapping pin
that has to read LOW to enter download mode, and the sensor's onboard I2C
pull-up holds it HIGH — producing:

```
A fatal error occurred: Failed to connect to ESP32: Wrong boot mode detected (0xb)!
```

No button sequence fixes this; holding BOOT only controls GPIO 0. So SDA
skips one pin over to **D4**, which has no strapping role:

```
3V3 -> VIN      GND -> GND      D15 -> SCL      D4 -> SDA      (D2 unused)
```

With that wiring, uploads work with the sensor connected.

## Connecting

The device hosts its own WiFi network. **There is no station/client
mode** — the ESP32 never joins a home or public network, so it is never
exposed on a shared network and the address never changes.

1. On your phone, join the WiFi network **SmartSpirometer**
   (password `breathe123`).
2. Ignore the "no internet connection" warning — that's expected.
3. Open **http://192.168.4.1** (type `http://` so the browser doesn't
   treat it as a search).

If an OLED is attached, the boot screen shows the network name,
password, and URL. The same details are printed to Serial at 115200.

## Session flow

Idle -> Breathing -> Hold (5 s countdown) -> Done. From Done, tap
**New Breath** in the app to start the next trial with the same target
(change the target anytime via the Set goal field or Serial).

## Tunable knobs (in the .ino)

| Constant           | Default | Purpose                                        |
|--------------------|---------|------------------------------------------------|
| IDLE_DEADZONE_ML   | 100     | Pre-breath readings below this display as 0    |
| STOP_NEG_COUNT     | 4       | Consecutive low-flow reads to confirm a stop (raise to 6–8 if patients pause mid-inhale) |
| STOP_FLOW_THRESH   | 0.0     | Flow below this counts as "not inhaling" (raise to ~30 if flat-zero hold fails to trigger) |
| HOLD_SECONDS       | 5       | Hold countdown length                          |
| BREATH_TIMEOUT_S   | 15      | Safety cutoff for a runaway breath             |
| FLOW_RAMP_SKIP     | 1.0     | Seconds of ramp-up noise to ignore             |
| FLOW_TOO_SLOW/FAST | 250/700 | Flow classification bounds (mL/s, empirical)   |
| D_EMPTY / D_FULL   | 187/42  | Calibration distances (mm) — re-measure if the sensor bracket moves |
