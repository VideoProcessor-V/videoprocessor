# LED measurement (`drive.led_measure`)

Unit: **drive** | side: output | order: 220 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

Measure the picture's light (pre-compensation content, after Stage 13) for the LED drive; peaks picture-only, full coverage.

## Contract

- Input: target-light picture at output size
- Output: unchanged picture + measurement readback
- Geometry: picture area only (bars excluded)
- Delay share: LED latency ~80 ms (time-based, inside the queue)
- Look-ahead: LED look-ahead (ms-based)

## Settings

- Existing keys: `led_compensation`, `led_protect`
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: LED: drive held (measurement unavailable)
- Behaviour: Holdover keeps the last drive.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.appearance`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- LedMeasurement tests
- LED correlation harness

Contract tests:
- measures after Stage 13, before compensation

Chain tests:
- LED + subtitles at a 2-frame queue

## Known issues / open items

- (none yet)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/led/LedMeasurement.cpp`
- `src/VideoProcessor-Lib/led/LedMeasurementHook.cpp`
- `src/VideoProcessor-Lib/led/LedLookahead.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../docs/decisions/README.md)
