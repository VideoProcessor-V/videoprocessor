# M2 motion + queue/timing modules: code findings (2026-10-25)

Branch `feat/m2-motion`. The README-vs-code pass of the M2 moves (steps 16-18:
`motion.estimation`, `motion.vector_delivery`, `motion.consumers`, `input.queue_timing`,
`output.presentation`; migration-m2-plan.md, "Mandatory for every M2 module move") found the items
below. The move commits change no behaviour; each finding is fixed in its own commit, or handed to
the work package that owns the code. Line numbers are at the branch tip before the call-site
patches (`LibplaceboVideoRenderer.cpp` = the renderer host, Bill's file: fixes there go through a
patch).

Disposition key: **WP-4** = the silent-fallbacks call-site patch (`silent-fallbacks-callsites.patch`)
already wires the builder; **host patch** = needs a renderer call site; **gate** = changes timing
or pixels, needs the big test round first; **no fix** = not a defect on inspection.

## Visibility (ADR-0001, vp-no-silent-fallbacks)

| # | Sev | Where | Finding | Disposition |
|---|---|---|---|---|
| V1 | HIGH | host 7020-7055 | render-ahead open loop is log-only; `FallbackCatalog::Presentation` / `RefreshRate` have no call site | WP-4 (its patch calls both) |
| V2 | MED | host 17108, 23251-23279; `DejudderWarp.cpp` 3359-3365 | optical-flow engine failure / producer stop -> matcher is log-only | WP-4 (`FallbackCatalog::Motion`) |
| V3 | MED | `RenderedWarp.cpp` 358-371 | PRE_OUTPUT cache copy failures counted, `Failed()` never read: extras silently use the source warp | host patch (engage an entry while failures recur); listed for WP-4's "builders ready but not wired" table |
| V4 | MED | host 18310, 7294, 17948 | warp / rendered warp / interpolation failures hold A with a log + status line only | host patch (entry while failures keep recurring) |
| V5 | LOW | host 27071 | the PENDING-RA note keeps a `delay budget` entry engaged whenever render-ahead runs, though nothing is downgraded | host patch (move to a status line); keep until the render-ahead total is verified |
| V6 | LOW | `VideoProcessorDlg.cpp` 14967 (Bill's) | `ClampToCapacity`'s `clamped` result ignored: the target clamp to `queue_size` is silent | note for Bill / host adapter |
| V7 | LOW | host (several) | render-ahead gate step-down, refresh switch kept another rate, low-memory engine release: log-only | WP-4 covers the refresh switch; the others host patch |

## Correctness

| # | Sev | Where | Finding | Disposition |
|---|---|---|---|---|
| C1 | MED | vector validity | a scene cut is not a `MotionValidity` cause and `MotionSegment` has no cut miss; only the per-pair frame trust holds a cut. The owner's rule (vp-vector-validity) lists cuts | gate: add `Cause::Cut` + the host invalidation at the scene detector's cut (behaviour change on cut pairs) |
| C2 | MED | host 17013-17018 | the motion-source key comes from the settings, not the source actually used: a producer stop / engine failure / low latency changes the vector source without a `MotionSourceChange` invalidation; the matcher history carries over | host patch + gate (key on `ExternalFlowUsed` / `OpticalFlowUsed`) |
| C3 | MED | `DejudderWarp.cpp` 3359-3365 | `opticalFlowFailed` latches for the instance: one transient engine error disables the render-time engine until the matcher is recreated | fix with a retry/backoff or clear on resync (gate: changes the motion source) |
| C4 | MED | host 8153, 28576 | `FitDelayBudget` judges render-ahead against the frame-count target, not the hold's whole frames | gate (timing) |
| C5 | MED | `core/DelayBudget.h` 41, `DelayBudgetLadder.h` 25 | the budget assumes a 2-frame swapchain while render-ahead's chain runs at latency 3 (`RenderAheadPolicy.h` 32): with render-ahead the total may be one refresh short | gate (timing; the strict delay gate measures it) |
| C6 | MED | `DelayBudgetLadder.h` 20, `VpvSection.cpp` 164 | the Config recommendation uses a fixed 60 Hz reference (`kReferenceRefreshHz`) - against vp-general-not-fixed | fix: use the measured/published refresh (Config display only; no renderer decision) |
| C7 | LOW | host 27177 | plain-path age gate waits the full hold while the render-ahead credit lowers the target; could overshoot by the credit if render-ahead ran without the VP timeline (not reachable today) | check before fixing |
| C8 | LOW | host 17004 | settings-change and motion-source invalidations only with `motion_history_fixes` on | by design (fix switch); Original pathway keeps upstream behaviour |
| C9 | LOW | `MotionSegment.h` 203-212 | after a successful frame-old fallback `lastMiss` still shows the earlier miss | fix with P-b1 step 2 (segment not delivering yet) |
| C10 | LOW | `InterpolationTimeline.h` 121 | `FieldPeriods` clamps to `kMaxSpanFrames` (4) while gap pairs can span up to 10 at 60p | harmless today; revisit with fill-gap spans |
| C11 | LOW | `DejudderPolicy.h` 86 | legacy fixed film-rate bands remain with `general_rate_fixes` off | by design (the switch's off state) |
| C12 | LOW | host 16433 | `ii.subtitleComposed` is always false: the SubtitleComposed reason never occurs | host patch |
| C13 | LOW | `MotionAheadProducer.cpp` 358-359 | a target size the producer cannot make returns success silently, so every pair misses | fix: count + log once (then V2 makes it visible) |
| C14 | LOW | `MotionAheadProducer.cpp` 69-71 | R12B decode reads whole 36-byte blocks at the row end | no fix: R12B rows are padded to whole 8-pixel blocks (DeckLink row bytes = ceil(w/8) x 36), as v210's 6-pixel groups |
| C15 | LOW | `MotionAhead.h` 23-27 | the B3 comment ("167 ms at 24p") reads as current although r6 fix 3 replaced it | fixed in this branch (comment) |
| C17 | MED | host ~16620-16626 (`dejudderAhead.Consume` branch) | an estimate-ahead field gets `dejudderEstimatedEpoch = motionValidity.Epoch()` at CONSUME time, not the epoch its inputs were taken in; an invalidation between the ahead estimate (previous frame's extras) and the consume is not seen by `DejudderLiveFieldFor` (`Consume` itself checks no epoch; only `PairValid` of the pair at consume time). The S0 segment keeps the inputs' epoch and refuses it: the shadow gate's mismatch counter will show it | gate: record the ahead estimate's epoch in `AheadSchedule` and use it at consume (or let P-b1 step 2's segment decide) |
| C16 | LOW | host 17081 | the failure log interval is a fixed 2400 frames | fix with `GeneralRate::LogInterval` (log cadence only) |

## Efficiency

| # | Where | Finding | Disposition |
|---|---|---|---|
| E1 | `MotionAheadProducer.cpp` 144-155 | `HalfLuma8` creates up to 3 threads per frame | persistent pool (WP-23 shared luma decode replaces this decode) |
| E2 | `DejudderPolicy.h` 591 | `LatencyFloor::Floor` allocates a vector per call | small-buffer fix |
| E3 | host 7390-7412 | WIP debug (`VP_DEBUG_PHASE_NOVECTORS` / `VP_DEBUG_PHASE_AB`) calls `GetEnvironmentVariableA` per phase picture | remove with the near-black investigation (integ/batch1 WIP, not this branch) |
| E4 | `consumers/SampledPassTimers.h` | written, not wired (P-b2 arms it) | P-b2 |
| E5 | host 7010 | the vblank sample blocks the render thread ~9 refreshes at most every 10 s | documented in code; acceptable |

## Key lists and fallback entries (applied in this branch)

- slots.json / manifests now list the keys the code reads (commits "M2 README-vs-code pass").
- Removed entries no code produces (`Vectors: invalid`, `Motion consumers: no-motion form`);
  estimation and presentation now name the `FallbackCatalog` texts (produced once WP-4 lands).
- Not owned by any slot yet: `dejudder_pulldown`, `switch_refresh_rate`,
  `[display_refresh_rate_override]`, the deinterlace keys read by `TemporalRenderer.h` (owner to
  place).
