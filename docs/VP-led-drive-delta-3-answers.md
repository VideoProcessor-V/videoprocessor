# Delta 3 — answers to the owner requirements and the implementer's questions

2026-10-11. Read with `VP-led-drive-delta-3.md` (repo copy, with the owner's
section) and `VP-led-drive-delta-4.md`. These answers take precedence.

## 1. Fallback that is the script, exactly

- The fallback is `script` mode, **re-defined to send real CNTR commands**.
  Today's `script` mode emulates CNTR with VP gain because CNTR was "never
  driven"; delta 3 reinstates CNTR, so fidelity now means the script's
  actual bytes: `LPW1` and `CNTR` values, order and pacing as
  `beta_22.py`, tick-for-tick. Verify against the script and report every
  difference; the two documented one-level differences stay as the
  script has them.
- `script_smoothing: off | on`, default off. Off = byte-identical to the
  script on the wire and no VP gain at all. On = the same commands, plus
  VP gain interpolating between the integer CNTR steps and across the
  command latency so content stays constant between steps; the commands
  do not change.

## 2. Lookahead adapted to the hybrid law

Plan over the whole available window (4 future frames ≈ 167 ms at 24p
with `target_latency_ms: 200`); there is no benefit in planning less.

- **Known brightening cut at T:** send the CNTR command so it lands on the
  cut frame (T − `cntr_latency_ms`), and send the LED command to the new
  scene's operating point at the same time (T − `led_latency_ms`). See §3:
  both jump; nothing ramps.
- **Known dimming cut at T:** paired LED-down / CNTR-up early ramp, bounded
  by the current scene's highlight headroom (as now), aiming to arrive at
  the new floor on the cut frame; any remainder completes after the cut
  at `rate_down`.
- **Within a scene** (no cut in the window): paired ramps at the slow
  rates, each command sent latency-early for the frame it serves.
- **May an early ramp before a known cut use CNTR?** Yes — early moves are
  always *paired*, so content is constant; CNTR is half of every pair.
  The one thing never done early is an unpaired move.

## 3. Deeper dimming — the slow recovery mostly disappears

The LED has no physical slew to speak of (dead time only, 80 ms); the
slow `rate_up` was a *perceptual* limit to avoid pumping. A cut masks
pumping. Therefore:

- **At a cut, jump.** LED and CNTR both go straight to the new scene's
  operating point in one command each, timed on the cut. No recovery
  ramp. This removes the "large contrast distance" problem too: the CNTR
  jump is masked by the cut, however large.
- **Within a scene, ramp** at `rate_up` / `rate_down`, paired, as now.
- **Unseen cut (reactive, `Low_Latency`):** jump immediately; the only
  residual is the dead time (~80 ms of knee compression), which is the
  accepted "brief soft dip".
- Safety: `max_jump_levels` (default 255, i.e. none) in case the camera
  measurement ever shows an LED slew; and `max_cntr_jump` likewise.

Of the four candidates: (a) early recovery — subsumed by the cut jump,
and still used for *dimming* cuts; (b) bound the depth by recoverability —
not needed once cuts jump; keep `led_floor` / `led_floor_black` as the
bounds; (c) split the CNTR jump — no: VP gain cannot mask a *shortfall* of
light, only reduce it, so a split is just the knee for longer; (d)
measure rate limits — not needed: the limits are perceptual, and the one
physical number (dead time) is already in the plan. No cinema session
required.

## Notes on the implementer's notes

- Order of work confirmed: 6b-2b → 6c → Stage 7 step 7 (sign fix, deltas
  3 + 4, this file) → delta 5 → Stage 10.
- Brief: agreed, merge only the real changes into the repo's current copy.
  Include the repo's brief in every package from now on so revisions start
  from it.
- Delta 4 understood as described: the change is `compensation_eotf =
  2.41` with the raw transfer and highlight calculation on the same final
  value.
