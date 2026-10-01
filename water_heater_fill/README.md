# water_heater_fill

Water heater ("boiler") refill valve controller, utility room. A dedicated
ESP8266 switches a 1-channel relay that powers a 12V normally-closed solenoid
valve on the boiler's water feed. openHAB sends a **one-shot** fill request;
the ESP8266 opens the valve and **closes it itself** after a fixed time.

Not automatic filling. The separate `water_heater_level/` device (XKC-Y25V
sensor) alerts on low water; a human then presses "Fill" in openHAB. The two
devices share nothing.

## Configuration (top of `water_heater_fill.ino`)

| Constant | Default | Meaning |
|---|---|---|
| `FILL_DURATION_MS` | 10000 | Fill length for a plain `FILL` |
| `MAX_FILL_MS` | 10000 | Hard cap on any fill, including `FILL <sec>` |
| `COOLDOWN_MS` | 30000 | Fills rejected this long after the valve closes |
| `FAILSAFE_MARGIN_MS` | 1000 | Hardware-timer failsafe fires this long after the deadline |
| `RELAY_PIN` | 5 (D1) | Relay IN |
| `RELAY_ACTIVE_HIGH` | `true` | Relay energizes on GPIO HIGH. Set `false` for a bare low-level-trigger opto module (see below) |

## Hardware

- NodeMCU 1.0 (ESP-12E), USB powered.
- Relay module with a Songle **JQC-3FF-S-Z 5VDC** relay: 5V coil, ~70Ω,
  ~70mA. A GPIO can't drive the coil directly (12mA max) - the module's own
  driver stage does, and that stage is what decides whether 3.3V logic works:

| Module input stage (look at the board) | Works from 3.3V GPIO? | What to do |
|---|---|---|
| NPN transistor (SOT-23 "J3Y"/S8050 or TO-92) + resistor, no 4-pin opto chip, usually labeled "high level trigger" / pins `S + -` | **Yes** - ~2.6mA base current through a 1k resistor saturates it easily | Use as-is, `RELAY_ACTIVE_HIGH = true`. After Stage 1 confirms polarity, add 10kΩ D1 -> GND |
| PC817 optocoupler (4-pin chip marked "817"), "low level trigger", **with** a `JD-VCC` jumper | Yes, if you split the supplies | Remove the JD-VCC jumper: `VCC` -> ESP **3V3**, `JD-VCC` -> **VIN (5V)**, `GND` -> GND. `RELAY_ACTIVE_HIGH = false` |
| PC817 optocoupler, low level trigger, **no** JD-VCC jumper (VCC = 5V only) | **Not reliably.** GPIO HIGH = 3.3V leaves only ~1.7V across the opto LED + indicator LED vs. its 5V pull-up; many units never fully release, i.e. the valve might not close | Put an NPN in front: D1 -> 1kΩ -> base of a 2N2222/2N3904/BC547, emitter -> GND, collector -> module IN, 10kΩ D1->GND. That inverts the logic, so keep `RELAY_ACTIVE_HIGH = true` |
| H/L jumper selectable module | Yes | Set jumper to **H**, treat as row 1 |

**Stage 1 below confirms which one you have - don't connect the valve until
the relay clicks off reliably when the GPIO goes to its OFF level.**

**Installed module (Stage 1 passed 2026-09-30):** a 5V transistor-input
module, pins `S + -`, **active-high** -> `RELAY_ACTIVE_HIGH = true`. Stays
off at boot/power-cycle, `FILL 2` on for ~2s then off. (The first module
tried was active-low - relay ON at boot with this setting - and was
swapped out rather than relying on a 3.3V GPIO to hold a 5V opto input off.)

## Wiring

```
ESP8266 (NodeMCU)                Relay module (control side, installed module)
  VIN (5V from USB)  ----------> +
  GND                ----------> -
  D1 (GPIO5)         ----------> S
  (optional: 10k D1 -> GND, see below)

12V 2A adapter                   Relay module (switch side)
  +12V  -----------------------> COM
                                 NO  ----------> Solenoid +
  -12V  <------------------------------------- Solenoid -
                                 NC  (unused)

1N5408 across the solenoid coil, at the valve:
  stripe (cathode) -> Solenoid +    plain (anode) -> Solenoid -
```

