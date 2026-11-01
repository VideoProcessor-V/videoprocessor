<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TemporalDetail at 4K: ceiling check (one-off, 48 frames, luma, existing td_vectors pipeline, DIS+refine vectors, CPU)

All clips are SDR (no clean HDR 4K master exists under D:\testclips; Meridian HDR is an already-compressed 8-bit H.264). x265 medium, 10-bit, IBBBP, b-adapt 0, scenecut 0, one I (frame 0).
Frames 2..45 scored. Oracle = per 16 px block best single reference and strength chosen WITH the master (upper bounds).

## 1. Base numbers (PSNR vs master; oracle ceiling on B with master-derived vectors; real TD with default gates)

| clip | kb/s | PSNR I / P / B | oracle hb (B, master vec) | oracle hb (B, coded vec) | full blend (B, master vec) | real TD dB on B (master vec / coded vec) |
|---|---|---|---|---|---|---|
| tos4k 3840x1714 24p (film, fast pan) | 13800 | 48.50 / 49.86 / 49.02 | +0.091 | +0.086 | +0.415 | -0.001 / -0.001 (grain ratio 1.001) |
| bos 3840x2160 30p (UVG Bosphorus, 120->30p, mostly static) | 13230 | 44.25 / 44.06 / 44.07 | +0.047 | +0.039 | +0.140 | -0.001 / -0.001 (grain ratio 1.000) |

(The I frame is frame 0, scored on its own; with a pan the I/P/B PSNR are not comparable across content. Bosphorus P is not better than B: static areas are skip-copied.)
1080p study for comparison: ~+0.15 dB oracle on B, real TD ~0. 4K does not change the conclusion; if anything the ceiling is smaller (more pixels per detail, finer scale).

## 2. Nearest PRECEDING I frame (frame 0) as only source, bos (static content), oracle dB on the blocks, master vectors
Distance to I: 1-3 / 4-16 / 17-47 frames. hb = high-band transfer, base = low-frequency base carry-over.

| class (area share) | hb I raw | base I raw | hb I valid | base I valid(chain B) | hb P nearest | base P nearest |
|---|---|---|---|---|---|---|
| all | +0.05/+0.07/+0.08 | +0.20/+0.22/+0.14 | see json | +0.21/+0.24/+0.19 | +0.03/+0.09/+0.07 | +0.10/+0.60/+0.26 |
| static (69%) | +0.00/+0.08/+0.07 | +0.00/+0.24/+0.14 | | | +0.05/+0.11/+0.08 | +0.22/+0.76/+0.33 |
| static and dark < 50 nit (54%) | +0.00/+0.08/+0.07 | +0.00/+0.23/+0.11 | +0.00/+0.08/+0.11 | +0.00/+0.26/+0.19 | +0.05/+0.11/+0.08 | +0.22/+0.77/+0.32 |
| static smooth (0.1% only) | +0.00/+0.37/+0.32 | +0.00/+1.23/+0.85 | +0.00/+0.46/+0.36 | +0.00/+2.74/+2.31 | +0.12/+0.31/+0.32 | +0.97/+1.97/+1.18 |

At distance 1-3 the gain is exactly zero in static areas: the encoder skip-copies the I frame there, so the B frame already equals the I-frame content. Share of the I-vs-B PSNR gap sitting in static+dark blocks: 62% of the area-weighted gap (area 54%): no concentration in specific classes; the gap is spread (top 10% of blocks hold 65% of the I-B gap, 30% hold 110%, so about a third of blocks have negative gap).

## 3. Validity (chained from the I frame, bos) and live B veto
Valid fraction (all area) at 1-3 / 4-16 / 17-47: chain A (fwd-bwd + residual) 89 / 51 / 27 %; chain B with P checkpoints 58 / 40 / 22 %; chain B without the live B veto 58 / 40 / 23 %.
Static smooth: A 100 / 28 / 7 %. False invalidation (invalidated though the memory matched the master within 12 codes): 86 / 74 / 64 % of invalidated pixels (A): the gates are far too strict at 4K. Ghosting (valid but wrong >12 codes): A 0.7 / 2.3 / 1.5 %, B with veto 0.3 / 0.9 / 1.3 %, B without veto 0.3 / 1.4 / 6.3 %: the live B veto cuts ghosting at long distance by about 5x.
tos4k (pan, no static blocks): valid 70 / 45 / 13 %, oracle hb on valid pixels +0.18 / +0.20 / +0.41 dB (rises with distance because only the best pixels are left).

## 4. Banding / base replacement (bos, static smooth and static dark, chain A valid pixels, grain kept, sigma-4 base replaced by the I frame's)
Band error = RMS of the (sigma 2 - sigma 10) band of (decoded - master), codes. Distance 1-3: unchanged (B == I there). 4-16: static smooth 1.37 -> 1.43, mse 8.4 -> 9.9; static dark 2.30 -> 2.95, mse 59 -> 70. 17-47: worse (static dark 1.29 -> 2.35). Plain replacement of the base by the aged I-frame base INCREASES band error and mse; only the oracle (per block strength chosen with the master) gains. Banding of decoded static-smooth blocks by frame type: P 1.056, B 1.040 (B-frames do not band more than P).

## Not measured (scope; needs fuller harness)
Motion-speed/size fade fit, lighting/focus tracking, pumping spectra and crossfade lengths, motion-segment regions, NVOF seed (DIS+refine only), the P-vs-B concentration beyond the table above, other clips for held-out validation. Raw results: D:\codex\td-real-vectors-out\4k (res_*.json, bos_res_*.json, summ.py output txt files, region4k.py).
