# Basic chroma (stand-in): `scaling/chroma_upscale/basic`

Shared unit **U14** (`BasicChroma`, `kUnitVersion = 1`) | status: **draft, not wired** (R1-W1-RES part B, owner 2026-11-03).
A simple separable polyphase lanczos-class 4:2:0 / 4:2:2 (factors 1, 2, 4) to 4:4:4 chroma upsample with the source's chroma siting, for YCbCr
and for the DV-tunnel IPT (composed 12-bit IPT 4:2:2: P and T). A stand-in that proves the order, the 4:4:4 hand-off and the plumbing early;
the hybrid CfL + MV-chroma upscale (reset 2) replaces it.

## Position (designed, NOT wired now)

unpack -> source ID + vectors on real samples -> re-base -> grain split -> luma base repairs -> **this unit (chroma to 4:4:4, libplacebo's chroma
upscaler skipped)** -> grain re-add -> scaling -> tone mapping -> output (doc 65 "Chroma ORDER v2"). Wiring comes with the M2 P4 hooks.

## Either/or (deferred key)

Original = libplacebo's chroma upscaler. The planned key `chroma_upscale_basic = off | lanczos` is **deferred**: no key is added now because the
SCHEMA lock (`RendererProfileConfig.h` + `MainConfigSchema.h`) is held by W1-P3. Add it with `slots.json` `planned_keys` and the manifest of
`scaling.chroma_upscale` when the lock is free.

## The law (exact integer arithmetic: CPU model = GLSL, byte for byte)

- Position: chroma sample j sits at luma `(j + 0.5) f + shift - 0.5` (libplacebo `pl_plane::shift_x / shift_y`, luma-pixel units, as `ChromaCfl.h`;
  co-sited = `-(f-1)/2`, centred 0). `ShiftOf(Siting, fx, fy)` maps a stated siting (left, centre, top-left, top, bottom-left, bottom) to the shift.
  With `n = 4x + 2 - s4 - 2f` (`s4 = 4 shift`: shifts are multiples of a quarter luma pixel, which covers a shift scaled by the re-base, `ChromaSiting::ShiftAfterResample`): `i0 = floor(n / 4f)`, `phase = n - 4f i0`.
- Kernel: Lanczos a = 3, 6 taps `i0-2 .. i0+3`, 4f phases, Q14 weights, normalised before rounding, summing exactly to 16384 (the largest tap takes the few-unit rounding remainder).
- Passes: horizontal then vertical, `(sum + 8192) >> 14` in int32 each; the horizontal result stays an integer. The int32 bound (65535 x 16384 x (P^2 + N^2), P / N the positive / negative weight sums) is
  asserted by a test.
- Range: only the 16-bit container is clamped (`[0, 65535]`); overshoot beyond the video range is signal (BTB / WTW preserved).
- Edges: taps clamp to the plane; inside the active picture to the picture's rectangle, in the bars to the bar's side (nothing reads across).
- Refused (visible status, the caller keeps libplacebo's upscaler): a factor other than 1, 2, 4; a siting shift that is not a multiple of a quarter pixel;
  plane sizes that do not match `ceil(luma / f)`.

## Engine read path

`Upsample(...)` and `Result::td` read the reserved temporal-detail products for the chroma stage (base buffers with 4:2:0 planes, mirror, refined
vectors, confidence): absent -> the plain kernel, byte-identical (not requested today); requested but absent -> `product.td_chroma` engaged
(see `repair/temporal_detail/products/README.md`).

## Files

`BasicChroma.h` (CPU model, tables, siting, geometry), `BasicChromaGpu.h/.cpp` (one compute pass: `BuildHeader` + `Body`, `Module::Apply`),
`tests/BasicChromaTests.cpp`. The GPU test class `BasicChromaGpuTests` is a `GpuWarp` class (WARP unless `VP_TEST_GPU=hardware`; the chain gate runs
it on the NVIDIA adapter with `VP_TEST_ADAPTER=NVIDIA`).
