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

## P16 coding evidence (`CodingEvidence.h/.cpp`, U13)

**Design goal.** One measure of how the source picture was coded: block grid, banding, effective bit depth. Previously
SourceResolutionDetector held a private copy (MeasureCoding, GridBlockiness, BandEdgeBlock, EffectiveBitsOf); U13 counted 14
duplicates. The code was MOVED here, not copied; the detector keeps no coding law.

**Philosophy.** Modules adapt to the shared product (owner rule, D8). The detector reads `kCodingEvidence` through
`SkeletonMemo::Acquire` only. Without a memo (the Original pathway, no skeleton module) coding evidence is UNAVAILABLE and the
detector's readings stay neutral (bitrateQuality 1.0, effectiveBits 0, bandShare 0, blockiness 1.0); the monitor's Describe line
says "coding evidence unavailable (<reason>)". It is never measured privately (no silent fallback). The one fallback (rules 1/5,
rule 25 addendum): when NO renderer store exists at all (a standalone monitor in tests/tools, or a renderer torn down or re-created
while jobs are still queued) the monitor calls this unit's `Hub::MeasureWithoutMemo`, the same `MeasurePack` the owner fill runs,
counted (`Hub::MeasuredWithoutMemo`, `Snapshot::codingWithoutMemoFrames`) and shown in Describe: "coding evidence measured by its
owner without a memo (no renderer store; N frame(s))". Its result equals the pre-P16 per-tile measure (CodingEvidenceTests).

