# Upscale quality study (prepared 2026-10-24; GPU run pending)

B (the x2 net) is dropped for now, so VP's classical upscaling has to be excellent on its own.
This study measures which route and which libplacebo upscaler serve 1080p and 720p deliveries
best on the owner's FL35. The scripts are in `tools/upscale_study/`; nothing has been rendered yet.

## What is compared

**Reference.** The 4K master (read-only `D:\nn-corpus\trimmed\*`) through the kit's ideal
resample (`trainkit/common.py resample`: Kaiser-windowed sinc, 8 lobes), fitted into 2560x1600.
The size is the one VP used (from its "VP anamorphic layout" log line).

**Deliveries (cases).** Masters (7) x segments (2: at 1 s and 3 s, 48 frames each) x scored
frames (2 per segment: frames 12 and 35) = 28 frames per case.

| case | encoded | codec, bitrate | player | VP re-base |
|---|---|---|---|---|
| `n1080` | 1920x1080 fit | HEVC 6 Mb/s | none (native) | no |
| `p1080_2160` | 1920x1080 fit | HEVC 6 Mb/s | Catmull-Rom to 3840x2160 (TV app style, bars padded) | to 1080 (`source_resolution: 1080`, kernel match, latch fixes) |
| `n720` | 1280x720 fit | AVC 3 Mb/s | none | no |
| `p720_1080` | 1280x720 fit | AVC 3 Mb/s | Lanczos to 1920x1080 | to 720 |
| `p720_2160` | 1280x720 fit | AVC 3 Mb/s | Catmull-Rom to 3840x2160 | to 720 |

The encodes use rate control like a stream: `-b:v`, `maxrate` 1.5x, `bufsize` 2x, GOP 48, 3
B-frames. The master is first downscaled to the encoded size with ffmpeg Lanczos, as a studio
master would be.

### (a) The route (VP's real chain, `VpScaleRender`)

All variants use the kit's locked scaling settings (`scaling_render.json`): `fbo_float32` on,
deband / dither off.

| variant | what |
|---|---|
| `direct_vp_hybrid` | VP's shipped upscale: `adaptive_upscale` on (FSRCNNX + SSimSuperRes blend), upscaler auto, linear light |
| `direct_<u>` | Plain upscale to the output, `adaptive_upscale` off; u = lanczos, catmull_rom, ewa_lanczos, ewa_lanczossharp |
| `route4k_<u>` | Always-4K: stage 1 upscales to 3840x2160 with u (lanczos, catmull_rom, ewa_lanczossharp); stage 2 is VP's hybrid downscale (`adaptive_downscale` on, w_base 0.2, Lanczos, faithful ortho, SSimDownscaler) to the output |
| `route4k_<u>+D` | The same plus D, applied at scoring with the kit's PyTorch D: SCALER_SRC = the stage-1 picture, SCALER_OUT = the stage-2 output. It needs the round-1 D weights (`--d-staging`, kit drive) |

Notes:

- **Stage 1 emulates the route.** VP's `scaling.route_4k` slot is still a placeholder (planned:
  fixed 1-D x2 shaders, H then V, r16 hand-off). Stage 1 uses VP's own upscaler to 3840x2160,
  captured at 10-bit (R10): about 1/4 code of extra quantisation compared with r16. Report
  route results with that caveat.
- **Captures.** These are VP's rendered-output captures (VP-0133). The R10 code is mapped back
  with the kit's fixed limited->full inverse, (code x 876 / 1023 + 64) / 1023, with no fitting,
  so a level error stays visible.

### (b) libplacebo upscalers (`PlScaleSweep`, libplacebo directly)

VP's renderer offers no `spline36` and no anti-ringing strength for its main upscaler, so this
part runs the same libplacebo sampling code in a small harness:

- separable filters (`pl_shader_sample_ortho2`, H then V, r32f intermediate): lanczos, spline36,
  catmull_rom;
- polar EWA (`pl_shader_sample_polar`): ewa_lanczos, ewa_lanczossharp, ewa_lanczos4sharpest.

The sweep covers the 6 filters x anti-ringing {0, 0.25, 0.5, 0.75, 1.0} x 2 targets:

- direct to the output picture;
- to 3840x2160, then the kit's ideal downscale (the route's stand-in: the harness has no VP
  hybrid downscaler).

The input is the decoded picture at the encoded size, which is what VP's re-base reconstructs
(exact for the native cases). Domain: gamma-coded luma.

If the sweep finds a clear winner that VP cannot select today (spline36, or anti-ringing > 0),
the follow-up is a small renderer change: an `upscaler_antiring` key and a `spline36` choice.

### (d) The downscale A/B: AdaptiveDownscale polar vs separable (owner 2026-10-24)

This uses the native 4K case `native2160`: the 4K master itself (no encode), fitted into
3840x2160, then VP to the FL35. It compares the grain-aware hybrid downscale's faithful branch
both ways:

