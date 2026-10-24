# Motion estimation + vector store (`motion.estimation`)

Unit: **motion** | side: input | order: 50 | status: **built** | hook point: prepass

## Purpose / why

Estimate motion on the ORIGINAL frame at SOURCE size (NVOF seed + VP matcher) with a time-based look-ahead, and keep ONE vectors-valid state that every consumer obeys (invalidated at cuts, drops, repeats, drift, resyncs).

## Contract

- Input: original frame luma at source (delivered) size; queued next frames
- Output: vector fields at source size + validity state; forward/backward as configured
- Geometry: source size; consumers map through motion.vector_delivery
- Delay share: 0 extra frames: motion ahead is time-based inside the queue (~67 ms)
- Look-ahead: motion_ahead (time-based, falls back to the matcher on a short queue)

## Settings

- Existing keys: `motion_engine`, `motion_ahead_frames`, `motion_history_fixes`, `dejudder_est`, `dejudder_estimate_ahead`, `dejudder_search_range`
- Planned keys: `motion_ahead_ms`
- Tiers: Classic off .. Upper full (NVOF + matcher)

## Fallbacks

- "Fallbacks engaged" entry: Motion: matcher only (NVOF unavailable: <reason>)
- Behaviour: NVOF rejected/absent -> VP's matcher; vectors invalid -> every consumer falls back to its no-motion form.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.queue_timing`, `input.source_analysis`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- NvOpticalFlow tests
- DejudderWarp estimator tests
- MotionValidity tests

Contract tests:
- vectors estimated at source size, never on repaired/re-based pictures

Chain tests:
- vector validity across cuts/drops at 24/30/60p

## Known issues / open items

- A_motion needs these vectors BEFORE it runs (forced order)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/NvOpticalFlow.cpp`
- `src/VideoProcessor-Lib/vprenderer/NvOpticalFlow.h`
- `src/VideoProcessor-Lib/vprenderer/DejudderWarp.cpp`
- `src/VideoProcessor-Lib/vprenderer/MotionAheadProducer.cpp`
- `src/VideoProcessor-Lib/MotionAhead.h`
- `src/VideoProcessor-Lib/VectorStore.h`
- `src/VideoProcessor-Lib/MotionValidity.h`
- `src/VideoProcessor-Lib/OpticalFlowTrust.h`
- `src/VideoProcessor-Lib/MotionConfig.h`
- `src/VideoProcessor-Lib/vprenderer/MotionEngineIdle.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0006 in [docs/decisions](../../../../../docs/decisions/README.md)
