<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Evaluation round 2: repair headroom and motion-compensated local tone mapping

CPU only; no pixels committed. Scripts: `artefact_headroom.py`,
`tonemap_ref.py` and `ltm_eval.py`, with tests in `test_tonemap_ref.py`. Tables are in
`prototypes/artefact_headroom/` and `prototypes/ltm/`.

Each result is judged in the space the viewer actually gets:
* **SDR:** SDR sources, measured in gamma codes.
* **HDR-PQ:** HDR10 passed through, measured in PQ codes.
* **HDR->SDR:** master and decode both pass through VP's FL35 curve, then are measured in SDR codes.
  The curve is libplacebo's spline reproduced at VP defaults: peak detection off, 100 nit, black 0.

## A. Headroom for targeted repairs

Method as for the mosquito headroom: 1024x768 windows, 12 frames, at 1080p and 2160p.
* **SDR masters (8-bit):** Sintel sky, Cosmos, Meridian, tos4k dark, tos_dark_2stops.
* **HDR masters:** Sol Levante sky and dark, Nocturne dark and low-key (grain).
* **Encoders:** x264, x265 and libaom at 8 bit for SDR; x265 and libaom at 10 bit for HDR.

The error is split by scale, so grain loss never counts as banding:
* the coarse error (3 px Gaussian) is banding or blocking;
* the fine error (1-px residual) is texture or grain.

"hd" is the PSNR gain from a perfect repair, given as the mean over masters / the maximum. "cal" is
the master quantised to 8 bits with no dither, for scale.

| context | raster | encode (Mb/s) | band hd dB | false-flat | band step p99 (codes) | dark hd dB | blockiness excess | texture kept | texture hd dB |
|---|---|---|---|---|---|---|---|---|---|
| SDR | 2160p | x264 crf28 (6.2) | 0.03 / 0.04 | 0.17 | 4.6 | 0.02 | **+0.34** | 0.28 | 0.38 / 0.54 |
| SDR | 2160p | x265 / aom low (3.4 / 3.1) | 0.05 / 0.07 | 0.20-0.23 | 5.5-5.7 | 0.02-0.03 | 0.01 / -0.05 | 0.24-0.29 | 0.34-0.39 |
| SDR | 1080p | x264 crf28 (1.7) | 0.04 / 0.08 | 0.07 | 4.6 | 0.03 | **+0.37** | 0.31 | 0.38 / 0.79 |
| SDR | 1080p | x265 / aom low (1.1 / 1.0) | 0.07 / 0.11 | 0.09 | 5.4-6.2 | 0.05 | 0.06 / -0.01 | 0.27-0.34 | 0.36-0.47 / 1.03 |
| HDR-PQ | 2160p | cal | 0.05 | 0.00 | 0.5 | 0.02 | 0 | - | - |
| HDR-PQ | 2160p | x265 crf30 (7.4) / aom crf40 (4.0) | **0.29 / 0.72** | 0.09-0.16 | 10-11 | 0.15-0.18 / 0.36 | -0.1 | 0.21-0.29 | 0.22 |
| HDR-PQ | 1080p | x265 crf30 (1.8) / aom crf40 (1.6) | 0.10-0.12 / 0.20 | 0.06-0.07 | 7.4-8.9 | 0.06-0.09 / 0.23 | +0.02 | 0.18-0.25 | 0.22-0.24 |
| HDR->SDR | 2160p | x265 crf30 / aom crf40 | **0.29 / 0.56** | 0.10-0.17 | 10-11 | 0.17-0.20 / 0.41 | -0.1 | 0.19-0.28 | **0.55 / 1.1** |
| HDR->SDR | 1080p | x265 crf30 / aom crf40 | 0.08-0.10 / 0.19 | 0.07 | 7.6-9.1 | 0.06-0.09 / 0.22 | +0.01 | 0.19-0.28 | 0.39-0.48 / 1.1 |

**(1) Banding.** Worth a repair for 10-bit HDR; not for 8-bit SDR.
* **SDR:** at most 0.11 dB, with steps of 1 to 1.5 eight-bit codes. That is the known 8-bit deband case.
* **HDR, smooth gradients:** the Sol sky at 2160p, 4 to 7 Mb/s, has up to 0.7 dB of headroom.
* **HDR, dark grainy scenes:** Nocturne has up to 0.3 dB. Stripping the grain turns 15 to 33 % of
  the ramps into plateaus.
* **Tone mapping makes it worse:** it stretches the steps from 7-8 PQ codes to 11-13 SDR codes,
  about 3 eight-bit codes. These steps are 15 to 20 times the 8-bit quantisation of the master.

