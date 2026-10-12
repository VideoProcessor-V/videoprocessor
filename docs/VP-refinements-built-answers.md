# Refinements built — planner answers (2026-10-12, late)

On c8d37325..c0829d07. All five items answered; nothing blocks the gate.
Timing note: the figures were taken on a laptop RTX 3050 Ti, roughly a
third to a quarter of the 3090's throughput; scale the absolute
milliseconds accordingly, the ratios hold.

## 1. S2 — keep the output-pass float32 unconditional; add `fbo_float32` as a key, default on

The measurement settles it: the highlight floor is set at SCALED (float16
above white = 0.86 code), so only every-FBO float32 reaches the 1/16-step
target. Decision:

- `output_fbo_float32` stays **unconditional**: it is now a correctness
  fix (item 2), not only a precision one.
- `fbo_float32` becomes a rendering-profile key, **default on** in the
  sample and the FL35 config. +2.8 ms on the 3050 Ti is ≈ +0.7–1 ms on
  the 3090 on a ~10 ms render — inside the budget. If the stats line on
  the 3090 ever shows pressure, the owner turns it off and keeps the
  output-pass precision; that is what the key is for. `Low_Latency` may
  default it off (games do not need the last 1/16 of a step), the
  cinema profile on.
- Record the three-way measurement (1.02 / 0.84 / 0 step) in
  BUILD-STATUS.

## 2. Chain bias named — accepted; keep the A/B as a regression

The float16 round trip at the PRE_OUTPUT texture hooks explains every
observation (both libplacebos, both GPUs, Stage 10 on ≈ 0 because its
restore is float32). No separate flat-field test needed. Do add the A/B
itself as a Beta regression: with patch 0003's flag forced off, the bias
returns (0.25–0.6 code low) and the test expects it; with the flag on,
≈ 0. That pins the cause so a future libplacebo rebuild cannot lose it
silently. 7.360 keeps its 0.8 limit, unpatched, as agreed.

## 3. S1 also scales the per-pixel gate — approved

Correct: with the per-pixel `motion_threshold` at one code, grain would
reset the carry regardless of the block match, so scaling both with the
same `mix(1, scale, g)` is what makes S1 do anything. Payload stays
±0.5 code; byte-identical at scale 1 or without a grain map. Record in
the refinements note that S1 scales both gates.

## 4. D5 on the rendered path — leave source-only

Do not build B-early caching for an experiment switch that is off by
default. D5 stays source-warp only. Behaviour when the owner enables D5
with `dejudder_warp_stage: rendered`: log once, ignore D5, and say so on
the Config card ("occlusion fill: source warp path only"). If the
projector ever shows D5 is worth keeping, the B-early restructure is a
post-cut item.

## 5. D3 and a small object against a pan — accepted

A block-sized object moving against a pan is an outlier by definition;
with the promotion check it is moved with the pan only when its own match
at the median passes tolerance, which limits the cases. At `s` 0.25 the
effect is the object's motion smoothed by a quarter — the same class of
artefact every block-based system has, and the one the owner will judge
on the first pan. Note it in BUILD-STATUS under "known behaviours."

## D1 — noted with approval

Keeping the global vector itself as a half-resolution start, not only
the refined candidate, is the right fix for the 1/8 refinement's drift on
fine texture: 210/210 blocks on a 140 px pan versus 0/210 without D1 is
the clearest single result of the refinement work.

## Next

Gate → audit → deploy folder. Include in the deploy folder's
BUILD-STATUS: the 3050 Ti timings with the note that the 3090 figures are
to be read from the stats line on the first cinema session.
