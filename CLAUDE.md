# CLAUDE.md — Wandering Hour Clock

## Project Overview

This is an Arduino/C++ project for an ESP32-based WiFi-connected wandering hour clock. It drives a 28BYJ-48 stepper motor to display time using a wandering hour mechanism, syncs time via NTP, and exposes a web UI for manual adjustments and preferences.

**Single source file:** `wandering-hour-clock.ino` (Arduino sketch, ~580 lines)

---

## Repository Structure

```
wandering-hour-clock/
├── wandering-hour-clock.ino   # Main Arduino sketch (all logic lives here)
├── secrets.h                  # WiFi credentials (gitignored, must be created locally)
├── README.md                  # User-facing documentation and hardware instructions
├── LICENSE                    # GPLv3
└── images/                    # Photos, wiring diagrams, and demo GIFs
```

---

## Hardware Platform

- **MCU:** ESP32 (or ESP8266) with WiFi
- **Motor:** 28BYJ-48 unipolar stepper motor (2048 steps/revolution)
- **Driver:** ULN2003
- **Motor pins:** IN1=19, IN2=18, IN3=5, IN4=17
- **Built-in LED:** pin 13
- **Power:** 5V, 2A+

---

## Arduino Libraries Required

Install these in Arduino IDE before compiling:

