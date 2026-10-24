# Motion consumers (`motion.consumers`)

Unit: **motion** | side: processing | order: 140 | status: **built** | hook point: prepass

## Purpose / why

De-judder, hiccup repair, optional interpolation (exclusive with libplacebo smooth motion), and the temporal consumers (dither vectors, scaler refinements).

## Contract

- Input: pictures at the processing size + delivered vectors + validity
- Output: the picture for this refresh's phase
- Geometry: processing size
- Delay share: 0 extra frames; uses the shared queue
- Look-ahead: de-judder pair (1), estimate-ahead (2), hiccup spare (1) - all degrade on a short queue

## Settings

- Existing keys: `dejudder_strength`, `dejudder_hold`, `interpolation`, `interpolation_fallback`, `smooth_motion`, `frame_mixer`, `dither_vectors`
- Planned keys: (none)
- Tiers: Classic off .. Upper full

## Fallbacks

- "Fallbacks engaged" entry: Motion consumers: no-motion form (vectors invalid / short queue)
- Behaviour: Each consumer falls back as the vector rules say.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `motion.vector_delivery`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- DejudderWarp tests
- InterpolationTimeline tests
- PulldownDetector tests

Contract tests:
- exclusive with libplacebo smooth motion by profile rules

Chain tests:
- 24p in 60 at 200 ms, 4/3/2 frames

## Known issues / open items

- (none yet)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/DejudderWarp.cpp`
- `src/VideoProcessor-Lib/vprenderer/DejudderAhead.h`
- `src/VideoProcessor-Lib/vprenderer/RenderedWarp.cpp`
- `src/VideoProcessor-Lib/vprenderer/FrameInterpolator.cpp`
- `src/VideoProcessor-Lib/vprenderer/InterpolationTimeline.h`
- `src/VideoProcessor-Lib/vprenderer/InterpolationPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/OcclusionMaskHook.cpp`
- `src/VideoProcessor-Lib/vprenderer/TemporalRenderer.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)
