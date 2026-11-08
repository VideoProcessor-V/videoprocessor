<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Camera link: K1 fit runner, K2 P8 + accessor constants, K4 guided matching (module side)

Design: `D:\codex\vp-handover\56-design-camera-k1-k2-k4.md`. Everything here is **inert**: nothing is registered in a renderer,
no Bill file is touched, and with no consumer of P7 there is no thread, product, ring, pool or shader text.

## What is here

| Part | Files | Role |
|---|---|---|
| Interface | `CameraLink.h` (`ICameraLink`, query `vpv.camera.v1`), `../../VectorEngineInterface.h` (`IVectorEngine`, `IPreEstimateHook`, query `vpv.vector_engine.v1`) | the typed surfaces; P5a implements the second |
| K1-1/2 | `PairTiming.h`, `PairAssembly.h`, `FieldCopy.h` | pair seconds (film lock / measured / nominal / unknown = 0), `PairInput` assembly, stamp -> generation (full, continuity, settings[motion] only; re-base is always 0) |
| K1-3 | `CameraRecordPool.h`, `CameraUsable.h` | pool parallel to the P7 SlotSet, 330 B `CameraRecordPod` (declared 1 KB), the one usable set (D-K4) |
| K1-4 | `FieldReadbackRing.*`, `NativeCopyReadback.*`, `GuidanceCountsRing.*` | P3 field / 16 B counts to CPU without waiting: on D3D11 (no buffer transfers there) the shared `LedDrive::NativeTexReadback` with a copy pass; elsewhere `pl_buf` + `pl_buf_poll(...,0)`. Not an `IReadbackDevice` until K1-9 |
| K1-5 | `CameraFitProducer.*`, `CameraFitWorker.*` | `ILaneProducer` for P7; the one worker thread; never `BindFitter` (F2): the reader only bumps atomics |
| K1-6 | `CameraLinkCore.*`, `CameraFitStatus.h` | phases as plain methods, status, `camera.no_model.*` ids, `VP_CAMERA_FIT_TRACE` |
| K2 | `CameraLabelPack.h`, `CameraLabelProducer.*`, `CameraLabelFill.*`, `CameraConsumerBinding.*` | P8 upload / compute (one unit), lane / live fill, per-consumer binding |
| K4 | `CameraGuideProduct.h`, `GuidanceScheduler.*`, `GuidanceGate.h`, `GuidanceGovernor.h`, `GuidanceCountsRing.*`, `MotionBenchWriter.h` | `p7g_camera_guide`, refusal gate, hook, governor, counts readback, `VP_MOTION_BENCH` |
| K4 wiring (on P5a's contract) | `GuidanceLag.h`, `GuideLaneJob.*`, `GuidanceContext.h`, `GuidanceStatus.h`, `GuidanceTimers.h`, `GuidanceAdapter.*` | measured `d`, the guide lane job, the `PairContext` provider + flow resampler, skeleton-visible status, `motion.*` and `camera.*` PassTimers rows, the adapter that registers the ONE pre-estimate hook on the engine (P5a owns the call site: `VectorEngine::Estimate`) |

## Skeleton products declared

* P7 camera fit, P8 camera labels, P9 camera smoothed: K0 (`core/Products.h`). P8 is RGBA32F, 16 B per 16x16 block.
* `p7g_camera_guide` (D-K3): `CameraGuideProduct.h`. Cpu, T0, inputs P7 at lag d and P4 at lag 0, 20 B per block. It is REGISTERED
  (id 20, `core/Products.h`, with the floor lag 2) and has its catalog row and the "camera guidance" consumer; the tests pin the
  declaration (`GuideProductTests`, `ProductCatalogTests`). `SkeletonProductsWithGuide(d)` sets the measured d on a graph rebuild;
  d comes from `GuideLagMeter` (`GuidanceLag.h`): p95 of the fit cost (`CameraMotion::costMicros`) + the readback latency, over the
  measured source period; the floor when nothing is measured or the run is deterministic.
* Unit version: `kCameraUnitVersion` covers the fit, the labels (both modes) and the guide.

## Private history: justification (TEAM-RULES "skeleton first")

* **Readback ring** (`FieldReadbackRing`): `lane lead + 2` (at least 3) host-mapped buffers of one field each (4K: 0.5 MB x2 planes).
  It is a transfer staging area, not a frame history: an entry lives from the request until the next poll, then is freed. The
  shared products stay in the store; this is the only way a GPU product reaches the CPU fit without a blocking map.
* **Record pool** (`CameraRecordPool`): holds the `shared_ptr`s the P7 slot cannot (1 KB cap): the per-block arrays and the
  field copy. It is indexed like the P7 SlotSet ring, written only under the claim, read only for sealed / pinned seqs, and erased
  on a late publish. It stores nothing P7 does not already own; it is the heavy half of the same product.
* **Fitter history** (previous record, temporal filter, object tracker): inside `Fitter`, history 1 as P7 declares; it is the
  fit's own state, not a picture buffer.
* No private full-resolution texture exists anywhere here: P8 slots are per-block (0.5 MB at 4K), the counts texture is 1x1.

## Decisions recorded by the audit round (2026-11-01)

* **No deadline in the Fitter** (overrides doc 56 section 1.4 deadline row): `PairInput::deadlineMicros` is always 0. Every pair is
  fully fitted, so the fit history is deterministic; lateness is judged only at the publish / seal (late = fitted, not published).
  The seal-time estimate (`FramePhaseFacts::sealUs`) and the p95 guard were removed with it.
* **Reset / relatch generations are stamped into the job at submit** (`CameraFitWorker::Submit`); the fitting thread applies a reset
  before THAT job's Fit, whatever the queue backlog.
* **A pool entry is served only after the slot publish succeeded** (`committed`); only the seal observer may read an uncommitted one.
* **Guidance cool-down runs on measured pair time for every pair asked** (`GuidanceScheduler::Guide`), so narrow / E1-only configurations
  recover; lane and live both use the governed switches (snapshot under the mutex) and a lane result built under other switches or
  another lag is dropped. A NoRecord verdict is never stored by the lane.
* **Status holds**: each `camera.no_model.*` event engages its id for 0.5 s of measured time (frame period), then releases;
  `camera.no_model.queue` reports queue-full drops.
* **Binding**: the per-consumer define and the sampler follow the policy, never P8 presence; a 1x1 dummy is bound with `camera_enabled = 0`.

## K4 wiring on P5a's contract (2026-11-07)

**STATUS (camera live wiring, branch `r1cam/camlive`): K4 is reachable, default OFF.** `CameraExtension` builds the camera's own product table
(`CameraLane`, below) at the first verified engine field and attaches the core to it when `camera_link = report` AND `camera_model = on` (`p7Wanted`);
with any `camera_meas_*` switch also on it creates the `CameraGuidanceAdapter` and attaches it to the renderer's engine
(`Query("vpv.vector_engine.v1")` -> `IVectorEngineDirectory::EngineFor(host)`), which registers the ONE pre-estimate hook. The four .cpp files
(`GuidanceScheduler`, `GuideLaneJob`, `GuidanceAdapter`, `GuidanceCountsRing`) and what they need (`CameraLane`, `CameraLabelFill`, `CameraLabelProducer`,
`CameraModelGpu`, `GlobalModelMeasurement`, `GlobalModelSearchHints`) are in both renderer items props. All keys default off: nothing below exists then.
No Bill file changed; nothing is fed into the PassTimers accumulator (that needs a renderer call site: gap G3 below).

**LOCK RULE.** `CameraGuidanceAdapter::Detach` (also `SetSettings` / `Attach` when they unregister) runs on the render thread with `renderMutex`
held and waits on `CameraLinkCore`'s observer mutex; the camera worker holds that mutex while it pumps the lane job. So the host's `p4` reader,
its `nextPair` hint and anything they call must NEVER take `renderMutex` or wait for the render thread (they read published slots only), or
`Detach` deadlocks. `CameraLinkCore::SetFitDoneObserver` asserts in debug builds that it is not called from the observer's own thread.

P5a (m2/p5a, `HOOKS.md` section 7) owns the call site: `VectorEngine::Estimate` calls `BeforeEstimate` / `AfterEstimate` around the matcher's
`Estimate` for the in-render AND the ahead estimate (render thread, never for the warm-up pair). The camera side adds no host hunk.
* **Adapter (`CameraGuidanceAdapter`).** The engine of a renderer comes from `Query("vpv.vector_engine.v1")` -> `IVectorEngineDirectory::EngineFor(host)`.
  `Attach(engine, countsSource)` registers one hook only when a switch is on (all off: nothing registered, no observer; the engine runs its
  `Estimate` only). `Detach()` (at GpuReleased / Shutdown, render thread) clears the engine's hook FIRST, then the fit-done observer (it waits for a
  call in flight), then drops everything. `OnFrame()` finishes the counts a frame late and pumps the lane job; `RebuildGraph()` derives d.
