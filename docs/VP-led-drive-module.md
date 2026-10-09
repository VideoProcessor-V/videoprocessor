# VideoProcessor LED drive module — implementation guide (Stage 7)

Companion to `docs/VP-dither-judder-plan.md` (read that first; §0, §1, §4,
§4b and Stage 7 apply). This file is the detailed specification for the
module. The reference implementation being replaced is the owner's Python
script `beta_22.py` (copy it to `docs/reference/beta_22.py`); read it in full
before writing the controller, because the behaviour the owner has tuned by
eye over years lives in its curves, step tables and special cases.

Written 2026-10-09 against fork tag `1.4-beta-rc1` plus the Stage 2 work
(blanking keys, custom GLSL hook chain).

**Revised 2026-10-09 with `VP-led-drive-answers-r1.md`, which wins where the
two differ.** In short: modes are `compensate | script | log_only` (no
`contrast` mode; CNTR is only ever written once at startup to restore 50);
the script's contrast behaviour is emulated in VP as an encoded-domain gain;
step tables follow the script exactly with a configurable APL split; the
measurement pass and the compensation hook both sit at PRE_OUTPUT, measurement
first; `SerialPort` is separate from `BarcoAsciiDriver`; plain config sections.

---

## 1. Purpose

Drive the Barco FL35's LED light source from the picture content so dark
scenes get a lower black floor, while VP compensates the picture in float so
content brightness stays constant. Replaces a screenshot-based Python script
that commanded LED power and projector contrast over RS232.

What the owner gets: deeper blacks in dark scenes, no visible pumping, no
highlight crush, one set-and-forget configuration, works in both the cinema
and `Low_Latency` queue profiles.

## 2. Architecture — three layers, strictly separated

```
 renderer ──(per-frame stats)──▶ [1] Measurement ──▶ [2] Controller ──▶ [3] Projector protocol ──▶ COM port
                                                        │
 renderer ◀──(gain for compensation hook)───────────────┘
```

Each layer is its own source file(s). The renderer touches the module at
exactly two points: it hands over the statistics after rendering a frame,
and it reads the current gain before rendering the next one. The protocol
layer never sees the renderer. The controller never sees a COM port.

### Layer 1 — Measurement (GPU, in the renderer)

A small compute pass over the **active picture rectangle** (the fork already
tracks it; never a fixed crop) on the **output** codes — after tone map,
gamma, LUT, in the limited-range code domain where black = 16 — producing
per frame:

- `apl`: weighted mean of the gamma-encoded output, in limited-range code
  units (`code = 16 + 219 × value`). Weights `apl_weights`, default
  `0.114, 0.587, 0.299` (R, G, B): the script ran `cv2.cvtColor(BGR2GRAY)` on
  an RGB screenshot, so its effective weights are Rec.601 with R and B
  swapped. Measured at PRE_OUTPUT before the compensation gain (answers §4).
- `highlight`: the 99.5th percentile of 3×3-block means (replaces the
  script's "max of an 854×480 downscale ≥ 215"), plus `highlight_area`: the
  fraction of blocks above the knee threshold.
- `cut`: large frame-to-frame change (use the fork's scene-change signal if
  one exists; otherwise a normalised SAD of block means above a threshold).
- `fade`: APL declining monotonically toward black over N consecutive frames
  with block structure otherwise static.
- `ts`: the frame's presentation timestamp from the queue.

When the time-based queue (Stage 5) exists, statistics are produced for
frames still in the queue, so the controller sees them `lead_ms` early. Until
then they are produced for the frame being rendered (reactive mode).

Readback to the CPU: one small buffer per frame, asynchronously (never stall
the render thread on a GPU readback).

### Layer 2 — Controller (pure logic, no hardware, unit-testable)

Inputs: the per-frame statistics, the projector's reported light range,
config. Outputs, per tick: `light_target` (integer light-source level) and
`gain` (float, for the compensation hook), plus a log line.

Runs on its own thread at `tick_ms`. Must be deterministic given the inputs,
so it can be driven from a recorded statistics file in tests and in log-only
mode.

### Layer 3 — Projector protocol (pluggable transport)

Interface, roughly:

```
open(config)            // port, baud; fails cleanly if the port is absent
query_light_range()     // -> (min, max) or "unknown"
read_state()            // current light level, contrast, etc.
set_light(level)        // returns when ACKed or on timeout
set_contrast(value)     // only the one-off startup correction to 50
close()
```

One implementation per protocol, selected by config. `barco_ascii` is the
only one needed now; `none` is a dry-run driver that logs commands and sends
nothing. Serial I/O on its own thread with a bounded command queue; the
controller posts targets, the driver coalesces (if a newer target arrives
before the previous command is ACKed, send the newest, skip the stale one).

## 3. Config contracts

```
[projector.Default]
protocol: barco_ascii        # barco_ascii | none
port: COM1
baud: 19200
light_min: auto              # auto = query (?N); or an integer override
light_max: auto              # auto = query (?M)
ack_timeout_ms: 300
min_command_gap_ms: 100      # start conservative; see §5
echo: on                     # protocol must be in ECHO on mode for ACKs

[led_drive.Default]
enabled: true
mode: compensate             # compensate | script | log_only
tick_ms: 400
lead_ms: 0                   # cinema profile sets ~120 once Stage 5 exists
led_floor: 100               # scene floor (owner's anti-pump floor)
led_floor_black: 60          # floor for fades / cuts to black
led_ceiling: 255
# target curve: APL -> light level, piecewise linear between points
curve: 0:100, 20:100, 40:200, 70:230, 100:255, 255:255
# step tables by |distance to target|, per tick (script: beta_22.py)
steps_up_dark:   100:7, 50:5, 25:4, 10:1, 0:1
steps_up_bright: 100:7, 50:4, 25:2, 10:1, 0:1
steps_up_split_apl: 40
steps_down:      100:5, 50:3, 25:2, 10:1, 0:1
apl_weights: 0.114, 0.587, 0.299
highlight_percentile: 99.5
highlight_persist_ms: 250
highlight_tolerance_stops: 0.33
knee: 0.08
anti_pump_band: 3
compensation_eotf: 2.2
fade_frames: 6
fade_min_step: 0.5
cut_threshold: 24
```

Both sections are live-editable in the Config app like any other profile,
and `enabled` is toggleable from a key. The `Low_Latency` queue profile
forces `lead_ms: 0`; nothing else differs between profiles.

## 4. Controller specification

### 4.1 Port the script faithfully first

`mode: script` (formerly `contrast`) must reproduce `beta_22.py` as closely as
the tick allows. The contrast path is emulated in VP (gain =
contrast_actual / 50 on the encoded value), never sent to the projector:

- Light target from `curve` (the script: 255 above APL 100, 230 at 70,
  200 at 40, 100 at 20, floor below).
- Contrast target from the script's bands (50 at APL ≥ 60; 50/51 between 45
  and 60 depending on highlight state; rising to 62 at 30 and 74 at 20; 74
  below), applied as the VP gain described above (never sent to the projector).
