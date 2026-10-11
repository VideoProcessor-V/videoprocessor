# Stage 6b-2 — planner answers

2026-10-11. The survey of `RenderLocked` was the right thing to do first;
re-calling it per refresh would have been wrong for all the reasons listed.
Decisions follow the note's numbering.

## 1. Mechanism — approved

One block at the end of `RenderLocked`, after A's present and telemetry,
reusing A's `renderImage`, `target` and `frameParams`, running only when
the 6b-1 reason is Active and A rendered and presented. With these made
explicit:

- No NLS recovery on extras; a failed extra render stops the extras for
  that source frame and logs once (rate-limited thereafter).
- Extras keep their own counters and never touch A's presentation record,
  so drift correction and the pacing traces still see one present per
  source frame. The Code auditor diffs the present sequence with the flag
  off, as before, *and* checks that A's record is identical with it on.
- A's own present stays unwarped. Agreed, for the reason given (keeps the
  warp off today's main path); the ≤0.1-frame ideal offset is not worth
  it. Record as a possible later refinement, not a plan.
- Back-pressure: with max frame latency 2, the render thread may block on
  a Present while extras are queued. Acceptable because the "never delay
  B" rules bound it, but add a counter for "extra skipped because the
  swapchain was full" so a slow GPU shows up in the stats line rather than
  as silent judder.

## 2. LED measurement on the first present only — approved

Exactly one exact reading per source frame, from A's own present, pairs by
source identity by construction. The compensation hook stays on every
present (the gain is per source frame already). Note it in the LED module
guide §2 so nobody later "fixes" the missing measurement on extras.

## 3. libplacebo peak detection — accept and note

The owner's cinema profile runs a static tone map (LLDV, peak detection
off), so this does not arise for him. Note it in the brief under Stage 6:
"with `peak_detection` on, extras make the adaptation 2–2.5× faster in
wall time at 24p; not corrected." No freezing attempt.

## 4. Which cadences get extras — tighten the rule to uneven cadences only

The de-judder exists to remove the *unevenness* of 3:2. 25p on 50 Hz and
30p on 60 Hz are even 2:2 — there is no judder to remove, only
sample-and-hold, and an extra at δ = 0.5 there is plain motion
interpolation, which the owner does not want.

Rule: extras run only when the number of display refreshes per source
frame, `P_s / P_d`, is **not an integer** (tolerance ±0.05 to absorb
59.94/60 and 23.976/24). That selects 24p on 60 Hz (2.5) and would also
select, say, 25p on 60 Hz (2.4), while excluding every N:N cadence
regardless of rate. Replace the "source < display / 1.5" test with this;
keep the `Low_Latency` and cut exclusions as they are. Report the cadence
ratio and the decision in the stats line.

## 5. Config box and stats line — approved as proposed

Switch, Gentle 0.25 / Medium 0.35 / custom, cinema profile only; stats
line with Ctrl+I. Add the cadence ratio (item 4) and the swapchain-full
counter (item 1) to the line.

## 6. Logging — approved

First 200 extras, then 1 in 240, plus every hold reason rate-limited. Also
log once per source-rate change: the computed refresh count, φ0 and the
decision, so a cadence misdetection is visible without a cinema session.

## Frame trust (§2.3) — approved

GPU-only count of trusted blocks; whole-frame hold below 25%; cut test
asserts output equals A exactly. The threshold becomes a config key with
that default.

## Holds (§2.2) — approved, one addition

The subtitle cut-and-paste composition hold is right to report rather
than solve now; record it in the brief as a Stage 6 follow-on ("extras
with composed subtitles"), since scope content on the owner's screen will
use subtitle moving.

## Build order — approved

6b-2a (frame trust, extras block, phase; GPU and policy tests) then 6b-2b
(Config box, stats line). Each committed, packaged for design review, then
audited, with the present-sequence diff as the auditor's first check.
