
/*
  4096  Steps/revolution
  1092  steps/second  (motor is guaranteed 500 steps/second)
  According to a datasheet, "Frequency" is 100Hz, which results in 1 rev in 40.96 sec.
  Max pull-in frequency = 500 Hz (8.1 sec/rev, 7.32 rpm),
  Max pull-out frequency = 900 hz (4.55 sec/rev, 13.2 rpm).
*/

/* It seems that the max RPM these motors can do is ~ 13 RPM, = ~ 1100
   mSec per step. It also depends on the amount of current your power
   supply can provide.
*/
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TimeLib.h>  // https://github.com/PaulStoffregen/Time
#include <WebServer.h>
#include <Preferences.h>

#include <Stepper.h>

#include "secrets.h"

// Replace the ssid and password in secrets.h
const char* ssid = SECRET_SSID;
const char* password = SECRET_PASSWORD;

// Hostname
const char* hostname = "wandering-hour-clock";

// Preferences library namespace and keys. The library
// limits the namespace and attrib length to 16 characters max.
const char* pref_namespace = "whc"; // "Wandering Hour Clock"
const char* attrib_tzhours = "tzhours";
const char* attrib_tzmins = "tzmins";
const char* attrib_isdst = "isdst";

const int stepsPerRev = 2048; /* steps / rev for stepper motor */
const int maxSpeed = 8;   /* max speed stepper RPM, conservative */

#define IN1 19
#define IN2 18
#define IN3 5
#define IN4 17

unsigned int updateIntervalMinutes = 1;
unsigned long pMinute = 0;
unsigned long cMinute;
unsigned long cHour;
unsigned long pHour;

// initialize web server library
WebServer server(80); // Create a WebServer object that listens on port 80

// initialize the stepper library
Stepper myStepper(stepsPerRev, IN1, IN3, IN2, IN4);

Preferences preferences;

// initialize UDP library
WiFiUDP udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
const int maxRetryCount = 10;   // WiFi connection attempts before giving up

bool wifiAvailable = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 30000; // ms between reconnect attempts in loop()

// Define the NTP server and timezone offset
static const char ntpServerName[] = "us.pool.ntp.org";
//static const char ntpServerName[] = "time.nist.gov";

long timeZoneOffsetHours = 0;
long timeZoneOffsetMins = 0;
bool isDst = false;

time_t getNtpMinute();
void sendNTPpacket(IPAddress &address);
void handleDialAdjustments(int, int);
void runRecycleCycle();


// Attempt to connect to WiFi. Returns true on success, false if all retries
// are exhausted. Never restarts the board — the clock can run without WiFi
// and will keep retrying in loop().
//
// Hostname ordering note (arduino-esp32 v2.0.14): setHostname() must be
// called before config(), mode(), and begin() for DHCP to report the right
// name. See https://github.com/tzapu/WiFiManager/issues/1403
bool setupWiFi() {
  WiFi.setHostname(hostname);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  for (int retries = 0; retries < maxRetryCount; retries++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to WiFi");
      Serial.println("IP address: " + WiFi.localIP().toString());
      return true;
    }
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }

  Serial.println("Failed to connect to WiFi. Continuing without WiFi.");
  return false;
}

void setupTz() {
  preferences.begin(pref_namespace, true); // Readonly mode

  // Default to UTC
  timeZoneOffsetHours = preferences.getLong(attrib_tzhours, 0);
  timeZoneOffsetMins = preferences.getLong(attrib_tzmins, 0);
  isDst = preferences.getBool(attrib_isdst, false);

  preferences.end();
}

