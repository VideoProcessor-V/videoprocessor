# Stage 8 — build plan (adaptive downscale)

2026-10-12. Implementer's plan on `docs/VP-stage8-downscale-design.md`, the
survey `docs/VP-stage8-survey-proposal.md` and the planner's answers
(`docs/VP-planner-2026-10-12-answers.md` §3: faithful branch A, native
rendering-profile keys, matcher for the grain map on film-rate sources).

## Shape

A native module `vprenderer/AdaptiveDownscale.{h,cpp}` with two hooks, off
unless `adaptive_downscale: on` and only on frames that downscale (source
crop taller or wider than the destination rectangle). The SSIM shader
(`SSimDownscaler.glsl`, igv) is unchanged and stays on Mitchell.

1. **Measure hook, `PL_HOOK_PREKERNEL`** (texture input; hands the texture
   back unchanged — the 6d lesson):
   - **Faithful branch:** `pl_shader_sample_polar` with
     `pl_filter_ewa_lanczossharp`, anti-ringing on, from the PREKERNEL
     texture over `src_rect` to the `dst_rect` size, into its own rgba16f
     texture. Same light domain as libplacebo's own scaling (it is the same
     PREKERNEL picture).
   - **Edge map** at source resolution: Sobel on luma, soft ramp
     `edge_lo`..`edge_hi` (8-bit codes), r16f.
   - **Grain energy (step 1):** Laplacian energy where the gradient is
     below `edge_lo`, shadow-weighted (`grain_shadow_bias`), reduced on the
     GPU (16×16 tile partial sums, then one 1×1 pass) into a persistent 1×1
     texture that also applies the ramp `grain_lo`..`grain_hi` →
     `grain_floor`..1, the one-pole smoothing (`grain_smooth_ms`, the frame's
     time step) and the cut snap. No read-back on the render path.
2. **Blend hook, `PL_HOOK_POSTKERNEL`, pushed after the user hooks** so it
   runs after SSIM's four passes:
   - `w = w_base × (1 − edge) × grain_scale`, with the edge map sampled at
     the source position of the output pixel and `grain_scale` from the map
     (step 2) or the scalar.
   - Luma only (`chroma_ssim: 0`): `out = faithful + w·(Y_ssim − Y_faithful)`
     on R, G, B; `chroma_ssim: 1` blends RGB fully.
   - **Alpha (the Stage 10 carry):** blended like luma,
     `a = a_f + w·(a_ssim − a_f)`, so the offset follows the picture it
     belongs to. Test with the BTB/WTW probe.
3. **Renderer:** per frame sets the cut flag (the de-judder's frame trust
   when the matcher runs, else the LED module's cut signal), the time step,
   the field texture when present; the smoothing state advances once per
   source frame (A's render), never per de-judder extra (source path) — 6d's
   rendered extras already reuse A's blended picture through the cache.
   Needs the SSIM rule active; without it, logs once and does nothing.
4. **Stats line:** `grain_scale` and the mean `w` (1×1 results read back a
   few frames late, as the LED measurement does).

## Grain map (step 2, planner §3c)

- **Matcher for the grain map alone:** `DejudderPolicy` gains
  `grainMap` (input): with `adaptive_grain_map: 1` on a film-rate source
  (≤ 30 fps) the estimate runs even with strength 0 and the mask off, like
  mask-only mode; not for 50/60p, not in Low_Latency (unchanged gate order).
- **High-passed error:** the matcher's error pass writes a second block
  texture: the mean-removed motion-compensated absolute difference per
  trusted block (removing the block's mean change, so a fade or lighting
  change is not grain). No second difference pass.
- **Map:** per block, the same ramp → `grain_scale`; one-pole smoothed per
  block over `grain_map_smooth_ms` in a persistent texture, reset on a cut;
  bilinear between block centres in the blend; untrusted blocks and frames
  without a field take the scalar. Mapped to output positions with the same
  source map as the occlusion mask / 6d (crop, NLS).

## Light domain

`downscale_light: linear | gamma` (rendering profile, default linear): gamma
sets `disable_linear_scaling` on frames that downscale only (upscaling keeps
its sigmoid). Both branches follow, since both read PREKERNEL.

## Config (settings first)

Rendering profile keys, Rendering page card "Adaptive downscale":
`adaptive_downscale` (off), `downscale_light` (linear), `adaptive_w_base`
(1.0), `adaptive_edge_lo` / `adaptive_edge_hi` (8 / 40),
`adaptive_grain_lo` / `adaptive_grain_hi` (2 / 8), `adaptive_grain_floor`
(0.4), `adaptive_grain_smooth_ms` (1000), `adaptive_grain_map` (1),
`adaptive_grain_map_smooth_ms` (500), `adaptive_grain_shadow_bias` (0.5),
`adaptive_chroma_ssim` (0). All validated, fingerprinted, live-applied.

## Tests (WARP, 7.360 and the 377 Beta)

From the design §5, plus what this integration adds:
- hard edge → faithful within 1 code across it, SSIM far from it;
- white-noise field → `grain_scale` falls to the floor within the smoothing
  time, a clean field stays at 1, a cut snaps it;
- BTB/WTW probe through both branches and the blend: codes 4…1019 kept;
- gate: nothing on 1:1 or upscaled frames; applies on a downscale; one axis
  only works;
- anisotropic scope fit: no seams in `w`;
- both light domains give the same `w` map;
- vector map: static texture beside temporal noise, panned under a known
  field → near 1 on texture, near the floor on noise, stable, scalar on
  untrusted blocks;
- fallbacks: Low_Latency and 50/60p → scalar only (no matcher);
- smoothing advances once per source frame with de-judder extras (both
  `dejudder_warp_stage` values);
- SSIM rule absent → no blend, logged once.

## Commits and audits

1. **8-1** `downscale_light` alone (small; useful for the owner's first A/B:
   SSIM gamma vs linear).
2. **8-2** the module with the scalar grain term, Config card, stats.
3. **8-3** the vector grain map (matcher for the grain map, high-passed
   error, per-block map).

8-1 and 8-2 share one audit; 8-3 its own (it touches the de-judder's matcher
and policy).

## Projector check (design §6)

SSIM gamma, SSIM linear, blend with scalar, blend with the map, on a 4K film
with visible grain: grainy sky or wall, a high-contrast edge, fine detail.
Leave the blend off if 3/4 are not visibly better than 1/2.
