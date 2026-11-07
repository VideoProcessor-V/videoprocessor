# VP-V core: the pipeline skeleton

Header-only and pure: no libplacebo, no Windows, C++14. The renderer and the CPU unit tests
(`src/VP-V/tests/PipelineSkeletonTests.cpp`) share these headers.

## The headers, in reading order

| Header | One job |
|---|---|
| `PipelineSlots.h` | The slot table: ids, names, designed order, hook point, status and origin. It mirrors `docs/design/slots.json`, and the drift check compares the two |
| `SlotContracts.h` | What each slot needs and gives (resolution, light, plane layout, vectors, original frame), its zone, and the hard precede rules (ADR-0021) |
| `OrderValidator.h` | `Validate(order, &reason)`: accepts or rejects a whole order, with a plain reason |
| `SlotRegistry.h` | THE registry the renderer consults: which slots run (slot switches, the zero-cost rule), the Original pathway (ADR-0019), the current order (`SetOrderByNames`), and the net fingerprint key (`UpstreamKey`) |
| `SlotSwitchSettings.h` | Slot switches for `input.queue_timing`, the three motion slots and `output.presentation`: a slot off runs in its upstream form (its Original pathway overrides from `OriginalPathway.h`), `motion.consumers` follows its vectors (SlotContracts), and the Original pathway or `input.queue_timing` off select Bill's queue policy (`UsesUpstreamQueuePolicy`). Tests: `SlotSwitchSettings` |
| `EntranceStages.h` | The input side as one stage list plus the per-source-frame cache (barrier B3). Generic over the frame type, so it is tested on the CPU |
| `OutputHookOrder.h` | The corrected output-hook order (ADR-0013). The renderer uses `OrderSlotsInPlace`, which moves only slot hooks |
| `VectorScale.h` | Per-axis vector scaling (ADR-0006) |
| `DelayBudget.h` | The one user-set total delay: frames = floor((total - fixed) / period), the 4/3/2 ladder, padding (ADR-0020) |
| `SlotTiming.h` | Per-slot GPU timing and the projected 3090 result (barrier B4) |
| `FallbackStatus.h` | "Fallbacks engaged: (list) / none" (ADR-0001). Built on fix/r6-baseline and moved here |
| `FallbackCatalog.h` | The "Fallbacks engaged" entries of the renderer's automatic downgrades (silent-fallbacks batch) |
| `ResyncCause.h` | Resync vocabulary: `Level`, `Cause` (with tokens and plain text), `RefusalReason` |
| `Indicator.h` | The central Indicator: counters, stamps derived from a ring of events, readers (`Observe`); `LastCauseIn(counter, fromExclusive, toInclusive, cause, detail*)` returns the latest retained event's cause for an exact counter (false when none, or when the interval is older than the ring) |
| `Ancestry.h` | Ancestor references, headers and the pure validator for derived products |
| `FrameSeal.h` | `VpSeal::FrameSeal`: the ONE owner of the Indicator seal. On a new era it registers every provider (resync Drive, R2 motion readers, the lane handle, ...) before the first Observe, then seals once per frame and fans (seq, changed) out to subscribers (Drive latch, lane regulator). `AnalysisLane` holds the Indicator by shared handle (`BindIndicator`) and, with `SetExternalSeal(true)`, takes `OnSealed(seq, changed)` instead of sealing; the storm hold reaches its regulator through `SetStormHold` (T0) |
| `FrameAnalysisValidity.h` | `VpFrameAnalysis::RecordValid(indicator, recordSeq, useSeq, mask)`: per-record temporal validity from the Indicator's stamps (FrameAnalysisStore itself unchanged; no private frame history added) |
| `Products.h` | Skeleton S0 registry: product declarations (inputs with lag, backend, history, size, honoured counters, tier, residency, variants) the first products P0-P6 (+ P2m matcher pair) and the camera products P7-P9 (`SkeletonProducts()` is the one assembly point); look-ahead inputs (`InputRef::lead`, `AddLead`) with a `shortQueueSubstitute`; component consumer declarations |
| `ProductGraph.h` | Skeleton S0 graph: the closure of what active consumers need (each product once, topological, deterministic), fixed ring and live-pool counts, rebuild triggers, `GraphOwner` (advances the Indicator's `graph` counter), Indicator ring sizing (N6). A look-ahead input gets an effective lead = min(declared, lane lead - 1) (the lane makes N+L as N starts rendering, so N+L itself has no slack; a lead below the declared one is a PARTIAL lead, listed by `Graph::PartialLeadNodes()` with both numbers); a product read at lead k has k more ring slots; with no lead available the node resolves to its short-queue substitute and is recorded in `Graph::reductions` (asked -> made) and listed by `Graph::ReducedNodes()` (visible reduced mode; a substitute that was also asked directly is still listed, the record says what was reduced). `Registry::Validate` rejects lead chains, lead loops and a substitute that reads the product it replaces. Governed history (W1-SKG): `ProductDecl::historyFloor`, `GraphSettings::historyCaps` (the governor's grant), `Node::historyFull/historyMin`, `Graph::HistoryReducedNodes()`, rebuild reason `kRebuildHistory`. Lead edges are sealed by the K1 rule (see below) |
| `ProductSlots.h` | Skeleton S1 slots: the 64-bit state word, ABA-proof generation-tagged claim, lane/live headers, lane-owned rejected buffers, live pool for every backend, lane input pins, the seal (stamps, span, ancestry cgens) and the cascade (`SlotTable::Resolve`); lead ancestors (K1, `SetStampSource`); ordered-chain carry-forward |
| `CardProfile.h` | Skeleton S2: card profile (cached per adapter LUID + driver), adaptive VRAM headroom, system-RAM rules |
| `VramGovernor.h` | Skeleton S2: the ONE VRAM governor and the tier plan on it. Every VRAM user (early-upload pool, render-ahead, vector engine, TemporalDetail, camera model, interpolator, enhancers, fixed libplacebo/capture memory, and the skeleton products) registers levels + tier; one budget (own usage re-planned), one shed order (tier, lossless first, order); one status line per cutback. `PlanTiers` = T0 first then T1/T2 by ms saved per MB as that order; `ApplyLaneGrants` sets the slots |
| `LaneInterfaces.h` | Skeleton S3-S7 contracts: `SourceFrameView`, `ILaneProducer`, `IGapWork`, `IGpuDispatcher`, `IReadbackDevice` (the host owns every device call) |
| `LaneRunner.h` | Skeleton S3: the lane runner, one state machine per product over the S1 slot protocol (claim, pins, stamp, chunks, publish or abandon after the fence); no thread of its own |
| `OrderedChain.h` | W1-SKG: `ChainOrder`, the frame-order rule of an ordered-chain lane product (in order / carry-forward / refused) |
| `SealPoint.h` | Skeleton S3: `RenderStartSeq`, `CheckLead` (queue deep enough for the lane) and the N1 bounded deterministic wait (`DecideWait`, never past the present slack) |
| `GapScheduler.h` | Skeleton S4: render-thread idle-gap scheduler, fixed class priority (render-ahead extras, early-upload wraps, lane compute), deterministic order, never starts a chunk that does not fit |
| `Regulator.h` | Skeleton S5: the slack regulator, the ladder of lane/in-render/dropped states, margin on render-only time, dwell, multiplicative back-off, death-spiral guard, traces and notice |
| `AnalysisLane.h` | Skeleton S3-S7 facade: the ONE object the renderer holds (null when off): settings keys, per-graph state swapped by `shared_ptr`, host hooks (listed at its top), worker threads, status lines and fallbacks |
| `StatusText.h` | VP's status text: the renderer's `vp` module lines (generic `GetModuleStatus` entry), the Ctrl+I rows, crash-state and report lines, content/room/key-override/nets wording (WP-11) |

