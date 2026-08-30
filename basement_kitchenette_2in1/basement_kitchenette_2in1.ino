// Basement kitchenette "2-in-1" sensor: DS18B20 temperature + PIR motion on
// one ESP8266. Imported from a personal backup (Arduino 2/k_basement) and
// brought up to this repo's standard firmware pattern (WiFi/MQTT reconnect,
// LWT, debounce, retained publish, secrets.h) - see CLAUDE.md. MQTT topics
// below are kept byte-for-byte identical to the original sketch's runtime
// values in case this device is already wired into openHAB.
//
// Wiring (unchanged from the original sketch):
//   DS18B20 data -> GPIO2 (D4), needs a 4.7k pull-up to 3.3V.
//   PIR OUT      -> GPIO9. NOTE: GPIO9/10 are wired to the onboard flash
//   chip on most NodeMCU boards and are often not usable/exposed on the
//   header. This was carried over unchanged from the original sketch -
//   verify motion actually works on the physical board; move it to a
//   normal free GPIO (e.g. D1/D2, avoiding GPIO2 already used above) if not.

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h"

const char* mqtt_client_name = "basement_kitchenette_2in1";

// Topics kept identical to the original sketch's runtime values (note the
// extra "kitchenette" path segment, and the trailing slash on the command
// topic - both preserved for compatibility rather than "cleaned up").
const char* mqtt_topic_command = "openhab/devices/basement/kitchenette/2in1/";
const char* mqtt_topic_temp = "openhab/devices/basement/kitchenette/2in1/temp";
const char* mqtt_topic_motion = "openhab/devices/basement/kitchenette/2in1/motion";
const char* mqtt_topic_status = "openhab/devices/basement/kitchenette/2in1/status";

const int oneWirePin = 2; // GPIO2 / D4
const int motionPin = 9;  // see wiring note above

OneWire oneWire(oneWirePin);
DallasTemperature DS18B20(&oneWire);

// Temperature is only re-requested on this interval, not every loop.
// DS18B20.requestTemperatures() blocks for ~750ms at default resolution -
// doing that every loop iteration would delay motion detection. Motion is
// polled every loop instead, cheaply.
const unsigned long TEMP_INTERVAL_MS = 10000;
unsigned long lastTempRequest = 0;
float lastPublishedTemp = NAN;
const float TEMP_CHANGE_THRESHOLD = 0.5;

// Debounce: only trust a motion state once this many consecutive loop
// iterations (LOOP_DELAY_MS apart) agree, so a single noisy read can't
// trigger a false publish.
const unsigned long LOOP_DELAY_MS = 200;
const int DEBOUNCE_COUNT = 3;
int candidateMotionState = -1;
int candidateMotionCount = 0;
int lastPublishedMotion = -1; // force a publish on first stable read

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  Serial.begin(115200);
  pinMode(motionPin, INPUT);
  DS18B20.begin();
  setup_wifi();
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  // Any message on the command topic triggers an immediate re-publish of
  // current state. Replaces the old sketch's unfinished/broken magic-byte
  // commands ('2' was a no-op "todo", '9' published to a topic built from
  // a self-referencing, uninitialized String).
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] - republishing current state");
  publishTemp();
  publishMotion();
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Last will: if this device disconnects uncleanly (crash/power loss/
    // WiFi drop), the broker publishes "offline" to mqtt_topic_status for us.
    if (client.connect(mqtt_client_name, MQTT_USER, MQTT_PASSWORD,
                        mqtt_topic_status, 1, true, "offline")) {
      Serial.println("connected");
      client.publish(mqtt_topic_status, "online", true);
      client.subscribe(mqtt_topic_command);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  }
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  updateMotion();
  updateTemp();
  delay(LOOP_DELAY_MS);
}

void updateTemp() {
  unsigned long now = millis();
  if (now - lastTempRequest < TEMP_INTERVAL_MS) {
    return;
  }
  lastTempRequest = now;

  DS18B20.requestTemperatures();
  float reading = DS18B20.getTempCByIndex(0);

  if (isnan(lastPublishedTemp) ||
      fabs(reading - lastPublishedTemp) >= TEMP_CHANGE_THRESHOLD) {
    lastPublishedTemp = reading;
    publishTemp();
  }
}

void publishTemp() {
  if (isnan(lastPublishedTemp)) {
    return;
  }
  char buffer[16];
  snprintf(buffer, sizeof buffer, "%.1f", lastPublishedTemp);
  Serial.print("temp: ");
  Serial.println(buffer);
  client.publish(mqtt_topic_temp, buffer, true);
}

void updateMotion() {
  int reading = digitalRead(motionPin);

  if (reading == candidateMotionState) {
    candidateMotionCount++;
  } else {
    candidateMotionState = reading;
    candidateMotionCount = 1;
  }

  if (candidateMotionCount >= DEBOUNCE_COUNT &&
      candidateMotionState != lastPublishedMotion) {
    lastPublishedMotion = candidateMotionState;
    publishMotion();
  }
}

void publishMotion() {
  if (lastPublishedMotion < 0) {
    return;
  }
  // "0" on HIGH (motion detected), "1" on LOW (idle) - kept as the original
  // sketch published it. Looks inverted but is a consistent, deliberate
  // convention across this author's other motion/level sketches
  // (garage_2in1, basement_sump_pump) - do not "fix" without checking those too.
  Serial.print("motion: ");
  Serial.println(lastPublishedMotion);
  client.publish(mqtt_topic_motion, lastPublishedMotion == HIGH ? "0" : "1", true);
}
