# Stage 12 + delta 15 — build plan (roadmap item 5)

2026-10-15. Plan for `docs/VP-stage12-effective-resolution.md` and
`docs/VP-delta-15-downscale-refinements.md`. Nothing built yet. Order as
the spec: detection and the manual classes first (usable on day one),
then the responses behind `auto`; `native` is byte-identical to today.

## Step A — settings and the manual re-base (no detection)

- `[rendering profile]` `source_resolution: auto | native | 1440 | 1080 | 720`
  (default `native`), `source_resolution_auto_min_confidence`,
  `low_bitrate_handling`, `cnn_residual_stabilise`, `deband_vp`,
  `ssim_residual_stabilise`, `ssim_bitrate_aware`, `ssim_bitrate_floor`.
  Config controls for each (constraint 12; scaling card), parsed in
  RendererProfileConfig like the Stage 8 settings.
- Re-base point: after the source planes are uploaded/unpacked
  (LibplaceboVideoRenderer, the P010/P210 / native RGB / GPU RGB unpack
  block) and after the source warp replaces them, i.e. on the `pl_frame`
  that goes to `pl_render_image` (normal and extra paths alike). A
  `SourceRebase` module replaces each plane with a texture downscaled to
  the class height (width by the same factor, rounded to the chroma
  grid): luma 2160 -> 1080, 4:2:0 chroma 1080 -> 540, RGB 4:4:4 as one
  plane. `image.crop` and every source-pixel rectangle handed to the
  hooks (NLS mapping, the grain field's `sourceCrop`, the active-picture
  rectangle) are scaled by the same factor.
- Kernel (owner agreed 2026-10-15): `ewa_lanczossharp` with anti-ringing, per plane in the signal
  domain (not linear light). Deviation from the spec's "linear
  light": a per-plane re-base keeps the source a real YCbCr 4:2:0 frame,
  so CfL, the chroma upscaler and every hook see exactly what a native
  1080p source gives them; and the upstream upscaler worked in the signal
  domain, so undoing it there is the closer inverse. A linear-light
  re-base would mean converting to RGB first, which the native path
  never does.
- Everything downstream then sees a 1080p source: the 8b gate (source
  height < output height) fires by itself; Stage 8's downscale does not
  (S8-3 needs no special case); the edge term is computed on the re-based
  picture (S8-4 by construction).
- The matcher keeps running on the native frame (its input is the CPU or
  GPU payload, before the re-base). Consumers in source pixels (the 8-3
  grain map, the dither vectors, Stage 10 carry) take the vector field
  scaled by the re-base factor; the vectors-valid state is invalidated
  when the class changes (a discontinuity, as at a resync).
- Stats line: `source_resolution=native|1080 (manual)` and which
  response acted (`rebase+8b`, `stage8`, `native`).
- Tests: `native` byte-identical; a 1080p chart upscaled bicubically to
  4K and re-based to 1080 through the 8b path matches the native 1080p
  chart through the same path within half-float; the crop and grain-map
  mapping scale; RGB and YCbCr inputs; both capture_upload paths.

## Step B — detection 1a/1b, readings only

Owner 2026-10-15: detection must not trigger inaccurately. A false re-base
softens real 4K (worse than missing an upscaled stream), so the design is
biased to native: readings only until the owner has watched them; auto acts
only on high, sustained confidence with hysteresis and cut resets; grain and
compression noise excluded; any doubt = native. Tests lead with false
positives (true 4K charts, grainy 4K, borderline sources) before the true
upscaled 1080/720 cases. Manual classes stay primary; auto is opt-in.

- Effective resolution, per source frame on the native luma: for each
  class h in {1440, 1080, 720}, the residual R_h = Y - up(down(Y, h))
  (separable passes at a few tiles, not the whole frame), its energy
  reduced on the GPU outside grain-map blocks (or a flat-region mask
  without vectors) and read back a few frames late like Stage 8's stats.
  An upscale from height h leaves R_h near the noise floor; the effective
  height is the smallest h whose residual is below the floor relative to
  R_1440's band energy. Smoothing ~2 s, snap on a cut (Stage 8's mean
  luma cut detector), hysteresis between classes. Derived from the
  frame's own height: the classes are fractions of the native height,
  not fixed 2160 (constraint: general, not fixed).
- `bitrate_quality` from block-boundary gradient energy at 8/16 px
  periods and the flat-block single-step share; `effective_bit_depth`
  from the code histogram's multiples of 4. Same reduction pass.
- Readings on the stats line and the Config monitor only; nothing acts
  on them yet. The owner watches them for a few evenings.
- Tests as the spec §4 (upscaled 1080/720 charts, true 4K, grainy 1080,
  cut snap, borderline hysteresis, blocked frame, 8-bit-in-10 ramp).

## Step C — the responses behind auto

- `source_resolution: auto` re-bases from the reading when its confidence
  passes the minimum.
- 2b/2d VP's own debander (libplacebo's algorithm reimplemented; PQ-unit
  thresholds on HDR; strength from `effective_bit_depth` and
  `bitrate_quality`; grain-map exclusion; at source resolution in the
  re-base pass; vector-held regions).
