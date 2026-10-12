# Stage 10b — BTB/WTW through a side texture

2026-10-12. Owner-approved (option 1 of the Stage 10 correction options) after
the whole-chain tests found 10a's limits. Implemented in
`vprenderer/ExtendedRange.{h,cpp}`; planner review requested for one item
(precision, below).

## Why

10a carried each pixel's out-of-range amount in alpha (a = 1 + o) and shifted
the pixel by o, then converted BT.709 -> the output primaries after scaling.
Measured with the whole-chain tests:

- On saturated colours (one channel out of range) neighbouring pixels got
  different shifts; and the primaries were converted after scaling while
  libplacebo converts before. A non-linear user shader (SSimDownscaler,
  SSimSuperRes) then saw other values than without the switch: up to 46
  codes on a strongly coloured texture, also far from any out-of-range
  content.
- The inline carry crashed libplacebo 7.360 (renderer.c:457, "w && h") for a
  single-plane RGB source (the subtitle move's composed picture, an RGB
  capture) and after CfL chroma prediction; fixed first with a texture form.

## How (10b)

- **Carry** — the last hook at `PL_HOOK_RGB` (after NLS), texture input:
  source gamma -> output gamma (sign-preserving), primaries converted here
  (before scaling, where libplacebo converts without the switch), then two
  passes into VP's own rgba32f textures: the picture clipped to 0..1 (handed
  on to libplacebo) and the per-channel remainder (value - clipped, kept).
  Alpha is not touched.
- **Restore** — the first hook at `PL_HOOK_SCALED`, texture form: libplacebo's
  own delinearize when the stage is linear, then the remainder added back:
  sampled at the output pixel's position in the carry's rectangle, averaged
  over the pixel's footprint (4x4 bilinear taps, at least one source pixel
  wide; positive weights only, so nothing rings and pixels away from
  out-of-range content get exactly 0).
- libplacebo is still told the source is in the output's colour space (its
  own clamping conversion is a no-op); everything between the hooks (SSIM,
  SSimSuperRes, the Stage 8 blend, prescalers, CfL, user shaders) sees
  exactly the picture it sees with the switch off.

## Measured (WARP, 7.360)

- `ExtendedRangeOnSaturatedColourWithSsim` (new): wherever no out-of-range
  source pixel lies within 12 source pixels (the filters' reach), Stage 10 on
  vs off: P3 output plain 1 code, with SSIM 3 (10a: 46); BT.709 output 1 / 1.
  Near out-of-range content the outputs differ legitimately (the true values
  are mixed in instead of clipped ones): 9-16 codes, reported.
- BTB/WTW kept through every scaling path (`ExtendedRangeKeepsBtbAndWtw...`,
  the whole-chain tests including NLS, SSIM, the blend, CfL, FSRCNNX/ArtCNN,
  SSimSuperRes, the subtitle move, blanking, bar detection + lens).
- **Precision (for the planner):** in-range edges vs off stay within max 1
  code, but the mean rises to 0.03-0.083 (planner criterion: mean <= 0.03).
  The cause is libplacebo storing the scaled picture in its half-float
  texture before a texture-form restore (about 1/4 code); after the carry's
  texture output libplacebo rejects an inline restore (measured), so 10b
  always pays it (10a paid it only with SSIM / the blend). Limits recorded at
  mean <= 0.1, accepted by the planner 2026-10-12; max 1 is enforced.

## Not covered

The real GPU path for the Beta build after 10b (the Beta test exe is blocked
by Windows Application Control on this machine since the last rebuilds; the
Beta suite passed on 10b on WARP before the block).
