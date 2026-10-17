# Dynamic Drive module — answers to implementation questions (round 1)

2026-10-09. Answers are decisions unless marked "verify". Where this file
contradicts `VP-led-drive-module.md` or the brief, this file wins; fold the
changes into those documents (and note them under the brief's Open
questions where something still needs a projector test).

---

## 1. Contrast handling — never send CNTR; emulate it in VP

Decision: the projector's contrast control is never driven. Replace
`mode: contrast` with **`mode: script`**: a faithful port of `beta_22.py`
where the LED power path is unchanged and the contrast path is emulated in
VP by a gain that reproduces what the projector's CNTR control would have
done to the picture.

Definition of the emulation. Assumption (standard for projector contrast
controls, not verified on the FL35): CNTR is a gain on the encoded signal in
code space, before the projector's own EOTF, with 50 as unity. If the owner's
eye says `script` mode is brighter or darker than the old script was, the
first thing to try is `gain_domain = linear` for this mode. So:

```
contrast_target  = script's contrast bands (50 … 74), driven by APL and
                   highlight state exactly as the script does
contrast_actual  = contrast_target followed with the script's dynamics
                   (1 step/tick, both fast dumps, anti-pump band)
gain_encoded     = contrast_actual / 50          (1.0 … 1.48)
```

applied as a multiply on the **encoded** (gamma-space) value at the
compensation hook, with the knee engaging as the value approaches 1.0 so it
soft-clips instead of hard clipping (the projector's CNTR presumably
clipped; soft is a small improvement either way).

Relation to `mode: compensate`: both modes drive the same hook with the
same two parameters (`gain`, `knee`). They differ in how the gain is
derived and in which domain it is applied:

- `script`: gain from the contrast bands, applied in encoded space
  (emulating CNTR).
- `compensate`: gain from the LED level and the highlight policy, applied
  in linear light (physically exact for an LED that scales light).

The hook therefore takes a third parameter, `gain_domain` (0 = encoded,
1 = linear), set by the controller per mode. See question 7 for the stage
and the linearisation.

Startup: the driver reads `CNTR?`; if it is not 50 (the old script may have
left it at 74), set it to 50 once and log that. After that CNTR is never
written again. The `set_contrast` interface method stays, used only for
that startup correction.

Modes are therefore: `compensate | script | log_only`.

## 2. Step tables — follow the script, make the APL split configurable

Confirmed: the guide's tables were my summary and were wrong in two places;
`beta_22.py` is authoritative. Config shape:

```
steps_up_dark:   100:7, 50:5, 25:4, 10:1, 0:1     # applies when APL < steps_up_split_apl
steps_up_bright: 100:7, 50:4, 25:2, 10:1, 0:1     # applies when APL >= split
steps_up_split_apl: 40
steps_down:      (exactly the script's down table; transcribe it from the source)
```

Each table is "distance threshold : step", evaluated from the largest
threshold down. If the script's structure doesn't fit this shape exactly,
pick the shape that reproduces it exactly and say so.

## 3. Second fast contrast dump (7–9 above target, every 100 ms)

Keep it in `mode: script` — it is part of the behaviour the owner tuned, and
`script` mode is defined as the faithful port. Drop it in `mode: compensate`,
where the continuous highlight policy supersedes both dumps.

## 4. Measurement feedback — measure the uncompensated picture

Confirmed: statistics must come from the picture before the compensation
gain, or the loop feeds back. Decision: **measure before the gain hook**, not
by dividing it out (division is not invertible through the knee).

Implementation: the measurement pass is the first entry in the hook chain
at the same stage as the compensation hook (PRE_OUTPUT, see 7), reading the
hooked texture and writing only its statistics buffer, never the image.
The compensation hook follows it. The sigma-delta hook stays at OUTPUT,
after both.

Note this means the APL is measured at the same point the gain is applied,
which is after tone map, gamut map and LUT — the same picture the script was
measuring from the screenshot, before the projector's contrast. Good.

Scale: PRE_OUTPUT values are normalised 0–1 (pre-encode), but the script's
curves are in 0–255 limited-range codes. Convert for the statistics:
`code = 16 + 219 × value`, so `apl`, `highlight` and the curve breakpoints all
stay in the script's units.

## 5. Greyscale weights — reproduce the script's effective weights

Decision: configurable, default to what the script actually computed:

```
apl_weights: 0.114, 0.587, 0.299     # R, G, B — the script's effective weights
```

with true Rec.601 (`0.299, 0.587, 0.114`) documented as the alternative.
The owner's curves were tuned on the script's numbers, so matching them is
what makes the port faithful; the difference only matters on strongly
coloured scenes, and the owner can switch later and retune if he wishes.
Put a one-line comment in the config explaining why the default looks
"backwards".

## 6. Projector state on exit, disable, mode change, crash

Agree with the proposal, with two additions:

- Clean exit, module disabled, or mode changed to `log_only`: restore
  `LPW1` to `led_ceiling` (CNTR is already 50 and is not touched). Do this
  synchronously with a short timeout (≤ 1 s) so shutdown cannot hang on the
  port.
- Mode change between `script` and `compensate`: no restore; the controller
  continues from the current level.
- Startup: before engaging, always bring the projector to a known state
  (`LPW1` = `led_ceiling`, CNTR = 50 if not already), so a previous crash
  leaves nothing behind once VP runs again. Log the state found.
