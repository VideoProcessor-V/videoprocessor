# Motion consumers (`motion.consumers`)

Unit: **motion** | side: processing | order: 140 | status: **built** | hook point: prepass

## Purpose / why

De-judder, hiccup repair, optional interpolation (exclusive with libplacebo smooth motion), and the temporal consumers (dither vectors, scaler refinements).

## How the code works

Checked against the code on 2026-10-25 (feat/m2-motion). The module is the de-judder's GPU matcher and warps, VP's 50/60p interpolator and its timeline, the estimate-ahead bookkeeping, the occlusion-mask hook and the sigma-delta's vector field. `TemporalRenderer.h` (libplacebo smooth motion / frame mixer / deinterlacing) is Bill's and stays read-only. The renderer host (`LibplaceboVideoRenderer.cpp`) owns every instance and all call sites.

### Public API

- `DejudderWarp.cpp`: implementation of `Dejudder::MotionWarp` (declared in `../estimation/matcher/DejudderWarp.h`; it is listed by `motion.estimation` and lives in `../estimation/matcher/`). `Estimate(A, B, occlusionSlot, grainError)` runs the block matcher (or the optical-flow engine's field as seed/candidate, `SetExternalFlow` for pairs computed ahead) and writes the block field (one texel per 16x16 source block: vx, vy, trust, error), frame trust (r = trusted share, g = the D2/D4/R-A strength scale), optional sub-field, B's occlusion mask, grain error, pixel-trust mask, camera vector. `Warp` / `WarpProcessed` (B3) warp A's planes by `shift`; `SnapshotForWarp` copies the warp inputs so extras keep reading (A, B) while (B, C) is estimated. `ForgetHistory` drops the temporal state. Pass timing (`SetPassTiming`) drains the GPU and is tests only.
- `RenderedWarp.h/.cpp` (namespace `DejudderRendered`, Stage 6d): `Cache` (a PRE_OUTPUT hook armed per source frame; copies A's rendered picture to rgba16f), `Warp::Run` (warps the cached picture through the crop / NLS map, Catmull-Rom, optional per-pixel displacement for the sigma-delta), `VectorField::Run` (the sigma-delta's VPVEC per 16x16 output block, also `toNext` for B's first present), `UseExternalVectors`, `Rounded`, `Frame`, `OutputOnly` (the extra renders with OUTPUT-stage hooks only).
- `OcclusionMaskHook.h/.cpp` (namespace `OcclusionMask`, Stage 6c): `Hook` (an OUTPUT hook after OutputEncodingRepair and before the sigma-delta; writes "seen in A" (1) / "new" (0) into alpha, with the grain amount when a grain map is set), `SetFrame`, `SetNls`, `SetGrain`, `DropMask`; `NlsMapGlsl()` shared with the rendered warp.
- `FrameInterpolator.h/.cpp` (namespace `Interpolator`): `FrameInterpolator::Run(A, B, Motion, t, fallback, out, estimateWidth, estimateHeight)`: bidirectional warp at phase t; `Motion::field == nullptr` (no valid vectors) gives the no-vector fallback (hold the nearest frame, or blend); a cut holds the nearest frame; `SetGrain` (preserve: low band blended, high band from the nearer frame, energy-matched).
- `InterpolationPolicy.h` (pure): `Decide(Input) -> Decision` (Off, SmoothMotion, LowLatency, Interlaced, UnknownRates, FilmToDejudder, FilmIntegerRatio, IntegerRatio, RatioBelowOne, NativeRgb, SubtitleComposed, NoNextFrame, else Interpolate), `WholeRatio` (relative tolerance 0.5%), `IsFilmLike` (legacy / measured; with `cadence_class_fixes` the class comes from the source rate alone), `Phase`, `RefreshPhase`, parsers for `interpolation`, `interpolation_fallback`, `interpolation_grain`, `source_class`.
- `InterpolationTimeline.h` (pure): `Timeline::Plan(generation, sequence, pts, nextPts, sourceHz, displayHz, discontinuity)` (the refreshes inside [A, B) with their phases; restart on a discontinuity / generation / rate change / jump, re-anchor beyond a quarter period, cadence repeat re-presents the last output, film update gets no refresh, at most 16 outputs), `SetRateTrim` (+-1%), `MaxSpanFrames` / `MaxPairSpanFrames` (the gap span as 4 periods of 23.976p in time with `general_rate_fixes`), `GapFillPair`, `GapFillStandalone`, `StandaloneGap`, `FieldPeriods` / `OnePeriodField`.
- `DejudderAhead.h`: `Dejudder::AheadSchedule` (`TryAhead` after A's first extra, `Consume` at the next frame, `Reset`, reason counters).

### De-judder module (M2 P5b)

The de-judder policy and the per-vsync timeline moved out of the renderer file (doc 59 section 2):

- `DejudderState.h`: `VpMotion::DejudderPolicyState`, the policy's and the timeline's plain state (moved verbatim). The renderer keeps one reference alias per member (table below).
- `DejudderPolicyRunner.h`: `RunDejudderDecision` (the decision part of `UpdateDejudderEstimate`: the de-judder reason, extras, strength, even cycle, the interpolation decision, the standalone gap repair, the status publish and reason logs, the idle engine keeper) and `LogInterpolationDecisionFor`, templates over the settings and a host (every renderer access is a host call at the old point). `DejudderPolicyRunner.inl` (renderer `Impl` members): `RunDejudderPolicy` (the old body around the decision), `ImplDecisionHost`, `LogDejudderAhead`, `DejudderHold`, `FlushDejudderHolds`. `UpdateDejudderEstimate` stays a one-line forwarder with its 15 arguments packed into `DejudderEstimateCall.h` (same order, same values).
- `PhaseTimeline.inl` (renderer `Impl` members): the phase picture (`RunVpPhaseInterpolation`, `PrepareVpPhasePicture`, `VpPhaseDrawn`), `TimelineDisplayHz`, `VpTimelineWanted`, `LogVpTimeline`, `PublishDejudderStatus`, `ReleaseDejudder`, Stage B's no-refresh frames (`VpStageBFrame`), the draw piece (`DrawVpPhasePicture`).
- `DejudderExtension.h/.cpp`: the hub module `motion.dejudder` (hub order Resync -> VectorEngine -> Camera -> Dejudder -> Entrance -> CoreSegments). The renderer registers one `DejudderHostBinding`. `Query("vpv.dejudder.v1")` / `Query("vpv.phase_timeline.v1")` answer an `IDejudderDirectory` (`DejudderInterface.h`); a consumer asks it for its renderer's `IDejudder` (reason, effective strength, even cycle, extras allowed, interpolating, estimated sequence, `Hold(why)`, `FieldChoice` = the K8 insertion point, 0 = today's) or `IPhaseTimeline` (active, skip, phase, content rate); `DejudderViews.h` implements both over the state.
- Threading: render thread. The policy runs in `RenderLocked`; the timeline plan runs in `RenderLoop` before it, on the same thread and with `renderMutex` HELD (the loop's `unique_lock`, released only inside `WaitForDisplaySlot`, between draws).

Aliases left in the renderer (`LibplaceboVideoRenderer.cpp`, one VPV-HOOK block after the `dejudderPolicy` member); each is deleted by the package that moves its last reader outside this module:

| Alias | Readers left outside the module | Deleted by |
|---|---|---|
| `dejudderLatencyFloor`, `dejudderCallLatency`, `dejudderLoggedFloor`, `dejudderPhaseGeneration`, `dejudderPhaseLogPending`, `dejudderPredictedVsync`, `dejudderPredictedGeneration`, `dejudderPredictedSequence` | `PresentDejudderExtras`; `LogRenderAhead` (floor); `RetireOutput` / `RenderLocked` (prediction reset) | P5c (extras), P6 (render-ahead log) |
| `dejudderReason`, `dejudderExtrasAllowed` | `RepeatReuseContext`, `RetireOutput` (teardown log), `PresentDejudderExtras`, `RenderLocked` (extras gate, `pin.dejudderEstimating`), the render loop's display-repeat either/or | P5c / P6 (IDejudder) |
| `dejudderEffectiveStrength`, `dejudderEvenCycle`, `diagLastHoldCode`, `dejudderAheadLogTick` | `PresentDejudderExtras`, `UnpackRawPayload` area declarations | P5c |
| `dejudderReasonChangesNotLogged`, `dejudderReasonLogTick`, `diagDejudderModeTick`, `diagDejudderModeSkipped` | `RetireOutput` (the last reason line at teardown) | P5c / P11 |
| `interpolationDecision`, `interpolating`, `interpolationReasonLogged`, `interpolationLogTick`, `interpolationLogCount` | `RepeatReuseContext`, `BuildSigmaDeltaVectorsWithoutExtras`, `PresentDejudderExtras`, the render loop's dequeue (gap pair) | P5c / P10 |
| `smoothMotionEngine`, `motionEngineLogged`, `smoothMotionMixingLogged` | `RepeatReuseContext`, `VectorDemandInput` | P4 / P5a-rest |
| `dejudderMaskRateLogged`, `dejudderSourceHz` | `RenderAheadChainRatio`, `RenderLocked` (deband weight, pin) | P6 / P4 |
| `vpTimeline`, `vpTimelineActive`, `vpSkipOutput`, `vpSkipIsUpdate`, `vpFilmUpdates`, `vpOutputPhase`, `vpTimelineLogged`, `vpCaptureRateHz`, `vpGapPhaseScale`, `vpQueueSteering`, `vpQueueGeneration`, `vpQueueErrorReset`, `timelineDisplayRateHold` | `RenderLocked` (scene correction gate, upload reuse, deband content rate), `HoldTicksUntilDueLocked`, the render loop's draw loop (`vpOutputPhase`) and display-repeat either/or, `BuildSigmaDeltaVectorsWithoutExtras` | P6 (IPhaseTimeline) / P10 / P4 |

Hooks and call sites (M2 P5b): R10 `PlanOutputs` (render loop, renderMutex held: `Impl::PlanVpOutputs`, the smooth-motion mixing log and `TimelinePlanner.h`'s `PlanVpTimeline`), `OnQueue(BacklogFlush)` (the backlog flush that restarts the timeline's steering; appended to H1's `QueueEvent`, distinct from `Flush` = the queue generation's arm), and in-place calls: `ResolveMotionEngine` (the motion_engine either/or), `UpdateDejudderEstimate` -> `RunDejudderPolicy`, `VpStageBFrame`, `DrawVpPhasePicture`, `RegisterDejudderCacheFrame`, `ApplyMotionInvalidate` -> `TakeDejudderHistoryAction` (doc 50 Q4: the `motion-history` reader action; R2 off / shadow / unregistered = the epoch path as before). Unchanged and visible: the "display repeat" fallback line (present_every_vsync paused while VP schedules the refreshes), the D3 `SyncFallback` of the phase interpolation, render-ahead's display-rate fallback (output.presentation, `SelectDejudderDisplayRate`).

H1 amendment R10a (M2 P5b-5, appended only): `QueueEvent::BacklogFlush` (raised by the render loop's backlog flush when it restarts the timing; route: `NotifyQueue` -> hub -> `DejudderExtension::OnQueue` -> the binding -> `vpQueueErrorReset`, exchanged by the next plan; every other `OnQueue` handler ignores it, ResyncExtension does not arm on it), `OutputPlan::arrivalQpc`, `OutputPlan::queueTimingFixes`. `TimelinePlanner.h` holds `PlanVpTimeline`, `TimelineNotPlanned` (no plan this frame - no binding / hub, or a contained throw: no stale timeline flags, the visible "timeline" fallback) and `VpTimelineLine`; `PhaseTimeline.inl` holds `Impl::ResolveMotionEngine`, `Impl::PlanVpOutputs`, `Impl::NoteTimelinePlanned`.

Declared substitutions in the moved code (each the old statement, at the old point): the decision's renderer reads as host calls (`ImplDecisionHost`: the telemetry snapshot, the payload / plane checks, the intent scale, `TimelineDisplayHz`, the clock, the log, the report-a-problem record, the status publish / clear, the engine keeper); the timeline's renderer reads as host calls (`ImplTimelineHost`: the resync reader, the QPC frequency - now read whenever the timeline plans, still pure -, the reader generation, `TimelineDisplayHz`, `VpTimelineWanted`, `LogVpTimeline`); `holdFraction` / `queueTimingFixes` read at the call site (`CurrentQueueHoldFraction`); `LogVpTimeline` formats through `VpTimelineLine` (a 512-byte buffer, then `DebugLog::Log("%s", ...)`: the same text); the draw piece's pure `phasePicture` test after `bAlreadyA`'s first evaluation; `submittedNls != nullptr` evaluated by the cache stage's caller.

Skeleton products (doc 70): the module consumes the vector engine's pairs (P2 forward pairs for the estimate, the P3 field for the warp / interpolator / sigma-delta) through `IVectorEngine` and the renderer's estimate state; it declares one private frame history, the rendered-picture cache (`DejudderRendered::Cache`, A's PRE_OUTPUT picture), justified because the extras and the rendered warp redraw A's processed picture at the refreshes A owns without re-running the chain; it is released with the GPU (`ReleaseDejudder`) and invalidated at every motion invalidation and NLS retry. The policy and timeline state (`DejudderPolicyState`) hold no frames.

Identity tests (CPU, recorded from test-only verbatim copies of the code before the move; the moved code must reproduce them): `DejudderDecisionTableTests` (96 rows, 4 sequences: copy and moved runner), `TimelinePlanTraceTests` (13 scenarios, 236 frames: copy and moved planner), `DejudderHistoryActionTests`, `DejudderExtensionHookTests` (the no-plan guard, the BacklogFlush / PlanOutputs routes through the hub), `ResyncExtensionTests::BacklogFlushDoesNotArm`.

Footprint (`tools/vp_footprint.py`, LibplaceboVideoRenderer.cpp stray lines): 7468 at fork/integ/batch1 746af45b0, 6707 after P5b-5 (P5b-6/7 add one call / one line each).

Open items:
- Owner change, this reset: the de-judder cache stage (`RegisterDejudderCacheFrame`) and the draw piece (`DrawVpPhasePicture`) stay IN PLACE in `RenderLocked`, one call each at today's position; they are retired when P4 moves the hook block (`CollectHooks` / `OnDraw`).
- R2 activation (W1-C2a MUST 2, closed): the apply point (`ApplyMotionInvalidate`) takes from `vectorEngine.motionResyncReaders`, the readers the resync "motion" provider registers on each era's Indicator; with `resync_event` on the reader drives `TakeDejudderHistoryAction` (its release and a rebase-axis-only forget act: on-mode-only behaviour change), off / shadow keep the epoch path. The unregistered `DejudderState::motionHistory` copy was removed.
- The status line's tail segments and the release (`ReleaseDejudder`) are moved verbatim but have no CPU identity test (GPU state); the chain-gate motion subset covers them.
- With no hub module registered (a renderer built without HubBootstrap, or a test that clears the registry), `PlanOutputs` is not answered: the timeline does not plan, the host's own outputs are drawn and the "timeline" fallback is shown (`NoteTimelinePlanned`).

### Data flow

1. Decision, per source frame on the render thread: `DejudderPolicy::Decide` (this folder) and `InterpolationPolicy::Decide`, then the standalone gap repair (`interpolation_fill_gaps`). The de-judder takes film-like sources it has a strength for; the interpolator takes the rest; libplacebo smooth motion (TemporalRenderer, `motion_engine` resolved by `DejudderPolicy::ResolveEngine`) excludes both.
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
- `timeline`: the per-refresh timeline did not plan this frame (the de-judder module is not registered for the renderer, or its plan failed); frames are shown at their own times.
- `display repeat` (output.presentation's entry, raised here): present_every_vsync paused while VP's per-refresh timeline / de-judder extras schedule the refreshes.
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

libplacebo (gpu, dispatch, renderer, shaders), D3D11 (the optical-flow engine path in `DejudderWarp.cpp`), `../estimation/matcher/DejudderWarp.h` and `DejudderPolicy.h` (this folder), `../estimation/NvOpticalFlow.h`, `MotionAhead.h`, `OpticalFlowTrust.h`, `VP-V/modules/scaling/output/NlsMaskMapping.h`, `GeneralRate.h`, the grain helpers (`GrainPreserve`). The pure headers (`InterpolationPolicy.h`, `InterpolationTimeline.h`, `DejudderAhead.h`) need only the standard library (InterpolationPolicy also `DejudderPolicy.h`).

### Test it standalone

- CPU (VideoProcessor-Test): `InterpolationPolicyTests`, `InterpolationTimelineTests`, `InterpolationDebandHoldTests`, `InterpolationDecisionMatrix`, `GapSpanMatrix`, `TimelineSpanMatrix` (GeneralRateMatrixTests.cpp), `Hunt2CadenceClassPolicyTests`, `RenderAheadGapFillTests`, the AheadSchedule cases in `LibplaceboLutParserTests`; M2 P5b recorded identity tables `DejudderDecisionTableTests` (the de-judder / interpolation decision, tests/DejudderDecisionRows.inl) and `TimelinePlanTraceTests` (the per-vsync timeline block, tests/TimelinePlanTraceRows.inl), from test-only verbatim copies of today's code. Their scope: the decision part and the status line's head (the ahead / render-ahead / dither / pulldown / mask / motion-source tail segments of the status line do not depend on the decision and are out of scope); the pair validity check after the decision is covered by the existing `MotionValidity*` tests. Rows and sequences are written as neutral Facts, replayed through the copy and (from P5b-3) the moved code.
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

- `src/VP-V/modules/motion/consumers/DejudderPolicy.h`
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
- Fallback entry ("Fallbacks engaged"): `motion consumers: <warp failed | rendered warp failed | interpolation failed>: the source frame is shown instead`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): DejudderWarp tests; InterpolationTimeline tests; PulldownDetector tests.
- Contract (formats, geometry, fallback entry): exclusive with libplacebo smooth motion by profile rules.
- Chain (total delay at the 4/3/2-frame budgets): 24p in 60 at 200 ms, 4/3/2 frames.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->

## Filler edge entry (K8-1, pure unit, not wired)

`FillerEdgeEntry.h` is the CPU mirror of the trust / occlusion filler's per-block decision: an untrusted camera block takes the
P8 model vector (accessor Step 2); a filled block whose back-projection (`p - t v`, `p + (1 - t) v`) leaves the active picture is
new content and is never filled with warped pixels. Not called by `DejudderWarp` yet (needs the P5b motion extension and K8-2).
Tests: `tests/FillerEdgeEntryTests.cpp`.

## All settings (complete list)

Config placement is the master VP-V page sub-page named in the column (Advanced = measured internals; Main = taste master / switch cards). "Original (Bill)" = what the Original pathway does with the key (`core/OriginalPathway.h`). The module reads no key itself; the host parses them. Defaults and placement come from `docs/settings/coverage-audit.md`; ranges from `RendererProfileConfig.h`. The 2026-11 camera work adds no key here (K3 adds `camera_*` keys); `FillerEdgeEntry.h` has no setting.

| Key | Section | Type | Range | Default | Config placement | What Original (Bill) does | Visible fallbacks / status lines | Tier differences |
|---|---|---|---|---|---|---|---|---|
| `cadence_class_fixes` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | On | VP-V > Proposed retirement | forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_block_taper` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | Off | VP-V > Proposed retirement | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_estimate_ahead` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | On | VP-V > Advanced (Main) | forced off | `look-ahead` entry when fewer frames are queued than the design depth | none |
| `dejudder_even_rates` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | whole, whole_and_1001, 30, 30_and_29.97 | whole | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_frame_trust` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..1 | 0.25 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_hold_feather_px` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..16 px | 4 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_hold_hysteresis` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | integer | 1..8 | 2 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_landing_check` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | On | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_motion_rolloff` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | Off | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_motion_scale_hi` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..32 | 1.5 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_motion_scale_lo` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..16 | 0.25 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_object_strength` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..1 | 1 (off) | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_occlusion_fill` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | Off | VP-V > Proposed retirement | not forced; no effect while dejudder_strength / interpolation are forced off | `entrance once fill` entry when B3 pictures skip the fill; "ignored with the rendered warp" (log once) | none |
| `dejudder_pixel_trust` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | On | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_pixel_trust_hi` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0.5..32 | 8 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_pixel_trust_lo` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0.25..16 | 3 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_roll_floor` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..1 | 0.25 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_roll_hi` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..400 | 24 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_roll_lo` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..200 | 8 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_source_warp_filter` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | catmull, bilinear | catmull | VP-V > Proposed retirement | forced bilinear | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_strength` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..1 | 0 = off; the slider sets it | VP-V > Advanced | forced 0 (off) | status text "De-judder: hold (<why>)" and off reasons; vectors invalid = no-motion form (log) | none |
| `dejudder_strength_even` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..1 | 0 = off | VP-V > Advanced | forced 0 (off) | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_taper_sad` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..32 | 0 | VP-V > Proposed retirement | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_trust_scale` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | number | 0..1 | 0.6 | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `dejudder_warp_stage` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | rendered, source | rendered | VP-V > Proposed retirement | not forced; no effect while dejudder_strength / interpolation are forced off | unknown NLS shader map: rendered warp falls back to the source warp (log once) | none |
| `dither_vectors` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | auto, own | De-judder's (auto) | Rendering > Profile details (Proposed retirement) | not forced; dither follows its own vectors when the extras are off | none of its own (see "Fallbacks the user actually sees") | none |
| `frame_mixer` | `[vprenderer.motion]` / profile | choice | oversample, hermite, linear, cubic, mitchell_clamp | Oversample | Motion > Profile details | Bill's key, read by MotionConfig.h as upstream | none of its own (see "Fallbacks the user actually sees") | none |
| `general_rate_fixes` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | On | VP-V > Proposed retirement | forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `interpolation` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | off, auto, always | Off | VP-V > Advanced (Main) | forced off | status text off reasons (short queue, RGB capture, smooth motion); failed interpolation: `motion consumers: interpolation failed: the source frame is shown instead` | none |
| `interpolation_fallback` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | hold, blend | hold | VP-V > Advanced (Main) | forced hold | none of its own (see "Fallbacks the user actually sees") | none |
| `interpolation_fill_gaps` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | off, on | On | VP-V > Advanced (Main) | forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `interpolation_grain` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | preserve, blend | preserve | VP-V > Proposed retirement | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |
| `occlusion_mask` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | on/off | on, off | Off | Rendering > Profile details (Main) | forced off | hook failures counted in a periodic log line; mask dropped (no entry) | none |
| `smooth_motion` | `[vprenderer.motion]` / profile | choice | off, auto, always | Off | Motion > Profile details | Bill's key, read by MotionConfig.h as upstream | exclusive with VP motion by profile rules (`motion_engine`) | none |
| `source_class` | profile (`[vprenderer.<profile>]`, `profiles.*`, display rules) | choice | legacy, measured | legacy | VP-V > Advanced | not forced; no effect while dejudder_strength / interpolation are forced off | none of its own (see "Fallbacks the user actually sees") | none |

Tiers: the Motion tier (Classic / Low / Mid / Upper) sets only `dejudder_est_qpel`, `dejudder_est_split`, `motion_ahead_frames`, `optical_flow` (motion.estimation); it never touches the de-judder amount.

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.

## Folded in: the former `motion/vector_delivery` folder (slot `motion.vector_delivery`)

The folder was retired by consolidation W0-1 (2026-11-10, pure renames): `DejudderPolicy.h` now lives in this folder, `DejudderWarp.h` and `DejudderWarp.cpp` in `../estimation/matcher/` (the P3 producer), `MotionSegment.h` in `../estimation/`. `src/VP-V/core/VectorScale.h` is listed in this module's MANIFEST. One forwarder remains at the old path `vector_delivery/MotionSegment.h` (LibplaceboVideoRenderer.cpp:13 includes it; Bill's file takes no hunk) and goes at the next Bill-merge window. The slot id `motion.vector_delivery` stays in the slot table. Its former README follows, unchanged except for the paths.

Unit: **motion** | side: processing | order: 130 | status: **partial** | hook point: host

### Purpose / why

Hand source-size vectors to consumers at the processing size: PER-AXIS scaling (re-base/4K route give different x/y factors), mapping through crop/A-lens/NLS, and an optional +-1 px local refinement for precision consumers.

### How the code works

Checked against the code on 2026-10-25 (feat/m2-motion). What the folder holds today is three different things: the de-judder's policy rules (`DejudderPolicy.h`), the matcher/warp's public types (`DejudderWarp.h`, implemented in `../estimation/matcher/DejudderWarp.cpp`), and two NOT YET WIRED pieces of the target design (`MotionSegment.h`, `src/VP-V/core/VectorScale.h`). The per-axis delivery the purpose describes is not a separate step in the running renderer: the warps and the interpolator map positions in their shaders (estimate px to plane px from the real texture sizes, `FrameInterpolator::Run(..., estimateWidth, estimateHeight)`, the rendered warp's crop/NLS map slope).

#### Public API

- `DejudderPolicy.h` (pure, header-only, namespace `DejudderPolicy`):
  - `Decide(Input) -> Reason`: whether the motion estimate runs for this source frame (first failing check wins: Off, SmoothMotion, LowLatency, Interlaced, UnknownRates, IneligibleCadence, VideoRateMaskOnly, NativeRgb, PlaneFormat, NoNextFrame, NextFrameDiffers, else Active). The occlusion mask alone starts it; the grain map never does (`Input::grainMap` is carried for the reason text only).
  - `EffectiveStrength` / `ExtrasAllowed`: `dejudder_strength` on an uneven cadence, `dejudder_strength_even` on a whole cadence of a selected rate (`IsRate1001`, `EvenRateSelected`), 0 otherwise, film-like sources only.
  - Rate/cadence helpers, all from measured rates: `CadenceRatio`, `IsFilmLike(hz, generalRates)` (video from 49 Hz = 50 Hz family less 2%; legacy fixed bands when `general_rate_fixes` is off), `IsUnevenCadence`, `EvenCycle` (tolerance +-0.05 refreshes), `MaxExtras` (capped at 15).
  - `PlanExtra(ExtraInput) -> ExtraDecision`: whether the next refresh is still A's (stops: next frame arrived / near, past the next frame, extra limit); `LatencyFloor` (5th percentile of first-vsync minus callback over 5 s at any rate); `ExtrasBetween` / `MainPresentFor` (present ids without extras for the frame statistics).
  - `ResolveEngine(motion_engine, vpConfigured, smoothConfigured)`: VP motion or libplacebo smooth motion, never both; `Configured`.
  - Pairing rules: `ClassifyDolbyVisionFrame/Payload`, `DolbyVisionPairTrusted`, `DolbyVisionNextPairs` (byte-equal DM matrices with `motion_history_fixes`), `MotionAheadBreakBefore` (the optical-flow producer's break: gap, film update, non-adjacent pair times with `timing_continuity_fixes`, untrusted DV pair), `MatcherHistoryChained`.
  - Matcher scaling helpers used by `DejudderWarp.cpp`: `ScaledSearchRange` (range given for a 1080-line working picture), `OpticalFlowReach`, `CameraWindowPixels` (stepped, or continuous in height with `general_rate_fixes`), `EngineCellSourcePixels` / `EngineGlobalSourcePixels`, `SearchHalvingsFor` (test model).
- `DejudderWarp.h` (namespace `Dejudder`): `Planes`, `Settings` (every matcher/warp option; the renderer fills it in `DejudderMatcherSettings` with reference height 2160 lines and reference rate 23.976 Hz, which `MotionWarp::Scaled` converts to the source), `WarpHeader`, the GLSL field rules `FieldVectorGlsl` / `FieldVectorGlslSplit` / `FieldAllGlsl`, and class `MotionWarp` (`Estimate`, `Warp`, `WarpProcessed` (B3), `SnapshotForWarp`, field/trust/sub-field/occlusion/grain/camera accessors, `ForgetHistory`, optical-flow status accessors). The implementation and its threading are described in `README.md`.
- `MotionSegment.h` (namespace `VpMotion`, header-only, GPU-free; NOT used by the renderer yet): `MotionSegment<Field, Slots=4>` owns or shares (`external`) the one `MotionValidity::State`; `Publish(direction, from, to, field, source, epochAtInputs, geometryGeneration, fillPair)`, `Retract(direction)`, `Fetch` / `FetchFrom` / `FetchAt` / `FetchBackward(allowFrameOld)` return a `Delivery` (field or a counted `Miss`: Disengaged, NotEstimated, Stale, NotAdjacent, Geometry), `Engage(bool)` (smooth motion exclusivity: disengaging invalidates with `SmoothMotionMix`), `Stats(consumer)`, `FallbackEntry()`. Wiring plan: `docs/design/motion-timing-callsites.md` section 5 and `docs/design/motion-shadow-callsites.patch` (shadow step P-b1: the segment shares the host's state).
- `VectorScale` (`src/VP-V/core/VectorScale.h`; NOT used by the renderer yet): `FromSizes`, `Then`, `Inverse`, `Anisotropic`, `Identity`, `Apply` (dx * sx, dy * sy).

#### Data flow

- In: the renderer host (`LibplaceboVideoRenderer.cpp`, `RunDejudder...` around lines 16340-16540) fills `DejudderPolicy::Input` from the active settings, the measured rates (display: DXGI statistics, else render-ahead's open-loop rate, else the hook refresh) and the queued frames A/B.
- Out: `Reason`, the effective strength, the even cycle and `ExtrasAllowed` decide whether the matcher runs and whether extras are presented; the interpolation policy (`InterpolationPolicy.h`) reuses `EffectiveStrength` so the two never take the same source.
- Vectors themselves do not pass through this folder today: consumers read `MotionWarp`'s textures directly (or its snapshot), each re-checking validity with `motionValidity.Current(epoch)` plus sequence compares (`DejudderLiveFieldFor`, the mask slots' epochs, the B3 snapshot epoch).

#### Threading

- `DejudderPolicy` and `VectorScale`: stateless (except `LatencyFloor`, owned by the render thread); called on the render thread. `MotionAheadBreakBefore` is called on the capture/dequeue path that submits to the optical-flow producer.
- `MotionSegment`: no locks; meant for the render thread. Only its `MotionValidity::State` is thread-safe (`Invalidate` from any thread).

#### Config keys the code really reads

The folder's code reads no configuration itself. The host parses these in `ApplyDisplayRuleOverrides` (renderer profile sections, e.g. `[vprenderer.<profile>]`, the `profiles.<group>.<name>` sections and display rules) and passes the values in:

- `dejudder_strength`, `dejudder_strength_even` (0..1), `dejudder_even_rates` (`whole` / `whole_and_1001`) - `Decide`, `EffectiveStrength`.
- `occlusion_mask` (on/off) - `Decide` (the estimate runs for the mask alone).
- `general_rate_fixes` (on/off) - `IsFilmLike`, `CameraWindowPixels`.
- `timing_continuity_fixes` (on/off) - `MotionAheadBreakBefore` (pair times checked).
- `motion_history_fixes` (on/off) - `DolbyVisionNextPairs` (same DM matrices), `MatcherHistoryChained` (via `Settings::historyFixes`).
- `motion_engine` (`auto` / `vp` / `smooth_motion`; read by `MotionConfig.h` from the profile section and `[vprenderer.motion]`) - `ResolveEngine`.

The matcher/warp keys that fill `Dejudder::Settings` are listed under `motion.consumers` (warp side) and `motion.estimation` (matcher side). `MotionSegment` and `VectorScale` read no keys; no keys are planned.

#### Fallbacks the user actually sees

- None from this folder. `Decide`'s reason shows on the de-judder status line (Ctrl+I / the status text, `PublishDejudderStatus`) and in the log; every consumer then takes its no-vector form.
- A vectors invalidation (resync, queue reset, gap, seek, drift drop, format change, smooth motion switch, settings / motion-source change with `motion_history_fixes`) is LOG ONLY: "De-judder: motion invalidated (<cause>; N in total); the vectors' users fall back until a fresh pair".
- The listed entry `Vectors: invalid (<reason>)` is produced by no code. `MotionSegment::FallbackEntry()` would produce "motion: matcher (optical flow unavailable)" after 8 consecutive matcher estimates, but the segment is not wired.

#### What is needed to lift it

- `DejudderPolicy.h`: C++ standard library, `vprenderer/CaptureTimestampGap.h` and `VideoFrameEncoding.h` (VideoProcessor-Lib).
- `DejudderWarp.h`: libplacebo headers, `../estimation/NvOpticalFlow.h` (D3D11 / ATL `CComPtr`), `GeneralRate.h`, `MotionAhead::Flow`; the implementation `../estimation/matcher/DejudderWarp.cpp`.
- `MotionSegment.h`: `../estimation/MotionValidity.h` only. `VectorScale.h`: `<cmath>` only.

#### Test it standalone

- CPU (VideoProcessor-Test): `MotionSegmentTests` and `PipelineSkeletonTests` (`VectorsScalePerAxis`) in `src/VP-V/tests`; `DejudderMatrix`, `CameraWindowMatrix`, `MotionAheadMatrix` (GeneralRateMatrixTests.cpp); `DolbyVisionPairingTests`, `DisplayRateHoldTests` (BugHunt20261017Tests.cpp); `Hunt2MotionHistoryTests`, `Hunt2DolbyVisionPairTests`; `MotionAheadBreakTests`; `DolbyVisionCaptureEncodingTests`; `H3TimingTests` (engine global vector scale); the `DejudderPolicy::Decide` / `PlanExtra` cases inside `ConfigurationReadCacheTests` (ConfigFileTests.cpp; no class is named DejudderPolicyTests).
- Run single-threaded and read the TRX:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Tests:MotionSegmentTests,PipelineSkeletonTests,DejudderMatrix,CameraWindowMatrix,MotionAheadMatrix,DolbyVisionPairingTests,DisplayRateHoldTests,Hunt2MotionHistoryTests,Hunt2DolbyVisionPairTests,MotionAheadBreakTests /Logger:"trx;LogFileName=vector_delivery.trx"`

- GPU coverage of `MotionWarp` is listed in `README.md`.

### Contract

- Input: vectors at source size + per-axis factors + geometry
- Output: vectors in the consumer's coordinate space (scale x, scale y separately)
- Geometry: source -> processing -> output, per axis
- Delay share: 0 frames
- Look-ahead: none

### Settings

- Existing keys (values the folder's code takes, parsed by the host): `dejudder_strength`, `dejudder_strength_even`, `dejudder_even_rates`, `occlusion_mask`, `general_rate_fixes`, `timing_continuity_fixes`, `motion_history_fixes`, `motion_engine`
- Planned keys: (none)
- Tiers: follows motion.estimation

### Fallbacks

- "Fallbacks engaged" entry: Vectors: invalid (<reason>) - listed, but NOT produced by any code (invalidations are log-only; see "How the code works")
- Behaviour: an invalidation moves the one MotionValidity epoch and every consumer takes its no-vector form until a fresh pair. A geometry map that is not known is not an invalidation today: with an unknown NLS shader map the host marks this frame's de-judder geometry invalid, the extras warp the source instead of the rendered picture and the sigma-delta takes no de-judder vectors (logged once: "De-judder extras: NLS shader map not known; warping the source instead"); MotionSegment's `Miss::Geometry` would cover it once wired.

### Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `motion.estimation`, `scaling.rebase`, `scaling.route_4k`, `scaling.geometry`

### How to build / test

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

### Known issues / open items

- +-1 px refinement for temporal dither / scaler refinements / texture restore (not built)
- `MotionSegment` and `VectorScale` are not wired into the host (P-b1 shadow step pending); consumers still fetch from `MotionWarp` with their own validity expressions
- The listed fallback entry `Vectors: invalid (<reason>)` is never produced; invalidations are log-only
- A scene cut is not a MotionValidity cause; each warp holds a cut pair through its frame trust only

### Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/core/VectorScale.h`
- `src/VP-V/modules/motion/consumers/DejudderPolicy.h`
- `src/VP-V/modules/motion/estimation/matcher/DejudderWarp.h`
- `src/VP-V/modules/motion/estimation/MotionSegment.h`

Forwarding headers (old paths kept for Bill's files, the renderer host and RendererCaptureHarnessTests.cpp; removed at M2 step 0):

- `src/VideoProcessor-Lib/vprenderer/DejudderPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/DejudderWarp.h`

### Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

### Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

### Settings

- Keys: `dejudder_even_rates`, `dejudder_strength`, `dejudder_strength_even`, `general_rate_fixes`, `motion_engine`, `motion_history_fixes`, `occlusion_mask`, `timing_continuity_fixes`, `vector_demand`.
- Tiers: follows motion.estimation.
- VP-V page, Motion tab: tiers Classic / Low / Mid / Upper; tier keys `dejudder_est_qpel`, `dejudder_est_split`, `motion_ahead_frames`, `optical_flow`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#3-motion](../../../../../docs/settings/VP-settings-reference.md#3-motion).
- Fallback entry ("Fallbacks engaged"): `motion: matcher (optical flow unavailable)`.

### Tests

- Placeholder (byte-identity on/off, zero cost): MISSING.
- Isolation (fixed inputs, expected outputs, cost): per-axis scaling unit tests (VectorScale).
- Contract (formats, geometry, fallback entry): sx != sy maps exactly; identity factors leave vectors unchanged.
- Chain (total delay at the 4/3/2-frame budgets): single-axis re-base + de-judder.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