* **Engine contract used.** Every `IVectorEngine` method except `FieldEra` is render-thread only: the provider asks `PairSpansSlip` only on the render
  thread; the camera worker never touches the engine and never calls `FieldOf` (its texture lives until the next estimate). `pairSeq` = `bSequence`
  = `FieldOf`'s `pairTo`. A hook failure is contained by P5a (visible fallback `vector_engine_hook`).
* **The d-th PREVIOUS record, not N - d.** `ICameraLink::PreviousRecordSeq(N, d)` walks the chain of REQUESTED fields (known the moment the copy is
  asked for), so cadence repeats (24p in 30) leave no false "record late" and a fit that is still running is "late", never replaced by an older record.
* **One gate, one unit.** `GuidanceRefusal()` (`GuidanceGate.h`): all switches off, record late, slip / ancient stamp, `!Usable`, confidence < 0.7
  (0.85 when the static gate is the only switch). `BuildGuide()` calls it for lane and live; every refusal is counted per reason and shown.
* **Lane job.** `CameraLinkCore::SetFitDoneObserver` fires on the fitting thread after the publish; `GuideLaneJob` wants the NEXT pair (the host's
  `nextPair` hint, the first P4-published seq after the newest requested record) and stores it through `TryPublishLane` once the record and P4(N) are
  there. A stored lane result is used only when the render's own inputs are identical (switches, lag, thresholds, record seq, epoch); a slip that
  appeared meanwhile is re-checked at consumption (`SetSlipCheck`); a discontinuity bumps the epoch, so a pass that is mid-compute cannot store.
  Lane == live byte for byte (`GuideProductTests`, `GuidanceWiringTests`, including a barrier thread test of the ask-while-computing interleaving).
