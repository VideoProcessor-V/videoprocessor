# BTB/WTW preservation (`colour.extended_range`)

Unit: **colour** | side: processing | order: 175 | status: **built** | hook point: libplacebo

## Purpose / why

Keep below-black and above-white codes through every conversion (owner rule: both kept in all cases). Cross-cutting: VP relabels the source and its hooks do the conversion.

## Contract

- Input: picture + source/target colour
- Output: same, with BTB/WTW carried
- Geometry: n/a
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: `preserve_extended_range`, `extended_range_more_cases`
- Planned keys: (none)
- Tiers: always on

## Fallbacks

- "Fallbacks engaged" entry: BTB/WTW: clipped (<reason>)
- Behaviour: Hook failure or libplacebo frame mixing clips BTB (today log-only).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.tone_gamut`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- ExtendedRange tests

Contract tests:
- BTB/WTW probe codes identical with every output slot on and off

Chain tests:
- chain gate BTB/WTW probes

## Known issues / open items

- (none yet)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/ExtendedRange.cpp`
- `src/VideoProcessor-Lib/vprenderer/ExtendedRange.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: ADR-0002 in [docs/decisions](../../../../docs/decisions/README.md)