- 2c CNN residual stabilisation and S8-1 SSIM residual stabilisation
  (one shared motion-compensated one-pole on the residual, trusted blocks
  only; falls back without vectors, never starts the matcher).
- 2c / S8-2 bitrate-aware anti-ringing and SSIM weight.
- Tests per the specs, both warp paths where vectors are read.

## Status 2026-10-15 night (re-read against the specs verbatim)

Step A built (9cf342e3): `source_resolution` native | auto | any height
144-8640 (owner: general, not a fixed list), per-plane re-base, crop scaled,
Stage 8 gates on the re-based height. Gaps against the spec and this plan:

- A1 the other settings and Config controls: `source_resolution_auto_min_confidence`
  (0.8), `low_bitrate_handling` (auto|off|on), `cnn_residual_stabilise` (on),
  `deband_vp` (auto|off|on), `ssim_residual_stabilise` (on),
  `ssim_bitrate_aware` (on), `ssim_bitrate_floor` (0.4).
- A2 stats line: `source_resolution=...` and the response that acted
  (`rebase+8b`, `stage8`, `native`); S8-3 precedence stated in both Config
  cards.
- A3 while re-based the grain map, occlusion mask and dither grain are off
  (a safe first cut); the plan is the vector field scaled by the factor and
  the vectors-valid state invalidated when the factor changes.
- A4 tests still to add: `native` byte-identical; the re-based 1080 chart
  through the 8b path matches the native 1080 chart within half-float; RGB
  and YCbCr inputs; both capture_upload paths.

Step B, detection (SourceResolutionDetector, in progress):

- Effective resolution by **mirror coherence** instead of the down/up
  residuals or the spectral cliff: a resample from rate r makes the
  content at f and r - f phase-locked copies; real detail is independent.
  Measured on the 7 real 4K clips: genuine frames 0.1-0.2, uncoded upscales
  0.9-0.99 at the exact ratio with every kernel (lanczos, Catmull-Rom,
  Mitchell, bilinear). Both axes must agree (the UVG 120p clips are scaled
  vertically only and must read native); the 8x8 coding grid is excluded;
  the fundamental is taken over its harmonics. The spectral cliff was
  measured too and overlapped (real 4K up to 0.92 decades, cubic upscales
  from 0.29) - kept only as the texture gate.
- Display both: the spec's live reading (~2 s smoothing, snap on a cut,
  hysteresis) and the conservative declared value auto would use (several
  shots, a minute of evidence, native at once on any detail above it,
  reset on seek/skip, source/format/HDR-DV mode change, signal loss,
  resync; paused or repeated frames add nothing).
- Still to build: grain-map exclusion of tiles; `bitrate_quality` (8/16 px
  block-boundary gradient energy against the mean, plus the share of flat
  8x8 blocks with a single code step); `effective_bit_depth` (codes on
  multiples of 4); renderer wiring (tile readback, worker thread, the
  resets, stats line, crash state); spec §4 tests (blocked frame,
  8-bit-in-10 ramp, cut snap, borderline hysteresis).
- Apple TV with Match Dynamic Range off delivers LLDV (spec §2b): readings
  must work in PQ codes (tested: SDR re-encoded to PQ keeps its reading),
  and tunnelled DV must be read from the decoded picture.

- Owner 2026-10-15: the debander (spec §2b/§2d) is for any low-bitrate
  source, native 4K streams included, not only re-based ones. It runs at
  source resolution at the re-base point: inside the re-base pass on a
  re-based source, on the native planes otherwise. `bitrate_quality` and
  `effective_bit_depth` are therefore read on every source, independent
  of the resolution reading. `effective_bit_depth` from the share of
  codes in use (not only "multiples of 4"), so 8-bit content re-encoded to
  PQ by the Apple TV (LLDV) still reads its real step (measured: 8-bit
  SDR in PQ codes reads 9, two PQ codes per level - the step to hide).
- Owner 2026-10-15: the debander is vector-informed (spec §2d): its
  blur-or-not regions held across frames along trusted vectors, with the
  standing fallback (vectors when present for the frame, else per frame;
  never starts the matcher); tests on both warp paths and without vectors.
- Found on the real clips: the UVG "10-bit" files carry 8-bit content
  (every code a multiple of 4) - the bit-depth reading says 8 on them.

