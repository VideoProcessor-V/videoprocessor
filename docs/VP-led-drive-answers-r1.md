# LED drive module — answers to implementation questions (round 1)

2026-10-09. Answers are decisions unless marked "verify". Where this file
contradicts `VP-led-drive-module.md` or the brief, this file wins; the changes
are folded into those documents (and noted under the brief's Open questions
where something still needs a projector test).

## 1. Contrast handling — never send CNTR; emulate it in VP

Decision: the projector's contrast control is never driven. Replace
`mode: contrast` with `mode: script`: a faithful port of `beta_22.py` where the
LED power path is unchanged and the contrast path is emulated in VP by a gain
that reproduces what the projector's CNTR control would have done to the
picture.

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

applied as a multiply on the encoded (gamma-space) value at the compensation
hook, with the knee engaging as the value approaches 1.0 so it soft-clips
instead of hard clipping.

Relation to `mode: compensate`: both modes drive the same hook with the same
two parameters (`gain`, `knee`). They differ in how the gain is derived and in
which domain it is applied:

* `script`: gain from the contrast bands, applied in encoded space (emulating CNTR).
* `compensate`: gain from the LED level and the highlight policy, applied in
  linear light.

The hook therefore takes a third parameter, `gain_domain` (0 = encoded,
1 = linear), set by the controller per mode.

Startup: the driver reads `CNTR?`; if it is not 50, set it to 50 once and log
that. After that CNTR is never written again. `set_contrast` stays, used only
for that startup correction.

Modes: `compensate | script | log_only`.

## 2. Step tables — follow the script, make the APL split configurable

`beta_22.py` is authoritative.

```
steps_up_dark:   100:7, 50:5, 25:4, 10:1, 0:1     # applies when APL < steps_up_split_apl
steps_up_bright: 100:7, 50:4, 25:2, 10:1, 0:1     # applies when APL >= split
steps_up_split_apl: 40
steps_down:      100:5, 50:3, 25:2, 10:1, 0:1     # transcribed from the script
```

Each table is "distance threshold : step", evaluated from the largest
threshold down.

## 3. Second fast contrast dump (7–9 above target, every 100 ms)

Keep it in `mode: script`. Drop it in `mode: compensate`, where the continuous
highlight policy supersedes both dumps.

## 4. Measurement feedback — measure the uncompensated picture

Statistics come from the picture before the compensation gain (measure before
the gain hook, not by dividing it out). The measurement pass is the first
entry at the same stage as the compensation hook (PRE_OUTPUT), reading the
hooked texture and writing only statistics, never the image. The compensation
hook follows it. The sigma-delta stays at OUTPUT, after both.

Scale: PRE_OUTPUT values are normalised 0–1; statistics are converted with
`code = 16 + 219 × value` so `apl`, `highlight` and the curve stay in the
script's units.

## 5. Greyscale weights — reproduce the script's effective weights

```
apl_weights: 0.114, 0.587, 0.299     # R, G, B — the script's effective weights
```

True Rec.601 (`0.299, 0.587, 0.114`) is the documented alternative.

## 6. Projector state on exit, disable, mode change, crash

* Clean exit, module disabled, or mode changed to `log_only`: restore `LPW1`
  to `led_ceiling` synchronously with a short timeout (≤ 1 s).
* Mode change between `script` and `compensate`: no restore.
* Startup: always bring the projector to a known state (`LPW1` =
  `led_ceiling`, CNTR = 50 if not already); log the state found.
* Crash: last level persists until the next VP start.

## 7. Compensation hook stage — PRE_OUTPUT, encoded domain, hook linearises itself

`PL_HOOK_PRE_OUTPUT` is after colour management (tone map, gamut map, target
LUT) and before blending/rotation, in the target colour space with the target
transfer, normalised 0–1, before the limited-range encode (verify in
renderer.c when implementing). For `gain_domain = linear` the hook does:

```
lin   = pow(v, compensation_eotf)
lin  *= gain                     // then knee above the headroom limit
v'    = pow(lin, 1.0 / compensation_eotf)
```

`compensation_eotf` (new `[led_drive]` key, default 2.2) is the projector's
actual display gamma, not VP's `output_gamma`.

## 8. Definitions

* `light_full` = `led_ceiling`, clamped to the reported `?M`;
  `gain_led = led_ceiling / light_actual`.
* `highlight_linear` = `pow(clamp((p − 16) / 219, 0, 1), compensation_eotf)`,
  `p` = 99.5th-percentile code.
* `highlight_area` = fraction of 3×3 blocks whose linear block mean exceeds
  `(1 − knee) / gain_led`.
* `fade_frames` (6), `fade_min_step` (0.5 code): APL decreasing each of the
  last N frames by at least the step, block difference below the cut threshold.
* `cut_threshold` (24 codes): mean absolute block-luma difference between
  consecutive frames; use the fork's scene-change signal if it fires on the
  same frame.

## 9. Config app — plain settings sections

Plain `[projector]` and `[led_drive]` sections, edited in the Config app's
raw/advanced view. No profile groups, no dedicated pages, no key toggle.

## 10. Order — proceed now

Build Stage 7 now, regardless of the Stage 3 ramp test. Sigma-delta stays
disabled by default in the test config.

## 11. Stage 9 lux sensor — separate the transport now

Split the protocol layer into a reusable `SerialPort` and the
`BarcoAsciiDriver` that uses it. Do not write the sensor driver yet.
