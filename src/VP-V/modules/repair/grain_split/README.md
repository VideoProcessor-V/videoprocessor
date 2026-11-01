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