- Per tick, move the light level by the step from `steps_up`/`steps_down`
  for the current distance to target (the script brightens faster than it
  dims — keep that asymmetry).
- Contrast moves one step per tick, except the script's **fast dump**: when
  contrast must drop by ≥ 10 while a highlight is present, step it down
  quickly (the script did one step per 100 ms) until within 10 of target.
  The script also fast-steps contrast down every 100 ms whenever it is 7–9
  above target, highlight or not; `mode: script` keeps both dumps (answers §3).
- **Anti-pump:** ignore contrast differences within `anti_pump_band` in the
  direction the script ignores them (read the script for which direction
  depends on highlight state).
- The script's `sudden_flag` path is permanently off; omit it.
- The script's loop period was not fixed (~300–500 ms: screenshot time plus
  200 ms pauses). `tick_ms: 400` is the starting guess; the owner will match
  the feel by eye.

This mode exists so the owner can confirm the port before anything new is
enabled. It also stays as the fallback.

### 4.2 `mode: compensate` — the intended form

The projector contrast command is never sent; contrast is left at the
owner's calibrated 50. Instead the controller outputs a gain that VP applies
in linear light **before** the LUT and the sigma-delta quantizer:

```
gain_led       = light_full / light_actual
gain_highlight = 1 / highlight_linear           (with the knee, §4.3)
gain           = min(gain_led, gain_highlight)
```

`light_actual` is the LED's modelled *current* level, not the target: the LED
has a response time (measure once with a camera: command a step, film it,
fit a first-order lag) and the compensation must follow that trajectory or
the picture flashes. Until the measurement exists, use the last ACKed level
with a configurable one-pole lag.

Light target is still from `curve`, but the floor is chosen per scene as the
lowest level that keeps highlight compression within
`highlight_tolerance_stops` (§4.3), bounded by `led_floor`, and by
`led_floor_black` when `fade` or a cut-to-black is detected.

Result: content brightness is constant through every LED change; only the
black floor moves. This removes the two reasons the script floored at 100
(visible contrast steps, and pumping from lag).

### 4.3 Highlight policy

The only trade in a scene is black-floor depth vs highlight headroom; the
LED level is the single dial and compensation holds everything else fixed.

- **Persistence gates protection.** A highlight that has been present for
  less than `highlight_persist_ms` is soft-clipped by the knee and otherwise
  ignored: never dim the scene for a flash, and never let the LED chase it
  (the LED target is filtered over a longer window than the gain).
- **Area decides how.** Small sustained highlights (headlights, lamps, lit
  windows) are handled by the **knee**: roll off the top of the curve above
  what the current LED supports so the highlight keeps its gradation while
  the dark surround stays fully compensated. Large sustained highlights
  lower the **global gain** while the LED comes up. Blend continuously by
  `highlight_area`.