## How a frame runs through the skeleton

1. **Settings apply.** `UpdatePipelineSlots` in the renderer builds a `Registry` from the
   `[vpv]` keys (ADR-0023):
   - the Original pathway, if `pipeline_original_pathway = on`;
   - slot switches from `pipeline_slots_off`;
   - the order from `pipeline_order`, which is validated. An invalid order keeps the designed
     order and shows "Fallbacks engaged: pipeline order (...)".
2. **Entrance.** The renderer's `RunEntrance` walks `registry.ActiveEntrance()`:
   - re-base, deband and the texture-restore stages, in the registry's order;
   - one list for the main draw, de-judder extras and the crossfade's path being left;
   - a cadence repeat of a picture drawn as itself reuses the cached result.
3. **Output hooks.** Each slot hook is tagged with its slot. `OrderSlotsInPlace` puts the slot
   hooks in the registry's order without moving any other hook.
   - The designed order gives today's chain unchanged.
   - With `calibration_lut_stage = vp`, the LUT becomes VP's last colour hook.
4. **Zero cost.** A slot that is off, or a placeholder without a component, has no case in the
   stage list and registers no hook. The picture is byte-identical to the slot not existing.

## Adding a slot (checklist)

1. Add it to `docs/design/slots.json` and to the table in `PipelineSlots.h`, in designed order.
2. Declare its contract in `SlotContracts.h` (`ContractOf`) if it may move.
3. Create its module folder (`src/VP-V/modules/<unit>/<slot>/`) and run
   `python tools/check_module_manifests.py --write`.
