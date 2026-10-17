# Laptop 4K30 budget (RTX 3050 Ti, 2026-10-17, analysis agent, read-only)

Owner goal: 4K30 usable on the laptop with all or most features and hardware acceleration on. The
3090 defaults are unchanged.

## Summary

- **30 Hz output fits** after the plan below.
- **60 Hz with de-judder is marginal.** The binding limit is the new-frame render, not VRAM.
  - The extras run only if the new frame's work plus 4 ms fits in one refresh
    (LibplaceboVideoRenderer.cpp ~13679). At 60 Hz that leaves 12.7 ms.
  - The last measured new-frame slot was about 29 ms worst (43e74cde, 2026-10-13). That was before
    seed mode, the sigma-delta quarter-res pass and Catmull-Rom chroma. Re-measure it.
- **Possible blocker:** the extras need DXGI display statistics. 904debe6 says neither laptop GPU
  provides them on the internal panel. Check the hold reasons on the internal panel and on an
  external port wired to the GPU.
- **VRAM is about 1.4-2.1 GB of 4 GB** (1.1-1.6 GB with 16-bit FBOs). It fits. Stay at or below
  ~2.6 GB so the driver does not evict. Nothing logs VRAM use today.

## Measured stage costs at 4K (laptop)

| Stage | Cost |
|---|---|
| Unpack (v210 / UYVY / R12) | 0.36 / 0.30 / 2.6 ms |
| Upload on the render thread | 2.4 ms blocking (5.5 ms with the transfer, 12-bit) |
| Re-base 2:1 | 3.5 ms (one axis 1.1) |
| VP debander | 0.8 ms |
| Chroma upscale | ~0.65 ms |
| Polar vs separable adaptive downscale | +2.4 ms for polar |
| fbo_float32 on all FBOs | +2.8 ms (output pass alone +0.73) |
| Matcher, full | 7.85 ms |
| Matcher, seed mode | 5.5 ms |
| Optical-flow producer | 11.3 ms per pair, on its own worker and engine |
| De-judder extras (worst) | ~7 ms |
| Sigma-delta search | 4.2 ms per new frame (~5 ms saved by reusing de-judder vectors) |
| DV GPU unpack | 0.41 ms |

Unmeasured:
- NLS, SSimSuperRes and FSRCNNX (they do not engage for a 4K-to-panel downscale)
- Stage 10 extended range
- LED hooks
- present
- the interpolator's Stage B, which reruns the whole chain each refresh, so it costs about one new
  frame per refresh

## Budget for a 4K30 source

| Display / content | Constraint | Now | After plan |
|---|---|---|---|
| 60 Hz, 24p in 30p | new frame ≤ 12.7 ms | ~25-29 ms, de-judder holds | 10-17 ms, borderline |
| 60 Hz, true 30p | ≤ 33.3 ms | marginal | fits |
| 60 Hz, interpolation "always" | two chains per 33 ms | does not fit | keep off |
| 30 Hz, 24p in 30p | ≤ 29.3 ms | marginal | fits (~40% headroom) |
| 30 Hz, true 30p | ≤ 33.3 ms | marginal | fits |

Spike margin: keep p99 at or below 75% of the slot. Laptop clocks vary 20-30% between runs.

## Ranked savings per new frame

| # | Change | Saving | Notes |
|---|---|---|---|
| 0 | Display statistics available (external GPU-wired port, or the dGPU display mode) | Unlocks the extras and #2 | No code needed |
| 1 | `capture_upload: early` | -2.4 to -3.3 ms | Experimental; +243-410 MB VRAM |
| 2 | `dither_vectors_new_frame` (already on; needs #0) | -4 to -5 ms | |
| 3 | `adaptive_faithful: ortho` (separable) | -2.4 ms | Keeps 82% of the blend's effect; the owner prefers separable |
| 4 | `fbo_float32: off`, output pass stays rgba32f | -2.1 ms measured, est. -3 to -5 ms | Owner decision; half-float was never agreed |
| 5 | Move the resolution-monitor copy into repeat/extra slots | Removes a 0.9 ms spike | |
| 6 | 16-bit rendered-warp cache | -0.2 to -0.4 ms per extra | New key |
| 7 | Merge the matcher's 18 small passes | -0.5 to -1 ms | Output unchanged |
| 8 | Fewer early-upload slots (11 to 7) | -90 to -150 MB VRAM | New key |

Not worth it:
- optical-flow grid or preset (own engine, 34% busy at 30p)
- compute shaders (VP passes are already compute)
- half-res debander (fidelity loss)

## Proposed "Laptop 4K30" profile (laptop config only)

**Stays on:**
- GPU unpack, native RGB ingest
- optical-flow seed with motion_ahead_frames 4
- de-judder rendered warp + estimate ahead
- sigma-delta with new-frame vectors
- BTB/WTW and extended range
- VP debander, Stage 12 detection and re-base
- LED, subtitles, Catmull-Rom chroma, SSIM blend, grain map

**Cheaper:**
- capture_upload early
- adaptive_faithful ortho
- (fbo_float32 off, only if the owner agrees)
- interpolation auto only
- optional 16-bit warp cache and 7 upload slots

## Measurement run (Claude on the GPU, on mains power, about 70-80 min)

Common env vars: VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA, VP_TEST_REQUIRE_GPU=1. Use vstest with
/Logger:trx.

1. **Prerequisite (about 5 min).** OnScreenLiveExtrasAndEstimateAhead, internal panel vs external,
   reading the hold reasons.
2. **Beta timing gate (3 x ~10 min).** VP_TEST_TIMING_GATE with a 4K clip.
   - Rate pairs: 23.976 on 60, 30 on 60, 23.976 on 30.
   - Each with and without ortho.
   - Gives per-pass GPU times.
3. **Re-base and upload (about 2 min).** Stage12PassCostsAt4K and PinnedUploadCapabilitiesAndTiming.
4. **Live 4K (about 25 min).** Needs code first:
   - a 4K harness case;
   - VP_PASS_TIMING=1: per-pass GPU time split by slot type (new / extra / repeat), every 10 s;
   - VRAM usage and budget logging;
   - Prepare and Extras timer stages.
