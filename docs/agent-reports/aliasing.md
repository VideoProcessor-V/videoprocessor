# Aliasing and moire fidelity suite (2026-10-18)

Owner: aliasing and moire are core to the fidelity goal. This suite measures them, together with the
passband, ringing and pan shimmer, for VideoProcessor's FULL picture chain at the real output
resolutions. It also attributes each effect to the stage that causes it, and gates regressions.

## What runs

- **Chain.** The shipped renderer DLL is driven through `RendererCaptureHarnessTests::AliasingSuiteCaptures`,
  rendering off screen with the window at the output size. It uses:
  - NVIDIA adapter (`VP_TEST_ADAPTER`);
  - bundled shaders (`VP_TEST_SHADER_DIR`);
  - 8-bit output with VP's temporal sigma-delta (`display_bit_depth: 8`, `temporal_dither: sigma_delta`);
  - 23.976p input, the base profile (no input rule), V210 4:2:2 10-bit.

  Captures are the presented backbuffer (VP-0133), read back as R10 codes.
- **Sizes.** `VP_ALIAS_SOURCES` × `VP_ALIAS_OUTPUTS`:
  - defaults are 3840x2160 and 1920x1080 sources × 2560x1600, 1920x1200 and 1920x1080 outputs;
  - 3840x2160 is an optional output;
  - every ratio is derived from the sizes, with an aspect-preserving fit (16:9 sources letterboxed on
    16:10 outputs, never stretched). The capture's picture rectangle is checked against that fit.
- **Driver.** `tools/aliasing/alias_suite.py` runs the steps in this order: `prepare`, `precompute`,
  `validate`, `run`, `analyse`, `report`, `gate`.

## Method

The reference is ideal band-limited resampling of the exact source codes onto the output picture grid
(periodic sinc / Dirichlet), separable, in float64. The cut-off on each axis is the lower of the source
and output Nyquist. Chroma is co-sited 4:2:2. The output RGB is mapped to Y'CbCr by a least-squares fit,
so no range, matrix or primaries assumption is needed.

- **Static card.** Every tile is faded to grey and runs to 0.9 of the source Nyquist:
  - zone plates, and horizontal, vertical and diagonal sweeps;
  - luma, plus luma with grain-like noise;
  - Cb and Cr chirps;
  - a hard-edged rectangle, a flat tile (noise floor), and colour patches.
  - **Aliased energy:** output energy, in 32 px local windows, at frequencies where the ideal has none,
    with the noise floor removed, relative to the reference energy (dB).
  - **Passband:** the projected gain against the ideal per local frequency (MTF), in bands of the
    output Nyquist.
  - **Ringing:** the overshoot beyond the plateaus within 8 output pixels of the edge, as a % of the step.
- **Motion (synthetic).** An x-periodic card is panned at 3 and 8 source px per 24p frame. Its bands sit
  at fractions of the pair's cut-off:
  - passband 0.4 and 0.7, near 0.9, and stopband 1.15 and 1.4 where the source can carry them;
  - a diagonal band at 0.8, and grain.

  Each capture is registered to the ideal at the same sub-pixel position. Two metrics follow:
  - **pan error:** the error energy against that motion-matched ideal (dB);
  - **shimmer:** the temporal standard deviation of each window's energy error, in units of a source band.

  Captures that match no pan position are counted as "unmatched". They are held, blended or torn frames.
- **Motion (real).** Clip-library pans (Tears of Steel, 120 and 480 px/s, 1920x800 inside 1080). Each
  capture is registered to the nearest ideal-resampled source frame. The metrics are the error in the
  fine-detail band (0.3-0.95 of the highest frequency the source carries at the output) and the shimmer.

## Validation (must pass before results count)

Controls are rendered offline, packed into synthetic captures (limited-range BT.709 R10, black bars) and
pushed through the identical analysis (`alias_suite.py validate`, CPU). Aliased energy is in dB relative to the reference.
Ideal = the reference itself (floor). Every pair passes (`D:\codex\alias-suite\validation.json`).

