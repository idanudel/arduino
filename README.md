# arduino

Personal ESP8266 projects, all reporting to openhab over MQTT.

## Layout

One folder per project, at the repo root, matching Arduino's sketch convention
(folder name == `.ino` file name):

```
project_name/
  project_name.ino
  secrets.h.example   <- committed template, no real values
  secrets.h           <- gitignored, your real WiFi/MQTT credentials
  README.md           <- wiring notes, optional
```

## Secrets

Never put real WiFi/MQTT credentials in the `.ino` file. Each project
`#include`s a local `secrets.h` (gitignored) for:

- `WIFI_SSID`, `WIFI_PASSWORD`
- `MQTT_SERVER`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD`

To set up a new checkout or a newly-imported project: copy that project's
`secrets.h.example` to `secrets.h` and fill in real values. `secrets.h` is
gitignored repo-wide so this only needs doing once per project folder.

## Device naming pattern

MQTT topics follow `openhab/devices/<room>/<device>`. In each sketch, define
the room and device as `#define` macros at the top and derive everything
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

## Projects

- `water_heater_level/` - XKC-Y25V non-contact liquid level sensor on the
  steam water heater tank (utility room), reports low-water state to openhab
  via MQTT.
