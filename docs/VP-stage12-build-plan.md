# Stage 12 + delta 15 — build plan (roadmap item 5)

2026-10-15. Plan for `docs/VP-stage12-effective-resolution.md` and
`docs/VP-delta-15-downscale-refinements.md`. Nothing built yet. Order as
the spec: detection and the manual classes first (usable on day one),
then the responses behind `auto`; `native` is byte-identical to today.

## Step A — settings and the manual re-base (no detection)

- `[rendering profile]` `source_resolution: auto | native | 1440 | 1080 | 720`
  (default `native`), `source_resolution_auto_min_confidence`,
  `low_bitrate_handling`, `cnn_residual_stabilise`, `deband_vp`,
  `ssim_residual_stabilise`, `ssim_bitrate_aware`, `ssim_bitrate_floor`.
  Config controls for each (constraint 12; scaling card), parsed in
  RendererProfileConfig like the Stage 8 settings.
- Re-base point: after the source planes are uploaded/unpacked
  (LibplaceboVideoRenderer, the P010/P210 / native RGB / GPU RGB unpack
  block) and after the source warp replaces them, i.e. on the `pl_frame`
  that goes to `pl_render_image` (normal and extra paths alike). A
  `SourceRebase` module replaces each plane with a texture downscaled to
  the class height (width by the same factor, rounded to the chroma
  grid): luma 2160 -> 1080, 4:2:0 chroma 1080 -> 540, RGB 4:4:4 as one
  plane. `image.crop` and every source-pixel rectangle handed to the
  hooks (NLS mapping, the grain field's `sourceCrop`, the active-picture
  rectangle) are scaled by the same factor.
- Kernel (owner agreed 2026-10-15): `ewa_lanczossharp` with anti-ringing, per plane in the signal
  domain (not linear light). Deviation from the spec's "linear
  light": a per-plane re-base keeps the source a real YCbCr 4:2:0 frame,
  so CfL, the chroma upscaler and every hook see exactly what a native
  1080p source gives them; and the upstream upscaler worked in the signal
  domain, so undoing it there is the closer inverse. A linear-light
  re-base would mean converting to RGB first, which the native path
  never does.
- Everything downstream then sees a 1080p source: the 8b gate (source
  height < output height) fires by itself; Stage 8's downscale does not
  (S8-3 needs no special case); the edge term is computed on the re-based
  picture (S8-4 by construction).
- The matcher keeps running on the native frame (its input is the CPU or
  GPU payload, before the re-base). Consumers in source pixels (the 8-3
  grain map, the dither vectors, Stage 10 carry) take the vector field
  scaled by the re-base factor; the vectors-valid state is invalidated
  when the class changes (a discontinuity, as at a resync).
- Stats line: `source_resolution=native|1080 (manual)` and which
  response acted (`rebase+8b`, `stage8`, `native`).
- Tests: `native` byte-identical; a 1080p chart upscaled bicubically to
  4K and re-based to 1080 through the 8b path matches the native 1080p
  chart through the same path within half-float; the crop and grain-map
  mapping scale; RGB and YCbCr inputs; both capture_upload paths.

## Step B — detection 1a/1b, readings only

- Effective resolution, per source frame on the native luma: for each
  class h in {1440, 1080, 720}, the residual R_h = Y - up(down(Y, h))
  (separable passes at a few tiles, not the whole frame), its energy
  reduced on the GPU outside grain-map blocks (or a flat-region mask
  without vectors) and read back a few frames late like Stage 8's stats.
  An upscale from height h leaves R_h near the noise floor; the effective
  height is the smallest h whose residual is below the floor relative to
  R_1440's band energy. Smoothing ~2 s, snap on a cut (Stage 8's mean
  luma cut detector), hysteresis between classes. Derived from the
  frame's own height: the classes are fractions of the native height,
  not fixed 2160 (constraint: general, not fixed).
- `bitrate_quality` from block-boundary gradient energy at 8/16 px
  periods and the flat-block single-step share; `effective_bit_depth`
  from the code histogram's multiples of 4. Same reduction pass.
- Readings on the stats line and the Config monitor only; nothing acts
  on them yet. The owner watches them for a few evenings.
- Tests as the spec §4 (upscaled 1080/720 charts, true 4K, grainy 1080,
  cut snap, borderline hysteresis, blocked frame, 8-bit-in-10 ramp).

## Step C — the responses behind auto

- `source_resolution: auto` re-bases from the reading when its confidence
  passes the minimum.
- 2b/2d VP's own debander (libplacebo's algorithm reimplemented; PQ-unit
  thresholds on HDR; strength from `effective_bit_depth` and
  `bitrate_quality`; grain-map exclusion; at source resolution in the
  re-base pass; vector-held regions).
- 2c CNN residual stabilisation and S8-1 SSIM residual stabilisation
  (one shared motion-compensated one-pole on the residual, trusted blocks
  only; falls back without vectors, never starts the matcher).
- 2c / S8-2 bitrate-aware anti-ringing and SSIM weight.
- Tests per the specs, both warp paths where vectors are read.

## Costs (to measure, laptop then 3090)

- Re-base: one downscale per plane per source frame at 4K input (the
  faithful branch Stage 8 already runs is the reference), then the 1080p
  upscale path, which is cheaper than the 4K path it replaces.
- Detection: three down/up residuals on tiles plus one reduction per
  source frame; target well under 1 ms on the 3090.
