# Projector control (`drive.projector_control`)

Unit: **drive** | side: control | order: 900 | status: **built** | hook point: host

## Purpose / why

RS-232 projector driver: LED (LPW1) + CNTR commands, script intent / internal script / compensate modes, latency timing.

## Contract

- Input: LED drive decisions
- Output: serial commands
- Geometry: n/a
- Delay share: led_latency_ms aligns commands with the picture
- Look-ahead: LED look-ahead

## Settings

- Existing keys: `led_compensation`
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Projector: not connected (LED drive off)
- Behaviour: No serial projector -> drive off.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- BarcoAsciiDriver tests
- LedController tests

Contract tests:
- (none yet)

Chain tests:
- (none yet)

## Known issues / open items

- separate CNTR latency if measured different (phone slow-mo)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/led/LedController.cpp`
- `src/VideoProcessor-Lib/led/BarcoAsciiDriver.cpp`
- `src/VideoProcessor-Lib/led/ProjectorDriver.cpp`
- `src/VideoProcessor-Lib/led/SerialPort.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../docs/decisions/README.md)
