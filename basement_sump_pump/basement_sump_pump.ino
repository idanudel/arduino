// Basement sump pump water level sensor on an ESP8266. Imported from a
// personal backup (Arduino 2/waterLevel) and brought up to this repo's
// standard firmware pattern (WiFi/MQTT reconnect, LWT, debounce, retained
// publish, secrets.h) - see CLAUDE.md.
//
// The original sketch would not compile: its subscribe-topic constant was
// `= \n .c_str();` with no object to call .c_str() on (a leftover of the
// same self-referencing-String bug seen in the sibling basement_kitchenette
// sketch). Rebuilt here to subscribe on the base topic, matching every
// other device in this repo.
//
// Wiring (unchanged from the original sketch):
//   Water level switch -> GPIO2 (D4).

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

const char* mqtt_client_name = "basement_sump_pump";

// Topics kept identical to the original sketch's runtime values (note
// "sumppump", not "sump_pump" - preserved for compatibility).
const char* mqtt_topic_command = "openhab/devices/basement/sumppump/";
const char* mqtt_topic_waterlevel = "openhab/devices/basement/sumppump/waterlevel";
const char* mqtt_topic_status = "openhab/devices/basement/sumppump/status";

const int waterLevelPin = 2; // GPIO2 / D4

// Debounce: only trust a level state once this many consecutive loop
// iterations (LOOP_DELAY_MS apart) agree, so a single noisy read can't
// trigger a false publish.
const unsigned long LOOP_DELAY_MS = 200;
const int DEBOUNCE_COUNT = 3;
int candidateLevelState = -1;
int candidateLevelCount = 0;
int lastPublishedLevel = -1; // force a publish on first stable read

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  Serial.begin(115200);
  pinMode(waterLevelPin, INPUT);
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
  publishLevel();
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Last will: if this device disconnects uncleanly (crash/power loss/
    // WiFi drop), the broker publishes "offline" to mqtt_topic_status for
    // us - worth alerting on for a device guarding against a flooded sump.
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
  updateLevel();
  delay(LOOP_DELAY_MS);
}

void updateLevel() {
  int reading = digitalRead(waterLevelPin);

  if (reading == candidateLevelState) {
    candidateLevelCount++;
  } else {
    candidateLevelState = reading;
    candidateLevelCount = 1;
  }

  if (candidateLevelCount >= DEBOUNCE_COUNT &&
      candidateLevelState != lastPublishedLevel) {
    lastPublishedLevel = candidateLevelState;
    publishLevel();
  }
}

void publishLevel() {
  if (lastPublishedLevel < 0) {
    return;
  }
  // "0" on HIGH (water level triggered), "1" on LOW (normal) - kept as the
  // original sketch published it. Looks inverted but is a consistent,
  // deliberate convention across this author's other motion/level sketches
  // (garage_2in1, basement_kitchenette_2in1) - do not "fix" without
  // checking those too.
  Serial.print("water level: ");
  Serial.println(lastPublishedLevel);
  client.publish(mqtt_topic_waterlevel, lastPublishedLevel == HIGH ? "0" : "1", true);
}
