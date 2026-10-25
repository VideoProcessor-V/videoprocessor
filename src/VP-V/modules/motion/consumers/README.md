# Motion consumers (`motion.consumers`)

Unit: **motion** | side: processing | order: 140 | status: **built** | hook point: prepass

## Purpose / why

De-judder, hiccup repair, optional interpolation (exclusive with libplacebo smooth motion), and the temporal consumers (dither vectors, scaler refinements).

## How the code works

Checked against the code on 2026-10-25 (feat/m2-motion). The module is the de-judder's GPU matcher and warps, VP's 50/60p interpolator and its timeline, the estimate-ahead bookkeeping, the occlusion-mask hook and the sigma-delta's vector field. `TemporalRenderer.h` (libplacebo smooth motion / frame mixer / deinterlacing) is Bill's and stays read-only. The renderer host (`LibplaceboVideoRenderer.cpp`) owns every instance and all call sites.

### Public API

- `DejudderWarp.cpp`: implementation of `Dejudder::MotionWarp` (declared in `../vector_delivery/DejudderWarp.h`; this file is also listed by `motion.estimation`). `Estimate(A, B, occlusionSlot, grainError)` runs the block matcher (or the optical-flow engine's field as seed/candidate, `SetExternalFlow` for pairs computed ahead) and writes the block field (one texel per 16x16 source block: vx, vy, trust, error), frame trust (r = trusted share, g = the D2/D4/R-A strength scale), optional sub-field, B's occlusion mask, grain error, pixel-trust mask, camera vector. `Warp` / `WarpProcessed` (B3) warp A's planes by `shift`; `SnapshotForWarp` copies the warp inputs so extras keep reading (A, B) while (B, C) is estimated. `ForgetHistory` drops the temporal state. Pass timing (`SetPassTiming`) drains the GPU and is tests only.
- `RenderedWarp.h/.cpp` (namespace `DejudderRendered`, Stage 6d): `Cache` (a PRE_OUTPUT hook armed per source frame; copies A's rendered picture to rgba16f), `Warp::Run` (warps the cached picture through the crop / NLS map, Catmull-Rom, optional per-pixel displacement for the sigma-delta), `VectorField::Run` (the sigma-delta's VPVEC per 16x16 output block, also `toNext` for B's first present), `UseExternalVectors`, `Rounded`, `Frame`, `OutputOnly` (the extra renders with OUTPUT-stage hooks only).
- `OcclusionMaskHook.h/.cpp` (namespace `OcclusionMask`, Stage 6c): `Hook` (an OUTPUT hook after OutputEncodingRepair and before the sigma-delta; writes "seen in A" (1) / "new" (0) into alpha, with the grain amount when a grain map is set), `SetFrame`, `SetNls`, `SetGrain`, `DropMask`; `NlsMapGlsl()` shared with the rendered warp.
- `FrameInterpolator.h/.cpp` (namespace `Interpolator`): `FrameInterpolator::Run(A, B, Motion, t, fallback, out, estimateWidth, estimateHeight)`: bidirectional warp at phase t; `Motion::field == nullptr` (no valid vectors) gives the no-vector fallback (hold the nearest frame, or blend); a cut holds the nearest frame; `SetGrain` (preserve: low band blended, high band from the nearer frame, energy-matched).
- `InterpolationPolicy.h` (pure): `Decide(Input) -> Decision` (Off, SmoothMotion, LowLatency, Interlaced, UnknownRates, FilmToDejudder, FilmIntegerRatio, IntegerRatio, RatioBelowOne, NativeRgb, SubtitleComposed, NoNextFrame, else Interpolate), `WholeRatio` (relative tolerance 0.5%), `IsFilmLike` (legacy / measured; with `cadence_class_fixes` the class comes from the source rate alone), `Phase`, `RefreshPhase`, parsers for `interpolation`, `interpolation_fallback`, `interpolation_grain`, `source_class`.
- `InterpolationTimeline.h` (pure): `Timeline::Plan(generation, sequence, pts, nextPts, sourceHz, displayHz, discontinuity)` (the refreshes inside [A, B) with their phases; restart on a discontinuity / generation / rate change / jump, re-anchor beyond a quarter period, cadence repeat re-presents the last output, film update gets no refresh, at most 16 outputs), `SetRateTrim` (+-1%), `MaxSpanFrames` / `MaxPairSpanFrames` (the gap span as 4 periods of 23.976p in time with `general_rate_fixes`), `GapFillPair`, `GapFillStandalone`, `StandaloneGap`, `FieldPeriods` / `OnePeriodField`.
- `DejudderAhead.h`: `Dejudder::AheadSchedule` (`TryAhead` after A's first extra, `Consume` at the next frame, `Reset`, reason counters).

### Data flow

1. Decision, per source frame on the render thread: `DejudderPolicy::Decide` (vector_delivery) and `InterpolationPolicy::Decide`, then the standalone gap repair (`interpolation_fill_gaps`). The de-judder takes film-like sources it has a strength for; the interpolator takes the rest; libplacebo smooth motion (TemporalRenderer, `motion_engine` resolved by `DejudderPolicy::ResolveEngine`) excludes both.
2. Estimate: A and B (the next queued frame, uploaded to `dejudderNextTextures`) go to `MotionWarp::Estimate`; the optical-flow producer's pair is attached when `MotionValidity::PairValid`. With `dejudder_estimate_ahead` (rendered warp only) `AheadSchedule::TryAhead` snapshots (A, B) and estimates (B, C) in an extra's spare refresh.
3. Pictures:
   - de-judder extras (`PresentDejudderExtras`): `PlanExtra` picks each refresh's phase; shift = effective strength x phase. Rendered warp (`dejudder_warp_stage: rendered`, default): `Cache` copied A at PRE_OUTPUT, `Warp::Run`, then a 1:1 render with OUTPUT-stage hooks. Source warp: `MotionWarp::Warp` / `WarpProcessed` and a full re-render. Interpolating (Stage A): `FrameInterpolator::Run`.
   - per-refresh interpolation (Stage B): `InterpolationTimeline::Plan` per dequeued frame; `PrepareVpPhasePicture` / `RunVpPhaseInterpolation` draw each phase picture through the whole chain.
   - sigma-delta: `VectorField` (VPVEC) or the rendered warp's displacement (VPPIX) when `dither_vectors` is `auto` and the fork's texture API exists; B's occlusion mask through `OcclusionMask::Hook` on B's first refresh (`occlusion_mask`).
4. Validity: each consumer checks the epoch it was made in (`DejudderLiveFieldFor`, `dejudderMaskEpoch`, the B3 snapshot epoch). `ApplyMotionInvalidate` forgets the history, the ahead schedule and the mask slots.

### Threading

- Everything here runs on the render thread: the matcher's and warps' compute dispatches, `FrameInterpolator`, the policy and timeline. `Cache` and `OcclusionMask::Hook` run inside libplacebo's `pl_render_image` on the render thread (no locks; their state is set before the render).
- The optical-flow pairs computed ahead come from `MotionAheadProducer`'s worker (motion.estimation); `MotionWarp` only consumes them.
- `FrameInterpolator`'s grain weight cache (`LowWeightsGlsl`) is a function-static map behind a mutex.
- MotionValidity invalidations may arrive from any thread; they are applied at the render thread's apply point.

### Config keys the code really reads

The module reads no configuration itself. The host parses these in `ApplyDisplayRuleOverrides` (renderer profile sections `[vprenderer.<profile>]`, `profiles.<group>.<name>` sections and display rules) and `DejudderMatcherSettings`; `smooth_motion` / `frame_mixer` are read by `MotionConfig.h` (Bill's) from the profile section and `[vprenderer.motion]`:

- Strength and cadence: `dejudder_strength`, `dejudder_strength_even` (0..1), `dejudder_even_rates`.
- Warp path: `dejudder_warp_stage` (`rendered` / `source`), `dejudder_source_warp_filter` (`catmull` / `bilinear`), `dejudder_estimate_ahead` (on/off; AheadSchedule), `dejudder_frame_trust` (0..1; the cut threshold every warp and the interpolator use).
- Warp shaping (applied by both warps; D2/D4/R-A are computed in the estimate): `dejudder_motion_scale_lo`, `dejudder_motion_scale_hi`, `dejudder_trust_scale`, `dejudder_motion_rolloff`, `dejudder_roll_lo`, `dejudder_roll_hi`, `dejudder_roll_floor`, `dejudder_object_strength`, `dejudder_hold_feather_px`, `dejudder_hold_hysteresis`, `dejudder_block_taper`, `dejudder_taper_sad`, `dejudder_pixel_trust`, `dejudder_pixel_trust_lo`, `dejudder_pixel_trust_hi`, `dejudder_landing_check` (on/off = 1.5 px), `dejudder_occlusion_fill` (source warp only).
- Interpolation: `interpolation` (`off` / `auto` / `always`), `interpolation_fallback` (`hold` / `blend`), `interpolation_fill_gaps` (on/off), `interpolation_grain` (`preserve` / `blend`), `source_class` (`legacy` / `measured`), `cadence_class_fixes`, `general_rate_fixes`.
- Occlusion mask and dither: `occlusion_mask` (on/off), `dither_vectors` (`auto` / `own`).
- libplacebo smooth motion (TemporalRenderer): `smooth_motion` (`off` / `auto` / `always`), `frame_mixer`.

Not keys: `dejudder_hold` (only a change-log group name for the four hold/taper keys). The matcher keys (`dejudder_match_tolerance`, `dejudder_consistency_px`, `dejudder_search_range`, `dejudder_global_prior`, `dejudder_vector_smooth`, `dejudder_est_*`, `dejudder_global_two`, `dejudder_regional_hold`, `dejudder_cut_histogram`, `dejudder_texture_tolerance`, `dejudder_fade_match`, `dejudder_fade_floor`, `dejudder_verify`, `dejudder_verify_share`, `dejudder_low_res`, `optical_flow*`, `motion_ahead_frames`) reach `DejudderWarp.cpp` too but belong to motion.estimation. `dejudder_gate` gates the extras but is render-ahead's (output.presentation). TemporalRenderer.h also reads the deinterlacing keys (`deinterlace`, `deinterlace_algorithm`, `field_order`, `deinterlace_cadence`, `deinterlace_skip_spatial`), which no slot lists yet. No keys are planned.

### Fallbacks the user actually sees

Visible ("Fallbacks engaged", one-time notice):
- `entrance once fill`: the source warp's occlusion fill is off on B3's processed planes.
- `entrance once`: B3 pictures that took the per-picture stages.
- `look-ahead`: fewer queued frames than the design depth (estimate-ahead and the pairs degrade).

Status line / log only (no entry):
- Vectors invalidated: every consumer takes its no-vector form (log line "De-judder: motion invalidated ...").
- A failed warp, rendered warp or interpolation: the extra is not shown, the refresh holds A ("De-judder: hold (<why>)" every 5 s, "held N" on the status line); a failed per-refresh interpolation shows the source frame (log every 2400 failures).
- Rendered warp to source warp: an unknown NLS shader map (logged once); a failed PRE_OUTPUT cache copy is not even logged (`Cache::Failed()` is never read).
- Occlusion fill ignored with the rendered warp (logged once); occlusion-mask hook failures (counted in a periodic log line).
- The optical-flow engine failing (matcher for the session; motion.estimation): one log line.
- De-judder / interpolation off reasons (short queue, RGB capture, smooth motion, ...) show as the status text.

The listed entry `Motion consumers: no-motion form (vectors invalid / short queue)` is produced by no code.

### What is needed to lift it

libplacebo (gpu, dispatch, renderer, shaders), D3D11 (the optical-flow engine path in `DejudderWarp.cpp`), `../vector_delivery/DejudderWarp.h` and `DejudderPolicy.h`, `../estimation/NvOpticalFlow.h`, `MotionAhead.h`, `OpticalFlowTrust.h`, `VP-V/modules/scaling/geometry/NlsMaskMapping.h`, `GeneralRate.h`, the grain helpers (`GrainPreserve`). The pure headers (`InterpolationPolicy.h`, `InterpolationTimeline.h`, `DejudderAhead.h`) need only the standard library (InterpolationPolicy also `DejudderPolicy.h`).

### Test it standalone

- CPU (VideoProcessor-Test): `InterpolationPolicyTests`, `InterpolationTimelineTests`, `InterpolationDebandHoldTests`, `InterpolationDecisionMatrix`, `GapSpanMatrix`, `TimelineSpanMatrix` (GeneralRateMatrixTests.cpp), `Hunt2CadenceClassPolicyTests`, `RenderAheadGapFillTests`, the AheadSchedule cases in `LibplaceboLutParserTests`.
- GPU (D3D11; WARP unless `VP_TEST_GPU=hardware`; run only when GPU tests are allowed): `LibplaceboLutParserTests` `Dejudder*` (27 methods: warp, rendered warp, hold at a cut, occlusion fill, ...), `OcclusionMask*`, `InterpolatorKeepsGrainRmsAtEveryPhase`, `InterpolatorGrainNotSmearedAcrossFrames`; VideoProcessor-BetaTests (`BetaGpuTests.cpp`, MotionWarp corpora; read the TRX) and `src/VP-V/tests/gpu/MotionTimingGpuTests.inc`.

Run single-threaded and read the TRX:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Tests:InterpolationPolicyTests,InterpolationTimelineTests,InterpolationDebandHoldTests,InterpolationDecisionMatrix,GapSpanMatrix,TimelineSpanMatrix,Hunt2CadenceClassPolicyTests,MotionSegmentTests /Logger:"trx;LogFileName=consumers.trx"`

## Contract

- Input: pictures at the processing size + delivered vectors + validity
- Output: the picture for this refresh's phase
- Geometry: processing size
- Delay share: 0 extra frames; uses the shared queue
- Look-ahead: de-judder pair (1), estimate-ahead (2), hiccup spare (1) - all degrade on a short queue

## Settings

- Existing keys: see "Config keys the code really reads" above (35 keys; `dejudder_hold` is not a key)
- Planned keys: (none)
- Tiers: Classic off .. Upper full

## Fallbacks

- "Fallbacks engaged" entry: Motion consumers: no-motion form (vectors invalid / short queue) - listed, but NOT produced by any code; the entries that exist are `entrance once`, `entrance once fill` and `look-ahead`
- Behaviour: each consumer falls back as the vector rules say (hold A, the interpolator's hold/blend, no VPVEC, mask dropped); see "Fallbacks the user actually sees" for which are visible.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `motion.vector_delivery`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- DejudderWarp tests: `LibplaceboLutParserTests` `Dejudder*` (GPU), VideoProcessor-BetaTests `BetaGpuTests.cpp`
- InterpolationTimeline tests: `InterpolationTimelineTests`, `TimelineSpanMatrix`, `GapSpanMatrix`
- InterpolationPolicy: `InterpolationPolicyTests`, `InterpolationDecisionMatrix`, `Hunt2CadenceClassPolicyTests`
- PulldownDetector tests (input.queue_timing's detector; not this module's code)

Contract tests:
- exclusive with libplacebo smooth motion by profile rules (`Decide` SmoothMotion cases)

Chain tests:
- 24p in 60 at 200 ms, 4/3/2 frames

## Known issues / open items

- The listed fallback entry is never produced; warp/interpolation failures and the rendered-to-source warp downgrade are log/status only
- `RenderedWarp` cache copy failures are counted but never read or shown
- KNOWN GAP (audit item 3): the consumers run before the entrance; B3 (ADR-0024) warps the processed frame

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/motion/consumers/DejudderWarp.cpp`
- `src/VP-V/modules/motion/consumers/DejudderAhead.h`
- `src/VP-V/modules/motion/consumers/RenderedWarp.cpp`
- `src/VP-V/modules/motion/consumers/FrameInterpolator.cpp`
- `src/VP-V/modules/motion/consumers/InterpolationTimeline.h`
- `src/VP-V/modules/motion/consumers/InterpolationPolicy.h`
- `src/VP-V/modules/motion/consumers/OcclusionMaskHook.cpp`
- `src/VideoProcessor-Lib/vprenderer/TemporalRenderer.h`
- `src/VP-V/modules/motion/consumers/RenderedWarp.h`
- `src/VP-V/modules/motion/consumers/FrameInterpolator.h`
- `src/VP-V/modules/motion/consumers/OcclusionMaskHook.h`
- `src/VP-V/modules/motion/consumers/DejudderCacheStage.h`
- `src/VP-V/modules/motion/consumers/DejudderCacheStage.cpp`

Forwarding headers (old paths kept for Bill's files, the renderer host and RendererCaptureHarnessTests.cpp; removed at M2 step 0):

- `src/VideoProcessor-Lib/vprenderer/DejudderAhead.h`
- `src/VideoProcessor-Lib/vprenderer/FrameInterpolator.h`
- `src/VideoProcessor-Lib/vprenderer/InterpolationPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/InterpolationTimeline.h`
- `src/VideoProcessor-Lib/vprenderer/OcclusionMaskHook.h`
- `src/VideoProcessor-Lib/vprenderer/RenderedWarp.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `cadence_class_fixes`, `dejudder_block_taper`, `dejudder_estimate_ahead`, `dejudder_even_rates`, `dejudder_frame_trust`, `dejudder_hold_feather_px`, `dejudder_hold_hysteresis`, `dejudder_landing_check`, `dejudder_motion_rolloff`, `dejudder_motion_scale_hi`, `dejudder_motion_scale_lo`, `dejudder_object_strength`, `dejudder_occlusion_fill`, `dejudder_pixel_trust`, `dejudder_pixel_trust_hi`, `dejudder_pixel_trust_lo`, `dejudder_roll_floor`, `dejudder_roll_hi`, `dejudder_roll_lo`, `dejudder_source_warp_filter`, `dejudder_strength`, `dejudder_strength_even`, `dejudder_taper_sad`, `dejudder_trust_scale`, `dejudder_warp_stage`, `dither_vectors`, `frame_mixer`, `general_rate_fixes`, `interpolation`, `interpolation_fallback`, `interpolation_fill_gaps`, `interpolation_grain`, `occlusion_mask`, `smooth_motion`, `source_class`.
- Tiers: Classic off .. Upper full.
- VP-V page, Motion tab: tiers Classic / Low / Mid / Upper; tier keys `dejudder_est_qpel`, `dejudder_est_split`, `motion_ahead_frames`, `optical_flow`.
- Listed under the tier: `interpolation_fill_gaps`, `interpolation`, `dither_vectors`, `interpolation_fallback`, `intent_motion`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#3-motion](../../../../../docs/settings/VP-settings-reference.md#3-motion).
- Fallback entry ("Fallbacks engaged"): none (control slot).

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): DejudderWarp tests; InterpolationTimeline tests; PulldownDetector tests.
- Contract (formats, geometry, fallback entry): exclusive with libplacebo smooth motion by profile rules.
- Chain (total delay at the 4/3/2-frame budgets): 24p in 60 at 200 ms, 4/3/2 frames.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
