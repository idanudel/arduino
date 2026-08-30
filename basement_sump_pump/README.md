# basement_sump_pump

Water level switch on the basement sump pump, reporting to openHAB over MQTT.

Imported from a personal backup (`Arduino 2/waterLevel`) and brought up to
this repo's standard firmware pattern - see the "Firmware architecture"
section of the repo's `CLAUDE.md`.

The original sketch would not compile as found: its subscribe-topic
constant was `= \n .c_str();` with no object to call `.c_str()` on (the
same self-referencing-`String` bug also present in the sibling
`basement_kitchenette_2in1` sketch's dead test topic). Rebuilt here to
subscribe on the base topic, matching every other device in this repo.

## Wiring

- Water level switch -> GPIO2 (D4).

## MQTT topics

Kept identical to the original sketch's runtime topic strings (including
`sumppump`, not `sump_pump`) in case this device is already wired into
openHAB.

- `openhab/devices/basement/sumppump/` - command topic (rebuilt - the
  original didn't compile, see above); any published message triggers an
  immediate re-publish of current level state.
- `openhab/devices/basement/sumppump/waterlevel` - retained, `0` on level
  triggered (switch HIGH), `1` on normal (LOW), debounced (3 consecutive
  consistent reads, 200ms apart). This "0 = triggered" mapping is a
  deliberate convention shared with `garage_2in1` and
  `basement_kitchenette_2in1` - not inverted here. This is a flood/alert
  sensor - double check the polarity against the physical switch before
  relying on it.
- `openhab/devices/basement/sumppump/status` - new: retained `online`/
  `offline` (via MQTT last will).

## openHAB wiring

Check whether this device is already wired into openHAB config before
assuming it isn't. Use the `openhab-changes` skill either way.