| pair | ideal | good: Lanczos-4 sinc | good, 8-bit dithered | nearest | bilinear, no AA | sharpened + sinc (ringing %) | pan err: ideal / sinc / nearest / bilinear-noAA | checks |
|---|---|---|---|---|---|---|---|---|
| 3840x2160 -> 2560x1600 | -55.1 | -26.4 | -26.6 | -3.0 | -10.7 | -16.4 (27%) | -59.7 / -16.6 / -4.0 / -8.7 | PASS |
| 3840x2160 -> 1920x1200 | -90.0 | -27.8 | -27.6 | -0.0 | -10.0 | -20.6 (16%) | -57.4 / -16.6 / -4.5 / -9.4 | PASS |
| 3840x2160 -> 1920x1080 | -90.0 | -27.8 | -27.6 | -0.0 | -10.0 | -20.6 (16%) | -58.0 / -16.6 / -4.5 / -9.4 | PASS |
| 1920x1080 -> 2560x1600 | -56.1 | -27.8 | -27.8 | -7.8 | -18.5 | -20.0 (46%) | -60.5 / -13.8 / -4.3 / -6.7 | PASS |
| 1920x1080 -> 1920x1200 | -69.5 | -90.0 | -70.9 | -90.0 | -90.0 | -20.7 (45%) | 1:1 (identity) | PASS |
| 1920x1080 -> 1920x1080 | -69.5 | -90.0 | -67.9 | -90.0 | -90.0 | -20.7 (45%) | 1:1 (identity) | PASS |

- Known-bad controls score at least 6 dB worse than the good reference: nearest is 15-28 dB worse; bilinear without
  anti-aliasing is 9-17 dB worse; the sharpened pre-pass shows 16-46% ringing against 3-10%. 8-bit dither does not
  move the score.
- On 1:1 pairs (1080 on 1080, and 1080 letterboxed in 1200) luma has no resampling: every control equals the ideal.
  The chroma controls still differ, because 4:2:2 is upsampled 2x.
- The in-chain capture path is validated too:
  - `ctl_down_gpu_noaa` (`downscaler: gpu`, hardware bilinear) scores -11.0 dB, against -10.7 for the offline bilinear-no-AA;
  - `ctl_up_nearest` scores -7.8 dB, against -7.8 offline (round-4 iGPU smoke run).


## Results

**State: PAUSED by the owner (usage; TrainKit has priority) before the full matrix.** What exists:

- Round 4 (3164dda2, NVIDIA, sigma-delta, bundled shaders): 3840x2160 -> 2560x1600 complete, covering stills, pans,
  the blend sweep and the GPU cost rows. `D:\codex\alias-suite\results.json` / `results.md`.
- Round 5 (integ-r4 c940e705; renderer DLL sha256 cd2f7e57..., built 13:32): 3840x2160 -> 2560x1600 stills and
  synthetic pans. `D:\codex\alias-r5\results.json` / `results.md`.
- Round 4 and round 5 agree to 0.1 dB on every static metric at 4K -> 1600. Latch, efficiency and the fast detail
  pulse did not change scaling fidelity.
- An earlier round-4 run on the Intel iGPU, without the bundled shaders, is kept only as history
  (`results_igpu_noshaders.json`). It did not count: no sigma-delta and no hybrid shaders.

Columns, as fractions of the output Nyquist:
- **alias:** luma zone plates and sweeps, with and without grain;
- **MTF:** passband gain;
- **ringing:** overshoot, % of the step;
- **pan err / pan moire:** synthetic pan error, all bands / stopband only;
- **shimmer:** per-window temporal energy error, in source-band units;
- **unmatched:** captures that match no pan position.

### Round 5, 3840x2160 -> 2560x1600 (the owner's main pair; picture 2560x1440, x0.667)

