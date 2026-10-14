# Planner answers — review brief 2026-10-13 night (9f1d3552..d63186ba)

The brief: `C:\dev\handoff\planner-2026-10-13-review-brief.md`. Received
2026-10-13 night. Status per answer at the end.

Answers to Q1–Q12. The efficiency work changed the budget picture
materially (the sigma-delta search 35.9 → 4.2 ms, the de-judder matcher
110 → 25 ms, both byte-identical) and the failure map is the first
quantitative view of where the estimator stands. Nothing here holds the
deploy; two items are conditional on tonight's clips.

## Q1 — `dejudder_low_res: all` as default: yes; keep `search` as A/B

108% of the full-size benefit at half the cost settles it. Keep `search`
documented as the A/B option (it is the one that isolates whether a
future refinement needs full-size detail) and keep `off` as the
regression reference. Record the E4-gate bug (unwritten trust read) in
BUILD-STATUS "fixed on the way" so the 65% figure is not rediscovered.

## Q2 — cinema profile `target_frames: 3`: yes

Set it. One frame of latency is nothing for film and the audio delay is
set once anyway (the owner's constant-delay target already implies ≥3
at 24p with 200 ms). Ahead engaging "only when the queue happens to
allow it" would make the critical refresh time depend on queue state,
which is the kind of variability the timing gate exists to remove.
`Low_Latency` unchanged (1 frame; ahead cannot run there, correctly).

## Q3 — subtitle rectangles: hard edge, no feather

Subtitles have their own black outline and sit on a rectangle the
viewer reads as a separate layer; a feather would smear the warped
picture into the held box and look like a halo under the text. Keep
the hard edge. If the cut-and-paste boxes are ever generous (margin
beyond the glyphs), shrink the hold to the glyph bounds plus outline
rather than feathering.

## Q4 — per-slot budget: 14.6 ms is right as the gate; measure on the 3090 is a deploy-type step the owner can take now

Half a refresh is the correct engineering budget for a slot (the other
half is the presenter's margin and the LED shader is already inside the
measured slot). Keep 14.6 ms as the requirement. Do not tighten to 12 ms
on paper; tighten only if the 3090 measurement shows the presenter
needing more than the half. Measuring: copying the Beta test program to
the cinema PC is a read-only visit (no VP install, nothing in the Startup
folder) and is worth doing before the deploy so the 3090 numbers replace
the 3–4.5× estimate. Owner's go.

## Q5 — VP setting the port's line settings in listen mode: no objection, with (a) and (b) as stated

(a) VP sets the port only before its own starts of the script; a script
already running from an earlier session is left alone. (b) A third
program holding the port makes the setup fail, logged and shown, and the
script then fails as it would have anyway. Both are the right behaviours.
Add one line to the Config monitor: "port settings applied by VP /
skipped / failed", so the owner can see which case he is in. VP never
holding the port in listen mode remains the invariant.

## Q6 — r210 / R12L: wait

No current source delivers 12-bit RGB to the capture path; the GPU
unpack exists for V210/UYVY/HDYC where the saving is real. Build the RGB
formats when a low-latency RGB source appears; the fallback to the CPU
formatters covers them today. Note it in BUILD-STATUS.

## Q7 — R-A roll-off with estimate ahead: look ahead, with the rate limit

Yes: with the next pair's global motion one refresh early, start the
roll-off on the first frame of a fast pan rather than one frame late.
Keep R-A's existing ±0.25/frame rate limit so the look-ahead cannot
produce a step, and keep the cut hold. Fall back to the current pair
when ahead did not run (the schedule already reports that). Test: a pan
that accelerates past `roll_lo` — the roll-off begins one frame earlier
with ahead than without, both rate-limited.

## Q8 — match tolerance 4.5: A/B profile first, default after the clips

Make it the A/B profile (step 7) now. Promote it to the default only if
tonight's real clips confirm the gain *and* wrongly trusted blocks stay
near 0 on the fade scene with fix 5 off (the 3% at 4.5 came from the
fade; if fix 5 lands and the fade is handled, that objection goes away).
Either way the choice is one key.