4. Give it a case in the renderer's `RunEntrance`, or tag its hook with `TagSlotHooks`, only when
   a component exists.
5. Add tests: placeholder byte-identity, isolation, contract, chain.

## Segments (DRAFT 2026-10-24, not integrated)

`SegmentRunner.h/.cpp` is the core of the owner's chain-segment principle: VP stages hand
directly to the next VP stage, and the host (libplacebo) sees one entry and one exit per
contiguous VP segment (`docs/design/central-migration-needs.md` section 6).

| Piece | Where | What |
|---|---|---|
| Stage contracts | `SegmentRunner.h` (`StageContract`, `StageInfo`) | size domain, colour domain, intermediate format, BTB/WTW range, vector validity; roles Vp / Edge (user shaders split the segment) / Quantiser (must be last) |
| Checks | `CheckOrder`, `CheckContracts` | run on a settings change only (`Segment::Configure`, keyed by a signature). The registry order must pass `OrderValidator`; stages follow the registry ranks; nothing after the dither; no stage clips BTB/WTW before the dither |
| Fallbacks | `Segment::Configure` / `Downgrade` | a failed check or a run-time stage failure runs the separate hooks (today's behaviour) and is listed as "Fallbacks engaged: output segment S2/S3 (...)" |
| Timing | `TimingSampler` | 1 in N runs timed; results polled without waiting; recorded per slot in `SlotTiming` |
| Pool + one-hook runner | `src/VideoProcessor-Lib/vprenderer/HookSegment*.h` | generic (no VP-V dependency, offerable upstream): runs a run of hooks as one libplacebo hook on its own texture pool |
| Output stage adapters | `src/VP-V/segments/` | S2 `OutputColourSegment`, S3 `OutputFinishSegment` (separate form, byte-identical), `Merger` (merged form behind `output_segments_merged`, default off) |

S2 (PRE_OUTPUT): SDR tone shape, DV trims, [Stage 13], LED measurement, LED compensation,
[black level], | user shaders |, calibration LUT. S3 (OUTPUT): encoding repair, occlusion mask,
sigma-delta last. The calibration LUT stays in S2 while libplacebo owns the output encoding
(see `docs/design/segments-callsites.md`).

The skeleton packages (S0-S2) are core library only: no renderer call sites yet. Vector demand
states its consumers in registry terms in `modules/motion/estimation/VectorDemandProducts.h`
(VectorDemand's decision is unchanged). Slot sizing departs from design doc 25 B3 in one place:
the live pool is history + 1 + R (+ span) + one spare per input reference held by lane readers,
not R + 1, because every frame the render holds may be a live fill and a late lane reader may pin a
released live buffer; ring and live pool are separate memory (a lane costs its whole ring).

`Indicator.h`, `Ancestry.h` and `ResyncCause.h` stay in `src/VP-V/core/` because they are skeleton-owned shared headers, not a pipeline slot (a module folder would need a fake slot). The resync detector and its shadow live in `src/VP-V/modules/input/signal`.


## GPU allocator (SA1)

`GpuAllocator.h` / `ScratchArena.h`: the shared scratch arena (per-render temporaries, capacity = busiest scope per texture shape), `ProductTextures` (ring + live pool from a declared layout that must equal the product's `SizeRule`), the arena as governor user `kUserScratchArena`, `Residency::Transient` (not a ring product) and `VramLedgerLines` for Ctrl+Alt+D. Details and call sites: `modules/control/gpu_memory/README.md`. `kP10GrainSplit` (SA2) is the only product added after the camera products; SA0 adopts its declaration.

## Seal and graph capabilities (W1-SKG, doc 72 section 5 items 1-4)

Unit: control/skeleton (core). No new settings keys; every capability is declared by the products and bounded by the
VRAM governor (budget-driven, no per-GPU flags).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics

| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products

| Capability | Contract | Seal / fill rule | Visible when it engages |
|---|---|---|---|
| Lead edges (`InputRef::lead`, `AddLead`) | the lane job of N pins the ancestor result of N+k (it waits until that frame is published); effective lead = min(declared, lane lead - 1) | the seal accepts a lead ancestor that is Published (or Sealed / Live) with the recorded cgen and an honoured stamp equal to `StampOf(N+k)` under the ancestor's own reads (`SlotTable::SetStampSource`; `AnalysisLane` sets it); a moved stamp is `StampMismatch` -> live fill; without a stamp source the seal refuses (`AncestryIncomplete`). Never two producers per slot per frame; a lane publishes before the seal else the first consumer live-fills (doc 25, unchanged). Residual risk: N+k can still be rejected at its own later seal (its readers were sealed earlier); counted in `SlotCounters::leadAccepted`, and in `leadLaterRejected` when it happens (also for a reader that was LIVE-filled from a Published lead ancestor). A lead reader of an ORDERED-CHAIN product cannot be sealed ahead (the chain target runs at most one frame ahead of its seal, see the next rows), so it falls to its live fill / declared reduced form: `Registry::Validate(&why, &warning)` reports every such read as a WARNING (never a reject; `LeadChainReads()`), `Graph::leadChainReads` / `leadChainWarning` keep the EFFECTIVE ones (see "Lead reads of an ordered chain" below) | `Graph::PartialLeadNodes()`, `ReducedNodes()` (short queue -> causal substitute); status line "Analysis warning: <reader> -> <chain> (lead N): lead read of an ordered chain: reader live-fills its reduced form" and the Fallbacks entry `skeleton_lead_chain` (appended only while such a read is in the graph) |
| Limits | `kMaxInputs` 16 (was 4), `kMaxConsumes` 10 (was 6), `VpAncestry::kMaxAncestors` 16 (was 8, static_assert >= kMaxInputs); `ProductFrameResult::missingInputs` is a 32-bit mask | all fixed arrays are sized from the constants. Cost: `Node` +96 B, `ProductDecl` +48 B, `ConsumerDecl` +16 B, slot header pair +384 B; one `Header` copy per live fill (about 0.4 KB), nothing else per frame | `CameraLink::kMaxGuideLag` pins the guide lag clamp at 16 (it was `kMaxInputs * 4`) |
| Ordered chain (`ProductDecl::orderedChain`) | the product reads itself at lag >= 1; its lane jobs run in frame order (the runner is sequential per product; `ChainOrder` refuses a frame not after the last published one) | an absent self lag input (chain start, a late/abandoned predecessor, a skipped range) is not waited for: the job runs in its declared reduced form (`LaneJobContext::chainReduced`), publishes `Header::chainReduced` with an empty self ancestor, the seal accepts exactly that (`SlotCounters::chainCarryForward`). An undeclared hole is rejected (live fill). A chain job pins only a RENDER-VALID predecessor (Sealed / Live; `SlotSet::PinRef(..., renderValidOnly)`): a Published-unsealed predecessor is Transient and the job waits, so a rejected-and-live-filled frame can never leave a successor holding a replaced ancestor (no `AncestorReplaced` cascade); the chain runs at most one frame ahead of the seal | status lines "ordered-chain frames ran in the reduced form", "out-of-order chain frames refused", "ordered-chain frames waited on a rejected predecessor" (a Rejected predecessor is waited for, not carried; counted once per frame in `SlotCounters::chainWaitRejected`, also when its live fill declines or has no buffer), "ordered-chain frames waited for the predecessor's seal" (`SlotCounters::chainWaitUnsealed`, once per frame); `LaneProductCounters::chainCarryForward/chainRefused`. Declared chains in the catalog: td.blocks, coding_evidence, quality_pattern, quality_pattern.causal |
| Single store (`ProductDecl::singletonStore`) | one persistent store read at lag 1 by its readers; declared history 0, no `historyFloor`, not self-reading (`Registry::Validate`); depth stays max(0, largest lag read); copied to `Node::singletonStore`; never governed; every reader reads it at lag 0 or 1 (`Registry::Validate`, `BuildGraph` for consumers). `BuildGraph` sizes it as a ping-pong: ring = 2 buffers whatever R and L are (`kSingletonStoreBuffers`), live pool = 0 (lane-only: no live fill), VRAM figure = 2x the store | the producer's job for frame N+1 reuses the slot of frame N-1 and waits (Transient) while a lag-1 reader still pins it (`SlotCounters::writeWaitPinned`, counted once per job by `LaneRunner`); an absent frame is Unavailable (`NoLiveBuffer`, no live pool exists) and its readers run their own visible fallback; the governor's lane-drop level for it is "not kept" (0 B) and it ranks as LOSSY (`VramUser::lossless = !singletonStore`: with no live fill, dropping the lane makes its readers lose the store and run their own fallback, a reduced non-identical mode, so it is not shed in the lossless-first class). A lag-0 / lag-1 READER job whose store frame is Rejected (the seal refused it; a store has no live fill, so the frame never gets content) waits until the frame is released: counted once per reader frame in `SlotCounters::storeWaitRejected` (on the store's set) | rejection reasons "a single-store product ..." (lane unavailable, Fallbacks); status lines "lane jobs waited for a late reader to release their slot", "N single-store reader frames waited on a rejected store frame (a store has no live fill: the reader job waits until the frame is released)"; "N product frames unavailable"; the governor line "<product> not kept (no live fill: its readers use their own fallback)" |
| Declaration overflow (`ProductDecl::overflow`, `ConsumerDecl::overflow`) | `AddInput`/`AddLead`/`AddConsume` past kMaxInputs/kMaxConsumes record nothing and set the flag (debug builds also assert); `Registry::Validate` / `BuildGraph` reject it | fail closed in Release, never a silently dropped read | "a product declares more inputs than the seam allows", "a consumer declares more reads than the seam allows" on the analysis_lane Fallbacks entry and the status lines |
| Governed history (`ProductDecl::historyFloor`) | `history` is the full depth; the product runs at any depth in [historyFloor, history] (never below the largest lag read). `PlanTiers` adds one governor level per depth AFTER the lossless lane drop (lane -> in render, then history steps; the user ranks as lossy) | `TierPlan::historyCaps` -> `GraphSettings::historyCaps` -> rebuild (`AnalysisLane::Configure` does it once, the grant persists so a repeated Configure does not flap); ring and live pool shrink by the shed frames | `Video memory: <product> keeps N of M past frames of history (not enough free video memory)` on the status lines; `Graph::HistoryReducedNodes()`; `TierEntry::historyGranted` |

### Lead reads of an ordered chain (S2 / reset-2 design options)

**What happens today.** An ordered-chain product C (reads itself at lag >= 1) pins only a RENDER-VALID predecessor (Sealed / Live), so its job for frame
M waits for the seal of M-1 and C runs at most one frame ahead of its seal. A reader R at lead k >= 1 needs C(N+k) PUBLISHED when R(N) is sealed; for
chain targets that is not the case, so R(N) is rejected at its seal and live-filled in its declared reduced form (counted, visible). In the catalog
this is quality_pattern (P15) reading coding_evidence (P16) at lead 1 and 2: the role-at-lead-2 estimate always runs reduced when it is a lead reader of
the chain. It is correct (a visible reduced form, nothing wrong is used), but the lead is wasted. `Registry::Validate(&why, &warning)` now reports the
declaration as a WARNING (never a reject), `Graph::leadChainReads` the effective reads, and the analysis status lines / Fallbacks list show it.

**Options for S2 / reset 2** (not decided here; each needs the lead's / owner's choice):

1. **Drop the chain on coding_evidence** (recommended: fewer pathways, smallest change to the seal). Its self lag-1 read exists only for the hash
   history (copy flags against N-1 / N-2). Move the hashes into a small single-store (or a lag-1 view of P13) read at lag 0 / 1 and make coding_evidence
   a plain lane product with no self read: lead readers then pin a Published target as any other lead edge. Cost: the hash store needs its own single
   writer ordering and its own absent / reduced form (a copy flag of "unknown" at the chain start, as today).
2. **Re-anchor on a replaced predecessor** (keep the chain, let it run ahead). Allow a chain job to pin a Published-unsealed predecessor and let the seal
   accept a successor whose recorded predecessor was REPLACED by a live fill when the job is marked "re-anchored" (carry-forward from the new cgen)
   instead of rejecting with `AncestorReplaced`. Cost: a seal rule change in `SlotTable::Check`, a declared reduced form that survives a replaced
   predecessor, and a counter for the re-anchors; the cascade that the render-valid-only pin removed (a permanent per-frame live fill) must be shown not
   to return (test: reject-and-live-fill the predecessor, then check the next 100 frames).
3. **Declare the lead honestly.** Give quality_pattern the lead the chain supports (lead 1, or the causal P15c as its default) and keep the chain. Cost:
   a later role estimate (less look-ahead for the anchor decision); no seal change. Measure first: the counters `chainCarryForward`, `leadAccepted` and
   `leadLaterRejected` say how often the lead read actually runs reduced.

Until one is chosen the warning stays; no behaviour changes.

## Status and fallbacks

Governed history reductions and chain carry-forwards are on the analysis status lines (Ctrl+I) as above; a product with a
reduced history is a non-identical reduced mode and must declare `Variants::NonIdentical` and handle `Node::history`.

## Tests

`SkeletonGraphTests` (new), `ProductSlotsTests`, `ProductGraphTests`, `VramGovernorTests`, `AnalysisLaneTests`,
`LaneProductsTests`, `PipelineSkeletonTests`, `SkeletonModuleTests`, `ProductCatalogTests`, `CameraProductsTests`.
CPU only: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:SkeletonGraphTests,... /Logger:trx`.

