# GrainSplit (`repair.grain_split`, DRAFT)

Unit **repair** | draft, no slot, no call sites | CPU model + GPU passes | promotes texture_restore's grain estimator to a shared product | hook: prepass (before every reader)

## Purpose

One grain-free picture per presented source frame, computed once and shared by every feature that must not touch grain:

    clean = base + f D,   G = Y - clean   (Y = clean + G exactly, by construction)

A reader works on `clean` and adds its own frame's `G` back at the end, exactly: `Y_out = F(clean) + G`. Structure the
split mistakes for grain is added back unchanged (no gain there, no harm). Grain the split mistakes for structure (dense
texture, where the Wiener share f is near 1) stays in `clean`; a reader that MOVES structure between frames must budget for
that residual itself (temporal_detail does: template averaging over its slots plus a grain budget, and an output-vs-input
grain guard on this same map).

Readers: `repair.temporal_detail` and `scaling.chroma_upscale` (CfL: the grain-free luma guide and the chroma planes'
own splits for its chroma-grain guard). Intended next: mosquito repair, scaling (grain-free kernels, grain re-added at
the output size). Readers run it on their active picture (the bar boundary is the plane edge).

## Estimator (reused, not new)

texture_restore's (`repair.texture_restore`, TextureRestore.cpp): the detail band `D = Y - G(2.5 s) Y` (fine `D1 = Y - G(1 s) Y`,
mid `D2 = D - D1`), a per-block grain level `N` (mean of the 4 quietest of 16 sub-block energies x 2.2, minimum over 5 x 5
blocks) and the per-pixel Wiener share `f = clamp(1 - 2 N / mean3x3(D^2), 0, 1)`. Other VP grain splits were considered:
the debander's grain split (repair.deband, `GrainPreserve::kSplitSigma`) is a pure frequency split (everything above a
sigma-3 low band is "grain"), so fine structure would be added back untransferred; the Stage 8 grain map is a per-block
flag/level, not a per-pixel split. The Wiener split is the one that separates fine structure from grain per pixel.

## Product and cost

Owned as a skeleton product by [input.source_analysis/regions](../../input/source_analysis/regions/README.md) (coordinator
2026-10-31): `RegionEvidence::AppendProducts` declares `grain_split`, `ComputeGrain` makes it once per decoded source frame
(early, on the source, picture extended by mirroring so bars never enter it), `GrainView` is the one read API (clean, grain,
Wiener share, block level). Declared readers: camera-model grain map, temporal_detail, chroma-from-luma grain guard,
grain-safe contrast recovery, the region block stats.

`GrainSplit::kProduct` (`grain.split`): input the picture-extended source luma (lag 0), GpuCompute, history 1. Outputs: base (half-resolution
R16F), s1/s2 = f D1 / f D2 (RG16F), f (R8), N (block grid); `clean` and `G` are formed on read (`base + s1 + s2`, `Y - clean`).
Cost (estimate, not dispatched): three separable Gaussians (sigma 1, 2.5, 6 x Scale; the 2.5/6 pair at half resolution), one
3 x 3 energy, a 16-sub-block reduction and a 5 x 5 block minimum per frame; ~7 B/px (~58 MB at 4K), well under 1 ms on the
RTX 3090. Computed once per frame; readers never recompute it.

## Files

- `GrainSplit.h`: header-only CPU reference model (`Plane`, `Gauss`, `Params` with texture_restore's values, `Compute`,
  `kProduct`). Deterministic (raster loops, double sums, no threads), C++14-compatible, no Windows headers.
- `GrainSplitGpu.h/.cpp`: the same split as five libplacebo compute passes on any plane component (rows, columns, grain
  level, neighbourhood minimum, Wiener share) -> `split` (s1, s2, f, base; rgba32f) and `blocks` (N, e3). Matches the model
  within 5e-5 codes on WARP (`GrainSplitGpuMatchesModel`). Two rgba32f planes: 253 MiB at 4K (full-resolution bands, kept
  for exactness against the model; the half-resolution base of the plan would halve it).
- `GpuPassUtil.h`: the compute-pass helpers its readers share.
- Tests: see `tests/README.md`.

## Settings

None. The split's law is fixed (texture_restore's constants); readers choose what they do with it.

## Storage layouts and the read accessor (SA2)

The GPU product has two layouts behind an either/or switch (`GrainSplit::Storage`, `Options.storage` of `Splitter::Compute`;
default **Full**, byte-identical to the path before SA2). The settings key `grain_split_storage` is declared by the
integrator's node/settings, not here.

