# Dynamic Drive module — answers round 2: integrating the hardware design notes

2026-10-09. Reconciles "LED Drive Module — Design Notes for Planning Agent"
(from the owner's projector-hardware discussion) with `VP-led-drive-module.md`
and `VP-led-drive-answers-1.md`. Precedence: this file > answers-1 > guide
> brief, on any conflict.

---

## A. Already covered — no change

- One actuator (LED via `LPW1`); no iris of any kind; driver interface stays
  pluggable (`protocol:` key) but no second driver is planned.
- Per-frame gain compensation in float ("dynamic gamma" in the notes =
  `mode: compensate` here), applied at PRE_OUTPUT, black and near-black
  uncompensated by design.
- Configurable LED floor (`led_floor` 100 → likely 60 later,
  `led_floor_black` for fades), fade-to-black as a goal.
- Asymmetric rates (brighten fast, dim slow), deadband (= `anti_pump_band`),
  lookahead via the fork's queued-frame walk.
- Upstream hygiene: PR-able code never references the LED module;
  `Low_Latency` untouched; `source_rate` auto-selection.

## B. New requirements — adopt

### B1. Flux lookup table (new, important)

The light engine is non-stock; the projector's startup colour balance does
not characterise absolute flux vs `LPW1`. Do not assume flux is linear in
the command value.

- New config key `flux_table` in `[projector.Default]`: pairs of
  `lpw:relative_flux`, piecewise-linear between points, e.g.
  `flux_table: 60:0.55, 80:0.78, 100:1.00` (values illustrative).
- Default when absent: linear (`flux = lpw / lpw_max`).
- The controller's LED term becomes
  `gain_led = flux(led_ceiling) / flux(light_actual)` — everywhere the
  guide says `light_full / light_actual`, read flux ratio.
- The highlight policy's headroom limit uses the same flux ratio.
- Table to be measured with the ES-1000 later (fixed mid-grey, `LPW1` 100→60
  in steps of 5, log luminance and xy); until then linear. Log a one-line
  warning at startup when the default is in use.

### B2. Latency: one parameter, two uses

The notes ask for the gain to be delayed by a fixed offset so gain and LED
land together. The guide asks for commands to be sent `led_response_ms`
ahead. These are the same requirement from two sides; unify:

- `led_latency_ms` (rename of `led_response_ms`, and moved to
  `[projector.Default]` since it is a property of the projector): command
  transmission plus projector response, measured once with a camera.
- With lookahead available: the command is sent `led_latency_ms` before the
  frame it applies to; the gain is applied on that frame.
- Without lookahead (`Low_Latency`, or a change the walk could not see):
  the command is sent now and the gain is delayed by `led_latency_ms`
  (rounded to frames at the output rate) so it still lands with the LED.
- In both cases the gain follows the modelled LED trajectory (one-pole lag
  with that latency) rather than the command, as before.

### B3. Slew-limited ramps in `compensate` mode

In `compensate` mode the controller runs **per frame**, not per tick, and
moves the LED toward its target by at most `rate_up` / `rate_down` levels
per second (config), with the deadband around the current level. The gain
follows the same ramp on the same frames. The script's per-tick step
tables remain the behaviour of `mode: script` only (they are what the owner
tuned and are kept for the faithful comparison). `tick_ms` therefore
applies to `script` mode; `compensate` is frame-driven.

Consequence for the serial side: per-frame targets at 60 Hz exceed what the
port can carry. The driver coalesces as already specified (always send the
newest target, skip stale ones) and the modelled trajectory, not the ACK
stream, drives the gain. Expect a sustainable command rate in the region
of 5–10 per second; the ramp is smooth in the picture regardless because
the gain is per frame.

### B4. Early ramps before a known cut — permitted under compensation

Answers-1 and the guide said "never move the LED earlier than its latency,
or the picture changes before the cut." That rule was written for the
uncompensated case and is too strict for `compensate` mode. Corrected rule:

- **`script` mode:** unchanged — never early.
- **`compensate` mode:** when the lookahead walk shows a cut (or a large
  APL change) ahead, the controller may begin the ramp early so the LED
  arrives at the new level on the cut frame, **because the gain holds the
  content constant during the ramp**. The only thing that moves before the
  cut is the black floor and, in the dimming direction, highlight headroom.
  So the early ramp is bounded by the *current* scene's highlight policy: it
  may dim ahead of a cut only as far as the current scene's highlights
  permit within `highlight_tolerance_stops`; brightening ahead of a cut to
  a bright scene is unconstrained (headroom only increases). Within those
  bounds, start as early as the walk allows.
- This is what makes the fast-response vs smooth-in-scene trade-off
  disappear, and it is the capability the screenshot script never had.

### B5. Acceptance tests (replace the guide's §7 projector list)

Short, in cinema-time order:
1. **Slow grey fade to black** — must look continuous. Staircase ⇒ ramp
   rate or flux table.
2. **Hard cut dark → bright** — no flash, no clipped highlights. Flash or
   dip ⇒ `led_latency_ms` wrong.
3. **Static mid-APL scene, 30 s** — no breathing. Breathing ⇒ deadband
   too small or flux-table error.
4. **5–10% grey full field** (dither test, run alongside) — static, ideally
   invisible; any sense of motion ⇒ sigma-delta motion gate not holding.
5. Keep from the guide: night-driving headlights; lightning flash; the
   `script`-mode side-by-side with the Python script; the one-off latency
   and `SHUT`/`PMUT` measurements.

### B6. Lookahead statistics are computed on raw frames, mapped through the profile

The fork's queued-frame walk runs on the CPU on the *raw captured* frames,
before tone mapping. Lookahead statistics therefore cannot be measured at
PRE_OUTPUT (those frames have not been rendered). Resolution:

- **Raw path for queued frames:** in the existing walk, take the luma plane
  of the v210 4:2:2 data (subsampled is fine), map each sample through a
  **1D input-code → output-code transfer** built once per rendering
  profile — input transfer (PQ for HDR/LLDV, gamma for SDR) → the static
  tone-map curve at the configured target nits → `output_gamma` →
  limited-range code — and compute `apl`, the highlight percentile and the
  block structure from the mapped values. Rebuild the transfer when the
  profile changes. This is accurate to a code or two for APL, which is far
  inside the controller's band widths.
- **Exact path for the presented frame:** the GPU measurement at
  PRE_OUTPUT stays as specified for the frame being rendered.
- **Self-correction:** for each presented frame the module has both the
  raw-predicted APL (from when it was queued) and the exact rendered APL.
  Keep a smoothed ratio of the two (seconds-long window) and apply it to
  the predictions for frames still in the queue. This absorbs what the 1D
  model ignores (gamut mapping, channel weights, any profile detail)
  without modelling it.
- Cut and fade flags are computed on raw luma; they measure change, not
  level.
- `script` mode does not use raw-frame predictions: it keeps the exact
  PRE_OUTPUT measurement of the presented frame, which is what the Python
  script saw (corrected per the delta-1 answers).

## C. Corrections to the notes (do not implement as written)

### C1. "Digital gain into 8-bit output costs headroom; bound by what the dither can carry"

The gain is applied in float before the quantizer, so it does not cost
precision — the sigma-delta quantises the compensated picture exactly as
it would any other. What gain *does* cost is highlight headroom: content
above `1/gain` clips. That is already handled by the highlight policy and
the knee, not by the dither. The coupling to plan and test together is
therefore highlights-vs-floor (§4.3 of the guide), and the dither's job is
unchanged. Test 4 above is still a good joint test, for the reason the
notes give (floor motion), not for headroom.

### C2. Rationale note (for the record)

The ANSI argument in the notes — the projector's internal floor scales with
LED while room bounce scales with content, so dimming plus compensation
lowers the dark-square value for the same bright-square value — is correct
and is the clearest statement of why this module is worth its place. Add it
to the guide's §1.

## D. Config additions (summary)

```
[projector.Default]
flux_table:            # lpw:relative_flux pairs; absent = linear
led_latency_ms: 80     # replaces led_response_ms; measured once

[led_drive.Default]
rate_up: 400           # levels per second, compensate mode
rate_down: 120         # levels per second, compensate mode
deadband: 3            # levels; = anti_pump_band for script mode
early_ramp: true       # compensate mode: ramp ahead of a known cut (B4)
```

`tick_ms` and the step tables: `script` mode only.

## E. Deferred (unchanged, recorded so nobody builds them)

Colorimeter package (flux table measurement, white-point hold across the
range, LED colour-coordinate refinement); 660 nm red assist; any 10-bit
input path; remaining hardware stray-light items. None of these are VP work.