| stage config | alias dB | chroma alias dB | grain err dB | MTF 0.4-0.6 | MTF 0.6-0.8 | MTF 0.8-0.95 | ringing % | pan err dB | pan moire (stop) dB | shimmer | unmatched pan frames | real-pan HF err dB | real-pan shimmer | vs default |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| default | -16.1 | -24.4 | -12.5 | 0.78 | 0.62 | 0.45 | 0.1 | -0.8 | -10.1 | 3.684 | 0/20 | - | - | baseline |
| deband_off | -16.1 | -24.4 | -12.5 | 0.78 | 0.62 | 0.45 | 0.0 | - | - | - | - | - | - | neutral |
| deband_vp_on | -16.1 | -24.3 | -12.6 | 0.78 | 0.63 | 0.45 | 0.0 | -7.3 | -9.9 | 2.267 | 0/20 | - | - | pan err -6.5 dB |
| detail_pulse_off | -16.1 | -24.4 | -12.5 | 0.78 | 0.62 | 0.45 | 0.0 | -0.6 | -10.1 | 3.945 | 0/20 | - | - | neutral |
| texture_restore_on | -16.1 | -24.4 | -12.5 | 0.78 | 0.62 | 0.45 | 0.1 | -0.7 | -10.1 | 4.070 | 0/20 | - | - | neutral |
| rebase_1080 | -18.4 | -23.2 | -7.4 | 0.78 | 0.27 | 0.03 | 6.8 | -4.6 | -30.2 | 0.015 | 0/20 | - | - | alias -2.3 dB, pan err -3.8 dB, MTF0.6 -0.35, ringing +6.7% |
| src_native | -16.1 | -24.4 | -12.5 | 0.78 | 0.62 | 0.45 | 0.0 | - | - | - | - | - | - | neutral |
| hybrid_down_ssim | -15.5 | -21.8 | -12.0 | 0.98 | 0.83 | 0.52 | 3.4 | -1.4 | -10.6 | 2.552 | 0/20 | - | - | MTF0.6 +0.21, ringing +3.3% |
| down_lanczos | -19.1 | -20.5 | -13.7 | 1.01 | 0.89 | 0.62 | 3.8 | -2.7 | -14.2 | 1.896 | 0/20 | - | - | alias -3.0 dB, pan err -1.9 dB, MTF0.6 +0.27, ringing +3.8% |
| down_mitchell | -19.3 | -25.3 | -12.9 | 0.78 | 0.58 | 0.37 | 1.2 | - | - | - | - | - | - | alias -3.2 dB, MTF0.6 -0.04 |
| down_gamma | -16.3 | -24.7 | -12.4 | 0.80 | 0.65 | 0.47 | 0.1 | - | - | - | - | - | - | neutral |
| motion_on | -16.1 | -24.3 | -12.5 | 0.78 | 0.62 | 0.45 | 0.0 | 1.0 | -8.0 | 2.881 | 0/20 | - | - | pan err +1.8 dB |
| sigma_off | -16.0 | -24.3 | -12.4 | 0.78 | 0.63 | 0.45 | 0.1 | -1.5 | -9.4 | 3.049 | 0/10 | - | - | neutral |
| dither_libplacebo | -16.1 | -24.4 | -12.6 | 0.78 | 0.63 | 0.45 | 0.1 | - | - | - | - | - | - | neutral |
| dither_off_all | -16.0 | -24.3 | -12.4 | 0.78 | 0.63 | 0.45 | 0.1 | - | - | - | - | - | - | neutral |
| all_detail_off | -16.1 | -24.4 | -12.5 | 0.78 | 0.62 | 0.45 | 0.1 | - | - | - | - | - | - | neutral |
| all_detail_on | -16.1 | -24.4 | -12.6 | 0.78 | 0.62 | 0.45 | 0.1 | - | - | - | - | - | - | neutral |
| ctl_down_gpu_noaa | -11.0 | -22.7 | -10.6 | 0.85 | 0.74 | 0.61 | 0.0 | - | - | - | - | - | - | alias +5.1 dB, MTF0.6 +0.12 |

### Hybrid scaler blend sweep (round 4, 4K -> 1600)