- **Boot-state resistor - only after Stage 1 has confirmed polarity**
  (the wrong one would *energize* the relay while D1 floats during
  reset/boot): active-high -> 10kΩ D1 to **GND**; active-low -> 10kΩ D1 to
  **3V3**. (For a 5V-powered opto module, 3V3 may not fully hold it off -
  that's the same problem as row 3 of the table; prefer the NPN stage.)
- Use **NO**, never NC: an unpowered/rebooting relay must mean a closed valve.
- The 12V side never touches the ESP8266. Don't join 12V negative to ESP GND
  (not needed - the relay contacts isolate them).
- 1.3A through the relay contacts is fine (rated 10A); use ≥ 20AWG for the
  12V loop.
- The NodeMCU's VIN pin passes USB 5V (minus a diode, ~4.6V) - enough for
  the coil (must-operate ≤ 3.75V), but use a decent USB supply/cable; the
  relay coil pulling ~70mA on a weak supply can brown out the ESP. A
  brownout mid-fill shows up as ERROR (see below), not a stuck valve.

### Why GPIO5 (D1)

Not a boot strapping pin (unlike GPIO0/D3, GPIO2/D4, GPIO15/D8), no boot-time
toggling (GPIO16/D0 and TX/RX do), not shared with flash (GPIO6-11, see
CLAUDE.md), and high-impedance during reset/boot/flashing - so the relay
stays off until firmware drives it OFF as the very first thing in `setup()`.

## MQTT

Base: `openhab/devices/utility_room/water_fill` (repo convention).

| Topic | Dir | Retained | Payload |
|---|---|---|---|
| `.../request` | in | **never** | `FILL`, `FILL <1-10>` (seconds, for bench tests), `STOP`, `RESET` |
| `.../status` | out | yes | `online` / `offline` (LWT) |
| `.../state` | out | yes | `IDLE` / `FILLING` / `COOLDOWN` / `ERROR` |
| `.../available` | out | yes | `YES` only in IDLE, else `NO` |
| `.../last_result` | out | yes | `completed`, `stopped by command`, `ERROR: failsafe timer closed valve (loop stalled)` |
| `.../last_duration` | out | yes | seconds the valve was actually open, e.g. `10.0` |
| `.../last_fill_time` | out | yes | ISO-8601 UTC (only if NTP synced) |
| `.../event` | out | yes | latest command response / boot info, e.g. `rejected: cooldown, 18s left` |

## Safety behaviour

- **Boot:** relay latch preloaded to OFF before the pin becomes an output.
- **Local timeout:** loop() closes the valve at the deadline; an
  independent **timer1 hardware interrupt** closes it 1s later if loop()
  was stalled -> state `ERROR`. The ISR is one GPIO register write + a
  flag (IRAM, no calls); its OFF level derives from `RELAY_ACTIVE_HIGH`
  at compile time (verified by disassembly for both polarities). Not
  `Ticker` - that's an SDK soft timer that can't fire while loop() is
  busy-waiting. A fully hung CPU trips the watchdog -> reset -> relay OFF.
- **STOP:** forces the relay OFF first, in any state, even inside the
  post-reconnect ignore window.
- **Network independent:** loop() never waits for WiFi/MQTT, and never
  attempts an MQTT connect while FILLING. WiFi/MQTT/openHAB dying mid-fill
  doesn't matter - the valve closes on time.
- **No timer extension:** FILL while FILLING/COOLDOWN/ERROR is rejected.
- **No stale commands:** clean MQTT session (nothing queued while offline),
  any retained message on `request` is erased before subscribing, and
  commands in the first 2s after (re)subscribing are ignored. openHAB
  side: command-only channel, `retained=false`, item `autoupdate="false"`,
  not persisted/restored.
- **Reset during a fill:** a marker in RTC memory (survives resets, not
  power loss) makes the device boot into `ERROR` and refuse fills until
  `RESET` (openHAB shows a Reset button in that state + Pushover alert).
- **Recovery:** if MQTT has been down 15 min and the valve is idle, the ESP
  reboots itself.

## openHAB

Thing `mqtt:topic:utility_room_water_fill` (`things/default.things`), Items
`UtilityRoom_WaterFill_*` (`items/items.items`), sitemap frame "Boiler
Water" (top-level, after "Switches"), Pushover rule on ERROR
(`items.rules`), offline/online alerts (`alerts.rules`). The fill button is
a `Switch` with `mappings=[FILL="Fill", STOP="Stop"]` on a String command
item - a momentary button, not an ON/OFF switch - hidden while the
controller is offline. Reference copy of all of it:
`openhab/openhab-config.txt` (the Pi's live files are the source of truth).

## Flash

```bash
cp secrets.h.example secrets.h   # then fill in (same values as the other devices)
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 water_heater_fill
arduino-cli board list
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 --port COMx water_heater_fill
arduino-cli monitor -p COMx -c baudrate=115200
```

## Test procedure

**Serial commands** (in `arduino-cli monitor`, type a line + Enter; work
with no network at all): `FILL`, `FILL <sec>`, `STOP`, `RESET`, and
`WIFIOFF <sec>` (drops WiFi+MQTT for that long, to simulate an outage).

Watch MQTT traffic on the Pi:
`mosquitto_sub -h localhost -u idanudel -P '<pw>' -t 'openhab/devices/utility_room/water_fill/#' -v`

1. **ESP + relay only** (no 12V). Boot -> relay must stay off (also hold
   RST, and watch it during upload). Type `FILL 2`: relay clicks on, off
   after 2s; serial log shows `valve closed after 2000ms`. Check with a
   multimeter that COM-NO is open when off. **If the relay never releases,
   chatters, or clicks at boot, stop** - see the relay table above.
2. **+ MQTT/openHAB.** Press Fill in the app: relay on for 10s (time it),
   state FILLING -> COOLDOWN -> IDLE after 30s. Press Fill again during
   FILLING and during COOLDOWN: rejected (see Last Event), timer unchanged.
   Press Stop mid-fill: closes immediately.
3. **+ 12V solenoid on the bench** (diode fitted, water optional). `FILL 2`,
   `FILL 5`, `FILL` (10s): valve clicks open/shut with the relay. Pull the
   12V plug mid-fill: valve snaps closed. Pull the ESP's USB mid-fill:
   valve closes; on reconnect state = ERROR (reset marker); press Reset.
   (Pulling USB is a full power loss, so RTC may not survive - ERROR is
   guaranteed only for resets, e.g. pressing the RST button mid-fill.)
4. **Network failure mid-fill.** Press Fill in openHAB, then within 2s type
   `WIFIOFF 60` in the serial monitor: valve still closes at 10s (serial
   log), openHAB shows the controller `offline` (LWT, after MQTT keepalive
   ~20s), then `online` again with state/last result updated. For a real
   outage instead of a simulated one, unplug the WiFi access point mid-fill.
5. Only then plumb it into the boiler feed.