## Q9 — regional hold: build it; the cut stays frame-wide

No conflict if the two signals are kept distinct, which they should be:
- **Cut** = frame-wide, as today: no trust anywhere *plus* the
  luma-histogram change. This is the signal the LED module (7c dump,
  black-frame handling) and the de-judder's cut hold consume.
- **Regional hold** = a trust decision inside a non-cut frame: trusted
  regions warp, untrusted hold. It never emits the cut signal.
A frame with trust below `minTrustedShare` but no histogram change is a
hard-motion frame, not a cut: regional hold applies, and the LED module
sees nothing. Add the test: a whole-frame low-trust pan without a
histogram change must not raise the cut flag.

## Q10 — allowance for the five fixes: ≤1 ms on the matcher, confirmed

Together at most 1 ms on the matcher on the 3090, inside the 14.6 ms
slot; fixes 1 and 2 are at 1/32 size and should be a fraction of that,
fix 3 is a policy change (no cost), fix 4 is a few extra scores on
blocks the period detector flags, fix 5 is one extra reduction. Measure
each on the laptop and scale; if the five together exceed 1 ms scaled,
the fence disambiguation (4) is the one to gate on content (only where a
period is detected).

## Q11 — fades: mean-removed matching behind a fade detector; limited warp

Mean-removed SAD changes behaviour on every brightness change, and on
flicker or an explosion holding is right. So: enable mean-removed
matching only when the frame-level luma shift is **smooth and global**
(the fade detector the LED module already has: monotonic APL change,
structure unchanged), and even then limit the warp with R-A's floor
(a fading scene is not where judder is noticed). Flicker and explosions
fail the "smooth and global" test and stay held. Test: a 3%/frame fade
warps at the floor strength; a one-frame flash holds.

## Q12 — hold back from the deploy: nothing

- **Estimate ahead:** in, on, with `target_frames: 3` in the cinema
  profile (Q2); its fallbacks are tested and reported.
- **Capture path:** in, default `standard`; `gpu` on the rule-selected
  low-latency profile only, as proposed.
- **Delta 14:** in, on by the owner's choice; off is D2 byte-identical.
All three are behind settings with tested off-states and the matrix at
80 combinations passing. The deploy waits only for tonight's gate, the
audit, and the owner's request.

## Also noted

- Frame 247 of Jockey trusting nothing at ~20 px is the right first
  diagnosis tomorrow; a 20 px move that holds is a matcher bug, not a
  limit, and it may be the same class as the E4-gate bug.
- The failure map's conclusion — a learned *controller* would add ~0.5%
  over the best fixed setting — is the useful kind of negative result;
  record it in the 11b note's §2 as the outcome of step 9. The learned
  *candidate finder* (step 10) stays open pending fixes 1–5.
- Licences: keep the UVG/Xiph clips local and unlisted in the repo, as
  you have; the corpus scripts may reference them by name.

---

## Status (2026-10-13 night)

| | Status |
|---|---|
| Q1 | Default unchanged (all); E4-gate bug recorded in BUILD-STATUS |
| Q2 | The source sample `[queue.Normal]` already has `target_frames: 3`. The owner's deployed config lives on the cinema PC: check it at deploy time, change only with his OK (user data) |
| Q3 | No change (hard edge already) |
| Q4 | Owner's decision: copy the Beta test program to the cinema PC for the 3090 numbers |
| Q5 | Built: `port=applied|skipped|failed` in led-script-now.txt, shown in the Config monitor; test |
| Q6 | Recorded in BUILD-STATUS |
| Q7 | To build with the matcher fixes (tomorrow), with the accelerating-pan test |
| Q8 | A/B profile after tonight's clips; default decision on the clip results |
| Q9-Q11 | Matcher fixes 1-5, tomorrow, default off, with the named tests |
| Q12 | Nothing held back |
| Step 9 | Recorded as ruled out in docs/VP-stage11b-learned-candidates.md §2 |
