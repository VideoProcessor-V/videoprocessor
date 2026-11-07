<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# tools/eval/motion_bench

Offline scoring and unattended-run tooling for the motion/camera switches. Python needs numpy only
(`python -I -c "import numpy"`; otherwise use `D:\hfr\venv\Scripts\python.exe`). Nothing here needs the GPU
except `run_motion_bench.ps1 -Run`.

| file | purpose |
|---|---|
| `score_flow.py` | EPE / outlier / sub-pixel / occlusion / temporal scoring of a predicted `.flo` against a ground truth |
| `run_motion_bench.ps1` | runner skeleton; dry run unless `-Run` |
| `summarize_bench.py` | merges per-run `VP_MOTION_BENCH` csv files into `summary.csv` + `summary.md` |
| `offline_adapter.py` | no-camera vs camera-guided comparison through `tools/camera_eval match`; optional RAFT reference |
| `synth_gt.py` | synthetic sequences with exact flow, occlusion band, labels, small-fast-object masks (cv2 + numpy) |
| `offline_matrix.py` | offline matrix stages `prep` / `gpu` / `cpu` / `report` (NVOF seed, engine, engine + camera, RAFT) |
| `run_offline_matrix.ps1` | driver: GPU lock, mains / PRIORITY checks, one sequence per GPU chunk, writes `<DataRoot>\results\summary.md/csv` |

## VP_MOTION_BENCH csv (one row per pair)
Header: `pair,seq,switch_mask,d,confidence,reason,narrow,full,escalated,skipped,governor_state,gpu_ms`

Written by VP when the environment variable `VP_MOTION_BENCH` names a csv path. The runner names files
`<clip>__<config>.csv`. Summary assumptions (adjust `summarize_bench.py` if VP defines them differently): `narrow`,
`full`, `skipped` are block counts per pair, `escalated` counts narrow blocks that escalated to the full search; the
percentages divide by `narrow+full+skipped` summed over the run.

## summary.csv
Header: `clip,config,pairs,narrow_pct,escalated_pct,skipped_pct,gpu_ms_mean,epe_all,epe_camera,epe_object`

`config` is the matrix entry (`off`, `diagnostics`, a switch name, `all`). The epe columns come from an optional
`<clip>__<config>.scores.csv` (score_flow output) and are empty when absent. `summary.md` is the same table.

## score_flow.py
```
python -I score_flow.py --pred A.flo --gt B.flo [--camera-labels labels.npy|.csv] [--object-mask mask.png|.npy]
                        [--small-mask small.npy|.png] [--prev-pred P.flo [--prev-gt G.flo]] --out scores.csv
python -I score_flow.py --self-test
```
`--small-mask`: pixels of small fast objects (<= 32 px, >= 20 px/frame); adds the `smallfast` metric rows. `run_offline_matrix.ps1` treats `BatteryStatus` 11 (partially charged) as NOT mains.
`.flo`: Middlebury (float32 magic 202021.25, int32 w,h, interleaved float32 u,v). Ground-truth entries >= 1e9 are unknown and skipped.
Labels: (H,W) or a block grid that divides (H,W); 1 = camera, 2 = object, other = ignored. Mask: nonzero = occlusion-boundary pixel
(`.png`: 8-bit, non-interlaced only).

Output `scores.csv` is `metric,value` rows: `all_n/_epe/_outlier_pct` (outlier = EPE > 3 px), the same for `camera_*`, `object_*`
(only with labels), `subpixel_*` (|gt| < 2 px), `occlusion_boundary_*` (only with a mask), and `tc_pred`, `tc_gt`, `tc_excess`
(only with `--prev-pred`, `--prev-gt`). Temporal metric: mean |cur(x) - prev(x - prev(x))|, i.e. the previous flow warped by itself
(bilinear, border-clamped) against the current flow; `tc_excess = tc_pred - tc_gt`.

Run-twice hash: every scoring is computed twice from the files; the output bytes must match, the SHA-256 is printed
(`sha256=... (run twice, identical)`), and a mismatch exits 1.

Self-test (temp dir, exits 0/1): constant 1 px offset (EPE exactly 1.0); two-region case (camera EPE 5, object EPE 0.5, all 2.75, outliers
100 % / 0 % / 50 %, sub-pixel EPE 0.5, occlusion strip EPE 0.5); temporal case (0.5 / 0 / 0.5); `.flo` round trip.