The blend weight is `adaptive_w_base`, with SSimDownscaler after Mitchell. At w = 0 the output is the faithful
branch (polar EWA-lanczossharp with anti-ringing); at w = 1 it is SSIM. The 4K -> 1080 and 1080 -> 1600 sweeps
(FSRCNNX + SSimSuperRes) are planned but not run.

| blend w_base | alias dB | ringing % | MTF 0.4-0.6 | MTF 0.6-0.8 | grain err dB | pan err dB | pan moire dB | shimmer |
|---|---|---|---|---|---|---|---|---|
| 0.00 | -19.8 | 2.1 | 0.98 | 0.82 | -12.0 | -1.1 | -14.2 | 2.743 |
| 0.20 | -19.2 | 2.3 | 0.98 | 0.82 | -12.1 | -1.2 | -13.5 | 2.668 |
| 0.40 | -18.4 | 2.7 | 0.99 | 0.82 | -12.0 | -0.8 | -13.0 | 2.993 |
| 0.60 | -17.5 | 2.5 | 0.98 | 0.82 | -12.1 | -1.1 | -12.2 | 2.717 |
| 0.80 | -16.5 | 3.2 | 0.98 | 0.83 | -12.0 | -0.8 | -11.5 | 3.019 |
| 1.00 | -15.5 | 4.1 | 0.98 | 0.84 | -11.9 | -1.1 | -10.7 | 2.649 |

Pick: w_base 0.20 (alias -19.2 dB, ringing 2.3%, MTF 0.6-0.8 0.82).

### GPU cost per frame (round 4, 4K -> 1600, RTX 3050 Ti)

D3D11 timestamp queries from the renderer's render-load window (`GetRenderLoad`), not wall clock. Machine load at
the time: CPU 100% (this suite's analysis workers plus other agents), and the GPU otherwise idle (nvidia-smi 0%
between runs).

| source -> output | scaler configuration | GPU ms/frame (avg) | peak | core render ms (avg) | GPU frames |
|---|---|---|---|---|---|
| 3840x2160 -> 2560x1600 | default | 15.48 | 17.53 | 12.41 | 16 |
| 3840x2160 -> 2560x1600 | sep_lanczos | 19.51 | 25.45 | 15.74 | 22 |
| 3840x2160 -> 2560x1600 | sep_mitchell | 17.33 | 20.05 | 14.40 | 18 |
| 3840x2160 -> 2560x1600 | hyb_down_ssim | 30.52 | 37.98 | 27.41 | 22 |
| 3840x2160 -> 2560x1600 | rebase_1080 | 21.75 | 22.82 | 17.99 | 24 |


## Owner summary

Provisional: one pair, 4K -> 2560x1600. The other five pairs are pending.

- **Aliasing at 4K -> 1600 comes from the default downscaler.** `downscaler: auto` resolves to **hermite** at
  quality=high.
  - It scores -16.1 dB, against -26.4 dB for a windowed sinc. The 1.15x-Nyquist stopband shows visible moire beats
    when panning (-10 dB pan moire).
  - **Lanczos** cuts aliasing by 3 dB and raises the passband from 0.62 to 0.89 at 0.6-0.8 Nyquist, at +4 points of
    ringing and +4 ms GPU time.
  - **Mitchell** cuts aliasing by 3.2 dB at the same passband as hermite, with less ringing.
  - The diagonal 0.8-Nyquist band is almost entirely lost on the pan: about 15% of its energy is kept.
- **Hybrid SSIM downscale.**
  - At the default w = 1 it is no better than hermite on aliasing (-15.5 dB) and adds 3.4% ringing, though its
    passband is far better (0.83).
  - Aliasing falls monotonically as w drops: at w = 0.2 it reaches -19.2 dB with 2.3% ringing and MTF 0.82.
    **Recommendation: `adaptive_w_base` around 0.2 at 4K -> 1600**, since information preserved matters more
    than sharpness.
  - It costs about 2x the GPU time of hermite on the 3050 Ti (30.5 against 15.5 ms per frame).
