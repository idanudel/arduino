# garage_2in1

DS18B20 temperature + PIR motion on one ESP8266, reporting to openHAB over MQTT.

Imported from an older standalone sketch (outside this repo) and brought up
to this repo's standard firmware pattern - see the "Firmware architecture"
section of the repo's `CLAUDE.md`.

## Wiring

- DS18B20 data pin -> GPIO0 (D3), with a 4.7k pull-up resistor from data to 3.3V.
- PIR sensor OUT -> GPIO4 (D2).

## MQTT topics

- `openhab/devices/garage/2in1` - command topic; any published message
  triggers an immediate re-publish of current temp + motion state.
- `openhab/devices/garage/2in1/temp` - retained, published on >=0.5C change,
  checked every 10s.
- `openhab/devices/garage/2in1/motion` - retained, `0` on motion detected
  (PIR OUT HIGH), `1` on idle (LOW), debounced (3 consecutive consistent
  reads, 200ms apart). This "0 = triggered" mapping looks backwards but is
  a consistent convention across this author's other motion/level sketches
  (`basement_kitchenette_2in1`, `basement_sump_pump`) - kept as-is
  deliberately, not inverted.
- `openhab/devices/garage/2in1/status` - retained `online`/`offline` (via
  MQTT last will).

## openHAB wiring

Not yet wired into openHAB config (Thing/Items/Sitemap). Use the
`openhab-changes` skill when doing so.