- **Continuous, never stepped.** No thresholds a borderline scene can
  toggle across: response strength is a smooth function of persistence and
  area, eased in over a few frames. Protection engages faster than it
  relaxes (the script's "dump fast, recover slowly", made continuous).
- **One taste parameter:** `highlight_tolerance_stops` — how much knee
  compression is accepted in exchange for a lower floor. Start at 1/3 stop.
  Bind it to a key pair so the owner can nudge it from the seat.

Headlights in a dark scene are the acceptance case: dark road at full
compensated brightness, deep floor, headlight core and bloom still
graduated, no visible change when the LED moves.

### 4.4 Modes by event

- **Cut:** jump the light target (masked by the cut). Reset persistence.
- **Scene:** drift at the step tables.
- **Fade to black:** track the light down toward `led_floor_black` at the
  fade's own rate; gain unchanged; resume normal behaviour when the picture
  returns.
- With lookahead (`lead_ms > 0`): act on the statistics of the frame that
  will be shown `lead_ms` from now, minus the LED's modelled response time,
  so the level lands with the frame.

## 5. Barco ASCII protocol driver

From the F(L)35 ASCII Commands Protocol Reference (601-0381-00) and the
script:

- Serial 19200 8N1, no flow control. Commands are ASCII `:MNEM value`
  terminated by a carriage return per the manual. The script sends
  `echo :lpw1<n>cr > com1` and `echo :cntr<n>cr > com1` (so `LPW1` = LED
  power, `CNTR` = contrast). Note that `echo` transmits the literal letters
  `c` `r` *and then* its own CR/LF, so the exact terminator the projector
  accepts is not established: on first connection, try `\r` alone, then
  the literal `cr` + `\r`, and keep whichever is ACKed. Record the answer
  in the brief.
- Responses (with `ECHO` on): `%001 MNEM 000nnn\r`. Queries: `:LPW1?`,
  `:CNTR?`; range: `:LPW1 ?N` (min), `:LPW1 ?M` (max).
- Documented timing: ≥ 500 ms between commands, wait for the response, 5 s
  pause after 20 commands. The script has run at 200 ms gaps for years, so
  the projector is more tolerant than documented. **Pace on ACKs:** send,
  wait for the response (or `ack_timeout_ms`), then honour
  `min_command_gap_ms`. Log any timeout. Find the real sustainable rate
  empirically and record it in the brief.
- At startup: `ECHO` on, query range, read back `LPW1` and `CNTR`, and treat
  those as the current state (the script assumed 255/50 blindly).
- Clamp every outgoing value to the reported range. Never send anything on
  a port that failed to open; the module degrades to `log_only`.
- The old script configured COM1 by launching Tera Term; the driver sets
  the port parameters itself with the Win32 serial API (`CreateFile` on
  `\\.\COMn`, `SetCommState`, `SetCommTimeouts`).

## 6. Compensation hook

A libplacebo custom hook at `PL_HOOK_PRE_OUTPUT` (after tone map, gamut map
and the target LUT; before the limited-range encode and the sigma-delta OUTPUT
hook), installed through the Stage 2 hook chain, with `//!PARAM gain`,
`//!PARAM knee` and `//!PARAM gain_domain` set each frame from the controller.
In the linear domain it linearises with `compensation_eotf` itself. The
shader is trivial: multiply by gain, apply the knee above the headroom
limit. Parameters are set via the hook's `pl_hook_par` data (the pattern the
NLS hook already uses for `stretch_ratio`) — no template reparse per frame.

## 7. Tests and acceptance

Unit tests (fork style, no hardware):
- Controller reproduces the script's curve and step tables from a recorded
  APL sequence (recorded from the script's log, if available, else a
  synthetic sequence).
- Highlight persistence: a 3-frame flash produces no gain change; a
  sustained highlight produces a smooth, monotonic engagement.
- Fade detection and tracking on a synthetic fade.
- Protocol driver against a fake port: framing, ACK parsing, coalescing,
  timeout, clamping.
- `none` driver and `log_only` mode never open a port.

On the projector (owner):
1. `mode: script` side by side with the Python script (log-only in one,
   live in the other) until the owner is satisfied the port matches.
2. Static dark frame with a small highlight, `mode: compensate`: step the
   LED from 255 to 60 with compensation on. Mid-tones must not move.
3. Night-driving scene: headlights graduated, road at full brightness, no
   visible change when the LED moves. Tune `highlight_tolerance_stops`.
4. Lightning / muzzle-flash scene: no dip.
5. Fade to black: floor follows the fade; no step at the end.
6. One-off: LED step response filmed; `SHUT 1` / `PMUT 1` speed and
   audibility for true black on cuts.

## 8. Portability rules (restated)

- Three layers in their own files under `src/VideoProcessor-Lib/led/`
  (or `vprenderer/led/`), plus one shader file.
- Renderer integration: exactly two call sites (stats out, gain in) and one
  hook-chain entry.
- No new windows or menus; Config app sections and key bindings only.
- Nothing in `Low_Latency` changes except `lead_ms` being forced to 0.
- Keep `mode: script` working for as long as the module exists.
