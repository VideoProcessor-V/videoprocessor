# ADR-0022: All VP-V code under one source folder, src/VP-V

- Status: Accepted
- Date: 2026-10-24 (owner direction)
- Slots: all (code location)

## Context

VP-V's own code was spread through Bill's project folders, mostly `src/VideoProcessor-Lib/vprenderer`.
That made it hard to see what is VP-V and what is upstream, and hard to lift a module into another
program. The skeleton was the cheapest moment to fix it.

## Decision

- All VP-V code lives under `src/VP-V/`:
  - `core/` (the pipeline skeleton);
  - `modules/<unit>/<slot>/` (module docs, manifests, tests, migrated code);
  - `nets/` (the net runner and fingerprints);
  - `config/` (the VP-V Config section);
  - `tests/` (VP-V unit tests, compiled by `VideoProcessor-Test` under the filter "VP-V").
- Bill's code stays where it is. It reaches VP-V only through MINIMAL, clearly commented
  `#include <VP-V/...>` hooks.
- Includes go from the `src` root (`<VP-V/core/SlotRegistry.h>`). The root `Directory.Build.targets`
  adds `src` to every project's include path.
- Each VP-V folder has a README saying what lives there and how it connects. The skeleton is the
  reference example for future modules: why-comments, small focused units, consistent naming,
  no magic numbers.
- Legacy VP code still migrates gradually (ADR-0017): one module per change, `git mv`, with the
  manifests and the master list in the same commit, and no duplicate source (the drift check
  enforces it).

## Consequences

- Moved now: the skeleton headers, `FallbackStatus.h` (from fix/r6-baseline, includes updated
  in the GUI, renderer and tests), the module tree, the VP calibration-LUT stage and the
  skeleton tests.
- The shared headers stay C++14 for the test project.
