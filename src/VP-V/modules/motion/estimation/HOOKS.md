<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Vector engine: what moved out of the renderer, what is left, who reads what (M2 P5a)

Plan: doc 52 section 3 (inventory a-l, hooks, identity, commit sequence). Base cda5794e. This file is the P5a-1
inventory (member list, readers outside P5a's ranges = the alias list, the thread of every access) and the
alias table the package leaves behind.

## 1. Status by inventory item

| Item | What | State at this tip | Where it lives now |
|---|---|---|---|
| a | estimate state (ahead payload/textures/schedule, upload layout, `dejudder`, estimated seq/epoch, idle keeper, live-field bookkeeping) | NOT moved (P5a-5: heavy host coupling: textures, GPU unpack, early upload) | Impl |
| b | vector engine: demand decision, statistics service, global-pair hysteresis, delivered log, engine size | MOVED (`VectorDemandInput` stays in Impl: it only gathers the renderer's facts) | `VectorEngine.cpp` |
| c | producer / look-ahead / validity members, `MotionShadowCheck` | MOVED (members: `VectorEngineState`; shadow check: `VectorEngine`) | `VectorEngineState.h` |
| d | invalidation requests, apply point, counters | PARTLY: counters, epoch consume and the rate-limited log moved; `RequestMotionInvalidate` (also feeds `frameAnalysis`) and the consumer-forget body of `ApplyMotionInvalidate` stay in Impl | `VectorEngine::TakeInvalidation/LogInvalidation` |
| e | `WarmIdleMotionEngine`, `PollMotionEngineBudget`, `QueryMotionEngineBudget` | NOT moved (P5a-5, with a) | Impl |
| f | `UploadDejudderPlanes`, `TakeNextPlanes`, `RunDejudderEstimate`, `EstimateAhead` | NOT moved (P5a-5) | Impl |
| g | producer lifecycle and depth decision | MOVED: pure `ProducerPlan::Lifecycle/Depth` + `VectorEngine::PlanProducer` (a plain-value call site in `RenderLocked`, not a hook phase: it needs the pass's own facts) | `ProducerPlan.h`, `VectorEngine.cpp` |
| h, i | look-ahead copy under the queue lock; engage rule, `Submit` loop and its catch | NOT moved: Z3a (LVR RenderLoop 33700-34050) is held by T1 (`r1w1/t1` a9627e41, not merged), which rewrites the catch (B4 `LookAheadFailures`) | Impl |
| j, k | invalidate requests at Reset / ResetLiveQueue / gap / seek / format change as `OnQueue` events | NOT done: the six `RequestMotionInvalidate` sites in Z6 call a VP one-liner; converting needs the engine to reach `frameAnalysis` from a hub event (host binding), then net-0 lines | Impl |
| l | producer stop on GPU release (fix C, `fix-module-lifetime` 5f9c96b0) | NOT in this base: `RetireOutput` does not stop the producer here; `~Impl` does | - |
| reservations | `refined_field` (P3r / U15), `SubmitLongPair` with external hints | interface + contracts, default "absent" | `VectorEngineInterface.h` |

## 2. Cross-thread map (the P5a-1 thread column)

`Rt` = render thread (RenderLocked, `renderMutex` held). `Loop` = render loop thread (queue lock / no lock). `Any` = any
thread. `Ui` = UI / config thread.

| Member (State field) | Writers | Readers | Protection |
|---|---|---|---|
| `vectorDemand` | Rt (`UpdateVectorEngine`) | Rt; Loop (`motionAheadEngaged` log text: `lowLatencyAllowed`, 33942) | none: plain struct read by Loop (unchanged from before the move; benign torn read at worst of a log text) |
| `queueAheadCapacity` | Loop (queue pass) | Rt (`VectorDemandInput`), Loop | atomic |
| `motionAheadBackward`, `motionAheadDepth`, `motionAheadConfigured` | Rt (`PlanProducer`) | Loop, Rt | atomics (acquire/release) |
| `motionAheadEngaged` | Loop (engage/disengage), **Rt** (`PlanProducer`, two writes) | Loop, Rt | **atomic since this package** (was a plain bool written under two different locks: doc 52 D-3, no value change) |
| `motionAheadEngagedGeneration`, `motionAheadPrev*` | Loop | Loop | single thread |
| `motionProducer` (the unique_ptr) | Rt (`PlanProducer`: create via `motionProducer = std::move(producer)`, stop, reset) | Rt; **Loop** (33716, 33720, 33959, 33988: `motionProducer && ...->LastSubmitted()`, `->Submit`) | **none. Existing hazard, unchanged by P5a** (byte-identical rule; first audit: not better, not worse). The loop null-checks at 33959 and dereferences at 33988 with `PrepareFrameAnalysis` (a conversion) in between, so the window is the whole per-frame hand-off, not only `LastSubmitted()`; a `reset()` destroys the `Producer` including the mutex `Submit` locks (use-after-free on a mutex), and the creating `std::move` assignment is a racing write to the same pointer. Fix options for the 2A audit: `std::shared_ptr` with atomic load, or a small mutex the loop and the render thread both take. Not done here. |
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
   `VectorEngine::UpdateVectorEngine` -> `VectorEngine::ServeStatistics` -> the estimate (item f, Impl) ...;
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

## 5. Teardown

`~Impl` still destroys the producer (its own optical-flow device). GPU objects of the matcher are released by
`ReleaseDejudder` (item a, unchanged). The state struct is declared at the position of the earliest moved member (the
vector-demand block), earlier than the old `motionProducer` / `motionValidity` positions; destruction order against the
members declared in between changed, none of which the producer or the validity state uses (the audit greps this).
Fix C (explicit producer stop on GPU release) is not in this base; when it lands it belongs in `OnLifecycle(GpuReleased)`.

## 6. Identity proof status

| Proof | State |
|---|---|
| `ProducerPlanTests` (3.2 M rows against a test-only copy of the old logic; a mutation fails it) | written; run in the scratch harness, passes; project run after the gate |
| `VectorEngineTests` (fake host: sync/publish rules, hysteresis, size, shadow, reservations) | written and syntax-checked; project run after the gate |
| existing: VectorDemand, MotionValidity, MotionAhead, DejudderPolicy, `EstimatorHookTests`, QueueStressHarness | to run on the fresh full-solution build |
| chain-gate motion subset, frame hashes vs G1; skeleton timing gate; "Motion ahead:" log diff | coordinator, after the gate |
