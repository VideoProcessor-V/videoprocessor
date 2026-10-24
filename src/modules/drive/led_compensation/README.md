# LED compensation (adaptive gamma) (`drive.led_compensation`)

Unit: **drive** | side: output | order: 230 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

Compensate the picture for the LED drive change so content light stays constant; CNTR never above need.

## Contract

- Input: target-light picture
- Output: compensated picture (BTB/WTW passthrough parameters)
- Geometry: output size
- Delay share: aligned to the LED latency
- Look-ahead: LED look-ahead

## Settings

- Existing keys: `led_compensation`
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: LED compensation: off (shader did not load)
- Behaviour: Failure -> drive held at full, no compensation.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `drive.led_measure`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- LED compensation shader tests

Contract tests:
- CNTR never above need

Chain tests:
- LED + Stage 13 + LUT + black level

## Known issues / open items

- (none yet)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `shaders/LED compensation.glsl`
- `src/VideoProcessor-Lib/led/LedDriveModule.cpp`
- `src/VideoProcessor-Lib/led/LedDriveArbiter.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../docs/decisions/README.md)
