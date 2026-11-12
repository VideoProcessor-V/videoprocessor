<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Vector engine: what moved out of the renderer, what is left, who reads what (M2 P5a)

Plan: doc 52 section 3 (inventory a-l, hooks, identity, commit sequence). Base cda5794e for P5a-0..4; P5a-5 (items a, e, f, K4, IVectorEngine) is on bff1c35e (line numbers below: at d8605823). P5a-rest (items h, j, k, l and the producer hardening) is on 852424a7 (m2/p5a: f0ac0f11 + the HubBootstrap include-order fix). This file is the P5a-1
inventory (member list, readers outside P5a's ranges = the alias list, the thread of every access) and the
alias table the package leaves behind.

## 1. Status by inventory item

| Item | What | State at this tip | Where it lives now |
|---|---|---|---|
| a | estimate state (B's planes, ahead payload/textures/schedule, upload layout, `dejudder`, estimated seq/epoch/periods, idle keeper + budget adapter, live-field bookkeeping, warm-up flags) | MOVED (P5a-5), members verbatim with their comments; aliases in Impl (section 4). Left in Impl: `dejudderReason` (policy, P5b), `dejudderAheadLogTick` (`LogDejudderAhead`, P5b), `motionEngineIdleReleasesLogged`, the upload-reuse records `dejudderNextUpload/AheadUpload` (+ `dejudderNextUploadedSequence`, `uploadsReused*`), `dejudderRgbLuma/RgbScratch`, `dejudderMask*`, `dejudderEstimates/Failures`, `motionSourceLogged` (shared with P5b/P10 readers) | `EstimateState.h` (`VpMotion::EstimateState<Payload>`, member `vectorEstimate`) |
| b | vector engine: demand decision, statistics service, global-pair hysteresis, delivered log, engine size | MOVED (`VectorDemandInput` stays in Impl: it only gathers the renderer's facts). T1 B3: `UpdateVectorEngine` syncs its fallback entries through `VectorEngineFallbackSync::Sync/Release` (four entries: the three old ones plus "vector producer", the look-ahead's restarts) | `VectorEngine.cpp` |
| c | producer / look-ahead / validity members, `MotionShadowCheck` | MOVED (members: `VectorEngineState`; shadow check: `VectorEngine`) | `VectorEngineState.h` |
| d | invalidation requests, apply point, counters | PARTLY: counters, epoch consume and the rate-limited log moved; `RequestMotionInvalidate` (also feeds `frameAnalysis`) and the consumer-forget body of `ApplyMotionInvalidate` stay in Impl | `VectorEngine::TakeInvalidation/LogInvalidation` |
| e | `WarmIdleMotionEngine`, `PollMotionEngineBudget`, `QueryMotionEngineBudget` | MOVED (P5a-5) verbatim: out-of-line `Impl` member definitions; Impl keeps one declaration line each (same names, same default arguments) | `EngineKeeper.inl` |
| f | `UploadDejudderPlanes`, `TakeNextPlanes`, `RunDejudderEstimate`, `EstimateAhead` | MOVED (P5a-5) verbatim, as e. Only edits in the bodies: the K4 wrap of the matcher's `Estimate` call (`VectorEngine::Estimate`), the `NoteEngineGlobal` CPU record next to `NoteGlobalPair`, and `RunDejudderEstimate`'s defaulted `ahead` argument (true only from `EstimateAhead`'s estimate step). Callers in the P5b zone (`UpdateDejudderEstimate`, `EntranceOnceFieldAhead`, the extras) are unchanged | `VectorEngineEstimate.inl` (includes `EngineKeeper.inl`; included ONCE by the renderer right after the Impl struct) |
| K4 | camera-guided matching insertion point | DONE (P5a-5): `IPreEstimateHook::BeforeEstimate` right before / `AfterEstimate` right after the matcher's `Estimate`, inside `RunDejudderEstimate` (so both the in-render estimate and the estimate ahead); never for the warm-up's dummy estimate. No hook = `estimate()` only | `VectorEngine::Estimate` (VectorEngine.h), call site `VectorEngineEstimate.inl` |
| IVectorEngine | `vpv.vector_engine.v1` | DONE (P5a-5): `VectorEngine` implements it from today's state (section 7); the renderer's engine registers under its host (`vectorEngine{ this }`); `VectorEngineExtension` (hub module, after ResyncExtension) answers the query with the `IVectorEngineDirectory` | `VectorEngine.h/.cpp`, `VectorEngineExtension.h/.cpp`, `core/hub/HubBootstrap.cpp` |
| g | producer lifecycle and depth decision | MOVED: pure `ProducerPlan::Lifecycle/Depth` + `VectorEngine::PlanProducer` (a plain-value call site in `RenderLocked`, not a hook phase: it needs the pass's own facts) | `ProducerPlan.h`, `VectorEngine.cpp` |
| h, i | look-ahead copy under the queue lock; engage rule, `Submit` loop and its catch | DONE. Item i (T1 port, `m2/p5a-t1`): the engage rule, the `Submit` loop and its catch moved verbatim into `MotionAheadWalk.h` (`MotionAhead::RunQueueWalk`; a throw engages `FallbackCatalog::MotionAheadWorker`, released after 8 good batches); the source-resolution look-ahead likewise (`SourceAheadWalk.h`). Item h (P5a-rest, lead decision after the audit): the queue-lock copy moved verbatim into `MotionAhead::CollectQueueAhead(impl, queue, out)` next to `RunQueueWalk`, a templated direct call from the render loop with m_queueMutex held as before (not the R5 `CollectAhead(QueueView&)` hook: its `QueueView` is opaque; a typed view waits for P10). Only edits: `m_impl->` -> `impl->`, `m_frameQueue` -> `queue`, `const QueuedFrame&` -> `const auto&`, the producer read through one `std::atomic_load` copy. Test: `LookAheadCopyTests`. The walks read the engine state through the Impl aliases (`view.impl.motionAheadEngaged` ...) | `MotionAheadWalk.h` |
| j, k | invalidate requests at Reset / ResetLiveQueue / gap / seek / format change as `OnQueue` events | DONE (P5a-rest): the five sites (Reset, ResetLiveQueue: UI thread; the dequeue's gap, file discontinuity, format change: render loop) are one-line `RendererExtension::NotifyQueue` call sites (QueueEvent Reset / LiveQueueReset / Gap with the gap's cause as text / Discontinuity / FormatChange); `VectorEngineExtension::OnQueue` maps each to the old cause (Resync / QueueReset / Gap / Seek / SourceFormatChange) and calls `VectorEngine::RequestInvalidationFor(host)`: under the engine directory's leaf lock the gap detail is stored (gap only, null too) and the engine's sink `InvalidateHost<Impl>` calls the Impl's own `RequestMotionInvalidate` (validity epoch + frame-analysis epoch). Synchronous dispatch: same causes, same order, same logs. `sourceMonitor.Discontinuity` stays in the host (P11). The drift drop and the smooth-motion mix still call `RequestMotionInvalidate` directly (not items j/k). `ResyncExtension` arms its generation on the Flush only (Reset / LiveQueueReset were never raised before) | `VectorEngineExtension.cpp`, `VectorEngine::RequestInvalidationFor`, `VectorEngineHost.h` |
| l | producer stop on GPU release (fix C = doc 30 F4; it was never committed: `fix-module-lifetime` 5f9c96b0 is a test-text commit) | DONE (P5a-rest): `VectorEngineExtension::OnLifecycle(GpuReleased)` (raised by `RetireOutput` before `pl_d3d11_destroy`) calls `VectorEngine::ReleaseProducerFor(host)`: the newest engine of that host unpublishes its producer (`atomic_exchange` under the directory lock), clears the start latches and back-off (W2-C1: `motionProducerStartFailed` / `motionProducerStopLogged` / `motionProducerStartRetry`; `motionEngineReady` removed, the engine state is `State()`); the producer is stopped outside the lock. Keyed by host (70b section 7: a replacement renderer's producer is untouched). Nothing logged | `VectorEngine.cpp`, `VectorEngineExtension.cpp` |
| reservations | `refined_field` (P3r / U15), `SubmitLongPair` with external hints | interface + contracts, default "absent" | `VectorEngineInterface.h` |

## 2. Cross-thread map (the P5a-1 thread column)

`Rt` = render thread (RenderLocked, `renderMutex` held). `Loop` = render loop thread (queue lock / no lock). `Any` = any
thread. `Ui` = UI / config thread.

| Member (State field) | Writers | Readers | Protection |
|---|---|---|---|
| `vectorDemand` | Rt (`UpdateVectorEngine`) | Rt; Loop (`motionAheadEngaged` log text: `lowLatencyAllowed` in `RenderLoop`'s engage log, 33393) | none: plain struct read by Loop (unchanged from before the move; benign torn read at worst of a log text) |
| `queueAheadCapacity` | Loop (queue pass) | Rt (`VectorDemandInput`), Loop | atomic |
| `motionAheadBackward`, `motionAheadDepth`, `motionAheadConfigured` | Rt (`PlanProducer`) | Loop, Rt | atomics (acquire/release) |
| `motionAheadEngaged` | Loop (engage/disengage), **Rt** (`PlanProducer`, two writes) | Loop, Rt | **atomic since this package** (was a plain bool written under two different locks: doc 52 D-3, no value change) |
| `motionAheadEngagedGeneration`, `motionAheadPrev*` | Loop | Loop | single thread |
| `motionProducer` (a `std::shared_ptr` since the P5a-rest hardening commit) | Rt (`PlanProducer`: start / stop through `PublishProducer` = `std::atomic_store`); retirement (GPU release, `VectorEngine::ReleaseProducerFor`, `atomic_exchange`) | Rt directly; Loop through one `std::atomic_load` copy per use (`CollectQueueAhead`, the walk's per-frame copy) | **Rt == Loop: the same OS thread** (audit correction: `RenderLocked` runs only inside `RenderLoop`, and `PlanProducer` only inside `RenderLocked`), so the earlier "race" between them could not occur. The only cross-thread writer is the retirement path: `Retire` joins `m_renderThread` (and the conversion thread) before `RetireOutput` raises GpuReleased, and `~Impl` is single-threaded; that join is the precondition for `ReleaseProducerFor`'s exchange. The shared_ptr publish / copy is kept as cheap **hardening** (not a race fix): it stays safe if a writer ever moves to another thread. A loop copy keeps a stopped producer alive until its use ends (`Submit` on a stopped producer returns at once). Stress test of the pattern: `ProducerRaceTests` (LookAheadWalkTests.cpp). The 541079b7/c31721fd "fix: motion producer race" commit message overstates this: read it as hardening |
| `motionEngineStatus` / `motionEngineWork` (W2-C1; replaces `motionEngineReady`) | Rt | Rt (`State()` / `Work()`) | render thread only; derived at the end of UpdateVectorEngine and PlanProducer |
| `motionValidity` | Any (`Invalidate`), Rt (`Consume`) | Any | atomics inside `MotionValidity::State` |
| `motionValiditySeen`, counters (`motionInvalidations`, log tick/cause, `motionRecoveryLogPending`) | Rt | Rt | single thread |
| `motionGapDetail` | Loop / Any (`RequestInvalidationFor`, the dequeue's gap event) | Rt (log) | atomic pointer to a static string |
| `motionHandoff*` | Loop | Rt (periodic log) | atomics |
| `motionSegment`, shadow counters | Rt | Rt | single thread |
| `motionAheadHits/Misses`, `vectorPairs`, vector counters | Rt | Rt | single thread |
| `motionAheadLogged/At` | Rt (`PlanProducer`) | Rt | single thread |

## 3. Call order (unchanged by the move)

0. Any thread: the queue events of items j/k (`OnQueue`, synchronous, in the order the host raises them) request the invalidation; consumed at step 3 as before.
1. Render loop, queue lock: the look-ahead copy (item h, `MotionAhead::CollectQueueAhead`).
2. Render loop, no lock: engage / disengage and `Submit` (item i, still in Impl); reads the atomics written below.
3. Render thread, in `RenderLocked`, per rendered source frame: `UpdateDejudderEstimate` -> `ApplyMotionInvalidate`
   (engine half `TakeInvalidation`, the renderer's consumers forget, engine half `LogInvalidation`) ->
   `VectorEngine::UpdateVectorEngine` -> `VectorEngine::ServeStatistics` -> the estimate (item f, `VectorEngineEstimate.inl`:
   `RunDejudderEstimate` -> [K4 `BeforeEstimate`] matcher `Estimate` [K4 `AfterEstimate`]) ...;
   later in the same pass `VectorEngine::PlanProducer` (producer lifecycle + depth for the NEXT loop pass).

## 4. Aliases left in `Impl` (one VPV-HOOK block; each deleted by the package that moves its last reader)

Declared right after the `vectorEngine` member, reference to the field of the same name in `VectorEngineState`.

| Alias | Readers outside P5a | Deleted by |
|---|---|---|
| `vectorDemand` | the whole pass: P4 (adaptive stage, entrance), P5b/c (extras, interpolator, dither), render loop log | P5b/P4 (last reader), P6 |
| `queueAheadCapacity` | render loop (queue pass, engage depth), `VectorDemandInput` | P10 / T1 |
| `motionAheadBackward`, `motionAheadDepth`, `motionAheadConfigured` | render loop (queue pass), `MotionAheadWalk.h` (reads them through `view.impl`) | P5a-6 (the walk can take `VectorEngineState&` instead of the Impl once the aliases go) |
| `motionAheadEngaged`, `motionAheadEngagedGeneration`, `motionAheadPrev*` | render loop (engage rule, break rule) | P5a-6 |
| `motionProducer` (now `std::shared_ptr`) | `MotionAheadWalk.h` (`std::atomic_load(&impl.motionProducer)`); `RunDejudderEstimate`, `EstimateAhead` (item f). `CollectQueueAhead` (item h) reads it through `impl->motionProducer` with `std::atomic_load` | P5a-6 / P5b (with the walk taking `VectorEngineState&`) |
| `motionValidity` | every vector consumer (P4, P5b, P5c, P10), `RequestMotionInvalidate`, dequeue | P5b/P6/P10 |
| `motionSegment` + `using MotionShadowField` | `ReleaseDejudder`, `ApplyMotionInvalidate`, `RunDejudderEstimate`, `WarmIdleMotionEngine` | P5a-5 |
| `motionAheadHits`, `motionAheadMisses`, `vectorPairs` | `RunDejudderEstimate` (item f) | P5a-5 |
| `motionHandoffMaxMs/TotalMs/Count` | render loop hand-off timing, `RunDejudderEstimate` stats log | P5a-5 / P5a-6 |
| `motionInvalidations`, `motionRecoveryLogPending` | status line, first-estimate log in `RunDejudderEstimate` | P5a-5 |

Deleted by P5a-rest: `motionGapDetail` (its last reader, the dequeue, now raises the Gap queue event; the engine stores the detail).

The rows above marked "P5a-5" stay after P5a-5: the moved bodies are verbatim and still read these names through the
aliases; they go when the bodies are rewritten against the engine state (P5a-rest), or with item i (P5a-6).

P5a-5 aliases (reference to the field of the same name in `EstimateState`, member `vectorEstimate`, declared right after
the block above; `using DejudderUploadLayout` for the moved type):

| Alias | Readers outside P5a | Deleted by |
|---|---|---|
| `dejudder` | the whole de-judder / interpolator / extras / entrance / dither code (`->Field()`, `->Snap*`, `ReleaseDejudder`) | P5b (last reader), P4 |
| `dejudderNextTextures`, `dejudderAheadTextures`, `dejudderAheadPayload`, `dejudderAheadPayloadSequence` | `UpdateDejudderEstimate` (B upload, C ahead payload), `ReleaseDejudder`, the frame upload (`textures` swap), the entrance's B ahead | P5b / P4 / P10 |
| `dejudderAhead`, `dejudderLayout` (+ `DejudderUploadLayout`) | `UpdateDejudderEstimate`, `LogDejudderAhead`, status line, `ReleaseDejudder`, extras | P5b |
| `dejudderEstimatedSequence`, `dejudderEstimatedEpoch`, `dejudderEstimatedPeriods`, `dejudderLiveFieldFrom` | `DejudderLiveFieldFor`, entrance / repeat reuse / dither gate / adaptive stage / extras | P5b / P4 / P6 |
| `motionEngineIdle`, `motionEngineBudgetAdapter`, `motionEngineWarmPending`, `motionEngineWarming`, `dejudderCreates`, `dejudderMotionSourceKey` | `UpdateDejudderEstimate` (keeper decision), `RetireOutput` (adapter release), resync / format change (warm pending), `ReleaseDejudder` | P5b / P11 |

## 5. Teardown

P5a-5: `vectorEstimate` is declared right after `vectorEngine`, i.e. later than the old positions of its members, so at
member destruction it goes earlier than the members declared in between; none of them holds the matcher (grep:
`MotionWarp*` / `MotionWarp&` only inside the matcher itself). The GPU release is unchanged: `ReleaseDejudder` (and
`RetireOutput`'s budget-adapter release) run before the device goes, through the aliases. `~EstimateState` unbinds its
matcher view from the engine; `~VectorEngine` removes the engine from the directory (and logs once if a
pre-estimate hook is still registered: the consumer must clear it first).
Members that now OUTLIVE the matcher at member destruction (declared between the old `dejudder` position and
`vectorEstimate`): `frameInterpolator`, `dejudderCache`, `repeatReuse`, `dejudderRenderedWarp`, `dejudderVectorField`
(declared before it, so destroyed after it). None keeps a pointer to the matcher, and `ReleaseDejudder` frees all of them (and the
matcher) first, before the device goes, so member destruction normally finds them empty.

`~Impl` still destroys the producer (its own optical-flow device). GPU objects of the matcher are released by
`ReleaseDejudder` (item a, unchanged). The state struct is declared at the position of the earliest moved member (the
vector-demand block), earlier than the old `motionProducer` / `motionValidity` positions; destruction order against the
members declared in between changed, none of which the producer or the validity state uses (the audit greps this).
P5a-rest (item l, fix C): the producer is stopped at the GPU release (`OnLifecycle(GpuReleased)`, before the render device goes); `~Impl` then finds it empty. A render-loop copy still in flight keeps the stopped object until its use ends (`Submit` returns at once on a stopped producer).

## 6. Identity proof status

| Proof | State |
|---|---|
| P5a-5 move | bodies verbatim against d378d4f0 except the listed edits (move script `D:/codex/videoprocessor/m2-p5a-move-ef.py`; diff of the dedented old ranges against the .inl files shows only the `Impl::` qualifications and the dropped default arguments before the K4 edits) |
| `VectorEngineHookTests` (null hook and all-off hook leave the matcher's call sequence unchanged; warm-up never hooked; bound guidance reaches the matcher before its estimate; IVectorEngine absent values, live field, epoch, engine global, directory) | written, syntax-checked (`cl /Zs`); project run after the gate |
| `ProducerPlanTests` (3.2 M rows against a test-only copy of the old logic; a mutation fails it) | written; run in the scratch harness, passes; project run after the gate |
| `VectorEngineTests` (fake host: sync/publish rules, hysteresis, size, shadow, reservations) | written and syntax-checked; project run after the gate |
| existing: VectorDemand, MotionValidity, MotionAhead, DejudderPolicy, `EstimatorHookTests`, QueueStressHarness | to run on the fresh full-solution build |
| chain-gate motion subset, frame hashes vs G1; skeleton timing gate; "Motion ahead:" log diff | coordinator, after the gate |

## 7. IVectorEngine (vpv.vector_engine.v1) from today's state (P5a-5)

`Query("vpv.vector_engine.v1")` (hub, `VectorEngineExtension`) returns an `IVectorEngineDirectory*`; `EngineFor(host)` gives
that renderer's `VectorEngine` (host = the RendererExtension `Context::host`, the renderer's Impl). Render thread unless noted.

| Method | Source state | Absent value |
|---|---|---|
| `LatestVerifiedFieldSeq` | the last successful estimate's pair `to`, while `FieldOf(to)` holds | 0 |
| `FieldEra` | `motionValidity.Epoch()` (any thread) | - |
| `FieldOf(seq)` | the matcher's live block field (`MotionWarp::Field()`, rgba32f, vx/vy source px, trust, error; 16x16 source px blocks) of the last successful estimate (from, to = seq) while `dejudderLiveFieldFrom == from` and the epoch it was made in is current; recorded only when the epoch the caller chose the pair under (`motionShadowEpoch` at the top of `RunDejudderEstimate`) still holds after the estimate; `bwd`, `trust` null; `scaleToSourcePx` 1; `stamp` = the bound Indicator's `StampOf(pairTo)` (`MotionValidity::StampAt`; unbound / resync off: the default `Stamp`) | false |
| `FieldScale` | 1 (the field is in source px) | - |
| `SeedFieldOf(pairTo)` (F2; the camera fit's ONLY input) | the optical-flow engine's seed of the same pair: the SEARCHING matcher's `ofSearch[0]` / `[1]` (`MotionWarp::LastSeed`; the lowRes inner when the matcher halves, recursively), under exactly `FieldOf`'s rules (pair `to` = the last successful estimate, `dejudderLiveFieldFrom == from`, epoch current, valid until the next estimate, same `StampAt` stamp) and only when that Estimate's first sweep WAS the seed (`seedFed`: engine on, candidate + seed mode, not a skipped pair). Layout checked, never assumed: a texel per 8x8 block of the searching half size = 16 x 2^halvings source px (1080p 16, lowRes 4K 32, 8K 64), rgba = (vx, vy) seed px, SAD (8-bit codes / px, 1e4 = beyond reach), engine weight; `scaleToSourcePx` 2 x 2^halvings; the seed must cover the live 16-px grid exactly (ceil(live / 2^halvings)), which `sourceGridW/H` carry. The camera readback copies both seed planes in its existing two-plane copy and converts on the CPU (`SeedToSourceGrid`). The guidance never writes `ofSearch` (its E1 pass only samples it). Trust limits are the live consistency pass's: `sadTrustMax` = taper_sad with block_taper on, else match_tolerance; `weightPenalty` = 2 x that with the engine's trust weights applied, else 0. `seedFed` / `seedSkipped` are cleared at the top of EVERY Estimate for the matcher and its lowRes inners | false, `absence` = `Skipped` for a pair whose searches the guidance skipped (a cut), else `NotSeeded` (engine off / failed, matcher-only, candidate or replace mode, layout mismatch) |
| `GlobalFromEngine` | `dejudder->GlobalFromEngine()` | false (no matcher) |
| `EngineGlobal(seq)` | the producer pair's `flow->global[0]` taken in `RunDejudderEstimate` (pair to = seq, current epoch, z < 0.9), x/y = cell px (`EngineCellSourcePixels`) x the D1 texels; a ring of the last 16 pairs keyed by `to` (a pair without a producer flow is recorded as absent) | false |
| `PairSpansSlip(from, to)` | false only when every adjacent pair in [from, to] is `PairValidAt` in the current epoch, from >= the first estimate of that epoch, span <= 64 | true (unknown = slip) |
| `FlowIsCpuResident`, `FlowOf` | none kept today | false |
| `SetPreEstimateHook` | atomic pointer read once per estimate. Contract: set AND clear on the render thread or under renderMutex; the hook object may be destroyed only after the clear has returned (`~VectorEngine` logs a hook left registered) | null = none |
| reservations (`RefinedFieldOf`, `SubmitLongPair`, ...) | interface defaults | absent |

Hook contract notes for K4: `pairSeq` = the pair's later frame (`bSequence`); `ahead` is true for the estimate ahead,
which today also runs on the render thread (inside `RenderLocked`, from the extras / entrance), never on a lane;
`nextPairSeconds` = (b - a) / the content rate (`dejudderSourceHz`), 0 when unknown. `AfterEstimate(pairSeq, const
EstimateOutcome&)` gets plain data (`VectorEngineInterface.h`, valid for the call only; one `CameraGuidanceUsed()` read,
`CameraGuidanceCounts()` only when used with a narrow or E1 pass, exactly as before; no new GPU work):

| `EstimateOutcome` field | Source | Absent value |
|---|---|---|
| `guidanceUsed` | `CameraGuidanceUsed()` reports a guided part (model global, static-from-model, matcher skipped, narrow or E1 pass) and no rejection | false |
| `narrowIdle` | `CameraGuidanceUsed().narrowDroppedEngine` (narrow planned, the engine seeded or replaced the searches) | false |
| `rejection` | `CameraGuidanceUsed().rejected` when non-empty (the matcher's literals, static lifetime) | nullptr |
| `gpuMs` | none today: the matcher's per-pass GPU timers (`TakePassGpuMs`) are polled a frame later and taken by the renderer after the estimate returns, so no per-estimate figure exists at the hook; no new timer query is added | -1.0 (always, today) |
| `countsTex` | `CameraGuidanceCounts()` only when `guidanceUsed` and a narrow or E1 pass ran (`pl_tex`, engine-owned, valid until the next estimate: copy, never keep) | nullptr |

The hook is called for both ahead == false and ahead == true, on the render thread with renderMutex
held; a future off-render lane estimate must not call `GuidanceScheduler::Guide` from it. A throwing hook is contained:
`BeforeEstimate` throwing clears the guidance and runs the plain estimate (no `AfterEstimate`); `AfterEstimate` throwing
is caught; an exception of the estimate itself is rethrown after `AfterEstimate(pairSeq, an all-absent EstimateOutcome)`. Each hook
failure is counted, logged (first 20, then every 100th) and raised as the fallback entry `vector_engine_hook`
("camera guidance skipped for some pairs", released after 8 clean hooked estimates, or at once when the hook is gone).

## 8a. W2-C1 (B9) call sites and hook lines

| Where | Line(s) | What |
|---|---|---|
| `consumers/DejudderPolicyRunner.inl` (VP-V module, existing VPV-HOOK block; not in the renderer's `VectorDemandInput`) | the `UpdateVectorEngine` call | `vectorFacts.opticalFlow`, `vectorInput.starterEngaged = dejudderStanding \|\| interpolating \|\| vpGapFillStandalone`, `NoteVramRelease(keeper pressured && !dejudder)`; `analysisEstimate` counted after `RunDejudderEstimate` |
| `consumers/DejudderPolicyRunner.h`, `tests/DejudderDecisionLegacyCopy.h` | the policy input | `occlusionMaskServed` from `vectorDemand.refused` |
| `estimation/EngineKeeper.inl` | the keeper | `NoteVramRelease(Pressured() && !engineHeld)` |
| `estimation/VectorEngineEstimate.inl` | matcher create site; after `vectorEngine.Estimate` | `++matcherCreates`, `NoteVramRelease(false)`, `DeclareStepped(kUserMotionMatcher)` + `MarkActive`; `++matcherEstimates` (not warm-ups) |
| `consumers/PhaseTimeline.inl` `ReleaseDejudder` (outside the C1 region, noted in Z3) | after `dejudder.reset()` | `VramUsers::Release(this, kUserMotionMatcher)` |
| `LibplaceboVideoRenderer.cpp` Reset / ResetLiveQueue (VPV-HOOK, lane A4 one-liner) | next to `m_pulldownResetRequested.store(true ...)` | `m_pulldownAcquiring.store(false, relaxed)`: no stale acquiring cadence keeps the de-judder standing across a reset |

## 8. Open items (audit of bff1c35e..d8605823, not fixed by decision)

- F7: only the LATEST field is served (`FieldOf` answers the last estimated pair only; the matcher keeps no ring). A
  consumer that misses a frame cannot ask for the field it skipped, and the engine does not count missed fields. A field
  ring (with a missed-field counter on the status) belongs to the P3 verified-field store, not to the live matcher.
- F8: the engine global's validity uses the statistics consumers' rule (`SampleGlobal(...).usable`: z < 0.9) and ignores
  the `w > 0.5` written flag that the intent interpreter checks; left as is (same rule as the fast-pan input).

## 9. Output-domain search + dither vector chain (W2-C8)

Call sites: 0 new lines in Bill-owned files (rule 32). `BuildSigmaDeltaVectorsWithoutExtras` keeps its declaration in `LibplaceboVideoRenderer.cpp`; its body moved to `estimation/DitherVectorSourceHost.inl`, included by `consumers/PhaseTimeline.inl` (VP-V module). The chain (shared pair -> matcher field -> on-demand -> own search -> static), its four fallback entries (`dither vectors matcher | on demand | own search | unvalidated`) and the status segment are described in README.md "Output-domain search + dither vector chain (W2-C8)".

- Q1: reuse gate `SpanStatus != Invalid`; `Unknown` reuses visibly (counter, segment, "dither vectors unvalidated").
- Step 5 = `motion_compensation 0` (shader default): status only, no vectors mapped. Engine absent / not serving = step 4, the module's own search (previous behaviour).
- DEVIATION (lead 4): the search is included INTO the one sigma-delta hook by `// VP_INCLUDE motion.output_domain_search` (`output/dither/VpShaderInclude.h`), not a second hook: libplacebo keeps SAVEd and `//!TEXTURE` storage per parsed shader (`custom_mpv.c` `struct hook_priv` :920-940; `pl_mpv_user_shader_set_texture` :1737 swaps only SAMPLED textures).
- The pass text is `fallback/OutputDomainSearchGlsl.h`; `tools/check_motion_bypass.py` no longer allow-lists the shader (the allowed `motion/` path holds the passes).
