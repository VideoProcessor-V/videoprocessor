<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Skeleton activation (`control.skeleton`)

Switches the analysis lane (`core/AnalysisLane.h`) on through the VP-V hub, one lane per renderer, and (70b G5) hosts the render-thread
GPU dispatcher for the GPU products P10 / P11 / P12. **Off by default**: with `[vpv.skeleton] analysis_lane = off` (and in the Original
pathway) there is no state, no thread, no source tap, no seal provider, no dispatcher and no status line; the renderer behaves byte for
byte as before.

Unit: control/skeleton

## Settings

Keys of `[vpv.skeleton]` in `VideoProcessor.cfg`. The file is read at `GpuCreated` and at every `SettingsApplied` (`code != 0` = the Original
pathway = off), on the render thread. An invalid value keeps the default and is shown under Fallbacks (`skeleton_settings`), never log-only.
"Original (Bill)" is the Original pathway: the lane is off, no state, thread, tap, dispatcher or status line.

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `analysis_lane` | [vpv.skeleton] | enum | off, shadow, on | off | not in Config (cfg-only until the M2 Config page) | off (nothing built) | `analysis_lane` (reduced / forced live / at minimum), `skeleton_lane` (waiting for `resync_event=on` or `frame_analysis_store=on`), `skeleton_gpu_shadow` ("shadow: GPU products skipped"), `skeleton_gpu_device` ("GPU products: no render device"), Ctrl+I line, Ctrl+Alt+D | the lane plans its tiers by video memory (VramGovernor); shadow = CPU lane only, no GPU dispatcher; on + a render device = CPU and GPU products (see the shed order under Status and fallbacks) |
| `analysis_lane_chunk_us` | [vpv.skeleton] | int | 0..4000000 (0 = derived from the frame period) | 0 | not in Config (cfg-only until the M2 Config page) | n/a (VP-only) | invalid value: `skeleton_settings`, default kept | same on all |
| `analysis_lane_deterministic` | [vpv.skeleton] | enum | auto, on, off | auto | not in Config (cfg-only until the M2 Config page) | n/a (VP-only) | `analysis_lane_determinism` when frames miss the bounded wait | same on all |
| `analysis_lane_test_wait_ms` | [vpv.skeleton] | int | 0..4000000 | 0 | not in Config (cfg-only until the M2 Config page; test only) | n/a (VP-only) | invalid value: `skeleton_settings` | same on all |
| `camera_link` | [vpv.skeleton] | enum | off, report | off | not in Config (cfg-only until the M2 Config page) | off (no state, no line) | `camera_link` when asked to report but unable (no store, no seal); status line when on | same on all |
| `skeleton_gpu_gap` | [vpv.skeleton] | enum | off, after_render | off | not in Config (cfg-only until the M2 Config page) | n/a (VP-only; off in the Original pathway) | after_render: `skeleton_gpu_gap` ("GPU lane work runs ahead of the present"); off with a dispatcher: `skeleton_gpu_gap_off`; invalid value: `skeleton_settings` | off: the GPU products get no lane producer (in-render-only, live form deferred); after_render: P10/P11/P12 lane jobs run in the AfterRender gap, chunked by the gap budget (one chunk per pass); the CPU products' gap is unchanged either way |
| `grain_split_storage` | [vpv.skeleton] | enum | full, compact | full | not in Config (cfg-only until the M2 Config page) | n/a (VP-only) | `grain_split_storage` entry of the kernel (Compact refused for the plane -> Full, with the reason; Compact granted -> Released); invalid value: `skeleton_settings` | P10 storage either/or; Compact is also the governor's visible budget step (Compact -> Full / Full -> Compact is shown by the kernel's decision, never silent) |
| `source_stats_walk` | [vpv.skeleton] | enum | original, lane | original | not in Config (cfg-only until the M2 Config page) | Bill's active-picture walk | planned: the cfg key is not read from the file yet (a lane key set by tests and tooling); `source_stats_walk` entry when the lane's source stats are unavailable | same on all |
| `pipeline_entrance_once` | [vpv] (global; also read from the profile) | bool | on, off | on | VP-V > Pipeline (expert) (a test key; stored in [vpv]) | Original pathway sets `off` (ADR-0019) | B3 (ADR-0024): the motion consumers warp or blend the processed A and B (the entrance stages run once per source frame) instead of re-running the stages for every picture; off = per-picture stages (a diagnostic) | same on all (not tiered) |
| `pipeline_order` | [vpv] (global; also read from the profile) | string | slot names in the registry's syntax (letters, digits, _ . , ; > and space); validated by the renderer, an invalid order never runs | empty (the designed order) | VP-V > Pipeline (expert) (stored in [vpv]) | Original pathway clears it (the designed order, ADR-0019) | Moves slots within their zone (ADR-0021) | same on all (not tiered) |
| `pipeline_original_pathway` | [vpv] (global; also read from the profile) | bool | on, off | off | VP-V > Pathway (stored in [vpv] as vpv.pathway) | n/a (VP-only): unchanged by the Original pathway | The Original pathway baseline (ADR-0019): every VP addition is switched off and every slot that exists upstream runs in its upstream form, reproducing Bill's original pathway | same on all (not tiered) |
| `pipeline_slots_off` | [vpv] (global; also read from the profile) | string | slot names in the registry's syntax (as pipeline_order) | empty | VP-V > Pipeline (expert) (stored in [vpv]) | n/a (VP-only): unchanged by the Original pathway | Slots switched off at slot level; motion and queue/timing slots switched off run in their upstream form | same on all (not tiered) |

