# VideoProcessor LED drive module — implementation guide (Stage 7)

Companion to `docs/VP-dither-judder-plan.md` (read that first; §0, §1, §4,
§4b and Stage 7 apply). This file is the detailed specification for the
module. The reference implementation being replaced is the owner's Python
script `beta_22.py` (copy it to `docs/reference/beta_22.py`); read it in full
before writing the controller, because the behaviour the owner has tuned by
eye over years lives in its curves, step tables and special cases.

Written 2026-10-09 against fork tag `1.4-beta-rc1` plus the Stage 2 work
(blanking keys, custom GLSL hook chain). Revised the same day to fold in
`VP-led-drive-answers-1.md` and `VP-led-drive-answers-2.md`; precedence on
any conflict: answers-2 > answers-1 > this guide.

---

## 1. Purpose

Drive the Barco FL35's LED light source from the picture content so dark
scenes get a lower black floor, while VP compensates the picture in float so
content brightness stays constant. Replaces a screenshot-based Python script
that commanded LED power and projector contrast over RS232.

What the owner gets: deeper blacks in dark scenes, no visible pumping, no
highlight crush, one set-and-forget configuration, works in both the cinema
and `Low_Latency` queue profiles.

Why it is worth its place: the projector's *internal* black floor scales
with LED level, while room bounce scales with screen content. Dimming the
LED and restoring content with per-frame gain therefore lowers the dark
parts of a scene for the same bright parts — a real intra-scene (ANSI)
contrast gain, not only an on/off one. The LED is the only actuator; the
lens irises are static and are never motorised.

## 2. Architecture — three layers, strictly separated

```
 renderer ──(per-frame stats)──▶ [1] Measurement ──▶ [2] Controller ──▶ [3] Projector protocol ──▶ COM port
                                                        │
 renderer ◀──(gain for compensation hook)───────────────┘
```

Each layer is its own source file(s). The renderer touches the module at
exactly three points: it hands over the exact statistics after rendering a
frame; it reads the current gain before rendering the next one; and the
existing queued-frame walk calls a neutral "queued-frame statistics"
observer interface that the module subscribes to (that interface, and the
walk code, never mention the LED module, so they stay PR-able). The
protocol layer never sees the renderer. The controller never sees a COM
port.

### Layer 1 — Measurement (GPU, in the renderer)

A small compute pass over the **active picture rectangle** (the fork already
tracks it; never a fixed crop) on the **output** codes — after tone map,
gamma, LUT, in the limited-range code domain where black = 16 — producing
per frame:

- `apl`: weighted mean of the gamma-encoded output codes on a 0–255 scale
  (`code = 16 + 219 × value` from the normalised PRE_OUTPUT value), weights
  `apl_weights`, default `0.114 R, 0.587 G, 0.299 B` — the weights the
  script effectively used (cv2 BGR2GRAY on an RGB array), so the owner's
  curves carry over; true Rec.601 is the documented alternative.
