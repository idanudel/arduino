---
name: openhab-changes
description: "How to safely read and change openHAB config (items, things, rules, sitemaps) on Idan's Orange Pi home server for new/changed ESP8266 devices. Use this whenever a new device from this repo needs to be wired into openHAB, or existing openHAB config needs checking."
---

# openHAB changes — Idan's home server

Idan's smart home runs on an Orange Pi (openHABian), hostname `openhabian`,
LAN `192.168.0.83`, running **openHAB 4.0.0** (`dpkg -l | grep openhab`).
The Pi also runs his expense-tracker app and other unrelated personal
projects — see `~/.claude/PI-INFRA.md` (global, cross-project) for shared
infra facts (network/port topology, nginx, GitHub runners, Pushover key
reuse) that apply to any project on this box, not just openHAB. **Be
careful — breaking openHAB or nginx here affects the actual house**, not a
test environment.

## Access

```bash
ssh -i ~/.ssh/claude_pi claude@192.168.0.83
```

- Dedicated agent user `claude`, key auth, passwordless.
- Groups: `claude`, `adm`, `systemd-journal`, **`openhab`** (added 2026-08-23,
  at Idan's request, via `sudo usermod -aG openhab claude`) — can read logs
  via `journalctl`, and can now **read AND write** everything under
  `/etc/openhab` directly (it's group-owned `openhab:openhab`,
  group-writable). No staged-script dance needed for items/things/rules/
  sitemaps anymore — just edit the files directly.
  - Group membership only takes effect on a **fresh SSH connection** — an
    already-open session won't pick it up.
  - `chown` on these files still fails for `claude` (not root) — harmless,
    files just end up owned `claude:openhab` or similar after a direct
    write instead of `openhab:openhab`; that doesn't affect openHAB reading
    them (still group/other readable).
- `sudo -l` shows a narrow allowlist (verified 2026-08-23):
  ```
  (ALL) NOPASSWD: /usr/bin/systemctl restart expense-tracker
  (ALL) NOPASSWD: /usr/bin/systemctl reload nginx
  (ALL) NOPASSWD: /usr/bin/systemctl restart nginx
  (ALL) NOPASSWD: /usr/sbin/nginx -t
  (ALL) NOPASSWD: /usr/bin/cp /home/claude/staging/openhab /etc/nginx/sites-enabled/openhab
  ```
  **Nothing covers restarting `openhab.service` itself** — and that
  restart is required after any sitemap change (see below). That still
  needs Idan to run it.
- Revoke `openhab` group access: `sudo gpasswd -d claude openhab` (keeps the
  `claude` user itself). Revoke agent access entirely:
  `sudo deluser --remove-home claude && sudo rm /etc/sudoers.d/claude` (run
  on the Pi, not by the agent).

## The one thing that matters most: text config vs. UI-managed JSON

openHAB config comes from two completely different places that look similar
but are NOT interchangeable:

1. **Text config in `/etc/openhab/*`** — items, things, rules, sitemaps,
   persistence, transform, scripts. Safe to hand-edit, diff, and version.
   **Items, Things, and Rules hot-reload automatically on save** (confirmed
   2026-08-23: saving a new `.rules` file produced an immediate
   `Loading model '...'` log line and the new Thing/Items were live via the
   REST API within ~1s, no restart).
   **Sitemaps do NOT hot-reload on this box, despite being the same kind of
   text file** — confirmed 2026-08-23 by editing an already-working,
   long-existing sitemap line and finding the change absent from
   `GET /rest/sitemaps/default/<page>` even after 15+ seconds and a `touch`;
   reconfirmed 2026-08-24 the same way with a different edit. **Not an
   openHAB-version limitation** — this box runs a modern **openHAB 4.0.0**
   (`dpkg -l | grep openhab`), and sitemap hot-reload has been supported
   for years by that point; the old "sitemaps need a restart" limitation
   only ever applied to much older 1.x/early-2.x releases. Root cause is
   still genuinely unconfirmed beyond that — pinning it down further would
   need DEBUG logging on the model/UI bundles via the Karaf console
   (`openhab-cli console`, port 8101), which the `claude` user doesn't have
   credentials for. **Any sitemap edit needs `sudo systemctl restart
   openhab` to take effect** (not in `claude`'s sudo allowlist, so this
   always needs Idan) — treat this as a reliable, permanent workaround
   rather than something worth re-investigating each time.
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
3. Add **two** lines to `/etc/openhab/sitemaps/default.sitemap` in the
   relevant group (see `FloodBasementShower`/`FloodUpperShower`/
   `BasementSumpPumpWaterLevel` in the "Sensors" frame for the pattern) —
   the sensor value AND its `_Status` companion. It's easy to add only the
   first and forget the status indicator (happened building
   `utility_room_water_level` — had to come back and add it after the
   fact), but the whole point of wiring up the LWT status topic in
   firmware is defeated if nothing in the UI surfaces it:
   ```
   Text item=UtilityRoom_WaterLevel icon="water" label="Utility Room Water Level"
   Text item=UtilityRoom_WaterLevel_Status icon="network" label="Utility Room Water Level Sensor Status"
   ```
   `icon="network"` is the convention for a connectivity/status indicator
   (no existing precedent for this before `utility_room_water_level` — this
   is now the reference for the next one).
   **Gotcha (hit and fixed 2026-08-23):** don't put a `[MAP(...):%s]`
   transform pattern in the sitemap widget's own `label=`. If the Item
   already declares that pattern (as in the Items step above), openHAB
   silently drops the whole widget from the rendered sitemap — no log line,
   no error, it just doesn't appear. Leave the sitemap label plain (e.g.
   `label="Utility Room Water Level"`, no bracket suffix at all) and let
   the Item's own `stateDescription` pattern handle formatting — that's
   what the working `BasementSumpPumpWaterLevel` entry does. (The `_Status`
   item has no transform pattern to worry about — it's a plain String
   showing raw `online`/`offline`.)