// Start network-dependent services (OTA, mDNS). Called once WiFi is
// available — either at boot or when the connection is first established.
void startNetworkServices() {
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";
      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
  ArduinoOTA.begin();

  if (MDNS.begin(hostname)) {
    Serial.println("mDNS responder started");
  } else {
    Serial.println("Error setting up mDNS responder!");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Booting");

  // Attempt WiFi — the clock continues without it if unavailable
  wifiAvailable = setupWiFi();

  // Load timezone from non-volatile storage (works without WiFi)
  setupTz();

  // Register NTP sync provider. getNtpMinute() returns 0 if WiFi/NTP is
  // unavailable, so TimeLib retries safely every 300 s.
  udp.begin(localPort);
  Serial.print("NTP UDP local port: ");
  Serial.println(localPort);
  setSyncProvider(getNtpMinute);
  setSyncInterval(300);

  if (wifiAvailable) {
    // Wait up to 30 seconds for initial NTP sync
    Serial.println("Waiting for NTP sync...");
    unsigned long ntpDeadline = millis() + 30000;
    while (timeStatus() == timeNotSet && millis() < ntpDeadline) {
      delay(5000);
    }
    if (timeStatus() == timeNotSet) {
      Serial.println("NTP sync timed out. Clock will sync when reachable.");
    } else {
      Serial.println("NTP sync successful.");
    }

    startNetworkServices();
  } else {
    Serial.println("No WiFi — skipping NTP sync, OTA and mDNS at boot.");
    Serial.println("The clock will sync once WiFi becomes available.");
  }

  // Web server is always started; it becomes reachable once WiFi is up
  server.on("/", HTTP_GET, handleRoot);
  server.on("/submit", HTTP_POST, handleFormSubmit);
  server.on("/forward-5", HTTP_POST, handleFormForward5);
  server.on("/backward-5", HTTP_POST, handleFormBackward5);
  server.on("/recycle", HTTP_POST, handleFormRecycle);
  server.on("/demo", HTTP_POST, handleFormDemo);
  server.on("/set-preferences", HTTP_POST, handleFormSetPreferences);
  server.begin();

  myStepper.setSpeed(maxSpeed);

  time_t currentTime = now();
  tm localTime = *localtime(&currentTime);
  cMinute = pMinute = localTime.tm_min;
  cHour = pHour = localTime.tm_hour % 12;

  // Start up cycle
  runRecycleCycle();
}


void loop() {
  //  Handle Arduino OTA upload requests
  ArduinoOTA.handle();

  // Handle incoming client requests
  server.handleClient();

  // WiFi watchdog: non-blocking reconnect with rate limiting.
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiAvailable) {
      // Transition: was connected, just lost it
      wifiAvailable = false;
      Serial.println("Wi-Fi disconnected.");
    }
    unsigned long now_ms = millis();
    if (now_ms - lastReconnectAttempt >= reconnectInterval) {
      lastReconnectAttempt = now_ms;
      Serial.println("Attempting WiFi reconnect...");
      // Non-blocking: WiFi.begin() starts the association process in the
      // background; status is checked on the next loop iteration.
      WiFi.begin(ssid, password);
    }
  } else if (!wifiAvailable) {
    // Transition: WiFi just became available (first connect or reconnect)
    wifiAvailable = true;
    Serial.println("WiFi (re)connected. IP: " + WiFi.localIP().toString());
    startNetworkServices();
    // Trigger an immediate NTP sync
    time_t ntpTime = getNtpMinute();
    if (ntpTime > 0) {
      setTime(ntpTime);
      Serial.println("NTP sync complete after reconnect.");
    }
  }

  // Get the current time in seconds since January 1, 1970 (Unix time)
  time_t currentTime = now();

  // Convert the Unix time to local time
  tm localTime = *localtime(&currentTime);

  /* We go 1 rev = 2048 steps / hour */
  /* Every updateIntervalMinutes minutes, do a little move */
  cMinute = localTime.tm_min;

  /*
  Minute difference handles both linear advance and hour-boundary wrap:
    linear:  pMinute=1,  cMinute=5  → diffMinute=(5-1+60)%60=4
    wrap:    pMinute=59, cMinute=4  → diffMinute=(4-59+60)%60=5
  Using diffMinute directly avoids the rollover false-negative in the old
  condition (cMinute >= (pMinute + N) % 60) that broke near hour boundaries.
  */
  int diffMinute = ((int)cMinute - (int)pMinute + 60) % 60; /* minutes since last step */

  if (diffMinute >= (int)updateIntervalMinutes) { /* time for update? - every updateIntervalMinutes minutes */
    pMinute = cMinute;
    long cStep = (stepsPerRev * diffMinute) / 60;  /* desired motor position - 170 steps every 5 minutes */

    // Debug prints
    Serial.print(diffMinute);
    Serial.print(" minute(s), ");
    Serial.print(cStep);
    Serial.print(" steps");
    Serial.println();

    myStepper.step(cStep);
    return;
  }

  // Handle hour offsets
  cHour = localTime.tm_hour % 12;
  int diffHour = ((int)cHour - (int)pHour + 12) % 12;
  if (diffHour >= 1) {
    int stepsPerMinute = stepsPerRev / 60; // 1 rev = 60 minutes = 2048 steps => 34 (int) steps per minute instead of 34.133333
    int stepsPerHour = 60 * stepsPerMinute; // 34 * 60 = 2040 steps in 1 hour. => we are missing 8 steps every hour

    int missingSteps = stepsPerRev - stepsPerHour;  // 2048 - 2040 = 8
    Serial.print("Hour complete: recovering ");
    Serial.print(missingSteps * diffHour);
    Serial.print(" missed steps");
    Serial.println("");
    if (missingSteps > 0) {
      myStepper.step(missingSteps * diffHour);
    }
    pHour = cHour;
  }
}