**Lifetime.** `Hub::Instance()` is a leaked heap object (never destroyed, so a module destructor in static destruction still
finds it). The memo provider is called under a shared lock; `SetMemoProvider` returns a token and `ClearMemoProvider(token)`
takes the lock exclusively, so once `~SkeletonModule` has cleared, no call into it is running, and it clears only its own
installation (a newer module's provider is kept).

**Decisions.**
- Form: CPU, per monitor frame seq, the default variant; bytes = Header + one record per tile (`Serialise` / `Deserialise`, magic PCE1).
  The per-8x8 GPU declaration of P16 (Products.h) is untouched; a GPU per-8x8 form later is a FORM/size variant of this same product
  for temporal detail (one unit, rule 20), not a second estimator.
- Fill input: the monitor stages its tile pack for the seq (`Hub::Stage`); the owner fill measures it. The hub is a module-level seam
  whose memo provider is set from this unit's own block in SkeletonModule.cpp (no Bill hook line). L-CORE's MemoFor replaces only the provider.
- Seq: the monitor's frame sequence. The fill declares keepFrames = the monitor's worker latency; a lagging monitor frame inside
  that window fills once at read (the memo's release is per owner, P16-LATCH-EVIDENCE); one older is refused (FrameReleased) and
  shown as unavailable.

**Provenance.** Algorithms: Hunt 2 M2 (effective bits, Hill number of order 4 over 16-code windows), M3 (grid finder, band-edge
blocks), specs 1b / 2d; code moved verbatim from SourceResolutionDetector.cpp at f61d18823. Tests: tests/CodingEvidenceTests.cpp.

## BusyEdge (L-B5, in progress): S5 busy share and edge map

Design goal: one computation of the debander guard's busy share (P11 term S5, variant axes: canonical lap_min / ext_min thresholds and
plane) and one per-pixel Sobel edge map (ProductId kEdgeMap, consumers AdaptiveDownscale and L-FOCUS, later P11 gmax), both owner-filled
in source_analysis and read by consumers through Acquire. Philosophy: the arithmetic is MOVED from VpDebander / AdaptiveDownscale, not
copied; a private pass is deleted in the same commit as its consumer's switch.
Status: BusyEdge.h (CPU reference and tolerance oracle) and BusyEdgeGlsl.h (shader mirrors, one tap form kSourceTapCodesPerUnit) exist
with BusyEdgeTests. The kEdgeMap owner fill (EdgeMapFill.h, on L-CORE's OwnerFillRequest::gpu) is built; its MODULE is
registered at start-up (HubBootstrap) and the FILL at the first observed Entrance size (reserve derived from the source luma size, rule 6) with EdgeMapFillTests (registration, reserve, CPU declines; the GPU
parity test, tests/EdgeMapStageGpuParityTests.cpp, is written for both variants and its first run is owed before the first reader row,
102b KEDGEMAP-RULE3). It has NO reader yet, so it never runs; no consumer is switched and
every private pass (debander guard, AdaptiveDownscale measure) is still present. Decisions: R14-66 (lead 14, 2026-11-14 20:54Z,
lead14-state.md:102) registers kEdgeMap under rule 3, first reader L-FOCUS; R14-64 (lead 14, 2026-11-14 ~21:10Z, lead14-state.md:89)
kept X5 private, superseded by the owner's ruling of 2026-11-14 ~17:00 EST (design-EDGEMAP-DOMAIN-AXIS rev 2, 102b X5-EDGEMAP-CJ3): X5
acquires a kEdgeMap stage variant in CJ-3.
Settings: none added (no new keys); thresholds are the debander's existing lap_min / ext_min.

## MeasureDomain (CB slice 18 S2a): the edge / grain measure domain

Design goal: the kEdgeMap stage variant (S2b) computes its luma in the same domain X5's measure pass uses, so the law lives with
the product's owner. MeasureDomain.h (namespace VpMeasure: GuardTransfer, GuardLinear, MeasureDomain, MeasureDomainFor,
LumaWeightsGlsl, MeasureLumaCode) and MeasureDomainGlsl.h (the ad_lin / ad_luma GLSL text) are a PURE MOVE out of
scaling/output/AdaptiveDownscale.h/.cpp: unchanged arithmetic and byte-identical shader text. Decisions: X5 keeps its names through
using-declarations and keeps GuardCode / GuardSlope (its residual guard); MeasureDomainFor reads the weights from core/LumaWeights.h
(LumaCoeff::ForSystem) directly, the table DetailPulse::LumaWeights forwarded to (identical values). Settings: none.

## kEdgeMap stage variant (CB slice 18 S2b)

Design goal (design-EDGEMAP-DOMAIN-AXIS rev 2 s2a): X5's measure pass and L-FOCUS read ONE edge map of the picture X5 measures,
not a private Sobel. The fill serves VariantKey{stage, width x height, codeUnit 8, param1 = picture ordinal} (EdgeMapStageKey) on
the P13 stage picture of the same key (SourcePlanes::OfferStage), as RG32F at the picture size: R = Sobel |G| in 8-bit measure
codes, G = the measure luma (MeasureDomainFor of the picture's own colour facts). Philosophy: one body and one law for both
variants (BusyEdgeGlsl.h: em_tap / em_store per variant, |G| = length(gx, gy), the expression order of X5's former Sobel, so
X5's grad = R x 0.25 exactly); the default variant (R32F, source luma, 10-bit codes) is unchanged in value.
Decisions: RG32F, not R32F, because X5's grain sums (Laplacian, energy) still read the luma at the base (slice 9 / X5-GRAIN-P10
absent), so G has a reader; WHOLE picture, no crop axis (the design's kAxisCrop is not declared): X5's 3 x 3 neighbourhood at its
src_rect border reads luma beyond the crop, which a crop-clamped map would change; code unit 8 only on the stage variant (the
default stays 10); the declared product size (the ledger term) and the fill's reserve are ONE figure, the largest variant
(EdgeMapFillReserveBytes == EdgeMapReserveBytes, 8 B/px of the source luma: an RG32F picture of the luma size fits; 265 MB at
8K, 66 MB at 4K; the default R32F uses half), and the ahead budget counts it once per fill one render frame holds (SkeletonMemo's
measured occupancy: one per stage ordinal held at once, a cadence repeat does not add; CB-18 SHOULD-2, r2 MUST-1); a larger stage picture declines visibly. Tests: tests/EdgeMapStageTests.cpp (CPU law and fill), tests/EdgeMapStageGpuParityTests.cpp (GPU parity, first run owed). Reader: X5 (S3). Settings: none.

S3 (the X5 reader): the stage dispatch is `DispatchEdgeMapStage(dispatch, picture, colour, system, primaries, dst, why)` in
EdgeMapDispatch.h (moved unchanged out of EdgeMapFill.h with DispatchEdgeMap, so the fill file takes no pl_dispatch parameter;
the fill hands in `static_cast<pl_dispatch>(req.gpu->Dispatch())`), one function shared by the owner's fill (FillEdgeMapStage delegates to it) and X5's visible no-memo fallback
(a host without a skeleton memo, e.g. the hook tests), so the fallback cannot drift from the product. X5 asks for the map under
StageAfter(scaling.geometry), the measured base's size and a per-seq picture ordinal (scaling/output/EdgeMapStageOrdinal.h).
The catalog's X5 row (row 3) does not yet declare the kEdgeMap read (left to the C5+A test run, see the CB-18 lane report).
