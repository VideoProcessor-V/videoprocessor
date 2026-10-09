# Delta 1 — questions from the implementer

2026-10-09. Read against `docs/VP-led-drive-delta-1.md` and the current
implementation (Stage 7 steps 1–4, Stage 2 addendum; branch
`stage2-addendum-upscaling`, commit `8db148cf`). Nothing is implemented yet.
Each question has the implementer's proposed answer; confirm or correct.

---

## 1. What `led_latency_ms` models

Item 2 uses it two ways: as a pure delay (gain delayed by `led_latency_ms`,
rounded to whole output frames) and as the time constant of a one-pole lag
on the modelled trajectory. Applied together, the gain would be delayed
twice (dead time, then a further exponential lag).

**Proposal:** treat it as a pure delay. The modelled LED level switches to
the commanded value `led_latency_ms` after the command (rounded to whole
output frames), and the gain switches with it. If the camera measurement
shows a gradual settle after the delay, add a separate time-constant key
then.

## 2. Default for `led_latency_ms`

The delta gives none. Rev 13 / the revised guide gave 80 ms for
`led_response_ms`.

**Proposal:** 80 ms. The old `led_response_ms` key in `[led_drive]` is
rejected with a message naming its new place, so an old config cannot keep a
stale value silently.

## 3. Cuts and fades under the ramp limits (`compensate`)

Today `compensate` jumps the LED to the target on a cut and follows a fade
at its own APL ratio. Item 3 limits every move to `rate_up` / `rate_down`.

**Proposal:**
- A cut the lookahead did not see: rate-limited like any other change
  (at 400/s a full-range brighten takes ~0.5 s; the gain keeps content
  constant throughout).
- A cut the lookahead saw: item 4's early ramp.
- A fade to black: follows its own rate but never faster than `rate_down`;
  gain held as today.

## 4. Lookahead is currently off, and its depth is short

`active_picture_lookahead_frames` is not set in the test configuration, so
it is 0 and items 2 (lookahead path), 4 and 5 never run. The fork caps it at
8 frames (~0.33 s at 24p) and shares it with its black-bar detector, so
enabling it also changes that detector's behaviour.

Even at 8 frames, item 4's early ramp is mostly partial: a full-range
brighten needs ~0.5 s at `rate_up` 400, a full-range dim ~1.6 s at
`rate_down` 120. Full early ramps need Stage 5's longer queue.

**Proposal:** set `active_picture_lookahead_frames: 8` in `[queue.Normal]`
only (`Low_Latency` untouched), and note the partial-ramp limitation.

## 5. A third renderer touch point

The guide allows exactly two (statistics out after a frame, gain in before
one) plus one hook-chain entry. Item 5's lookahead statistics need a third:
a call inside the existing queued-frame walk (`AnalyzeActivePictureLookahead`)
that hands each newly queued frame's predicted statistics to the module.

**Proposal:** accept the third call site, inside the existing walk (no
parallel mechanism), and update the guide's portability rule and the audit
checklist accordingly.

## 6. `script` mode and the mapped values

Item 5 says `script` mode "uses the mapped values too". It already uses
output-domain values — the exact PRE_OUTPUT measurement of the rendered
frame — which is what the Python script saw. Item 4 keeps `script` mode
"never earlier than `led_latency_ms`", and item 6 requires its behaviour to
be unchanged.

**Proposal:** `script` mode keeps the exact measurement of the presented
frame and does not use raw-frame predictions. Its tick-for-tick equivalence
test against `beta_22.py` stays as the "unchanged" proof.

## 7. Approximations in the raw-frame prediction (for the record)

- Raw frames carry luma (Y') only, so `apl_weights` (R/G/B) cannot be
  applied to predictions; the smoothed correction ratio absorbs the
  difference, as it does gamut mapping, `saturation` and
  `contrast_recovery`.
- The 1D transfer will use libplacebo's own curve (`pl_tone_map_sample`
  with the configured tone-mapping function and target nits) rather than a
  re-implementation, and includes the module's `gamma_boost` (the exact
  measurement sees the boosted picture).
- The fork's frame reader (`AnalysisLumaSource`) also handles RGB capture,
  so the walk will cover RGB sources too, not only v210.

No decision needed unless you disagree.

## 8. "lpw_max" in item 1

With the linear default, `flux(ceiling) / flux(actual)` = `ceiling / actual`
whatever `lpw_max` is, so it cancels.

**Proposal:** `lpw_max` = the projector's reported maximum (`?M`), falling
back to `led_ceiling`; it only matters for the startup log line.