- `highlight`: the 99.5th percentile of 3×3-block means (replaces the
  script's "max of an 854×480 downscale ≥ 215"), plus `highlight_area`: the
  fraction of blocks above the knee threshold.
- `cut`: large frame-to-frame change (use the fork's scene-change signal if
  one exists; otherwise a normalised SAD of block means above a threshold).
- `fade`: APL declining monotonically toward black over N consecutive frames
  with block structure otherwise static.
- `ts`: the frame's presentation timestamp from the queue.

Statistics are produced inside the fork's existing queued-frame lookahead
walk (`AnalyzeActivePictureLookahead`), so the controller sees them as early
as that walk reaches; its depth is the fork's `active_picture_lookahead_frames`
and the queue's real depth. No separate lookahead key. In `Low_Latency` the
queue holds one frame, so the module is reactive.

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
set_contrast(value)     // only used once at startup to restore CNTR 50
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
min_command_gap_ms: 200      # the script's proven pace; Barco documents >= 500; see §5
max_levels_per_command: 12   # largest LED drop per command (0 = no limit)
burst_commands: 0            # optional pause after this many commands (Barco: 20); 0 = none
burst_pause_ms: 5000         # length of that pause (Barco: 5 s)
read_back: off               # off = send only (the script's way); on = wait for replies
echo: on                     # protocol must be in ECHO on mode for ACKs
led_latency_ms: 80           # command + projector response; measured once (answers-2 §B2)
flux_table:                  # lpw:relative_flux pairs, piecewise linear; absent = linear
                             # (answers-2 §B1; the engine is non-stock, measure later)

[led_drive.Default]
enabled: true
mode: compensate             # compensate | script | log_only
compensation_eotf: 2.2       # projector's real display gamma (not VP's 2.4)
apl_weights: 0.114, 0.587, 0.299   # script's effective weights; Rec.601 = 0.299,0.587,0.114
tick_ms: 400                 # script mode only; compensate mode is per frame
rate_up: 400                 # compensate mode: max levels/second brightening
rate_down: 120               # compensate mode: max levels/second dimming
deadband: 3                  # compensate mode; = anti_pump_band in script mode
early_ramp: true             # compensate mode: ramp ahead of a known cut (answers-2 §B4)
led_floor: 100               # scene floor (owner's anti-pump floor)
led_floor_black: 60          # floor for fades / cuts to black
led_ceiling: 255
# target curve: APL -> light level, piecewise linear between points
curve: 0:100, 20:100, 40:200, 70:230, 100:255, 255:255
# step tables by |distance to target|, per tick
steps_up_dark:   100:7, 50:5, 25:4, 10:1, 0:1   # APL < steps_up_split_apl
steps_up_bright: 100:7, 50:4, 25:2, 10:1, 0:1   # APL >= split
steps_up_split_apl: 40
steps_down:      (transcribe exactly from beta_22.py)
fade_frames: 6
fade_min_step: 0.5
cut_threshold: 24
highlight_percentile: 99.5
highlight_persist_ms: 250
highlight_tolerance_stops: 0.33
knee: 0.08
anti_pump_band: 3
```

Both sections are live-editable in the Config app (plain settings sections,
no profile groups, no key toggle — answers file §9). Nothing is profile-
specific: in `Low_Latency` the lookahead walk holds one frame, so the module
is reactive by construction.

## 4. Controller specification

### 4.1 Port the script faithfully first

`mode: script` must reproduce `beta_22.py` as closely as the tick allows —
with the projector's contrast control **emulated in VP** rather than sent
(answers file §1): `gain_encoded = contrast_actual / 50`, applied in encoded
space at the compensation hook with `gain_domain = encoded`.

- Light target from `curve` (the script: 255 above APL 100, 230 at 70,
  200 at 40, 100 at 20, floor below).
- Contrast target from the script's bands (50 at APL ≥ 60; 50/51 between 45
  and 60 depending on highlight state; rising to 62 at 30 and 74 at 20; 74
  below), realised as the emulated gain, never as a CNTR command.
- Per tick, move the light level by the step from `steps_up_dark` /
  `steps_up_bright` (chosen by APL against `steps_up_split_apl`) or
  `steps_down` for the current distance to target — transcribed from the
  script, which brightens faster than it dims; keep that asymmetry.
- Contrast moves one step per tick, except the script's two **fast dumps**:
  (a) when contrast must drop by ≥ 10 while a highlight is present, step
  down quickly (one step per 100 ms) until within 10 of target; (b) when
  contrast is 7–9 above target, step down every 100 ms regardless of
  highlight state. Both kept in `script` mode only.
- **Anti-pump:** ignore contrast differences within `anti_pump_band` in the
  direction the script ignores them (read the script for which direction
  depends on highlight state).
- The script's `sudden_flag` path is permanently off; omit it.
- The script's loop period was not fixed (~300–500 ms: screenshot time plus
  200 ms pauses). `tick_ms: 400` is the starting guess; the owner will match
  the feel by eye.

This mode exists so the owner can confirm the port before anything new is
enabled. It also stays as the fallback. CNTR itself is read at startup and
set to 50 once if it was left elsewhere; after that it is never written.

### 4.2 `mode: compensate` — the intended form

The projector contrast command is never sent; contrast is left at the
owner's calibrated 50. Instead the controller outputs a gain that VP applies
in linear light at `PL_HOOK_PRE_OUTPUT` — after tone map, gamut map and the
target LUT, before encode and before the sigma-delta OUTPUT hook; the hook
linearises with `compensation_eotf` (answers file §7):

```
gain_led       = flux(led_ceiling) / flux(light_actual)   // flux_table; linear if absent
gain_highlight = 1 / highlight_linear                      (with the knee, §4.3)
gain           = min(gain_led, gain_highlight)
```

`light_actual` is the LED's modelled *current* level, not the target: the LED
has a latency (`led_latency_ms`, measured once with a camera: command a
step, film it) modelled as pure dead time — the modelled level switches to
the commanded value `led_latency_ms` after the command, in whole output
frames, and the gain switches with it (a separate `led_settle_ms` can be
added later if the measurement shows a gradual settle). With lookahead the
command is sent `led_latency_ms` ahead and the gain lands on the frame;
without it, the command goes now and the gain is delayed by the same
amount. In
`compensate` mode the controller runs per frame with slew-limited ramps
(`rate_up`, `rate_down`, `deadband`); the gain follows the same ramp on the
same frames. Per-frame targets exceed the serial rate, so the driver
coalesces to the newest target and the modelled trajectory, not the ACK
stream, drives the gain (answers-2 §B3).

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
  compression is accepted in exchange for a lower floor. Start at 1/3 stop;
  edited live in the Config app.

Headlights in a dark scene are the acceptance case: dark road at full
compensated brightness, deep floor, headlight core and bloom still
graduated, no visible change when the LED moves.

### 4.4 Modes by event

- **Cut:** `script` mode jumps the light target (masked by the cut).
  `compensate` mode ramps to it like any other change when the lookahead
  did not see the cut (the gain keeps content constant; a dark-to-bright
  cut dims briefly while the LED comes up, bounded by headroom), and
  ramps early when it did (below). Reset persistence.
- **Scene:** drift — per-tick step tables in `script` mode; slew-limited
  per-frame ramps (`rate_up`/`rate_down`, `deadband`) in `compensate` mode.
- **Fade to black:** track the light down toward `led_floor_black` at the
  fade's own rate; gain unchanged; resume normal behaviour when the picture
  returns.
- With lookahead, `script` mode: send each command `led_latency_ms` ahead
  of the frame that needs it — never earlier, because without compensation
  the picture would change before the cut.
- With lookahead, `compensate` mode (`early_ramp: true`): the ramp may
  begin ahead of a known cut so the LED arrives on the cut frame, because
  the gain holds content constant during the ramp. Dimming ahead of a cut
  is bounded by the *current* scene's highlight headroom
  (`highlight_tolerance_stops`); brightening ahead of a cut is
  unconstrained. Within those bounds, start as early as the walk allows
  (answers-2 §B4).

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
- Documented timing (Barco RS232 user guide TDE6115, 2.1): ≥ 500 ms between
  commands, wait for the response, ≥ 2 s before re-sending when none came,
  5 s pause after 20 commands. The script ran at 400 ms ticks (100 ms
  contrast steps) for years without the pause. **Owner decisions
  (2026-10-09, revised after design review 1 SF-2):** `min_command_gap_ms`
  is a hard floor for every command (startup included; only the bounded
  exit restore may cut a wait short, and logs it), default 200 ms - the
  script's proven pace - with Barco's 500 ms as the documented conservative
  value; `max_levels_per_command` (default 12) caps each LED *drop* so the
  black floor never steps far (the rest of the drop follows in later
  commands); brightening is not capped, so a bright scene never waits; the
  20/5 s pause is optional (`burst_commands`, default 0 = off); read-backs
  are a toggle (`read_back`, default off).
- `read_back: off`: send only - the script's exact bytes (or the configured
  terminator), no `ECHO`, no queries; contrast is reset to 50 blind at
  startup and light set to the ceiling, as the script did; the range comes
  from `led_floor`/`led_ceiling`. `read_back: on`: at startup `ECHO` on,
  probe the terminator, query range, read back `LPW1` and `CNTR`; wait for
  each reply up to `ack_timeout_ms`, and 2 s before the next command after a
  missing reply. Log any timeout.
- Clamp every outgoing value to the reported range. Never send anything on
  a port that failed to open; the module degrades to `log_only`.
- The old script configured COM1 by launching Tera Term; the driver sets
  the port parameters itself with the Win32 serial API (`CreateFile` on
  `\\.\COMn`, `SetCommState`, `SetCommTimeouts`).

## 6. Compensation hook

A libplacebo custom hook at `PL_HOOK_PRE_OUTPUT` (verify the value domain in
renderer.c: full-range normalised, target transfer, pre-encode), placed in
the Stage 2 hook chain **after** the measurement pass and **before** the
sigma-delta OUTPUT hook. Parameters set each frame from the controller via
`pl_hook_par` data (the pattern the NLS hook uses for `stretch_ratio`):
`gain`, `knee`, `gain_domain` (0 = encoded for `script` mode, 1 = linear for
`compensate`), `compensation_eotf`. Linear path:
`lin = pow(v, eotf); lin *= gain; knee above the headroom limit;
v' = pow(lin, 1/eotf)`. Encoded path: multiply `v` directly, same knee.

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

On the projector (owner), in cinema-time order (answers-2 §B5):
1. Slow grey fade to black — continuous; staircase ⇒ ramp rate or flux
   table.
2. Hard cut dark → bright — no flash, no clipped highlights; flash or dip ⇒
   `led_latency_ms` wrong.
3. Static mid-APL scene, 30 s — no breathing; breathing ⇒ deadband too
   small or flux-table error.
4. 5–10% grey full field (dither test alongside) — static; any sense of
   motion ⇒ sigma-delta motion gate not holding.
5. Static dark frame with a small highlight, `mode: compensate`: step the
   LED 255 → 60; mid-tones must not move.
6. Night-driving scene: headlights graduated, road at full brightness, no
   visible change when the LED moves. Tune `highlight_tolerance_stops`.
7. Lightning / muzzle-flash scene: no dip.
8. `mode: script` side by side with the Python script (log-only in one,
   live in the other) until the owner is satisfied the port matches.
9. One-off: LED latency filmed; `SHUT 1` / `PMUT 1` speed and audibility.

## 8. Portability rules (restated)

- Three layers in their own files under `src/VideoProcessor-Lib/led/`
  (or `vprenderer/led/`), plus one shader file.
- Renderer integration: exactly three call sites (exact stats out, gain in,
  and the generic queued-frame statistics observer in the lookahead walk)
  and one hook-chain entry.
- No new windows or menus; plain Config app sections only (no profile
  groups, no key bindings — answers file §9).
- Nothing in `Low_Latency` changes; the module is reactive there because
  the queue holds one frame, not because of a profile-specific setting.
- Keep `mode: script` working for as long as the module exists.
- Exit/disable/startup state handling per answers file §6 (LED restored to
  `led_ceiling` on clean exit or disable; known state re-established at
  startup).
- `SerialPort` separated from `BarcoAsciiDriver` (answers file §11).

## 9. Owner decisions and implementation notes (2026-10-09, step 4)

- **Gamma boost lives in the module.** `[led_drive] gamma_boost: 2.4`
  ("encode as if for a 2.4 display" on `compensation_eotf` 2.2) replaces the
  old `output_gamma: 2.4` lift; VP's `output_gamma` is set honestly to 2.2.
  The compensation hook applies `v^(compensation_eotf / gamma_boost)` first,
  then the script contrast gain or the compensate linear gain, matching the
  old physical order (VP's boosted signal, then the projector). The boost
  applies whenever it is configured, even with `enabled: false` or in
  `log_only`; `gamma_boost: off` disables it. The measurement applies the
  same boost so the statistics describe the boosted picture, as the script's
  screenshots did. With no calibration LUT this is identical to the old
  pipeline.
- **mode: compensate as implemented** (interpretations to confirm):
  - The LED target follows the *sustained* APL (minimum over
    `highlight_persist_ms`), so a flash never makes the LED chase it; a
    darkening acts at once.
  - Protection engages within ~100 ms once a sustained highlight would
    reach the knee after compensation, and relaxes linearly over
    `highlight_release_ms` (new key, default 2000).
  - Highlight floor = `protection x ceil(h_lin / 2^(tolerance x (1 - large)) x ceiling)`,
    where `large` is a smoothstep of the highlight area between the new keys
    `highlight_area_small` (0.005) and `highlight_area_large` (0.05):
    small sustained highlights are compressed by the knee within the
    tolerance, large ones lower the global gain (`gain = mix(gain_led,
    min(gain_led, 1/h_lin), protection x large)`).
  - Tiny highlights are free because the 99.5th-percentile statistic does
    not see anything under 0.5% of the picture (the same size as
    `highlight_area_small`); no separate rule.
  - Fade: the light target is `led_floor_black`, the LED follows the fade's
    APL ratio (at least one down-table step per tick) and the gain is held
    at its pre-fade value. Cut: jump to the target; a cut to black (APL
    below 20) uses `led_floor_black`.
  - Superseded by step 5 (delta-1 and its answers): `led_latency_ms` dead
    time replaces the one-pole `led_response_ms` model; compensate mode is
    per frame with `rate_up`/`rate_down`/`deadband` instead of step tables
    and cut jumps; lookahead statistics come from raw queued frames through
    a 1D transfer (answers-2 B6).
  - No key binding for `highlight_tolerance_stops` (owner: keys optional).

### Step 5 (delta-1) implementation notes

- Dead time: commands are numbered by output frame; the modelled level is
  the last command at least `led_latency_ms` (rounded to whole frames at
  the measured frame rate) old, and the gain follows it. One model serves
  both cases: with lookahead the command is computed for the frame shown
  when it lands; without, from the latest exact measurement.
- `compensate` runs inside the render thread's per-frame call
  (BeginFrame), under a short lock; serial commands are only queued to the
  driver's own thread, which coalesces them. The module thread only logs.
- Deadband with hysteresis: it stops a resting LED from starting to move,
  but a ramp that has started runs on to its target (otherwise a ramp could
  end up to `deadband` levels short).
- Fades: the LED follows the fade's APL ratio, never faster than
  `rate_down`; no deadband during a fade; the gain is still held.
- Early ramp trigger: a cut anywhere in the visible queue, or an APL
  difference of at least `cut_threshold` between the furthest queued frame
  and the frame the command is for. "Current scene" for the dimming bound
  is the frame shown when the command lands.
- `script` mode: unchanged controller; its emulated contrast (the gain) is
  applied `led_latency_ms` after the script step, so it lands with the LED
  commands the script sends alongside it (as the old script's CNTR
  commands did). Never early; never uses predictions.
- Flux table: outside its range the flux is clamped to the end values
  (delta-1 item 1), so a table that starts above `led_floor_black` gives no
  extra gain below its first point; measure down to the lowest level used.
  With no table the startup log line reports `lpw_max` (`?M`, else
  `led_ceiling`).
- Raw lookahead path: luma only, from the fork's `AnalysisLumaSource`
  (YUV: native limited-range codes; 10-bit RGB: limited-range luma with
  BT.709/BT.2020 weights; 8/12-bit RGB: full range). The exact pass uses
  `apl_weights` on RGB, so coloured scenes differ; the correction ratio
  (exact / predicted APL above black, 3 s window, reset whenever the
  transfer is rebuilt) absorbs it. The transfer matches libplacebo's own
  rendering of grey within about half a code for SDR and PQ sources
  (test LedRawTransferMatchesTheRenderedOutput), including the SDR black
  level that BT.1886 adds.
- Third call site: the queued-frame walk calls a neutral
  `QueuedFrameStatistics::IObserver` (src/VideoProcessor-Lib/
  QueuedFrameStatistics.h); the module subscribes from the outer renderer's
  LED update. Nothing in the walk names the LED module.
- Side effect: `active_picture_lookahead_frames: 8` also turns on the
  fork's own look-ahead inspection (crop proofs) for `[queue.Normal]`, so
  it costs some CPU per frame whether or not the LED module is enabled.
- Serial rate: at 200 ms the port carries up to 5 LED commands a second.
  The driver keeps only the newest target, and the modelled LED level
  follows the levels the driver actually wrote (counted at the write, in
  the frame it happened), so the gain steps with each real LED step. Each
  drop is at most `max_levels_per_command` (12): a full dim from 255 to 60
  takes about 3.3 s; rises are limited only by `rate_up` and the gap.
- Not built: a reserve of commands for cuts. It only matters with the
  optional pause on (a cut could otherwise wait up to `burst_pause_ms`);
  add it before enabling `burst_commands`.

### Design review 1 responses (2026-10-09)

- SF-1 (withdrawn; gamma boost stays in the module): the boost exponent
  depends only on `gamma_boost` and `compensation_eotf`, never on `enabled`
  or `mode` (tested for every combination), and the renderer runs the
  compensation hook whenever the boost is on. If the hook is configured but
  not running, the log says so once, loudly ("WARNING gamma boost NOT
  applied"), and again when it recovers.
- SF-2: owner chose 200 ms default (500 ms documented); new
  `max_levels_per_command` (12). Decision: the cap applies to dimming only.
  A capped rise would leave a bright scene's highlights compressed by the
  headroom bound for seconds, which SF-3 exists to prevent; a rise is also
  usually a cut, which masks the floor change.
- SF-3: new `apl_persist_ms` (100) for the sustained APL, separate from
  `highlight_persist_ms` (250). A cut restarts the APL window, so the new
  scene's APL drives the target at once (dimming already acted at once).
  The suggested alternative ("an APL rise of cut_threshold for two frames")
  was not built: a lightning flash covering a fifth of the picture rises
  that much for 2-5 frames and would then lift the LED. A flash that trips
  the cut detector (whole-picture change) does lift the target; tune
  `cut_threshold` if lightning shows it.
- SF-4: the GPU pass also reduces the brightest 3x3 block (`max_block`, in
  the same storage buffer, checked on the GPU in
  LedMeasurementHookMeasuresWithoutTouchingThePicture); `script` mode's
  highlight test uses it, `compensate` keeps the percentile. The raw
  lookahead path fills the same field.

### Code audit 1 responses (2026-10-09; files in docs/audit/)

- Blocker (stage7-step4-compensate.md, restart on every edit): fixed.
  `LedDriveModule::Reconfigure` applies `[led_drive]` changes, script and
  compensate switches, `led_latency_ms` and `flux_table` to the running
  module; the LED and both controllers carry on from the current level
  (a switch resets the script's emulated contrast to 50). The renderer only
  builds a new module - restoring full light, as answers-1 §6 asks - when
  the connection settings change, or the module starts or stops driving
  the projector (`enabled`, `log_only`).
- Should fix (stage7-step1-protocol.md / step5b, exit restore): fixed.
  `RestoreFullLight` first calls `BeginShutdown()`: queued targets are
  dropped, a command waiting for its reply stops waiting, and the driver
  thread never sends after it. The restore itself is bounded to 1 s in
  total, including the wait for the port.
- Should fix (stage7-step5-delta1.md, output vs source frames): fixed. The
  renderer passes the source frame rate; the queued frame a command is for
  is `led_latency_ms` in source frames ahead, and the raw-path statistics
  run on the source frame clock. Dead time stays in output frames. New test
  FilmIn60HzCountsTheLatencyInSourceFrames (24p in 60 Hz, 3:2 repeats).
- Notes acted on: the exact path skips a repeated source frame (24p fades
  are detected again); a libplacebo runtime disable of the measurement pass
  is logged; the `terminator` comment in the test config is reworded (it
  also contained two stray carriage-return bytes since step 1).
- Notes left as they are: `automatic_crop` in the test config is the owner's
  call; custom-shader order (profile order before stage, following the
  fork's madVR loader, vs the brief's stage first) and the saturation
  equivalence go to the planner; `{{refresh_hz}}` is an integer but no
  shader uses it; the compensation shader's clamp below black (BTB) is
  unchanged - libplacebo has already clamped there; with `read_back: off`
  the driver clamps to 0-255 while both controllers keep inside the floors.

### Design review 2 responses (2026-10-10; round-2 packages 15e202ec..c1b46205)

- SF-1 (`compensation_eotf`): open, waiting on the owner. The 2.41 does not
  come from a code-to-light measurement, nor through VP. It comes from the
  light-meter table of CNTR against LPW1 at equal white
  (docs/reference/LED-power-vs-contrast.md), measured on the original light
  engine. If CNTR is a gain on the encoded signal ahead of the projector's
  gamma (the fit with 50 as unity says it is), white light goes as
  (CNTR/50)^E, so 2.41 estimates the display exponent E, at whatever gamma
  preset was set at the time; that preset is unknown. The same E also
  decides `gamma_boost` (0.9167 = 2.2/2.4 assumes E = 2.2): if E were 2.4,
  the boost would make the picture darker than intended. Proposed check: a
  direct code-to-light reading with the LED drive off (so no boost), black
  subtracted, for a 50% field (code 126) against white (code 235):
  ratio 0.218 means E = 2.2, ratio 0.189 means E = 2.4. Until then the
  owner's decision of 2.2 stands. The value, and how it was measured, will
  go into the brief §4 and the config comment.
- N-1: DEPLOY.txt §6 now asks for `read_back` on for the first custom-dim
  run. With read-back on, a missing or refused LDC*/LDMM reply is logged
  ("no LDCR reply ..." or "reply ... ignored while waiting for LDCR").
- N-5 (`target_latency_ms` 120 vs 200): the owner's call; the test config
  keeps 120 (no lip-sync change) until he decides.
- N-6: agreed. The rc2 rebase is next, before anything more is added to
  the Config app or the queue.
- N-2, N-3, N-4, N-7, N-8: no change.
