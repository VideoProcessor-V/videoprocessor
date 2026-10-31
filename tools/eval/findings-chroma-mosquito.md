<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Findings: chroma and mosquito Python prototypes (2026-10-31)

Offline Python reference runs on the laptop CPU, scored by the harness. Tables are in `prototypes/*/`
(read `summary.md` first). Material: 9 cases per family (G18 4K, tos4k, sintel, breathing, meridian dark;
chroma adds a tos4k FFV1 case with no codec, mosquito a crf36 stress encode), all centre crops. The
`D:\testclips\eval-2026-10-31` clips have no masters, so they could not be scored. DIS flow stands in for
VP's vectors.

## A. Chroma (default: Catmull-Rom, what VP gives libplacebo when `chroma_upscaler` is absent)

| candidate (means over 9 cases) | PSNR Cb / Cr | edge-band Cr | LPIPS | grain_rms Cb | verdict |
|---|---|---|---|---|---|
| Catmull-Rom (default) | 47.33 / 47.52 | 44.88 | 0.0291 | 0.625 | - |
| JBU | 46.91 / 46.70 | 43.56 | 0.0328 | 0.331 (13 FAIL) | **DROP** |
| residual JBU (Catmull-Rom + luma-guided part) | 46.97 / 46.84 | 43.10 | 0.0291 | 0.637 | **DROP** |
| KrigBilateral-style | 46.92 / 46.84 | 43.85 | 0.0316 | 0.412 (8 FAIL) | **DROP** |
| chroma-from-luma (guided filter) | 47.20 / 46.95 | 43.95 | 0.0298 | 0.595 | **DROP** (Cb wins 5 of 9 cases, up to +0.5; Cr loses) |
| MV temporal + residual JBU | 47.07 / 47.00 | 43.35 | 0.0291 | 0.609 | **DROP for now** |

* None beats Catmull-Rom on average PSNR, and all lose in the luma-edge band (where bleeding should
  improve). Residual JBU, CfL and MV gain only +0.002 in SSIM Cb.
* JBU and Krig gain only on noisy encodes, by smoothing codec chroma noise. That halves chroma grain
  (FAIL), which breaks the grain rule.
* Clean FFV1: Catmull-Rom 54.8 dB, guided methods 50 to 53.4 dB. On sintel the residual forms overshoot
  (-6 dB at the edges).
* MV: flow confidence is 0.48 to 0.96, but there is no gain over its spatial base, so neighbour chroma
  carries no recoverable detail here.
* **Caveat:** the truth is the master's band-limited 4:2:0 chroma, which favours Catmull-Rom. A true
  4:4:4 or RGB master is still missing.
* GPU cost classes: JBU, residual JBU and CfL **low**; Krig **medium** (a 13x13 solve per cell); MV
  **medium** plus vectors.

**Recommendation:** keep Catmull-Rom. EVALUATE ON GPU: none now. Re-test CfL (cheap) and MV once a
true-4:4:4 truth set exists.

## B. Mosquito repair (default: none)

The design: grain split with exact add-back, ringing zones near strong edges, MC filtering of the
structure layer only, BTB/WTW untouched, and a per-tile grain guard (≤ 5 % in-zone grain removal).

| candidate | ringing energy, smooth near-edge band (vs input = 1) | PSNR band | edge keep | harness gates |
|---|---|---|---|---|
| MC (the design) | 0.976 (best 0.949, tos4k) | +0.04 dB | 0.9985 | all grain / BTB / WTW vs-input PASS; 1 FLAG (sintel band -0.07 dB) |
| spatial only | 0.998 | +0.00 | 1.0000 | PASS (the guard reverts most of it: 17 to 127 tiles/frame) |
| naive MC, no grain split (contrast) | 0.966 | +0.04 | 0.9984 | PASS |

* The gain is small (≤ 5 % less ringing, ≤ +0.13 dB band, stress encode 0.96). Ringing at these rates
  is low and largely temporally coherent.
* The first run failed WTW vs input (0.987), because averaging pulled super-whites inside. Fixed:
  pixels beyond the limits are untouched.
* **Harness gap:** the naive variant passes every grain gate, because the gates use flat tiles far from
  edges; only the guard statistics show in-zone loss. Add a near-edge grain gate.
* Cost class **medium** (bilateral, 2 warps, validation, plus vectors).

**Recommendation:** DROP spatial and naive. DROP FOR NOW the MC repair: it is rule-compliant but gives
no material gain. Revisit with low-bitrate SD or broadcast material.

CPU time: scored runs 28 min, tuning 8 min (on crops that differ from the scored ones), aborted first
passes about 35 min.
