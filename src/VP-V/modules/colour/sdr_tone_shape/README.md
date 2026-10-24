# SDR tone shape + DV trims (`colour.sdr_tone_shape`)

Unit: **colour** | side: output | order: 200 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

Delta 13's SDR tone shape and headroom, and the (non-certified) DV trims; first at PRE_OUTPUT.

## Contract

- Input: target-light picture at output size
- Output: same domain
- Geometry: output size
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: `sdr_tone_shape`, `dv_trims`
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: SDR tone shape: not applied (shader did not load)
- Behaviour: Shader failure skips it.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.tone_gamut`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- SdrToneShape tests
- DolbyVisionTrims tests

Contract tests:
- output encoding repair present when a PRE_OUTPUT hook runs

Chain tests:
- (none yet)

## Known issues / open items

- (none yet)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/SdrToneShape.h`
- `src/VideoProcessor-Lib/vprenderer/DolbyVisionTrims.h`
- `shaders/SDR tone shape.glsl`
- `shaders/DV trims.glsl`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0002 in [docs/decisions](../../../../../docs/decisions/README.md)
