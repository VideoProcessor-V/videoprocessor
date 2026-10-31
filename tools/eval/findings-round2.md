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
A grain-split contrast recovery is tested in C1.

## C. Follow-ups (owner requests)

The domains are as follows. **pre-TM** means the signal domain: PQ codes for HDR, gamma codes for SDR. **post-TM** means
the SDR output codes; there, lc is the output's PQ against the source's PQ. All runs are HDR at
1080p with x265-10 CRF 30 unless noted.

**C1. Grain-safe contrast recovery** (`cr_grainsafe_eval.py`, 6 clips; codec rows use 4 masters × 2 encodes)

The grain is split off, libplacebo's recovery runs on the grain-free picture, and the grain is added
back at the curve slope.

| post-TM | restored share | halo | grain vs plain (worst clip) | codec band / dark MSE |
|---|---|---|---|---|
| libplacebo CR 0.30 as-is | 0.13 | 0.00016 | 1.082 (1.131) **fail** | 1.05 / 1.02 |
| grain-safe 0.15 | 0.05 | 0.00005 | 1.021 (1.035) pass | 1.00 / 1.00 |
| grain-safe 0.30 | 0.09 | 0.00014 | 1.042 (1.072) pass on mean, 1 clip over | 1.00 / 1.00 |
| grain-safe 0.45 | 0.13 | 0.00024 | 1.065 (1.111) fail | 1.00 / 1.00 |

* At the same restoration as CR 0.3 (0.13), the grain-safe version needs strength 0.45 and still
  fails the grain gate.
* It never amplifies codec banding or dark error (CR as-is: +5 %).
* Lag-1 is unchanged for every strength.
* **Verdict: EVALUATE ON GPU** as an either/or option to libplacebo CR, at strength ≤ 0.25. The cost
  is one grain split (two passes) added to CR.

**C2. Is banding caused by stripped texture?** (`texture_dither.py`, 32-px smooth tiles, all Part A cases)

* **HDR at 2160p: yes.** The step error sits where grain retention is lowest (Spearman ρ −0.36 to
  −0.66). The lowest-retention quartile of tiles carries 24 to 35 % of the step energy.
* **SDR and HDR at 1080p: weak.** ρ runs from −0.17 to +0.14 (HDR→SDR 1080p up to +0.41).
* **What re-injecting the master's fine band does** (the TemporalDetail upper bound): it removes only
  4 to 9 % of the step energy, but masks 25 to 33 % (SDR) and 45 to 76 % (HDR) of the step
  visibility (masking proxy).
* **Conclusion:** texture recovery hides banding rather than repairing it, and strongly so in HDR.

**C3. Stacking in pipeline order** (`deband_stack.py`, 4 masters per context, 8 frames)

The order is texture oracle → deband → tone stage. Two deband models were used:
* **VP deband:** a Python model of `repair/deband` at low-bitrate strength.
* **Bill's libplacebo deband:** default settings. Bill turns it off for HDR, so the HDR rows are a
  what-if.

The plain curve on the master keeps lc 0.773, so tone mapping takes 23 % of local contrast. The
encode lowers it to 0.546, a further 0.23.

| post-TM HDR→SDR | PSNR | LPIPS | step visibility | lc | halo |
|---|---|---|---|---|---|
| decode, plain curve | 37.3 | 0.184 | 1.72 | 0.546 | 0 |
| + VP deband / + libplacebo deband | 37.3 / 37.3 | 0.172 / 0.172 | 1.40 / 1.28 | 0.550 / 0.556 | 0 |
| + grain-safe CR 0.30 / + pre-emphasis | 36.8 / 36.9 | 0.188 / 0.188 | 1.70 / **1.99** | 0.557 / 0.595 | 0.00006 / 0.0004 |
| texture oracle, plain | **41.9** | **0.042** | 0.70 | **0.688** | 0 |
| oracle + grain-safe CR 0.30 | 41.5 | 0.043 | 0.69 | 0.703 | 0.00005 |
| oracle + pre-emphasis | 40.9 | 0.048 | 0.84 | 0.731 | 0.0003 |
| oracle + libplacebo deband, plain | 41.7 | 0.042 | 0.60 | 0.691 | 0 |

**Findings:**
* **Encoder vs tone curve:** texture recovery brings back about 62 % of the local contrast the
  encoder took (0.546 → 0.688). That gain is about 10 times what any tone stage adds.
* **Stacking:** tone stages stack roughly additively on top of texture: CR +0.015, pre-emphasis
  +0.043. They do not interfere on grain.
* **CR needed once texture is restored:** about 0 to 0.15. The extra contrast is small, and CR costs
  PSNR.
* **Pre-emphasis:** it gives the most contrast, but raises banding visibility 16 % (its band mask
  barely helps: 1.99 vs 2.08 unmasked) and halo about 7 times. Its grain is 1.04 (pass). Verdict:
  not worth it.
