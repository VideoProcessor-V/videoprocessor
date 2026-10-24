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
| `EntranceStages.h` | The input side as one stage list plus the per-source-frame cache (barrier B3). Generic over the frame type, so it is tested on the CPU |
| `OutputHookOrder.h` | The corrected output-hook order (ADR-0013). The renderer uses `OrderSlotsInPlace`, which moves only slot hooks |
| `VectorScale.h` | Per-axis vector scaling (ADR-0006) |
| `DelayBudget.h` | The one user-set total delay: frames = floor((total - fixed) / period), the 4/3/2 ladder, padding (ADR-0020) |
| `SlotTiming.h` | Per-slot GPU timing and the projected 3090 result (barrier B4) |
| `FallbackStatus.h` | "Fallbacks engaged: (list) / none" (ADR-0001). Built on fix/r6-baseline and moved here |

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
