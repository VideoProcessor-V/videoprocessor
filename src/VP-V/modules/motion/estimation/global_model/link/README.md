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
| Ahead fit (`camera_ahead_fit`) | `AheadFlow.h`, the park in `CameraFitWorker.*`, `PollAhead` / `OnTimeSeal` in `CameraLinkCore.*` | the fit from the ahead NVOF pair (FlowOf), on-time publish or the lag bridge, visible counters ("Ahead fit" below) |

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

**STATUS (camera live wiring, branch `r1cam/camlive`; step 2 `r1cam/slots`): K4 is reachable, default OFF.** `CameraExtension` (no skeleton demand until SK-W2, see below)
skeleton, builds the lag bridge (`CameraLane`, below) at the first verified engine field and attaches the core to it when `camera_link = report` AND `camera_model = on` (`p7Wanted`);
with any `camera_meas_*` switch also on it creates the `CameraGuidanceAdapter` and attaches it to the renderer's engine
(`Query("vpv.vector_engine.v1")` -> `IVectorEngineDirectory::EngineFor(host)`), which registers the ONE pre-estimate hook. The four .cpp files
(`GuidanceScheduler`, `GuideLaneJob`, `GuidanceAdapter`, `GuidanceCountsRing`) and what they need (`CameraLane`, `CameraLabelFill`, `CameraLabelProducer`,
`CameraModelGpu`, `GlobalModelMeasurement`, `GlobalModelSearchHints`) are in both renderer items props. All keys default off: nothing below exists then.
No Bill file changed; the camera's cost rows reach the pass-timer readout through the motion inbox that SK-W's host call drains (G3 below, closed).

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
  `CameraLinkCore` posts `camera.fit` (CPU work, the worker's fit) and `camera.readback` (a WAIT: request -> ready latency) to the motion pass-timer
  inbox (`PassTimers::Inbox::Motion().PostCameraFit / PostCameraReadback`, SK-W); the host drains it into `PassTimerHook::acc` (LVR, SK-W's drain).
  The camera-private sample queue (`DrainTimerSamples`, `FeedPassTimers`, `TimerSample`, `AddCameraRows`) is gone (camera-live step 2).
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

> **LAG BRIDGE UNTIL SK-W2** (camera-live step 2). What SK-W covers now goes through SK-W: the fit and the label fill hold their table through OWNED leases
> (`CameraLinkCore::AttachLane`, `CameraLabelFill(LaneLeaseSource, ...)`; a generation change rebinds and refuses a stale job, `PublishOutcome::StaleLane`), and the
> costs go to the pass-timer inbox. `CameraLane` remains ONLY as the lagged seal, because the skeleton lane seals every product of frame N at the render
> start of N (`SlotTable::Resolve`; `SlotSet::Seal` has no deferral) and P7(N) only exists a readback + fit later. See "Lag bridge until SK-W2" below.

### Lag bridge until SK-W2

| Kept in `CameraLane` (smallest surface) | Why it cannot move yet | What SK-W2 does instead |
|---|---|---|
| the lagged seal (`OnFrame` -> `SealOne` at N + `kSealLag`, the Indicator's stamp of N) and its `Sealed {p7Published, hadField}` result | the skeleton seals P7(N) at N: it would be absent on every frame | seals P7(N) (and P8(N)) at N + lag in the skeleton lane and reports the result to the camera |
| the table P7 / P8 are published into, and its `Lease()` / `Source()` (owned, one generation per `Build` / `Clear`) | a lagged seal needs the slots it seals | the lease source becomes `[lane]{ return lane->Lease(kP7CameraFit); }` (and `kP8CameraLabels`): one line in `CameraExtension::EnsureLive` |
| the input markers (empty P3 / P4 / P2m / P1 slots with the honoured stamp) | the lagged seal checks P7's ancestry inside its own table; the skeleton's P3 / P4 slots are sealed and released on the skeleton's schedule | P7's ancestors are the skeleton's own P3 (engine feed) / P4 / P2m slots |
| the field marks (`NoteField`) telling a cadence repeat from a late fit | same seal | the skeleton's engine feed knows which frames had a field |
| the P8 LIVE fill: `SealOne` passes `CameraLabelFill::FillLive` as the seal's fill for P8 | the skeleton seal declines every live fill of a product published outside the lane (`AnalysisLane::LiveFillOf`, `externalDeclined_`, AnalysisLane.h ~l.1469) | a fill hook for an external product (P8 live from a sealed P7) in the skeleton seal |
| `NextFloor()` (the label fill's gap-work floor: the first frame not sealed yet) | follows the bridge's own seal position | the skeleton's lagged seal position |
| `LabelRing()` / `LabelLive()` (P8 ring and live-pool sizes the label producer's textures are sized by) | read from the bridge's P8 node | the skeleton graph's P8 node |
| `ReleaseAfter(seq)` after each lagged seal (the N2 retire: a frame's slots are released only after it was sealed) | the bridge seals late, so it must also retire late | the skeleton retires P7 / P8 (and the ancestors they pin) after their lagged seal |
| (not in the bridge) the skeleton demand: `SetExternalDemand(host, owner, DeclareCameraConsumers(LiveCameraDemand(labels)), {P7, P8})` | see "No skeleton demand yet" below | declared in `CameraExtension::Reconcile` while the fit is wanted, cleared when off / at GpuReleased |

**Shown while it runs.** While the bridge carries the fit, Ctrl+I shows `camera: private lag bridge (P7 / P8 sealed at N+6 in the camera's own table, not in the skeleton slots) until SK-W2` and Fallbacks the entry `camera_lag_bridge: <same text>` (`CameraExtension::kLagBridgeId` / `LagBridgeText()`); nothing with the camera off (test `TheLagBridgeIsShownWhileItCarriesTheFitAndNothingWhenTheCameraIsOff`). SK-W2 removes it with the bridge.

**No skeleton demand yet (decision, camera-live step 2).** `publishedOutside` only stops the skeleton from attempting a live fill: the frame is still sealed
absent and counted (`ExternalDeclined`, "Analysis: products published outside the lane ...") on EVERY frame, and the demand also sets `feedNeeded` /
`SetCameraFitWanted` (SkeletonModule.cpp ~l.1296 / ~l.1381), so the engine feed would publish P2m / P3 slots that nothing consumes. No SK-W mechanism lets a
declared product be "expected later". So the camera declares NOTHING to the skeleton until SK-W2; P7 / P8 stay on the bridge, the skeleton status is unchanged
with the camera on, and the camera's own behaviour is unchanged (it never depended on the demand: the engine runs for the camera as before). The entry SK-W2 uses
is in place and tested on a real module: `Query(Skeleton::kExternalDemandQueryId)` -> `IExternalDemand` (SkeletonModuleTests
`TheExternalDemandQueryGivesTheModulesOwnSetAndClear`, which also shows the absent-every-frame P7).

Open for SK-W2 too: with the bridge gone the camera needs the skeleton lane, which exists only with `analysis_lane` on (and `resync_event` on); today the
camera runs without it. P9 (no producer, K9-1) and p7g (the guide is not slot-backed) are not declared. Validity stays on the P5a `MotionValidity` facade
(`PairSpansSlip`); scene facts come from the frame-analysis store, not `SceneFactsOf`. `NoteRenderOnlyMs` is not fed: the camera has no measured render-only span.

| Part | Live now | How |
|---|---|---|
| P7 fit | yes, with camera_link = report + camera_model = on | `p7Wanted` is set by the keys, NOT by P5a's vector demand (that decision only covers P2/P3 and nothing in a renderer declares P7 as a consumer). The fit reads the engine's verified fields (`IVectorEngine::FieldOf`) through the readback ring |
| P7 as a skeleton product | sealed in the lag bridge; not yet declared to the skeleton (SK-W2) | the bridge graph is built from `LiveCameraDemand`, the list SK-W2 declares to the skeleton. The fit is held by lease on `CameraLane.h` (the lag bridge, same registry / SlotTable / SlotSet / CpuBuffers, graph from the same declarations), which publishes empty-content input markers for P3 / P4 / P2m / P1 each frame (ancestry + honoured stamp) and seals `kSealLag = 6` frames behind the render, with the Indicator's stamp of that frame |
| validity | era / stamp / slip | the record's honoured stamp is checked at the seal (a resync event or a slip between marker and seal rejects it); a pair that spans a slip or has an ancient stamp is not fitted (`camera.no_model.slip`) |
| trust | confidence, reason, P8 untrusted code | in the record (`CameraMotion`), the status line and `Usable()`; the labels carry the untrusted code (K2) |
| P8 labels | yes, when labels are wanted (overlay != off, any camera_use_* switch, camera_labels_gpu) and a render device exists | `CameraLabelProducer` in UPLOAD mode (CPU labels uploaded to a texture; compute mode with camera_labels_gpu on), `CameraLabelFill` as the post-submit gap work and the seal's live fill. Because the seal lags, a consumer reads the labels of a frame that has been sealed (`LabelsFor(seq)`); no consumer is wired to them yet |
| P9 smoothed | NO | no producer exists (K9-1 `CameraSmoother` is not built); declared by no consumer, so the graph never holds it |
| K4 guidance | yes, with a `camera_meas_*` switch on | adapter + hook; P4 facts for the lane job come from the frame-analysis store (`Identity.picture`, `Scene.cut / fade` when the scene group was written); a pair without a scene record is counted and shown (`camera_scene`) |
| status | Ctrl+I, Config status, Fallbacks, Ctrl+Alt+D | `CameraStatus.h` + the adapter's lines and decisions + `camera_engine` / `camera_graph` / `camera_fit` / `camera_gpu` / `camera_scene`; `camera_fit` = "camera fit waits for resync_event: turn resync_event on" (the seal is not driving: a visible fallback, since every camera default is off and an enabled camera is explicit) |

**Gaps (not closed here).** G1 the lagged seal: SK-W2 (see "Lag bridge until SK-W2"); the bridge's markers carry ancestry only. G2 (closed) `m2/p5a-outcome` is merged and mapped (see AfterEstimate above); the bench `gpu_ms` stays empty until the engine can measure a per-estimate GPU time. G3 (closed, camera-live step 2) `camera.fit` /
`camera.readback` go through the pass-timer inbox, which SK-W's host call drains into the readout.
G4 flash events and fade gain / offset have no producer (flash false, gain 1, offset 0). G5 `measuredSourceHz` is the ingress rate (`HostResync::measuredRateHz`); a cadence-locked source
uses the store's film rate when its cadence group was written, else the ingress rate.

### What an explicit camera-on adds per frame (camera_link = report, camera_model = on, resync_event = on; every camera_meas_* and camera_use_* off)

Read from the code, not measured. NO hook is registered (`guideWanted` is false: no adapter, no `SetPreEstimateHook`), so `VectorEngine::Estimate` runs the matcher's passes exactly as before.

| Cost | Where | Size |
|---|---|---|
| CPU fit | the ONE camera worker thread, once per verified field (cadence repeats have none); `Fitter::Fit` on the readback copy | async: the render thread never waits; p95 not yet measured on the laptop (indicative 9-12 ms CPU) |
| GPU -> CPU readback of `IVectorEngine::FieldOf(seq)` | D3D11 (the production backend): ONE small copy pass (a shader writes the engine's rgba32f forward field into a VP-owned slot texture by imageStore), ONE native CopyResource into a staging texture and an event query; read later with Map(DO_NOT_WAIT). Not D3D11: `pl_tex_download` into a host-mapped `pl_buf`, `pl_buf_poll(..., 0)`. The backward plane is requested only when the engine gives one (`FieldOf` gives none today) | 16 B per 16x16 block: 1080p 120 x 68 = 8,160 blocks = 130,560 B (0.12 MiB) per pair; 4K 240 x 135 = 32,400 blocks = 518,400 B (0.49 MiB) per pair. Ring of at least 3 slots allocated at attach (about 0.4 MiB at 1080p, 1.5 MiB at 4K); a full ring drops the request visibly |
| `CameraLane` (lag bridge) | CPU only: one lease read per frame (an atomic shared_ptr load), input-marker headers for about 4 nodes per frame (atomic claims / publishes), the lagged seal (`Resolve`, `ReleaseAfter`), no GPU, no allocation per frame | microseconds |
| Frame-analysis store reads | `UpdateTiming` (one cadence read per frame) and `DescribePair` (one record read per fit); each copies a `Record` (about 4 KB) under the store mutex | negligible |
| P8 labels upload | only when labels are wanted (overlay != off, any `camera_use_*`, `camera_labels_gpu`): none with the defaults | 0 |
| GPU operations added | the one copy pass and the one CopyResource + query per verified field, and nothing else (no guidance counts readback, no label upload, no change to any matcher pass) | |

### Resolved (F2, 2026-11-08): the fit reads the optical-flow SEED, never the guided live field

The camera fit's only input is now `IVectorEngine::SeedFieldOf` (HOOKS.md section 7): the engine's seed (`ofSearch`), which the
camera guidance never shapes. `CameraLinkCore` and `CameraExtension` never call `FieldOf`. Provenance by type: `SeedField` is a
distinct type with no conversion from `VerifiedField`; the ring stamps each copy (`FieldOrigin::Seed` / `Live`) and
`CameraLinkCore::SubmitField` refuses a `Live` copy (counted `guidedRefused`). No seed for a pair = no fit and a visible fallback, never the
guided field instead: `camera_fit: camera fit needs the optical-flow seed (engine off or matcher-only)`, or for a pair whose
searches were skipped at a cut `camera_fit: camera fit skipped this pair (cut: no seed)` (Fallbacks = Config status / Ctrl+I, and the
camera status command). Readback: the same two-plane copy (D3D11: the target was already two planes; the copy pass now also reads the backward
seed plane), converted on the CPU onto the 16-px source grid (each 16-px block takes the seed texel its centre lies in; trust =
the live consistency rule on the seed, with the live SAD limit: taper_sad under block_taper, the engine-weight penalty only when the
trust weights are applied). Note: the NVOF vectors are quantised to 1/32 px of the engine's input (the searching half
size), so the seed carries steps of 1/16 source px at 1080p and 1/8 source px at a lowRes 4K search before the fit pools blocks.

The history of the issue:
`FieldOf(seq)` returns the matcher's LIVE block field of the last successful estimate (HOOKS.md section 7; `MotionWarp::Field()`), and production feeds the fit from exactly that. With every guidance switch off that field is the unguided matcher output, so nothing feeds back. With `camera_meas_model_global` (or any switch that changes the estimate) ON, the matcher's field is the one the camera's own model guided, so the fit re-measures its own guide: diagnosis ac283dae2a47417e5 found a feedback drift of about 0.004 px per pair. The fix is to fit from the engine's seed field (the NVOF / pre-guidance field) instead of the guided live field; it needs a second field accessor on `IVectorEngine` and is not done. Until then, a measurement switch that changes the estimate (camera_meas_model_global, and a default-on option (a) if the owner chooses it) carries this drift.

## Ahead fit (owner decision, reset 1; key `camera_ahead_fit`, default off)

The camera fits from the AHEAD motion: the NVOF pair the one MotionAhead producer computed ahead of render (P2, served by
`IVectorEngine::FlowOf`), so P7 / P8 can be ready before their frame's seal. Off (the default) nothing below exists: no source, no poll,
no park, no extra line or decision, the pool and queue keep their sizes (`CameraAheadFitTests::OffIsInert`).

| Part | Where | What |
|---|---|---|
| Input | `AheadFlow.h` `CopyAhead` | `FlowOf(N)` (pair N-1 -> N) is called on the RENDER thread (`CameraLinkCore::PollAhead`, at Begin and at AfterRender), because FlowOf's contract is "call and read on the render thread" and its view points into the engine's lease, valid only until the next FlowOf call. Every array the fit needs is copied in that same call (`AheadCopy` owns them), so neither a later FlowOf nor a Prune of the P2 slot can free them. The copy goes to the camera worker as a job; the worker never calls the engine and never takes renderMutex (LOCK RULE). |
| Layout | `CopyAhead` | read from the view, never assumed: `gridW x gridH` cells of `cellEnginePx` engine px, `rawUnitsPerEnginePx` fixed-point units (S10.5 today), `enginePxToSource`, `engineW x engineH`; an inconsistent view is refused (`Layout`), a view without backward vectors or forward costs too (`Incomplete`: the engine trust cannot be computed, never guessed) |
| Conversion | `AheadToSourceGrid` | onto the camera's 16-px source grid (the seed's `sourceGridW x sourceGridH`, the grid that sized the camera graph): each 16-px block takes the TRUST-WEIGHTED mean of the cells whose centres lie inside it (2 x 2 cells at a half-size search, one at a quarter-size lowRes 4K search), its trust = the mean cell trust. Deterministic (fixed loop order). Origin `FieldOrigin::Ahead` |
| Trust | `OpticalFlowTrust::CellTrust` (`AheadCellTrust`) | the engine's ONE definition (MV5): the cost weight of the cell's de-judder block (`CostWeights` on the forward costs, the producer's `FlowExtras` maths, block `kEngineTrustBlockPx` = `MotionAhead::Flow::kBlockPx`) x the forward/backward consistency weight (`FbError`, FallOff from the matcher's consistency limit to twice it). The limit is the matcher's own (`SeedField::consistencyPx`, refreshed whenever a seed is seen; default 2, the dejudder_consistency_px default). Bit-equal to SK-MV's inline TD trust (`TheTrustIsTheEngineDefinitionAndIsMonotonic`); SK-MV's `SharedMotionSource::Take` should call `CellTrust` too |
| Provenance | `FieldOrigin::Ahead`, `SubmitAheadField` | the pair is the producer's UNGUIDED NVOF output. `AheadCopy` is made only from a `FlowView` (static_asserts: not from `VerifiedField` / `SeedField`); `SubmitField` still refuses `Live` (the guided field); `SubmitAheadField` takes only `Ahead` |
| Facts | `CameraExtension.cpp` `DescribeAhead`, `TakeAhead` | an ahead pair is not estimated by the matcher yet: no `PairSpansSlip` (FlowOf serves only pairs the vectors-valid state holds), the engine global from the pair itself (`MotionAhead::FlowGlobal`, one cell = `cellEnginePx x enginePxToSource` source px, usable at ambiguity < 0.9, as `VectorEngine::NoteEngineGlobal`), picture / cut / fade from the frame-analysis store |
| Order | `PollAhead`, the seed loop | the fit history is in frame order: the poll asks N = the next frame and stops at the first frame without a pair. When the render reaches a frame without an ahead pair (low latency, a producer drop, a pair not computed in time) that frame takes the SEED (F2's readback, the bridge path) and later ahead pairs wait until its fit is submitted (a seed copy that never comes back is given up after 16 frames). Back-pressure (a full queue) only defers the poll |
| On time | `CameraFitWorker` (park), `CameraLinkCore::OnTimeSeal` | an ahead job is fitted at once (submit order) and PARKED until it can publish: into the on-time target while its frame's seal has not run (the target's inputs of N are probed BEFORE the claim, because a claimed-then-abandoned slot can never be claimed again; "inputs not published" is retried at every Tick: Begin and AfterRender), else into the lag bridge once N began (its markers exist). At N's Begin the core reads the target's P7(N): Sealed -> the record goes to the store, `SealedRecord` serves it from N on, and the lag bridge's later seal of N is skipped (retired: no second verdict, no false "late") |
| Resync | `PoolEntry::honoured`, `OnTimeSeal`, `SealedRecord` | the publish reads the Indicator stamp of N after the pins (AheadLane provenance) and keeps it with the record; the seal re-checks it (a resync after the publish: rejected, never served, `onTimeRejected`), and so does every consumer read (`SealedRecord` against the Indicator of the lane it was published into: `staleAtUse`) |
| Visible | `AheadStatusLine`, `kIdAheadBridged` | Ctrl+I / Config status / Ctrl+Alt+D: `camera ahead fit: N on time, M bridged (A ahead fits late for the on-time seal, S from the seed: no ahead pair), L late, R rejected after the publish (resync), U refused at use; no pair / incomplete / layout; on-time target: <name or none>`. Fallbacks: `camera.ahead.bridged` while frames are bridged (held 0.5 s of measured time), and always while there is no on-time target |

### Deterministic mode: ONE path, the bridge

`CameraLinkSettings::deterministic` (N1 runs, `ProcessPending`): the ahead fit is OFF (`AheadActive()` false), the fit reads the seed through the
lag bridge as before, and the status says `camera ahead fit: off in deterministic mode (the deterministic path is the seed fit through the lag
bridge)`. Reason: whether an ahead result is on time or bridged depends on wall-clock (the producer's and the fit's timing); the bridge's
lagged seal does not. Test: `DeterministicModeTakesTheBridgePathOnly` (the ahead source is never asked).

### Ahead fit: gaps (reported, not worked around)

* **G-A1 no lease of the skeleton lane.** SK-W gives an outside component no way to the skeleton lane's `AnalysisLane::Lease(kP7CameraFit)`:
  `IExternalDemand` (the only entry the camera can query, `kExternalDemandQueryId`) has `SetExternalDemand` / `ClearExternalDemand` only, and
  `SkeletonModule::LaneOf` is on the concrete module, render thread only. So production has NO on-time target (`AttachOnTime` is not called):
  every ahead result is bridged, visibly (`on-time target: none ...`, `camera.ahead.bridged`). The mechanism is complete and tested against a
  lane that seals at N (`CameraAheadFitTests`); the fix is one accessor on `IExternalDemand` (a lease source per host and product) plus one
  `AttachOnTime([...]{ return lease; }, "skeleton lane")` line in `CameraExtension::EnsureLive`, with the demand declared as below.
* **G-A2 late = sealed absent.** The skeleton seals P7(N) at N's Begin with no deferral (`SlotSet::Seal`; `AnalysisLane::LiveFillOf` declines
  every live fill of a product published outside, `externalDeclined_`). With the demand declared, every frame whose ahead fit misses the seal
  is sealed ABSENT in the skeleton and counted there ("products published outside the lane ..."); the camera then bridges it in its own table.
  A frame cannot be "expected later" in the skeleton (SK-W2).
* **G-A3 P7's lag-0 inputs are render-time products.** P7 declares P3(N), P4(N), P2(N) at lag 0 (`core/Products.h`); the skeleton's P2 / P3
  slots of N are published by the ENGINE FEED from the matcher's verified field (`SkeletonModule::FeedEngine` at AfterRender of the frame that
  estimated N's pair, sealed at N's Begin). So an ahead P7(N) can only be claimed in the window [AfterRender(N-1), Begin(N)], however early it
  was fitted (the park retries at AfterRender for exactly that window). An ahead-fed P7 would need its inputs to be P2 (the NVOF pair) and P4.
* **G-A4 P8 on time.** P8(N) needs P7(N) and P3(N) published plus a GPU fill before N's seal, and the seal declines a live fill of an external
  product (G-A2), so P8 stays on the bridge (the label fill and its live fill at the bridge's seal). With an on-time target, a frame sealed on
  time has no P7 in the bridge, so its P8 would be absent: P8 on time needs the label fill against the target's lease (a second
  `CameraLabelFill(LaneLeaseSource)` over `kP8CameraLabels`) before the on-time target is switched on with labels wanted.
* **What the demand would be.** `SetExternalDemand(host, owner, DeclareCameraConsumers(LiveCameraDemand(labels)), {kP7CameraFit (, kP8CameraLabels)})`
  while the ahead fit is wanted and the target exists; not declared today (G-A1: nothing could publish into it).

### Measurement (prepared, not run)

`VP_TEST_CAMERA_ACCURACY_AHEAD=1` adds `model_global_ahead` and `all_switches_ahead` to the camera accuracy report (BetaGpuTests
`RunBetaCameraAccuracy`). The producer's NVOF pair is REPRODUCED OFFLINE (`tests/beta_gpu/AheadPairReplica.h`: the producer's `Process()` step
for step, SLOW preset, half / quarter luma by the shared half-luma unit, ping-pong hints, the same readback and extras; the Beta host does not
link the producer), served as the FlowView FlowOf gives and converted by the production `CopyAhead -> AheadToSourceGrid`. Outputs:
`summary.md` sections "Ahead fit" (timing vs the lead: pair / readback / convert / fit / latency, on-time share at leads 1-4 from a FIFO
two-lane model of the measured costs) and "Accuracy A/B: ahead fit minus F2 seed fit", plus `camera-accuracy-ahead.csv`. Script:
`D:\codex\videoprocessor\r1cam-ahead-run.ps1` (4 chunks: 24p / 59.94p x 1080 / 4K, FRAMES=120, GPU-QUIET protocol).

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
| settings change | `OnSettings(...)`, `AttachLane(source)` (live; see "Holding the lane" below) or `AttachGraph(table, bytes, indicator)` (tests / standalone), `CameraLabelProducer` mode from `ChooseLabelMode` |
| `CollectStatus` | `CollectStatus(lines)` + `CollectDecisions()` into the fallback registry |
| `QueryInterface("vpv.camera.v1")` | `Link()` |
| graph rebuild | `GuideLagMeter::Lag(sourcePeriod, deterministic)` -> `GuidanceScheduler::SetLag(d)` + `SkeletonProductsWithGuide(d)` |
| camera adapter, once the engine exists | `CameraGuidanceAdapter(core, settings, {p4Reader, nextPair, onRenderThread})`; `Attach(engine, std::make_unique<GuidanceCountsRing>(gpu))`; per frame `OnFrame()` (the costs go to the pass-timer inbox, no adapter call); `CollectDecisions()` into the fallback registry; `RebuildGraph(period, deterministic)` before `AttachLane`; `Detach()` at GpuReleased. Add `GuidanceScheduler.cpp`, `GuideLaneJob.cpp`, `GuidanceAdapter.cpp`, `GuidanceCountsRing.cpp` to the renderer items props in that commit |
| `Estimate` / `EstimateAhead` (P5a) | none: P5a's `VectorEngine::Estimate` calls the registered hook |
| Indicator setup | `RegisterReader(indicator)` |
| P5a engine | `SetEngine(IVectorEngine*)`, `engine->SetPreEstimateHook(&CameraGuidanceHook)` |

## Holding the lane (SK-W fix: the use-after-free)

The lane swaps its whole `LaneState` (graph, `SlotTable`, buffers, Indicator hold) at every rebuild: an external-demand change, a VRAM re-plan, a moved queue or period.
A raw `SlotTable*` kept by the fit thread across that swap was a use-after-free. The link now holds the lane through a lease (`core/LaneLease.h`):

| API | Meaning |
|---|---|
| `AnalysisLane::Lease(kP7CameraFit)` | the current `LaneLease` (shared hold of the `LaneState`, the P7 bytes ring, the Indicator, `generation`); any thread |
| `CameraLinkCore::AttachLane(std::function<LaneLease()> source)` | live attach: the link takes the first lease now and re-acquires through `source` (bind `[lane]{ return lane->Lease(kP7CameraFit); }`) |
| `CameraLinkCore::AttachGraph(table, bytes, indicator)` | borrowed, unowned, generation 0: for tests / standalone drivers whose objects outlive the link (never stale) |
| `CameraFitWorker::Rebind()` | takes the lane's current lease; true when it changed (`FitCounters::rebound`). Called by `Submit`, by every publish and by `CameraLinkCore::OnFrameStart` (which also resizes the seal marks if the P7 ring changed) |
| `CameraFitWorker::Lease()` / `LaneGeneration()` | a copy of the held lease (safe to use after any rebuild) / its generation |
| `CameraFitJob::laneGen` | the generation stamped at `Submit`; a job whose generation is not the worker's at publish time returns `PublishOutcome::StaleLane`: refused, `FitCounters::staleLane`++, shown in `StatusLine` ("stale lane (refused after a graph rebuild)", "lane rebinds"); the frame then seals without P7 (`camera.no_model.late`) |

`CameraLabelFill` (P8) holds the lane the same way: `CameraLabelFill(LaneLeaseSource, link, device)` (bind `AnalysisLane::Lease(kP8CameraLabels)`), `Rebind()` at every entrance, the lease held for a whole production, a production claimed in an older generation abandoned and counted (`LabelFillCounters::staleLane` / `rebound`, `LabelFillStatusLine`). The old `(SlotTable&, link, device, indicator)` constructor is the borrowed (tests) form. Lease reads happen outside any lock, so installs follow `VpSkeleton::LaneLeaseNewer` (never backwards; test `AnOlderLeaseReadNeverReplacesANewerOneWhateverTheInterleaving`).

The publish takes the lease once and keeps it for the whole Claim -> write -> Publish, so a rebuild during it cannot free what it writes; the old state dies with its last holder
(outside the worker's mutex). Tests: `CameraFitThreadingTests` (a rebuild with a held lease: old state alive, stale post refused and counted, then a rebound post; a threaded stress).

Also pending: `FieldReadbackRing` becomes an `IReadbackDevice` again at K1-9 (the half-wired methods were removed); `items.props` (H7) listing `GlobalModel*.cpp` and `link/*.cpp`; K1-9 (LaneRunner registration of
`CameraFitProducer` at SK-W); a text-guard / pixel-residual producer ("text guard: none" is shown until then).

## Tests

CPU: `CameraAheadFitTests`, `PairTimingTests`, `PairAssemblyTests`, `VerdictGateTests`, `CameraFitRunnerTests`, `CameraFitThreadingTests`, `CameraOffTests`,
`CameraSettingsTests`, `CameraStatusTests`, `CameraDumpTests`, `CameraOverlayOffTests`, `CameraLabelUploadTests`, `CameraConsumerBindingTests`, `P8FillRuleTests`, `GuidanceSchedulingTests`, `GuideProductTests`,
`GuidanceGovernorTests`, `MotionBenchWriterTests`. WARP (category `GpuWarp`): `FieldReadbackRingWarpTests`, `CameraLabelWarpTests`,
`GuidanceNullIdentityWarpTests`.

## Phase interpolation (K9-2, pure unit, not wired)

`CameraPhaseInterp.h` interpolates camera models at a phase through log-scale and shortest-arc angle (never homography entries)
with a per-pixel evaluation and a block-grid helper; degenerate models fall back to the usable end or the identity (`ok = false`).
It also holds the strict de-judder policy constants 0.85 / 0.7 / 0.05 / 0.2 (pinned equal to `PolicyFor(DejudderWarp)`).
Tests: `../../tests/CameraPhaseInterpTests.cpp`. The P9 producer (`CameraSmoother`, K9-1) is a separate step.

## All settings (complete list)

The link has one key of its own, `camera_link` (`[vpv.skeleton]`, off / report, default off; Original (Bill): off; Config placement: none yet, planned master VP-V page > Pipeline (expert); visible state: the `camera_link` Fallbacks entry and the camera status line; not tiered). Fit options (`twoLayer`, `dynTexture`) are internal `Config` fields. K3 added the `[vpv.camera]` keys (section "K3" above; full table in `../../README.md`); the ahead fit added `camera_ahead_fit` there (a link key: `kCameraLinkKeys`, not on the Camera model card yet). The full table is in `../README.md` ("All settings") and `../../../../control/skeleton/README.md`.

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.
