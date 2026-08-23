// XKC-Y25V non-contact liquid level sensor -> MQTT -> openhab.
// Reports whether water is present at the sensor's mounted height on the
// steam water heater tank (e.g. a "low water" warning point).
//
// XKC-Y25V wiring (3 wires): brown = VCC (5-12V), blue = GND, black = OUT.
// OUT is an NPN open-collector output: it only ever pulls to GND when
// triggered, it never sources 5V. That makes it safe to read directly with
// an ESP8266 3.3V GPIO in INPUT_PULLUP mode (the ESP's internal pull-up
// supplies the "high" side, the sensor just sinks it to ground) - no
// external level shifter needed. VCC must come from a separate 5V supply;
// tie its GND to the ESP8266's GND.
// Verify with a multimeter against your specific sensor's datasheet before
// wiring it up - some variants/output modes differ.

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

// Device identity - change just these two lines when copying this sketch
// for a new project/room. Adjacent string literals concatenate at compile
// time, so client name and topic below derive from these automatically.
#define ROOM "utility_room"
#define DEVICE "water_level"

const char* mqtt_client_name = ROOM "_" DEVICE;
const char* mqtt_topic_level = "openhab/devices/" ROOM "/" DEVICE;
// Retained "online"/"offline" (via MQTT last will) so openhab knows if this
// device itself has died, not just what it last reported.
const char* mqtt_topic_status = "openhab/devices/" ROOM "/" DEVICE "/status";

const int levelPin = 14; // D5 on NodeMCU/Wemos D1 mini

// Debounce: only trust a state once this many consecutive loop iterations
// (2s apart) agree, so a single noisy read near the sensor's threshold
// can't trigger a false publish.
const int DEBOUNCE_COUNT = 3;
int candidateState = -1;
int candidateCount = 0;
int lastPublishedState = -1; // force a publish on first stable read

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  Serial.begin(115200);
  pinMode(levelPin, INPUT_PULLUP);
  setup_wifi();
  client.setServer(MQTT_SERVER, MQTT_PORT);
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

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Last will: if this device disconnects uncleanly (crash/power loss/
    // WiFi drop), the broker publishes "offline" to mqtt_topic_status for us.
    if (client.connect(mqtt_client_name, MQTT_USER, MQTT_PASSWORD,
                        mqtt_topic_status, 1, true, "offline")) {
      Serial.println("connected");
      client.publish(mqtt_topic_status, "online", true);
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
  delay(2000);
}

void updateLevel() {
  // OUT is pulled to GND (LOW) when the sensor detects water, thanks to
  // INPUT_PULLUP it reads HIGH when no water is present.
  int reading = (digitalRead(levelPin) == LOW) ? 1 : 0;

  if (reading == candidateState) {
    candidateCount++;
  } else {
    candidateState = reading;
    candidateCount = 1;
  }

  if (candidateCount >= DEBOUNCE_COUNT && candidateState != lastPublishedState) {
    lastPublishedState = candidateState;
    Serial.print("water level state: ");
    Serial.println(lastPublishedState);
    client.publish(mqtt_topic_level, lastPublishedState ? "1" : "0", true);
  }
}
