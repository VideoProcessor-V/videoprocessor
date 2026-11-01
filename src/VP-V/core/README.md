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
| `FrameAnalysisValidity.h` | `VpFrameAnalysis::RecordValid(indicator, recordSeq, useSeq, mask)`: per-record temporal validity from the Indicator's stamps (FrameAnalysisStore itself unchanged; no private frame history added) |
| `Products.h` | Skeleton S0 registry: product declarations (inputs with lag, backend, history, size, honoured counters, tier, residency, variants) and the first products P0-P6 (+ P2m matcher pair); component consumer declarations |
| `ProductGraph.h` | Skeleton S0 graph: the closure of what active consumers need (each product once, topological, deterministic), fixed ring and live-pool counts, rebuild triggers, `GraphOwner` (advances the Indicator's `graph` counter), Indicator ring sizing (N6) |
| `ProductSlots.h` | Skeleton S1 slots: the 64-bit state word, ABA-proof generation-tagged claim, lane/live headers, lane-owned rejected buffers, live pool for every backend, lane input pins, the seal (stamps, span, ancestry cgens) and the cascade (`SlotTable::Resolve`) |
| `CardProfile.h` | Skeleton S2: card profile (cached per adapter LUID + driver), adaptive VRAM headroom, system-RAM rules |
| `VramGovernor.h` | Skeleton S2: the ONE VRAM governor and the tier plan on it. Every VRAM user (early-upload pool, render-ahead, vector engine, TemporalDetail, camera model, interpolator, enhancers, fixed libplacebo/capture memory, and the skeleton products) registers levels + tier; one budget (own usage re-planned), one shed order (tier, lossless first, order); one status line per cutback. `PlanTiers` = T0 first then T1/T2 by ms saved per MB as that order; `ApplyLaneGrants` sets the slots |
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