| Layout | `Frame::split` | `Frame::splitB` | `Frame::blocks` | bytes/px | bytes/block |
|---|---|---|---|---|---|
| Full | rgba32f `(s1, s2, f, base)` codes | none | rgba32f `(N, e3, 0, 0)` | 16 | 16 |
| Compact | texture A, rg16f `(s1, s2)` codes | texture B, rg16 unorm `(f, base x codeUnit)` | rg32f `(N, e3)` | 8 | 8 |

N and e3 stay fp32 (they exceed the fp16 range); f and the base (the plane value, 0..1) are unorm16; s1, s2 are half floats in
codes (11 significant bits). `Fits` needs `splitB` for a Compact frame only. `Compact(const Frame*)` tells the layout.

**Intermediates.** rows, bands and quiet are temporaries. With `Options.arena` set they come from the shared scratch arena
(scope `kArenaScope` = 0x0201, owner `grain_split`, tier T0, acquired in the fixed order rows, bands, quiet, scope closed by an
RAII guard). In Compact all three are arena temporaries; in Full the final pass writes the split into the rows texture, which is
the product, so rows stays private (or `Targets.split`) and only bands and quiet come from the arena.
`Splitter::ArenaRequests(gpu, w, h, B, storage)` lists the requests (Full: bands x1 + quiet x1; Compact: rows+bands x2 + quiet x1).
A null handle makes `Compute` return false with `LastError()` starting `scratch memory short (grain split)` and
`ScratchShort()` true (reset on every Compute); no private temporaries are made then and the arena's `scratch_arena` fallback is
engaged (visible), recovering on the first frame after a larger plan is reserved. `Options.targets` (split, splitB, blocks) lets a
skeleton node supply the output textures; they are never destroyed by the Splitter.

**Read path.** `GrainSplit::Gpu::ViewGlsl(storage, codeUnit)` + `BindView(frame, descriptors)` are the only read path. The GLSL
prelude defines `vec4 gs_fetch(ivec2 p)` (s1, s2, f, base in codes) and `vec2 gs_blocks(ivec2 b)` (N, e3). Full samplers: `split`,
`gblocks`; Compact: `gs_a`, `gs_b`, `gblocks` (Compact: `f = B.x`, `base = B.y / codeUnit`). Readers switch to it in the next job.

**Tests** (`tests/GrainSplitCompactGpuTests.cpp`, WARP, not yet registered in a project): `GrainSplitCompactMatchesFull`,
`GrainSplitFullIsBitIdenticalToToday`, `GrainSplitCompactIsDeterministic`, `GrainSplitArenaShortageIsVisible`,
`GrainSplitCompactWritesIntoTargets`, `GrainSplitViewGlslAndBind`. They log every number: max |clean| and |G| error against
Full (assert <= 0.5 code), std(G) and lag-1 autocorrelation of G per region class (5 %), block exactness, and the accessor's
Compact-vs-Full deviation. Numbers from the first integration run belong here (not measured when this was written: only compiled).
Derived bounds: s1, s2 <= 2^-11 relative (0.03 codes at 64, asserted <= 0.05); base <= half a unorm16 step of the plane value
(0.0078 codes at a 64/65535 code unit, asserted <= 0.05); f <= 1/65535 (asserted <= 1/255); N and e3 exact.

**VRAM per slot (split + blocks).** Block edge B = round(16 x Scale(planeLines)), Scale = lines / 2160 above 2160 lines, else 1
(TemporalDetail `kBlockPx`): 16 px at 1080p and 4K, 32 px at 8K; blocks = ceil(w/B) x ceil(h/B).

| Plane | B | Blocks | Full | Compact | Saved |
|---|---|---|---|---|---|
| 1920 x 1080 | 16 | 120 x 68 | 33,308,160 B (31.8 MiB) | 16,654,080 B (15.9 MiB) | 50 % |
| 3840 x 2160 | 16 | 240 x 135 | 133,228,800 B (127.1 MiB) | 66,614,400 B (63.5 MiB) | 50 % |
| 7680 x 4320 | 32 | 240 x 135 | 531,360,000 B (506.7 MiB) | 265,680,000 B (253.4 MiB) | 50 % |

Scratch (arena, shared across scopes, not per slot): Full 16 B/px (bands) + 16 B/block (quiet); Compact 32 B/px (rows, bands) + 16 B/block.

