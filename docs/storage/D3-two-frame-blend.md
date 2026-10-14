# In storage: D3, the two-frame blend (removed 2026-10-14)

The owner's decision on 2026-10-14 was to strip it out and keep it in storage in case a use turns up. The de-judder
uses frame A alone, with M6 per-pixel trust and the landing check.

## Where it is

- **Branch `storage/d3-two-frame-blend`** (local): the last commit with D3 in the main code, abc102b1. It holds the
  `blendFrames` setting (test-only, never a config key), B's field from the consistency pass with the searches swapped,
  B's pixel weights, the `BLEND` branches in the source warp's luma and chroma shaders, the corpus set `blend`, and
  the test `DejudderBlendFillsUncoveredContentFromB`.
- **Branch `storage/motion-experiments-2026-10-14`** (local): the separate repo's test-only variants from the same
  day. It holds the D3 gates (sample agreement, motion agreement, fill only, B's own blocks), B alone, the landing
  check on B, the match-certainty test, the damage and field dumps (`VP_TEST_DAMAGE_DIR`, `VP_TEST_DAMAGE_FRAMES`),
  and the 30p midpoint mode (`VP_TEST_30P`).

## What it does

With M6, the source warp blends A moved forward with B moved backward, weighted (1 - s) by A's pixel weight and s
by B's. The idea is that content A hides (uncovered areas) is shown by B instead of being dragged.

## What it measured

Real HFR clips. The landing check is on in the last three rows.

| Variant | All removed | All damaged | Film-like removed | Film-like damaged | Extreme damaged |
|---|---|---|---|---|---|
| A alone (M6) | 41.9% | 0.97% | 31.7% | 0.39% | 1.99% |
| D3 blend | 50.2% | 2.29% | 40.8% | 0.78% | 4.92% |
| A alone + landing check (the default) | 41.3% | 0.30% | 31.3% | 0.13% | 0.61% |
| D3 + landing check on A and B | 49.8% | 1.74% | 40.3% | 0.59% | 3.75% |
| B alone + landing check | 38.1% | 7.84% | 25.3% | 6.15% | 10.79% |

The RAFT ceiling for a two-frame blend is 55.8% (film-like 46.5%).

## Why it was shelved

D3's extra removal always came with several times the damage of A alone. Four gates did not change that: sample
agreement, motion agreement, fill only, and B's own blocks only. The worst-frame crops show rectangles of B's
picture in the wrong place. Compared with RAFT, B's trusted motion is mostly right (about 1 px); the damage comes
from backward-warp occlusion and from blurred or smooth areas where every check passes.

## Cost

Measured on the laptop GPU, source path: about 2.4x the warp, plus B's field and pixel-weight passes. The planner's
rendered-warp version would also need B rendered one refresh early.

## To bring it back

Use `git log storage/d3-two-frame-blend` and `git diff <current> storage/d3-two-frame-blend -- src/VideoProcessor-Lib/vprenderer/DejudderWarp.*`.
Re-apply the D3 parts onto the current code, and keep the landing check on both A and B. A better motion estimate
for B is the prerequisite. Planner D3 (docs/VP-review-2026-10-14-overnight-answers.md) also requires
`dejudder_blend: off` by default and a rendered-warp-only version.
