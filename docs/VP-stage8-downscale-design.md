# Stage 8 — adaptive downscale: design note

2026-10-12. For the implementer, after the Stage 10 carry is in. Scope:
4K sources to the 2560×1600 raster (≈1.5× horizontally; vertical factor
set by the anamorphic/scope fit — anisotropic). 1080p sources are
upscaled and are not this stage's concern.

## 0. What this stage is and is not

At 1.5× the difference between a good kernel and SSIM is modest, and the
difference between SSIM and anything cleverer is smaller still. SSIM's
two real weaknesses on film are (a) it amplifies grain and compression
noise, because it preserves local variance whatever its origin, and (b)
it can halo on hard high-contrast edges. This stage fixes those two
things and nothing else. It is a **blend between two existing outputs**,
not a new scaler, and if the A/B ever shows the blend is not earning its
place the owner switches it off and keeps plain SSIM.

Where the motion vectors come in — and where they do not. They are not
needed to stabilise the edge term: it is computed from the *source* and
edges are stable frame to frame. They are needed for the **grain term**:
spatially, grain and fine texture are indistinguishable (both are
high-frequency energy), but temporally they differ completely — texture
follows the motion field, grain does not. A motion-compensated difference
between consecutive frames is therefore a reliable **per-pixel** grain
map, consistent from frame to frame because the vectors make it so. This
replaces the per-frame scalar wherever trusted vectors exist (the
de-judder's bidirectional field, cinema profile) and falls back to the
scalar where they do not (untrusted blocks, `Low_Latency`). The scalar is
still built first; the vector map is the second step.

## 1. The two branches

- **Faithful:** libplacebo's own downscale, `ewa_lanczossharp`, linear
  light, anti-ringing on. This is what the pipeline does today with the
  SSIM shader off.
- **Perceptual:** igv's `SSimDownscaler.glsl` as installed in Stage 2
  (POSTKERNEL hook binding PREKERNEL). Run in the **same light domain** as
  the faithful branch so the two are comparable pixel for pixel. Note: the
  owner's madVR ran SSIM in gamma light; libplacebo scales in linear.
  Expose the choice (`downscale_light: linear | gamma`), default linear;
  the first cinema A/B compares the two, because the owner is used to the
  gamma look.

Both branches must keep the Stage 10 carry: alpha untouched (the BTB
offset rides in alpha from RGB to SCALED and both hooks sit inside that
range), RGB kept in range by the carry's shift, as the Stage 10
investigation already verified for SSim.

## 2. The blend

Per pixel, `out = w·SSIM + (1 − w)·faithful`, with

```
w = w_base × (1 − edge) × grain_scale
```

- `w_base` (config, default 1.0): how much SSIM when nothing argues
  against it.
- `edge` ∈ [0, 1]: a smooth function of local gradient magnitude on the
  *source* luma at the block scale the downscale uses (Sobel on a 3×3 of
  source pixels per output pixel is enough), with a soft ramp between
  `edge_lo` and `edge_hi` (config, defaults ≈ 8 and 40 codes). Hard,
  high-contrast edges → faithful; texture → SSIM.
- `grain_scale` ∈ [0, 1], in two steps:
  - **Step 1 — per-frame scalar (built first, and the fallback):**
    mean high-frequency energy in *flat* regions of the source (Laplacian
    energy where the local gradient is below `edge_lo`), mapped through a
    soft ramp between `grain_lo` and `grain_hi` so clean sources give 1
    and heavily grained ones approach `grain_floor` (default 0.4);
    one-pole smoothed over ≈1 s; snapped across a detected cut.
  - **Step 2 — per-pixel map from the vectors (cinema profile; needs the
    de-judder's matcher running, i.e. `dejudder_strength > 0` or
    `occlusion_mask: on`):** for each source block with a *trusted*
    bidirectional vector, take the
    motion-compensated absolute difference between this frame and the
    previous one, high-pass it (so slow lighting changes do not count),
    and map its local mean through the same `grain_lo`/`grain_hi` ramp to
    a per-block `grain_scale`; bilinearly interpolate between block
    centres (as the de-judder does for its vectors) so there are no block
    seams; one-pole smooth per block over ≈0.5 s; untrusted blocks and
    frames without a vector field use the step-1 scalar. The result: the
    SSIM weight drops exactly where grain is and stays up on static
    texture, with no flicker because the map is motion-consistent. Reads
    the de-judder's vector field through the same app-provided texture
    path the occlusion mask uses (M1′), so no new facility.

Soft ramps everywhere; no thresholds a scene can sit on. The blend runs
in the same hook pass as SSIM; the extra work is the gradient and energy
reductions on the source and, for step 2, one block-resolution
motion-compensated difference per source frame (a few hundred microseconds
at 4K), plus a persistent copy of the previous source luma.

## 3. Anisotropy

The source → output ratio differs per axis under the anamorphic fit (and
the vertical ratio may be ≈1). Consequences:
- The SSIM shader's radii and the faithful kernel are already separable
  and per axis; nothing to change.
- The edge and grain measures are taken on the **source**, before
  scaling, so they are isotropic by construction and do not care about
  the output ratio.
- Gate the whole stage on "source height > output height" (output-height
  template, per delta 5), so it never runs on native or upscaled
  content; if only one axis is downscaled, the stage still applies — the
  blend is harmless on an axis at 1:1.

## 4. Config

```
[shader.standard.adaptive_downscale]
label: Adaptive downscale
glsl_file: Adaptive downscale.glsl       # blend hook; requires SSimDownscaler
param_w_base: 1.0
param_edge_lo: 8
param_edge_hi: 40
param_grain_lo: 2.0
param_grain_hi: 8.0
param_grain_floor: 0.4
param_grain_smooth_ms: 1000
param_grain_map: 1                       # 1 = per-pixel map from vectors where trusted, 0 = scalar only
param_grain_map_smooth_ms: 500
param_chroma_ssim: 0
param_grain_shadow_bias: 0.5
downscale_light: linear                  # linear | gamma (rendering profile)
```

Disabled by default; SSIM alone remains the baseline entry. The stats
line shows `grain_scale` and the mean `w` per frame so the owner can see
what the blend is doing on a given disc.

## 5. Tests (software renderer)

- Synthetic hard edge: with the blend on, output equals the faithful
  branch within 1 code across the edge; far from it, equals SSIM.
- Synthetic grain field (white noise on flat grey): `grain_scale` falls
  to `grain_floor` within the smoothing time; a clean field stays at 1;
  a cut snaps it.
- Carry: a BTB/WTW probe frame through both branches and the blend keeps
  its alpha offset and codes exactly as the Stage 10 fixtures require.
- Gate: no effect on native or upscaled content; applies on 4K.
- Anisotropic: a scope frame under the owner's anamorphic fit renders
  without seams or axis-dependent artefacts in the blend weight.
- Both light domains produce the same `w` map (weights come from the
  source, not the scaled result).
- Vector grain map: a synthetic scene with static fine texture on one
  side and temporal noise on the other, panned under a known vector
  field — the map must be near 1 on the texture and near `grain_floor`
  on the noise, stable across frames, and fall back to the scalar on
  blocks marked untrusted.

## 6. On the projector (one short A/B, 4K film with visible grain)

1. SSIM alone, gamma light (closest to the owner's madVR look).
2. SSIM alone, linear light.
3. Blend on, linear light, defaults, scalar grain only (`grain_map: 0`).
4. Blend on with the vector grain map (`grain_map: 1`) — the test for
   this one is mixed content: grainy sky over a sharp foreground, or a
   textured wall beside a flat one.
Look at: a grainy sky or wall (does the grain calm down without the
texture going soft), a high-contrast edge such as text or a horizon
(halo gone?), and fine detail elsewhere (unchanged?). Tune `grain_floor`
if the grain is still too present, `edge_lo/hi` if halos remain. If 3 and 4 are
not visibly better than the owner's preferred 1 or 2, leave the blend
off; if 4 is not better than 3, leave the grain map off.

## 6b. Refinements worth including (small, each one line of config)

- **Luma only.** Apply the SSIM branch and the blend to luma; downscale
  chroma with the faithful kernel. SSIM on chroma buys nothing visible at
  1.5× and can fringe colour at edges. `param_chroma_ssim: 0` (default).
- **Weight the grain map toward the shadows.** The LED compensation lifts
  the darks, so grain there is the grain that shows. Multiply the grain
  energy by a soft inverse-luminance weight before the ramp
  (`param_grain_shadow_bias`, default 0.5; 0 = off) so a grainy shadow
  pulls the SSIM weight down harder than the same grain in a highlight.
- **Snap everything on cuts**, not only the scalar: the per-block map's
  smoothing resets on a detected cut, so the first frame of a new scene
  is not blended with the last scene's classification.
- **No anti-ringing on the SSIM branch.** igv's shader has none; the edge
  term is the anti-ringing. Adding libplacebo's would soften the branch
  the blend is trying to preserve.

Not worth including: separate per-axis weights (the measures are taken on
the source and are isotropic; the kernels are already per-axis); any
temporal averaging of the picture itself (that is noise reduction, which
changes the look — out of scope).

## 7. Is this the best practical implementation?

For 1.5× on film, yes, in the sense that matters: it removes SSIM's two
visible faults with two interpretable controls and adds no new artefact
class. The alternatives are not better here:
- **DPID** is sharper than SSIM but overshoots more on grain — the same
  weakness, worse. Worth a try only if SSIM itself disappoints; it would
  replace the perceptual branch, not add to it.
- **Learned downscalers** target up-scale recoverability, not viewing,
  and none ships as a hook.
- A **spatial-only per-pixel grain mask** would flicker, which is why the
  per-pixel map here is derived from the motion-compensated temporal
  difference instead — the vectors are what make it stable. This is the
  one place the project's motion-estimation work pays into the scaler.
On the source side there is no limit to work around: the input is 12-bit
4:2:2 LLDV captured at 10-bit, so the downscaler has real information;
the DVI link constrains only the output, which is the quantizer's job.
