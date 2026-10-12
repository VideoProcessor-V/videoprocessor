# Stage 8 — survey and proposed integration

2026-10-12. Implementer → planner, on `docs/VP-stage8-downscale-design.md`
(constraint 7: survey before designing). Nothing is built yet.

## 1. What exists

- **SSIM shader** (`shaders/SSimDownscaler.glsl`, igv, LGPL-3.0): four
  POSTKERNEL passes. It binds PREKERNEL (the unscaled picture) and HOOKED
  (libplacebo's own downscale) and **corrects libplacebo's downscale**:
  the final pass is `avg[1] + avg[2]·L − avg[0]` with `L` = POSTKERNEL. It
  is designed to follow a Mitchell-Netravali downscale, which is why the
  cinema config sets `downscaler: mitchell` (`docs/cinema/VideoProcessor-test.cfg`).
  The final pass keeps `L.a`, so the Stage 10 carry survives it (checked in
  the source; Stage 8's carry test will prove it on the GPU).
- **Light domain:** libplacebo scales in linear light by default; VP has no
  setting for it today (`disable_linear_scaling` is never set).
  `downscale_light: gamma` maps to `disable_linear_scaling` for frames that
  downscale (upscaling keeps its sigmoid, which needs linear light).
- **De-judder field** (`DejudderWarp::Field()`): one texel per 16×16 source
  block: vector, trust, and the **motion-compensated match error** (mean
  absolute luma difference, limited-equivalent 8-bit codes, after the
  forward match). For trusted blocks that *is* the temporal difference the
  grain map wants; only the high-pass (removing a block's mean change, so a
  fade or a lighting change does not count as grain) is missing — one more
  output of the existing error pass.
- **When the matcher runs** (`DejudderPolicy`): with `dejudder_strength > 0`
  or `occlusion_mask: on`, and only for uneven cadence with a next frame
  (24p on a 60 Hz output, ≤30 fps sources for mask-only). On a 24 Hz output
  (refresh switching) or 1:1 cadence it does not run: the grain map falls
  back to the scalar there, as the spec allows.
- **Native hooks** (`OcclusionMaskHook`, `ExtendedRange`): VP already writes
  its own libplacebo passes with persistent textures, renderer-set values
  per frame and failure logging. A user shader cannot receive an app
  texture except through alpha, and **alpha between RGB and SCALED belongs
  to the Stage 10 carry**, so the grain map cannot travel in alpha at
  POSTKERNEL.

## 2. The conflict to decide: what "faithful" is

The spec's faithful branch is `ewa_lanczossharp` (libplacebo's downscale);
its perceptual branch is the SSIM shader. But the SSIM shader is built *on*
libplacebo's downscale, and is meant to follow Mitchell. One libplacebo
downscale cannot be both. Options:

- **A (recommended): libplacebo keeps Mitchell for SSIM; VP computes the
  faithful branch itself** from PREKERNEL in a native hook with libplacebo's
  own sampler (`pl_shader_sample_polar` with `pl_filter_ewa_lanczossharp`,
  anti-ringing on). The SSIM shader stays unmodified, exactly as it is today.
  Cost: one extra polar downscale per frame at 1600p (≈1 ms on the
  estimate's scale).
- B: faithful = libplacebo's Mitchell output (the HOOKED texture SSIM
  corrects). No extra pass, but the faithful branch is softer than the
  spec's and has no anti-ringing choice.
- C: libplacebo downscales with `ewa_lanczossharp` and SSIM corrects that.
  Not what igv's shader is designed for (it would over-sharpen and ring).

## 3. Proposed structure (with A)

Three native hooks, all off unless `adaptive_downscale: on`, all gated on
source height > output height, the SSIM shader untouched between them:

1. **PREKERNEL — measure:** from the unscaled luma, the edge map (Sobel,
   soft ramp `edge_lo`..`edge_hi`, at source resolution, read by the blend
   at the output position) and the per-frame grain energy (Laplacian energy
   where the gradient is below `edge_lo`, shadow-weighted) reduced on the
   GPU into a persistent 1×1 texture with the one-pole smoothing and the cut
   snap applied there — no read-back. Also renders the faithful branch
   (`pl_shader_sample_polar`, ewa_lanczossharp, anti-ring) into its own
   texture at the output size.
2. **POSTKERNEL, after the SSIM shader — blend:** `w = w_base × (1 − edge)
   × grain_scale`; luma only: `out = faithful + w·(Y_ssim − Y_faithful)`
   added equally to R, G, B (chroma stays faithful: `chroma_ssim: 0`);
   alpha passed through (the carry). Step 2's grain map is a block-grid
   texture from the de-judder's field (trusted blocks; others take the
   scalar), bilinear between block centres, smoothed per block over ≈0.5 s
   in a persistent texture, reset on a cut.
3. The renderer sets per frame: the cut flag (the de-judder's frame trust,
   or the LED module's cut signal when the matcher is not running), the
   field texture when present, and the stats-line values (`grain_scale`,
   mean `w`, read back a few frames late like the LED measurement).

Placing (1) at PREKERNEL and (2) after the user shaders at POSTKERNEL uses
the hook-order rule already in place (native hooks of a stage run in list
order with the user hooks).

## 4. Config (settings first, constraint 12)

As native code, Stage 8 cannot use a `[shader.…]` rule's `param_*` keys.
Proposed rendering-profile keys instead (Config page "Processing" card,
next to the downscaler): `adaptive_downscale: on|off` (default off),
`downscale_light: linear|gamma`, `adaptive_w_base`, `adaptive_edge_lo`,
`adaptive_edge_hi`, `adaptive_grain_lo`, `adaptive_grain_hi`,
`adaptive_grain_floor`, `adaptive_grain_smooth_ms`, `adaptive_grain_map`,
`adaptive_grain_map_smooth_ms`, `adaptive_grain_shadow_bias`. The spec's
defaults unchanged. The SSIM rule stays as it is (the blend needs it
enabled; without it the blend logs once and does nothing).

## 5. Order

1. Light-domain switch (`downscale_light`) alone — small, useful for the
   owner's first A/B (SSIM gamma vs linear).
2. Hooks 1–2 with the scalar grain term; tests from the spec §5 (edge,
   grain field, carry, gate, anisotropy, light domains).
3. The vector grain map (the field's high-passed error), with the
   synthetic texture/noise test.

## 6. Questions

1. Faithful branch: A (VP's own ewa_lanczossharp from PREKERNEL, SSIM
   unchanged on Mitchell), B or C?
2. Config home: rendering-profile keys as in §4 (recommended), or a shader
   rule?
3. On a 24 Hz output (no de-judder) the grain map is scalar only. Accept, or
   run the matcher for the grain map alone when `adaptive_grain_map: 1`
   (its cost, ≈1–2 ms per source frame at 4K)?
