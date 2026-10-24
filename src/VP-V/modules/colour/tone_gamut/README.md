# Tone + gamut map (`colour.tone_gamut`)

Unit: **colour** | side: processing | order: 170 | status: **built** | hook point: libplacebo

## Purpose / why

libplacebo tone/gamut mapping (keeps all of its colour science) OR, for low-end cards, the combined static HDR->SDR 3D LUT in the same slot.

## Contract

- Input: output-size picture in the source colour space
- Output: picture in the target colour space (linear target light needed by LED compensation)
- Geometry: output size
- Delay share: 0 frames
- Look-ahead: peak detection (off keeps DTM off)

## Settings

- Existing keys: `tone_mapping`, `tone_mapping_tuning`, `gamut_mapping`, `peak_detection`
- Planned keys: `combined_hdr_lut`
- Tiers: Classic/Low may use the combined LUT; Mid/Upper libplacebo

## Fallbacks

- "Fallbacks engaged" entry: Tone map: libplacebo (combined LUT missing)
- Behaviour: Missing/invalid combined LUT -> libplacebo tone mapping.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `scaling.output`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- libplacebo gamut probe

Contract tests:
- combined LUT must output linear light or teach compensation the display response (ADR-0013)

Chain tests:
- LED + Stage 13 + LUT + black level combo

## Known issues / open items

- combined LUT vs LED compensation catch (decide when building the LUT option)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.cpp`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboOutputPolicy.cpp`
- `src/VideoProcessor-Lib/ToneMappingTuning.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013, ADR-0002, ADR-0016 in [docs/decisions](../../../../../docs/decisions/README.md)
