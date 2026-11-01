# Shared region evidence (`input.source_analysis/regions`)

Sub-module of [input.source_analysis](../README.md) | unit **input** | order 40 (before every movable module) | status: **draft, not wired** (no call sites; skeleton product declarations ready for the integrator)

## Purpose

Owner rules (2026-10-31): every repair or enhancement is dynamic and regionally adaptive (per region and frame, local
evidence, bounded by the grain guard, smoothed); the grain map is computed ONCE, EARLY, on the decoded source and shared;
whatever several adaptive processes need is computed once. Baseline (memory): store only the shared heavy PRECURSORS as
skeleton products tiered by the VRAM governor; each consumer derives its thin maps on the fly inside its own pass.

## Two layers

**Layer 1, stored precursors (skeleton products, T0):**

| Product | Contents | Bytes / buffer 4K | 8K | Ref ms (3050 Ti, 1080p) |
|---|---|---|---|---|
| `grain_split` | repair.grain_split, REUSED law, computed on the luma clamped into the active picture (`ExtendPicture`, mirrored about the edge): base, f D1, f D2, f (clean picture + grain layer formed on read), and one block texel per region block (E1, E2, E3, N: band energies over the block's picture pixels and the grain level, `BandTexelBlock`) written by the split pass itself, which already forms D1..D3 and reduces sub-block energies | 45.7 MB (5.5 B/px + 16 B/block) | 182.6 MB | 1.2 (estimate) |
| `region_block_stats` | per block (B = 32 at 4K, 64 at 8K), read from the RESIDENT decoded luma plane (no extra full-resolution texture): mean luma, local min/max (block + stencil ring), max gradient, mean structure tensor Jxx/Jyy/Jxy (magnitude + orientation), Laplacian energy, band energies E1/E2/E3 and N (copied from the split's block texel), off-edge Laplacian energy (ringing precursor), plane-fit slopes, mean nits, mean PQ value, picture coverage, off-edge share, post-tone-mapping visibility; plus the aux texel (overlay, camera confidence, camera label code) | 0.78 MB (8160 blocks x 96 B) | 0.78 MB | 0.35 (estimate) |

The governor counts each product's ring + live pool (graph sizes; test log line `REGION_VRAM`). At the skeleton's default
depths (4 ring + 3 live buffers) the precursor set holds 305 MiB at 4K (split 174.5 + 130.9 MiB, stats 4.5 MiB) and
1.22 GiB at 8K (split 696.6 + 522.4 MiB, stats 4.5 MiB); under pressure the governor steps the split down to its live
pool alone (131 / 522 MiB, lossless: recomputed in the render). The full-resolution s1/s2 planes (4 B/px) dominate: if
8K budgets are tight, storing f and base only and re-forming f D1 / f D2 in the readers is the next saving to measure.

**Layer 2, derived on the fly (not stored):** `DeriveBlock<T>` (CPU) / `region_evidence(ivec2 blk)` (GLSL, fused into
the consumer's pass): grain level, edge strength, local edge contrast, ringing vs the grain floor, banding-prone
(flat/gradient), texture energy (texture-deficit reference), brightness, visibility, flags (in picture, overlay, no grain
split, tiny mover), camera confidence and label. About 30 ALU, sqrt only, 6 texel fetches from one block (cache-resident).

Stored derived maps: **none**. A stored set would cost 0.38 MB and one 0.02 ms pass; deriving costs ~30 ALU per evaluation:
per block in a compute pass (8160 evaluations) it is free; per pixel in a 4K fragment pass ~8.3 M x 30 = 0.25 GOP, about
0.01 ms on the 3090 and 0.05 ms on the laptop per consumer, with the same texture traffic as reading a stored map. The only
transcendental-heavy term (visibility: three EOTF evaluations and an exp) was moved INTO the precursor pass, so no derived
term meets "several consumers AND costly".

## Precursor ranking (compute cost x consumers)

1. Grain split + band decomposition (three Gaussians, Wiener share; ~1.2 ms laptop 1080p) x 7 readers: temporal_detail,
   texture_restore (same estimator), detail pulse (D1/D2 band energies), debander (low band), LED grain weighting, adaptive
   downscale grain, the block stats. Highest; built (reused).
2. Block statistics (one full-resolution read, ~0.35 ms) x 6: the derived maps, detail pulse tile energies (2 x 2 blocks
   at its 32 px tiles), texture_restore tile energies, temporal_detail E1..E3, adaptive downscale grain scalar, camera
   model `PairInput::texture`/`textGuard`. Built.
3. Per-pixel gradient magnitude/orientation plane (Sobel; ~8 B/px, 66 MB at 4K) x 2 (adaptive downscale edge ramp,
   debander structure tensor): per-pixel users only, both VP-module shaders that already compute it in-pass at no extra
   read. Not built: memory exceeds the saving. The block tensor covers block-level users.
4. Per-pixel linear / PQ luminance plane x 1-2 (LED measures the OUTPUT, not the source). Not built; block means are in
   the stats.

## Inputs reused (never recomputed)

- Grain: `GrainSplit::Split` of the picture-extended (mirrored) luma on the same grid (`ComputeFrame` does extend + split + stats on
  the CPU). Missing or of another size/grid: the evidence is reduced and says so (`StatusOf` names every reduction,
  flag `kNoGrain`); a mismatched split is never read.
- Active picture: `FrameAnalysis::Identity::picture` (Bill's crop decision copied read-only by VpFrameAnalysisHost). Every
  pixel read is clamped into it, and the split runs on the picture-extended (mirrored) luma, so a bar never reads as edge, ringing or
  texture and never lowers the grain level. Blocks less than half covered by picture carry no evidence: on an off-grid
  rect up to B/2 - 1 picture rows or columns go without regional evidence; a consumer can read the coverage stat.
- Text / overlay: Bill's `SubtitleBoxResult` rectangles (bounds, line panels) copied by the host as plain `Rect`s (as
  DynamicDriveFrame already receives them), an optional text-guard grid (`MaskGrid`, e.g. the source-resolution tile
  flags once exported), and camera-model `Overlay` labels (static logos).
- Motion: the camera model record (`CameraMotion`: confidence, labels, tiny movers) from FrameAnalysisStore `kCamera`.
  Records with `labelsOnGpu` give CPU labels "unknown" (visible); a GPU consumer binds the label pass texture instead.

## Accessor

`RegionEvidence::View(precursors, DeriveParams)`: `At(bx, by)`, `AtPixel(x, y)`, `Stat(bx, by, index)` (layer 1),
`Status()`. Out of grid or no precursors = a neutral invalid `Sample` (consumer acts as without evidence, and shows the
status). `Smooth()` / `region_smooth()`: the one-pole temporal smoothing a consumer applies to a strength, reset at a
discontinuity. Shader: `RegionEvidenceGlsl()` (descriptors `re_s0..re_s4`, `re_aux`; uniforms `re_grid`, `re_block`,
`re_d0..re_d2` from `Uniforms(DeriveParams)`).

## Producer

`StatsPassGlsl()` (+ `TransferGlsl()` header): one thread per block, the arithmetic order of `StatsBlock<float>`;
deterministic (fixed raster order). Designed for the analysis lane; the in-render fallback is the same shader
(`Variants::Identical`). Production form (follow-up): a workgroup per block with a fixed-order shared-memory tree; the
reference form is occupancy-bound at 8160 threads (estimate ~1 ms at 4K on the 3090 versus ~0.2 ms for the tree form;
PassTimers replace both). `AppendProducts(decls, dims)` appends both declarations with the next dense ids.

## Consumers that can switch (survey 2026-10-31)

Identical (VP-module internals, when wired): LED grain weighting (reads a per-block grain map already), LED and detail
pulse bar exclusion (the trusted rect instead of a luma floor), camera model `textGuard`/`texture` inputs (no producer
today). Same estimator, switch with the grain split: texture_restore, temporal_detail grain level N. Behaviour change
(list only): debander spatial grain share (per-frame Laplacian share), detail pulse tile energies (rebased domain),
adaptive downscale per-pixel edge ramp and shadow-weighted grain scalar, source-resolution TextBand (spectral 128/256 px
tiles). Not applicable: LED peak grid (screen-derived). Nothing was rewired in this package (none is wired to a split
today, so no switch is byte-identical yet).

## Files

- `RegionEvidence.h`: the shared grain product (`GrainProduct`, `ComputeGrain`, `StampGrain`, `GrainView`, `GrainConsumers`:
  camera-model grain map, TemporalDetail, chroma-from-luma grain guard, grain-safe contrast recovery all read this one
  product; the graph keeps one node), geometry, transfer functions, `StatsBlock<T>`, `PackAux`, `DeriveBlock<T>`, `Compute` (CPU
  reference), `View`, `Smooth`, `Stamp`, `CostAt`, `AppendProducts`.
- `RegionEvidenceGlsl.h`: stats pass, consumer accessor, float mirrors (`StatsGridMirror`, `PackSource`).
- Tests: [tests/README.md](tests/README.md).
