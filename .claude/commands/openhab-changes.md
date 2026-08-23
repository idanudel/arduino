---
name: openhab-changes
description: "How to safely read and change openHAB config (items, things, rules, sitemaps) on Idan's Orange Pi home server for new/changed ESP8266 devices. Use this whenever a new device from this repo needs to be wired into openHAB, or existing openHAB config needs checking."
---

# openHAB changes — Idan's home server

Idan's smart home runs on an Orange Pi (openHABian), hostname `openhabian`,
LAN `192.168.0.83`. It also runs his expense-tracker app. **Be careful —
breaking openHAB or nginx here affects the actual house**, not a test
environment.

## Access

```bash
ssh -i ~/.ssh/claude_pi claude@192.168.0.83
```

- Dedicated agent user `claude`, key auth, passwordless.
- Groups: `claude`, `adm`, `systemd-journal` — can read logs via `journalctl`,
  but is **NOT** in the `openhab` group.
- `sudo -l` shows a narrow allowlist (verified 2026-08-23):
  ```
  (ALL) NOPASSWD: /usr/bin/systemctl restart expense-tracker
  (ALL) NOPASSWD: /usr/bin/systemctl reload nginx
  (ALL) NOPASSWD: /usr/bin/systemctl restart nginx
  (ALL) NOPASSWD: /usr/sbin/nginx -t
  (ALL) NOPASSWD: /usr/bin/cp /home/claude/staging/openhab /etc/nginx/sites-enabled/openhab
  ```
  Nothing here covers openHAB itself. Confirmed: **`claude` can read all of
  `/etc/openhab` (group-readable) but cannot write to it** — `test -w` on
  `items.items` / `default.things` returns false. There's no sudo rule for
  writing openHAB config either.
- Revoke agent access entirely: `sudo deluser --remove-home claude && sudo rm /etc/sudoers.d/claude` (run on the Pi, not by the agent).

## The one thing that matters most: text config vs. UI-managed JSON

openHAB config comes from two completely different places that look similar
but are NOT interchangeable:

1. **Text config in `/etc/openhab/*`** — items, things, rules, sitemaps,
   persistence, transform, scripts. These are plain files openHAB watches
   and **hot-reloads automatically on save, no restart needed**. Safe to
   hand-edit, diff, and version.
2. **UI-managed state in `/var/lib/openhab/jsondb/*.json`** — anything
   created/edited through the openHAB UI (PaperUI/MainUI) instead of a text
   file. The MQTT broker and the main "8f618a1559" MQTT topic Thing that
   most of Idan's existing devices (garage, kidsroom, basement sump pump)
   are wired into live here, in
   `org.openhab.core.thing.Thing.json`.
   **Never hand-edit this file.** It's live runtime state loaded by a
   running openHAB instance — a bad edit (or a save race with the running
   server) risks corrupting Thing loading on next restart. There's no
   equivalent of "just reload" for this file the way there is for text
   config.

**Practical consequence:** don't add a new device's channel into the
existing UI-managed `8f618a1559` Thing. Instead add a brand-new, self-contained
**text-based** Thing in `/etc/openhab/things/default.things`, referencing the
same broker. There's already precedent for this exact pattern in that file
(the `ibbq4t` Thing) — mixing a couple of hand-written Things alongside the
UI-managed ones is normal on this box.

## What's in `/etc/openhab/` (surveyed 2026-08-23)

| Folder | Contents | Notes |
|---|---|---|
| `items/items.items` | ~14KB, all Items | One flat file, hand-edited |
| `things/default.things` | A few text Things (systeminfo, 2x network ping, 1x MQTT `ibbq4t`) | Everything else is UI-managed |
| `rules/*.rules` | `items.rules`, `motion.rules`, `presence.rules`, `system.rules`, `time.rules`, `kodeshDayRules.rules`, `kodeshDayUpdater.rules`, `voiceRule.rules` | Legacy Xtend DSL rules engine — this is what's actually used, not the JS engine |
| `automation/js` | `openhab_rules_tools` npm package installed | No actual `.js` rule scripts present — JS rules engine set up but unused |
| `automation/jsr223` | empty | Unused |
| `sitemaps/default.sitemap` | ~13KB | Controls what shows in the mobile/web UI — a new Item won't appear here until added |
| `transform/*.map`, `*.js` | `motion.map`, `triggered.map`, `onOff.map`, kodesh-related `.js` transforms | See below for which map to use |
| `persistence/influxdb.persist` | InfluxDB persistence config | |
| `services/*.cfg` | `addons.cfg` (all commented — bindings installed via Karaf feature installer, not this file), `runtime.cfg`, `basicui.cfg`, `influxdb.cfg`, `openhabcloud.cfg` | |
| `scripts/kodeshTimes.js` | Referenced by kodesh-day rules | |
| `sounds/`, `icons/`, `html/` | Static assets | Rarely relevant |

