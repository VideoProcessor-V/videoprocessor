# Stage 6d — de-judder extras: warp the rendered picture, not the source

2026-10-12. Optimisation for the 4K target. Build only if the stats line
says it is needed (see "When").

## Why

Each de-judder extra is today a full re-render of the whole chain (tone
map, gamut map, chroma, scale, SSIM, LUT, compensation, carry, dither) of
a warped *source* frame — ≈10 ms per extra at 4K. The extras differ from
A by nothing but a geometric warp, so the chain's work is repeated for no
change. At 24p on 60 Hz (one or two extras per source frame) that is
25–35 ms of GPU per 41.7 ms with everything on: feasible, thin margin.

## What

- Cache A's image at **PRE_OUTPUT** — after everything content-dependent
  (tone map, scale, SSIM, LUT, LED compensation, BTB restore) — once per
  source frame.
- For each extra: **warp the cached image** in output space by `s·δ·v`
  (edge clamp, hard hold where untrusted) with a **4-tap kernel**
  (Catmull-Rom or Lanczos-2), not bilinear: this warp lands on the final
  picture rather than on a 4K source whose blur the downscale would
  absorb, so bilinear would soften the extras visibly against the
  unwarped A (shimmer on fine texture). The warp must not clamp values —
  the carry's extended range is present at PRE_OUTPUT. Then run only the
  OUTPUT-stage hooks on it: occlusion-mask injection, encoding repair,
  sigma-delta. ≈1 ms warp at 1600p + ≈1.5 ms OUTPUT passes per extra.
- Budget at 24p with everything on: ≈10 (A) + 2 × 2.5 (extras) ≈ 15–16 ms
  per 41.7 ms, ≈40% of the card.

## Details

- **Vectors into output space:** scale the half-res source vectors by the
  source→output ratio per axis, then through the NLS per-column mapping
  (already built for the occlusion mask), so a block's displacement is
  correct after the stretch. Chroma is already 4:4:4 at PRE_OUTPUT, so no
  per-plane halving.
- **Hold decisions** (untrusted blocks, frame trust < 25%, cuts) are
  identical to today; a whole-frame hold is simply A's cached image
  re-presented.
- **Occlusion mask** is already injected at OUTPUT per extra; unchanged.
- **LED gain** is per source frame and already in the cached image;
  unchanged. The exact LED measurement runs on A only, as before.
- **Sigma-delta** runs per present on the warped image, as before.
- **Edge cases:** subtitle composition and any OUTPUT-stage overlay
  happen after the cache point; unchanged. If a future hook is added
  between PRE_OUTPUT and OUTPUT that depends on the source rather than
  the picture, it must declare itself and force the full re-render path.
- `dejudder_warp_stage: source | rendered` (default `rendered` once
  built; `source` keeps today's path for A/B and as the fallback).

## When

Measure first. The stats line already reports render time per present.
Play a 4K film with CfL, SSIM + blend, the grain map, the motion-carry
dither and the de-judder all on; if the per-source-frame total exceeds
about 60% of the frame period (≈25 ms at 24p), build 6d. Below that,
defer.

## Tests

- A warped-cached extra matches a source-warped extra within 1 code on a
  synthetic pan (same vectors); differences confined to the hold edges.
- Sharpness: on a fine-texture pan, the difference between A and its
  extras (high-frequency energy) is no larger by the cached path with the
  4-tap kernel than by the source path. Cinema check: a slow pan over
  fine detail, `rendered` vs `source`, looking for shimmer.
- NLS mapping: under the owner's scope profile, a vertical bar panned
  horizontally lands in the same place by both paths.
- Hold paths identical by both methods; cut ⇒ A exactly.
- Render-time counters: extras in `rendered` mode under 3 ms each at 4K.
