# Output scaling (hybrid SSIM + D) (`scaling.output`)

Unit: **scaling** | side: processing | order: 160 | status: **partial** | hook point: libplacebo

## Purpose / why

Scale to the output active size inside libplacebo: hybrid SSIM downscale (adaptive_w_base 0.2) with the optional D refine add-on; Lanczos fallback; polar retired.

## Contract

- Input: processing-size picture; D runs in the VP NetRunner inside AdaptiveDownscale's POST_KERNEL hook (light domain locked in its fingerprint, barrier R8)
- Output: output-size picture (linear or gamma light per downscale_light)
- Geometry: output active area
- Delay share: 0 frames
- Look-ahead: none

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `adaptive_chroma_ssim` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Apply the SSIM branch to R, G and B fully (chroma too) instead of luma only; can fringe colour | same on all (not tiered) |
| `adaptive_downscale` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Scaling > Advanced > Set by the Scaling tier | Original pathway sets `off` (ADR-0019) | Adaptive downscale: per pixel, blends a faithful downscale with the SSIM downscaler (luma only): w = w_base x (1 - edge) x grain_scale. Hard edges and grain lean faithful, texture keeps SSIM. Runs only when the frame downscales vertically and the SSimDownscaler shader is in the chain | Scaling Classic=off; Scaling Low=off; Scaling Mid=on |
| `adaptive_edge_hi` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 255 | 40 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Upper end (8-bit gradient codes) of the edge ramp: at and above it the blend is fully faithful. Measured threshold, leave alone | same on all (not tiered) |
| `adaptive_edge_lo` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 255 | 8 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Lower end (8-bit gradient codes, Sobel on luma) of the soft edge ramp: below it an area is not an edge and keeps the SSIM weight. Measured threshold, leave alone | same on all (not tiered) |
| `adaptive_faithful` | profile ([vprenderer.Default] and matching profiles) | enum | auto, ortho, polar (polar is retired and runs as auto) | auto | VP-V > Scaling > Main > Grain-aware downscale; also VP Renderer > Rendering > Adaptive downscale (upstream copy greyed while VP-V is on) | n/a (VP-only): unchanged by the Original pathway | The faithful branch of the adaptive downscale: auto = polar ewa_lanczossharp where both axes shrink alike, the separable (1D) filter when they differ by more than 10 %; ortho = two separable Lanczos passes always (about a quarter of polar's cost, keeps the character of grain). status line: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | same on all (not tiered) |
| `adaptive_grain_floor` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 0.4 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Lowest grain scale: the grain term ramps from this (at adaptive_grain_lo) to 1 (at adaptive_grain_hi) and lowers the SSIM weight on grainy pictures. Measured, leave alone | same on all (not tiered) |
| `adaptive_grain_hi` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 64 | 8 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Laplacian-energy code (8-bit units) at which the grain term reaches its full effect. Measured, leave alone | same on all (not tiered) |
| `adaptive_grain_lo` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 64 | 2 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Laplacian-energy code (8-bit units) at which the grain term starts to lower the SSIM weight. Measured, leave alone | same on all (not tiered) |
| `adaptive_grain_map` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Main > Grain-aware downscale; also VP Renderer > Rendering > Adaptive downscale (upstream copy greyed while VP-V is on) | n/a (VP-only): unchanged by the Original pathway | Measures grain per 16x16 source block from the de-judder's vectors instead of one number per frame; untrusted blocks and frames without a vector field take the per-frame term. Grainy areas lean faithful. status line: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | same on all (not tiered) |
| `adaptive_grain_map_smooth_ms` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 10000 | 500 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Per-block smoothing time (ms) of the grain map, following the content along the block's vector; a cut resets it | same on all (not tiered) |
| `adaptive_grain_shadow_bias` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 0.5 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | How strongly shadows are weighted in the grain measurement of the adaptive blend. Measured, leave alone | same on all (not tiered) |
| `adaptive_grain_smooth_ms` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 10000 | 1000 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | One-pole smoothing time (ms) of the per-frame grain term; it snaps on a cut (mean luma jumping by more than 20 codes) | same on all (not tiered) |
| `adaptive_motion` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | Original pathway sets `off` (ADR-0019) | Motion-adapted blend (candidate): per 16x16 block of the de-judder's field the sharper branch's weight (SSIM on a downscale, CNN on an upscale) is kept on steady, confidently tracked areas and lowered towards the faithful branch on fast or untrusted motion; no vectors (or NLS): factor 1, shown in status | same on all (not tiered) |
| `adaptive_motion_floor` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 0 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Factor applied on fast or untrusted motion; 0 = the faithful branch only (adaptive_motion) | same on all (not tiered) |
| `adaptive_motion_smooth_ms` | profile ([vprenderer.Default] and matching profiles) | float | 0 - 10000 | 250 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | One-pole smoothing time (ms) that follows each block's vector so the motion factor does not flicker at motion boundaries | same on all (not tiered) |
| `adaptive_motion_speed_hi` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 10.0 | 0.4 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Motion speed (picture heights per second) above which the weight falls to adaptive_motion_floor | same on all (not tiered) |
| `adaptive_motion_speed_lo` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 10.0 | 0.1 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Motion speed (picture heights per second) below which the sharper branch keeps its full weight (adaptive_motion) | same on all (not tiered) |
| `adaptive_upscale` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Scaling > Main > Grain-aware upscale; also VP Renderer > Rendering > Adaptive upscale (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Adaptive upscale for sources smaller than the output: blends the CNN upscale with a faithful upscale by the grain term (CNN upscalers sharpen grain). status line: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | same on all (not tiered) |
| `adaptive_upscale_superres_scale` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Scales SSimSuperRes's strength by the grain term in the adaptive upscale | same on all (not tiered) |
| `adaptive_upscale_w_base` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 1.0 | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | n/a (VP-only): unchanged by the Original pathway | Base weight of the CNN branch in the adaptive upscale | same on all (not tiered) |
| `adaptive_w_base` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 1.0 | VP-V > Scaling > Advanced > Set by the Scaling tier | n/a (VP-only): unchanged by the Original pathway | Base weight of the SSIM branch in the adaptive downscale where nothing argues against it (design value 0.2; the Scaling tier Mid writes it) | Scaling Mid=0.2 |
| `cnn_residual_stabilise` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Advanced > Scaler steadiness (expert); also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | The same hold for the CNN upscaler's added detail. status line: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | same on all (not tiered) |
| `downscale_light` | profile ([vprenderer.Default] and matching profiles) | enum | linear, gamma | linear | VP Renderer > Scaling page (downscale light) | Original pathway sets `linear` (ADR-0019) | Light domain of downscaling frames (Stage 8-1); gamma matches madVR's SSIM | same on all (not tiered) |
| `fbo_float32` | profile ([vprenderer.Default] and matching profiles) | enum | auto, on, off | auto (on, everywhere) | VP-V > Colour > Main > Range and precision (named there); the control is on VP Renderer > Rendering > Processing (Bill's page) | Original pathway sets `off` (ADR-0019) | Every libplacebo intermediate texture in 32-bit float (exact highlights, more bandwidth; half-float rounds values above white). auto = on; the output pass is rgba32f regardless. status line: Tone map: libplacebo (combined LUT missing) | same on all (not tiered) |
| `residual_stabilise_grain_guard` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Proposed retirement > Proven fixes (all on by default) | Original pathway sets `off` (ADR-0019) | Limits the residual hold to the coding-artefact band: blocks the grain term marks as grain pass this frame's residual, the held change is clamped to the expected coding-noise RMS; off = the 2026-10-15 one-pole, byte for byte. proposed for retirement (group A) | same on all (not tiered) |
| `ssim_bitrate_aware` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Main > Grain-aware downscale; also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Lowers the SSIM weight (and the CNN's) at a low bitrate-quality reading, down to ssim_bitrate_floor: gentler sharpening on poor streams (SSIM would sharpen blocking). status line: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | same on all (not tiered) |
| `ssim_bitrate_floor` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 0.4 | VP Renderer > Rendering > Fixes under test | n/a (VP-only): unchanged by the Original pathway | The lowest factor ssim_bitrate_aware lowers the SSIM weight to | same on all (not tiered) |
| `ssim_residual_stabilise` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Advanced > Scaler steadiness (expert); also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Holds the SSIM downscaler's added detail along the de-judder's trusted vectors (a one-pole over about 3 frames) so it does not shimmer; untrusted blocks, no vectors or NLS: this frame's residual. status line: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | same on all (not tiered) |
| `upscale_light` | profile ([vprenderer.Default] and matching profiles) | enum | linear, gamma | linear | VP-V > Scaling > Advanced > Grain-aware scaler thresholds (measured, leave alone) | Original pathway sets `linear` (ADR-0019) | Light domain of upscaling frames (Stage 8b) | same on all (not tiered) |


- Existing keys: `adaptive_downscale`, `adaptive_w_base`, `adaptive_edge_lo`, `adaptive_edge_hi`, `adaptive_grain_lo`, `adaptive_grain_hi`, `adaptive_grain_floor`, `adaptive_grain_smooth_ms`, `adaptive_grain_shadow_bias`, `adaptive_chroma_ssim`, `adaptive_grain_map`, `adaptive_grain_map_smooth_ms`, `adaptive_faithful`, `adaptive_upscale`, `adaptive_upscale_w_base`, `adaptive_upscale_superres_scale`, `downscale_light`, `upscale_light`, `ssim_bitrate_aware`, `ssim_bitrate_floor`, `cnn_residual_stabilise`, `ssim_residual_stabilise`, `residual_stabilise_grain_guard`, `fbo_float32`, `adaptive_motion`, `adaptive_motion_speed_lo`, `adaptive_motion_speed_hi`, `adaptive_motion_floor`, `adaptive_motion_smooth_ms`
- Planned keys: `hybrid_refine_d`
- Tiers: Classic Lanczos; Low..Upper hybrid; Upper + D

## Fallbacks

- "Fallbacks engaged" entry: output scaling: libplacebo downscaler (SSimDownscaler not in the shader list) | plain SSIM (adaptive downscale failed on the GPU)
- Behaviour: SSimDownscaler missing while adaptive_downscale is on, or a GPU failure of the blend (FallbackCatalog::OutputScaling).

## Net fingerprint / upstream

- Fingerprint: D: hybrid shader identity + VP commit
- Upstream modules: `scaling.chroma_upscale`, `motion.consumers`, `scaling.geometry`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- AdaptiveDownscale tests
- D cost measured in VP first

Contract tests:
- D counts only without sharpness/detail/grain loss

Chain tests:
- pan shimmer test

## Motion-adapted blend and Stage 8b one-axis (2026-10-31)

- **Motion-adapted blend** (`adaptive_motion`, off by default: a candidate until the sweep in `tools/scaling_sweep/` decides). The sharper branch's weight (SSIM on a downscale, the CNN on an upscale) is multiplied per 16x16 block by a factor from the de-judder's field: 1 for a steady, trusted block, ramping by `smoothstep(adaptive_motion_speed_lo, adaptive_motion_speed_hi, speed)` to `adaptive_motion_floor` on fast motion (speed in picture heights per second: general over sizes and rates), the floor for untrusted blocks or frames; a one-pole over `adaptive_motion_smooth_ms` that follows each block's vector (no flicker at motion boundaries); bilinear between block centres in the blend. Lowering the weight only moves towards the faithful branch, so grain is kept. The camera model's per-block confidence multiplies the target when it lands (`MotionTarget`'s `confidence`, 1 today). No vectors (none running, NLS, a discontinuity): factor 1 (today's blend), shown as "Motion-adapted blend: no vectors on this frame (plain blend)". With it on, the de-judder estimate is requested like the grain map's. CPU mirror: `MotionTarget` / `MotionStep` / `MotionAlpha` / `MotionSpeed` in `AdaptiveDownscale.h`.
- **Stage 8b one-axis** (N9-P12): the upscale gate (`UpscaleAxesOf`) also opens for a width-only upscale with the height 1:1 (the height-only gate missed it); mixed frames (one axis up, one down) keep the old routing. A frame upscaled on one axis with the other 1:1 is **faithful only** (w = 0; SSimSuperRes gets a zero grain term, so it adds nothing) and any LUMA prescaler (a CNN doubler, recognised from its shader source by `CustomHookChain::IsLumaPrescaler`) is dropped from that frame's hooks, so the CNN invents no detail on the axis that is not upscaled. The faithful branch is already separable there (`adaptive_faithful` auto: the axes' ratios differ), which is identity on the 1:1 axis. Status: "Adaptive upscale: faithful only (one-axis upscale; CNN doubler skipped)" / "Adaptive upscale: CNN + faithful blend".
- **Optimal blend**: the non-net default stays VP's full blend (edge term, grain map, auto faithful, residual hold); `adaptive_w_base` / `adaptive_upscale_w_base` defaults are re-set from the downscale and upscale sweeps (`tools/scaling_sweep/`), re-run with the camera model on/off once it lands. The kit's locked set (w 0.2, ortho) stays a D/TrainKit concern only.
- Config UI controls for the `adaptive_motion*` keys: present since 81c38abe0 (the choice `adaptive_motion` and the four number fields, built by the adaptive scaler sections on the Scaling > Advanced card "Grain-aware scaler thresholds"); the earlier "not added yet" note was stale (P8-5, 2026-11-11). Their rows are in the Settings table above.

## Known issues / open items

- D cost unknown in VP (PyTorch bound 35.8 ms at 4K)
- D grain gate FAIL (0.75)

## How the code works

Checked against the code 2026-10-25 (M2 step 8 + WP-13).

### Public API

- `AdaptiveDownscale.h` (namespace `AdaptiveDownscale`): `Module` with two hooks, `Measure()` (PRE_KERNEL: edge and grain statistics, the grain map) and `Blend()` (POST_KERNEL: blends the SSIM / CNN picture with the faithful branch by a per-pixel weight). Per frame: `SetFrame(settings, sequence, frameSeconds)`, `SetUpscale(Upscale)` (the Stage 8b form), `SetSourceDownscales`, `SetMotion` / `ClearMotion` (residual stabilisation), `SetGrainField` / `ClearGrainField` (the 8-3 grain map). Readouts: `Active()`, `LastStats()`, `LastMapState()`, `GrainMap()`, `LastUpscaleRatio()`, `Failures()`. Helpers: `BitrateScale`, `BitrateAntiring`.
- `DownscaleLight.h`: `Downscales(source, target)`, `UseGammaLight(setting, source, target)`.
- `AdaptiveDownscaleStage.h` (WP-13): the host glue. `Register(Frame, Host, SharedState, hooks)`: upscale or downscale form, measure first, blend before SSimSuperRes or last, bitrate weighting, residual motion, grain field, the SSIM-missing log and the status tick. `ApplyLight(params, Light)`: the Stage 8-1 / 8b light domain. `UpscaleThisFrame()`.

### Data flow

The renderer (render thread, `RenderLocked`) calls `Register` after the user hooks and the VP calibration LUT are collected and before the sigma-delta's code-domain step. The stage asks the host (the `Host` callbacks) for the source monitor's bitrate reading, the NLS map, whether SSimDownscaler is submitted and the de-judder's field for this source frame, only where the code needs them. `SharedState` writes back the host members other host code reads: `bitrateWeightScale` (status line), `adaptiveGrainMapWanted` (the de-judder estimate runs for the map) and `adaptiveNoSsimLogged` (status line). `ApplyLight` runs on the frame's `pl_render_params` (the de-judder extras reuse them). The grain map also feeds the sigma-delta's grain tolerance and the LED highlight-grain suppression (host code).

### Threading

Render thread only. No locks.

### Config keys actually read

Per renderer profile (display-rule overrides): `adaptive_downscale`, `adaptive_w_base`, `adaptive_edge_lo`, `adaptive_edge_hi`, `adaptive_grain_lo`, `adaptive_grain_hi`, `adaptive_grain_floor`, `adaptive_grain_smooth_ms`, `adaptive_grain_shadow_bias`, `adaptive_chroma_ssim`, `adaptive_grain_map`, `adaptive_grain_map_smooth_ms`, `adaptive_faithful` (retired values migrated), `adaptive_upscale`, `adaptive_upscale_w_base`, `adaptive_upscale_superres_scale`, `downscale_light` and `upscale_light` (`linear` / `gamma`), `ssim_bitrate_aware`, `ssim_bitrate_floor`, `cnn_residual_stabilise`, `ssim_residual_stabilise`, `residual_stabilise_grain_guard`. `fbo_float32` (Beta builds: every libplacebo FBO in rgba32f; a scaling.output VP addition, off with the slot off and in the Original pathway). Read here but owned elsewhere: `low_bitrate_handling` (input.source_analysis). `hybrid_refine_d` is planned (not read).

### Fallbacks a user sees

- `output scaling` under "Fallbacks engaged" (`FallbackCatalog::OutputScaling`): adaptive downscale on but SSimDownscaler not in the shader list (libplacebo's downscaler), or the blend failed on the GPU on the last frame (plain SSIM). Engaged through `AdaptiveDownscaleStage::Stage::Fallback` (the fix after the WP-13 move; findings 2026-10-25).
- The custom-shader status line also says "Adaptive downscale: needs SSimDownscaler" / "idle" / "measuring".
- The grain map not used with an unknown NLS shader: per-frame grain (logged once). A designed path, not a downgrade.

### What it needs to be lifted

libplacebo (hooks, dispatch, filters), `pch.h`, `VP-V/modules/scaling/output/NlsMaskMapping.h` (this folder), `VP-V/modules/motion/consumers/OcclusionMaskHook.h`, `vprenderer/GrainPreserve.h`, `CustomHookChain` (output.dither) for SSimSuperRes's parameter, `DebugLog.h`, `GeneralRateLog.h`. The SSIM shaders stay in `shaders/`.

### Test it standalone

- CPU: `OutputGlueStagesTests` methods `Adaptive*` and `DownscaleLight*`; `PostMergeAudit20261017Tests`.
- GPU (WARP unless `VP_TEST_GPU=hardware`; only when allowed): `LibplaceboLutParserTests` Stage 8 methods, `OutputGlueStagesGpuTests.AdaptiveUpscaleStageRendersTodaysCodes` (Category GpuHardware).

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~OutputGlueStagesTests|FullyQualifiedName~PostMergeAudit20261017Tests" /Logger:"trx;LogFileName=scaling_output.trx"`

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/scaling/output/AdaptiveDownscale.cpp`
- `src/VP-V/modules/scaling/output/AdaptiveDownscale.h`
- `src/VP-V/modules/scaling/output/DownscaleLight.h`
- `shaders/SSimDownscaler.glsl`
- `shaders/SSimSuperRes.glsl`
- `src/VP-V/modules/scaling/output/AdaptiveDownscaleStage.h`
- `src/VP-V/modules/scaling/output/AdaptiveDownscaleStage.cpp`

Forwarding headers (legacy path; removed in M2 step 0): `src/VideoProcessor-Lib/vprenderer/AdaptiveDownscale.h`, `src/VideoProcessor-Lib/vprenderer/DownscaleLight.h`.

The forwarders stay for LibplaceboVideoRenderer.cpp (Bill's host) and the upstream LibplaceboLutParserTests.cpp / ConfigFileTests.cpp, which this move does not edit. The SSIM shaders stay in shaders/ (runtime layout). AdaptiveDownscaleStage.* is the host glue (WP-13): the host's call sites are in docs/design/m2-output-callsites.patch.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0012 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `adaptive_downscale`, `adaptive_w_base`, `adaptive_edge_lo`, `adaptive_edge_hi`, `adaptive_grain_lo`, `adaptive_grain_hi`, `adaptive_grain_floor`, `adaptive_grain_smooth_ms`, `adaptive_grain_shadow_bias`, `adaptive_chroma_ssim`, `adaptive_grain_map`, `adaptive_grain_map_smooth_ms`, `adaptive_faithful`, `adaptive_upscale`, `adaptive_upscale_w_base`, `adaptive_upscale_superres_scale`, `downscale_light`, `upscale_light`, `ssim_bitrate_aware`, `ssim_bitrate_floor`, `cnn_residual_stabilise`, `ssim_residual_stabilise`, `residual_stabilise_grain_guard`, `fbo_float32`, `adaptive_motion`, `adaptive_motion_speed_lo`, `adaptive_motion_speed_hi`, `adaptive_motion_floor`, `adaptive_motion_smooth_ms`.
- Planned keys (not yet implemented): `hybrid_refine_d`.
- Tiers: Classic Lanczos; Low..Upper hybrid; Upper + D.
- VP-V page, Scaling tab: tiers Classic / Low / Mid; tier keys `adaptive_downscale`, `adaptive_w_base`, `source_resolution`.
- Listed under the tier: `adaptive_faithful`, `adaptive_grain_map`, `ssim_bitrate_aware`, `ssim_residual_stabilise`, `cnn_residual_stabilise`, `adaptive_upscale`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#5-scaling](../../../../../docs/settings/VP-settings-reference.md#5-scaling).
- Fallback entry ("Fallbacks engaged"): `Output scaling: Lanczos (hybrid unavailable: <reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): MISSING.
- Isolation (fixed inputs, expected outputs, cost): AdaptiveDownscale tests; D cost measured in VP first.
- Contract (formats, geometry, fallback entry): D counts only without sharpness/detail/grain loss.
- Chain (total delay at the 4/3/2-frame budgets): pan shimmer test.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->

## Private GPU textures (skeleton rule)

This module still creates its own GPU textures (see the static test src/VP-V/tests/PrivateTextureScanTests.cpp). It migrates in SA6/SA7/SA8/SA12 (doc D:\codex\vp-handover\51-skeleton-adoption-audit.md section 6): per-render temporaries move to the shared scratch arena (src/VP-V/core/ScratchArena.h) and kept state to skeleton product nodes.
