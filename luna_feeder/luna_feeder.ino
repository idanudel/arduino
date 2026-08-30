// Luna's automatic pet feeder: ESP8266 driving a relay/motor to dispense
// food on an MQTT command. Imported from a personal backup
// (Arduino 2/luna_feeder) and brought up to this repo's standard firmware
// pattern (WiFi/MQTT reconnect, LWT, secrets.h) - see CLAUDE.md.
//
// The original sketch also declared a DS18B20/OneWire temperature sensor
// and a PIR motion pin, but never read or published either anywhere - dead
// copy-paste from a sensor template. Removed here since this device has
// neither sensor, only the feeder relay.
//
// CONTROL_PIN drives the feeder mechanism: HIGH = idle/off, a brief LOW
// pulse triggers one dispense. Command topic and "ON"/"OFF" semantics are
// kept identical to the original sketch since this feeds a live pet and
// may already be wired into an openHAB rule.

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

const char* mqtt_client_name = "luna_feeder";

// Topics kept identical to the original sketch's runtime values (note
// "autofeeder", not "feeder" - preserved for compatibility).
const char* mqtt_topic_command = "openhab/devices/luna/autofeeder";
const char* mqtt_topic_status = "openhab/devices/luna/autofeeder/status";

const int CONTROL_PIN = 5; // D1

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  Serial.begin(115200);
  // pinMode() before digitalWrite() - the original sketch had these
  // reversed, which left CONTROL_PIN's driven level briefly undefined
  // right after boot since digitalWrite() on a still-INPUT pin doesn't
  // drive it.
  pinMode(CONTROL_PIN, OUTPUT);
  digitalWrite(CONTROL_PIN, HIGH); // idle/off
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
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(message);

  if (message == "OFF") {
    Serial.println("CONTROL_PIN -> idle");
    digitalWrite(CONTROL_PIN, HIGH);
  } else if (message == "ON") {
    Serial.println("CONTROL_PIN -> dispense pulse");
    digitalWrite(CONTROL_PIN, LOW);
    delay(150);
    digitalWrite(CONTROL_PIN, HIGH);
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Last will: if this device disconnects uncleanly (crash/power loss/
    // WiFi drop), the broker publishes "offline" to mqtt_topic_status for
    // us - worth alerting on for a device that feeds a live pet.
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
  delay(100);
}
