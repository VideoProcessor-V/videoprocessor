# Motion + queue/timing extraction: design, commit plan and host call-site patch (DRAFT)

2026-10-24, branch `feat/motion-timing-extract` from `feat/skeleton` d0212534 (worktree
`D:\codex\videoprocessor\motion-timing`). Not integrated: the branch adds new files only; every
edit to an existing file is in `motion-timing-callsites.patch` (`git apply --check --cached`
passes against d0212534).

## 0. Status 2026-10-25 (branch `feat/m2-motion`, WP-7 + M2 steps 16-18)

Rebased onto `integ/batch1` dc149284 together with the M2 moves of the VP motion/timing files
(`src/VP-V/modules/motion/{estimation,vector_delivery,consumers}`, `input/queue_timing`,
`output/presentation`; forwarding headers keep the host's old include spellings). Paths below that
name `src/VideoProcessor-Lib/...` for those files are the old ones; the module READMEs and
`migration-path-map.md` have the new ones. The three call-site patches, applied in this order on
the branch tip (each `git apply --check` clean alone and stacked):

| # | Patch | What | Behaviour |
|---|---|---|---|
| 1 | `motion-timing-callsites.patch` (P-a) | host queue calls `IQueueTimingPolicy` at the four points + one walk; `AlphaQueuePolicy.h` back to Bill's 83b7ffea text; tests include the compat shim; test project registers `QueueTimingPolicyTests` + `MotionSegmentTests` | byte-identical decisions (frozen-host comparison, re-frozen at 2f75d36d with the re-audit MED 1 frame-start rule: `DueContext::arrivalIsFrameStart`, `BacklogContext::headArrivalIsFrameStart`) |
| 2 | `motion-timing-slots.patch` (P-c + slot switches) | `SwitchHonoured` for `input.queue_timing`, `motion.estimation`, `motion.vector_delivery`, `motion.consumers`, `output.presentation`; one `SlotSwitchSettings::Apply` call in `ReadPipelineSettings`; `QueuePolicy()` picks `BillQueuePolicy` for the Original pathway or `input.queue_timing` off; the pipeline state line names the switched slots | Original pathway = Bill's queue policy. Decision-identical to today's Original pathway (budget off, `cadence_class_fixes` off = no cadence scaling, render-ahead off = no credit: the preconditions of `OriginalPathwayDecidesLikeUpstreamOnAFrameCountQueue`). A switched-off slot = its Original pathway overrides (one list, `OriginalPathway.h`); `motion.consumers` follows its vectors |
| 3 | `motion-shadow-callsites.patch` (P-b1 step 1) | the S0 segment runs beside the host's validity check and counts mismatches | pixels unchanged; the gate must show 0 mismatches before step 2 |

Apply from the committed blob, not the working-tree file: with `core.autocrlf=true` the checked-out
`.patch` has CRLF line ends and `git apply` rejects it (one cause of the rejects reported on
dc149284). On the branch tip: `git show HEAD:docs/design/motion-timing-callsites.patch | git apply`,
then `motion-timing-slots.patch`, then `motion-shadow-callsites.patch`. They target the tip of
`feat/m2-motion` (its test-include moves and the frame-start policy commits), not bare dc149284.
Compile-checked together (VPRenderer + Test, x64 Release, 2026-10-25 18:16).

P-b1 step 2 (consumers fetch from the segment) is NOT drafted as a patch yet: it goes in only after
step 1 shows 0 mismatches in the gates (section 5.1). One mismatch source is predicted
(findings C17): the estimate-ahead path refreshes `dejudderEstimatedEpoch` at consume time, so the
host accepts an ahead field across an invalidation that the segment (epoch at the inputs) refuses.

Sources: central-migration-needs.md section 6 (segment S0, clusters 7/20/22/25), the efficiency audit
of 2026-10-24, the v1.4.02 upstream forecast, and Bill's changes read with
`git diff 83b7ffea origin/v1.4.02-beta` (b1415da4).

## 1. What is upstreamable and what is VP-only

Owner direction (2026-10-24): the queue/timing policy interface and the shared look-ahead walk must
be offerable to Bill as a PR. His queue calls the interface, his current behaviour is the default
"Bill" policy, and ours plugs in from the fork. So the interface commits come first, are separate
from our implementation, and depend on nothing in `src/VP-V`.

| File | Class | Depends on | Notes |
|---|---|---|---|
| `src/VideoProcessor-Lib/vprenderer/QueueTimingPolicy.h` | **UPSTREAMABLE** | `AlphaQueuePolicy.h`, std | `IQueueTimingPolicy` with 4 decision points; `BillQueuePolicy` = his behaviour |
| `src/VideoProcessor-Lib/vprenderer/QueueLookaheadWalk.h` | **UPSTREAMABLE** | `AlphaQueuePolicy.h`, std | one walk: active picture + his preparation (preview UNION outward) + deeper statistics observers |
| `src/VP-V/modules/input/queue_timing/VpQueueRules.h` | VP-only | upstream header | our additions to `AlphaQueuePolicy.h`, moved verbatim |
| `src/VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h` | VP-only | interface, `DelayBudget.h` | fixed total delay, 4/3/2 ladder, capacity cap, held shed, start-based age gate |
| `src/VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h` | VP-only, transitional | `VpQueueRules.h` | keeps the `AlphaQueuePolicy::X` names for the host and ~10 tests |
| `src/VP-V/modules/motion/vector_delivery/MotionSegment.h` | VP-only | `MotionValidity.h` | S0: one vectors-valid state, published fields, consumers fetch directly |
| `src/VP-V/modules/motion/consumers/SampledPassTimers.h` | VP-only | std | GPU timers sampled 1 in N and polled; never drained |
| `src/VP-V/tests/QueueTimingPolicyTests.cpp`, `MotionSegmentTests.cpp` | VP tests | | CPU |
| `src/VP-V/tests/gpu/MotionTimingGpuTests.inc` | VP tests | BetaGpuTests helpers | written, not run |

The upstreamable pair compiles against both 83b7ffea and b1415da4. It does not call
`SelectActivePicturePreparation`, which exists only in his tip; it reproduces that function as the
walk's `preparation` view. A test checks the view against a reference copy of his function.

The PR to Bill would be those two headers plus the host hunks that call them. Those hunks are the
part of this patch that does not mention VP: `QueuePolicy()` returns a `BillQueuePolicy` in his
tree. **We do not open it.** billslack2 is read-only for us, and opening the PR is the owner's call.

## 2. The interface (where the host queue calls it)

| Point | Host site (d0212534) | Upstream policy (`BillQueuePolicy`) | VP policy (`VpFixedBudgetPolicy`) |
|---|---|---|---|
| `PlanHold(HoldContext)` | `UpdateQueueDelayHold` | no hold (frame-count queue) | `DelayBudget::Compute`, ladder, FIFO capacity cut, visible notes |
| `QueueTarget(TargetContext)` | `PrefillTargetLocked` | `ClampDesiredDepthToCapacity` (his exact function) | hold whole frames, else the cadence-scaled depth; render-ahead credit; look-ahead floor |
| `TicksUntilDue(DueContext)` | `HoldTicksUntilDueLocked` | 0 | age gate from the frame START (`arrivalQpc`, DeckLink normalised), transfer counted once; off on the VP timeline |
| `EvaluateBacklog(BacklogContext, BacklogRunState&)` | `RenderLoop` backlog block | `ShouldRecoverBacklog` with **his 5-argument signature**, flush to target | stale beyond target (H1), then `ShouldShedHeldBacklog` / `ShouldShedHeldExcess` (run of late heads) / `HeldShedFrames` (exact late frames, timing kept) |

- The host still acts on every decision: drops, logs, `EngageFallback`/`ReleaseFallback` and the
  steering/anchor resets. The policy only decides.
- `BacklogRunState` is owned by the caller, so one policy object can be shared.
- Our VP flags (Original pathway, `queue_timing_fixes`) are not part of the upstream contexts. The
  VP policy holds them as atomics.
- **Either/or** (vp-either-or-overlaps). The default is `queue_policy = vp`; `upstream` is the
  alternative. Both are shown in status through `Name()`.
  - In commit (a) the VP policy also serves the Original pathway (budget off, fixes off). That is
    exactly today's decisions.
  - A separate later commit hands the Original pathway to `BillQueuePolicy`, making Original =
    Bill. The test `OriginalPathwayDecidesLikeUpstreamOnAFrameCountQueue` proves the two policies
    are equal under the precondition: no cadence scaling and no render-ahead credit.
- Because the Bill policy calls `AlphaQueuePolicy::ShouldRecoverBacklog(5 args)`, his v1.4.02 rule
  applies to the upstream policy after the merge with no edit. That rule is the depth-scaled stale
  threshold `max(100 ms, 3P, (depth+1)P)`.
- The VP policy has its own copy of the 83b7ffea thresholds. His change therefore can never
  double-count the target on our held path, which is the forecast's section 2.1 defect.

**One walk** (`QueueLookaheadWalk::SelectWalk`). The consumers declare their depths:
- the active-picture horizon;
- `statisticsDepth`: max(LED wanted, intent wanted);
- `outwardFutureFrames`: his outward certificate, `AlphaSourceCrop::OUTWARD_PICTURE_CONFIRMATIONS_REQUIRED`
  after the merge.

One pass over the queue gives three views:
- `window`: every frame walked (statistics);
- `activePictureFrames`: the active-picture prefix (his `SelectActivePicturePreview`);
- `preparation`: his `SelectActivePicturePreparation`.

With `statisticsDepth = 0` and `outward = 0` the result is his preview exactly.

## 3. Commit plan

| # | Commit | Kind | Byte identity / timing expectation |
|---|---|---|---|
| 1 | c33aaa41 interface + walk (upstreamable) | new files | no behaviour (not included anywhere yet) |
| 2 | ded1ecfd VP rules moved verbatim + VP policy | new files (a) | the function text is identical. Decisions == frozen host code, checked by test (sweep: 20k hold cases, 200k target/due/backlog cases) |
| 3 | 089edbc3 motion segment + sampled timers | new files | no behaviour (not wired) |
| 4 | 1a012cba CPU tests | new files | see section 6 |
| 5 | (atomics) VP policy options as atomics | new files | same decisions |
| P-a | **patch, queue part** (apply = commit (a) of the integration) | host call sites | **byte-identical**: same decisions in the same order, including the `\|\|` short-circuit that leaves the excess run untouched; `AlphaQueuePolicy.h` returns to 83b7ffea byte for byte; renderer pixels unchanged (r6 byte-identity gate B3 on/off); delay totals at 4/3/2 x 8 rates unchanged (strict delay gate, ±0 in the model). Known non-semantic differences: `UpdateQueueDelayHold` now always takes `m_queueMutex` once for the capacity read (before: only when a plan was feasible); log text unchanged |
| 6 | DejudderWarp `PollPassTimers()` + `PassGpuMs()` (efficiency (b)) | VP file | byte-identical (adds non-blocking collection; defaults unchanged; production timing stays off until the segment arms it) |
| P-b1 | motion call sites (section 5): consumers fetch from `MotionSegment` | host, later | byte-identical: the same fields are delivered under the same validity predicate. Gate: r6 byte identity + capture-harness extras |
| P-b2 | arm `SampledPassTimers` in production (1 in 30, poll only) | host + DejudderWarp, later | pixels identical; GPU-time overhead ≤3% of an estimate (GPU test G2), versus 1.5-4 ms per estimate when drained |
| P-c | Original pathway → `BillQueuePolicy`; `queue_policy` key | host + settings, later | **behaviour change on Original only**: hold/target/backlog equal under the stated precondition; the Original-pathway regression run and the owner decide |

## 4. The patch (`motion-timing-callsites.patch`)

12 files, +121/-369. Apply with `git apply docs/design/motion-timing-callsites.patch` on d0212534.

- `AlphaQueuePolicy.h` (Bill's file): **restored to 83b7ffea** (−240 lines). This is the whole VP
  delta removed from his file.
- `LibplaceboVideoRenderer.h`:
  - `QueueHold` becomes `QueueTimingPolicy::HoldPlan`, with the same field names;
  - adds `m_vpQueuePolicy` and `m_upstreamQueuePolicy`;
  - adds `QueuePolicy()`;
  - `m_heldExcessRun` becomes `m_backlogRun`.
- `LibplaceboVideoRenderer.cpp`, at 4 decision points plus the walk:
  - `UpdateQueueDelayHold`: about 60 lines of planning become one `PlanHold` call; status and log
    stay in the host.
  - `PrefillTargetLocked`: becomes a `QueueTarget` call.
  - `HoldTicksUntilDueLocked`: becomes a `TicksUntilDue` call.
  - The `RenderLoop` backlog block: about 40 lines of rules become the context fill plus one
    `EvaluateBacklog` call.
  - The look-ahead walk: `SelectLookaheadWalk` becomes `SelectWalk`.
- 7 test files and `QueueStressHarness.h`: one `#include` of the compat shim. The test vcxproj
  registers the 2 new test files.
- `QueueTimingFixesTests.cpp`: the `NoCopiedRateBoundLiterals` scan skips `AlphaQueuePolicy.h`.
  Upstream's file keeps its literal 10..240 bound; `UpstreamRateBoundEqualsCaptureRateBounds`
  proves it equals `CaptureRateBounds`.

### How the patch meets Bill's v1.4.02 (where his hunks land)

His renderer (b1415da4) has **no motion code at all**: zero matches for de-judder or
MotionValidity. The motion extraction therefore never touches lines he edits; it only shrinks our
side. Queue and timing:

| Region | His hunk | Ours today (d0212534) | Ours with the patch | Expected merge |
|---|---|---|---|---|
| `AlphaQueuePolicy.h` | +21: `SelectActivePicturePreparation`; depth-scaled `StaleBacklogAgeThresholdMs`; 5-argument call | +237/−3 (forecast: 2 hunks, 212 conflicted lines) | **= 83b7ffea** | **clean: his version taken whole** |
| `RenderLoop` preview/walk | his preview + preparation loop + `futureOutwardFrames` loop after the preview | `SelectLookaheadWalk` + source-resolution/NVOF look-ahead (conflict @35700) | `SelectWalk` request | still textual (adjacent), but the resolution is 2 lines: `walkRequest.outwardFutureFrames = OUTWARD_...;` and use `walk.preparation` for `pictureMeasurementFrames` (one pass instead of three) |
| `RenderLoop` backlog block | 5-argument `ShouldRecoverBacklog`; log adds `StaleBacklogAgeThresholdMs(rate, target)` | about 40 lines of VP rules inside his block (conflict @30646 neighbourhood) | context fill + one call; his `while`/log lines unchanged | smaller: his 5-argument call lives in `BillQueuePolicy`; take his log arguments as they are |
| `PrefillTargetLocked` | unchanged (`ClampDesiredDepthToCapacity`) | VP hold/credit/floor body | policy call | clean (he did not touch it); his body is `BillQueuePolicy::QueueTarget` |
| `UpdateQueueDelayHold`, `HoldTicksUntilDueLocked` | absent | ours | thinner | clean (ours only) |

**Measured** with `git merge-tree --write-tree` of b1415da4 against two trees: d0212534 alone,
and d0212534 + the 5 commits + this patch (a temporary tree; no checkout, no branch moved).

| | d0212534 | with the patch |
|---|---|---|
| Conflicted files | 36 | **35** |
| `AlphaQueuePolicy.h` | 2 hunks / 208 lines | **0** (his file is taken whole) |
| `LibplaceboVideoRenderer.cpp` | 26 hunks / 3,816 lines | 26 / 3,816 (unchanged) |

- All four new call sites merge **clean**: the walk request, the backlog context, `QueueTarget`
  and `PlanHold`.
- The conflicts that remain next to them are unrelated:
  - his `previewBacklog` line against our `queuedAheadCounts`;
  - his `ActivePictureDiscardReason::BacklogRecovery` against our diagnostics record in the drop
    loop.
- His `SelectActivePicturePreparation` loop sits inside the existing 35673 conflict. Resolving it
  means setting `walkRequest.outwardFutureFrames` and taking `walk.preparation`.
- After the merge, `BillQueuePolicy` automatically runs his depth-scaled stale rule, and the VP
  policy keeps its own rule. That settles the forecast's section 2.1 item by construction.
- The renderer conflict count is unchanged because these queue sites were never inside the 26
  hunks. The renderer conflicts shrink only when the motion/timeline glue (clusters 7/20/21f)
  moves out (P-b1 and later).

## 5. Motion segment: call sites for the follow-up patch (P-b1)

`MotionSegment<GpuField>` lives in the renderer `Impl`. `GpuField` is the pl_tex bundle: field,
frame trust, sub-field, occlusion mask, camera, pixel mask, vector-ok. Today's estimation and
upload stay as the segment's estimation part. `RunDejudderEstimate` publishes; the consumers fetch.

| Today (d0212534) | Becomes |
|---|---|
| `RunDejudderEstimate` 16822-16975 (producer `Take` or `dejudder->Estimate`, mask epoch per slot) | the same calls, then `motion.Publish(Forward, a, b, field, source, epochAtInputs, geometryGen)`; the source is `EngineAhead`, `EngineAtRender` or `Matcher` (visible downgrade) |
| VP phase picture `Interpolator::Motion` assembly 7369-7382 | `motion.FetchFrom(Interpolator, A)` plus `motion.FetchAt(OcclusionMask, B)` |
| extras' `Interpolator::Motion` 17810-17820 (a duplicate of the above) | the same two fetches |
| extras' VPVEC 17895-17935 (`VectorField::Run` + hook binding + guard) | `FetchFrom(DitherExtras, A)`; the binding guard stays in the host (it is libplacebo hook plumbing) |
| B's first present VPVEC 24860-24890 (`ditherNextGate.CheckAt`) | `FetchFrom(DitherNext, A)` bound at production; `ditherNextGate` retires |
| B3 snapshot 6089-6192 (`EntranceOnceSnapshotFor`, epoch compare) | `Fetch(EntranceSnapshot, A, B)` on the snapshot slot |
| `RequestMotionInvalidate` / `motionValidity` (Impl 8088) | `motion.Validity()`: the same object, owned by the segment |
| smooth motion on/off | `motion.Engage(false/true)` (exclusivity) |

That removes about 6 separate validity expressions and 2 duplicate assemblies. The sequence/epoch
checks become one predicate (`Evaluate`), and per-consumer fallback counters come for free.

### 5.1 Shadow step, drafted: `motion-shadow-callsites.patch`

This patch (renderer only, +31 lines) applies alone or on top of the queue patch; both orders pass
`git apply --check --cached` on d0212534. It is P-b1 step 1, and nothing is delivered from the
segment yet:

- The `Impl` gets `MotionSegment<MotionShadowField> motionSegment{ &motionValidity }`, which
  **shares** the host's state.
- `RunDejudderEstimate` publishes the pair where the host sets `dejudderLiveFieldFrom = aSequence`.
  The source is engine ahead, engine at render or matcher. A pair spanning missed frames is
  published as a fill pair.
- `Retract(Forward)` runs at all 4 `dejudderLiveFieldFrom = 0` sites.
- The VP phase picture asks `MotionShadowCheck(seq, next, DejudderLiveFieldFor(seq))`. A mismatch
  is counted and logged on the first occurrence and every 2400th.

Expectations:
- pixels are identical, since only bookkeeping runs (r6 byte-identity gate);
- the shadow mismatch count is **0** over the gate runs with `motion_history_fixes` on (the
  default). With it off, the host does not require the live-field match, so mismatches after a
  retract are expected and explained.
- Step 2 is consumers fetching from the segment. It goes in only after step 1 shows 0 in the gates.

## 6. Tests

CPU (`VideoProcessor-Test`, single-threaded; built with the patch applied, see section 7):

- `QueueTimingPolicy`, 11 tests (incl. the upstream rate bound == `CaptureRateBounds`):
  - frozen-host equivalence for hold, target, due and backlog;
  - the 4/3/2 ladder and the capacity cap (visible note);
  - the held shed drops exactly the late frames and keeps timing;
  - the excess run needs 30 consecutive late heads at 60p and a skipped evaluation breaks it;
  - the age gate counts from the frame start, with the transfer counted once;
  - upstream policy = upstream functions;
  - Original = upstream on a frame-count queue;
  - either/or selection;
  - the one walk == the old walk and == his preparation (50k random queues).
- `MotionSegmentTests`, 11 tests (Retract and gap-fill/shared-state added):
  - direct fetch;
  - all 9 discontinuity causes make every consumer fall back;
  - an estimate straddling an invalidation is never delivered;
  - repeats, holes and geometry changes;
  - smooth-motion exclusivity;
  - the newest estimate wins across the ring;
  - low-latency backward current and last fields;
  - a sustained matcher fallback becomes visible (not when `optical_flow` is off);
  - sampled timers only poll.

GPU and timing (written, not run): `src/VP-V/tests/gpu/MotionTimingGpuTests.inc`, G1-G2 plus the
T1-T3 renderer gates with stated expectations. They need the 2.5 h timing gates next session.

## 7. Verification (2026-10-24)

Both patches were applied in the worktree and then reverted; the branch carries them as files.

- **Builds** (x64 Release, shared slot runner, `/m:4`, `CL_MPCount=2`): `VideoProcessor-Test` +
  `VideoProcessor-VPRenderer` built with the queue patch, and again with queue + shadow patches.
  The DejudderWarp efficiency commit built with them (one earlier relink failed only because a
  test process held the DLL).
- **CPU tests**, single-threaded vstest with TRX:
  - new tests 21/21: `QueueTimingPolicy` 11, `MotionSegmentTests` 10;
  - affected suites with the queue patch: 544/545 (Queue*, RenderAhead*, DelayGate,
    PipelineSkeleton, Efficiency, *Lookahead*, IntentInterpreter, CombinationRunner). The one
    failure was `NoCopiedRateBoundLiterals`, flagging upstream's restored literal; it is fixed by
    the exemption plus the equivalence test, and `QueueTimingFixes` is now 21/21.
- **Not run**: GPU tests (none today); the 2.5 h timing gates (T1-T3); r6 byte identity; the
  shadow mismatch count; the full CPU suite (about 4k tests); the Beta renderer project.
- **Patch checks**: `git apply --check --cached` on d0212534 passes for the queue patch, the
  shadow patch, and the two stacked.

## 8. Inefficiencies found in today's motion/timing glue (estimates, not measured)

| # | Where | Waste | Est. cost | Addressed by |
|---|---|---|---|---|
| 1 | `MotionWarp::TimedDispatch`/`FlushPassTimers` | per-pass timing drains the GPU: `pl_gpu_finish` before and after each of about 40 passes, plus 2 more in Flush (inner + outer). So production has **no** motion timing at all | 1.5-4 ms per estimate on the laptop at 4K when on (round trips + lost overlap); 0 today because it is off | commit 0bcab280 (`PollPassTimers`, single finish) + `SampledPassTimers` (1 in N, poll only; G2 expects ≤3%) |
| 2 | render-thread upload (`UploadDejudderPlanes`, raw RGB route) | the normal (film) mode uploads each 4K frame on the render thread; native RGB + de-judder uploads twice (no `TakeNextPlanes` reuse) | ~5.4 ms of render-thread time per 4K 12-bit source frame, ×2 on RGB | not here (efficiency audit #3); the segment's estimation part is where the shared early slot should feed both |
| 3 | vector hand-offs | about 6 host sites re-derive validity with separate expressions; `Interpolator::Motion` is built twice (phase + extras); `VectorField::Run` runs per extra | CPU <0.05 ms; the risk is the cost (the grain map and matcher history carried stale state across resyncs before) | `MotionSegment` (shadow now, delivery next) |
| 4 | sigma-delta own search when the de-judder field exists but is not carried | duplicate motion search at output size | ~0.2-0.5 ms on the laptop | `FetchFrom(DitherNext/DitherExtras)` makes "field exists" one query |
| 5 | NVOF producer CPU luma decode | the 5th-7th CPU pass over the same 4K frame (analysis, LED, intent, producer) | ~1-3 ms CPU per pass on the laptop (uncertain) | shared reduced-luma product (interface proposal; partly Bill's analysis) |
| 6 | two or three queue walks after the upstream merge | his preview, preparation and `futureOutwardFrames` loops plus our LED/intent walk: 3 passes and 3 `SourceBufferAddRef` sets | µs CPU, refcount churn | `QueueLookaheadWalk` (one pass, three views) |
| 7 | silent motion-source downgrade | "De-judder motion: block searches (no engine)" is **log-only** (vp-no-silent-fallbacks) | visibility, not time | `MotionSegment::FallbackEntry` (sustained matcher run while the engine is wanted) |
| 8 | `PrefillTargetLocked` | called repeatedly in dequeue waits, each call taking `m_queueHoldMutex` and re-running `LockedFilmQueueTarget` | µs | unchanged by design in commit (a); cache the target per queue change later |
