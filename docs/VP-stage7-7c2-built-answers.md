# Stage 7 step 7c-2 as built — planner answers

2026-10-11. On stage7-rc2 728ebc24 (7c-2), f6bb3c10 (7c-1 answers) and
c539459c (7c-1 audit fixes). All acknowledged as built; one correction
to the fade-up's rates; the next step is 7c-3.

## Acknowledged

- 7c-1 audit fixes: CNTR never left above the paired need (the owner's
  "stuck 51" was exactly this), margin only on the way up; a waiting CNTR
  dropped when a fade's gain hold or custom dim begins. Right.
- 7c-2: single dump to 50 (or partial to hold `max_cut_dip`) landing with
  the LED's jump; CNTR never rising during the fade-up; residual
  descending with the bias; `cut_cntr_dump: off` holds CNTR; fade-up
  starting from the CNTR just sent and ending only when full
  compensation fits under the ceiling; `dmd_clip_cntr` limiting software
  headroom, fade-up ceiling and the floor's use of CNTR; scene floor =
  max(`led_floor`, the `max_cut_dip` floor) = 102 at 0.5; dark cut paired
  on the modelled light now; `cntr_assist_max` lifted with the key kept.
  All as answered.
- Information 1: yes — with CNTR held, a bright cut out of an assisted
  dim can be at full brightness on the cut frame with no fade-up, only
  the paired descent. That is precisely the other side of the A/B.
- Information 2: regression recipe + `max_cut_dip: 0`, `shaped_recovery:
  off`. Agreed; record it.

## Information 3 — the fade-up must not use the LED's script-matched rates

Not intended; thank you for measuring it. A 15 s fade-up after a bright
cut would be the most visible thing in the build and the opposite of the
policy's intent. The script-matched rates exist to hide *within-scene*
drift, where slow is the point. After a cut the cut has already masked
the change, and the recovery should be as fast as the LED's light allows
— the ceiling in the shaped recovery is the LED's modelled light, so the
rate law cannot outrun the LED anyway; its job is only to smooth the
LED's step response, not to slow the recovery below it.

Decision: **two rate laws.**
- `rate_*` (within a scene): script-matched, as now.
- `recovery_rate_*` (after a cut, both directions): fast — seed from the
  "compensated" preset (≈1 s floor-to-full), with the same clamp form.
  The shaped recovery's `L_R` ramp uses `recovery_rate_*`; the LED's own
  command after a cut is not rate-limited at all (it is one command to
  the scene's level; its physical curve is the limit). A dark cut's
  paired descent after the cut also uses the recovery law (the floor
  dropping fast after a cut is invisible), then hands back to the scene
  law once the target is reached.
- `rate_preset` applies to both families (script-matched / compensated)
  so the owner can still try the fast within-scene variant.

Record in the policy §3.2 (rate law: "within a scene") and §3.3 ("the
recovery ramp runs under the recovery rate law; the LED command on a cut
is not rate-limited").

## On the policy file

Apologies for the dropped black-frames note again; my copy was behind. I
will stop sending whole copies of the policy. From here, policy changes
come as short deltas against the repo's file, and the next package should
include the repo's current `VP-led-drive-control-policy.md` so I amend
from it.

## Next

7c-3: look-ahead extras (black-frame variant, head start, exit from
black) and the Config page keys — with the two rate families on the page.
