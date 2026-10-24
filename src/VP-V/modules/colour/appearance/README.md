# Stage 13 appearance (CAM16) (`colour.appearance`)

Unit: **colour** | side: output | order: 210 | status: **placeholder** | hook point: PRE_OUTPUT

## Purpose / why

CAM16 Stevens/Hunt appearance matching from reference to dim-screen conditions (static per viewing setup), replacing the manual gamma boost + saturation. Off by default; on after the i1Pro screen-white measurement.

## Contract

- Input: linear target RGB at output size
- Output: same domain; BTB/WTW codes pass through unchanged
- Geometry: output size
- Delay share: 0 frames; a few dozen ALU ops per pixel
- Look-ahead: none

## Settings

- Existing keys: (none)
- Planned keys: `appearance_adaptation`, `appearance_strength`, `appearance_reference_white_nits`, `appearance_reference_surround`, `appearance_screen_white_nits`, `appearance_screen_surround`
- Tiers: every tier when on (cost ~0)

## Fallbacks

- "Fallbacks engaged" entry: Appearance: off (shader failed)
- Behaviour: Failure -> off, previous gamma boost/saturation restored.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.sdr_tone_shape`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- byte-identical with the slot off
- zero hooks when off

Isolation tests:
- identity at equal conditions; reference CAM16 grey ramp / primaries / skin; monotonic J; hue within 1 degree

Contract tests:
- BTB/WTW probe codes identical on and off

Chain tests:
- constant output across synthetic LED dimming with compensation on

## Known issues / open items

- one owner per correction with the ambient light sensor (no double correction)

## Where the code lives (live file list)

Code location: **none (placeholder)**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- (none yet)

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0014 in [docs/decisions](../../../../../docs/decisions/README.md)