- `adaptive_faithful: auto`: polar `ewa_lanczossharp` whenever both axes scale alike, as
  4K -> 2560x1440 does (AdaptiveDownscale.cpp:631-636);
- `adaptive_faithful: ortho`: two separable Lanczos passes.

Everything else follows the kit's locked down settings (`w_base` 0.2, linear light,
SSimDownscaler). Each runs with `fbo_float32` on and off, so there are 4 variants. The same
metrics apply, plus cost (`study.py cost`, below).

The owner agreed to test separable on clips first. It becomes the default only if it is faithful:
no worse grain keep, halos or ringing.

### (e) fbo_float32 on/off (the per-tier option decision)

- `down_*_fbo32on|off` and `direct_vp_hybrid` vs `direct_vp_hybrid_fbo32off` on the upscale
  cases.
- **Highlight error:** `hl_err_codes`, the mean error where the reference is above code 900.
- **WTW keep:** `wtw_kept`, the share of the reference's above-white pixels (> 940) still above
  940. VP's captures are scored unclamped, so WTW survives into the metric.
- Real masters have few above-white pixels, so the downscale case adds a synthetic still
  `wtwramp`: a code 64 -> 1019 ramp with 1-code noise, plus thin lines at 1019.
- **Cost:** `study.py cost` runs each downscale variant and the fbo pair on a 12 s held still
  with no capture. It samples nvidia-smi utilisation every 200 ms (the kit's vpbench method,
  first 4 s dropped) and writes `cost.json`; the report adds a cost table.

### (c) Metrics (`metrics.py`, the kit's functions, on the GPU with the kit's Python)

- **PSNR, SSIM:** luma, `C.psnr` / `C.ssim`.
- **ring_codes:** mean |out - ref| in the near-edge band of the reference (`C.near_edge_mask`,
  the kit's ringing term).
- **overshoot_codes:** how far the output leaves the reference's local 3x3 range in that band
  (halo).
- **grain_keep:** the kit's `C.grain_keep_term` ratio (high-band RMS of out / ref on grain tiles;
  under 0.95 = grain lost, flagged in the report).
- **hf_err_codes:** the high-band error.
- **gpu_ms:** the pl_timer median (sweep). VP-chain cost is not timed here; the kit's `vpbench`
  measures it.

`report.py` writes one table per case, sorted by PSNR, with grain loss flagged.

## How to run (after the GPU all-clear)

```
python tools/upscale_study/study.py plan    --work D:\codex\upscale-study            # check + write jobs
python tools/upscale_study/study.py prepare --work D:\codex\upscale-study            # CPU: ffmpeg
tools\upscale_study\build_sweep.bat                                                  # once (cl)
python tools/upscale_study/study.py render  --work D:\codex\upscale-study            # GPU
python tools/upscale_study/study.py cost    --work D:\codex\upscale-study            # GPU: (d)/(e) cost, ~2 min
python tools/upscale_study/study.py score   --work D:\codex\upscale-study [--d-staging E:\VP-NN-TrainKit\work\staging]
python tools/upscale_study/study.py report  --work D:\codex\upscale-study
```

- Every stage takes `--dry-run`: it validates the inputs, tools and renderer keys and prints the
  commands without running them.
- `render` and `score` wait while another agent's vstest/testhost or a VideoProcessor process
  runs. They never kill anything.
- `--work` may not be on E:, F: or under `D:\nn-corpus`.
- Use a VpScaleRender built from `fix/vpscalerender-motion` (`--render-tool`); the older build
  mis-counts frames across consecutive pictures.

## Expected run time

The dry run (`plan --dry-run`) gives: 28 frames per case (29 with the WTW ramp in the
downscale case), 6 cases, 64 VpScaleRender jobs, 300 sweep batches (8,700 upscales), and about
10,500 scored items. Parts (d)/(e) add about 10 min of renders and a 2-3 min cost run on either
PC.

| stage | laptop (RTX 3050 Ti) | cinema PC (RTX 3090) | bound by |
|---|---|---|---|
| prepare (ffmpeg extract, 70 short encodes, decodes) | ~20 min | ~8 min | CPU |
| render: VpScaleRender (55 jobs x (15 s start + 29 stills x ~1.3 s)) | ~50 min | ~45 min | the hold per still, not the GPU |
| render: PlScaleSweep (300 batches) | ~12 min | ~6 min | GPU + readback |
| score (kit Python, CUDA: SSIM / masks at 2560x1440; 420 D inferences) | ~15 min | ~5 min | GPU |
| **total** | **~1.6 h** | **~1.1 h** | |

The GPU is busy for about 30 min on the laptop and 15 min on the 3090. The rest is CPU or wall
clock. Disk: the raws and captures take about 25 GB under `--work`; delete the
`sweep\*\*.y16` files after scoring.
