<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Vector engine: what moved out of the renderer, what is left, who reads what (M2 P5a)

Plan: doc 52 section 3 (inventory a-l, hooks, identity, commit sequence). Base cda5794e for P5a-0..4; P5a-5 (items a, e, f, K4, IVectorEngine) is on bff1c35e (line numbers below: at d8605823). This file is the P5a-1
inventory (member list, readers outside P5a's ranges = the alias list, the thread of every access) and the
alias table the package leaves behind.

## 1. Status by inventory item

| Item | What | State at this tip | Where it lives now |
|---|---|---|---|
| a | estimate state (B's planes, ahead payload/textures/schedule, upload layout, `dejudder`, estimated seq/epoch/periods, idle keeper + budget adapter, live-field bookkeeping, warm-up flags) | MOVED (P5a-5), members verbatim with their comments; aliases in Impl (section 4). Left in Impl: `dejudderReason` (policy, P5b), `dejudderAheadLogTick` (`LogDejudderAhead`, P5b), `motionEngineIdleReleasesLogged`, the upload-reuse records `dejudderNextUpload/AheadUpload` (+ `dejudderNextUploadedSequence`, `uploadsReused*`), `dejudderRgbLuma/RgbScratch`, `dejudderMask*`, `dejudderEstimates/Failures`, `motionSourceLogged` (shared with P5b/P10 readers) | `EstimateState.h` (`VpMotion::EstimateState<Payload>`, member `vectorEstimate`) |
| b | vector engine: demand decision, statistics service, global-pair hysteresis, delivered log, engine size | MOVED (`VectorDemandInput` stays in Impl: it only gathers the renderer's facts) | `VectorEngine.cpp` |
| c | producer / look-ahead / validity members, `MotionShadowCheck` | MOVED (members: `VectorEngineState`; shadow check: `VectorEngine`) | `VectorEngineState.h` |
| d | invalidation requests, apply point, counters | PARTLY: counters, epoch consume and the rate-limited log moved; `RequestMotionInvalidate` (also feeds `frameAnalysis`) and the consumer-forget body of `ApplyMotionInvalidate` stay in Impl | `VectorEngine::TakeInvalidation/LogInvalidation` |
| e | `WarmIdleMotionEngine`, `PollMotionEngineBudget`, `QueryMotionEngineBudget` | MOVED (P5a-5) verbatim: out-of-line `Impl` member definitions; Impl keeps one declaration line each (same names, same default arguments) | `EngineKeeper.inl` |
| f | `UploadDejudderPlanes`, `TakeNextPlanes`, `RunDejudderEstimate`, `EstimateAhead` | MOVED (P5a-5) verbatim, as e. Only edits in the bodies: the K4 wrap of the matcher's `Estimate` call (`VectorEngine::Estimate`), the `NoteEngineGlobal` CPU record next to `NoteGlobalPair`, and `RunDejudderEstimate`'s defaulted `ahead` argument (true only from `EstimateAhead`'s estimate step). Callers in the P5b zone (`UpdateDejudderEstimate`, `EntranceOnceFieldAhead`, the extras) are unchanged | `VectorEngineEstimate.inl` (includes `EngineKeeper.inl`; included ONCE by the renderer right after the Impl struct) |
| K4 | camera-guided matching insertion point | DONE (P5a-5): `IPreEstimateHook::BeforeEstimate` right before / `AfterEstimate` right after the matcher's `Estimate`, inside `RunDejudderEstimate` (so both the in-render estimate and the estimate ahead); never for the warm-up's dummy estimate. No hook = `estimate()` only | `VectorEngine::Estimate` (VectorEngine.h), call site `VectorEngineEstimate.inl` |
| IVectorEngine | `vpv.vector_engine.v1` | DONE (P5a-5): `VectorEngine` implements it from today's state (section 7); the renderer's engine registers under its host (`vectorEngine{ this }`); `VectorEngineExtension` (hub module, after ResyncExtension) answers the query with the `IVectorEngineDirectory` | `VectorEngine.h/.cpp`, `VectorEngineExtension.h/.cpp`, `core/hub/HubBootstrap.cpp` |
| g | producer lifecycle and depth decision | MOVED: pure `ProducerPlan::Lifecycle/Depth` + `VectorEngine::PlanProducer` (a plain-value call site in `RenderLocked`, not a hook phase: it needs the pass's own facts) | `ProducerPlan.h`, `VectorEngine.cpp` |
| h, i | look-ahead copy under the queue lock; engage rule, `Submit` loop and its catch | NOT moved: Z3a (LVR `RenderLoop`, from 32815: the look-ahead copy and the submit loop) is held by T1 (`r1w1/t1` a9627e41, not merged), which rewrites the catch (B4 `LookAheadFailures`) | Impl |
| j, k | invalidate requests at Reset / ResetLiveQueue / gap / seek / format change as `OnQueue` events | NOT done: the six `RequestMotionInvalidate` sites in Z6 call a VP one-liner; converting needs the engine to reach `frameAnalysis` from a hub event (host binding), then net-0 lines | Impl |
| l | producer stop on GPU release (fix C, `fix-module-lifetime` 5f9c96b0) | NOT in this base: `RetireOutput` does not stop the producer here; `~Impl` does | - |
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
| `motionProducer` (the unique_ptr) | Rt (`PlanProducer`: create via `motionProducer = std::move(producer)`, stop, reset) | Rt; **Loop** (`RenderLoop`: 33167, 33171 `m_impl->motionProducer && ...->LastSubmitted()`; 33410 null check, 33439 `->Submit`) | **none. Existing hazard, unchanged by P5a** (byte-identical rule; first audit: not better, not worse). The loop null-checks at 33410 and dereferences at 33439 with `PrepareFrameAnalysis` (a conversion) in between, so the window is the whole per-frame hand-off, not only `LastSubmitted()`; a `reset()` destroys the `Producer` including the mutex `Submit` locks (use-after-free on a mutex), and the creating `std::move` assignment is a racing write to the same pointer. Fix options for the 2A audit: `std::shared_ptr` with atomic load, or a small mutex the loop and the render thread both take. Not done here. |
| `motionEngineReady` | Rt | Any | atomic (no reader left in the renderer file) |
| `motionValidity` | Any (`Invalidate`), Rt (`Consume`) | Any | atomics inside `MotionValidity::State` |
| `motionValiditySeen`, counters (`motionInvalidations`, log tick/cause, `motionRecoveryLogPending`) | Rt | Rt | single thread |
| `motionGapDetail` | Loop (dequeue gap) | Rt (log) | atomic pointer to a static string |
| `motionHandoff*` | Loop | Rt (periodic log) | atomics |
| `motionSegment`, shadow counters | Rt | Rt | single thread |
| `motionAheadHits/Misses`, `vectorPairs`, vector counters | Rt | Rt | single thread |
| `motionAheadLogged/At` | Rt (`PlanProducer`) | Rt | single thread |

## 3. Call order (unchanged by the move)

1. Render loop, queue lock: the look-ahead copy (item h, still in Impl).
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
| `motionAheadBackward`, `motionAheadDepth`, `motionAheadConfigured` | render loop (queue pass, submit) | P5a-6 (with item i, after T1) |
| `motionAheadEngaged`, `motionAheadEngagedGeneration`, `motionAheadPrev*` | render loop (engage rule, break rule) | P5a-6 |
| `motionProducer` | render loop; `RunDejudderEstimate`, `EstimateAhead` (item f) | P5a-5 / P5a-6 |
| `motionValidity` | every vector consumer (P4, P5b, P5c, P10), `RequestMotionInvalidate`, dequeue | P5b/P6/P10 |
| `motionSegment` + `using MotionShadowField` | `ReleaseDejudder`, `ApplyMotionInvalidate`, `RunDejudderEstimate`, `WarmIdleMotionEngine` | P5a-5 |
| `motionAheadHits`, `motionAheadMisses`, `vectorPairs` | `RunDejudderEstimate` (item f) | P5a-5 |
| `motionHandoffMaxMs/TotalMs/Count` | render loop hand-off timing, `RunDejudderEstimate` stats log | P5a-5 / P5a-6 |
| `motionInvalidations`, `motionRecoveryLogPending` | status line, first-estimate log in `RunDejudderEstimate` | P5a-5 |
| `motionGapDetail` | dequeue (Z6) | P5a-7 (j/k) |

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
Fix C (explicit producer stop on GPU release) is not in this base; when it lands it belongs in `OnLifecycle(GpuReleased)`.

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

## 8. Open items (audit of bff1c35e..d8605823, not fixed by decision)

- F7: only the LATEST field is served (`FieldOf` answers the last estimated pair only; the matcher keeps no ring). A
  consumer that misses a frame cannot ask for the field it skipped, and the engine does not count missed fields. A field
  ring (with a missed-field counter on the status) belongs to the P3 verified-field store, not to the live matcher.
- F8: the engine global's validity uses the statistics consumers' rule (`SampleGlobal(...).usable`: z < 0.9) and ignores
  the `w > 0.5` written flag that the intent interpreter checks; left as is (same rule as the fast-pan input).
