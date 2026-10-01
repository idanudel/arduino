// Water heater (boiler) refill valve controller: ESP8266 + 1-channel relay
// switching a 12V normally-closed solenoid valve. openHAB sends a one-shot
// "FILL" request over MQTT; this device opens the valve and closes it
// ITSELF after a fixed time. Nothing on the network is ever responsible for
// closing the valve.
//
// Companion to water_heater_level/ (the XKC-Y25V low-water sensor on the
// same tank) - separate device, separate sketch, no shared state.
//
// Safety design (see README.md for the full reasoning):
// - Relay output is driven to OFF before anything else in setup(), and the
//   chosen GPIO (GPIO5/D1) is high-impedance with no boot-time toggling.
// - Fill timing is local: the main loop closes the valve at FILL_DURATION,
//   and an independent hardware-timer failsafe (timer1 interrupt - a real
//   ISR, fires even if loop() is busy-waiting, unlike Ticker/os_timer)
//   closes it at FILL_DURATION + FAILSAFE_MARGIN if loop() is stalled.
//   A fully hung CPU trips the watchdog, which resets the chip -> relay
//   OFF at boot.
// - STOP always forces the relay OFF immediately, whatever the state.
// - The loop never blocks waiting for WiFi/MQTT, and never attempts an MQTT
//   (re)connect while the valve is open.
// - A FILL while FILLING/COOLDOWN/ERROR is rejected - never extends a fill.
// - Stale commands can't fire: clean MQTT session (nothing queued while
//   offline), any retained message on the request topic is wiped before
//   subscribing, and commands arriving in the first moments after
//   (re)subscribing are ignored.
// - If the chip resets while the valve was open (crash/watchdog/brownout),
//   an RTC-memory marker survives the reset and the device boots into
//   ERROR, refusing fills until an explicit RESET command.

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include "secrets.h"

// Device identity - see repo CLAUDE.md naming pattern.
#define ROOM "utility_room"
#define DEVICE "water_fill"
#define TOPIC_BASE "openhab/devices/" ROOM "/" DEVICE

const char* mqtt_client_name = ROOM "_" DEVICE;
const char* mqtt_topic_request       = TOPIC_BASE "/request";        // in: FILL | FILL <sec> | STOP | RESET (never retained)
const char* mqtt_topic_status        = TOPIC_BASE "/status";         // retained online/offline (LWT)
const char* mqtt_topic_state         = TOPIC_BASE "/state";          // retained IDLE/FILLING/COOLDOWN/ERROR
const char* mqtt_topic_available     = TOPIC_BASE "/available";      // retained YES/NO
const char* mqtt_topic_last_result   = TOPIC_BASE "/last_result";    // retained, outcome of last fill
const char* mqtt_topic_last_duration = TOPIC_BASE "/last_duration";  // retained, seconds valve was actually open
const char* mqtt_topic_last_fill     = TOPIC_BASE "/last_fill_time"; // retained, ISO-8601 UTC
const char* mqtt_topic_event         = TOPIC_BASE "/event";          // retained, last command response / boot info

// ---- Configuration --------------------------------------------------------
const unsigned long FILL_DURATION_MS   = 10000; // default fill for a plain "FILL"
const unsigned long MAX_FILL_MS        = 10000; // hard cap, also caps "FILL <sec>"
const unsigned long COOLDOWN_MS        = 30000; // fills rejected for this long after valve closes
const unsigned long FAILSAFE_MARGIN_MS = 1000;  // timer1 ISR closes valve this long after the loop should have
const unsigned long STARTUP_IGNORE_MS  = 2000;  // ignore commands right after (re)subscribing
const unsigned long MQTT_RETRY_MS      = 5000;
const unsigned long OFFLINE_REBOOT_MS  = 15UL * 60UL * 1000UL; // reboot if no MQTT this long (only when idle)

// Relay: GPIO5 = D1. Boot-safe: not a strapping pin, high-impedance during
// boot/flash, no boot-time pulses (unlike GPIO0/2/15/16 or TX/RX).
constexpr int RELAY_PIN = 5;
// true  = relay energizes when GPIO is HIGH (transistor-input "high level
//         trigger" module, or an active-low module behind an NPN inverter).
// false = relay energizes when GPIO is LOW (bare optocoupler "low level
//         trigger" module). Verify in Stage 1 - see README.
// THIS IS THE ONLY POLARITY SETTING - relayOff()/relayOn(), boot init, and
// the timer1 failsafe ISR all derive from it, so changing it keeps every
// OFF path correct.
constexpr bool RELAY_ACTIVE_HIGH = true;
constexpr uint8_t RELAY_ON_LEVEL  = RELAY_ACTIVE_HIGH ? HIGH : LOW;
constexpr uint8_t RELAY_OFF_LEVEL = RELAY_ACTIVE_HIGH ? LOW : HIGH;
static_assert(RELAY_ON_LEVEL != RELAY_OFF_LEVEL, "relay ON/OFF levels must differ");
static_assert(RELAY_PIN >= 0 && RELAY_PIN < 16, "GPOS/GPOC only cover GPIO0-15");

