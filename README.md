# Smart Spirometer — ESP32 Firmware + Mobile Dashboard

Add-on device for a standard AirLife 4000 mL incentive spirometer. A VL53L0X
time-of-flight sensor tracks piston displacement; the ESP32 computes volume and
flow in real time, drives an SSD1306 OLED for patient-facing feedback, and now
also hosts a mobile web dashboard over its own WiFi hotspot.

## File structure

```
SmartSpirometer/
├── README.md                       <- you are here
└── SmartSpirometer/                <- Arduino sketch folder (name must match the .ino)
    ├── SmartSpirometer.ino         <- full firmware: sensing, OLED, WiFi AP, web server
    └── webpage.h                   <- embedded mobile dashboard (PROGMEM HTML/CSS/JS)
```

Open `SmartSpirometer/SmartSpirometer.ino` in the Arduino IDE (or VSCode with
the Arduino extension). `webpage.h` is pulled in automatically via `#include`.

## Libraries required

Install via Arduino IDE Library Manager:

| Library            | Notes                                          |
|--------------------|------------------------------------------------|
| VL53L0X            | Pololu version                                 |
| Adafruit GFX       |                                                |
| Adafruit SSD1306   |                                                |
| ESPAsyncWebServer  | mathieucarbou or lacamera fork both work       |
| AsyncTCP           | ESP32 version — NOT `ESPAsyncTCP` (ESP8266)    |
| ArduinoJson        | v6+                                            |

Board: any ESP32 dev board (Tools -> Board -> ESP32 Dev Module).

## Wiring (unchanged from previous build)

Both the VL53L0X and the SSD1306 share one I2C bus:

| Signal | ESP32 pin |
|--------|-----------|
| SDA    | GPIO 21   |
| SCL    | GPIO 22   |
| LED    | GPIO 4    |
| Button | GPIO 0    |

## Using the mobile dashboard

1. Flash the firmware and power the device.
2. On your phone, join the WiFi network **SmartSpirometer**
   (password `breok\\\\athe123` — change `AP_SSID` / `AP_PASS` in the .ino if you like).
3. Your phone may warn "no internet connection" — that's expected; stay connected.
4. Open a browser and go to **http://192.168.4.1**

The dashboard shows:

- **Live volume** (large readout) with a progress bar toward the goal
- **Flow rate** and **peak volume (score)** for the current breath
- **Session state**: Idle -> Breathing -> Hold -> Done
- A **Set goal** field (1–4000 mL). This does the same thing as typing a target
  into the Serial Monitor — whichever happens first wins.

Data is pushed from the ESP32 to the phone over a WebSocket at ~10 Hz, so there
is no polling and the sensor loop keeps its normal 20 ms cadence
(ESPAsyncWebServer handles requests off the main loop).

## Notes / current limitations

- After a session ends (`Done`), the firmware freezes on the results screen
  until a hardware reset — same behavior as before. A web "reset session"
  button is a natural next feature.
- The webpage is embedded in flash as a PROGMEM string. If it grows into a
  multi-page app later, migrating to LittleFS is straightforward.
- During the 5-second hold countdown, the OLED loop uses `delay(1000)`; the
  dashboard still receives one state push per second inside the countdown.

## Tunable knobs (in the .ino)

| Constant           | Default | Purpose                                        |
|--------------------|---------|------------------------------------------------|
| STOP_NEG_COUNT     | 4       | Consecutive low-flow reads to confirm a stop (raise to 6–8 if patients pause mid-inhale) |
| STOP_FLOW_THRESH   | 0.0     | Flow below this counts as "not inhaling" (raise to ~30 if flat-zero hold fails to trigger) |
| HOLD_SECONDS       | 5       | Hold countdown length                          |
| BREATH_TIMEOUT_S   | 15      | Safety cutoff for a runaway breath             |
| FLOW_RAMP_SKIP     | 1.0     | Seconds of ramp-up noise to ignore             |
| FLOW_TOO_SLOW/FAST | 250/700 | Flow classification bounds (mL/s, empirical)   |
| D_EMPTY / D_FULL   | 187/42  | Calibration distances (mm) — re-measure if the sensor bracket moves |