| Library | Purpose | Source |
|---|---|---|
| `WiFi.h` | WiFi connectivity | Built-in (ESP32 core) |
| `ESPmDNS.h` | mDNS/Bonjour hostname | Built-in (ESP32 core) |
| `WiFiUdp.h` | UDP for NTP | Built-in (ESP32 core) |
| `ArduinoOTA.h` | Over-the-air firmware updates | Built-in (ESP32 core) |
| `WebServer.h` | HTTP server | Built-in (ESP32 core) |
| `Preferences.h` | Non-volatile storage | Built-in (ESP32 core) |
| `TimeLib.h` | Time functions | [PaulStoffregen/Time](https://github.com/PaulStoffregen/Time) |
| `Stepper.h` | Stepper motor control | Built-in (Arduino) |

---

## secrets.h — Required Local Configuration

This file is **gitignored** and must be created manually before compiling:

```cpp
#define SECRET_SSID "your_wifi_network_name"
#define SECRET_PASSWORD "your_wifi_password"
```

Never commit `secrets.h`. It is excluded via `.gitignore`.

---

## Code Architecture

### Entry Points

- **`setup()`** — Runs once on boot:
  1. Connects to WiFi (`setupWiFi()`)
  2. Loads timezone preferences from non-volatile storage (`setupTz()`)
  3. Initializes NTP time sync via UDP (syncs every 300 seconds)
  4. Configures ArduinoOTA
  5. Registers all HTTP route handlers and starts web server
  6. Sets stepper speed, records initial time, runs recycle cycle

- **`loop()`** — Runs continuously:
  1. Handles OTA upload requests
  2. Handles web server client requests
  3. Checks WiFi and reconnects if dropped
  4. Checks if a minute has passed → advances stepper proportionally
  5. Checks if an hour has passed → compensates for integer rounding drift (8 missing steps/hour)

### Key Global State

```cpp
unsigned long pMinute, cMinute;   // previous/current minute (for change detection)
unsigned long pHour, cHour;       // previous/current hour (for drift correction)
long timeZoneOffsetHours;         // from Preferences, default 0 (UTC)
long timeZoneOffsetMins;          // from Preferences, default 0
bool isDst;                       // from Preferences, default false
```

### Motor Step Math

- 1 revolution = 2048 steps = 1 hour on the clock face
- Each minute = `2048 / 60 = 34` steps (integer truncation)
- Missing steps per hour: `2048 - (34 × 60) = 8` — recovered at hour boundary
- Motor speed: 8 RPM max (conservative, hardware-limited)

---

## Web Server Endpoints

All served on port 80 at `http://wandering-hour-clock.local/` (mDNS) or the device IP.

| Method | Path | Handler | Description |
|---|---|---|---|
| GET | `/` | `handleRoot()` | Main UI: set time, adjust dial, preferences, debug info |
| POST | `/submit` | `handleFormSubmit()` | Set clock to a specific hour:minute |
| POST | `/forward-5` | `handleFormForward5()` | Advance dial 5 minutes |
| POST | `/backward-5` | `handleFormBackward5()` | Rewind dial 5 minutes |
| POST | `/recycle` | `handleFormRecycle()` | Motor test: full CCW then CW rotation |
| POST | `/demo` | `handleFormDemo()` | Demo: advance 12 full hours |
| POST | `/set-preferences` | `handleFormSetPreferences()` | Save timezone/DST to non-volatile storage |

### Dial Adjustment Logic (`handleDialAdjustments`)

Takes the time the user sees on the physical clock face, computes the difference from actual NTP time, and drives the motor by the corresponding number of steps to correct the display.

---

## Preferences / Non-Volatile Storage

Uses the ESP32 `Preferences` library with namespace `"whc"` (max 15 chars):

| Key | Type | Description | Valid Range |
|---|---|---|---|
| `tzhours` | `long` | UTC hour offset | -14 to +12 |
| `tzmins` | `long` | UTC minute offset | 0 to 59 |
| `isdst` | `bool` | DST currently active | true/false |

Preferences persist across power cycles. After changing timezone, manually resync the clock via the web UI.

**Timezone note:** For negative UTC offsets with minute components (e.g., UTC-3:30), `tzhours = -3` and `tzmins = 30`. The sign of the minutes offset is inferred from the hours sign in `getNtpMinute()`:
```cpp
secs += (timeZoneOffsetHours < 0 ? -timeZoneOffsetMins : timeZoneOffsetMins) * SECS_PER_MIN;
```

---

## NTP Time Sync

- Server: `us.pool.ntp.org` (UDP port 123)
- Local UDP port: 8888
- Sync interval: 300 seconds (5 minutes)
- NTP epoch offset from Unix epoch: `2208988800UL` seconds
- If NTP fails, returns `0` (TimeLib will retry)
- DST adds `SECS_PER_HOUR` (3600 seconds) when enabled

---

## WiFi Behavior

- Max retry count before ESP restart: 10 (with 1s delays between attempts)
- Hostname: `"wandering-hour-clock"` (appears in DHCP/mDNS)
- On disconnect during runtime, `loop()` calls `setupWiFi()` to reconnect
- **Hostname ordering quirk** (arduino-esp32 v2.0.14): `setHostname()` must be called before `config()`, `mode()`, and `begin()` for DHCP to report the correct name

---

## OTA (Over-The-Air) Updates

ArduinoOTA is configured and active. To update firmware wirelessly:
1. Open Arduino IDE
2. Select the device from Tools > Port (it should appear as a network port)
3. Upload normally

---

## Build & Flash Instructions

### Prerequisites
1. Install [Arduino IDE](https://www.arduino.cc/en/software)
2. Add ESP32 board support: Preferences > Additional Board Manager URLs → `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Install `TimeLib` via Library Manager
4. Create `secrets.h` with WiFi credentials (see above)

### Compile & Upload
- Select board: **ESP32 Dev Module** (or your specific ESP32 variant)
- Select the correct COM port
- Click Upload

### Command-line (arduino-cli)
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 wandering-hour-clock/
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/ttyUSB0 wandering-hour-clock/
```

### Serial Monitor
- Baud rate: **115200**
- Useful for debugging WiFi connection, NTP sync, and step counts

---

## Development Conventions

### Code Style
- Arduino C++ (`.ino` file — treated as C++ by the compiler)
- Procedural style: all logic in functions, no classes used
- Global variables for shared state (motor position tracking, preferences)
- `Serial.print()` used extensively for debugging — leave debug prints in place
- HTML for the web UI is built via string concatenation in `handleRoot()`

### Adding Web Endpoints
1. Declare the handler function before `setup()` if needed (or use forward declaration)
2. Register it in `setup()` with `server.on("/path", HTTP_METHOD, handlerFunction)`
3. Add a corresponding HTML form button in `handleRoot()`
4. Handler must call `server.send(statusCode, "text/plain", "message")`

### Modifying Stepper Behavior
- `stepsPerRev = 2048` — do not change unless using a different motor
- `maxSpeed = 8` RPM — keep conservative; higher speeds may miss steps
- Motor always runs clockwise for time advancement (positive `step()` values)
- Forward/Backward handlers do a full CCW+CW reset before the actual adjustment to ensure consistent needle positioning

### Preferences Changes
- Always open with `preferences.begin(pref_namespace, false)` for write mode
- Always close with `preferences.end()` after reading or writing
- Call `setupTz()` after saving to sync globals from storage
- Call `setTime(getNtpMinute())` after timezone changes to resync NTP immediately

### No Test Framework
There is no automated test suite. Validation is done:
1. Via Serial monitor output during development
2. Physically by observing the clock mechanism
3. Via the web UI debug info section showing `cHour:cMinute` and `pHour:pMinute`

---

## Known Limitations & Design Notes

- **Integer division drift:** `2048 / 60 = 34` (not 34.133...), causing 8 steps lost per hour. This is compensated in `loop()` at each hour boundary.
- **No HTTPS:** Web server uses plain HTTP — not suitable for use over untrusted networks.
- **Single-threaded:** The main loop handles OTA, web server, WiFi, and motor updates sequentially. Long motor operations (demo, recycle) block all other processing.
- **Forward/Backward adjustment quirk:** These handlers always do a full recycle first (CCW+CW), then apply the step offset. This works because the wandering hour mechanism relies on a physical reset position.
- **No SPIFFS/filesystem:** No files stored on flash; HTML is generated in-memory.
- **ESP.restart() on WiFi failure:** If WiFi cannot connect in 10 attempts at startup, the board hard-restarts.
