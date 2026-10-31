<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Scaling sweeps (`tools/scaling_sweep/`)

GPU-ready sweeps of VP's scaling chain through `VpScaleRender` (the real renderer DLL, one cfg per
variant), scored by the common eval harness (`tools/eval` on branch `test/eval-harness`, protocol
`same`). The scripts were only dry-run and unit-tested on the CPU; nothing here has rendered yet.

| sweep | question | grid (per geometry) |
|---|---|---|
| 1 downscale | best hybrid `adaptive_w_base` / faithful branch | 4K master -> 2560x1440 and 1920x1080; `adaptive_w_base` {0,.1,.2,.3,.5,.7,1} x `adaptive_faithful` {auto,ortho}; controls `ctrl_lanczos` (adaptive off) and `ssim_only` (adaptive off + SSimDownscaler) = 16 variants |
| 2 upscale | 8b grid, direct vs 2-stage route | sources 1920x1080, 2560x1440 (1:1 control), 1280x720 -> 2560x1440, plus 1920x1080 in a 2560x1600 window; `adaptive_upscale_w_base` {0,.25,.5,.75,1} x `superres_scale` {on,off} x `upscale_light` {linear,gamma}, `d_*` direct and `r_*` route (stage 1 = same 8b to 2x capped at 3840x2160, captured; stage 2 = hybrid downscale at `--down-w-base`); control `ctrl_ewa` (+ `r_ctrl_ewa`) = 42 variants |
| 3 one-axis | one-axis / anamorphic upscale | 2560x1080 (V only), 1920x1440 (H only), 2048x1080 (two axes, V > H) -> 2560x1440; w_base {0,1} x superres {on,off} + `ctrl_ewa` = 5 variants |
| 4 motion | motion-adapted vs optimal | 24-frame sequences (`picture_seq`, 24/1, `motion vectors`, `capture per_frame`, settle 3, `require_motion 0.5`), downscale form (4K -> 2560x1440) and upscale form (1080p -> 2560x1440); `opt` (adaptive_motion off at `--best-w-base`) vs `adaptive_motion on` x speed_lo {.05,.1,.2} x speed_hi {.3,.6} x floor {0,.5} x smooth_ms 250 (lo >= hi skipped) = 13 variants |
| 5 camera model | on/off | `--camera-model-key KEY` doubles sweeps 1, 2, 4 (`KEY: on` / `KEY: off`, controls included; each variant's baseline is the control with the same tag) |

Pictures: stills = master frames 12 and 35 of the first `--n-masters` (3) masters (boat, chimera,
foodmarket), letterboxed into a 3840x2160 canvas (like the upscale study). Sequences = 24 frames from
`--seq-start` (default frame 24) of the first `--n-seq-masters` masters.

## Choosing the pans (sweep 4)

The motion benefit only shows with camera motion. Look at each candidate master (`D:\nn-corpus\trimmed\<id>\reference_ffv1.mkv`),
find a 1 s stretch with a steady pan, and pass its first frame: `--seq-start boat=60,chimera=96,foodmarket=30`.
Use `--n-seq-masters N` for more masters. Keep the same flags on every stage.

## Settings

`sweep_settings.json` is a copy of the TrainKit r2 locked set (`tools/scaling_render.json`: `common`, `down`,
`up`, `shader_sections`); the kit is not read at run time. Everything is written under `--work`
(default `D:\codex\scaling-sweep`; E:, F: and `D:\nn-corpus` are refused). Layout: `vp/<job>/` (cfg, job.txt,
shots, result.txt, `done.txt`), `src/` (source luma raws), `pre/`+`ref/`+`coded/`, `stage1/`, `suites/`, `cap/`, `score/`, `report/`.

## Run (coordinator)

Build first: x64 Release with `VpScaleRender` and the renderer DLL (the defaults point at this worktree's
`x64\Release\VpScaleRender\VpScaleRender.exe` and `x64\Release\vprenderer\VideoProcessorVPRenderer.dll`).
Use a python with numpy and OpenCV (`cv2` reads the 16-bit PNGs fast; without it a slow pure-numpy fallback runs).

```
cd D:\codex\videoprocessor\feat-scaling-8b-blend
python -I tools/scaling_sweep/sweep.py plan    --dry-run [--camera-model-key camera_model --seq-start boat=60,...]
python -I tools/scaling_sweep/sweep.py plan                       # writes plan.json + all job.txt / render.cfg
python -I tools/scaling_sweep/sweep.py prepare                    # CPU, ffmpeg (single thread)
python -I tools/scaling_sweep/sweep.py render                     # GPU; laptop on mains; waits, never kills
python -I tools/scaling_sweep/sweep.py score  --score-python D:\hfr\venv\Scripts\python.exe --lpips
python -I tools/scaling_sweep/sweep.py report
```
Pass the same options to every stage. `--sweeps 1,3` limits the run; every stage resumes (finished
jobs have `done.txt`, scored variants have `score\...\results.csv`). `render` first runs all stage-1 and
single-stage jobs, then the route stage 2. Optional `--encode-kbps 6000` adds an HEVC round trip to the
upscale sources (sweeps 2 and 4 up form). Re-run for the camera model with
`--camera-model-key <key>` once it lands (a new `plan` rewrites the jobs; existing results are kept).

## Time (estimates, unmeasured)

Default grid (`plan` prints these): about 377 jobs, GPU about 2.7 h on the laptop RTX 3050 Ti (15 s start-up per
job + 1.3 s per still, about 33 s per 24-frame motion sequence); `prepare` about 10-15 min CPU; `score` about
3.3 h CPU single process (harness metrics on 2560x1440; add LPIPS time with `--lpips`); `report` seconds.
With `--camera-model-key` the GPU time is about 5.3 h and scoring about 6.5 h. GPU cost per variant can be
cut by `--sweeps`, `--n-masters 2` or `--stills 12`.

## Scoring path and harness gaps

Each final capture is converted to a capture-candidate directory (`NNNNNN.yuv` yuv420p10le, Y from the
capture's R10 mapped to limited codes as `upscale_study` does, chroma 512; `capture.json` schema 1, producer
`VpScaleRender`, `source_frame_index` = the geometry's global frame number). The reference (luma of the
canvas ideal-resampled with ffmpeg Lanczos to the output raster, centred in the window on code 64) and a
`coded` input are raw yuv files per geometry; one suite JSON per geometry (protocol `same`, one case per picture);
the harness runs once per (variant, geometry) with `--baseline capture:<control dir>`. Gaps (harness not edited):

- The `same` protocol needs a `coded` source of the same raster. It is a plain Catmull-Rom (ffmpeg bicubic) scale
  of the source, so the `*_vs_input` rows (grain_keep, lag1) compare against that, not against VP's own input.
- Capture candidates carry no `gpu_ms` (the sidecar has none): cost rows are NA. The run uses `--no-cost`.
- Chroma is neutral 512 in reference and captures: only the Y rows mean anything.
- The capture is full-range RGB (R10): BTB/WTW and `clip_detect` rows cannot be judged (reported, not gated).
- Reference resampling uses ffmpeg Lanczos (fast at 4K), not `eval_io.resample_plane` (lanczos3, sparse matrices).
- Content type is `film24` for every case; `gop`/`minigop` are omitted (GOP rows NA). Stills are 1-frame
  cases, so temporal rows (flicker, ghost, smear, grain_hf/lag1) are NA there; they apply to sweep 4.
- LPIPS needs the torch venv (`--score-python D:\hfr\venv\Scripts\python.exe --lpips`).

`report` writes `report\sweep<k>_<name>.md`: per geometry and variant the guard FAILs and the mean of
psnr_Y, ssim_Y, lpips, halo_overshoot_Y, grain_rms_ratio_Y, grain_keep_vs_input_Y, flicker_excess_Y,
ghost_coeff_Y, smear_Y, and the best variant (highest mean ssim_Y) that has no FAIL flag on a grain, halo,
flicker, ghost or smear guard.

Tests: `python -I -m unittest discover -s tools/scaling_sweep -p "test_*.py" -v`
