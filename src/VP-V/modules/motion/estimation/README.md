# Motion estimation + vector store (`motion.estimation`)

Unit: **motion** | side: input | order: 50 | status: **built** | hook point: prepass

## Purpose / why

Estimate motion on the ORIGINAL frame (source raster, before re-base/repair) with the NVIDIA optical-flow engine (NVOF) seeding VP's matcher, with a time-based look-ahead, and keep ONE vectors-valid state that every consumer obeys (invalidated at cuts, drops, repeats, drift, resyncs).

## How the code works

Checked against the code 2026-10-25 (M2 pass). The estimator itself (`Dejudder::MotionWarp::Estimate` and its searches) is compiled from `src/VP-V/modules/motion/consumers/DejudderWarp.cpp`, with its class in `src/VP-V/modules/motion/vector_delivery/DejudderWarp.h`; the renderer host (`src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`) owns every instance and wires the settings, the producer and the validity state.

### Public API

- `NvOpticalFlow::Engine` (`NvOpticalFlow.h/.cpp`): wraps NVIDIA's D3D11 optical-flow interface. `Open(device, context, w, h, preset, costs)` loads `nvofapi64.dll` from System32 at run time (nothing shipped; MIT headers in `3rdparty/nvidia-optical-flow`), needs API 5.0, opens a 4 x 4 grid, both directions, optional costs. `Run(from, to, fwd, bwd, fwdCost, bwdCost, result, continuous)` takes R8_UNORM inputs and writes R16G16_SINT S10.5 vectors and R8_UINT costs; `continuous` enables the engine's temporal hints. `LastError()` gives the reason for any failure (driver missing, API too old, no engine on this GPU / busy, init or execute failed, texture not accepted).
- `MotionAhead` (`MotionAhead.h`): the look-ahead depth policy. `AheadMs(configured)` = configured frames at the 60 Hz reference (4 = 66.7 ms); `DepthAt(configured, sourceHz, general)` turns that time into whole frames at the measured rate (24/25/30p 2, 50/60p 4, 120p 8, 240p 16 = `kMaxGeneralDepth`; an unmeasured rate or `general_rate_fixes: off` keeps the configured count). `Decide(...)` gives the depth and a `Reason` (`Active`, `ShallowQueue`, `NoFramesAhead`, `Off`, `Disabled`, `LowLatency`, `NoEngine`, `NoConsumer`); `QueueFramesFor`, `PendingJobsFor`.
- `MotionAhead::Producer` (`MotionAheadProducer.h/.cpp`): `Start(renderDevice, preset)`, `Stop`, `Submit(sequence, generation, luma, breakBefore)`, `Take(from, to)` (the pair's `FlowPtr` or null), `Prune`, `SetTargetSize`, `SetDepth`, `SetGeneralRates`, `Running`, `EngineReady`, `Error`, `GetStats`, `StoreStats`. Helpers: `HalfLuma8` (half-size 8-bit luma from P010/P210, v210, UYVY/HDYC, R12B, R12L, r210/R10b/R10l, BGRA/ARGB, else the source's sampler), `FlowGlobal` (whole-frame motion and a second layer from one field, in the matcher's D1 format), `FlowExtras` (cost weights and forward/backward error per block).
- `VectorStore<Payload>` (`VectorStore.h`): pairs `(from, to)` keyed under a continuity generation; `SetGeneration` clears, `Put` refuses a stale generation, `Get` counts hits/misses, `Prune`, `Plan`; bounded (16 in the producer), oldest first out.
- `OpticalFlowTrust` (`OpticalFlowTrust.h`): pure CPU maths. `CostStatistics`/`CostWeights` (cost against the field's median and MAD), `FbError`/`FbWeights` (the engine's own forward/backward consistency), `Combine`, `GlobalUsable` (ambiguity < 0.9).
- `MotionValidity::State` / `ConsumerGate` (`MotionValidity.h`): one epoch counter moved by `Invalidate(Cause)` (11 causes: resync, queue reset, gap, cadence repeat, drift drop, smooth-motion mix, re-base change, source format change, seek, motion source change, settings change); `PairValid`, `NoteKept` (the drop safety net), `KeptPairValid`, `StreamGeneration` (moves only on resync, queue reset, seek, format change), `Consume` (what changed since the consumer last looked, and whether GPU resources must be released). Consumers bind derived state with `ConsumerGate::Bind` and check it before each use.
- `MotionEngineIdle::Keeper` (`MotionEngineIdle.h`): keeps the motion engine loaded and idle while hiccup repair (`interpolation_fill_gaps`) could need it; releases it under video memory pressure (allocation failure, or DXGI usage >= 95 % of budget) and keeps it again after 30 s with room under 85 %.
- Estimator entry points on `Dejudder::MotionWarp`: `Estimate(a, b, occlusionSlot, grainError)`, `SetExternalFlow(flow)` (a pair computed ahead), `OpticalFlowUsed`, `ExternalFlowUsed`, `GlobalFromEngine`, `OpticalFlowError`, `EngineInputSize`, `SearchHalvings`, `ForgetHistory`, `SetPictureArea`; outputs `Forward()`, `Backward()`, `Field()`, `FrameTrust()`, `Camera()`.

### Data flow

1. Render loop (queue walk, the `motionAheadFrames` hand-off in the host): once the producer runs and at least the depth is queued ahead ("engaged"), each queued source frame is handed to `Producer::Submit` once, with its luma source (it keeps the frame buffer alive), `MotionValidity::StreamGeneration()` and `breakBefore` (`DejudderPolicy::MotionAheadBreakBefore`: a capture gap, a film update, non-adjacent times or an untrusted DV pair).
2. Producer worker: `HalfLuma8` -> optional further halving to the matcher's engine size (`SetTargetSize`; any other size is skipped and nothing stored) -> upload to its own D3D11 device on the render adapter -> `Engine::Run` (preset Slow, both directions, costs) for each adjacent pair in the same generation -> read back -> `FlowGlobal` both directions + `FlowExtras` -> `VectorStore::Put(previous, sequence, generation)`.
3. Render thread (`RunDejudderEstimate`): when `MotionValidity::PairValid(a, b, ...)` holds, `Producer::Take(a, b)` -> `MotionWarp::SetExternalFlow`. `Estimate` then builds the half-size planes and pyramids (reusing A from the last pair's B), the picture area and the fade test, and:
   - an ahead pair of the right size goes straight into the engine output textures (no engine run), with its global motion and optional trust weights;
   - otherwise (a miss) the render-time engine runs on the matcher's half-size planes (preset Medium);
   - with neither (no NVIDIA engine, `optical_flow: off`, or a latched failure) the block searches run.
   By default (seed mode) the engine's vectors stand in for the first sweep and the refinement follows; the engine's whole-frame motion replaces the global search when it is clear (`optical_flow_global`, `optical_flow_global_fallback`). Then consistency, quarter-pel, smoothing, verification, block split, frame trust and hysteresis produce `field`. At tall inputs (`dejudder_low_res`) an inner instance searches at half again and its results are scaled back.
4. Out: forward/backward block searches, the trusted `field` (+ sub-field), frame trust, camera, occlusion and grain-error textures, at the matcher's half size of the source (8 px blocks); `motion.vector_delivery` maps them to the consumers' geometry. The engine's whole-frame motion also feeds `source_resolution` auto's fast-pan switch and the motion intent.
5. Validity: `NoteKept` on every presented frame, and the host's `RequestMotionInvalidate(cause)` at resyncs, queue resets, re-base changes, settings and motion-source changes; consumers check `ConsumerGate` before using any vector-derived state.

### Threading

- Render thread: `MotionWarp::Estimate` (all GPU passes through libplacebo, plus the render-time engine on the same D3D11 immediate context after `pl_gpu_flush`), `Take`/`Prune`, the producer start/stop decision, `NoteKept`, `Consume`.
- Render loop (queue walk): `Submit` (takes the producer mutex briefly; never waits: beyond `PendingJobsFor(depth)` pending jobs the oldest is dropped).
- Producer worker thread: creates its own D3D11 device (so a start never stalls the render thread), runs the engine and the CPU extras; `HalfLuma8` splits rows over up to 4 short-lived threads for frames 512 lines and taller. Three consecutive failures stop it for the session (`Running()` false); exceptions are caught and counted.
- Locks: the producer's mutex (jobs, error, stats); the store's own mutex; `MotionValidity::State` is lock-free except the small kept-run mutex. `Invalidate` may be called from any thread.

### Config keys actually read

All are renderer-profile keys, read per profile section (`[vprenderer.<name>]`, the display rule's section) by the host's profile parser and copied into `Dejudder::Settings` by `DejudderMatcherSettings`:

- Motion source: `optical_flow` (on/off, default on), `optical_flow_global` (on), `optical_flow_global_fallback` (on), `optical_flow_cost_trust` (off), `optical_flow_fb_check` (off).
- Look-ahead: `motion_ahead_frames` (0..4; any value above 0 means 4, i.e. 66.7 ms; 0 = off). The producer also needs the de-judder running and no low-latency queue (`low_latency_path`, `input.queue_timing`).
- Search structure: `dejudder_search_range` (1..16), `dejudder_low_res` (off/search/all), `dejudder_global_prior`, `dejudder_global_two`, `dejudder_est_candidates`, `dejudder_est_qpel`, `dejudder_est_gate`, `dejudder_est_split`, `dejudder_est_split_sad`, `dejudder_est_lambda`, `dejudder_vector_smooth`, `dejudder_match_tolerance`, `dejudder_consistency_px`, `dejudder_texture_tolerance`, `dejudder_regional_hold`, `dejudder_cut_histogram`, `dejudder_fade_match`, `dejudder_fade_floor`, `dejudder_verify`, `dejudder_verify_share`.
- History and rates: `motion_history_fixes` (matcher history restarts, settings/motion-source invalidation, ahead breaks), `general_rate_fixes` (time-based depth, vote range, camera window).
- Scheduling: `dejudder_estimate_ahead` (estimate (B, C) during A's spare refresh; the schedule lives in `consumers/DejudderAhead.h`, so the key is listed by both slots).
- `motion_engine` (`auto`/`vp`/`smooth_motion`, `MotionConfig.h`, Bill's file; also read from `[vprenderer.motion]`): VP motion or libplacebo's smooth motion; with smooth motion no estimate runs.
- Code defaults, not config: the engine presets (producer Slow, render-time Medium), seed/candidate mode, the trust constants.
- `dejudder_est` is not a key (only the change-log label of the `dejudder_est_*` group). `motion_ahead_ms` is planned and read nowhere.

### User-visible fallbacks / notices

None of this module's downgrades engages a "Fallbacks engaged" entry today; all are log-only (an ADR-0001 gap):

- NVOF unavailable or failed at render time -> block searches: logged once per motion-source change ("De-judder motion: block searches (no engine) - <reason>"). The failure is latched for that matcher instance.
- Producer did not start, or stopped after three failures in a row -> render-time estimation: logged once ("Motion ahead: the producer did not start / stopped (<reason>)"); retried only when the settings stop and start wanting it again.
- Short queue / no frames ahead / low latency: the "Motion ahead: configured, queued ahead, depth (reason)" log line.
- Low video memory: the idle engine is released and recreated later (logged).
- Estimate failed: "De-judder: motion estimate failed" (first, then every 2400); the de-judder holds that frame.
- Vectors invalid: consumers use their no-motion form until a fresh contiguous pair ("motion invalidated (cause)" in the log). By design, not a fallback.
- `FallbackCatalog::Motion` (`src/VP-V/core/FallbackCatalog.h`, tested in `FallbackCatalogTests`) produces `motion: matcher only (optical flow failed: <error>)` and `motion: no optical-flow look-ahead (producer stopped: <error>)`, but the host does not call it yet (its hunk is in `docs/design/silent-fallbacks-callsites.patch`, not applied in this tree). The listed entry `Motion: matcher only (NVOF unavailable: <reason>)` is produced by no code.

### Dependencies to lift it

- Pure, CPU-only (C++ standard library): `MotionAhead.h` (needs `GeneralRate.h`), `VectorStore.h`, `OpticalFlowTrust.h`, `MotionValidity.h`, `MotionEngineIdle.h`.
- `NvOpticalFlow.*`: Windows, D3D11, ATL, the `3rdparty/nvidia-optical-flow` headers, an NVIDIA driver at run time.
- `MotionAheadProducer.*`: the above plus `AnalysisLumaSource.h` (VideoProcessor-Lib) and DXGI.
- The estimator: libplacebo (`pl_gpu`, D3D11 interop), `DejudderPolicy.h`, `DejudderWarp.h`, `GeneralScale`; it is one class with the consumers' warp, so it lifts with `motion.consumers` unless split.
- The host wiring (producer start/stop, submit, validity causes, settings) stays in the renderer until the M2 adapter.

### Test it standalone

- CPU: `VectorStoreTests`, `MotionAheadTests`, `MotionAheadSwitchTests`, `MotionAheadLumaTests`, `MotionAheadRgbLumaTests`, `OpticalFlowTrustTests` (all in `VectorStoreTests.cpp`), `MotionAheadMatrix` (`GeneralRateMatrixTests.cpp`), `MotionAheadBreakTests`, `MotionValidityTests`, `Hunt2MotionHistoryTests`, `H3TimingTests` (the idle keeper); `FallbackCatalogTests` (VP-V tests).
- GPU (NVIDIA, `VP_TEST_GPU=hardware`; otherwise they report and pass; run only when GPU tests are allowed): `NvOpticalFlowTests` (self-check, producer, extras); estimator regressions in `LibplaceboLutParserTests` and `BetaGpuTests`.

Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~VectorStoreTests|FullyQualifiedName~MotionAhead|FullyQualifiedName~OpticalFlowTrustTests|FullyQualifiedName~MotionValidityTests|FullyQualifiedName~Hunt2MotionHistoryTests|FullyQualifiedName~H3TimingTests" /Logger:"trx;LogFileName=motion-estimation.trx"`

## Vector engine on demand

`VectorDemand.h` (header-only, pure, CPU-tested) decides when the Vector engine runs and which consumers it serves.
Synthesis consumers (dejudder extras, interpolator, hiccup repair, occlusion mask) keep their own policies and are never counted as served or unserved.

`Decide` rules, in order:

1. Not enabled: nothing (no status line). Slots off: "off (slot off)", nothing served or unserved.
2. No consumer wants vectors, or only synthesis consumers do: mode Off, idle.
3. Interlaced: no vectors, every wanting consumer is unserved (visible fallback).
4. libplacebo smooth motion: see the exception below.
5. Low latency with no frame ahead (queue capacity 0): Backward pairs. Source statistics consumers (source monitor, intent) are served; source-field consumers (grain map, deband hold, detail pulse, entrance segment) and output dither are unserved ("needs a frame ahead").
6. Otherwise Forward (ForwardShallow in low latency with capacity of at least 1). Every wanting consumer is served; the estimate and producer run when anything is served. `EngageAhead(configured, capacity)` is max(1, min(configured, capacity)).

`MotionAhead::Decide(..., backwardPairs)` gives depth 0 with reason BackwardPairs for the backward mode in low latency (engine available, consumer present); without the flag nothing changes.

DELIBERATE exception: while libplacebo smooth motion runs, source-domain analysis consumers (source field and source statistics) keep the VP vectors, and output-domain consumers (output dither) fall back visibly ("libplacebo smooth motion owns output"). This is analysis only: no VP interpolation or de-judder synthesis runs alongside smooth motion. Revert with `VectorDemand::kAnalysisUnderSmoothMotion = false`, which makes smooth motion exclusive (every consumer falls back visibly).

Fallback entries (`FallbackCatalog`): `vector_engine` (unserved consumers and why), `vector_engine_producer` (look-ahead stores nothing for this size), `vector_engine_global` (fast-pan / intent without whole-frame motion).

## Contract

- Input: original frame luma at the delivered source size (before re-base/repair); queued next frames for the look-ahead
- Output: forward/backward block vectors, trusted field and frame trust at the matcher's half size of the source (8 px blocks; a further half inside at tall inputs), the engine's whole-frame motion, and the validity state
- Geometry: source raster; consumers map through motion.vector_delivery
- Delay share: 0 extra frames: motion ahead uses frames already queued (66.7 ms of look-ahead as whole frames at the source rate)
- Look-ahead: `motion_ahead_frames` (time-based depth; a short queue gives fewer pairs ahead, the rest is estimated at render time)

## Settings

- Existing keys: see "Config keys actually read" above (`optical_flow*`, `motion_ahead_frames`, `motion_history_fixes`, `motion_engine`, `general_rate_fixes`, `dejudder_estimate_ahead`, `dejudder_search_range`, `dejudder_low_res` and the `dejudder_est_*` / search keys). `dejudder_est` is not a key.
- Planned keys: `motion_ahead_ms`
- Tiers: Classic off .. Upper full (NVOF + matcher)

## Fallbacks

- "Fallbacks engaged" entry: none produced today (log-only). Designed: `motion: matcher only (optical flow failed: <error>)` / `motion: no optical-flow look-ahead (producer stopped: <error>)` (`FallbackCatalog::Motion`, not wired). The listed `Motion: matcher only (NVOF unavailable: <reason>)` is produced by no code.
- Behaviour: NVOF absent/failed -> VP's block searches; producer stopped -> render-time estimation; vectors invalid -> every consumer falls back to its no-motion form.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.queue_timing`, `input.source_analysis`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_GPU=hardware` (`VP_TEST_ADAPTER=NVIDIA` picks the GeForce).

Placeholder tests:
- (none yet)

Isolation tests:
- `NvOpticalFlowTests` (GPU), `VectorStoreTests`, `MotionAhead*Tests`, `OpticalFlowTrustTests`, `MotionValidityTests`, `H3TimingTests` (idle keeper); no class is named "DejudderWarp estimator tests" (the estimator is covered by the GPU regressions in `LibplaceboLutParserTests` / `BetaGpuTests`)

Contract tests:
- vectors estimated on the original (not repaired/re-based) frame at the matcher's half size (not yet a dedicated test)

Chain tests:
- vector validity across cuts/drops at 24/30/60p (`MotionAheadBreakTests`, `Hunt2MotionHistoryTests`; GPU chain not yet)

## Known issues / open items

- A_motion needs these vectors BEFORE it runs (forced order)
- No "Fallbacks engaged" entry for any downgrade (see "User-visible fallbacks"): wire `FallbackCatalog::Motion`.
- A render-time engine failure is latched per matcher instance for the session (cleared only when the instance is recreated).

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/motion/estimation/NvOpticalFlow.cpp`
- `src/VP-V/modules/motion/estimation/NvOpticalFlow.h`
- `src/VP-V/modules/motion/consumers/DejudderWarp.cpp`
- `src/VP-V/modules/motion/estimation/MotionAheadProducer.cpp`
- `src/VP-V/modules/motion/estimation/MotionAhead.h`
- `src/VP-V/modules/motion/estimation/VectorStore.h`
- `src/VP-V/modules/motion/estimation/MotionValidity.h`
- `src/VP-V/modules/motion/estimation/OpticalFlowTrust.h`
- `src/VideoProcessor-Lib/MotionConfig.h`
- `src/VP-V/modules/motion/estimation/MotionEngineIdle.h`
- `src/VP-V/modules/motion/estimation/MotionAheadProducer.h`

Forwarding headers (old paths kept for Bill's files, the renderer host and RendererCaptureHarnessTests.cpp; removed at M2 step 0):

- `src/VideoProcessor-Lib/MotionAhead.h`
- `src/VideoProcessor-Lib/MotionValidity.h`
- `src/VideoProcessor-Lib/vprenderer/MotionAheadProducer.h`
- `src/VideoProcessor-Lib/vprenderer/MotionEngineIdle.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `dejudder_consistency_px`, `dejudder_cut_histogram`, `dejudder_est_candidates`, `dejudder_est_gate`, `dejudder_est_lambda`, `dejudder_est_qpel`, `dejudder_est_split`, `dejudder_est_split_sad`, `dejudder_estimate_ahead`, `dejudder_fade_floor`, `dejudder_fade_match`, `dejudder_global_prior`, `dejudder_global_two`, `dejudder_low_res`, `dejudder_match_tolerance`, `dejudder_regional_hold`, `dejudder_search_range`, `dejudder_texture_tolerance`, `dejudder_vector_smooth`, `dejudder_verify`, `dejudder_verify_share`, `general_rate_fixes`, `motion_ahead_frames`, `motion_engine`, `motion_history_fixes`, `optical_flow`, `optical_flow_cost_trust`, `optical_flow_fb_check`, `optical_flow_global`, `optical_flow_global_fallback`.
- Planned keys (not yet implemented): `motion_ahead_ms`.
- Tiers: Classic off .. Upper full (NVOF + matcher).
- VP-V page, Motion tab: tiers Classic / Low / Mid / Upper; tier keys `dejudder_est_qpel`, `dejudder_est_split`, `motion_ahead_frames`, `optical_flow`.
- Listed under the tier: `dejudder_estimate_ahead`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#3-motion](../../../../../docs/settings/VP-settings-reference.md#3-motion).
- Fallback entry ("Fallbacks engaged"): `motion: matcher only (optical flow failed: <error>) / no optical-flow look-ahead (producer stopped: <error>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): NvOpticalFlow tests; DejudderWarp estimator tests; MotionValidity tests.
- Contract (formats, geometry, fallback entry): vectors estimated at source size, never on repaired/re-based pictures.
- Chain (total delay at the 4/3/2-frame budgets): vector validity across cuts/drops at 24/30/60p.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
