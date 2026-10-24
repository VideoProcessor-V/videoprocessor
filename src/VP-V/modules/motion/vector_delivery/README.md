# Vector delivery (`motion.vector_delivery`)

Unit: **motion** | side: processing | order: 130 | status: **partial** | hook point: host

## Purpose / why

Hand source-size vectors to consumers at the processing size: PER-AXIS scaling (re-base/4K route give different x/y factors), mapping through crop/A-lens/NLS, and an optional +-1 px local refinement for precision consumers.

## Contract

- Input: vectors at source size + per-axis factors + geometry
- Output: vectors in the consumer's coordinate space (scale x, scale y separately)
- Geometry: source -> processing -> output, per axis
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: (none)
- Planned keys: (none)
- Tiers: follows motion.estimation

## Fallbacks

- "Fallbacks engaged" entry: Vectors: invalid (<reason>)
- Behaviour: Invalid geometry map -> vectors invalid -> consumers fall back.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `motion.estimation`, `scaling.rebase`, `scaling.route_4k`, `scaling.geometry`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- per-axis scaling unit tests (VectorScale)

Contract tests:
- sx != sy maps exactly; identity factors leave vectors unchanged

Chain tests:
- single-axis re-base + de-judder

## Known issues / open items

- +-1 px refinement for temporal dither / scaler refinements / texture restore

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/core/VectorScale.h`
- `src/VideoProcessor-Lib/vprenderer/DejudderPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/DejudderWarp.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)
