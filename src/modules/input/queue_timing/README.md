# Queue + timing (`input.queue_timing`)

Unit: **input** | side: input | order: 30 | status: **built** | hook point: host

## Purpose / why

Hold a FIXED, known delay (lip sync with a fixed audio delay): 200 ms target, 250 ms margin; 4 queued frames with a graceful 2-3 frame fallback; cadence/pulldown detection and the film clock; stale-burst handling.

## Contract

- Input: captured frames with timestamps
- Output: frames released at a constant end-to-end delay, with cadence/film-clock phase
- Geometry: n/a
- Delay share: TOTAL-DELAY BUDGET: queue frames = floor((budget - fixed costs) / frame period), ladder 4/3/2; fixed costs = swapchain depth + render-ahead + processing (+ capture transfer if the budget covers it). Pending owner O1/O3.
- Look-ahead: provides the shared look-ahead window: queue depth = deepest single look-ahead + hiccup spare (features share, never add)

## Settings

- Existing keys: `target_latency_ms`, `queue_size`, `queue_timing_fixes`, `queue_recovery`, `queue_auto_reset`, `pulldown_fixes`, `low_latency`, `low_latency_path`
- Planned keys: (none)
- Tiers: not tiered; 200 / 250 ms selectable

## Fallbacks

- "Fallbacks engaged" entry: Queue: short look-ahead (<n> of 4 frames)
- Behaviour: With 2-3 frames available, every look-ahead consumer shortens its look-ahead or falls back; the delay never changes.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.capture`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- QueueStressHarness
- H3TimingTests
- film clock tests

Contract tests:
- queue minimum follows the configured/time depth (no forced 5-frame minimum)

Chain tests:
- delay gate: 24p at 200 and 250 ms with 4/3/2 frames; repeat at 25/30/50/60p; constant delay, no stalls, recovery when frames return

## Known issues / open items

- remove the forced 5-frame minimum (fix/r6-baseline)
- motion_ahead time-based (fix/r6-baseline)
- render-ahead inside the delay budget (fix/r6-baseline)
- QueueLatency.h rounds UP (ceil) today: 200 ms at 23.976 = 5 frames (barrier B2)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/QueueLatency.h`
- `src/VideoProcessor-Lib/QueueConfiguration.h`
- `src/VideoProcessor-Lib/QueueLookahead.h`
- `src/VideoProcessor-Lib/CaptureFrameQueue.h`
- `src/VideoProcessor-Lib/VideoTimingController.cpp`
- `src/VideoProcessor-Lib/TimingClock.cpp`
- `src/VideoProcessor-Lib/UnifiedProfileRuntime.cpp`
- `src/VideoProcessor-Lib/vprenderer/PulldownDetector.h`
- `src/VideoProcessor-Lib/vprenderer/AlphaQueuePolicy.h`
- `src/VideoProcessor-GUI/VideoProcessorDlg.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../docs/design/slots.json)
- Decisions: ADR-0003 in [docs/decisions](../../../../docs/decisions/README.md)
