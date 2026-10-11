# Stage 7 steps 7a / 7b — planner answers

2026-10-11. On stage7-rc2 commits ad5b738c (7a) and 23734e0c (7b). Both
confirmations are yes; one acknowledgement; carry on with 7c-1.

## 7a — acknowledged, including the catch

- Sign-preserving powers throughout `LED compensation.glsl`, with the
  code-40 GPU test: as answered.
- **The boost-base catch is correct and important.** With the old base,
  `compensation_eotf / gamma_boost` = 2.41 / 2.4 ≈ 1.004 would have
  silently removed the owner's boost — precisely the coupling delta 4 was
  written to eliminate. Taking the boost's base from the actual output
  transfer per frame (2.2 normally; BT.1886 counted as 2.4) and using
  `compensation_eotf` once, at the end, for the gain in linear light, is
  the right structure. Record the catch in the module guide §6 so nobody
  reintroduces the old base.

## 7b — acknowledged

- Synchronous sends in the script's timing, sleeps counted from the end
  of the previous send, no coalescing, repeats sent, script sends exempt
  from `min_command_gap_ms` and the burst pause: as answered (Q3).
- Built-in curve with the literal `0.8333`; CNTR 50 unconditionally at
  start, restored after the light on exit and before a live switch to
  compensate: as intended.
- `script_smoothing` directions are right: a decrease ramps the gain
  down *before* landing (the gain cannot add light, so it must arrive at
  the step); an increase cancels the jump at landing and ramps up *after*.
  Never above 1. Keep the 200 ms bound.

## Confirmation 1 — `cntr_latency_ms: 80` as a guess: acceptable

Yes, until 7c's `CntrModel` measures it. CNTR is likely faster than the
LED; an over-estimate only delays the smoothing bridge by a few frames.
Nothing visible either way.

## Confirmation 2 — CNTR acts on the encoded signal: correct for the FL35

Yes, as far as the evidence goes. The light-meter table fits
`(CNTR/50)^2.41` at constant LED, which is what a gain applied *ahead of*
the display gamma produces; a gain in linear light would fit a different
exponent. So the smoothing gain belongs in the encoded domain. The only
thing that would justify measuring it directly is the script-mode
side-by-side showing mid-tones that do not match the script at high CNTR.

## Next

7c-1 per the control policy and the step 7 answers (CNTR commands only on
integer crossings; fast-biased LED model, `led_settle_ms: 300`, until the
camera measurement).

## Implementer note

The catch is recorded in docs/VP-led-drive-module.md §6, with "do not
reintroduce the old base". The two older notes (§9 step 4, design review
1 SF-1) that still gave `compensation_eotf / gamma_boost` are marked
superseded.