## One dependency unit, version and stale detection (SA2 audit)

The grain split is ONE unit: the CPU reference (`GrainSplit::Compute`, GrainSplit.h), its GLSL (GrainSplitGpu.cpp) and the storage layouts, versioned by `GrainSplit::kUnitVersion` (bump on ANY change of the law, a pass or a layout, and update the pin in GrainUnitGpuTests). A produced `Gpu::Frame` carries `seq` (the source frame, `Options::seq`), `version` and `storage`; the GLSL embeds the version as `#define GS_UNIT_VERSION` (also in the `ViewGlsl` prelude). `Splitter::Compute` resets `Out()` first (a failed Compute never leaves the previous frame looking valid). Readers call `Gpu::CheckFit(frame, w, h, B, scale, codeUnit, component, expectedSeq)`: another frame or another unit version is `Stale`, another geometry/unit/component `WrongSize`, nothing `Missing`; each falls back visibly to the module's own split (FallbackCatalog "grain split (<consumer>)") and recovers on the first fitting frame. A governor-dropped product is `Shed` (`Params::grainShed`).

The lane producer and the live-fill path are the same implementation: `GrainNodeGpu.h` `SplitterFill::Fill` calls `Splitter::Compute` into the slot textures of the node's `ProductTextures` (`GrainLayout`, `TargetsFor`, `FrameOf`); `GrainNode::LanePrecompute` and the first consumer's `Acquire` both go through it (test GrainUnitGpuTests::GrainUnitLaneAndLiveFillShareOneCompute: both results equal the plain Splitter result).

One code path, one instance per thread (audit 2): the lane thread and the render thread each own a SplitterFill + Splitter + pl_dispatch (a Splitter has mutable state and private intermediates; a pl_dispatch is not thread-safe), and each uses only its own thread's ScratchArena (the render arena and a lane arena are separate governor-counted pools). A SplitterFill binds to the thread of its first Fill; a Fill from another thread or with another thread's arena is refused before any GPU work, counted and shown (FallbackCatalog "grain split fill"), and the owning thread recovers at once (GrainUnitFillRefusesAnotherThreadAndAnotherThreadsArena; GrainUnitLaneAndRenderInstancesAreBitIdentical shows the two instances write identical bits). The node's layout comes from `ChooseNodeLayout`: the storage ChooseStorage grants for the plane (a refused Compact is laid out as Full) and a block grid of ceil(w / B) x ceil(h / B) at the node's real block edge (the declaration counts the grid at 16 px, the upper bound; `ProductTexLayout::MatchesDecl` accepts an absolute grid up to it). Targets laid out for a storage the plane no longer grants are refused visibly ("laid out again") rather than failing inside Compute (GrainUnitBlockGridFollowsTheBlockEdge, GrainUnitDowngradedCompactIsLaidOutAsFull).

Full-mode identity is pinned, not assumed: `Gpu::FullShaderFingerprint()` (FNV-1a 64 of the five Full pass bodies) equals the value of the strings at 160987e5, and the temporal-detail shader text with `gs_fetch/gs_blocks` mapped back to `texelFetch(split/gblocks, ...)` equals its pre-SA2 text byte for byte (size + hash, GrainNodeTests). The chroma upscaler's shaders changed structurally (three per-plane accessors), so its Full-mode identity is asserted on output (bit-identical to its own split), not on text.

### Compact storage rules

Compact is refused (Full instead, status + FallbackCatalog "grain split storage") when 1 / codeUnit > 4096 (more than 12 bits of codes: the fp16 s1/s2 would lose precision) and when the plane format is not UNORM/UINT (a float plane can hold blacker-than-black and whiter-than-white that the unorm base would clamp). `Gpu::ChooseStorage`, tests at 12 bit (compact), 16 bit (full) and on a float plane with values < 0 and > 1. Peak bytes incl. the arena scratch: compact 8 (ring + live) + 32 B/px against full 16 (ring + live) + 16: compact saves only when ring + live > 2 (`GrainSplit::StorageLedgerLines`).

### Private grain estimators outside the unit (not reachable in SA2)

These still estimate grain privately and are redirected to the unit in their own packages: TextureRestore N + Wiener (SA8), the debander (SA5), AdaptiveDownscale Laplacian (SA4), the matcher GrainError (SA3), VpFrameAnalysis 8x8 RMS (SA3), the interpolator LOW_HALF (SA3). Redirect in SA5/SA8/SA4/SA3. Region evidence reads the unit's CPU reference (`GrainSplit::Compute` via `ComputeGrain`) and the GPU product through `GrainView`; it has no estimator of its own.