## MQTT broker facts (confirmed from the UI-managed Thing config)

- Broker Thing: `mqtt:broker:dd06a9365e`, UI-managed, host `localhost` (openHAB
  talks to Mosquitto on the same box), TCP, not secure.
- Mosquitto (`/etc/mosquitto/mosquitto.conf`): single `listener 1883`,
  `password_file` auth, `allow_anonymous false`. `conf.d/` is empty.
- Username `idanudel` — same credential ESP8266 devices in this repo use in
  their `secrets.h`.
- External devices (all the ESP8266 sketches in this repo) connect to
  `idanudel.duckdns.org:8990` — that's a **router port-forward** to this
  Pi's `1883`, not a Mosquitto setting. Not visible/configurable from the
  Pi itself. Already proven working by every existing device, so just keep
  reusing it for new devices.

## Recipe: wiring a new binary MQTT sensor into openHAB

This repo's convention (see root `README.md`) is topics shaped like
`openhab/devices/<room>/<device>`, publishing `"0"`/`"1"`, retained. The best
existing precedent on the Pi for exactly this shape is
`basement_sump_pump` — copy this pattern, not the older `garage_motion`
string+`motion.map` one:

```json
// precedent channel (UI-managed, for reference only — don't edit this file):
{
  "channelTypeUID": "mqtt:number",
  "itemType": "Number",
  "configuration": { "stateTopic": "openhab/devices/basement/sumppump/waterlevel" }
}
```
```
// items.items precedent:
Number BasementSumpPumpWaterLevel "Basement Sump Pump Water Level [MAP(triggered.map):%s]" {channel="mqtt:topic:dd06a9365e:8f618a1559:basement_sump_pump"}
```
`transform/triggered.map`: `0=Untriggered`, `1=Triggered` — reads better for
a binary sensor than `motion.map`.

**Steps for a new device** (e.g. `utility_room`/`water_level`):

1. Append a new text-based Thing to `/etc/openhab/things/default.things`
   (don't touch the UI-managed one):
   ```
   Thing mqtt:topic:utility_room_water_level "Utility Room Water Level mqtt" (mqtt:broker:dd06a9365e) {
       Channels:
           Type number : water_level "water_level" [ stateTopic="openhab/devices/utility_room/water_level" ]
           Type string : status "status" [ stateTopic="openhab/devices/utility_room/water_level/status" ]
   }
   ```
2. Append matching Items to `/etc/openhab/items/items.items`:
   ```
   Number UtilityRoom_WaterLevel "Utility Room Water Level [MAP(triggered.map):%s]" {channel="mqtt:topic:utility_room_water_level:water_level"}
   String UtilityRoom_WaterLevel_Status "Utility Room Water Level Status [%s]" {channel="mqtt:topic:utility_room_water_level:status"}
   ```
3. Add a line to `/etc/openhab/sitemaps/default.sitemap` in the relevant
   group (see `Garage_motion`/`Kidsroom_motion` for the pattern) if it
   should be visible in the UI.
4. No restart needed — text config hot-reloads. Confirm with
   `journalctl -u openhab -n 50 --no-pager` (claude can read this, in `adm`
   group) that the new Thing came ONLINE and no parse errors appeared.

## Applying changes (claude can't write `/etc/openhab` directly)

Proven workflow (from prior sessions):

1. Prepare the new file content locally, `scp` or heredoc it to
   `/home/claude/staging/` on the Pi. Verify with `diff` against the live
   file before proposing anything.
2. Since there's no sudo rule for copying into `/etc/openhab/*`, this needs
   Idan. Options, in order of preference:
   - Give him one **short** command to run himself (his zsh terminal wraps
     long pasted lines into broken multi-line input — this has corrupted a
     config file before; never hand him a long one-liner).
   - Stage a script and ask him to run `sudo bash <path>`.
   - Ask him to add a specific line to `/etc/sudoers.d/claude` if this is
     going to happen often (validate any sudoers edit with
     `sudo visudo -c -f <file>` before installing).
3. Always confirm success afterward by reading the file back and/or
   checking `journalctl -u openhab` for load errors.

## Related

- This repo's `README.md` — the `ROOM`/`DEVICE` firmware-side naming
  convention that the openHAB-side topics/Things/Items above are built to
  match.
- `~/.claude/projects/*/memory/openhabian-server-access.md` (from the
  expense-tracker project) — broader server context: what else runs on this
  box (Grafana, expense-tracker, frontail, nginx/TLS, DuckDNS, SSL renewal
  cron), unrelated to openHAB config itself.
