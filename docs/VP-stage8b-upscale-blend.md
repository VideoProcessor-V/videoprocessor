# Stage 8b — adaptive upscale blend (1080p → 1600p); add to the cut

2026-10-12. Owner's decision: include before the deployment. Mostly reuse
of Stage 8: one blend hook, the existing grain map, the gate reversed.
Small, per source frame at 1080p.

## What it fixes

The CNN upscalers (FSRCNNX / ArtCNN, with SSimSuperRes on top) sharpen
whatever they are given, so grain and compression noise come out as
crisper noise, and on noisy frames the result can shimmer because the
network's output is not temporally consistent. The faithful kernel
(`ewa_lanczossharp`, anti-ringing) does neither. The blend keeps the CNN
where it earns its place (edges, texture) and leans to the kernel where
there is grain.

## The blend

Per pixel, luma only: `out = w·CNN + (1 − w)·faithful`, with

```
w = w_base × grain_scale
```

- **No edge term.** In upscaling the CNN is what is wanted on edges; only
  grain pulls the weight toward the kernel.
- `grain_scale` is Stage 8's grain term unchanged: the per-frame scalar
  as fallback, the per-pixel map from the matcher's trusted vectors
  (motion-compensated temporal difference, high-passed) where available,
  shadow-biased, snapped on cuts, same `grain_lo/hi/floor` ramp. The
  matcher runs for the grain map on film-rate sources (≤ 30 fps) exactly
  as for Stage 8.
- **SuperRes strength scaled by the same term**: `superres_strength ×
  grain_scale` per pixel, so it does not over-refine noise.
- Chroma stays with CfL (or the configured chroma hook); the blend does
  not touch it.
- Both branches in the same light domain (`upscale_light`, default
  linear; gamma available for comparison, as with Stage 8).

## Branches

- **Faithful:** libplacebo's `ewa_lanczossharp` upscale, computed by VP
  from PREKERNEL with libplacebo's sampler (the same native path as
  Stage 8's option A), anti-ringing on.
- **Perceptual:** the configured CNN hook output (FSRCNNX or ArtCNN),
  with SSimSuperRes applied after the blend's scaling of its strength.

## Anisotropy and gating

- Measures are taken on the source, as in Stage 8; the kernels are
  per-axis already. Under the anamorphic fit the vertical factor may
  exceed the horizontal; nothing to change.
- Gate: source height **<** output height (the mirror of Stage 8's
  gate), using the output-height template. Native and 4K sources
  unaffected. If one axis is upscaled and the other not, the stage still
  applies; the blend is harmless on an axis at 1:1.

## Config (rendering profile, native keys, as Stage 8)

```
adaptive_upscale: on
adaptive_upscale_w_base: 1.0
adaptive_upscale_superres_scale: on
upscale_light: linear
```
The grain keys are shared with Stage 8 (`adaptive_grain_*`); the
`grain_map` switch is shared too.

## Tests

- Synthetic grain field: the output approaches the faithful branch by
  `grain_floor`; a clean textured field equals the CNN branch; a cut
  snaps.
- Edge: with the blend on, an edge equals the CNN branch (no edge term).
- SuperRes: strength tracks `grain_scale`.
- Gate: no effect on native or 4K sources; applies at 1080p; both light
  domains give the same `w` map.
- Stage 10: alpha untouched, extended codes kept through both branches.
- Both warp paths and the matrix's "all refinements on" axis include it.

## As built (d1837c6d; planner answers 2026-10-12, night)

- **Faithful branch behind a CNN doubler.** FSRCNNX / ArtCNN hook LUMA, so
  PRE_KERNEL is already the doubled picture. When it is exactly 2x the
  source crop, VP averages it 2x2 back to source resolution in the same
  light domain (the doubler's own consistency, which SSimSuperRes relies
  on too), then ewa_lanczossharp (anti-ringing) to the output: the kernel
  from a consistent estimate of the source. No CNN: PRE_KERNEL itself.
  Other prescale ratios: skipped and logged.
- **upscale_light.** linear = libplacebo's linear light where it scales
  down (2x to 1.48x after a doubler), sigmoid off (sigmoid clips WTW on
  upscales); a plain upscale without a CNN is in gamma light. gamma = gamma
  light everywhere. Config card: "linear (downscale after a doubler)" /
  "gamma (all)".
- **SuperRes.** Bundled SSimSuperRes gains `vp_grain_strength` (default 0,
  unchanged); the blend runs just before it and hands the grain term over
  in alpha; SuperRes scales its correction by it and writes alpha 1. The
  doubler case is covered by the chain matrix (unmeasurable in the unit
  test: rounding-level correction on synthetic pictures).
- **Warp paths.** 8b runs in the main render; nothing path-specific.

## Cinema check (1080p source with grain, in the first session if time)

CNN alone → blend on, on a grainy sky over a sharp foreground: grain
calmer, edges unchanged, no shimmer. If not visibly better, leave it off.