4. Items/Things/Rules take effect immediately (no restart). **The sitemap
   change does not** — see the hot-reload caveat above. Ask Idan to run
   `sudo systemctl restart openhab` (~30-60s downtime for the whole smart
   home) before the sitemap entry will show up anywhere, including via the
   REST API, not just the browser.
5. Verify with the checks in the next section rather than assuming success.

## Verifying a change actually took effect

Don't rely on the browser — it's one more caching layer on top of an
already-inconsistent-across-file-types reload story. Check the backend
directly:

```bash
# Item existence + live state (works immediately for Items/Things):
curl -s http://localhost:8080/rest/items/<ItemName> | python3 -m json.tool

# Whether a sitemap widget is actually being served (only meaningful after
# an openHAB restart) — get the page's widgetId from the root sitemap first:
curl -s http://localhost:8080/rest/sitemaps/default/default | python3 -m json.tool | grep -B3 '"label": "<PageName>"'
curl -s http://localhost:8080/rest/sitemaps/default/<widgetId> | grep -o '<ItemName>'

# Rules/Things load status and parse errors - openHAB logs to FILES, not
# journald/syslog (journalctl only shows service start/stop/reload, not
# openHAB's own model-loading log lines):
grep -i '<name>' /var/log/openhab/openhab.log      # app log incl. model loads, errors
grep -i '<ItemName>' /var/log/openhab/events.log   # every item state change, timestamped

# Watch live MQTT traffic on a topic (useful when an Item's state seems
# stuck - confirms whether new messages are even arriving):
mosquitto_sub -h localhost -p 1883 -u idanudel -P '<mqtt password from secrets.h>' -t '<topic>' -v
```

## Applying changes

Now that `claude` is in the `openhab` group (see Access above), items,
things, rules, and sitemap files can just be edited/appended to directly
over the SSH session — no staging dance needed for those. Always back up
the file first (`cp file file.bak.$(date +%Y%m%d-%H%M%S)`) since these are
live, shared, hand-edited files with no version control on the Pi itself.

**Restarting `openhab.service`** (only needed after a sitemap change) is
the one thing still not writable/runnable by `claude` — that always needs
Idan. Prior workflow for anything requiring his intervention still applies:
give him one **short** command to run himself (his zsh terminal wraps long
pasted lines into broken multi-line input — this has corrupted a config
file before; never hand him a long one-liner), or stage a script and ask
him to run `sudo bash <path>`.

## Pushover alert rule pattern

Used consistently across every existing `.rules` file — copy this exactly
rather than inventing a different call style:

```
rule "<Descriptive rule name>"
	when
		Item <ItemName> changed from <A> to <B>
	then
		val actions = getActions("pushover", "pushover:pushover-account:daa1d3d0d7")
		actions.sendMessage("<message text>","Openhab")
end
```

- `val actions = getActions(...)` is declared **fresh inside each rule's
  `then` block**, not once at file-top — that's the dominant convention
  across `items.rules`/`kodeshDayRules.rules`/`system.rules` even though a
  top-level file-global `val` also works fine (confirmed the same account
  UID is reused as a global in `motion.rules` with no conflict — rule
  files don't share a variable namespace, so naming collisions aren't a
  real risk either way, but match the dominant per-rule-block style).
- Title argument is always the literal string `"Openhab"` — not the
  device/room name — by existing convention across every call site.
- The Pushover account Thing UID (`pushover:pushover-account:daa1d3d0d7`)
  is UI-managed and already configured — don't try to create a new one.

## Related

- This repo's `README.md` — the `ROOM`/`DEVICE` firmware-side naming
  convention that the openHAB-side topics/Things/Items above are built to
  match.
- `~/.claude/projects/*/memory/openhabian-server-access.md` (from the
  expense-tracker project) — broader server context: what else runs on this
  box (Grafana, expense-tracker, frontail, nginx/TLS, DuckDNS, SSL renewal
  cron), unrelated to openHAB config itself.
