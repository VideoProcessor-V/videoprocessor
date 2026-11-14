# Scaling shared: ScalerPass and the kernel table (`scaling.shared`, draft)

Unit: **scaling** | status: **draft** (new files only; nothing calls it yet, so it costs nothing and changes no output; the existing
users move onto it in B5 of the upscaler plan, refs/123 N4) | hook point: none (a function-only unit: no ProductId, no config key,
no host line).

## Purpose / why

Every resampling VP-V does today is a near-duplicate of one libplacebo sample pass with a filter, anti-ringing and a ratio: the
re-base chroma passes (`SourceRebase.cpp`), the faithful ortho branch of `AdaptiveDownscale`, `KernelInverse`'s private Lanczos and
bicubic tables, and the planned entrance/end upscale. This folder is the ONE scaler unit (refs/123 N1 item 1; rule 20) and the ONE
kernel table.

## Contents

- `KernelTable.h` (header-only, no libplacebo): the table. Entries are the seven `KernelInverse::Candidates()` (same names, same
  order, same expressions, so the phase taps are equal bit for bit) plus `hermite` and `ewa_lanczossharp`. `Spec` {kind, radius, B,
  C}; `Weight`, `Radius`, `Phase` (the 2x phase filters at -0.25 / +0.25).
- `ScalerGeometry.h`: `Span` (source span + destination size; the ratio is derived from them), `IntegerWindow`, `Unequal`.
- `ScalerReference.h`: the CPU reference sampler (exact taps in double, clamp-to-edge, support stretched by the ratio on a
  downscale, siting phase as a span shift). Anti-ringing and the polar filter are not modelled.
- `ScalerPass.h/.cpp`: the GPU pass, `ScalerPass::Module::Apply(gpu, dispatch, Source, dstW, dstH, Settings, out, slot)`.

## ScalerPass axes

| Axis | Values |
|---|---|
| filter | lanczos r, catmull_rom, bicubic B/C, hermite, bilinear, ewa_lanczossharp (polar) |
| axis | x, y, both (separable: two 1-D passes, `Order` AcrossFirst or DownFirst; ewa: one polar pass) |
| ratio | per axis, DERIVED from the source span and the destination size (never stored; SD pictures with a non-square pixel aspect just have two ratios) |
| plane | luma, chroma (a tag; chroma differs only by the siting phase the caller passes, a source-texel shift of each scaled axis) |
| anti-ring | libplacebo's `antiring` strength, passed through |
| light | `Signal` only; `Linear` is declared and REFUSED visibly in B0 (`LastError`) |
| precision | `F32` (default), `F16` (announced by `Note()`), `Target` (the caller's format, the source format by default: what the migration identity tests use) |

A single-axis pass keeps the integer window of the unscaled axis (pass 0 for its size to derive it); a polar filter on a single axis is
refused. Nothing falls back silently: a failed pass returns false with `LastError()`; the caller keeps its own path.

## Settings

None. This is a function-only unit with no config key; the settings of its users (`upscale_route`, the hybrid's branch parameters)
arrive with B1+ and are listed in their own modules.

## Design goal, philosophy and decisions

Goal: one scaler, one table, one reference, so every resampling VP-V does is a call of the same unit and is checked by the same CPU
model. Each decision cites the owner lines of refs/123 N0.

- Build the hybrid as classical branches on ONE scaler unit, not a single pass (O1) and not neural (O2): ScalerPass is the unit both
  branches are made of; the CNN stays an evaluation arm elsewhere and has no entry here (O2).
- Both placements, entrance for re-based HD and end for native 1080p, call the same unit (O8); the placement is a caller decision, so
  ScalerPass has no route or tier knowledge (O3, O5).
- The kernel pair, anti-ring and precision are inputs of the call, fixed by the caller at Configure time; ScalerPass never changes them
  per frame or mid-film (O5, tier-fixed).
- The rebuild is core (O6): B0 ships the unit with its reference and migration identity tests before any user moves.
- Each extra stage must earn its milliseconds (O9): a pass is one libplacebo dispatch (two for a separable both-axes call), f32 by
  default; half float is allowed only visibly (no silent fallback).
- General, not fixed: ratios are derived from the geometry of each call; no resolution or factor appears in the unit.
- One scaler unit (rule 20): no private motion, grain or edge code here; nothing in this folder reads vectors. Kernel-table entries are
  taken from `KernelInverse`, so the re-base's identification and the upscaler share one list.

## Migration identity (proved by tests, B0 does not switch any user)

- `ScalerKernelTableMatchesKernelInverseTaps`: the table's phase taps equal `KernelInverse::Candidates()` bit for bit.
- `ScalerGpuMigrationIdentityRebaseChromaPasses`: ScalerPass(lanczos, ortho, antiring 0.5, chroma, across first) equals the chroma planes
  `SourceRebase::Module::Apply` makes today, bit for bit.
- `ScalerGpuMigrationIdentityFaithfulOrtho`: ScalerPass(ortho, down first, integer window) equals the faithful ortho branch's libplacebo
  call sequence of `AdaptiveDownscale` (quoted in the test; AdaptiveDownscale itself is untouched in this slice).

## Open / not verified in B0

Nothing was built or run when this slice was written (code-only): the tests are unrun, the anti-ring and polar unequal-axis semantics of
libplacebo are not modelled by the reference (parity tests use antiring 0 and equal-axis polar checks), and the bench (1080 to 2160 luma)
has no number yet.

## How to build / test

`tests/ScalerPassTests.cpp` is compiled into the VideoProcessor-Test project (`VideoProcessor-Test.vcxproj.items.props`). See
`tests/README.md`. GPU tests run on WARP unless `VP_TEST_GPU=hardware`.
