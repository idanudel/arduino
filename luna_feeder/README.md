# luna_feeder

Luna's automatic pet feeder: an ESP8266 pulses a relay/motor to dispense
food on an MQTT command from openHAB.

Imported from a personal backup (`Arduino 2/luna_feeder`) and brought up to
this repo's standard firmware pattern - see the "Firmware architecture"
section of the repo's `CLAUDE.md`. This one has no sensor, only an
actuator, so it doesn't follow the sensor-read/debounce/retained-publish
part of that pattern - it kept the WiFi/MQTT reconnect and LWT parts, which
still apply.

The original sketch also declared a DS18B20 temperature sensor and a PIR
motion pin, but never actually read or published either anywhere - dead
copy-paste from a sensor-project template. Removed here; this device is
relay-only.

## Wiring

- Feeder relay/motor control -> GPIO5 (D1). `HIGH` = idle/off; a brief
  `LOW` pulse (150ms) triggers one dispense.

## MQTT

Kept identical to the original sketch's runtime values, since this feeds a
live pet and may already be wired into an openHAB rule - **do not** change
the topic or command strings without also updating that rule.

- `openhab/devices/luna/autofeeder` - command topic. Payload `"ON"`
  triggers one dispense pulse; `"OFF"` forces the relay back to idle.
  (Topic says "autofeeder", not "feeder" - kept as the original despite the
  folder/device name, for compatibility.)
- `openhab/devices/luna/autofeeder/status` - new: retained `online`/
  `offline` (via MQTT last will). Worth alerting on for a device that feeds
  a live pet.

## openHAB wiring

Check whether this device is already wired into openHAB config before
assuming it isn't. Use the `openhab-changes` skill either way.
