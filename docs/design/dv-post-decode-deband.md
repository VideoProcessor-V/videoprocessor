# Deband for Dolby Vision AFTER the tunnel decode (design, 2026-10-24)

Status: design only (feat/dv-l8). Queue item: "post-decode DV deband". Nothing here is built.

## Why

Streamed Dolby Vision (profile 5 / 8.1) is a 10-bit HEVC base layer plus a reshaping RPU. The
player reconstructs a 12-bit signal and sends it through the HDMI tunnel, but the base layer's
compression banding (flat skies, dark gradients, smoke) survives the reconstruction: the
reshaping is a smooth per-channel curve, so a band step in the base layer becomes a band step in
the 12-bit IPT picture, now spread over several 12-bit codes.

Today nothing debands it:
- libplacebo's plane deband and VP's own debander (`deband_vp`) are both passes on the SOURCE
  planes, before libplacebo's Dolby Vision decode. For the tunnel those are opponent-colour
  planes (I, Ct, Cp). Debanding there is the wrong domain: chroma rounding is amplified by the
  decode, and libplacebo's fp16 plane pass moved a saturated primary's near-zero channels by up
  to 3.6 output codes (docs/VP-dv-tunnel-autotest.md). Both are therefore skipped on tunnel
  pictures (`src/VP-V/modules/input/signal/DvTunnelPolicy.h`; the DV status line says
  "... deband off on the tunnel (12-bit IPT; no deband after decode yet)").
- The gap: streamed DV base-layer banding reaches the screen.

## Where it hooks

The right signal: the DECODED picture, still PQ-encoded (perceptually uniform, so one threshold
means the same visible step in shadows and highlights), BEFORE tone mapping (tone mapping
compresses highlights and would hide or distort bands; after it the steps are no longer uniform).

- libplacebo's Dolby Vision decode (IPT -> LMS -> BT.2020 RGB, PQ) happens in its colour decode,
  before linearisation. The first user-hook point after it is `PL_HOOK_RGB_INPUT` (RGB in the
  source colour space, before `LINEAR`, the scalers and the colour map). For the tunnel that is
  PQ BT.2020 RGB at source resolution: exactly the domain wanted.
- Hook there as a VP custom pass (like the calibration-LUT and DV trims passes) only for tunnel
  pictures (`PL_COLOR_SYSTEM_DOLBYVISION`); every other source keeps today's plane deband.
- Work on the ICtCp INTENSITY (I) of that RGB: convert PQ RGB -> linear -> LMS -> PQ -> I (the
  same written-out maths as `shaders/DV trims.glsl` dv_space; no matrix functions), deband I
  only, and rebuild RGB from the debanded I with the pixel's own Ct/Cp. Banding is an intensity
  phenomenon in practice; leaving Ct/Cp untouched avoids hue shifts at gamut edges and keeps the
  saturated-primary problem above impossible by construction.

## Which debander

Use VP's own algorithm (`VpDebander`), not libplacebo's:
- deterministic sample ring per pixel position (no per-frame random: a still picture never
  shimmers), no added grain (VP's output dither does that job);
- the grain guard / grain split (`deband_vp_grain_guard`, GrainPreserve.h): each pixel split
  into a low band and a residual; only the low band is debanded, the residual (grain, noise)
  passes untouched - the owner's grain rule;
- vector-held weights where the de-judder's vectors exist (no flicker or crawl), per-frame
  otherwise; never starts the matcher;
- the threshold from the source readings (effective bits, bitrate quality), here read on the
  decoded I plane (the source-resolution monitor already measures the tunnel's I).

Packaging: a single-channel variant of VpDebander's pass that takes the RGB_INPUT texture, computes
I, debands I with the existing kernels, and writes RGB back (rgba16f is enough on the decoded PQ
signal: an fp16 step at PQ 0.5-1.0 is 1/2048, below a 12-bit code, and the hazard that bit the
plane deband - amplification through the opponent decode - does not exist after the decode).
libplacebo's `pl_shader_deband` on the RGB would also work but adds per-frame random grain and has
no grain split: rejected.

## Grain-faithful constraints (memory: vp-grain-faithful)

- Never average grain away: only the low band of I is debanded; the residual is untouched.
  Gate: grain RMS through the stage >= 0.96x (the existing deband_vp target) on grainy film
  clips, temporal independence kept (no frame blending; vector holding only steers the blur
  weight, it never mixes frames' pixels).
- No synthetic grain substitution.
- Chroma untouched, so colour noise and chroma grain keep their amplitude by construction.
- BTB / WTW: PQ codes below the source minimum or above the source maximum pass unchanged (the
  stage works between them, as the trims stage does).

## Cost estimate (RTX 3090 primary; RTX 3050 Ti laptop as the reference)

VP's debander on luma at 4K costs about the same as one plane of today's deband_vp pass; the
extra work is the RGB <-> ICtCp conversion (a few dozen ALU ops per pixel, two PQ transfers each
way). Estimate: under 0.5 ms per 4K frame on the 3090, about 1-1.5 ms on the laptop (to be
measured with the existing deband timing harness). It replaces the skipped plane debands, so the
net change against today's DV path is small.

## Tests

- CPU: the I-only round trip is exact when the debander is idle (RGB in = RGB out within float
  precision); a synthetic PQ gradient with 10-bit base-layer steps reshaped to 12-bit loses its
  steps; Ct/Cp unchanged; BTB/WTW codes unchanged.
- GPU: shader parity with a C++ reference (as DolbyVisionTrimShaderL8AndIctcpPathsMatchTheirReferences);
  the synthetic DV corpus `acc-*` jobs stay within one code of the faithful path (no regression of
  the 3.6-code case); a new corpus segment with authored base-layer banding (dv_synth: quantise a
  smooth gradient to 10-bit, reshape, pack) shows the bands removed; grain RMS >= 0.96x on the
  grain segment and on real grainy clips (RealClipLibrary, via the existing grain gates).
- Status: "VP deband: active on the decoded DV intensity (threshold N codes)" in the log and the
  DV status line; "Fallbacks engaged: VP deband (DV post-decode pass not loaded)" if the pass fails.

## Relation to the Dolby-style tone mapping (DvDolbyStyle.h)

Order: decode -> post-decode deband (I, PQ) -> tone mapping (libplacebo's curve, or the
Dolby-style custom curve driven by L1) -> trims (luminance or ICtCp) -> output stages. The
deband runs on the SOURCE intensity, so it is independent of which curve follows; both curves are
monotonic in I, so a removed band stays removed. The Dolby-style curve compresses highlights more
than the source range would need when the target is low (e.g. 48 nits); banding is most visible
in shadows and mid-tones, which both curves keep close to the source, so the deband threshold
taken on the source (not the output) is the right one. The future display-adaptation module
(trims + CAM16) sits after the tone mapping and does not touch this stage.

## Open questions

- Whether L1-scene-aware thresholds help (dark scenes band more at the same bitrate).
- Whether profile 8.1 titles (YCbCr base layer reshaped to PQ) need a different threshold than
  profile 5 (IPT base layer); the readings on the decoded I should cover both.