- Crash: last level persists until the next VP start. Cannot be helped;
  acceptable.

## 7. Compensation hook stage — PRE_OUTPUT, encoded domain, hook linearises itself

Verified against the bundled libplacebo 7.360.1 `renderer.c`:

- `PL_HOOK_LINEAR` is before scaling **and before tone mapping**, in the
  *source* colour space. Applying a gain there would change the tone
  mapper's input on HDR sources. Not usable.
- `PL_HOOK_SCALED` is after scaling, before colour management. Same problem.
- `PL_HOOK_PRE_OUTPUT` (renderer.c ~line 2742) is **after colour management
  — tone map, gamut map and the target LUT — and before blending/rotation**,
  in the target colour space with the target transfer (your `output_gamma`,
  2.4), values normalised 0–1, before the limited-range encode.
- `PL_HOOK_OUTPUT` (~line 2833) is after encode, where the sigma-delta lives.

Decision: the compensation hook (and the measurement pass, question 4) go at
**PRE_OUTPUT**. Verify the exact domain there once more in renderer.c when
you write it (full-range normalised, target transfer, pre-encode).

Because PRE_OUTPUT is in the encoded target transfer, the hook does its own
linearisation for `gain_domain = linear`:

```
lin   = pow(v, compensation_eotf)
lin  *= gain                     // then knee above the headroom limit
v'    = pow(lin, 1.0 / compensation_eotf)
```

`compensation_eotf` is a new `[led_drive]` key: the projector's **actual**
display gamma, default 2.2 (the FL35 runs "film 2.2"; VP's `output_gamma`
is deliberately 2.4 for the brightening mismatch, so don't reuse that
value). Physically the LED scales light, so the exact inverse is in the
projector's real EOTF.

For `gain_domain = encoded` (script mode) the hook multiplies `v` directly,
no linearisation.

Why not apply the gain before the LUT: with a LUT present, the projector's
light is `LED × EOTF(LUT(x))`, so the exact compensation is after the LUT in
linear light — which is what PRE_OUTPUT gives. With no LUT (the owner's
current state) the two are the same.

## 8. Definitions

- **`light_full`** = `led_ceiling` from config, clamped to the reported
  `?M`. `gain_led = led_ceiling / light_actual`. (If the owner later sets a
  ceiling below the maximum, the compensation is relative to that ceiling,
  which is what he'd expect.)
- **`highlight_linear`** = `pow((p − 16) / 219, compensation_eotf)` where
  `p` is the 99.5th-percentile code (limited range, clamp (p−16)/219 to
  0…1). Uses the same EOTF as the hook so the two agree.
- **`highlight_area`** = fraction of 3×3 blocks whose linear block mean
  exceeds the **headroom limit** `(1 − knee) / gain_led` — i.e. the blocks
  that would reach the knee after compensation. That limit is the "knee
  threshold".
- **Fade window `N`** = `fade_frames`, default 6 (≈ 100 ms at 60 Hz,
  ≈ 250 ms at 24p). Fade = APL decreasing on each of the last N frames by
  at least `fade_min_step` (default 0.5 code) with the frame-to-frame block
  difference below the cut threshold.
- **Cut threshold** = mean absolute difference of block luma between
  consecutive frames above `cut_threshold`, default 24 codes (≈10% of
  range). Use the fork's own scene-change signal instead if one exists and
  it fires on the same frame.

All five are config keys with those defaults.

## 9. Config app — plain settings sections

Plain `[projector]` and `[led_drive]` sections, edited in the Config app's
raw/advanced view. No profile groups, no dedicated pages, no key toggle.
`enabled` is a key in the section. This is the minimum-work option and the
owner has confirmed it. If profile groups are ever wanted, that's a later
change and nothing here precludes it.

## 10. Order — proceed now

Confirmed: build Stage 7 now, regardless of the Stage 3 ramp test. The
owner's testing time is the bottleneck and he is deliberately banking
interim builds to test in batches. The ramp test still has to happen before
anyone judges the dithering; it just isn't a gate for this work. Keep the
sigma-delta disabled by default in the test config as before.

## 11. Stage 9 lux sensor — separate the transport now, build the driver later

Split the protocol layer into a reusable `SerialPort` (open/close,
read/write with timeouts, its own thread) and the `BarcoAsciiDriver` that
uses it. That costs nothing now and means the Stage 9 sensor is a second
small driver on a second `SerialPort` instance, not a redesign. Do not write
the sensor driver yet.

---

## Summary of changes to make in the guide and brief

- Modes: `compensate | script | log_only`; `mode: contrast` removed; CNTR is
  only ever written once at startup to restore 50.
- Step tables: `steps_up_dark` / `steps_up_bright` / `steps_up_split_apl`;
  transcribe all tables from the script.
- Second fast dump kept in `script` mode only.
- Measurement pass precedes the compensation hook at PRE_OUTPUT; both
  before the sigma-delta at OUTPUT.
- `apl_weights` default `0.114, 0.587, 0.299`.
- Exit/startup state handling as in question 6.
- Hook at PRE_OUTPUT; new keys `compensation_eotf` (2.2), `gain_domain`
  (set by mode), `fade_frames`, `fade_min_step`, `cut_threshold`.
- Plain config sections, no profile groups, no key toggle.
- `SerialPort` separated from `BarcoAsciiDriver`.
