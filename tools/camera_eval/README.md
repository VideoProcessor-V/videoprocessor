<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# tools/camera_eval — offline evaluation of the global camera model

CPU-only tool for `src/VP-V/modules/motion/estimation/global_model` (not in the solution, no GPU). It measures how
often the camera fitter's confidence is usable, how wrong it is when it is confident, and what the measurement plan
(narrow window + guided candidate) saves and costs in the reference matcher, always new defaults vs the phase-1
configuration (`--phase1`: inlier-share ramp 0.4/0.9, no temporal noise floor, no verdict gate).

## Build and run
```
tools\camera_eval\build_camera_eval.bat                       -> tools\camera_eval\bin\camera_eval.exe (gitignored)
python -I tools\camera_eval\run_eval.py --exe tools\camera_eval\bin\camera_eval.exe --out <scratch dir> [--match]
```
`run_eval.py` builds nothing. It generates the synthetic fields (`synth_fields.py`, 9 scenarios with exact camera
truth) under `--synth-root` (default `<out>/synth_fields`), fits them and every real sequence found under
`--real-root` (default `D:/codex/td-real-vectors-out`: `vec/<clip>_<tier>/<disref|nvof>`, dims and fps from
`data/data.json`), under both configurations, and writes `<out>/summary.md`. Keep `--out` outside the repo.
Single steps: `camera_eval fit ...`, `score_synth.py`, `xcheck_real.py`, and `camera_eval match ...`:
```
camera_eval fit   --vec DIR --frames N --w W --h H --block 16 --fps F --out F.camf [--csv F.csv] [--phase1]
camera_eval match --yuv CODED.yuv --w W --h H --bitdepth 10 --frames N --vec DIR --ref DIR --fps F
                  [--start 2] [--count 40] [--phase1] --out F.csv
```
`fit` output: a `.camf` (layout in `camera_eval.cpp`) plus an optional per-pair CSV (valid, type, reason, confidence,
all signals, h[9], label shares). `match`: for each pair t >= start the fit of pair t-1 (causal, one stateful Fitter
over the *input* vectors, e.g. the coded NVOF) builds a measurement plan; the reference matcher then runs on the
coded luma of frames t-1, t (YUV 4:2:0 planar, 16-bit LE samples holding 10-bit values; luma box-downscaled 2x and
divided by 2^(bitdepth-8), the matcher's code scale) without and with the plan.

## Metrics
- **usable**: share of pairs with `valid` and confidence >= 0.5.
- **WRONG-confident** (synthetic): a usable fit on a scene with no dominant camera motion, or with max picture-corner
  error > max(1 px, 10 % of the true corner displacement). (Real): the same error rule against the master truth.
- **corner error**: max over the four picture corners of |fitted - true displacement| (source px); median / p90.
- **label precision / recall**: camera-labelled blocks (label 1) against the true camera mask. **jitter**: std of the
  pair-to-pair change of the corner error.
- **master truth** (real): pairs where the DIS and NVOF fits of the master clip agree (corner difference within
  max(0.5 px, 5 %)); truth = their mean. **master-camera-block error**: mean |model displacement - truth displacement|
  over blocks the master DIS fit labels camera, median over judged pairs.
- **match**: `evals` = matcher candidate evaluations (guided / full). EPE = endpoint error in source px of the
  matcher's block vector (matcher px / 0.5) against the block mean of the reference flow (blocks with reference trust 0
  skipped); reported for all blocks, reference-camera blocks (the fit of the reference field labels 1) and the rest,
  as mean, median, share > 1 px and share > 3 px. Direction check: with the plan off, the median EPE on a clean film
  pan (tos_master) is well under 1 px (about 0.4), which confirms the from/to convention (frame_b(p + F(p)) ~ frame_a(p)).