- **Re-base to 1080 on genuine 4K hurts badly.** The passband at 0.6-0.8 drops 0.62 -> 0.27 and at 0.8-0.95 drops
  0.45 -> 0.03; grain error rises by 5 dB and ringing reaches 6.8%. It does show the lowest moire, but only because
  the detail is gone. The latch must not re-base true 4K.
- **Detail stages are neutral on stills:** debanders, detail pulse, texture restore, `src_native`, dither choice
  and de-judder extras all sit within 0.1 dB.
  - On pans, `deband_vp: on` (which turns libplacebo's deband off) lowers the pan error from -0.8 to -7.3 dB. That
    points at libplacebo's auto deband and its grain adding frame-varying energy under motion. To be confirmed by
    `deband_off` pans in the full run.
  - Detail pulse and texture restore are neutral on pans, within 0.2 dB.
  - `interpolation: always` (de-judder extras) raises the pan error by 1.8 dB and the pan moire by 2 dB.
- The sigma-delta has no measurable cost to aliasing or passband. It adds about 0.002 rms of blue noise, removed
  by the noise-floor term.

## Gate

`alias_suite.py gate` checks `tools/aliasing/alias_thresholds.json`: the default chain's aliasing, chroma aliasing,
MTF, ringing and pan error against baseline plus margin. The in-chain known-bad controls must stay at least 3 dB worse.
It passes on round 4 and round 5 (4K -> 1600). Limits for the other pairs are added after their first full run.

## Resume (after the weekly reset; integ-r4 binaries unchanged at c940e705)

The round-5 harness is in `D:\codex\videoprocessor\alias-r5` (branch test/aliasing-r5: integ/round5 7d217dde
plus this suite's test commits; same plugin ABI as c940e705, already built). Round 5 changed IRenderer.h, so the
round-4 harness cannot drive the round-5 DLL. The run waits by itself while pulse-fast, vp-scale-render or
VP-NN-TrainKit processes run. It covers 332 runs: 95 stills, 126 synthetic pans, 54 real pans, 36 blend sweep and
21 GPU cost. That is about 75 min of GPU, with the analysis running alongside on CPU.

```
cd D:\codex\videoprocessor\alias
set VP_ALIAS_REFCACHE=D:\codex\alias-refcache
rem the finished 4K->1600 rows are kept; clear D:\codex\alias-r5\captures and results.json to start clean
start python tools/aliasing/alias_suite.py analyse --watch --delete --workers 3 --dir D:/codex/alias-r5
python tools/aliasing/alias_suite.py run --no-analyse --dir D:/codex/alias-r5 --bin D:/codex/videoprocessor/alias-r5/x64/Release --renderer-bin D:/codex/videoprocessor/integ-r4/x64/Release
type nul > D:\codex\alias-r5\captures\STOP
python tools/aliasing/alias_suite.py report --dir D:/codex/alias-r5
python tools/aliasing/alias_suite.py gate --dir D:/codex/alias-r5
```

`--adapter` defaults to NVIDIA. `VP_TEST_SHADER_DIR` and `VP_TEST_RENDERER_DIR` are set from `--renderer-bin`.
Output sizes come from `VP_ALIAS_OUTPUTS` (default `2560x1600,1920x1200,1920x1080`; add `3840x2160` if wanted),
and sources from `VP_ALIAS_SOURCES`. Run `prepare --plan-only` after changing either.

## Files

- `src/VideoProcessor-Test/RendererCaptureHarnessTests.cpp`: harness additions and `AliasingSuiteCaptures`:
  - chroma cards, frame sequences and repeated captures (raw R10 dumps);
  - no input rule, and the `VP_TEST_RENDERER_DIR` override;
  - the GPU timestamp cost read from `GetRenderLoad`.
- `tools/aliasing/alias_suite.py`: cards, ideal reference, metrics, controls, runner, report and gate.
- `tools/aliasing/alias_thresholds.json`: gate limits.
- `tools/aliasing/plan.tsv`: the 332-run plan (a copy of `D:\codex\alias-r5\plan.tsv`).

