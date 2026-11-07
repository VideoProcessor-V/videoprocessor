# Basic chroma (stand-in): `scaling/chroma_upscale/basic`

A simple separable polyphase lanczos-class 4:2:0 / 4:2:2 (factors 1, 2, 4) to 4:4:4 chroma upsample with the source's chroma siting, for YCbCr and for the
DV-tunnel IPT (composed 12-bit IPT 4:2:2). A stand-in that proves the order, the 4:4:4 hand-off and the plumbing early; the hybrid CfL + MV-chroma upscale
(reset 2) replaces it. Status: draft, not wired (no key, no stage call; the SCHEMA lock is held by W1-P3).
Unit: scaling/chroma_upscale/basic (shared unit U20 `BasicChroma`, `kUnitVersion = 1`).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `chroma_upscale_basic` | deferred (profile; added when the SCHEMA lock is free) | enum | off, lanczos | off | deferred: VP-V > Scaling > Chroma upscale | libplacebo's chroma upscaler (either/or: Original = libplacebo chroma) | "Chroma upscale: libplacebo (<reason>)" | same on all (one pass, ~36 texel fetches per output pixel and plane pair) |

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| `VP_TEST_GPU`, `VP_TEST_ADAPTER` | which adapter the GPU byte-identity test runs on (WARP by default; `VP_TEST_GPU=hardware`, `VP_TEST_ADAPTER=NVIDIA` for the chain gate) | WARP | test |

## Products

Produces none (its output is the 4:4:4 chroma planes handed to libplacebo with zero shifts). Reads (read path only, nothing used in reset 1) the temporal-detail products
of the chroma stage: `td.maps` (validity, trail confidence), `td.delta`, `td.memory`, `refined_field` (see `../../../repair/temporal_detail/products/README.md`);
absent: the plain kernel below, byte-identical (the engine is not requested today); requested but absent: `product.chroma_cfl` engaged.

Position (designed, NOT wired now): unpack -> source ID + vectors on real samples -> re-base -> grain split -> luma base repairs -> **this unit (chroma to 4:4:4,
libplacebo's chroma upscaler skipped)** -> grain re-add -> scaling -> tone mapping -> output (doc 65 "Chroma ORDER v2"). Wiring comes with the M2 P4 hooks.

**The law (exact integer arithmetic: CPU model = GLSL, byte for byte)**

- Position: chroma sample j sits at luma `(j + 0.5) f + shift - 0.5` (libplacebo `pl_plane::shift_x / shift_y`, luma-pixel units, as `ChromaCfl.h`;
  co-sited = `-(f-1)/2`, centred 0). `ShiftOf(Siting, fx, fy)` maps a stated siting (left, centre, top-left, top, bottom-left, bottom) to the shift.
  With `n = 4x + 2 - s4 - 2f` (`s4 = 4 shift`: shifts are multiples of a quarter luma pixel, which covers the common shifts and a shift scaled by a
  re-base factor that is a multiple of 0.5, `ChromaSiting::ShiftAfterResample`): `i0 = floor(n / 4f)`, `phase = n - 4f i0`.
- Kernel: Lanczos a = 3, 6 taps `i0-2 .. i0+3`, 4f phases, Q14 weights, normalised before rounding, summing exactly to 16384 (the largest tap takes the few-unit rounding remainder).
- Passes: horizontal then vertical, `(sum + 8192) >> 14` in int32 each; the horizontal result stays an integer. The int32 bound
  (65535 x (P^2 + N^2) / 16384, P / N the positive / negative weight sums, 1.82e9 < 2^31) is asserted by a test.
- Range: only the 16-bit container is clamped (`[0, 65535]`); overshoot beyond the video range is signal (BTB / WTW preserved).
- Edges: taps clamp to the plane; inside the active picture to the picture's rectangle, in the bars to the bar's side (nothing reads across).
- Refused (visible status, the caller keeps libplacebo's upscaler): a factor other than 1, 2, 4; a siting shift that is not a multiple of a quarter pixel (a
  re-base factor that is not a multiple of 0.5 can produce one: that frame keeps libplacebo's chroma, said in the status); plane sizes that do not match `ceil(luma / f)`.

**DV-tunnel IPT input.** The composed 12-bit IPT frame arrives as plane 0 = I (full size) and plane 1 = an `rg16` texture of size W/2 x H whose two components alternate
P and T (4:2:2, horizontally co-sited = `Siting::Left`), codes in the PQ-IPT code domain (12-bit codes in the 16-bit container, `<< 4`). The filter is code-agnostic
(weights sum to 1): `Gpu::Module::Apply` takes that texture as it is and returns an `rg16` P/T pair at I's size. The tests use 12-bit codes `<< 4`.

## Status and fallbacks

"Chroma upscale: libplacebo (<reason>)" while a geometry is refused (factor, shift, plane size) or the GPU pass fails (`Module::LastError()`); `product.chroma_cfl` for the
temporal-detail read path once the engine is requested. Both are visible, never log-only.

## Tests

`BasicChromaTests.cpp` (`VideoProcessor-Test`): `BasicChromaModelTests` (CPU): phase tables sum exactly and respect the int32 bound; position logic against double math for every
factor and siting; co-sited outputs equal the input exactly; siting on a ramp; mirror symmetry; the kernel against a double-precision reference (worst error logged, <= 4 codes);
flat exact, BTB / WTW overshoot kept; bars flat and no read across the boundary; composed 12-bit IPT 4:2:2; refused geometries; the engine read path.
`BasicChromaGpuTests` (GpuWarp): the GLSL pass is BYTE-identical to the CPU model for 9 geometries, refuses what the CPU refuses, GLSL arithmetic is integer only.
Command: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:"FullyQualifiedName~BasicChroma"`; the chain gate runs the GPU class with `VP_TEST_ADAPTER=NVIDIA` while holding `D:\codex\GPU-QUIET`.