- Owner 2026-10-15 (supersedes "readings only first" and "the owner
  watches them for a few evenings"): `source_resolution: auto` is a
  selectable option now - it re-bases from the declared (conservative)
  reading. Default stays `native`. Engaging waits for the next cut (no
  change mid-shot); disengaging is immediate (detail above the re-based
  height, seek/skip, source/format/HDR-DV change, signal loss, resync).
  Gated by `source_resolution_auto_min_confidence` (0.8). The stats line
  names what auto chose and why.

Then step C (VP debander with PQ thresholds, the vector refinements
2c/2d/S8-1 with the dither_vectors-style fallback, S8-2).

## Status 2026-10-16 early morning (built overnight)

- Detection in the renderer and `source_resolution: auto` (7a1bfeec):
  SourceResolutionMonitor (tiles twice a second, worker thread, never
  waits), auto engages at a cut and disengages at once; status in the stats
  line and crash state (plugin API 31); Config: Auto (detected), Auto:
  confidence needed.
- A3 done: the grain map and occlusion mask stay on while re-based (both map
  through the native crop by relative position).
- A4: the re-based chart vs the native chart through libplacebo's upscale
  path: mean 0.00005 (half-float), worst 0.0023 (< one 8-bit code: the
  upstream kernel's own softening, which no re-base returns), 10x closer than
  the upscaled source as it was. Still to add: RGB input, both
  capture_upload paths, an explicit native byte-identity check.
- Step C, VP deband (`deband_vp` off/auto/on, default off;
  `low_bitrate_handling` auto/off/on): built - see BUILD-STATUS. Measured:
  8-bit sky banding 1.15 -> 0.07 codes RMS, texture variance 98% kept; the
  hold follows the vectors and is confined to the current blur band (never
  more blur on texture than the frame's own decision).
- A1 now partly: `deband_vp`, `low_bitrate_handling`,
  `source_resolution_auto_min_confidence` exist with Config controls; the
  rest (`cnn_residual_stabilise`, `ssim_residual_stabilise`,
  `ssim_bitrate_aware`, `ssim_bitrate_floor`) arrive with their features
  (2c / S8-1 / S8-2).
- 2c / S8-1 residual stabilisation built (`cnn_residual_stabilise`,
  `ssim_residual_stabilise`, on = with the vectors): residual flicker 0.81 ->
  0.22 codes, still picture unchanged, untrusted / off byte-identical.
- Fixed: the settings fingerprint lacked `source_range`, `capture_upload`
  and the Stage 12 keys, so a profile edit of only those was not applied live.
- 2c / S8-2 built: at a low bitrate quality the SSIM and CNN weights fall
  towards `ssim_bitrate_floor` (0.4) and the faithful anti-ringing rises
  (`ssim_bitrate_aware` on; 1 at quality 0.8 and above = today's output;
  steps of 0.05; low_bitrate_handling off = every source clean).
- A4: RGB input (one plane, all components, restored to 0.00 codes) and the
  early capture upload through the renderer added. `native` byte-identity
  stays by construction (factor 1: the frame is never touched; the harness
  checks no re-base runs) - no pixel-capture comparison yet.
- CNN-branch test added: CNN residual flicker 0.42 -> 0.12 codes, untrusted
  and off byte-identical.
- Open: a live profile-edit test of the fingerprint fix (needs a mid-run
  configuration change in the harness).

## Costs (to measure, laptop then 3090)

- Re-base: one downscale per plane per source frame at 4K input (the
  faithful branch Stage 8 already runs is the reference), then the 1080p
  upscale path, which is cheaper than the 4K path it replaces.
- Detection: three down/up residuals on tiles plus one reduction per
  source frame; target well under 1 ms on the 3090.

## Kernel-matched re-base for 1080p (owner 2026-10-16: "1080p will be the most common variant that streaming upscales from so this one needs to work ... optimize for it")

**Finding** (CPU prototype, scratch kernel_id3.py). Material: real x265 1080p decodes (UVG Bosphorus, ReadySetGo) upscaled 2× to 4K by ffmpeg. When the player's upscale kernel is known, its least-squares inverse recovers the decoded 1080p picture almost exactly.

PSNR against the 1080p decode:

| ffmpeg kernel | Today (Lanczos-type downscale) | Kernel-matched inverse |
|---|---|---|
| Bicubic | 61.6–66.7 dB | 74.4–76.8 dB |
| Lanczos | 63.3–68.0 dB | 75.3–77.7 dB |

On the synthetic set, at 8 bits: +9 to +12 dB.

**Identification.** Each candidate kernel's inverse, then the reprojection residual. Only the right kernel reaches the quantisation floor (0.13–0.25 codes here).

**Safety:**
- **Interpolating candidates only:** bicubic B=0 with C 0.5 / 0.6 / 0.75, Lanczos 2/3/4, bilinear (later spline36/64). A smoothing kernel (B-spline, Mitchell) fits a natural-spline upscale perfectly, but its inverse returns the prefilter coefficients: −20 dB.
- **A strict decision rule:** the best residual at or under the floor and at least 1.1× ahead of the next.
- **Result:** in all 8 real cases the decision was at least as good as today, and never worse. Spline and bilinear are not accepted and fall back.

**Filter.** A separable FIR of 32 taps per axis (48 for Lanczos-4: 99.3% at 32) at stride 2, one phase. GPU cost is trivial.

**Build:**
1. CPU identification in the monitor worker, on its 4K tiles, when the declared ratio is 2:1:
   - the floor follows the effective bits;
   - votes per accepted frame, a decision after shots/time;
   - a reset at discontinuities.
2. A GPU inverse re-base (two separable passes, the identified kernel's weights). Fallback: the polar EWA filter (today) when nothing is identified or the ratio is not 2:1.
3. Tests: synthetic exact recovery, GPU = CPU, the real ffmpeg material, the renderer path.
4. Later: 3:2 (1440p), per axis, chroma, and the Apple TV's own kernel from real captures (G19).