* **Deband vs texture (HDR):** texture recovery masks more banding than either deband removes (0.70
  vs 1.28 to 1.40). Deband on top adds little (0.60 to 0.65).
* **Deband in SDR (8-bit masters, little grain):** libplacebo deband helps more than the oracle (0.87
  vs 1.04).
* **What deband removes today** (projection onto the master's fine band): the VP model removes about
  1 % of the master's texture; libplacebo removes 7 % (SDR) or 1 % (HDR). Both add dither noise
  (non-master fine energy +0.24 SDR, +0.07 HDR).
* **VP model caveat:** at the modelled 8-bit threshold (12 codes) the VP model raised SDR coarse
  error 49 %. Verify against the real module.
* **Best combination:**
  * HDR→SDR: texture recovery, then grain-safe CR ≤ 0.15, with deband optional and light.
  * SDR: texture recovery plus a light deband.

**C4. Ringing repair in the stack** (`ringing_stack.py`)

The spatial mosquito repair was tried early (on the decode), late (after the tone stage) and in both
places.
* **Effect:** none in any placement. Near-edge ringing changes by ≤ 0.2 % and lc is unchanged. Its
  grain guard reverts it.
* **Do later stages amplify ringing?** CR raises it 10 %; pre-emphasis does not.
* **Verdict: it earns no placement.** Cost: Python 3.3 s/frame (two bilaterals). It stays DROP.

**C5. External vectors.** `vector_source.py`. MC-LTM (`ltm_eval.py --vectors`) and chroma_mv / CfL+MV
(`VP_EVAL_VECTORS`) can now take the camera model's fields; see the README. Missing fields raise.

**C6. Adaptive ringing repair** (`adaptive_mosquito.py`, `ringing_usecases.py`)

The repair is grain-safe and works region by region. Its strength comes from the ringing it measures
near each edge, the edge contrast, the brightness after tone mapping, and the bitrate class; it is
zero where no ringing is found.

| case (domain the viewer sees) | ringing removed | ringing before (10-bit rms) | PSNR gain | near-edge grain kept |
|---|---|---|---|---|
| tos4k text-like, x264 CRF 28 / 35 (SDR) | 8 / 6 % | 4.4 / 4.8 | +0.01 | 0.97 / 0.98 |
| Sintel sky, x264 CRF 28 / 35 | 53 / 59 % | 1.3 / 1.7 | +0.005 | 0.945 / 0.951 (borderline) |
| Sol dark HDR→SDR, early / late / both | 24 / 36 / 43 % | 2.7 | ±0.02 | 0.953 |
| Nocturne (grain) | 0 to 1 % (correctly does nothing) | 7.5 | 0 | 0.99 |
| (4) synthetic credits, CRF 28 / 35 / 40 | 6 / 7 / 4 % | 2.8 / 3.2 / 5.3 | ≤ +0.007 | 0.95 |
| (1)(2) ×2 upscale + unsharp k 0 / 0.5 / 1 | 2 to 10 %, the same at every k | rises 1.5× at k = 1 | ≤ +0.007 | - |
| (5) 720p rung CRF 38, shown ×2 | 5 to 7 % | 4.1 to 7.6 | ≤ +0.004 | 0.93 (credits: **fail**) |
| (3) chroma | none found in 5 of 6 planes; Sol Cr 66 % | 1.3 | +0.03 | 0.94 (**fail**) |

**Findings:**
* **Where the ringing is visible, the repair barely touches it.** That is text, credits and
  low-rung rungs, around 1 eight-bit step rms.
* **Where it removes a lot, there is little to remove.** Clean animation (Sol, Sintel) sits below
  1 eight-bit step.
* **Sharpening:** a pre-repair does not let it run sharper. The ringing it removes is worth less than
  0.1 of unsharp gain.
* **Lower-rung cases:** the grain gate is borderline or failing there.
* **Untested:** VP's actual kernel-inverse / CNN / SuperRes stages could not run in Python. The
  unsharp step is a linear stand-in.

**Verdict:** the adaptive version earns no place, not even as a low-bitrate auto mode. DROP stands.
Re-test only if a GPU run of the real sharpening stages shows ringing they create themselves.

## Caveats

* **Tone curve:** luma only. The spline matches libplacebo 7.360 line for line.
* **Black level:** 0 nit, from VP's `sdr_black_nits`. libplacebo's inferred black of 0.1 nit would
  lift shadows by about 13 codes.
* **Material:** tos_dark_2stops is itself an H.264 file, and Chimera's peak is assumed. Windows are
  short, rates are set by CRF, and SVT-AV1 and SD were not tested.
* **Pump** includes DIS warp error; eps was tuned on frames outside the scored windows.
* **CPU time:** Part A took about 25 min plus reruns; Part B about 45 min.