void handleRoot() {
  String html = "<html><head>";
  html += "<title>Wandering Hour Clock</title>";
  html += "  <meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "  <style>";
  html += "    body {";
  html += "      font-family: Arial, Helvetica, sans-serif;";
  html += "    }";
  html += "";
  html += "    .container {";
  html += "      width: 100%;";
  html += "      max-width: 400px;";
  html += "      margin: 0 auto;";
  html += "      padding: 20px;";
  html += "    }";
  html += "";
  html += "    label {";
  html += "      display: block;";
  html += "      margin-bottom: 10px;";
  html += "    }";
  html += "";
  html += "    input[type='number'] {";
  html += "      width: 100%;";
  html += "      padding: 10px;";
  html += "      margin-bottom: 20px;";
  html += "      border: 1px solid #ccc;";
  html += "      border-radius: 4px;";
  html += "      box-sizing: border-box;";
  html += "    }";
  html += "";
  html += "    button {";
  html += "      background-color: #4CAF50;";
  html += "      color: white;";
  html += "      padding: 10px 20px;";
  html += "      border: none;";
  html += "      border-radius: 4px;";
  html += "      cursor: pointer;";
  html += "      width: 100%;";
  html += "    }";
  html += "";
  html += "    button:hover {";
  html += "      background-color: #45a049;";
  html += "    }";
  html += "  </style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h2>Set Time</h2>";
  html += "<h3>Set the time you see on the clock now. Click submit to adjust the dial to current time automatically</h3>";
  html += "<form method='POST' action='/submit'>";
  html += "<label for='hour'>Hour (1-12):</label><input type='number' id='hour' name='hour' min='1' max='12' required><br>";
  html += "<label for='minute'>Minute (0-59):</label><input type='number' id='minute' name='minute' min='0' max='59' required><br>";
  html += "<button type='submit'>Set Time</button></form>";
  html += "<form method='POST' action='/forward-5'><button type='submit'>+5m</button></form>";
  html += "<form method='POST' action='/backward-5'><button type='submit'>-5m</button></form>";
  html += "<form method='POST' action='/recycle'><button type='submit'>Recycle</button></form>";
  html += "<form method='POST' action='/demo'><button type='submit'>Demo</button></form>";

  html += "<h2>Preferences</h2>";
  html += "<form method='POST' action='/set-preferences'>";
  html += "<label for='hour_offset'>TZ hour offset:</label><input type='number' id='hour_offset' name='hour_offset' min='-14' max='12' value='" + String(timeZoneOffsetHours) + "' required><br>";
  html += "<label for='minute_offset'>TZ minute offset:</label><input type='number' id='minute_offset' name='minute_offset' min='0' max='59' value='" + String(timeZoneOffsetMins) + "' required><br>";
  html += "<label for='dst'>Daylight Savings Time currently in effect?</label><input type='checkbox' id='dst' name='dst'" + String(isDst ? " checked" : "") + "><br>";
  html += "<button type='submit'>Save preferences</button></form>";

  html += "<h2>Debug Info</h2>";
  html += "<div>cHour: cMinute = " + String(cHour) + ":" + (cMinute < 10 ? "0" : "") + String(cMinute) + "</div>";
  html += "<br/><div>pHour: pMinute = " + String(pHour) + ":" + (pMinute < 10 ? "0" : "") + String(pMinute) + "</div>";
  html += "<br/><div>timeZoneOffsetHours : timeZoneOffsetMins = " + String(timeZoneOffsetHours) + ":" + (timeZoneOffsetMins < 10 ? "0" : "") + String(timeZoneOffsetMins) + "</div>";
  html += "<br/><div>isDst = " + String(isDst ? "true" : "false") + "</div>";
  html += "<br/><div>hostname = " + String(hostname) + "</div>";
  html += "</div>";

  server.send(200, "text/html", html);
}

// Motor-only helper: full CCW then CW rotation to reset needle position.
// Safe to call from setup() and from HTTP handlers alike.
void runRecycleCycle() {
  Serial.println("full rotation counterclockwise");
  myStepper.step(-stepsPerRev);

  Serial.println("full rotation clockwise");
  myStepper.step(stepsPerRev);
}

void handleFormForward5() {
  runRecycleCycle();
  Serial.println("Jump 5m");
  myStepper.step((stepsPerRev * 5) / 60);
  server.send(200, "text/plain", "Moved 5 minutes Forward");
}

void handleFormBackward5() {
  runRecycleCycle();
  Serial.println("Jump -5m");
  myStepper.step(-1 * (stepsPerRev * 5) / 60);
  server.send(200, "text/plain", "Moved 5 minutes Backward");
}

void handleFormRecycle() {
  runRecycleCycle();
  server.send(200, "text/plain", "Cycle complete");
}


void handleFormDemo() {
  Serial.println("full rotation clockwise");
  myStepper.step(stepsPerRev*12);

  server.send(200, "text/plain", "Demo 12h Cycle complete");
}