// timer1 at DIV256 = 312.5kHz (3.2us/tick), 23-bit counter -> max ~26.8s.
const uint32_t TIMER1_TICKS_PER_MS = 312500UL / 1000UL; // 312 (rounds down -> fires slightly early, never late)
static_assert((MAX_FILL_MS + FAILSAFE_MARGIN_MS) * 312UL < 8388607UL,
              "failsafe delay exceeds timer1 range");

// ---- State ----------------------------------------------------------------
enum FillState { IDLE, FILLING, COOLDOWN, ERROR_STATE };
const char* stateNames[] = { "IDLE", "FILLING", "COOLDOWN", "ERROR" };

FillState state = IDLE;
unsigned long fillStartMs = 0;
unsigned long fillDurationMs = 0;
unsigned long cooldownStartMs = 0;
unsigned long subscribedAtMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastMqttConnectedMs = 0;
bool wifiWasConnected = false;
volatile bool failsafeTripped = false;
String bootEvent;

WiFiClient espClient;
PubSubClient client(espClient);

// RTC user memory survives a reset (not a power loss) - used to detect "the
// chip reset while the valve was open".
const uint32_t RTC_MAGIC = 0xB01E5AFE;
struct RtcMarker { uint32_t magic; uint32_t filling; };

void writeRtcFilling(bool filling) {
  RtcMarker m = { RTC_MAGIC, filling ? 1u : 0u };
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&m, sizeof(m));
}

bool rtcSaysWasFilling() {
  RtcMarker m;
  ESP.rtcUserMemoryRead(0, (uint32_t*)&m, sizeof(m));
  return m.magic == RTC_MAGIC && m.filling == 1;
}

// ---- Relay ----------------------------------------------------------------
void relayOff() { digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL); }
void relayOn()  { digitalWrite(RELAY_PIN, RELAY_ON_LEVEL); }
bool relayIsOn() { return digitalRead(RELAY_PIN) == RELAY_ON_LEVEL; }

// timer1 hardware ISR, independent of loop(). Minimal ISR-safe work only:
// one GPIO register write + a volatile flag. No MQTT/Serial/String/WiFi,
// no flash-resident calls (IRAM only).
// The OFF level comes from RELAY_ACTIVE_HIGH (compile-time), the same as
// relayOff(): active-high -> GPOC (drive LOW), active-low -> GPOS (drive HIGH).
void IRAM_ATTR failsafeClose() {
  if constexpr (RELAY_OFF_LEVEL == LOW) {
    GPOC = (1 << RELAY_PIN);
  } else {
    GPOS = (1 << RELAY_PIN);
  }
  failsafeTripped = true;
}

void armFailsafe(unsigned long ms) {
  timer1_disable();
  timer1_attachInterrupt(failsafeClose);
  timer1_enable(TIM_DIV256, TIM_EDGE, TIM_SINGLE);
  timer1_write(ms * TIMER1_TICKS_PER_MS);
}

void disarmFailsafe() {
  timer1_disable();
  timer1_detachInterrupt();
}

// ---- MQTT publish helpers ---------------------------------------------------
void publishRetained(const char* topic, const String& value) {
  if (client.connected()) {
    client.publish(topic, value.c_str(), true);
  }
}

void publishState() {
  publishRetained(mqtt_topic_state, stateNames[state]);
  publishRetained(mqtt_topic_available, state == IDLE ? "YES" : "NO");
}

void setState(FillState s) {
  state = s;
  Serial.printf("[%lu] state -> %s\n", millis(), stateNames[s]);
  publishState();
}

void event(const String& msg) {
  Serial.printf("[%lu] event: %s\n", millis(), msg.c_str());
  publishRetained(mqtt_topic_event, msg);
}

String isoTimeNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) return ""; // NTP not synced yet
  struct tm t;
  gmtime_r(&now, &t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

// ---- Fill control -----------------------------------------------------------
void startFill(unsigned long durationMs) {
  if (durationMs > MAX_FILL_MS) durationMs = MAX_FILL_MS;
  fillDurationMs = durationMs;
  fillStartMs = millis();
  failsafeTripped = false;
  writeRtcFilling(true);
  armFailsafe(durationMs + FAILSAFE_MARGIN_MS);
  // (failsafe armed BEFORE the relay turns on, so there's no window where
  // the valve is open without it)
  relayOn();
  setState(FILLING);
  event("fill started (" + String(durationMs / 1000.0, 1) + "s)");
  String ts = isoTimeNow();
  if (ts.length()) publishRetained(mqtt_topic_last_fill, ts);
}

// Close the valve and record the outcome. Valve closes FIRST, before any
// network I/O.
void endFill(const String& result, bool toError) {
  relayOff();
  disarmFailsafe();
  writeRtcFilling(false);
  unsigned long openMs = millis() - fillStartMs;
  Serial.printf("[%lu] valve closed after %lums: %s\n", millis(), openMs, result.c_str());
  publishRetained(mqtt_topic_last_duration, String(openMs / 1000.0, 1));
  publishRetained(mqtt_topic_last_result, result);
  if (toError) {
    setState(ERROR_STATE);
  } else {
    cooldownStartMs = millis();
    setState(COOLDOWN);
  }
}

void handleCommand(String cmd, bool fromSerial) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  // STOP first, before any filtering: relay OFF immediately, in any state,
  // even inside the post-connect ignore window.
  if (cmd == "STOP") {
    relayOff();
    if (state == FILLING) {
      endFill("stopped by command", false);
      event("stopped by command");
    } else {
      event("STOP: relay forced OFF (was not filling)");
    }
    return;
  }

  if (!fromSerial && millis() - subscribedAtMs < STARTUP_IGNORE_MS) {
    event("ignored '" + cmd + "': just (re)connected, possible stale command");
    return;
  }

  if (cmd == "RESET") {
    if (state == ERROR_STATE) {
      relayOff();
      setState(IDLE);
      event("error cleared by RESET");
    } else {
      event("RESET ignored: not in ERROR");
    }
    return;
  }

  if (cmd == "FILL" || cmd.startsWith("FILL ")) {
    unsigned long durationMs = FILL_DURATION_MS;
    if (cmd.length() > 5) {
      long sec = cmd.substring(5).toInt();
      if (sec <= 0 || (unsigned long)sec * 1000UL > MAX_FILL_MS) {
        event("rejected: duration must be 1-" + String(MAX_FILL_MS / 1000) + "s");
        return;
      }
      durationMs = (unsigned long)sec * 1000UL;
    }
    switch (state) {
      case IDLE:
        startFill(durationMs);
        break;
      case FILLING:
        event("rejected: already filling (timer NOT extended)");
        break;
      case COOLDOWN: {
        unsigned long left = (COOLDOWN_MS - (millis() - cooldownStartMs)) / 1000 + 1;
        event("rejected: cooldown, " + String(left) + "s left");
        break;
      }
      case ERROR_STATE:
        event("rejected: device in ERROR, send RESET first");
        break;
    }
    return;
  }

  event("rejected: unknown command '" + cmd + "'");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.printf("[%lu] MQTT [%s] '%s'\n", millis(), topic, message.c_str());
  if (strcmp(topic, mqtt_topic_request) == 0) {
    handleCommand(message, false);
  }
}

// Bench testing over USB serial (115200, newline-terminated): same commands
// as MQTT (FILL, FILL <sec>, STOP, RESET), works with no network at all,
// plus WIFIOFF <sec> to simulate a WiFi outage (e.g. mid-fill).
unsigned long wifiOffUntilMs = 0;
String serialLine;

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (serialLine.length() < 32) serialLine += c;
      continue;
    }
    String cmd = serialLine;
    serialLine = "";
    cmd.trim();
    cmd.toUpperCase();
    if (cmd.length() == 0) continue;
    Serial.printf("[%lu] serial command: %s\n", millis(), cmd.c_str());
    if (cmd.startsWith("WIFIOFF")) {
      long sec = cmd.length() > 8 ? cmd.substring(8).toInt() : 30;
      if (sec <= 0) sec = 30;
      Serial.printf("simulating WiFi outage for %lds\n", sec);
      client.disconnect();
      WiFi.disconnect();
      wifiOffUntilMs = millis() + sec * 1000UL;
      if (wifiOffUntilMs == 0) wifiOffUntilMs = 1;
    } else {
      handleCommand(cmd, true);
    }
  }
}

