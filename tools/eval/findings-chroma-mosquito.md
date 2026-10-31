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

**Recommendation (this protocol):** keep Catmull-Rom. Superseded for CfL by A2 below.

### A2. Follow-up: GENUINE 4:4:4 truth (verdict change for CfL)

New protocol `chroma444_true`: Netflix Sol Levante (ProRes XQ 4:4:4 12-bit HDR10, animation), two scenes,
each with no codec and with x265 crf22 of the left-sited 4:2:0; Nocturne ProRes 4:2:2 film grain as a
partial check (horizontal only). PQ code values (no linearisation), 1024x768 crops, 6 frames
(`prototypes/chroma444_true/`).

| means over 5 cases | PSNR Cb / Cr | edge Cb / Cr | LPIPS | grain FAIL |
|---|---|---|---|---|
| Catmull-Rom | 52.70 / 51.31 | 43.79 / 44.30 | 0.0159 | - |
| JBU | 51.77 / 50.23 | 44.41 / 43.82 | 0.0201 | 7 |
| residual JBU | 52.68 / 51.36 | 44.00 / 44.48 | 0.0160 | 0 |
| **CfL** | **53.33 / 51.55** | **45.86 / 44.30** | 0.0164 | 1 (Sol x265 Cb grain 0.44 vs 0.49) |
| MV + residual JBU | 52.74 / 51.39 | 44.22 / 44.64 | 0.0161 | 0 |

* With a real 4:4:4 truth, CfL wins: +0.6 / +0.2 dB on average, +2.4 / +1.2 dB on the Sol t150 scene, and
  +2 dB Cb at edges. It loses on Sol t40 without a codec (-0.6 Cb) and slightly on Nocturne (grain, -0.1).
* MV adds only +0.05 to +0.08 dB at about 3x the cost of the spatial methods. JBU still smooths grain.

**Revised:** CfL is **EVALUATE ON GPU** (cost class low). Gate it by local luma/chroma correlation, as
it already does, and add a chroma-grain guard for grainy content. JBU, residual JBU, Krig and MV stay
DROP.

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

### B2. Follow-up: near-edge grain gate (verdict confirmed, stricter)

New harness gate (case key `grain_probe`): the candidate is re-run on its input plus synthetic grain of two
fixed seeds. out_A - out_B is the grain it passed, so structure cancels and the gate works next to edges.
Rows `grain_probe_keep/lag1_{edge,flat}_vs_input_Y` (keep within ±5 %, lag-1 ≤ +0.05). Results on 4
cases (`prototypes/mosquito_edgegrain/`):

| candidate | keep, near edges | keep, flat | lag-1, near edges |
|---|---|---|---|
| MC (the design) | **0.906 (FAIL, 4/4)** | 0.997 | +0.010 |
| spatial | 0.994 (PASS) | 1.000 | 0.000 |
| naive MC | **0.843 (FAIL, 4/4)** | 0.995 | +0.029 |

The MC design removes about 9 % of the near-edge grain. Its per-tile guard measures against the split's
grain layer, not the total, so part of the grain kept in the structure layer is averaged away. The old
flat-tile gates could not see this.

**Recommendation (after B2):** DROP spatial and naive. DROP the MC repair as designed: it fails the
grain rule near edges and gains little. A redesign would need its guard to measure total grain (for
example a two-seed self-check per zone).

CPU time: scored runs 28 min, tuning 8 min (on crops that differ from the scored ones), aborted first
passes about 35 min.
Follow-ups A2/B2: about 20 min of CPU (true 4:4:4 suite 10 min, edge-grain suite 10 min).

### B3. Mosquito headroom by codec (owner-approved addition)

Script `mosquito_headroom.py`; tables in `prototypes/mosquito_headroom/`. 4 masters (Nocturne film grain,
Sol Levante animation, tos4k film/CG, park_joy high detail), 1024x768 windows of 12 frames at 1080p
(downscaled master) and 2160p. Encoders: x264 (deblocking on), x265 (SAO on) and libaom-av1 cpu-used 6
(SVT-AV1 is not in this ffmpeg build). GOP 48. Near-edge mask is `near_edge_zone`. "Artefact share" is
the near-edge high-frequency error where the master is locally smooth, as a share of all error, i.e.
what a perfect mosquito repair could remove.

| raster | encode | Mbps (full raster) | PSNR Y | near-edge HF RMS | near-edge HF / error | artefact share | headroom dB mean / max |
|---|---|---|---|---|---|---|---|
| 2160p | x264 crf23 / crf28 | 46.2 / 20.7 | 39.1 / 36.9 | 11.7 / 14.8 | 6.8 / 5.8 % | 0.4 / 0.2 % | 0.02 / 0.06 |
| 2160p | x265 crf24 / crf30 | 40.1 / 15.5 | 39.5 / 37.0 | 10.3 / 13.1 | 6.4 / 5.2 % | 0.4 / 0.2 % | 0.02 / 0.06 |
| 2160p | AV1 crf30 / crf40 | 34.5 / 16.0 | 39.4 / 37.5 | 9.9 / 12.2 | 6.5 / 5.6 % | 0.4 / 0.3 % | 0.02 / 0.07 |
| 1080p | x264 crf23 / crf28 | 14.2 / 6.6 | 37.6 / 35.2 | 14.7 / 19.2 | 8.4 / 7.4 % | 0.2 / 0.1 % | 0.01 / 0.02 |
| 1080p | x265 crf24 / crf30 | 13.5 / 5.4 | 37.9 / 35.3 | 13.0 / 17.5 | 7.7 / 6.7 % | 0.2 / 0.1 % | 0.01 / 0.02 |
| 1080p | AV1 crf30 / crf40 | 13.1 / 6.1 | 38.1 / 35.9 | 12.0 / 15.6 | 8.0 / 7.3 % | 0.2 / 0.1 % | 0.01 / 0.03 |

(Mbps are means over the 4 windows, which park_joy dominates at 15 to 100 Mbps; the others run 1 to 33 Mbps.)

**Verdict: no worthwhile headroom on any codec, including H.264.** Near-edge high-frequency error is 5
to 8 % of all error, and x264 carries about 15 to 20 % more of it than x265 or AV1 at equal CRF tier.
But almost all of it is detail lost next to edges, not ringing over smooth areas. The removable artefact
share is ≤ 1.5 % (park_joy), which bounds the gain of a perfect repair at 0.07 dB. A mosquito repair
is not justified for HEVC/AV1, and not for H.264 at these rates either.

Caveats: Sol Levante's window has almost no edges above the mask threshold in PQ codes (zone < 0.1 %).
Rates below about 2 Mbps at 1080p are covered only by Nocturne and Sol. SD/broadcast MPEG-2 is
untested. Run time is about 10 min of CPU.