* **Status through the skeleton.** `CollectDecisions()`: FallbackCatalog Decisions `camera.guidance.escalations_high`, `.refused`, `.narrow_idle`.
* **Costs through the skeleton's pass-timer path.** `MotionTimerRows()` -> `motion.estimate / guided_pass / block_search / d1` (host: `AddGpuGroups`);
  `FeedPassTimers()` -> `camera.fit` (CPU work, the worker's fit) and `camera.readback` (a WAIT: request -> ready latency) into `PassTimerHook::acc`.
  `camera.labels` (P8) has no producer clock in this scope. The governor trips on escalation counts; a skeleton feed of a consumer's measured GPU ms
  into a governor does not exist yet (MV7 item).
* **AfterEstimate.** M2's `AfterEstimate(pairSeq, VpMotion::EstimateOutcome)` (`m2/p5a-outcome`, merged) is mapped by `CameraGuidanceHook::AfterEstimate` onto
  `CameraLink::EstimateOutcome` (the ONE mapping: guidance used, narrow idle -> `narrowDroppedEngine`, rejection text, gpuMs, counts texture) and forwarded to `AfterEstimateEx`.
  "Engine field used, narrow idle" is now filled. `gpuMs` is -1 today (the engine has no per-estimate GPU time: its pass timers are asynchronous), so the bench `gpu_ms` stays empty.
* **No new keys.** The measurement switches are plain `MeasurementSwitches` inputs (`GuidanceSettings`); K3 supplies the `vector_*` keys.
* **NVOF seed mode stays primary; guidance never drops a seed.** What each switch does there (DejudderWarp.cpp, `Estimate`):

| Switch | Seed mode (engine seeds the refinement) | Without the engine seeding |
|---|---|---|
| `modelGlobal` | replaces the engine's whole-frame D1 value when confident; saves work only when the engine's global was unclear (the global search is skipped) | skips the D1 search |
| `guidedCandidate` | no effect: CPU plan only, no GPU path yet (shown as "not applied") | same |
| `narrowWindow` | idle: `narrowDroppedEngine`, shown as "engine field used, narrow idle" (`camera.guidance.narrow_idle`) | the guided narrow pass before the block search |
| `e1Skip` | runs on the engine's seeds: blocks agreeing with the model skip the second sweep; seeds stay | runs on the first sweep |
| `staticGate` | the E4 flag reaches the refinement | also the block search |
| `cutSkip` / `flashBridge` | the whole pair is skipped, the seeds with it (by design: nothing is searched across a cut) | same |
| `fadeNormalised` | cost normalisation of the E1 check only | narrow pass and E1 check |

## Camera live wiring (default off; what is live and what is not)

> **STOPGAP - TO BE RETIRED.** `CameraLane` is the camera's PRIVATE SlotTable (owner rule: products live in the skeleton). It is retired for skeleton slots once SK-W
> provides live P2 / P3 / P4 content, the camera consumer declarations in the skeleton graph and the BeforeRender seal; then the markers and the lagged seal are deleted and the
> camera worker becomes a LaneRunner producer (K1-9).

| Part | Live now | How |
|---|---|---|
| P7 fit | yes, with camera_link = report + camera_model = on | `p7Wanted` is set by the keys, NOT by P5a's vector demand (that decision only covers P2/P3 and nothing in a renderer declares P7 as a consumer). The fit reads the engine's verified fields (`IVectorEngine::FieldOf`) through the readback ring |
| P7 as a skeleton product | yes, in the camera's own table | `CameraLane.h`: the same registry, SlotTable, SlotSet and CpuBuffers as the skeleton lane, built with the camera consumers. The skeleton lane carries only its report consumer, so no renderer graph holds P7; nothing publishes P2 / P3 as slots (the engine keeps its field as a texture). The camera publishes empty-content input markers for P3 / P4 / P2m / P1 each frame (ancestry + honoured stamp) and seals `kSealLag = 6` frames behind the render, with the Indicator's stamp of that frame |
| validity | era / stamp / slip | the record's honoured stamp is checked at the seal (a resync event or a slip between marker and seal rejects it); a pair that spans a slip or has an ancient stamp is not fitted (`camera.no_model.slip`) |
| trust | confidence, reason, P8 untrusted code | in the record (`CameraMotion`), the status line and `Usable()`; the labels carry the untrusted code (K2) |
| P8 labels | yes, when labels are wanted (overlay != off, any camera_use_* switch, camera_labels_gpu) and a render device exists | `CameraLabelProducer` in UPLOAD mode (CPU labels uploaded to a texture; compute mode with camera_labels_gpu on), `CameraLabelFill` as the post-submit gap work and the seal's live fill. Because the seal lags, a consumer reads the labels of a frame that has been sealed (`LabelsFor(seq)`); no consumer is wired to them yet |
| P9 smoothed | NO | no producer exists (K9-1 `CameraSmoother` is not built); declared by no consumer, so the graph never holds it |
| K4 guidance | yes, with a `camera_meas_*` switch on | adapter + hook; P4 facts for the lane job come from the frame-analysis store (`Identity.picture`, `Scene.cut / fade` when the scene group was written); a pair without a scene record is counted and shown (`camera_scene`) |
| status | Ctrl+I, Config status, Fallbacks, Ctrl+Alt+D | `CameraStatus.h` + the adapter's lines and decisions + `camera_engine` / `camera_graph` / `camera_fit` / `camera_gpu` / `camera_scene`; `camera_fit` = "camera fit waits for resync_event: turn resync_event on" (the seal is not driving: a visible fallback, since every camera default is off and an enabled camera is explicit) |

**Gaps (not closed here).** G1 P2 / P3 / P4 content is not in a skeleton slot: the markers carry ancestry only; moving P7 into the skeleton lane (K1-9 / SK-W) removes the private
table. G2 (closed) `m2/p5a-outcome` is merged and mapped (see AfterEstimate above); the bench `gpu_ms` stays empty until the engine can measure a per-estimate GPU time. G3 `camera.fit` /
`camera.readback` PassTimers rows need `FeedPassTimers(passTimerHook.acc)` at a renderer call site (a Bill hunk under a zone lock); the samples are bounded and unread until then.
G4 flash events and fade gain / offset have no producer (flash false, gain 1, offset 0). G5 `measuredSourceHz` is the ingress rate (`HostResync::measuredRateHz`); a cadence-locked source
uses the store's film rate when its cadence group was written, else the ingress rate.

### What an explicit camera-on adds per frame (camera_link = report, camera_model = on, resync_event = on; every camera_meas_* and camera_use_* off)

Read from the code, not measured. NO hook is registered (`guideWanted` is false: no adapter, no `SetPreEstimateHook`), so `VectorEngine::Estimate` runs the matcher's passes exactly as before.

| Cost | Where | Size |
|---|---|---|
| CPU fit | the ONE camera worker thread, once per verified field (cadence repeats have none); `Fitter::Fit` on the readback copy | async: the render thread never waits; p95 not yet measured on the laptop (indicative 9-12 ms CPU) |
| GPU -> CPU readback of `IVectorEngine::FieldOf(seq)` | D3D11 (the production backend): ONE small copy pass (a shader writes the engine's rgba32f forward field into a VP-owned slot texture by imageStore), ONE native CopyResource into a staging texture and an event query; read later with Map(DO_NOT_WAIT). Not D3D11: `pl_tex_download` into a host-mapped `pl_buf`, `pl_buf_poll(..., 0)`. The backward plane is requested only when the engine gives one (`FieldOf` gives none today) | 16 B per 16x16 block: 1080p 120 x 68 = 8,160 blocks = 130,560 B (0.12 MiB) per pair; 4K 240 x 135 = 32,400 blocks = 518,400 B (0.49 MiB) per pair. Ring of at least 3 slots allocated at attach (about 0.4 MiB at 1080p, 1.5 MiB at 4K); a full ring drops the request visibly |
| `CameraLane` | CPU only: input-marker headers for about 4 nodes per frame (atomic claims / publishes), the lagged seal (`Resolve`, `ReleaseAfter`), no GPU, no allocation per frame | microseconds |
| Frame-analysis store reads | `UpdateTiming` (one cadence read per frame) and `DescribePair` (one record read per fit); each copies a `Record` (about 4 KB) under the store mutex | negligible |
| P8 labels upload | only when labels are wanted (overlay != off, any `camera_use_*`, `camera_labels_gpu`): none with the defaults | 0 |
| GPU operations added | the one copy pass and the one CopyResource + query per verified field, and nothing else (no guidance counts readback, no label upload, no change to any matcher pass) | |

### Known issue (owner decision): the fit reads the GUIDED live field

`FieldOf(seq)` returns the matcher's LIVE block field of the last successful estimate (HOOKS.md section 7; `MotionWarp::Field()`), and production feeds the fit from exactly that. With every guidance switch off that field is the unguided matcher output, so nothing feeds back. With `camera_meas_model_global` (or any switch that changes the estimate) ON, the matcher's field is the one the camera's own model guided, so the fit re-measures its own guide: diagnosis ac283dae2a47417e5 found a feedback drift of about 0.004 px per pair. The fix is to fit from the engine's seed field (the NVOF / pre-guidance field) instead of the guided live field; it needs a second field accessor on `IVectorEngine` and is not done. Until then, a measurement switch that changes the estimate (camera_meas_model_global, and a default-on option (a) if the owner chooses it) carries this drift.

## Rules this code follows

* **Fill rule.** P7: the lane (camera worker) publishes before the seal; a result after the seal is discarded (SlotSet `Late`,
  pool entry erased); there is **no live fill** (D-K1): late = visible "camera: no model (late)". P8: lane in an idle gap, else
  the seal's live fill (only when P7 was sealed), later consumers reuse, P7 not sealed -> P8 absent -> `enabled = 0`.
* **Every fallback is visible.** `camera.no_model.{late,ambiguous,timing,readback,slip,depth0}` (FallbackCatalog `Decision`s from
  `CameraLinkCore::CollectDecisions`), `CollectStatus` lines, guidance refusal counters and governor line. Ambiguous engages after
  0.5 s of measured pair time and releases on the first other record.
* **Determinism.** Every submitted pair is fitted, late or not, so the fit history (hash `HistoryHash()`) does not depend on
  timing; deterministic mode (`CameraLinkSettings::deterministic`) starts no thread and drains with `ProcessPending()`.
* **No hard-coded rates.** Pair seconds, deadlines, cool-down and `d` all come from measured values.

## Activation (report mode, 2026-11-01)

`CameraExtension.h/.cpp` is the H1 adapter in REPORT mode (`[vpv.skeleton] camera_link = report`, off by default): per renderer a
`CameraLinkCore` over that renderer's `FrameAnalysis::Store` (found through `SourceFrameTap::StoreFor`), `OnGpuCreated`, the
Indicator reader through a FrameSeal provider, `OnFrameStart` at Begin (resync driving), `OnPostSubmitGap` after render, status and
decisions into the hub's status. No vector engine exists yet (P5a), so the core is configured with `p7Wanted = false`: no thread,
pool, ring or product. It never feeds a consumer and answers no `vpv.camera.v1` query. `VP_CAMERA_DUMP=1` logs the status once
a second (K3: `VP_CAMERA_DUMP=<dir>` writes flow files instead, see below). The rows below remain the plan for the full wiring.

## K3: C0 diagnostics (keys, status, dump; 2026-11-07)

Design: `D:\codex\vp-handover\61-design-p2-sourceid-camera-k3-k10.md` section 3.1. All inert and default off; the Original pathway forces every key off.

| Part | Files | Role |
|---|---|---|
| Keys | `CameraSettings.h` | section `[vpv.camera]`: `camera_model`, `camera_model_overlay` (off, labels, residual, vectors), `camera_labels_gpu`, the 10 `camera_meas_<name>` (one per `MeasurementSwitches` member except `objects`), `camera_use_{grain,detail,dither,deband,filler,dejudder}`. Parser, `Validate` (Verdict like `VpvSchemaKeys.h`), `ToConfigText` (round trip). Invalid value: default kept, listed (`camera_settings`); unknown key: listed, never rewritten. The switches and the consumers are inert unless `camera_model` is on (`Measurement()`, `Use()`, `Overlay()`, `LabelsGpu()` give off) |
| Status | `CameraStatus.h` | one pure builder (`BuildStatus`) for the Ctrl+I line, Config status, Fallbacks and the Ctrl+Alt+D section (per-consumer weight, deadline misses, `MeasurementDiagnostics` line). A switch that cannot act is shown: model off, `camera_link` off, no vector engine yet (P5a), an overlay that cannot draw. `CameraExtension` feeds it from the settings, `CameraLinkCore::LastFit()` and the counters |
| Dump | `CameraDump.h/.cpp` | `VP_CAMERA_DUMP=<dir>` (and `VP_CAMERA_DUMP_CLIP`, default `clip`): `<dir>/<clip>/flow_<a>_<b>.npy` forward and `flow_<b>_<a>.npy` backward (float32 (gridH, gridW, 2), source px, names as `tools/eval/vector_source.py`), `camera_<b>.json` with a `mode` tag (`engine`; `engine_camera` when a measurement switch is on). Fed from the camera worker after `Fit` (`CameraLinkCore::SetDump`): two shared_ptr copies and a bounded push; a full queue drops the new item and counts it (status + Fallbacks `camera_dump`). The writer thread exists only when the variable names a directory and a core exists (`camera_link = report`). `VP_CAMERA_DUMP=1` keeps its older meaning (status log once a second) and starts no writer |

Config: the keys have rows on the VP-V page, Motion unit, Advanced tab, card "Camera model" (`MakeCameraCard` in `config/VpvSection.cpp`; objects `vpv.camera.<key>`, a status row, greyed in the Original pathway and, except the model switch, while the model is off). Test: `testCameraModelControlsReachVp` (`config/tests/VpvConfigTests.cpp`).

Not in K3: the **overlay** itself (K2's P8 is not instantiated and the fit has no vector engine, so nothing could be drawn; the key parses, and a request is shown as "cannot draw yet").
`camera_model` allocates nothing by itself: `p7Wanted` stays false until P5a. Tests: `CameraSettingsTests`, `CameraStatusTests`, `CameraDumpTests`, `CameraOverlayOffTests`.

## Wiring TODO (the H1 adapter, ~40 lines, a later step; needs `feat/m2-hooks` c716298f)

`CameraExtension : IRendererExtension`, registered after `ResyncExtension`:

| H1 call | CameraLinkCore call |
|---|---|
| `OnRenderPhase(FrameStart)` after FrameSeal | `OnFrameStart({seq, sealUs, displayPeriod, p7SealedPublished})` (also `table.Resolve(...)` with `fill` calling `CameraLabelFill::FillLive` for P8) |
| `OnRenderPhase(PostSubmitGap)` | `OnPostSubmitGap()` |
| GPU created / released | `OnGpuCreated(pl_gpu)` / `OnGpuReleased()` |
| settings change | `OnSettings(...)`, `AttachGraph(...)` on a graph rebuild, `CameraLabelProducer` mode from `ChooseLabelMode` |
| `CollectStatus` | `CollectStatus(lines)` + `CollectDecisions()` into the fallback registry |
| `QueryInterface("vpv.camera.v1")` | `Link()` |
| graph rebuild | `GuideLagMeter::Lag(sourcePeriod, deterministic)` -> `GuidanceScheduler::SetLag(d)` + `SkeletonProductsWithGuide(d)` |
| camera adapter, once the engine exists | `CameraGuidanceAdapter(core, settings, {p4Reader, nextPair, onRenderThread})`; `Attach(engine, std::make_unique<GuidanceCountsRing>(gpu))`; per frame `OnFrame()` and `FeedPassTimers(passTimerHook.acc)`; `CollectDecisions()` into the fallback registry; `RebuildGraph(period, deterministic)` before `AttachGraph`; `Detach()` at GpuReleased. Add `GuidanceScheduler.cpp`, `GuideLaneJob.cpp`, `GuidanceAdapter.cpp`, `GuidanceCountsRing.cpp` to the renderer items props in that commit |
| `Estimate` / `EstimateAhead` (P5a) | none: P5a's `VectorEngine::Estimate` calls the registered hook |
| Indicator setup | `RegisterReader(indicator)` |
| P5a engine | `SetEngine(IVectorEngine*)`, `engine->SetPreEstimateHook(&CameraGuidanceHook)` |

Also pending: `FieldReadbackRing` becomes an `IReadbackDevice` again at K1-9 (the half-wired methods were removed); `items.props` (H7) listing `GlobalModel*.cpp` and `link/*.cpp`; K1-9 (LaneRunner registration of
`CameraFitProducer` at SK-W); a text-guard / pixel-residual producer ("text guard: none" is shown until then).

## Tests

CPU: `PairTimingTests`, `PairAssemblyTests`, `VerdictGateTests`, `CameraFitRunnerTests`, `CameraFitThreadingTests`, `CameraOffTests`,
`CameraSettingsTests`, `CameraStatusTests`, `CameraDumpTests`, `CameraOverlayOffTests`, `CameraLabelUploadTests`, `CameraConsumerBindingTests`, `P8FillRuleTests`, `GuidanceSchedulingTests`, `GuideProductTests`,
`GuidanceGovernorTests`, `MotionBenchWriterTests`. WARP (category `GpuWarp`): `FieldReadbackRingWarpTests`, `CameraLabelWarpTests`,
`GuidanceNullIdentityWarpTests`.

## Phase interpolation (K9-2, pure unit, not wired)

`CameraPhaseInterp.h` interpolates camera models at a phase through log-scale and shortest-arc angle (never homography entries)
with a per-pixel evaluation and a block-grid helper; degenerate models fall back to the usable end or the identity (`ok = false`).
It also holds the strict de-judder policy constants 0.85 / 0.7 / 0.05 / 0.2 (pinned equal to `PolicyFor(DejudderWarp)`).
Tests: `../../tests/CameraPhaseInterpTests.cpp`. The P9 producer (`CameraSmoother`, K9-1) is a separate step.

## All settings (complete list)

The link has exactly one key, `camera_link` (`[vpv.skeleton]`, off / report, default off; Original (Bill): off; Config placement: none yet, planned master VP-V page > Pipeline (expert); visible state: the `camera_link` Fallbacks entry and the camera status line; not tiered). Fit options (`twoLayer`, `dynTexture`) are internal `Config` fields. K3 added the `[vpv.camera]` keys (section "K3" above; full table in `../../README.md`). The full table is in `../README.md` ("All settings") and `../../../../control/skeleton/README.md`.

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.
