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
                  [--start 2] [--count 40] [--phase1] [--dump-flo DIR] --out F.csv
```
`fit` output: a `.camf` (layout in `camera_eval.cpp`) plus an optional per-pair CSV (valid, type, reason, confidence,
all signals, h[9], label shares). `match`: for each pair t >= start the fit of pair t-1 (causal, one stateful Fitter
over the *input* vectors, e.g. the coded NVOF) builds a measurement plan; the reference matcher then runs on the
coded luma of frames t-1, t (YUV 4:2:0 planar, 16-bit LE samples holding 10-bit values; luma box-downscaled 2x and
divided by 2^(bitdepth-8), the matcher's code scale) without and with the plan.

## Metrics
- **usable**: share of pairs with `valid` and confidence >= 0.5.
- **WRONG-confident** (synthetic): a usable fit on a scene with no dominant camera motion, or with max picture-corner
  error > max(1 px, 10 % of the true corner displacement).
- **WRONG-confident** (real, primary, region metric): truth pairs are those where the master DIS and master NVOF fits are
  both valid, their common camera labels cover >= 50 blocks and the p90 block difference between the two fits over those
  labels is <= 0.5 px; truth field = mean of the two fits. A confident fit is WRONG when its p90 block error over the
  true-camera blocks exceeds max(1 px, 10 % of the p90 truth magnitude there). The same test over the fit's own camera
  labels is reported too. The corner-error version (max corner error, "extrapolated corner error, includes non-camera
  regions") is kept as a secondary column: it mostly measures affine / homography extrapolation into corners that are not
  camera (runners in crowd), where the model is not used.
- **corner error**: max over the four picture corners of |fitted - true displacement| (source px); median / p90.
- **label precision / recall**: camera-labelled blocks (label 1) against the true camera mask. **jitter**: std of the
  pair-to-pair change of the corner error.
- **corner truth** (real, secondary): pairs where the DIS and NVOF fits of the master clip agree (corner difference within
  max(0.5 px, 5 %)); truth = their mean. **master-camera-block error**: mean |model displacement - truth displacement|
  over blocks the master DIS fit labels camera, median over judged pairs.
- **match**: `evals` = matcher candidate evaluations (guided / full). EPE = endpoint error in source px of the
  matcher's block vector (matcher px / 0.5) against the block mean of the reference flow (blocks with reference trust 0
  skipped); reported for all blocks, reference-camera blocks (the fit of the reference field labels 1) and the rest,
  as mean, median, share > 1 px and share > 3 px. Direction check: with the plan off, the median EPE on a clean film
  pan (tos_master) is well under 1 px (about 0.4), which confirms the from/to convention (frame_b(p + F(p)) ~ frame_a(p)).

## Two-layer comparison (K10a)
`run_eval.py --vs two-layer` (default) fits everything with `Config::twoLayer` off ("new") and on ("two"), side by side.
Real-clip truth is pinned to the default (twoLayer off) master DIS + NVOF fits for both columns, so the metric is not
self-referential. `camera_eval fit --two-layer` switches the layer on (`--tl-ring/--tl-swap/--tl-veto/--tl-floor/--tl-comp`
override the constants for tuning; `run_eval.py --tl-args "..."` passes them through). The CSV gains `fitUs` (microseconds
per Fit call), `layerFlags`, `layerRisk`, `border`, `continuity`, `haveObject`, `objId`, `objBlocks`, `objH2`, `objH5`; the
summary reports a per-clip table and the fit time (p50 / p95 / max). Synthetic foreground scenarios (camera = background):
`obj6040_pan`, `vehicle_pan`, `tracked_subject`; `water_boat` has no camera truth (any usable fit is WRONG).

`match --dump-flo DIR` also writes, per judged pair t, `DIR/pair_%06d/pred_off.flo` and `pred_on.flo` (Middlebury .flo, full picture, source px,
frame t-1 -> t as in the convention above; each matcher block vector is expanded over its `block` x `block` source pixels, the strip beyond the last
whole block repeats the edge block). The CSV is unchanged. `DIR` must exist.

## Noise tier and dynamic-texture detector (R1-GATES)
- **Noise tier**: the region metric is reported at two error floors. `WRONG@true-cam` uses max(1 px, 10 %); `>1.5 px@true-cam` uses
  max(1.5 px, 10 %) (`xcheck_real.NOISE_TIER_PX`). A pair that is wrong only between 1 and 1.5 px is a near-threshold miss (vector noise
  against a 1 px metric), not a foreground lock. `run_eval.py` adds the column to the per-sequence and per-clip tables, a summary count,
  and lists the pairs beyond 1.5 px (sequence/source/pair: p90 error) per configuration.
- **Dynamic-texture detector** (`Config::dynTexture`, `DynamicTexture.h/.cpp`): `camera_eval fit --dyn-texture [--dt-share F --dt-temporal F
  --dt-spatial F --dt-agree F]`; `run_eval.py --all-args "--dyn-texture"` turns it on for both configurations. The CSV gains `dynShare`,
  `dynTemporal`, `dynSpatial`, `dynField` (the raw-vector temporal incoherence) and `verdictFlags` (bit 1 = Ambiguous because of a dynamic
  texture). The summary counts the real pairs it turned Ambiguous.
