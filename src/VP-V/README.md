# VP-V

All VP-V code lives in this folder (owner 2026-10-24). Bill's VideoProcessor code stays where it
is, in `src/VideoProcessor-*`. It reaches into VP-V only through a few clearly commented hooks,
each an `#include <VP-V/...>` with a comment naming the folder.

| Folder | What lives there | How it connects |
|---|---|---|
| [`core/`](core/README.md) | The pipeline skeleton: the slot table, module contracts and the order validator, the slot registry, the entrance stage list, the output-hook order, per-axis vector scaling, the delay budget, per-slot timing, and "Fallbacks engaged" | The renderer (`LibplaceboVideoRenderer.cpp`) includes `core/SlotRegistry.h`, `core/EntranceStages.h` and `core/OutputHookOrder.h`. The GUI and the renderer include `core/FallbackStatus.h` |
| [`modules/`](modules/README.md) | One folder per pipeline slot (`<unit>/<slot>/`): module doc, `MANIFEST.json` with the live file list, `tests/`, and the slot's code once it has migrated | The renderer project compiles migrated module code (today `modules/output/calibration_lut/CalibrationLutStage.cpp`) |
| [`nets/`](nets/README.md) | The VP-side net runner and fingerprints (not built yet) | Will serve `repair.encoding`, `scaling.refine` and the hybrid scaler's D |
| [`config/`](config/README.md) | The VP-V Config section (the settings agent builds it, self-contained) | Generated from `docs/design/slots.json` |
| [`tests/`](tests/README.md) | VP-V unit tests | Compiled by `VideoProcessor-Test` (filter "VP-V") |

## Conventions (the reference example for future modules)

- **Includes** go from the `src` root: `#include <VP-V/core/SlotRegistry.h>`. The root
  `Directory.Build.targets` adds `src` to every project's include path.
- **Shared headers are C++14.** The test project compiles as C++14, so they use no
  `std::optional`, `string_view` or inline variables.
- **Why-comments, not what-comments.** Each file starts with why it exists and which design
  decision it implements (`docs/decisions/ADR-*`).
- **Small focused units:** one job per header.
- **No magic numbers:** derive from measured rates (ADR-0018).
- **One fallback pattern:** "Fallbacks engaged" (`core/FallbackStatus.h`).
- **Slot data:** `docs/design/slots.json` is authoritative for slot ids, order, zones and keys.
  `core/PipelineSlots.h` mirrors it, and `python tools/check_module_manifests.py` fails on any
  drift.
- **Migration:** legacy code moves here one module at a time, only when touched (ADR-0017).
  Each move is a `git mv` plus the project, include, manifest and master-list updates in the
  same commit.

## Where to start reading

1. `docs/design/VP-pipeline-final.md`: the design, by units.
2. `core/README.md`: how the skeleton runs a frame.
3. `modules/<unit>/<slot>/README.md`: one slot's contract.
