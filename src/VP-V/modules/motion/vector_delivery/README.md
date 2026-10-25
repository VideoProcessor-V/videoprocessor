# Vector delivery (`motion.vector_delivery`)

Unit: **motion** | side: processing | order: 130 | status: **partial** | hook point: host

## Purpose / why

Hand source-size vectors to consumers at the processing size: PER-AXIS scaling (re-base/4K route give different x/y factors), mapping through crop/A-lens/NLS, and an optional +-1 px local refinement for precision consumers.

## How the code works

Checked against the code on 2026-10-25 (feat/m2-motion). What the folder holds today is three different things: the de-judder's policy rules (`DejudderPolicy.h`), the matcher/warp's public types (`DejudderWarp.h`, implemented in `../consumers/DejudderWarp.cpp`), and two NOT YET WIRED pieces of the target design (`MotionSegment.h`, `src/VP-V/core/VectorScale.h`). The per-axis delivery the purpose describes is not a separate step in the running renderer: the warps and the interpolator map positions in their shaders (estimate px to plane px from the real texture sizes, `FrameInterpolator::Run(..., estimateWidth, estimateHeight)`, the rendered warp's crop/NLS map slope).

### Public API

- `DejudderPolicy.h` (pure, header-only, namespace `DejudderPolicy`):
  - `Decide(Input) -> Reason`: whether the motion estimate runs for this source frame (first failing check wins: Off, SmoothMotion, LowLatency, Interlaced, UnknownRates, IneligibleCadence, VideoRateMaskOnly, NativeRgb, PlaneFormat, NoNextFrame, NextFrameDiffers, else Active). The occlusion mask alone starts it; the grain map never does (`Input::grainMap` is carried for the reason text only).
  - `EffectiveStrength` / `ExtrasAllowed`: `dejudder_strength` on an uneven cadence, `dejudder_strength_even` on a whole cadence of a selected rate (`IsRate1001`, `EvenRateSelected`), 0 otherwise, film-like sources only.
  - Rate/cadence helpers, all from measured rates: `CadenceRatio`, `IsFilmLike(hz, generalRates)` (video from 49 Hz = 50 Hz family less 2%; legacy fixed bands when `general_rate_fixes` is off), `IsUnevenCadence`, `EvenCycle` (tolerance +-0.05 refreshes), `MaxExtras` (capped at 15).
  - `PlanExtra(ExtraInput) -> ExtraDecision`: whether the next refresh is still A's (stops: next frame arrived / near, past the next frame, extra limit); `LatencyFloor` (5th percentile of first-vsync minus callback over 5 s at any rate); `ExtrasBetween` / `MainPresentFor` (present ids without extras for the frame statistics).
  - `ResolveEngine(motion_engine, vpConfigured, smoothConfigured)`: VP motion or libplacebo smooth motion, never both; `Configured`.
  - Pairing rules: `ClassifyDolbyVisionFrame/Payload`, `DolbyVisionPairTrusted`, `DolbyVisionNextPairs` (byte-equal DM matrices with `motion_history_fixes`), `MotionAheadBreakBefore` (the optical-flow producer's break: gap, film update, non-adjacent pair times with `timing_continuity_fixes`, untrusted DV pair), `MatcherHistoryChained`.
  - Matcher scaling helpers used by `DejudderWarp.cpp`: `ScaledSearchRange` (range given for a 1080-line working picture), `OpticalFlowReach`, `CameraWindowPixels` (stepped, or continuous in height with `general_rate_fixes`), `EngineCellSourcePixels` / `EngineGlobalSourcePixels`, `SearchHalvingsFor` (test model).
- `DejudderWarp.h` (namespace `Dejudder`): `Planes`, `Settings` (every matcher/warp option; the renderer fills it in `DejudderMatcherSettings` with reference height 2160 lines and reference rate 23.976 Hz, which `MotionWarp::Scaled` converts to the source), `WarpHeader`, the GLSL field rules `FieldVectorGlsl` / `FieldVectorGlslSplit` / `FieldAllGlsl`, and class `MotionWarp` (`Estimate`, `Warp`, `WarpProcessed` (B3), `SnapshotForWarp`, field/trust/sub-field/occlusion/grain/camera accessors, `ForgetHistory`, optical-flow status accessors). The implementation and its threading are described in `../consumers/README.md`.
- `MotionSegment.h` (namespace `VpMotion`, header-only, GPU-free; NOT used by the renderer yet): `MotionSegment<Field, Slots=4>` owns or shares (`external`) the one `MotionValidity::State`; `Publish(direction, from, to, field, source, epochAtInputs, geometryGeneration, fillPair)`, `Retract(direction)`, `Fetch` / `FetchFrom` / `FetchAt` / `FetchBackward(allowFrameOld)` return a `Delivery` (field or a counted `Miss`: Disengaged, NotEstimated, Stale, NotAdjacent, Geometry), `Engage(bool)` (smooth motion exclusivity: disengaging invalidates with `SmoothMotionMix`), `Stats(consumer)`, `FallbackEntry()`. Wiring plan: `docs/design/motion-timing-callsites.md` section 5 and `docs/design/motion-shadow-callsites.patch` (shadow step P-b1: the segment shares the host's state).
- `VectorScale` (`src/VP-V/core/VectorScale.h`; NOT used by the renderer yet): `FromSizes`, `Then`, `Inverse`, `Anisotropic`, `Identity`, `Apply` (dx * sx, dy * sy).

### Data flow

- In: the renderer host (`LibplaceboVideoRenderer.cpp`, `RunDejudder...` around lines 16340-16540) fills `DejudderPolicy::Input` from the active settings, the measured rates (display: DXGI statistics, else render-ahead's open-loop rate, else the hook refresh) and the queued frames A/B.
- Out: `Reason`, the effective strength, the even cycle and `ExtrasAllowed` decide whether the matcher runs and whether extras are presented; the interpolation policy (`../consumers/InterpolationPolicy.h`) reuses `EffectiveStrength` so the two never take the same source.
- Vectors themselves do not pass through this folder today: consumers read `MotionWarp`'s textures directly (or its snapshot), each re-checking validity with `motionValidity.Current(epoch)` plus sequence compares (`DejudderLiveFieldFor`, the mask slots' epochs, the B3 snapshot epoch).

### Threading

- `DejudderPolicy` and `VectorScale`: stateless (except `LatencyFloor`, owned by the render thread); called on the render thread. `MotionAheadBreakBefore` is called on the capture/dequeue path that submits to the optical-flow producer.
- `MotionSegment`: no locks; meant for the render thread. Only its `MotionValidity::State` is thread-safe (`Invalidate` from any thread).

### Config keys the code really reads

The folder's code reads no configuration itself. The host parses these in `ApplyDisplayRuleOverrides` (renderer profile sections, e.g. `[vprenderer.<profile>]`, the `profiles.<group>.<name>` sections and display rules) and passes the values in:

- `dejudder_strength`, `dejudder_strength_even` (0..1), `dejudder_even_rates` (`whole` / `whole_and_1001`) - `Decide`, `EffectiveStrength`.
- `occlusion_mask` (on/off) - `Decide` (the estimate runs for the mask alone).
- `general_rate_fixes` (on/off) - `IsFilmLike`, `CameraWindowPixels`.
- `timing_continuity_fixes` (on/off) - `MotionAheadBreakBefore` (pair times checked).
- `motion_history_fixes` (on/off) - `DolbyVisionNextPairs` (same DM matrices), `MatcherHistoryChained` (via `Settings::historyFixes`).
- `motion_engine` (`auto` / `vp` / `smooth_motion`; read by `MotionConfig.h` from the profile section and `[vprenderer.motion]`) - `ResolveEngine`.

The matcher/warp keys that fill `Dejudder::Settings` are listed under `motion.consumers` (warp side) and `motion.estimation` (matcher side). `MotionSegment` and `VectorScale` read no keys; no keys are planned.

### Fallbacks the user actually sees

- None from this folder. `Decide`'s reason shows on the de-judder status line (Ctrl+I / the status text, `PublishDejudderStatus`) and in the log; every consumer then takes its no-vector form.
- A vectors invalidation (resync, queue reset, gap, seek, drift drop, format change, smooth motion switch, settings / motion-source change with `motion_history_fixes`) is LOG ONLY: "De-judder: motion invalidated (<cause>; N in total); the vectors' users fall back until a fresh pair".
- The listed entry `Vectors: invalid (<reason>)` is produced by no code. `MotionSegment::FallbackEntry()` would produce "motion: matcher (optical flow unavailable)" after 8 consecutive matcher estimates, but the segment is not wired.

### What is needed to lift it

- `DejudderPolicy.h`: C++ standard library, `vprenderer/CaptureTimestampGap.h` and `VideoFrameEncoding.h` (VideoProcessor-Lib).
- `DejudderWarp.h`: libplacebo headers, `../estimation/NvOpticalFlow.h` (D3D11 / ATL `CComPtr`), `GeneralRate.h`, `MotionAhead::Flow`; the implementation `../consumers/DejudderWarp.cpp`.
- `MotionSegment.h`: `../estimation/MotionValidity.h` only. `VectorScale.h`: `<cmath>` only.

### Test it standalone

- CPU (VideoProcessor-Test): `MotionSegmentTests` and `PipelineSkeletonTests` (`VectorsScalePerAxis`) in `src/VP-V/tests`; `DejudderMatrix`, `CameraWindowMatrix`, `MotionAheadMatrix` (GeneralRateMatrixTests.cpp); `DolbyVisionPairingTests`, `DisplayRateHoldTests` (BugHunt20261017Tests.cpp); `Hunt2MotionHistoryTests`, `Hunt2DolbyVisionPairTests`; `MotionAheadBreakTests`; `DolbyVisionCaptureEncodingTests`; `H3TimingTests` (engine global vector scale); the `DejudderPolicy::Decide` / `PlanExtra` cases inside `ConfigurationReadCacheTests` (ConfigFileTests.cpp; no class is named DejudderPolicyTests).
- Run single-threaded and read the TRX:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Tests:MotionSegmentTests,PipelineSkeletonTests,DejudderMatrix,CameraWindowMatrix,MotionAheadMatrix,DolbyVisionPairingTests,DisplayRateHoldTests,Hunt2MotionHistoryTests,Hunt2DolbyVisionPairTests,MotionAheadBreakTests /Logger:"trx;LogFileName=vector_delivery.trx"`

- GPU coverage of `MotionWarp` is listed in `../consumers/README.md`.

## Contract

- Input: vectors at source size + per-axis factors + geometry
- Output: vectors in the consumer's coordinate space (scale x, scale y separately)
- Geometry: source -> processing -> output, per axis
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys (values the folder's code takes, parsed by the host): `dejudder_strength`, `dejudder_strength_even`, `dejudder_even_rates`, `occlusion_mask`, `general_rate_fixes`, `timing_continuity_fixes`, `motion_history_fixes`, `motion_engine`
- Planned keys: (none)
- Tiers: follows motion.estimation

## Fallbacks

- "Fallbacks engaged" entry: Vectors: invalid (<reason>) - listed, but NOT produced by any code (invalidations are log-only; see "How the code works")
- Behaviour: an invalidation moves the one MotionValidity epoch and every consumer takes its no-vector form until a fresh pair. A geometry map that is not known is not an invalidation today: with an unknown NLS shader map the host marks this frame's de-judder geometry invalid, the extras warp the source instead of the rendered picture and the sigma-delta takes no de-judder vectors (logged once: "De-judder extras: NLS shader map not known; warping the source instead"); MotionSegment's `Miss::Geometry` would cover it once wired.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `motion.estimation`, `scaling.rebase`, `scaling.route_4k`, `scaling.geometry`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- per-axis scaling unit tests (VectorScale): `PipelineSkeletonTests.VectorsScalePerAxis`
- MotionSegment: `MotionSegmentTests` (fetch, every discontinuity, straddling estimates, repeats/gaps/geometry, retract, gap-fill pairs, smooth motion exclusivity, low-latency backward pairs, visible matcher run)
- DejudderPolicy: `DejudderMatrix`, `DolbyVisionPairingTests`, `Hunt2DolbyVisionPairTests`, `MotionAheadBreakTests`, cases in `ConfigurationReadCacheTests`

Contract tests:
- sx != sy maps exactly; identity factors leave vectors unchanged (`VectorsScalePerAxis`)

Chain tests:
- single-axis re-base + de-judder (GPU; not a dedicated test yet)

## Known issues / open items

- +-1 px refinement for temporal dither / scaler refinements / texture restore (not built)
- `MotionSegment` and `VectorScale` are not wired into the host (P-b1 shadow step pending); consumers still fetch from `MotionWarp` with their own validity expressions
- The listed fallback entry `Vectors: invalid (<reason>)` is never produced; invalidations are log-only
- A scene cut is not a MotionValidity cause; each warp holds a cut pair through its frame trust only

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/core/VectorScale.h`
- `src/VP-V/modules/motion/vector_delivery/DejudderPolicy.h`
- `src/VP-V/modules/motion/vector_delivery/DejudderWarp.h`
- `src/VP-V/modules/motion/vector_delivery/MotionSegment.h`

Forwarding headers (old paths kept for Bill's files, the renderer host and RendererCaptureHarnessTests.cpp; removed at M2 step 0):

- `src/VideoProcessor-Lib/vprenderer/DejudderPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/DejudderWarp.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `dejudder_even_rates`, `dejudder_strength`, `dejudder_strength_even`, `general_rate_fixes`, `motion_engine`, `motion_history_fixes`, `occlusion_mask`, `timing_continuity_fixes`.
- Tiers: follows motion.estimation.
- VP-V page, Motion tab: tiers Classic / Low / Mid / Upper; tier keys `dejudder_est_qpel`, `dejudder_est_split`, `motion_ahead_frames`, `optical_flow`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#3-motion](../../../../../docs/settings/VP-settings-reference.md#3-motion).
- Fallback entry ("Fallbacks engaged"): none (control slot).

## Tests

- Placeholder (byte-identity on/off, zero cost): MISSING.
- Isolation (fixed inputs, expected outputs, cost): per-axis scaling unit tests (VectorScale).
- Contract (formats, geometry, fallback entry): sx != sy maps exactly; identity factors leave vectors unchanged.
- Chain (total delay at the 4/3/2-frame budgets): single-axis re-base + de-judder.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
