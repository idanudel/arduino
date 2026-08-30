# basement_kitchenette_2in1

DS18B20 temperature + PIR motion on one ESP8266, reporting to openHAB over MQTT.

Imported from a personal backup (`Arduino 2/k_basement`) and brought up to
this repo's standard firmware pattern - see the "Firmware architecture"
section of the repo's `CLAUDE.md`.

## Wiring

- DS18B20 data pin -> GPIO2 (D4), with a 4.7k pull-up resistor from data to 3.3V.
- PIR sensor OUT -> GPIO9.

GPIO9/10 are wired to the onboard flash chip on most NodeMCU boards and
often aren't usable/exposed on the header. This pin choice was carried over
unchanged from the original sketch - **verify motion actually works on the
physical board** before trusting alerts; move it to a normal free GPIO
(e.g. D1/D2) if it doesn't.

## MQTT topics

Kept identical to the original sketch's runtime topic strings (including
the extra `kitchenette` path segment and the command topic's trailing
slash) in case this device is already wired into openHAB.

- `openhab/devices/basement/kitchenette/2in1/` - command topic; any
  published message triggers an immediate re-publish of current temp +
  motion state.
- `openhab/devices/basement/kitchenette/2in1/temp` - retained, published on
  >=0.5C change, checked every 10s.
- `openhab/devices/basement/kitchenette/2in1/motion` - retained, `0` on
  motion detected (PIR OUT HIGH), `1` on idle (LOW), debounced (3
  consecutive consistent reads, 200ms apart). This "0 = triggered" mapping
  is a deliberate convention shared with `garage_2in1` and
  `basement_sump_pump` - not inverted here.
- `openhab/devices/basement/kitchenette/2in1/status` - new: retained
  `online`/`offline` (via MQTT last will).

## openHAB wiring

Check whether this device is already wired into openHAB config before
assuming it isn't. Use the `openhab-changes` skill either way.
