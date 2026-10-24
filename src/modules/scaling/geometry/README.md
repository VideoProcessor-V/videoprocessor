# Geometry (`scaling.geometry`)

Unit: **scaling** | side: processing | order: 150 | status: **built** | hook point: libplacebo

## Purpose / why

Crop/active picture, A-lens (anamorphic), NLS, zoom/viewport.

## Contract

- Input: processing-size picture + active area
- Output: source and target rectangles for libplacebo; NLS hook
- Geometry: defines output active size (feeds the processing-size rule)
- Delay share: 0 frames
- Look-ahead: active-picture decisions use queued frames

## Settings

- Existing keys: `nls`, `nls_active`, `nls_curve`, `nls_geometry`, `nls_protect`, `nls_scale`
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Geometry: NLS safe fit (hook failure)
- Behaviour: NLS failure redraws with the linear safe fit (already in code).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.source_analysis`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- NlsGeometryPolicy tests
- AnamorphicPresentation tests

Contract tests:
- active area propagated to every net slot

Chain tests:
- A-lens/NLS + re-base + vectors

## Known issues / open items

- (none yet)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/NlsGeometryPolicy.cpp`
- `src/VideoProcessor-Lib/vprenderer/NlsMaskMapping.h`
- `src/VideoProcessor-Lib/vprenderer/NlsHookRecovery.h`
- `src/VideoProcessor-Lib/vprenderer/AnamorphicPresentation.h`
- `src/VideoProcessor-Lib/vprenderer/AlphaSourceCropPolicy.cpp`
- `shaders/NLS.glsl`
- `shaders/NLSPlus.glsl`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../docs/decisions/README.md)
