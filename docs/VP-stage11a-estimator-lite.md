# Stage 11a — estimator-lite (added to the cut by the owner)

2026-10-13. Recorded from the planner's handoff.

Four cheap changes to the de-judder's matcher. Together they give most of a
"true-motion" estimator's benefit at about 1–2 ms per source frame at 4K.

- No new mechanism beyond candidate lists and a split rule.
- No change to synthesis (warp, hold, mask) or to strength.
- Each change sits behind a setting; off = today's search exactly.

Every consumer of the field benefits: the de-judder (fewer held blocks), the
occlusion mask, the dither carry (delta 8 R1) and the grain map.

## E1 — neighbour and temporal candidates (`dejudder_est_candidates: on`)

At the fine level, each block gains these candidates:

- **Its eight neighbours' vectors, from a first sweep.** The GPU searches all
  blocks in parallel, so "already-searched neighbours in raster order" does
  not apply. Instead the fine search runs twice:
  - the first sweep uses temporal and global candidates only;
  - the second adds each block's neighbour vectors from the first sweep's
    result (ping-pong the field).
- **Its own block's vector from the previous frame's field.**
- **The global vector (D1)**, already present.

Each candidate is scored by SAD plus a smoothness penalty:
`λ × |v − median(neighbour vectors)|`.

- λ is configurable, default about 0.5 code per source pixel of
  disagreement.
- An isolated best-SAD match on texture then loses to a neighbour-consistent
  one unless its SAD advantage is real.

Then the existing half-pel refinement.

Cost: a second fine sweep with about 8 extra SAD evaluations per block;
about 0.5 ms per source frame at 4K.

## E2 — adaptive block split (`dejudder_est_split: on`)

- **Which blocks split.** After the fine search, blocks whose best SAD
  exceeds `split_sad` (default = `match_tolerance`) split into four 8×8
  sub-blocks.
- **Sub-block search.** Each sub-block is searched with the parent's vector
  and E1's candidates as starting points (small range, ±2 px).
- **Vectors and trust.** Each sub-block gets its own vector and trust.
- **Split flag.** The field texture gains a "split" flag. The warp's vector
  interpolation and the consistency check then work at sub-block granularity
  where present.
- **Cost.** Typically 10–20% of blocks split; about 0.5–1 ms per source frame
  at 4K.

## E3 — quarter-pel refinement on trusted blocks (`dejudder_est_qpel: on`)

- Only for blocks (and sub-blocks) already trusted at the half-pel result:
  one quarter-pel refinement step (8 neighbours).
- Skipped on untrusted blocks.
- Cost: small.
- It improves the warp on slow pans, where a half-pixel error shows as a
  slight softening of the extras.

## E4 — motion-gated search (`dejudder_est_gate: on`)

- **Static scenes.** If D1's global vector and the previous frame's mean
  trusted |v| are both below `motion_scale_lo` (D2's lower ramp), run the
  coarse level only and reuse the previous field: nothing to track, nothing
  to fix.
- **Resuming.** The full search resumes as soon as either exceeds it, or on
  a cut.
- **Saving.** The whole fine pass is skipped on static scenes.

## Consistency with what exists

- D3 (median smoothing) runs after E1/E2 as today; with E1 it will have less
  to do.
- The consistency check (forward/backward) runs at sub-block granularity
  where E2 split; trust is per sub-block.
- The grain map reads the block's own SAD, which is unchanged; the split flag
  lets it use sub-block SADs where present.
- The dither's external-vector path (delta 8 R1) reads the field at sub-block
  granularity where present; otherwise as before.

## Tests

- **E1:** a textured pan with an injected best-SAD outlier (a repeated
  pattern that matches elsewhere). The neighbour-consistent vector wins with
  the penalty; the outlier wins without it.
- **E2:** a small object against a pan. Split blocks follow the object and
  the surround follows the pan; without E2 the block is held or wrong.
- **E3:** a sub-pixel pan (e.g. 1.25 px/frame). Warp error on the extras is
  below ¼ px with E3, about ½ px without.
- **E4:** a static scene leaves the fine pass undispatched (counter); it
  resumes on a 3 px pan and on a cut.
- **All:**
  - off = byte-identical to today;
  - both warp paths;
  - the matrix axis gains "estimator-lite on";
  - render time per source frame reported on the stats line with and
    without.

## Order

- After delta 8, before the final gate.
- Then gate → audit → deploy folder.
- The `s = 1` experiment on the projector (post-cut) then shows what the lite
  estimator still misses. That decides whether anything further is ever
  worth building.
