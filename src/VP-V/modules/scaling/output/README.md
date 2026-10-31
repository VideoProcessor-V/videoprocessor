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

- Existing keys: `adaptive_downscale`, `adaptive_w_base`, `adaptive_edge_lo`, `adaptive_edge_hi`, `adaptive_grain_lo`, `adaptive_grain_hi`, `adaptive_grain_floor`, `adaptive_grain_smooth_ms`, `adaptive_grain_shadow_bias`, `adaptive_chroma_ssim`, `adaptive_grain_map`, `adaptive_grain_map_smooth_ms`, `adaptive_faithful`, `adaptive_upscale`, `adaptive_upscale_w_base`, `adaptive_upscale_superres_scale`, `downscale_light`, `upscale_light`, `ssim_bitrate_aware`, `ssim_bitrate_floor`, `cnn_residual_stabilise`, `ssim_residual_stabilise`, `residual_stabilise_grain_guard`, `fbo_float32`
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

libplacebo (hooks, dispatch, filters), `pch.h`, `VP-V/modules/scaling/geometry/NlsMaskMapping.h`, `VP-V/modules/motion/consumers/OcclusionMaskHook.h`, `vprenderer/GrainPreserve.h`, `CustomHookChain` (output.dither) for SSimSuperRes's parameter, `DebugLog.h`, `GeneralRateLog.h`. The SSIM shaders stay in `shaders/`.

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

- Keys: `adaptive_downscale`, `adaptive_w_base`, `adaptive_edge_lo`, `adaptive_edge_hi`, `adaptive_grain_lo`, `adaptive_grain_hi`, `adaptive_grain_floor`, `adaptive_grain_smooth_ms`, `adaptive_grain_shadow_bias`, `adaptive_chroma_ssim`, `adaptive_grain_map`, `adaptive_grain_map_smooth_ms`, `adaptive_faithful`, `adaptive_upscale`, `adaptive_upscale_w_base`, `adaptive_upscale_superres_scale`, `downscale_light`, `upscale_light`, `ssim_bitrate_aware`, `ssim_bitrate_floor`, `cnn_residual_stabilise`, `ssim_residual_stabilise`, `residual_stabilise_grain_guard`, `fbo_float32`.
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