// ---- Connectivity (non-blocking) ---------------------------------------------
void setup_wifi() {
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // No waiting here - loop() runs regardless of WiFi state.
}

void tryMqttConnect() {
  Serial.print("Attempting MQTT connection...");
  // Clean session (PubSubClient default) - broker queues nothing for us
  // while we're offline, so no stale command can be delivered on reconnect.
  if (client.connect(mqtt_client_name, MQTT_USER, MQTT_PASSWORD,
                     mqtt_topic_status, 1, true, "offline")) {
    Serial.println("connected");
    client.publish(mqtt_topic_status, "online", true);
    // Wipe any retained command someone may have published by mistake,
    // BEFORE subscribing, so it can never be replayed to us.
    client.publish(mqtt_topic_request, "", true);
    client.subscribe(mqtt_topic_request, 1);
    subscribedAtMs = millis();
    lastMqttConnectedMs = millis();
    publishState();
    if (bootEvent.length()) {
      event(bootEvent);
      bootEvent = "";
    }
  } else {
    Serial.print("failed, rc=");
    Serial.println(client.state());
  }
}

void maintainConnectivity() {
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasConnected) {
    wifiWasConnected = wifiUp;
    if (wifiUp) {
      Serial.print("WiFi connected, IP ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("WiFi lost (auto-reconnecting)");
    }
  }

  if (client.connected()) {
    lastMqttConnectedMs = millis();
    return;
  }
  // Valve open: no network recovery of any kind (connect attempts block).
  if (state == FILLING) return;

  // Recovery: no MQTT for a long time (WiFi down or not) -> reboot. Only
  // reached when not FILLING; boot is safe (relay OFF first thing).
  if (millis() - lastMqttConnectedMs > OFFLINE_REBOOT_MS) {
    Serial.println("No MQTT for too long, restarting");
    relayOff();
    delay(100);
    ESP.restart();
  }

  if (!wifiUp) return;
  if (millis() - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = millis();
  tryMqttConnect();
}

// ---- Main ---------------------------------------------------------------------
void setup() {
  // FIRST: preload the output latch with OFF, then switch to output - no
  // glitch to ON. (ESP8266 digitalWrite sets the latch even in INPUT mode.)
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();

  Serial.begin(115200);
  delay(50);
  Serial.println();
  String reason = ESP.getResetReason();
  Serial.printf("water_fill boot, reset reason: %s\n", reason.c_str());
  Serial.printf("fill=%lums max=%lums cooldown=%lums relay_active_high=%d pin=GPIO%d\n",
                FILL_DURATION_MS, MAX_FILL_MS, COOLDOWN_MS, RELAY_ACTIVE_HIGH, RELAY_PIN);

  if (rtcSaysWasFilling()) {
    writeRtcFilling(false);
    state = ERROR_STATE;
    bootEvent = "ERROR: reset while valve was open (" + reason + "), send RESET";
    Serial.println(bootEvent);
  } else {
    writeRtcFilling(false);
    state = IDLE;
    bootEvent = "boot: " + reason;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setup_wifi();
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);
  client.setSocketTimeout(5);
  lastMqttConnectedMs = millis();
}

void loop() {
  // Safety checks first, every iteration, regardless of network state.
  if (state == FILLING) {
    if (failsafeTripped) {
      // The hardware-timer failsafe had to close the valve: loop() was
      // stalled past the deadline. Valve is already closed; latch ERROR.
      endFill("ERROR: failsafe timer closed valve (loop stalled)", true);
    } else if (millis() - fillStartMs >= fillDurationMs) {
      endFill("completed", false);
    }
  } else {
    if (relayIsOn()) {
      // Should be impossible; never leave the valve open outside FILLING.
      relayOff();
      event("ERROR: relay found ON outside FILLING, forced OFF");
      if (state != ERROR_STATE) setState(ERROR_STATE);
    }
    if (state == COOLDOWN && millis() - cooldownStartMs >= COOLDOWN_MS) {
      setState(IDLE);
    }
  }

  pollSerial();
  if (wifiOffUntilMs) {
    if ((long)(millis() - wifiOffUntilMs) < 0) {
      delay(10);
      return; // simulated outage: skip all networking (safety checks above still ran)
    }
    wifiOffUntilMs = 0;
    Serial.println("simulated outage over, reconnecting WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  maintainConnectivity();
  if (client.connected()) {
    client.loop();
  }
  delay(10);
}
