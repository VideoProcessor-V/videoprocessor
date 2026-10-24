# Black level correction (`drive.black_level`)

Unit: **drive** | side: output | order: 240 | status: **placeholder** | hook point: PRE_OUTPUT

## Purpose / why

Compensate the projector black floor as LED/CNTR change (delta 11 toe / ANSI lift interplay), after compensation, before the calibration LUT.

## Contract

- Input: compensated target-light picture + current LED/CNTR state
- Output: same domain
- Geometry: output size
- Delay share: aligned to the LED latency
- Look-ahead: none

## Settings

- Existing keys: (none)
- Planned keys: `black_level_correction`
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Black level: off (<reason>)
- Behaviour: Off = pass-through.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `drive.led_compensation`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- byte-identical with the slot off
- zero hooks when off

Isolation tests:
- (none yet)

Contract tests:
- dark-hair scene check

Chain tests:
- (none yet)

## Known issues / open items

- design not yet specified beyond the position

## Where the code lives (live file list)

Code location: **none (placeholder)**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- (none yet)

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)
