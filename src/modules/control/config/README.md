# Config + tiers (`control.config`)

Unit: **control** | side: control | order: 930 | status: **partial** | hook point: host

## Purpose / why

Per-unit Classic/Low/Mid/Upper tier sliders + global, toggles listed beneath, unbuilt slots shown as 'not yet implemented'; settings preview exe.

## Contract

- Input: slots.json + config schema
- Output: Config UI + validated settings
- Geometry: n/a
- Delay share: n/a
- Look-ahead: n/a

## Settings

- Existing keys: (none)
- Planned keys: (none)
- Tiers: n/a

## Fallbacks

- "Fallbacks engaged" entry: (n/a)
- Behaviour: n/a

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- ConfigFileTests
- ConfigEditorWindowTests

Contract tests:
- every planned key exists once the settings agent lands

Chain tests:
- (none yet)

## Known issues / open items

- settings reference guide (item 54)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/PipelineSlots.h`
- `src/VideoProcessor-Lib/vprenderer/EntranceStages.h`
- `src/VideoProcessor-Lib/vprenderer/OutputHookOrder.h`
- `src/VideoProcessor-Test/PipelineSkeletonTests.cpp`
- `src/VideoProcessor-Config/ConfigEditorWindow.cpp`
- `src/VideoProcessor-Lib/ConfigFile.cpp`
- `src/VideoProcessor-Lib/ConfigSchema.h`
- `src/VideoProcessor-Lib/MainConfigSchema.h`
- `src/VideoProcessor-Lib/RendererProfileConfig.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: ADR-0015 in [docs/decisions](../../../../docs/decisions/README.md)
