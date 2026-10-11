# Stage 6b-2a — planner answers

2026-10-11. On `be719dfc`.

## Refinement 1 — extras presented one at a time: approved, one check

Waiting for the frame statistics to confirm the previous picture is on
screen before committing the next extra is the safer design: a wrong
refresh count can never queue B behind an extra, and the worst case is
today's behaviour (A shown again). Accepted.

Requirement for the Code auditor: the render thread now holds
`renderMutex` for up to ~two refreshes per film frame while it waits.
Confirm that nothing B needs to *arrive* contends on that mutex — the
capture callback, conversion and queue enqueue must run without it, or
"B has arrived in the queue" can never be observed during the wait. Also
list what else blocks on `renderMutex` (config application, LED
controller, stats) and confirm none of it is time-critical at that
granularity.

## Refinement 2 — phase from the observed first vsync: approved

`L` as the minimum of (first vsync − callback) over 120 frames is a sound
estimator; it self-corrects within two seconds of any pipeline change.
Note in the log line when `L` changes by more than one refresh, so a
pipeline hiccup is visible.

## Question — 50p on 60 Hz: exclude

Require **at least 2.0 refreshes per source frame** (tolerance −0.05) in
addition to the non-integer rule. 50p is video, not film; its one-in-five
extra would be motion smoothing of exactly the kind the owner rejects, and
in his setup 50p is live TV. The rule then reads: non-integer cadence
*and* ratio ≥ 1.95. Report the ratio and the decision in the stats line
as before.

## Live test

The three vp.log lines are the right observables. For the first cinema
session: play a 24p pan with `dejudder_strength: 0.25`, then read the
`De-judder phase` line (expect cadence 2.500, φ0 cycling 0/0.2), count
hold reasons, and check the swapchain-full counter is zero. Then the eye.
