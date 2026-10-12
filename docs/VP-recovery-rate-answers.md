# recovery_rate_* — the missing answer, restated, and answers to the three questions

2026-10-11. The earlier reply (to your 7c-2 note, information 3: the
15 s fade-up) did not reach you. What you applied in 6f943314 matches it
exactly, so nothing is lost; this restates it and answers the three
questions. The implementation note at the end is confirmed.

## The answer that went missing

A 15 s fade-up after a bright cut is the opposite of the policy's intent.
The script-matched rates exist to hide *within-scene* drift, where slow is
the point; after a cut the change is already masked, and the recovery
should be as fast as the LED's light allows. So, two rate families:

- `rate_*` — within a scene: script-matched, as now. Also the paired
  LED/CNTR moves *after* a cut, because content is constant during a
  paired move and slow is invisible (the owner's kinetics).
- `recovery_rate_*` — the displayed-light ramp for the remaining
  shortfall after a cut, bounded by the modelled light. Fast.
- The LED command on a cut is one command to the scene's level, not
  rate-limited; its physical curve is the limit.
- `rate_preset` (script-matched / compensated) applies to both families.

## 1. Defaults — confirmed

`recovery_rate_gain 4.0`, `recovery_rate_min 20`, `recovery_rate_max 400`
levels/s, from the compensated preset's up values; ≈1 s from 0.4 to full.
Keep them. They only matter where a shortfall exists after CNTR is at
`cut_cntr_level`, which with the owner's floors is small.

## 2. Where else `recovery_rate_*` applies

- **Exit from black (7c-3):** yes — the displayed-light ramp after the cut
  uses the recovery law. The pre-positioning during black is not a ramp
  at all: CNTR to `cut_cntr_level` in one command, LED to
  `black_prelift_max` in one command (no rate law on black).
- **LED after an unseen cut:** no — the LED command is still one command
  to the scene's level, unramped; the recovery law shapes only the
  *displayed* light. The paired CNTR descent that follows is at the
  script-matched rates.
- **Dark cut:** the paired descent after the cut stays at the
  script-matched rates (the floor dropping slowly is invisible; there is
  no shortfall ramp in the dark direction). The earlier reply had said
  the dark cut's post-cut descent could use the recovery law; drop that —
  the owner's correction (paired moves at script rates) governs.
- Nothing within a scene.

## 3. Anything else from that answer

- Record in the policy (delta against the repo's file): §3.2 "rate law:
  within a scene"; §3.3 "the displayed-light ramp for the remaining
  shortfall runs under `recovery_rate_*`; the LED command on a cut is not
  rate-limited; the paired move after the cut is at the scene rates."
- The regression recipe grows by `max_cut_dip: 0` and
  `shaped_recovery: off` (your information 2) — already agreed.
- Nothing else.

## Implementation note — confirmed

Correct on all four points, and the reasoning is the right one: above
`dmd_clip_cntr` more CNTR adds no highlight light, so putting the clip
into the CNTR need would make each step ask for the next. "CNTR stops at
`dmd_clip_cntr` while the scene has highlights; the highlight floor uses
CNTR only up to it; the fade-up ceiling shrinks in proportion above it;
without highlights the assist may go to `cntr_max_usable`" is the
intended behaviour. Record that wording in the policy's knee-term
description so the next reader does not reintroduce the clip into the
need.