void handleDialAdjustments(int iHour, int iMinute) {

  time_t currentTime = now();
  // Convert the Unix time to local time
  tm localTime = *localtime(&currentTime);

  cMinute = pMinute = localTime.tm_min;
  cHour = pHour = localTime.tm_hour % 12;

  // Print the local time to the serial monitor
  Serial.print("Current time: ");
  Serial.print(cHour);
  Serial.print(":");
  Serial.println(localTime.tm_min);

  // Parse the input time in hours and minutes
  Serial.print("Input time: ");
  Serial.print(iHour);
  Serial.print(":");
  Serial.println(iMinute);

  // Normalize input hour to 0-11 range (form sends 1-12; 12 maps to 0)
  int normalizedInputHour = iHour % 12;

  // Compute total minutes within a 12-hour window (0-719) for each time
  int actualTotal = cHour * 60 + (int)cMinute;
  int inputTotal  = normalizedInputHour * 60 + iMinute;

  // Difference: positive = clock is behind actual (advance motor)
  //             negative = clock is ahead of actual (rewind motor)
  // Normalize to the shortest path within ±360 minutes (half of 720).
  int minuteDifference = actualTotal - inputTotal;
  if (minuteDifference >  360) minuteDifference -= 720;
  if (minuteDifference < -360) minuteDifference += 720;

  Serial.print("Time difference: ");
  Serial.print(minuteDifference);
  Serial.println(" minutes");

  // Handle adjustments
  int steps = minuteDifference * stepsPerRev / 60; // 60 minutes = stepsPerRev => timeDiff * stepsPerRev / 60 offset steps required
  Serial.print(steps);
  Serial.println(" adjusting steps");
  myStepper.step(steps);

}

void handleFormSetPreferences() {
  if (server.hasArg("hour_offset") && server.hasArg("minute_offset")) {
    int tmp_hour_offset = server.arg("hour_offset").toInt();
    int tmp_minute_offset = server.arg("minute_offset").toInt();
    bool tmp_dst = server.hasArg("dst");

    if (tmp_hour_offset >= -14 && tmp_hour_offset <= 12 && tmp_minute_offset >= 0 && tmp_minute_offset <= 59) {
      Serial.print("Setting hour offset: ");
      Serial.println(tmp_hour_offset);
      Serial.print("Setting minute offset: ");
      Serial.println(tmp_minute_offset);
      Serial.print("Setting DST: ");
      Serial.println(tmp_dst ? "true" : "false");

      preferences.begin(pref_namespace, false); // read/write mode
      preferences.putLong(attrib_tzhours, tmp_hour_offset);
      preferences.putLong(attrib_tzmins, tmp_minute_offset);
      preferences.putBool(attrib_isdst, tmp_dst);
      preferences.end();

      // Reread and sync the global variables with the preferences values
      setupTz();

      // Force an immediate NTP resync to apply the new timezone offset.
      // Skipped if WiFi is unavailable — TimeLib will sync at the next
      // scheduled interval once the network is reachable.
      if (wifiAvailable) {
        time_t ntpTime = getNtpMinute();
        if (ntpTime > 0) {
          setTime(ntpTime);
        }
      }

      // Return a success message to the client
      server.send(200, "text/plain", "Preferences set successfully");

    } else {
      server.send(400, "text/plain", "Invalid values");
    }
  } else {
    server.send(400, "text/plain", "Missing fields");
  }
}

void handleFormSubmit() {
  // Check if the form was submitted
  if (server.hasArg("hour") && server.hasArg("minute")) {
    // Parse the hour and minute values from the form
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();

    // Validate the values
    if (hour >= 1 && hour <= 12 && minute >= 0 && minute <= 59) {
      // Print the values to the Serial monitor
      Serial.print("Hour: ");
      Serial.println(hour);
      Serial.print("Minute: ");
      Serial.println(minute);

      handleDialAdjustments(hour, minute);

      // Return a success message to the client
      server.send(200, "text/plain", "Time set successfully");
    } else {
      // Return an error message to the client
      server.send(400, "text/plain", "Invalid values");
    }
  } else {
    // Return an error message to the client
    server.send(400, "text/plain", "Missing fields");
  }
}

/*-------- NTP code ----------*/
// Ref: https://github.com/PaulStoffregen/Time/blob/master/examples/TimeNTP_ESP8266WiFi/TimeNTP_ESP8266WiFi.ino

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpMinute()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];

      // Adjust from UTC to local time zone
      unsigned long secs = secsSince1900 - 2208988800UL;
      secs += timeZoneOffsetHours * SECS_PER_HOUR;
      secs += (timeZoneOffsetHours < 0 ? -timeZoneOffsetMins : timeZoneOffsetMins) * SECS_PER_MIN;
      secs += (isDst ? SECS_PER_HOUR : 0);

      return secs;

    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}
