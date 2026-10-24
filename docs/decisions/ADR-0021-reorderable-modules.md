# ADR-0021: Reorderable modules, with declared contracts and a validator

- Status: Accepted
- Date: 2026-10-24 (owner requirement, queue item 65)
- Slots: every pixel-path slot (src/VP-V/core: SlotContracts.h `ContractOf`, OrderValidator.h `Validate`)

## Context

The control program places modules in a defined order. Because the pipeline is modular, the
owner wants to be able to move a module to another position. Uses:

- experiments, such as re-base before or after repair, or the deband position;
- future modules;
- tests: the pairwise generator can include order variants.

A move must never produce a pipeline that silently does something wrong.

## Decision

1. **The order is data.** The designed order (`slots.json` `order`, the `PipelineSlots.h`
   table) is the default. A custom order comes from the setting `pipeline_order`: a list of slot
   names, separated by `,`, space or `>`. The listed slots take the positions they occupy now, in
   the listed order.
   - Example: `repair.encoding, scaling.rebase` swaps the two.
   - An empty setting gives the designed order.
2. **Each module declares a contract** (`ContractOf`):
   - the resolution domain it needs and gives: delivered / encoded / processing / output;
   - the light/colour domain: source signal / target light / projector codes;
   - the plane layout: subsampled / full chroma;
   - whether it needs or produces motion vectors, needs the untouched original frame, or
     modifies the picture;
   - whether it may move at all.
3. **Zones:**
   - Host: outside the pixel path.
   - VP front end.
   - libplacebo-internal: fixed, because its hooks allow no move.
   - VP output hooks.
   - Quantiser.
   - Presentation.

   A module moves only within its zone. A fixed module keeps its place relative to every other
   slot in its zone.
4. **Hard precede rules:**
   - LED measurement before LED compensation;
   - LED compensation before black level;
   - SDR tone shape before Stage 13.
5. **The validator** (`Validate`) accepts or rejects a whole order and gives a plain-language
   reason, for example "repair.deband needs the delivered or encoded resolution, but at that
   position it gets the processing resolution (after scaling.route_4k)".
   - Validity is checked with every slot assumed on, so an order never becomes invalid just
     because a slot is switched on.
   - An invalid order never runs. The designed order is kept, and "Fallbacks engaged:
     pipeline order (designed order kept: reason)" is shown.
6. **Net fingerprints include the upstream order** (`Registry::UpstreamKey`: the running pixel-
   path slots before the net, in order). A net trained on another key is bypassed, visibly under
   "Fallbacks engaged". Moving, adding or removing an upstream module therefore bypasses the
   dependent nets instead of feeding them an input they never saw.
7. The renderer follows the order in both places:
   - the entrance stage list (`RunEntrance` walks `ActiveEntrance()`);
   - the output slot hooks (`OutputHookOrder::OrderSlotsInPlace` with the registry's ranks).

## Consequences / open

- The Original pathway (ADR-0019) always uses the designed order.
- Contracts are declared in C++ (`src/VP-V/core/SlotContracts.h`) and covered by tests:
  - a valid reorder works (the stage list follows it);
  - invalid reorders are rejected, each with its reason;
  - a move changes the downstream fingerprint keys.

  `slots.json` carries each slot's zone for the settings/test generators.
- Stateful stages that advance once per source frame on A (the debander hold, the detail
  history) process A through the stages before them, whatever the order.
- Config UI for the order (a reorder list with the validator's reason) belongs to the settings
  agent.
