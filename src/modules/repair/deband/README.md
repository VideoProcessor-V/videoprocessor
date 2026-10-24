# Deband (`repair.deband`)

Unit: **repair** | side: input | order: 80 | status: **built** | hook point: prepass

## Purpose / why

Remove banding at source resolution, after repair (cheaper and correct for source-resolution bands).

## Contract

- Input: planes at the processing-entry size (encoded or delivered)
- Output: same size and format; grain guard keeps grain
- Geometry: same as input
- Delay share: 0 frames
- Look-ahead: slot hold uses the current frame only

## Settings

- Existing keys: `deband_vp`, `deband_vp_exclusive`, `deband_vp_grain_guard`, `deband`, `deband_strength`, `deband_grain`
- Planned keys: (none)
- Tiers: Classic libplacebo deband; Low..Upper VP deband

## Fallbacks

- "Fallbacks engaged" entry: Deband: libplacebo deband (VP debander idle: <reason>)
- Behaviour: VP debander failure renders without it; temporal frames keep libplacebo's deband.

## Net fingerprint / upstream

- Fingerprint: deband on/off is an unlabelled robustness input to the refine net
- Upstream modules: `repair.encoding`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- VpDebander GPU tests
- real-pan shimmer x52 (queue item 1)

Contract tests:
- source-resolution run

Chain tests:
- deband + re-base + 4K route

## Known issues / open items

- NEED with A on (does A remove most banding?) - decide from the results
- x52 pan shimmer on 1080->1600 (item 1)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/VpDebander.cpp`
- `src/VideoProcessor-Lib/vprenderer/VpDebander.h`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: ADR-0009 in [docs/decisions](../../../../docs/decisions/README.md)