## Private GPU textures (skeleton rule)

This module still creates its own GPU textures through the `VpvGpu::Ensure` wrapper (outputs and, without an arena, the temporaries; see the static test src/VP-V/tests/PrivateTextureScanTests.cpp). It migrates in SA6/SA7/SA8/SA12 (doc D:\codex\vp-handover\51-skeleton-adoption-audit.md section 6): the outputs come from the node's `ProductTextures` (`SplitterFill` with `Targets`), the per-render temporaries already come from the shared scratch arena when `Options::arena` is set and the private ones are then destroyed.

## Stored layout: main map plus modifiers (owner decision)

The node stores one main map and small modifier fields; consumers derive every other map from them (`GrainSplit::GrainFields()` is the table, tested). Base (the main map): `base`, `s1`, `s2` (clean = base + s1 + s2; grain G = Y - clean). Modifiers: per-pixel Wiener share `f`; per-block grain level `N` and focus-band energy `e3`. Derivations: temporal detail and chroma-from-luma work on clean and add G back; region evidence derives grain sigma, texture (E1 + E2 - N), ringing and banding from N and the bands; texture restore (SA8) splits grain from texture with f and uses e3 for acutance; the debander (SA5) gates on N; the camera model grain map reads N. In Full the block texel's last two channels are reserved for further modifiers (band energies, spectral shape descriptors): not written yet; adding one bumps `kUnitVersion` and the table. Both storage layouts carry the same fields.

## Base vs modifiers (owner decision, unit version 2)

One shared node stores the MAIN map plus small MODIFIER fields; every component derives its own maps from them on the fly (`GrainSplit::Gpu::GrainFieldTable`, `GrainSplit::GrainFields`; both tested).

| Field | Role | Domain | Full | Compact |
|---|---|---|---|---|
| s1, s2 | base | pixel | split.x, split.y | A.x, A.y (rg16f) |
| base | base | pixel | split.w | B.y x codeUnit (rg16) |
| f (Wiener share) | modifier | pixel | split.z | B.x |
| N (grain level) | modifier | block | blocks.x | blocks.x (rgba32f) |
| e3 (focus band) | modifier | block | blocks.y | blocks.y |
| e1, e2 (block means of d1^2, d2^2) | modifier | block | not stored (zw = 0) | blocks.z, blocks.w |
| spectral tilt | derived | block | not available | `gs_tilt(b)` = log2((e1 + 1e-3) / (e2 + 1e-3)) |

Accessors (every shader reads only these): `gs_fetch(p)` = (s1, s2, f, base) in codes, `gs_blocks(b)` = (N, e3), `gs_energies(b)` = (e1, e2) or (-1, -1) where unavailable, `gs_tilt(b)`; `GS_HAS_ENERGIES` is 1 in Compact, 0 in Full: a reader treats "unavailable" as absent and derives its own (visible, as for any missing modifier). The Compact blocks cost 16 B per block (was 8): 8 B/px + 16 B/block against Full's 16 + 16. The Full layout and its pinned shader bodies are untouched; the Compact block passes are separate bodies (same N and e3, plus e1/e2 in raster order, no atomics).

| Consumer | Derives | From | Status |
|---|---|---|---|
| temporal detail (TD) | MEAS E1/E2/E3, guard, admission | e1, e2, e3 (E1/E2 from the pixel sums today; Compact blocks offer them directly), N, f | wired (reads s1, s2, f, base, N, e3 through the accessor; e1/e2 shortcut later) |
| chroma-from-luma (CfL) | per-plane grain guard: clean picture and G | s1, s2, base, f (per plane), N | wired |
| region evidence | block grain level, texture (E1 + E2 - N), ringing, banding | N, e1, e2 (CPU form today) | wired (N); e1/e2 GPU shortcut later |
| deband (SA8) | low band, veto, light level P12 | base (low band), N (veto) | later package |
| detail pulse (SA5) | E1/E2 | e1, e2 | later package |
| texture restore (SA5) | grain / texture split | N + f instead of its own Wiener | later package |
| adaptive downscale (SA8) | grain map | N + e1 | later package |
| interpolator grain bands (SA4) | grain bands | base | later package |
| camera model grain-map refinement (K5) | grain map refinement | N, tilt | later package |
| contrast recovery (WP8) | halo guard | base + energies | later package |