## run_motion_bench.ps1
```
.\run_motion_bench.ps1 -Clips D:\testclips\x.mkv -Matrix off,diagnostics,vector_narrow_window,all -OutDir D:\scratch\bench   # dry run
.\run_motion_bench.ps1 ... -Run                                                                                             # real
```
Matrix names: `off`, `diagnostics`, each of vector_model_global, vector_guided_candidate, vector_narrow_window, vector_e1_skip,
vector_static_gate, vector_cut_skip, vector_fade_normalised, vector_flash_bridge, vector_temporal_seed, vector_tiny_mover_roi, and `all`.
Limits of the skeleton: the per-config overrides are only printed (dry run) or written to `<csv>.overrides.txt`; it does NOT edit the
deployed config, so the operator (or a later revision) must stage the matching config before each run. VP's launch arguments and a
clean exit are assumed (it stops the launched PID after `-ClipSeconds`). Lock: `mkdir D:\codex\GPU-QUIET` (max 15 min per chunk, then
released and retaken), refuses while `D:\codex\build-slots\PRIORITY` exists, released in `finally`.

## offline_adapter.py and camera_eval limits
`camera_eval match` takes coded YUV + input-vector dir + reference-flow dir, runs the CPU ReferenceMatcher without and with the camera
plan in a single invocation, and writes only a per-pair CSV of block EPE statistics (all / camera / other blocks). It does **not**
write `.flo` files and does not take two arbitrary frames: it needs the YUV sequence plus vectors. So the adapter cannot produce
`pred_off.flo` / `pred_on.flo` today; it runs `match` and writes `match_summary.csv` (mean over pairs of every `*_mean` column).
`camera_eval match --dump-flo DIR` now writes `DIR/pair_%06d/pred_off.flo` and `pred_on.flo` (dense, source px, block vectors
expanded; CSV unchanged); `--score-flo` scores such files (`--pred-off`, `--pred-on`, `--gt`).

`--raft --frame0 a.png --frame1 b.png --out raft.flo` writes a torchvision `raft_large` flow (run with `D:\hfr\venv` python). Imports are
guarded, so the file imports without torch. It does not download: the pretrained weights must already be in the torch cache.

## Offline matrix (R1-MB)
```
.
.\run_offline_matrix.ps1 [-DataRoot D:\codex\videoprocessor\r1cam-bench-data] [-Stages synth,prep,gpu,cpu,report] [-Only name,...] [-DryRun]
1cam-bench-data] [-Stages synth,prep,gpu,cpu,report] [-Only name,...] [-DryRun]
```
Contenders on every sequence: `nvof_seed` (tools/td-cli `nvof_dump`, forward vectors, 4 px cells), `engine` (camera_eval `match`, CPU
ReferenceMatcher, plan off), `engine_camera` (plan on; the plan is built causally from the camera fit of the NVOF vectors of the previous
pair), `raft` (torchvision raft_large, BSD, cached weights only, luma replicated to 3 channels, 12 iterations). All contenders see only the
luma of the same 10-bit YUV. Sets: synthetic (`synth_gt.py`: pans integer / sub-pixel / slow, zoom, rotation, small fast objects <= 32 px at
20-28 px/frame, large objects with occlusions, grain, zoom + rotation + objects; 1280x720, 12 frames, pairs 2..11) and two real clips
(Tears of Steel pan from `td-real-vectors-out`, Cosmos Laundromat long take; 32 frames). On real clips RAFT is the reference, not ground
truth: scores there are "agreement with RAFT", camera labels come from camera_eval `fit` on the RAFT fields, the occlusion band from RAFT
forward/backward inconsistency. Synthetic ground truth is checked by warping frame t onto t-1 (`gt_warp_mae` in `meta.json`, ~0.002-0.003
in 0..1 gray, i.e. interpolation error only). Pooling: pixel counts weight EPE and outlier percentages; TC is the mean over pairs. Cost proxy
= matcher candidate evaluations and wall ms, guided (plan on) against full (plan off), plus escalated / narrow blocks.
Run-twice: `score_flow.py` scores every pair twice and requires identical bytes; the matcher dump of `pan_small_fast` is run twice and
hashed (`determinism.txt`). Dense predictions are deleted after scoring unless `--keep-flo`.

### Ground-truth data licences
- **MPI Sintel** (http://sintel.is.tue.mpg.de/license, read 2026-11-07): single-user, non-exclusive, non-transferable, free right for
  non-commercial scientific research, non-commercial education and non-commercial artistic projects; commercial use, use in a commercial
  product or service, and training for commercial / military / surveillance uses are prohibited; no copying, sharing or redistribution
  without written permission (one archive copy allowed); the two Butler / Wulff ECCV 2012 papers must be cited in publications; AS IS.
  "Testing" or "benchmarking a software product" is not named. **Decision: not downloaded** (unclear whether benchmarking a software
  project falls under non-commercial research; download requires accepting the terms, which needs the owner). Needs: owner decision.
  Where: MPI-Sintel-training_images.zip (1.7 GB) + MPI-Sintel-training_extras.zip (3.1 GB, flow + occlusions).
- **KITTI**: not downloaded (owner OK required, non-commercial licence).
- Real clips: Tears of Steel (CC BY 3.0, Blender Foundation) and Cosmos Laundromat (CC BY 4.0), see D:	estclips\README.md.
