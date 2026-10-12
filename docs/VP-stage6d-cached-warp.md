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

## Built (2026-10-12)

Built ahead of the measurement at the owner's request (2026-10-12: the de-judder
path is settled first so Stage 8's use of the vectors is built against it).
`vprenderer/RenderedWarp.{h,cpp}`; key `dejudder_warp_stage: rendered|source`
(input group, default `rendered`; Config "De-judder" card, "Extra frames
from").
- **Survey:** libplacebo's order is scale (SCALED hooks) → colour conversion
  and tone map → PRE_OUTPUT hooks → output encoding → OUTPUT hooks → target
  overlays. The PRE_OUTPUT picture is therefore in the target colour space,
  not yet encoded, with the LED compensation and the Stage 10 restore in it;
  subtitles and the stats overlay (target overlays) are drawn per present on
  top, unchanged.
- **Cache:** a native texture-input PRE_OUTPUT hook, pushed last in A's list
  (after the LED hooks), armed only when the frame may get extras (reason
  Active, not a cadence repeat). It copies the picture into an rgba16f
  texture and hands the texture back unchanged. Effect on A: libplacebo keeps
  the picture in its 16-bit float intermediate (as the LED measurement hook
  already makes it do): at most a code of rounding. On 7.360 the hook is a
  texture-input PRE_OUTPUT hook, so the encoding repair joins A's chain (the
  known PRE_OUTPUT log line); on 377 nothing changes.
- **Warp:** shares the source warp's GLSL vector rule
  (`Dejudder::FieldVectorGlsl`) and the occlusion mask's NLS map
  (`OcclusionMask::NlsMapGlsl`), so trust, hold and cut decisions are the
  same; the displacement is divided by the map's local slope per axis.
  Catmull-Rom (4×4 taps), edge clamp, no value clamp. With an NLS shader whose
  map is not known, the frame keeps the source path (logged).
- **Extra render:** the warped picture as an RGB frame in the cached colour
  space, 1:1 on the target's crop, with only `PL_HOOK_OUTPUT` hooks (the
  sigma-delta; the repair only if still needed) and deband, colour adjustment
  (saturation), peak detection, custom LUT, cone and distortion off. A's
  fallback on a failed extra is unchanged.
- **Tests:** `DejudderRenderedWarpMatchesTheSourceWarp` (7.360, 1:1 and 2×,
  full-range target): cache ≤ 1 code; hold and cut = A as shown exactly;
  both paths move the picture by (6, 3) / (12, 6); interior mean 0.10 / 0.15
  codes, 0.28% / 0.63% of samples over 2 codes (the source path decides
  chroma's hold at chroma positions; the kernels differ). Beta (377, limited
  8-bit target): cache ≤ 1, hold exact, extra vs source mean 0.17.
- **Not covered yet:** the NLS case on the GPU (the map is the occlusion
  mask's, tested there), the sharpness comparison and the render-time
  counters (WARP timings mean nothing; the 3050 Ti on this PC is the same
  generation as the cinema 3090 and will give an upper bound), the cinema
  check (slow pan over fine detail, `rendered` vs `source`).

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
