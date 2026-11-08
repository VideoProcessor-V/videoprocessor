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

**STATUS: K4 is NOT reachable at runtime yet.** Nothing instantiates `CameraGuidanceAdapter`, and the guidance sources are not in the renderer
projects (`GuidanceScheduler.cpp`, `GuideLaneJob.cpp`, `GuidanceAdapter.cpp`, `GuidanceCountsRing.cpp` are in the Test project only). It becomes
reachable only when `CameraExtension` wires the adapter in ONE commit: the four .cpp files into `VideoProcessor-VPRenderer.items.props` /
`...Beta.items.props`, the host instantiation (`Attach` with the renderer's engine from `IVectorEngineDirectory`, `OnFrame`, `FeedPassTimers`,
`Detach`) and the `MeasurementSwitches` from K3's `vector_*` keys. That step needs K3 (keys, rows, status: audited on `r1cam/k3b` `4c52b7e86`).
Until then every switch is off by construction and the engine runs its `Estimate` only.

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
* **AfterEstimate shim.** P5a's `AfterEstimate(pairSeq, guidanceUsed, countsTex)` forwards into `CameraLink::EstimateOutcome`
  (`CameraGuidanceHook::AfterEstimateEx`: narrow idle, rejection text, gpuMs). When M2's plain-data outcome lands the only edit here is one override
  mapping it onto `EstimateOutcome`. Until then "engine field used, narrow idle" and the bench `gpu_ms` stay empty.
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
