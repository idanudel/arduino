// Garage "2-in-1" sensor: DS18B20 temperature + PIR motion on one ESP8266.
// Imported from an older standalone sketch (ard/motionTemp) and brought up
// to this repo's standard firmware pattern (WiFi/MQTT reconnect, LWT,
// debounce, retained publish, secrets.h) - see CLAUDE.md.
//
// Wiring (unchanged from the original sketch):
//   DS18B20 data -> GPIO0 (D3), needs a 4.7k pull-up to 3.3V.
//   PIR OUT      -> GPIO4 (D2).

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h"

// Device identity - change just these two lines when copying this sketch
// for a new project/room. Adjacent string literals concatenate at compile
// time, so client name and topics below derive from these automatically.
#define ROOM "garage"
#define DEVICE "2in1"

const char* mqtt_client_name = ROOM "_" DEVICE;
// Command topic: any published message here triggers an immediate
// re-publish of the last known temp/motion state (useful for an openHAB
// rule or REST call that wants current status without waiting for the
// next change). Replaces the old sketch's single-digit magic-byte commands.
const char* mqtt_topic_command = "openhab/devices/" ROOM "/" DEVICE;
const char* mqtt_topic_temp = "openhab/devices/" ROOM "/" DEVICE "/temp";
const char* mqtt_topic_motion = "openhab/devices/" ROOM "/" DEVICE "/motion";
// Retained "online"/"offline" (via MQTT last will) so openhab knows if this
// device itself has died, not just what it last reported.
const char* mqtt_topic_status = "openhab/devices/" ROOM "/" DEVICE "/status";

const int oneWirePin = 0; // GPIO0 / D3
const int motionPin = 4;  // GPIO4 / D2

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
  // sketch published it. This looks inverted but turns out to be a
  // consistent, deliberate convention across this author's other
  // motion/level sketches (basement_kitchenette_2in1, basement_sump_pump),
  // not a one-off bug - do not "fix" this without checking the others too.
  Serial.print("motion: ");
  Serial.println(lastPublishedMotion);
  client.publish(mqtt_topic_motion, lastPublishedMotion == HIGH ? "0" : "1", true);
}