Next step: evaluate VP's existing `repair/deband` (PQ-aware, applied before tone mapping, through the
grain gates), not a new module. Targets: x265 and AV1 HDR at or below about 8 Mb/s at 2160p, and all
tested 1080p rates.

**(2) Dark blocking.** Not worth a repair.
* **HEVC and AV1:** blockiness is within ±0.06.
* **x264:** blockiness is +34 to 45 % (Cosmos +130 %). But the removable boundary energy is at most
  2.6 % of the error, i.e. 0.11 dB or less.
* **Nocturne at 2160p:** the boundary-step energy is block-wise grain loss, and the decode is less
  blocky than the master.

At most, add a cheap dark deblock for H.264 only, at low priority.

**(3) Flat-area texture loss.** The largest headroom, but no faithful repair on its own.
* **Kept:** encoders keep 20 to 40 % of low-contrast texture, and 1 to 5 % of Nocturne's grain.
* **Headroom:** 0.3 to 0.8 dB, up to 1.5 dB. Tone mapping raises it, because it stretches the shadows.
* **Verdict:** DROP a spatial repair, because it would invent detail. The only faithful source is
  other frames, so feed these numbers to TemporalDetail.

## B. Motion-compensated local tone mapping (HDR->SDR, 100 nit)

Six clips: Sol ×2, Nocturne ×2, Chimera (8-bit PQ, 4000 nit assumed) and a Sol->Nocturne splice.
Whole frames at 1280 wide, 16 frames each.

| candidate | local contrast (source = 1) | restored share | halo | pump (PQ codes) | flicker | grain vs global | codec band / dark MSE vs global | py ms |
|---|---|---|---|---|---|---|---|---|
| global (FL35 baseline) | 0.681 | 0 | 0 | 0 | 0 | 1.000 | 1.00 / 1.00 | 210 |
| dynamic global (DTM stand-in) | 0.703 | 0.04 | 0.0030 | 1.11 | 0.086 | 1.016 | - | 215 |
| contrast recovery 0.3 (libplacebo) | 0.721 | 0.13 | 0.00016 | 0.09 | 0.003 | **1.082** | 1.05 / 1.02 | 290 |
| LTM, guided base (1) | 0.749 | 0.22 | 0.00046 | 0.74 | 0.037 | **1.068** | **1.31 / 1.30** | 564 |
| LTM, half strength | 0.714 | 0.11 | 0.00014 | 0.38 | 0.019 | 1.032 | - | 532 |
| LTM + MC adaptation map (2) | 0.751 | 0.22 | 0.00047 | 0.84 | 0.050 | 1.081 | 1.35 / 1.32 | 1071 |
| LTM, Gaussian base (contrast) | 0.808 | 0.39 | 0.0017 | 0.94 | 0.039 | 1.126 | - | 521 |

**What the numbers show**
* **Contrast:** the global curve loses about 32 % of local contrast. The LTM restores 22 % of that
  and never boosts beyond the source (boost 0). That is 1.7 times what contrast recovery already
  restores. Restoring more needs a base that is not edge-aware, which costs 4 times the halo.
* **Grain:** at full strength the LTM fails the ±5 % grain gate. Half strength passes but restores no
  more than contrast recovery.
* **Codec artefacts:** the LTM amplifies codec banding and dark error by 30 %, which works against A(1).
* **Temporal stability:** the spatial LTM is already stable, with pump under 1 PQ code (the dynamic
  curve reaches 1.1). MC brings nothing:
  * it is slightly worse;
  * it costs 2 times as much;
  * it does not reduce the artefacts.

  Cut resets were all correct.
* **BTB:** the LTM keeps 99.2 % on the test card. This is fixable by masking the gain to in-range
  pixels.
* **Cost estimate:** about 10 full-resolution passes, roughly 0.6 to 0.9 ms at 4K on the 3090 (estimate).

**Verdicts**
* **(2) MC LTM: DROP.**
* **(1) Spatial LTM: DROP in this form.** Its gain over contrast recovery does not pay for the grain
  and artefact costs.

**Side finding:** libplacebo's contrast recovery (0.30 at quality high) amplifies grain by about 8 %.
A grain-split contrast recovery is a cheap next Python candidate.

## Caveats

* **Tone curve:** luma only. The spline matches libplacebo 7.360 line for line.
* **Black level:** 0 nit, from VP's `sdr_black_nits`. libplacebo's inferred black of 0.1 nit would
  lift shadows by about 13 codes.
* **Material:** tos_dark_2stops is itself an H.264 file, and Chimera's peak is assumed. Windows are
  short, rates are set by CRF, and SVT-AV1 and SD were not tested.
* **Pump** includes DIS warp error; eps was tuned on frames outside the scored windows.
* **CPU time:** Part A took about 25 min plus reruns; Part B about 45 min.
