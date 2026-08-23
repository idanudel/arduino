# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Personal ESP8266 sensor/device firmware. Every device connects to WiFi and
publishes/subscribes over MQTT to report into openHAB (home automation). No
build system, no package.json — plain Arduino sketches.

## Commands

Build tooling is `arduino-cli` (standalone binary, not the Store/GUI Arduino
IDE — that one can't be driven from a shell). ESP8266 core and libraries are
already installed in the environment where this has been used before; if
starting fresh:

```bash
arduino-cli core install esp8266:esp8266
arduino-cli lib install PubSubClient
```

Compile a project:
```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 <project_folder>
```

Upload to a connected board (find the port first — Windows shows it as a
`COM*` device, typically a CP210x or CH340 USB-serial chip):
```bash
arduino-cli board list
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 --port COM3 <project_folder>
```

Watch serial output live:
```bash
arduino-cli monitor -p COM3 -c baudrate=115200
```

`esp8266:esp8266:nodemcuv2` is the FQBN for a NodeMCU 1.0 (ESP-12E) board —
confirm the board model before compiling if it's not the same hardware as
before, since flash size settings must match the physical board for upload
to succeed.

## Repo layout

One folder per project, at the repo root, matching Arduino's sketch
convention (folder name == `.ino` file name):

```
project_name/
  project_name.ino
  secrets.h.example   <- committed template, no real values
  secrets.h           <- gitignored, real WiFi/MQTT credentials
  README.md           <- wiring notes, optional
```

## Secrets

Never put real WiFi/MQTT credentials in the `.ino` file. Each project
`#include`s a local `secrets.h` (gitignored repo-wide via `.gitignore`) for:

- `WIFI_SSID`, `WIFI_PASSWORD`
- `MQTT_SERVER`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD`

Setting up a new checkout or a newly-imported project: copy that project's
`secrets.h.example` to `secrets.h` and fill in real values.

## Device naming pattern

MQTT topics follow `openhab/devices/<room>/<device>`. Each sketch defines
the room and device as `#define` macros at the top and derives everything
else from them (adjacent string literals concatenate at compile time, no
runtime string building needed):

```c
#define ROOM "utility_room"
#define DEVICE "water_level"

const char* mqtt_client_name = ROOM "_" DEVICE;
const char* mqtt_topic_level = "openhab/devices/" ROOM "/" DEVICE;
```

When copying a sketch as a template for a new project, those two lines are
the only thing that need to change to get a correct client name and topic.

## Firmware architecture (per project)

Every sketch follows the same shape (see `water_heater_level/water_heater_level.ino`
for the current reference implementation):

- `setup_wifi()` — blocking connect loop, called from `setup()` and again
  from `loop()` if `WiFi.status() != WL_CONNECTED` (WiFi can drop
  independently of the MQTT connection).
- `reconnect()` — blocking MQTT (re)connect loop, called from `loop()` when
  `!client.connected()`. Registers a last-will (`willTopic`/`willMessage`)
  on a `.../status` topic so openHAB knows if the device itself goes
  offline, not just what it last reported; publishes `"online"` (retained)
  on successful connect.
- Sensor read + publish function — debounces raw reads (require N
  consecutive consistent readings before trusting a state change) and
  publishes retained, so a broker/openHAB restart immediately sees the
  current state rather than waiting for the next change.
- `loop()` order: check WiFi, check/reconnect MQTT, `client.loop()`, poll
  sensor, `delay()`.

## MQTT broker

External devices connect to `idanudel.duckdns.org:8990` — a router
port-forward to Mosquitto (port 1883) running on the openHAB Pi. Same
`MQTT_USER`/`MQTT_PASSWORD` across all devices.

## Wiring a device into openHAB

Publishing to MQTT isn't enough — each device needs a Thing/Item on the
openHAB side to be usable. Use the `openhab-changes` skill
(`.claude/commands/openhab-changes.md`) for this: it covers SSH access to
the home server, which openHAB config is safe to hand-edit vs. which is
live UI-managed JSON that must not be touched directly, the exact
Thing/Item pattern to copy for a new binary sensor, and how changes
actually get applied (the agent can read but not write `/etc/openhab`
directly).