`source_stats_walk` is a lane key (`LaneSettings::Parse`) that the module's key loop does not pass from the file yet (tests and tooling set it; the cfg key has no effect today). `skeleton_gpu_gap` and `grain_split_storage` are parsed in `SkeletonSettings.h` with the same problem-reporting style as the lane keys.
`camera_link = report` only creates the camera link core and its status line; there is no vector engine yet, so it never feeds a consumer.

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| Ctrl+I (status overlay) | one `skeleton=analysis lane <mode> <w>x<h>, ...` line while the lane runs | off with the lane | runtime |
| Ctrl+Alt+D (diagnostics) | the full status text and every engaged fallback of this module (`OnCommand("skeleton","status")` returns the same text) | n/a | runtime |
| `VP_TEST_GPU` | `hardware` runs the GPU tests of the dispatcher and the three kernels on the real adapter instead of WARP | WARP | test |

No `VP_*` variable changes the module's behaviour.

## Data sources

* **Source frames:** `VpFrameAnalysis::SourceFrameTap` (owner key = the renderer identity `host`). The frame-analysis worker decodes the shared
  luma plane for a registered sink and delivers it as a `SourceFrameView` (10-bit sampler codes, `msbShift` 6, `keepAlive` = the plane). Needs
  `frame_analysis_store = on`; without it the module shows `skeleton_lane: no source frames: needs frame_analysis_store=on`.
* **The one seal:** `ResyncExtension::SealFor(host)` (a `FrameSeal` provider rebinding the lane to each new-era Indicator and a subscriber calling
  `OnSealed`). Needs `resync_event = on`; otherwise `skeleton_lane: waiting: resync_event=on required`. The hooks are added once per renderer;
  switching the lane on after the first frame counts as a late provider (the lane binds at the next era; shown).
