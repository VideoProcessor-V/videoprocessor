# Stage 12 — effective resolution and low-bitrate handling (added to the deploy by the owner)

2026-10-14. Owner's observation: streaming (Crave especially) and
upscaled SDR releases arrive at 4K from the Apple TV, so VP treats them
as native 4K and never applies its upscale path. Two parts: detect the
picture's *effective* resolution and bitrate quality live; then undo the
upstream upscale and apply VP's own. Mostly reuse; small; in the deploy by
the owner's decision.

## 1. Detection (per source frame, one reduction each; cinema and Low_Latency)

### 1a. Effective resolution
- Radial power spectrum of the source luma (a 256-bin radial average of a
  windowed FFT over a few 512×512 tiles, or the equivalent from a
  Laplacian pyramid's per-level energy — the pyramid form is cheaper and
  already exists in the matcher). Find the frequency where energy falls
  to the noise floor; map to the nearest of {2160, 1440, 1080, 720}.
- Exclude blocks the grain map flags (grain and compression noise are
  broadband and would read as "detail").
- Smooth over ≈2 s; snap on a cut; publish `effective_height` on the
  stats line and the Config monitor. Hysteresis between classes so a
  borderline source does not flip.

### 1b. Low-bitrate quality
- Block-boundary energy: the ratio of luma gradient energy at 8- and
  16-pixel periods to the mean gradient energy (blocking); plus the share
  of flat 8×8 blocks with a single code step across them (banding).
- Smoothed and snapped as above; `bitrate_quality` ∈ [0, 1] on the stats
  line.

## 2. Response

### 2a. Upscaled source (effective < native)
- Downscale the 4K frame to `effective_height` with the faithful kernel
  (`ewa_lanczossharp`, linear light, anti-ring) — this removes the
  upstream upscaler and its ringing — then hand the frame to VP's own
  upscale path exactly as a native 1080p/720p source would be handled
  (FSRCNNX / ArtCNN, CfL, SSimSuperRes, the 8b blend, the grain term).
- Done at the point where the source enters the hook chain (a
  pre-scale "re-base" pass), so every later stage sees a source of the
  effective size and the 8b gate (source height < output height) fires
  naturally.
- Cost: one downscale at 4K per source frame plus the upscale path at
  the effective size — about the same as the native 4K path.

### 2b. Low-bitrate source
- Debanding on for that source, strength scaled by `bitrate_quality`.
  Caveat: with Match Dynamic Range off, the Apple TV delivers everything
  — Crave included — as LLDV, i.e. an HDR source in VP, where
  libplacebo's debanding has the black-speck bug (deband is off on HDR
  profiles for that reason). So for the HDR case this needs VP's own
  deband pass (a simple gradient-aware dither-and-blur in the same
  module as the grain term), not libplacebo's; for native SDR (Match
  Dynamic Range on) libplacebo's is fine.
- The grain map treats blocking as noise (it already reads the
  motion-compensated difference; blocking is temporally inconsistent at
  low bitrate), so the SSIM / CNN weights back off where the picture is
  broken.
- Optional later: a light deblocking pass at the detected block period.

## 2c. Upscale refinements (build with 2a)

- **Temporal stabilisation of the CNN residual** (`cnn_residual_stabilise:
  on`): the CNN's added detail (its output minus the kernel's) is what
  flickers on compressed streams, re-hallucinated every frame. Where the
  de-judder's vectors are trusted, average that residual with the
  previous frame's motion-compensated residual (one-pole, ≈3 frames);
  the kernel part is untouched; untrusted blocks use the current
  residual. Removes shimmer without softening static detail.
- **Bitrate-aware anti-ringing**: at low `bitrate_quality`, raise the
  faithful kernel's anti-ringing and lower the CNN weight (beyond what
  the grain term already does) — blocking edges are where a CNN rings
  worst.
- Re-base kernel stays `ewa_lanczossharp`; no attempt to identify the
  upstream upscaler.

## 2d. Debander refinements (VP's own pass; build with 2b)

- Algorithm: libplacebo's (random-radius sampling, blur where the
  neighbourhood is within threshold, `iterations`) reimplemented in VP's
  shader — same behaviour, without the HDR speck bug.
- **Effective bit depth as the strength source** (`effective_bit_depth`
  on the stats line): if the source's codes sit on multiples of 4 it is
  8-bit content in a 10-bit container; deband strength scales from 0 on
  true 10-bit to full on 8-bit. One reduction per source frame.
- **Thresholds in PQ units** for HDR sources (and in the source transfer
  for SDR), so one step of banding means the same visible thing in
  shadows and highlights.
- **Grain-map exclusion**: no blur where the map says texture.
- **At source resolution, in the re-base pass**, before any upscale.
- **Vector-stabilised decisions**: the blur-or-not region held across
  frames along the motion where trusted, so deband regions do not
  flicker.

## 3. Controls (settings first; constraint 12)

```
[rendering profile]
source_resolution: auto | native | 1440 | 1080 | 720   # manual override is primary
source_resolution_auto_min_confidence: 0.8
low_bitrate_handling: auto | off | on
cnn_residual_stabilise: on
deband_vp: auto | off | on            # VP's own; strength from effective_bit_depth in auto
```
- `native` = today. Manual classes are the primary control; `auto`
  uses 1a/1b once the owner has watched the readings for a few evenings.
- Stats line: `effective_height`, `bitrate_quality`,
  `effective_bit_depth`, and which response is active.

## 4. Tests

- Synthetic: a 1080p test chart upscaled bicubically to 4K reads as
  1080 (and 720 as 720); a true 4K chart reads as 2160; a grainy 1080p
  reads as 1080 (grain excluded); a cut between classes snaps; a
  borderline source does not flip within the hysteresis.
- Response: the re-based 1080p chart through the upscale path matches a
  native 1080p source through the same path within half-float; `native`
  is byte-identical to today.
- Low bitrate: a blocked synthetic frame gives `bitrate_quality` < 0.5
  and turns debanding on; a clean one stays off.
- Bit depth: an 8-bit ramp in a 10-bit container reads as 8 and is
  debanded; a true 10-bit ramp reads as 10 and is untouched; grain is
  excluded; the deband region is stable across a trusted pan.
- Residual stabilisation: a flickering synthetic CNN residual on a
  trusted pan is averaged; static detail is unchanged; untrusted blocks
  pass the current residual.
- Stage 10 carry, LED measurement and the de-judder see the re-based
  source consistently (the active-picture rectangle and the matcher use
  the effective size).

## 5. Cinema check

A Crave stream and a known upscaled SDR release: `native` → the manual
class → `auto`. Look for: fewer ringing halos on edges, cleaner text,
no added softness on real detail; on the low-bitrate stream, banding in
skies and blocking in shadows reduced without the picture going flat.

## Order

In the deploy: after the overnight items (D1 re-measure, D2 per-pixel
field, the two test fixes), before the final gate. Build 1a/1b with the
stats-line readings and the manual `source_resolution` classes first
(that alone is usable on day one), then 2a/2b behind `auto`; `native` =
today, byte-identical. Each behind its setting, tests as listed, the
matrix axis, both warp paths where the matcher is involved.
