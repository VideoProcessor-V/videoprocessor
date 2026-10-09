# Delta 1 — answers to the implementer's questions

2026-10-09. Numbering matches `VP-led-drive-delta-1-questions.md`. "Agreed"
means implement your proposal as written.

## 1. What `led_latency_ms` models — agreed, pure delay

Treat it as dead time only: modelled level switches to the commanded value
`led_latency_ms` after the command (whole output frames), gain switches
with it. No one-pole lag on top. If the camera measurement later shows a
gradual settle, add a separate `led_settle_ms` key then. Delete the lag
wording from the guide's §4.2 when you update it.

## 2. Default — agreed

80 ms. Reject the old `led_response_ms` key with a message naming its new
place.

## 3. Cuts and fades under the ramp limits — agreed, one note

Your three cases are right. Note for the unseen dark→bright cut: during the
~0.5 s brighten the highlight policy already bounds the gain by headroom,
so content dims briefly rather than clipping — that is the "brief soft
dip" the brief accepts for the reactive case. Nothing extra to build.

## 4. Lookahead depth — agreed, with the real limit stated

Set `active_picture_lookahead_frames: 8` in `[queue.Normal]` only. Record
in BUILD-STATUS (or the brief's open questions) that until Stage 5 the
queue itself holds only `target_frames` (3) ahead, so the effective
lookahead is ~3 frames regardless of the 8-frame cap, and early ramps are
partial. That is acceptable: even a partial head start plus "hold steady
before a known cut" is the win. Full early ramps arrive with the deeper
Stage 5 queue.

## 5. Third renderer touch point — agreed, with upstream hygiene

Accept the third call site inside the existing walk. Two requirements:

- It is a **generic observer**: the walk calls a neutral "queued-frame
  statistics" interface that the LED module subscribes to. Nothing in the
  walk code, its names, comments or commit messages mentions the LED
  module, so that part of the walk remains PR-able.
- I will update the guide's portability rule and the audit README to
  "three call sites plus one hook-chain entry" (done in this handoff).

## 6. `script` mode and the mapped values — agreed; my delta was wrong

`script` mode keeps the exact PRE_OUTPUT measurement of the presented frame
and never uses raw-frame predictions. The tick-for-tick equivalence test
against `beta_22.py` is the "unchanged" proof. Strike the last bullet of
delta-1 item 5.

## 7. Approximations — agreed, two notes

- Using `pl_tone_map_sample` with the configured function and target is
  better than a re-implementation; do that.
- The transfer must include the configured `output_gamma` (2.4) because the
  exact measurement sees that picture — but the module must not *set* or
  change gamma anywhere; it only reads the profile's value (brief,
  Stage 7, "do not change `output_gamma` in code"). If "the module's
  `gamma_boost`" means anything other than reading that profile value,
  tell me.
- RGB capture coverage via `AnalysisLumaSource`: good; note which luma
  weights that path uses for RGB so the correction ratio starts near 1.

## 8. `lpw_max` — agreed

Reported `?M`, falling back to `led_ceiling`; startup log only.

## After these — your instructions

Implement delta-1 as amended here (items 1–8 above take precedence over
delta-1 where they differ), with the tests from delta-1 item 6 plus one for
the dead-time timing and one for the three-frame effective lookahead
clamp. Update `VideoProcessor-test.cfg` defaults (`led_latency_ms: 80` in
`[projector]`, `active_picture_lookahead_frames: 8` in `[queue.Normal]`,
the new `[led_drive]` ramp keys) and the guide's §4.2 wording. Build x64
Release, run the full suite, and commit as
"Stage 7 step 5: delta-1 amendments".

Then produce the review packages for all completed steps per
`docs/REVIEW-PACKAGE.md` — this amendment as its own step folder — into
one zip, `C:\dev\handoff\review-all-steps.zip`, with an `INDEX.md` at the
top listing each step, its commits and its diff line count in build order.
Do not change any source while packaging.

If anything in items 1–8 is impossible against the real code, stop and say
so rather than working around it; the owner is not a developer, so explain
in plain terms.
