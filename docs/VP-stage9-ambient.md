# Stage 9 — ambient light (room lights) switching

2026-10-12. Implementer's design on the brief's Stage 9
(`docs/VP-dither-judder-plan.md`, "Stage 9 — Ambient-light profile
switching"). Owner (2026-10-12): Stage 9 is built before the deploy; the
detector's driver (9b) waits until the hardware is chosen.

## Idea

Room light raises the visible black; the response is to render to that
black (a rendering profile with a raised `sdr_black_nits`), not ad-hoc gamma.
VP decides whether the room is **dark** or **lit**; the existing profile
rules pick the profile. Nothing new chooses profiles.

## 9a — the state, the rules variable, settings and the backup key (now)

- **Rule variable `ambient`** (text: `dark` | `lit`; empty when ambient
  switching is off, so `ambient == lit` is false and nothing changes). Usable
  in every profile `when=` like `hdr` or `resolution`, e.g.

      [vprenderer.lit_room]
      when: $ambient=="lit"
      sdr_black_nits: 0.05

  Added to `DisplayRuleExpression::GetVariableType`, both expression
  allow-lists in `RendererProfileConfig.h`, the published source variables
  and the GUI's source lookup; `CONFIGURATION.html` documents it.
- **`[ambient]` section** (plain section, not a profile; settings first):
  - `mode: off | manual | sensor` (default `off` = Bill's VP). `manual`:
    the room is `room`. `sensor` (9b): the detector, with `room` as the
    state until the first reading and whenever the detector is lost.
  - `room: dark | lit` (default `dark`).
  - Detector thresholds, used by the 9a logic and by 9b: `lux_lit`
    (default 20) and `lux_dark` (default 8): above `lux_lit` the room
    becomes lit, below `lux_dark` dark, in between it stays (hysteresis);
    `hold_s` (default 5): a new state must persist this long before it
    switches (a door opening, someone passing the sensor).
- **`AmbientState`** (pure logic, `VideoProcessor-Lib/AmbientState.h`):
  readings in, state out, with the hysteresis and hold above; "no reading
  for 10 s" = detector lost (falls back to `room`). Unit-tested with
  simulated readings.
- **Switching:** a state change calls the GUI's
  `RefreshUnifiedProfilesForRuleContext("ambient")`. Rules are not
  re-evaluated while there is no capture signal (existing behaviour): a
  change then applies when the signal returns. As with any rules change, a
  key-selected profile in that group is released (delta 6, persistence
  off).
- **Backup key** `[shortcuts] room_lights` (no default key): toggles dark /
  lit as a key override until restart or a Config change of `room`/`mode`;
  shown on the overlay's "Key override" line.
- **Config:** a "Room lights" card (General page): mode, room, the three
  detector numbers, and a note with the example
  rule; the Shortcuts page row.
- **Overlay:** the active profile already shows which one the room chose;
  the key override line shows "room lights" while the key holds it. A
  "Room: ..." line with the sensor reading comes with 9b.

## 9b — the detector driver (built for the Arduino route; owner 2026-10-12: if the hardware changes, readjust)

Built: `Ambient::SensorReader` (`VideoProcessor-Lib/AmbientSensor.{h,cpp}`)
on its own `LedDrive::ISerialPort` and thread: opens `[ambient] port` at
`baud` (default 115200), reads one line per reading (`12.3`, `lux=12.3`,
`lux: 12.3`, `12.3 lux`; other lines such as a banner are ignored), keeps
the newest, reopens after an error or 10 s of silence, retries every 5 s.
The GUI polls it once a second into `Ambient::State` (hold, hysteresis; a
detector quiet for 10 s falls back to `room`, and its next reading decides
at once). The sensor has its own USB serial port: Apply refuses
`[ambient] port` equal to `[projector.default] port` (a port opens for one
user at a time). Overlay: "Room: lit (sensor 32.0 lux)" / "(manual)" /
"(key)" / "(sensor not reporting; saved room)". Sketch:
`tools/light-sensor/vp_light_sensor.ino` (VEML7700 default, TSL2591 by one
define; "lux=12.34" twice a second). Test
`AmbientSensorReadsLuxLinesFromItsOwnPort` (parser, simulated port with a
failed first open, port clash).

Original plan:

A second `LedDrive::ISerialPort` with its own small driver and thread (the
transport is already separate from the Barco driver), handing readings to
the GUI thread (posted message) where `AmbientState` runs. Keys `port`,
`baud` and the device's protocol. Candidates in the brief: ESP32/Arduino +
VEML7700 or TSL2591 over USB serial (a sketch sending one reading per line),
or a Yocto-Light-V4. Mounted behind the seating facing the ceiling so it
never sees the screen; calibrate the lights-off / lights-on readings once.