* **Queue phase:** hub `OnFrame(CollectAhead)` (P5a's call site; `phaseData` = `QueueView`) -> the lane-lead term: `QueueView::size` = frames queued
  ahead of the render. Inert while the lane is off or in the Original pathway.
* **Render phases:** hub `OnFrame(Begin)` -> `OnRenderStart` (lazy `Configure` once the source size is known / changes; never GPU work here:
  Begin runs before the render lock); `OnFrame(AfterRender)` -> deferred releases + dispatcher `Configure` (GPU-legal) -> post-submit gap (half the
  measured frame period; skipped and counted while it is unknown) -> `OnFrameRetired`; `AfterRender` also feeds the S5 regulator (`AnalysisLane::OnFrameTiming`,
  see "Regulator input" below); `OnQueue(Drop)` -> `OnFrameDropped` (applied at the next Begin).
* **Render device:** `LifecycleFacts::gpu` (the renderer's `pl_gpu`) is stored at `GpuCreated` (per-host aux slot) and read when the dispatcher is
  decided; `LifecycleFacts::device` serves `IDXGIAdapter3::QueryVideoMemoryInfo` at each lane `Configure` (a failure leaves zeros and is shown).

## GPU products and the dispatcher (70b G5)

* A `SkeletonGpuDispatcher` (own `pl_dispatch` on the renderer's `pl_gpu`) with `GrainSplitKernel` (storage per `grain_split_storage`),
  `RegionStatsKernel` and `LightLevelKernel` exists per renderer **only** while the lane is wanted AND `analysis_lane = on` AND a `pl_gpu` was given.
  Shadow, Off, the Original pathway and a missing/unusable render device construct none: no GPU call is ever made (tested), and the status says why.
* The Bundle (built when the source size is known) registers the three `GpuComputeLaneJob`s (`FixedStepsTag`, steps = `StepsFor(id, dims)`) as lane
  producers before the worker threads start, only with `skeleton_gpu_gap = after_render`; P12 carries a `LightLevelBeginGate` on `ReadOf(P12, seq-1)`
  ("waits for P12(N-1)"). A previous frame whose live fill is still *pending* for the Entrance phase is read as not made until G6 drives `RunDeferredLive` (the gate must never wait for work nothing runs): P12(N) then starts without a previous frame and the one-pole resets, shown as `light_level_smoothing`. Their predicted cost is the larger of the dispatcher's measured time and a conservative figure derived from the
  declaration's reference ms and the pixel count (never a fixed resolution or rate). The lane is configured on `TapFormProducts(SkeletonProducts(compact))`
  when a dispatcher is wired (the plain registry otherwise): P13 (RenderOnly, never filled) is dropped from P10/P11 or P10 would be InputMissing every frame.
* The declining live fill is installed only when there is no dispatcher (a live fill would win over it). With a dispatcher the lane defers GPU live
  fills (`SetGpuLiveDeferred`); `RunDeferredLive` is not called before the Entrance phase (M2 P4), so the status shows `live: waits for the Entrance phase (M2 P4)`.
* Textures are allocated at `Configure` (never per frame) in AfterRender, before the gap; the previous set is destroyed at the next AfterRender
  (`FlushReleases`) or at `GpuReleased` / switch-off (`Release`).
* With `skeleton_gpu_gap = off` the GPU products have no lane producer (in-render-only); until the Entrance phase exists nothing makes them, which the
  status states (`skeleton_gpu_gap_off`, `skeleton_gpu_live`).
* **Record histories follow the queue (owner ruling 2026-11-07):** the quality-pattern and td trail-summary depths are
  `max(floor seconds, queue multiple x (render-ahead + lane lead + look-ahead frames) x measured source period)`, floors 2.0 s and 0.25 s,
  queue multiple 1 for both today. The three queue terms are one atomic snapshot (`SkeletonModule::PublishQueue`, any thread; the render thread
  reads it once per frame in `Begin()`), and a change reconfigures the graph at the next frame, as for a moved source period. The
  look-ahead walk is delivered by the host's `SettingsApplied` lifecycle call (`LifecycleFacts::value` = `motion_ahead_frames`: 0 or the default
  depth; `PublishLookAheadWalk`). The other two terms are live feeds (owner decision 2026-11-07: **no new setting**; the lane lead is the number of frames
  actually queued ahead of the output, measured live, shown in Ctrl+I, and a drop in it is visible):
  * **Lane lead** = the input queue's size at the `CollectAhead` phase (`PublishLaneLead`, called by `OnCollectAhead`). The value published is the
    lowest size over the last half second of source time (a fall is taken at once, a rise only when the whole window is above it) so the graph is not
    rebuilt for every wobble of the queue. It is stored as fed (0..255) and capped when the graph is built: `RingSlotBudget()` frames (27 today:
    `kRingHeadroomFrames` minus the queue, span and largest lead-read slots) are shared by render-ahead + lane lead, because a ring that needs more than
    255 slots rejects the graph and the lane would be lost. A cap that bites is shown (`skeleton_lane_lead`, Ctrl+I "lane lead N (fed M, capped)").
    Memory beyond the slot count is the VRAM governor's: it sheds record history depth first, visibly.
  * **Render-ahead depth** = planned lead / source period (`PublishRenderAhead`, rounded up), delivered by ONE generic host call at the renderer's
    render-ahead plan block (`RendererExtension::Lifecycle::QueueDepth`). Until a render-ahead plan has run (render-ahead off, or not planned yet) the
    term is "not fed" and shown so.
  * **Feed rule:** every feed compare-and-swaps ITS term into the one snapshot (`MergeTerm`) and never calls `PublishQueue` with a fresh `QueueShape`
    (that would erase the other terms). `PublishQueue` and `SetQueueForTesting` are test entry points.
  * **Queue starve:** a lane lead in use below the best it had since the lane was built is `skeleton_queue_starve` (lead, peak, falls, and the
    look-ahead products that fell back to their causal substitute, if the graph has any); it releases when the queue recovers. Ctrl+I carries
    `lane lead N` and Ctrl+Alt+D the "Skeleton queue:" line (render-ahead, lane lead, look-ahead walk, queued ahead of output, falls).
  Undelivered terms sit at their defaults, the depths at their floors, and Fallbacks says so:
  `skeleton_queue: record history: queue not yet wired (render-ahead depth, lane lead); floors in use` (it names only the terms still undelivered).
  `SetQueueForTesting` is test-only and single-threaded; it is not a feed.
* **Regulator input (S5): observe only until a render-only span is measured.** `AfterRender` records the span from the end of `Begin` to the entry of
  `AfterRender` (slack = period - span, "over the period" = span above the period) and shows it ("Skeleton regulator input: derived from render time
  (observe only ...)", `skeleton_regulator`), but does NOT feed it to `AnalysisLane::OnFrameTiming`: that span contains the render lock wait and the
  pacing sleeps, so a regulator acting on it would shed because of waits (test: a span with a wait of 5 periods inside sheds nothing). The regulator
  acts only on `SkeletonModule::NoteRenderOnlyMs(host, seq, ms)`, a render-only time the HOST measured (the PassTimers render span; the host call site is
  not in this package). Plausibility: the observed span is the idle-inclusive frame time (a frame that renders in 10 ms in a 40 ms cadence shows about
  30 ms of slack by the observed form too, but so does one that waits 30 ms); with measured spans the figure is the real headroom. Until one arrives
  the line says "(none: shedding off)" and the Fallbacks entry stays.
* **Audit fixes (stage A-C).** The lane-lead window is timestamped: every `CollectAhead` call is one sample, the window is the last 0.5 s of REAL time
  whatever the call rate (call contract: any thread, any rate; a host that stops calling leaves the last value, which then ages out through the starve peak).
  The starve peak is the best lead of the last 10 s (it decays to the lead in use; not a permanent high-water mark). The render-ahead term is unwired
  (never left wired with an old value) when the host passes no value (`QueueDepth` with a negative value: no source period) and when no `QueueDepth` fact came
  for 8 source periods (the plan stopped). `Declare` runs only when a module's held size changed. The producers' readback staging copies are system
  RAM and are not in the VRAM ledger; the ledger prints MiB. A stepped user that the plan sheds but that still holds its texture is planned as transient
  until its module frees it (`VramPendingFreeOf`). `skeleton_vram_known` names the users that hold video memory and declare none yet. Ctrl+Alt+D shows
  how many cost rows were posted and dropped ("Skeleton cost rows").
* **Capped record history (visible):** the depths are bounded so every ring fits 255 slots (223 frames). When the rate asks for more,
  `skeleton_history: quality pattern history capped at 223 frames (source rate above 111.5 fps)` appears in Fallbacks; when the queue asks for
  more, `... (the configured queue asks for N frames)`. A graph whose slot counts still overflow is rejected (analysis lane unavailable, with
  the reason), never clamped.

## VRAM users and motion costs (MV7)

* **Declared VRAM users.** At every graph configure `SkeletonModule` reads `VpSkeleton::VramUsers::Instance().Take()` (core/VramUsers.h): the users outside the
  product catalog become `TierInput::otherUsers` and their held bytes `ownUsageBytes` (inside `CurrentUsage`, so VP's own: re-planned, never counted twice). The
  graph is re-planned when the declarations changed (`VramUsers::Version()`), at most once per `kReconfigureMinFrames` frames. Ctrl+Alt+D lists each declared user
  ("VRAM T0 optical-flow engines: N MiB level L"); a user below level 0 has its governor notice in the "Analysis:" status line. The `VramLedgerLines` rows (core/
  GpuAllocator.h) list every governor grant, so a declared user has a row there too. BasicChroma's kept output texture (`BasicChroma::Gpu::DeclaredUser`, luma-size
  rg16, 4 B/px = 31.6 MiB (33.2 MB) at 3840x2160: the real allocation, not the 66 MB RGBA16F estimate of the tracking note) is a stepped user (T1, lossy, shed first) declared
  before any call site exists; the owner-waived `PrivateTextureScan` entry is untouched.
* **Motion GPU job timings.** The NVOF producer times 1 pair in N (the GPU idle first, then the engine's completion, on the worker's own device) and posts it to
  `PassTimers::Inbox::Motion()` ("motion: NVOF producer engine (idle to completion)"). The skeleton prints the cumulative figures itself ("Skeleton motion GPU
  job ..."); the render thread's pass-timer readout gets one generic host call at the existing timer site (`PassTimers::Inbox::Motion().DrainInto(passTimerHook.acc); // VPV-HOOK`
  right after `entranceSpans.Poll(...)` in `LibplaceboVideoRenderer.cpp`; Z3, applied with the SK-W train; the include is in `PassTimerHook.h`, a VP file).
  DejudderWarp's own `SetPassSampling` / `TakePassGpuMs` have no caller yet: the same host site.

## Demand and products from outside the lane (SK-W stage D: what the camera link ports to)

| API (all `Vpv::Skeleton::SkeletonModule` unless noted) | Meaning |
|---|---|
| `SetExternalDemand(host, owner, consumers, publishedOutside)` / `ClearExternalDemand(host, owner)` | the camera extension declares its consumers (`CameraLink::DeclareCameraConsumers`: P7 / P8 / P9 / p7g with P2 / P3 / P4 as inputs) and the products it publishes itself. The next Begin reconfigures the lane graph (`externalVersion_`), which then holds them. Consumer ids must be unique (first wins; duplicates are counted and shown) |
| `Query(kExternalDemandQueryId)` -> `IExternalDemand*` (`SkeletonFacts.h`, id `"vpv.skeleton.external_demand.v1"`) | the two calls above for a component that does not include this module: the camera extension will find them through the hub at SK-W2 (declaring `CameraLink::LiveCameraDemand`, P7 (+ P8) published outside the lane). Not called yet: until the lagged seal exists the declared P7 / P8 would be sealed absent every frame (camera link README "No skeleton demand yet"). Test: `TheExternalDemandQueryGivesTheModulesOwnSetAndClear`. Every entry of a host is erased at its GpuReleased / Shutdown and when a new renderer is found at a dead one's address (`ExternalEntriesOf`; test `AReleasedHostLosesItsExternalDemandSoARecreatedOneStartsClean`) |
| `LaneOf(host)->Lease(kP7CameraFit)` (`core/LaneLease.h`; `LaneOf(host)->State()->table` is the same table) | where the camera fit worker publishes P7 / P8 / p7g (`Claim` -> `LaneAncestors` -> `Publish`, as `CameraLane` did in its own table). The lane REPLACES its state at every rebuild (external demand, VRAM re-plan, queue, period): an outside worker holds a LEASE (shared hold of the state + the product's bytes ring + the Indicator + the state generation), never a raw `SlotTable*`; see the camera link README "Holding the lane" (`CameraLinkCore::AttachLane`, `CameraFitWorker::Rebind`) |
| `AnalysisLane::SetExternalProducts` (set by the module from `publishedOutside` and, with any external consumer, P2 / P2m / P3) | the seal's live fill DECLINES these products (counted, "Analysis: products published outside the lane: ..."): the lane never makes them, a frame the outside producer did not publish in time is Unavailable and its readers fall back |
| `IVectorEngine::SetCameraFitWanted(bool)` (engine interface) and `VectorDemand::Consumer::CameraFit` | an external consumer that reads P7 / P8 / P9 / p7g starts the engine: the module calls it (render thread, on change); the engine merges the want into its next `VectorDemand::Decide` (source-field consumer: producer + render-time estimate). Withdrawn when the demand is cleared |
| the ENGINE FEED: `FeedCountersOf`, `EngineFieldFactsOf(host, seq, EngineFieldFacts&)` | at AfterRender (retried at Begin) the module takes `LatestVerifiedFieldSeq()`; unless `PairSpansSlip(from, to)` it publishes the P2m / P3 slots (P2 only with an engine session and a valid `EngineGlobal`) of frame `seq` = pairTo under `hdr.honoured = VerifiedField::stamp` (the engine's `MotionValidity::StampAt`); `FieldEra()` changes forget what was pending. The frame's own seal at its Begin accepts or rejects them. Facts: pair, grid, block size, scale, stamp, `EngineGlobal` (`SkeletonFacts.h`). The inputs of a slot (P1, P2m) are pinned BEFORE the slot is claimed: a claim abandoned for a missing input leaves the slot Abandoned for that frame and the retry then found it Stale (the retry could never succeed). A slot that cannot be claimed because the frame left the ring counts in `claimFailed` and gets the explanation line "Skeleton engine feed: N pair(s) arrived after the seal of their frame (backward / low-latency mode ...; expected in this mode)" (lane lead 0) or "... not published because their frame had already left the ring ..." (`ClaimFailedNote`). THREADING: `FeedCountersOf` / `EngineFieldFactsOf` / `SceneFactsOf` are RENDER THREAD ONLY (plain fields written by Begin / AfterRender); other threads read the status text |
| `SceneFactsOf(host, seq, SceneFacts&)` | cut / cut-possible / fade / flash and the raw record values derived from P4(seq) and P4(seq-1) by `VpUnit::CutFadeJudge::ClassifyEvents` (the classification only; scene events are scene facts. The SCENE record below stores the cut / fade verdict), and Bill's picture rectangle. It reads NO validity source: no `VpIndicator::TemporalValid`, no Indicator (`SceneFacts::fullBetween` / `continuityBetween` are reserved and always false); whether a vector pair is usable across a resync comes ONLY from the P5a `MotionValidity` facade (`tools/test_check_shared_units.py` `test_scene_facts_reads_no_validity_source`). Fade gain / offset: run `CutFadeJudge::FadeFit` on the given means and spreads with the reader's own scaling |
| the SCENE record (W1-C7): `SceneOf(host, seq)` -> `SceneAnswer` (`known`, `cut`, `fade`, `from` = Store / Live), `SceneCountersOf` | a skeleton product, sealed per frame: at the frame's Begin (after the seal) the module classifies once (`FillScene` = `SceneFactsOf`) and writes the frame-analysis store's scene group (`FrameAnalysis::Store::PublishScene`, origin Vp) when the store is ENABLED and holds the frame's record. Skipped when the group is already there (compute-once; a value another producer wrote first, e.g. a labelled Bill copy, is kept). With `frame_analysis_store=false` the Host still registers its store but DISABLED: the seal tests `Enabled()` first and does nothing (counters all zero). An enabled store with no record for the frame counts `notReady` once and classifies nothing at the seal. `SceneOf` is the one reader: the store's scene group first (`PeekScene`), else the same classification live, kept for that frame only (a second call for the same frame is a `cacheHits`, not a second classification) and written to the store when it is enabled and holds the record: the same cut / fade with the store off. The Peek-then-Publish pair is not atomic; the skeleton is the only scene producer today, so a race could at worst write the same value twice. The camera extension's `ReadScene` (Read(kIdentity), then `valid & kScene`) can see this group; switching it to `SceneOf` is a lane C3 follow-up (CameraExtension.cpp is not edited here), so its `camera_scene` count falls only for pairs described after their frame's seal. The CADENCE group is NOT published: no host sets `FrameContext.cadenceRepeat` and the skeleton is given no film rate; it is deferred to wave-5 item C7b (a feed from queue_timing, lane A4's integrator; `IngressPolicy.inl` stays with that lane) |
| `PassTimers::Inbox::Motion().PostCameraFit(ms)` / `PostCameraReadback(ms)` (`core/PassTimerInbox.h`) | the `camera.fit` (CPU work) and `camera.readback` (CPU wait) rows; the skeleton prints them ("Skeleton cost row ..."), the host drain puts them in the pass-timer readout |

**What the module does NOT do (named gaps).** (1) The CONTENT of P3 / P2m is the engine's texture (rgba32f, 16 x 16 source-pixel blocks); the P3 declaration is one texel per 8 x 8 block and a GPU product has no CPU buffer, so the slot is a published, sealed, stamp-checked marker plus the facts above, not a copy of the field: copying needs a resample kernel on the GPU-work hook. A reader that needs the pixels asks the engine (`FieldOf`) on the render thread, as the camera's readback ring does. (2) The seal of frame N is at the Begin of N (the hub's "BeforeRender": `NotifyFrame(Begin)`, after ResyncExtension's FrameSeal, already dispatched at LVR; the release is `AfterRender` -> `OnFrameRetired`, also dispatched), so a product that needs frames AFTER N to exist (the camera fit, which finishes a few frames after the engine made the field) cannot be sealed for N: its seal would have to lag by k frames with the frame's own stamp and its ancestors kept k frames longer. That is a core change (a per-node seal lag in `SlotTable::Resolve` / `AnalysisLane`, ring headroom of renderAhead + lag), not a host line; it is NOT in this stage and needs a decision (the camera keeps `CameraLane`'s lagged seal until then). (3) The `skeleton.report` consumer is unchanged; with external demand the governor also plans P2m / P3 / P7 / P8 rings, which cost memory the lane never fills for P2m / P3 (a few MB at 4K).

## Products

| Id | Unit version | Fill rule | Consumer fallback when absent |
|---|---|---|---|
| P1 half luma, P4 source stats | catalog | CPU producers on the lane's CPU worker; a late or missing result is filled live in the render, bit-identical (`ComputeHalfLuma` / `ComputeSourceStats`) | consumers compute their own |
| P10 grain split (GPU) | 2 (`kExpectedGrainSplitUnit`); tap form `kTapFormUnitVersion` = 1 | lane: tap bands, extend, five splitter passes (one chunk per pass); live: the same steps at once (Entrance, deferred); storage Full or Compact (`grain_split_storage`) | readers run their own split; `CheckFit` returns WrongSize / Stale and the reader falls back visibly |
| P11 region block stats (GPU) | 2 (`kExpectedRegionUnit`); tap form 1 | one pass on the extended tap plane; band energies from P10 Compact; without P10 / Compact: "P11 without bands" (re_have_split 0); display facts = shown defaults | regional repairs use their own local statistics (`product.region_evidence`) |
| P12 light level (GPU) | 3 (`kExpectedLightLevelUnit`) | one pass; reads P11 S0/S3/S4 and its own previous frame; no P11: frame level from P4's APL ("P12 frame-level"); CPU LUT built at Prepare / settings generation | light-level users use the global level (`product.light_level`) |
| P10c grain split chroma | n/a | **declined**: "P10c declined: chroma needs the source ring (M2 P10)" | chroma upscale runs a lower tier without the grain guard (`product.chroma_cfl`) |

**Tap form (70b section 3):** until the source ring (P13, M2 P10) exists, P10 and P11 read the *tap plane*: the lane's analysis luma (10-bit studio codes << 6,
R16 unorm, uploaded in row bands). Unit version `VpLaneProducts::kTapFormUnitVersion` = 1, provenance `VpLaneProducts::kTapProvenance`. At the migration to
P13 both change, and every consumer's `CheckFit` falls back visibly (WrongSize / Stale) instead of reading a plane of another provenance.
Nothing reads P10-P12 yet (the `skeleton.report` consumer only declares them).

## Status and fallbacks

Every downgrade is visible (Ctrl+I line, Ctrl+Alt+D, Config status); none is log-only. Strings (FallbackCatalog ids):

| Id | Text | When |
|---|---|---|
| `skeleton_lane` | `waiting: ...` / `no source frames: needs frame_analysis_store=on` | the lane cannot run yet |
| `skeleton_settings` | invalid key list | a `[vpv.skeleton]` value was not valid (default kept) |
| `skeleton_queue` | `record history: queue not yet wired (...); floors in use` | the render-ahead depth and/or lane lead have no feed yet (record histories at their floors) |
| `skeleton_queue_starve` | `queue starve: lane lead N (was M), K fall(s); look-ahead products reduced: ...` | the live lane lead is below the best it had since the lane was built (releases when the queue recovers) |
| `skeleton_lane_lead` | `lane lead capped at N frame(s) (queue holds M; ring slot budget)` (and the render-ahead form) | the fed lane lead / render-ahead depth exceeds the ring-slot budget shared by R + L |
| `skeleton_regulator` | `regulator input: observe only, slack derived from the Begin..AfterRender span (includes waits); render-only time not measured, shedding off` | no host-measured render-only span has been given (`NoteRenderOnlyMs`); the regulator is not fed |
| `skeleton_vram_known` | `VRAM not yet declared to the governor (known gaps): temporal detail, ...` | standing while the lane runs: users that hold video memory and declare none |
| `skeleton_vram_users` | `VRAM not declared to the governor: <modules> (its memory is not in the budget)` | a module that holds video memory called `VramUsers::MarkActive` but declared none of it (MV7 guard) |
| `skeleton_engine_feed` | `no vector engine for this renderer: the pair and field slots (P2m, P3) are not published; their readers fall back` | external consumers are declared (P2m / P3 are in the graph) but the renderer has no vector engine |
| `skeleton_history` | `quality pattern history capped at N frames (...)` | the rate or the queue asks for deeper record histories than the 255-slot rings allow |
| `skeleton_gpu_shadow` | `shadow: GPU products skipped (no GPU dispatcher, no GPU work)` | `analysis_lane = shadow` |
| `skeleton_gpu_device` | `GPU products: no render device (...)` | `analysis_lane = on` without a usable `pl_gpu` |
| `skeleton_gpu_live` | `live: waits for the Entrance phase (M2 P4)` | a dispatcher exists (live fills deferred) |
| `skeleton_gpu_gap` | `GPU lane work runs ahead of the present` | `skeleton_gpu_gap = after_render` |
| `skeleton_gpu_gap_off` | `GPU lane work off (skeleton_gpu_gap = off): the GPU products are not made ahead` | dispatcher exists, gap off |
| `skeleton_gpu_shed` | `VRAM shed: <products> lost the lane ring (not enough free video memory); made in the render only` | the tier plan refused a GPU product its ring (`GrantWhy::OverBudget`) |
| `skeleton_gpu_memory` | `GPU products not made: private working memory N MiB exceeds the free video memory M MiB (WxH)` | the dispatcher's private working memory alone (below) is larger than the free video memory at a graph build |
| `skeleton_gpu_failed` | `dispatch failed N time(s): <LastError>` | the dispatcher's failure counter is non-zero |
| `skeleton_gpu_chroma` | `P10c declined: chroma needs the source ring (M2 P10)` | a dispatcher exists |
| `skeleton_gpu_light_pending` | `light level: pending (G6 not wired): the previous P12 frame was read as not made N time(s); the smoothing restarted` | P12's begin gate read a previous frame whose live fill is still pending for the Entrance phase as not made (`LightPrevStateOf`, counted); engaged from the first such read until G6 drives `RunDeferredLive` |
| `skeleton_gpu` (dispatcher) | `<refusal>: the product is unavailable (readers fall back)` | a product's textures could not be allocated |
| kernel entries | grain split storage (Compact -> Full / Full -> Compact), `region_stats_bands` ("P11 without bands"), `region_stats_display` / `light_level_display` ("display facts: defaults"), `light_level_frame` ("P12 frame-level"), `light_level_smoothing` | reported by `SkeletonGpuDispatcher::Fallbacks()` (includes every kernel's list) |

**VRAM shed order (VramGovernor, one budget for all of VP's video memory):** over budget, users step down one level at a time. Rank: tier T2 first, then
T1, then T0; within a tier lossless steps before lossy ones; then the shed order; then the id. Skeleton products are lossless users (level 0 = ring + live
pool, level 1 = live pool only, "computed in the render"): within T0 the readers shed first (reverse topological), within T1/T2 the lowest ms saved per MB
first. So the GPU products shed P12 (T1) first, then P11, then P10 (T0, readers before the producer): each loses its lane ring and is made in the render only.
A tier plan that cannot fit even at every minimum says so (`Video memory: over budget even at minimum settings`).

**Private working memory (not in the declarations, so the governor does not plan it).** The dispatcher holds, outside the product textures: the tap ring
(3 x [tap + extended] R16 planes = 12 B/px) and the grain split's working textures, one set per Splitter (the lane form and the live form each own
one, because intermediates survive across gaps and a live fill must not clobber a lane job): Compact = rows + bands (32 B/px), Full = bands (16 B/px), plus the
block grid. `SkeletonGpuDispatcher::PrivateBytes` computes it analytically; each graph build adds it to the used video memory the governor sees, so the shed
order above acts on what is left. Figures at 3840x2160 (8.29 Mpx, B = 32), lane + live, tap ring included, product ring 2 + live 1:
Compact = 199 MB products + 532 MB working + 100 MB tap = about 0.83 GB; Full = 398 + 266 + 100 = about 0.76 GB (so, with two Splitters, Compact no
longer saves memory; its advantage returns only if the lane and live forms could share one working set). After the governor shed P10's lane ring
(live-only) the lane Splitter is never run, so only the live set stays: Compact about 0.37 GB, Full about 0.30 GB. If the private memory alone exceeds the
free video memory the GPU products are not wired at all (`skeleton_gpu_memory`; the lane's CPU products continue).

**Small frames (known limit).** The block-grid terms of P10 / P11 / P12 are declared at the 16 px upper bound (shift 4), but `RegionEvidence::BlockSizeFor`
is 8 px for frame heights of about 763 lines and below, so the kernels' layout (bw x bh at B = 8) is larger than the declaration and `ProductTextures` refuses it
(`layout does not match the declaration`, shown by the dispatcher's own entry). The lane jobs then decline (`Available(id)` gate) instead of failing every chunk,
and the readers keep their own computation. Lifting it needs a shift-3 declaration (a declaration and unit-version change owned by the product catalog).

## Tests

CPU only (no GPU): classes `SkeletonSettingsTests` (incl. `skeleton_gpu_gap` / `grain_split_storage` parsing and invalid-value problems),
`SkeletonModuleTests` (Off and Original construct no dispatcher and make no GPU call; shadow and "no render device" statuses), `SkeletonGpuStatusTexts`,
`SkeletonLaneTests`, `AnalysisLaneGpuHookTests` (lane deferral, `ReadOf`, `StepsFor`). Live queue feed and regulator input (`SkeletonModuleTests`):
`TheQueueFeedsEachMergeOnlyTheirOwnTerm` (three threads CAS-merging their terms), `TheLaneLeadIsTheQueueTheHostCollectedAheadAndIsShown` (0 and 255 cases),
`AQueueStarveIsShownAndRecovers`, `TheQueuePhaseIsInertWhenTheLaneIsOffOrOriginal`, `TheRegulatorGetsTheDerivedSlackEveryFrameAndSaysSo`,
`TheThreeFeedsTogetherWireTheWholeQueue`, `DeclaredVramUsersAreInThePlanTheLedgerAndTheGuard`, stage D: `ExternalCameraConsumersJoinTheLaneGraphAndStartTheEngine`,
`WithoutAnEngineTheFeedSaysSo`, `TheEngineFeedPublishesTheFieldSlotsWithTheFieldsOwnStamp`, `ASceneIsDerivedFromTwoP4Records`, `ASceneIsPublishedOnceAtTheSealAndServedFromTheStore`, `ASceneAlreadyInTheStoreIsNotComputedAgain`, `ASceneOfGivesTheSameAnswerWithTheStoreOffAndFillsALateRecord`, `ARealCutGivesTheSameAnswerFromTheStoreTheLiveFillAndWithTheStoreOff`, `ARegisteredButDisabledStoreCostsNothingAtTheSeal`, `ASealWithNoRecordToWriteIntoCountsItAndClassifiesNothing`.
GPU (libplacebo WARP unless `VP_TEST_GPU=hardware`): `SkeletonGpuDispatcherWarpTests` (1024 codes exact, extend == CPU, alloc == declaration, lane == live),
`LaneGpuGrainKernelWarpTests`, `LaneGpuRegionKernelWarpTests`, `LaneGpuLightKernelWarpTests`.

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Logger:trx /TestCaseFilter:"ClassName~SkeletonModuleTestsNs"
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Logger:trx /TestCaseFilter:"Name~Skeleton|Name~LaneGpu|Name~AnalysisLaneGpuHook"
```

Read the TRX summary line (vstest prints none for this DLL). A WARP test that needs a `pl_gpu` through `SkeletonModule` itself (dispatcher built from `GpuCreated`
with a real device, status lines of the engaged states) is not written yet: the fixture does not give a module a `pl_gpu`.

## Why "skeleton first"

Every analysis product has one owner and one sealed form; this module gives the lane a place to run under the real hub, with the real seal and real source
frames, before any consumer depends on it. Consumers move onto the products one package at a time, each behind its own switch.

## Threading

Lifecycle and frame phases: render thread. The tap sink: the frame-analysis worker. Queue drops: the queue thread (a mutex-guarded list the render thread
drains). Status: a cache rebuilt on the render thread. Per-host state is found through `core/hub/HostSlots.h` (lock-free lookup); the seal hooks hold weak
references, so a released renderer's state can never be touched. All dispatcher calls are render thread.

## Stale state and drops (audit notes)

* A renderer whose device creation throws never gets `GpuReleased`; a new renderer may then reuse its address. The tap keeps a generation per owner (bumped when
  the Host's store registration changes); module state records the generation it was built for and is rebuilt (lifecycle or the next frame) when it differs.
  The tap sink is registered again whenever it is missing.
* The seal hooks of a rebuilt state are appended to the seal again (a `FrameSeal` cannot drop them). The old hooks hold weak references to the discarded state
  and do nothing; the cost is a few idle closures per address reuse.
* Backlog-flush drops (`OnQueue(Drop)`): the call site passes the surviving sequence and the number of frames dropped before it, so the dropped sequences are
  taken as `[seq - n, seq)` (consecutive). That assumption is counted and shown (`dropped frame(s) were taken as consecutive sequences`); at most 16 per event
  are applied, the rest are counted as lost.
* A dispatcher is destroyed together with the lane bundle that points at it (the bundle is stopped first); a changed `pl_gpu`, `grain_split_storage` or
  `skeleton_gpu_gap` rebuilds the bundle at the next Begin.

**Downstream consumers (TEMPORAL DETAIL, docs 71 section Q5 and 72):** per-pair homography (P7/P9), the object layer (per-object motion), P8 per-block membership + confidence, and K4 guided residuals.

## GPU memory: scratch arena and product textures (folded in from `control/gpu_memory`, R2 M-e)

Former slot `control.gpu_memory` (order 935, draft manifest, not a pipeline slot; SA1: core + backend + TextureRestore adoption). Its two backend headers (`PlTexBackend.h`, `GpuProductTexBackend.h`) now live in this folder; the tests stay in `src/VP-V/tests/`.

### Purpose / why

One place that owns VP's shared GPU temporaries (doc 51 section 3). Per-render temporaries are requested per scope; scopes run one after another, so the arena keeps per texture shape only as many textures as the busiest scope needs (not the sum). Product textures (ring + live pool of a skeleton product) come from a declared layout whose bytes equal the product's `SizeRule`.

### Contract

- Core (pure C++14, no GPU): `src/VP-V/core/GpuAllocator.h` (TexFormat, TexShape, `PlanArena`, `ScratchArenaUser`, `ArenaGrantFor`, `ProductTextures`, `VramLedgerLines`) and `src/VP-V/core/ScratchArena.h` (`ScratchArena<Backend>`).
- Product-store backend: `GpuProductTexBackend.h` (this folder: the `ProductTextures` backend of the lane GPU products P10/P11/P12; creates product textures, host readback only where the format supports it, records why a create failed). The skeleton dispatcher also owns the small constant textures of those kernels (stand-ins, the light level LUT) through `IStaticTextures`: kernels never create textures.
- Backend: `PlTexBackend.h` (this folder, header-only, libplacebo: `pl_find_fmt` with fixed caps sampleable|storable, `pl_tex_create`/`pl_tex_destroy`).
- Governor: user `kUserScratchArena`, levels [plan(T2), plan(T1), plan(T0), 0] (equal levels dropped), tier T1, not lossless. `Residency::Transient` products are not ring products (no lane grant, no governor user); the arena user counts them.
- Determinism: the plan does not depend on request order; the k-th Acquire of a shape in a scope gets pool slot k; Acquire never allocates.
- Single render thread (another thread's Acquire returns null and is counted).

### Fallbacks (owner rule)

Acquire beyond capacity, for a tier the plan shed, outside a scope or off-thread returns a null handle. The consumer runs reduced/off for that frame; `ScratchArena::FrameDecision()` is the FallbackCatalog entry `scratch_arena` ("scratch memory short: <owner> reduced/off"), released by itself on the first frame without a shortage (recovery once a larger plan is reserved).

### Call sites for the hook layer (not wired here; zero Bill-file lines)

1. Renderer ownership: one `ScratchArena<PlTexBackend>` created with the libplacebo gpu next to the module objects in `LibplaceboVideoRenderer` (reserve after each governor update: `ArenaGrantFor(requests, grant of kUserScratchArena)` then `PlanArena(requests, tier)` then `Reserve`). Requests come from `TextureRestore::ArenaRequests` (and later modules).
2. Per frame: `arena.BeginFrame()` at render start; after the frame `Sync(registry, arena.FrameDecision())` (FallbackCatalog) in the place where `GetFallbacksStatus` (`LibplaceboVideoRenderer.cpp`, near line 33017) collects the entries.
3. Ctrl+Alt+D ledger: `VramLedgerLines(tierPlan, arena.Plan(), registry)` appended to the diagnostic history written by `LibplaceboVideoRenderer::WriteDiagnosticHistory` (near line 37965). No generic provider registry exists yet; adding one is the M2 hook.
4. `TextureRestore::Params::arena` receives the arena pointer where the renderer builds the texture restore Params.

### How to build / test

Tests: `src/VP-V/tests/GpuAllocatorTests.cpp` (CPU: plan, order independence, determinism, exhaustion + recovery, tier shed, Transient residency, ProductTextures, layout bytes, per-tier VRAM report at 1080p/4K/8K, ledger) and `src/VP-V/tests/PrivateTextureScanTests.cpp` (static scan: no private `pl_tex_create` in repair/scaling modules outside the justified allow-list). WARP: TextureRestoreGpuTests arena tests.

### Audit-1 changes (SA1/SA2)

- Governor: two arena users. `kUserScratchArena` (tier T0, levels [plan(T0), 0]) and `kUserScratchArenaExtra` (tier T1, levels [plan(T2) - plan(T0), plan(T1) - plan(T0), 0]): pressure on the optional temporaries never takes the T0 ones. `AddScratchArenaUsers`, `ArenaGrantFor(requests, t0Grant, extraGrant)`.
- Scopes: a nested `BeginScope` is refused (returns false, counted, shown by `FrameDecision`; the outer scope is untouched); a caller closes only a scope it opened.
- Usage is counted per (owner, shape), up to that owner's admitted count, from that owner's slice of the pool: one owner cannot take another's textures.
- `ProductTextures::Allocate(node, decl, layout, dims, plan)` refuses (visibly, FallbackCatalog "product textures") when the layout's bytes do not equal the declaration's SizeRule, unless the declaration is a planning-only size (`ProductDecl::planningSize`, the chroma product P10c): then the governor uses the ACTUAL bytes (`TierInput::actual`) and the layout takes the real plane dims (`TexTerm::absW/absH`), never an assumed subsampling.
- The arena path destroys the private temporaries it replaces (TextureRestore half/tmp and the foreign trio, the grain Splitter's rows/bands/quiet).
