# Queue + timing (`input.queue_timing`)

Unit: **input** | side: input | order: 30 | status: **built** | hook point: host

## Purpose / why

Hold ONE user-set total delay (lip sync with the AVR's fixed audio delay), the SAME at every frame rate (padded); 4 queued frames recommended (about 267 ms at 23.976 on 60 Hz, measured by the delay gate) with a graceful 4/3/2 ladder; cadence/pulldown detection and the film clock; stale-burst handling.

## How the code works

Checked against the code at M2 step 18a (2026-10-25). Two forms exist on this branch:

- **(a) Today's host path.** The renderer decides inline. VP's queue rules are still inside Bill's `vprenderer/AlphaQueuePolicy.h` (about 240 VP lines: `CombineQueueTarget`, `ShouldShedHeldBacklog`, `TimelineQueueSteering`, ...). `VpQueueRules.h` holds a verbatim copy of them that nothing in the build uses yet.
- **(b) The policy interface (pending).** `docs/design/motion-timing-callsites.patch` (described in `docs/design/motion-timing-callsites.md`) restores `AlphaQueuePolicy.h` to upstream 83b7ffea and makes the host call one `QueueTimingPolicy::IQueueTimingPolicy`. Until it is applied, `AlphaQueuePolicyVpCompat.h`, `VpFixedBudgetPolicy.h` and `QueueTimingPolicyTests.cpp` are not compiled: the shim's using-declarations clash with the VP functions still defined in `AlphaQueuePolicy.h`, and the test file is not in `VideoProcessor-Test.vcxproj` yet.

### Public API

- `DelayBudget` (src/VP-V/core/DelayBudget.h): `Inputs`, `FixedCostsMs`, `Compute` -> `Plan` (`queueFrames`, `feasible`, `degraded`, `fixedMs`, `paddingMs`, `holdMs`, `queueTargetFrames`, `holdFraction`), `WaitAfterTimestampMs`, `QueueCapacityFor` / `HoldForCapacity`, `RecommendedBudgetMs`. Constants: `kDesignDepth` 4, `kMinimumDepth` 2, `kSwapchainFrames` 2 (also the renderer's `max_frame_latency`), `kProcessingAllowanceMs` 8.
- `DelayBudget` helpers (src/VP-V/core/DelayBudgetLadder.h): `Model`, `InputsFor` (the capture transfer is one signal frame), `StandardRates`, `BudgetForFrames` (the 4/3/2 ladder totals), `LevelName`, `TotalMs`. Used by the VP-V Config delay card (src/VP-V/config/VpvSection.cpp), the delay gate and the combination runner.
- `QueueLatency` (QueueLatency.h): the frame-count form of `target_latency_ms`. `ResolveTargetFrames` (precedence: own latency, own frames, inherited latency, inherited frames), `FramesForLatency` (ceil, capped by `MaxFramesForLatency`), `ClampToCapacity`. Called by the GUI (VideoProcessorDlg.cpp) and the Qt Config preview.
- `QueueLookahead` (QueueLookahead.h): `ResolveActivePictureLookahead` (explicit value wins; else min(target - 1, queue_size - 1, 8); low-latency path 0), `LockedFilmLookaheadFloor`, `ExplicitZeroLookahead` and its Config notice.
- `Pulldown` (PulldownDetector.h): `Detector` (finds and locks a repeat cycle, `Decision` per frame, `Settings` switches D3/D4/H1/M1/L3), `FilmClock` (even film time with re-base, robust anchor and late-arrival guard), `FilmToSignalRatio`, `LockedQueueTarget`, `kLockedQueuePairFrames` 3.
- `VpQueueRules` (VpQueueRules.h): the VP rules listed above, plus VP's own copies of the 83b7ffea stall/stale thresholds.
- `VpQueueTiming::VpFixedBudgetPolicy` (VpFixedBudgetPolicy.h): implements the four decision points of `IQueueTimingPolicy` (src/VideoProcessor-Lib/vprenderer/QueueTimingPolicy.h): `PlanHold`, `QueueTarget`, `TicksUntilDue`, `EvaluateBacklog`. Its `Options` (Original pathway, `queue_timing_fixes`) are atomics. It also has `Select` / `SelectionName` for the future `queue_policy` switch. `QueueTimingPolicy::BillQueuePolicy` in the same upstream header is upstream's behaviour: a frame-count queue, no hold, a five-argument `ShouldRecoverBacklog`, flush to target.
- `QueueLookaheadWalk::SelectWalk` (vprenderer/QueueLookaheadWalk.h, upstreamable): one walk with three views (statistics window, active-picture prefix, upstream's preparation set). Used only by the patch.

### Data flow

1. The GUI (`CVideoProcessorDlg`, profile apply) resolves the queue profile (`RendererProfileConfig::ResolveQueue`). It sends `queue_size` (`SetFrameQueueMaxSize`) and the frame-count target (`QueueLatency::ResolveTargetFrames`, then `ClampToCapacity`, then `SetQueueFramePolicy`). It then sends `SetQueueDelayBudget(ms)`: `target_latency_ms` (own or inherited, same precedence), or 0 on the low-latency path or when the profile sets `target_frames` only. It also sends the active-picture look-ahead (`QueueLookahead`).
2. Render loop, per frame (`LibplaceboVideoRenderer::RenderLoop`): `UpdateQueueDelayHold(source rate, signal rate)`. The source rate is the film rate while a pulldown lock holds, else the measured rate, else the nominal one. Budget 0, the Original pathway, low latency or an unknown display refresh give no hold. Otherwise `DelayBudget::Compute` runs with the measured display refresh (`TimelineDisplayHz`), render-ahead's residual lead and the signal frame's transfer. The plan is cut when `queue_size` cannot hold it. The result is stored as `QueueHold` (active, holdMs, transferMs, fraction, targetFrames).
3. `PrefillTargetLocked`: the queue target is the hold's whole frames when a hold is active, else the frame-count target scaled for a pulldown lock (`LockedFilmQueueTarget`). The render-ahead credit and the look-ahead floor are applied through `CombineQueueTarget`.
4. `WaitForDequeueLocked` -> `HoldTicksUntilDueLocked`: on the plain per-refresh path the head waits until it is `holdMs` old (plus the transfer for a hardware frame-start time). On the VP timeline the steering (`TimelineQueueSteering`, target + fraction) settles the rest instead.
5. After a dequeue, the backlog block: `ShouldRecoverBacklog` (stale beyond target with `queue_timing_fixes`). With a hold it also uses `ShouldShedHeldBacklog` / `ShouldShedHeldExcess` and `HeldShedFrames`, which drop only the late frames and keep timing. Dropped frames are recorded as `DropBacklogFlush`, and the next head gets a no-fill gap.
6. Pulldown: with `dejudder_pulldown` on, the capture path feeds `Pulldown::Detector`. Exact repeats are not queued, and kept frames get `FilmClock` times and `filmHz`. A lock change leaves a discontinuity that the renderer consumes on the next kept frame.
7. With the patch: steps 2-5 become `QueuePolicy().PlanHold / QueueTarget / TicksUntilDue / EvaluateBacklog`, with the same decisions in the same order (the test compares a frozen copy of the host). The host keeps the status, logs and drops. `QueuePolicy()` always returns the VP policy for now. `queue_policy = upstream` and the hand-over of the Original pathway to `BillQueuePolicy` are a later commit.

### Threading

- GUI thread: profile apply, `SetQueueDelayBudget` (atomic `m_queueDelayBudgetMs`, then `m_queueChanged.notify_all`), `SetQueueFramePolicy` / `SetFrameQueueMaxSize` (under `m_queueMutex`).
- Render loop thread (`m_renderThread`): `UpdateQueueDelayHold` (takes `m_queueMutex` briefly for the capacity, writes `m_queueHold` under `m_queueHoldMutex`), the backlog block and the dequeue wait (under `m_queueMutex`), and `EngageFallback` / `ReleaseFallback` (the `FallbackStatus::Registry` has its own mutex).
- Capture callback thread: enqueue, pulldown decisions and film-clock times.
- Atomics shared between threads: `m_queueTimingFixes`, `m_cadenceClassFixes`, `m_pulldownCadence`, `m_queueTargetFloor`, `renderAheadCreditFrames`, `earlyUploadQueueTarget`, `lowLatencyQueueActive`.
- The module headers are pure and hold no locks. `VpFixedBudgetPolicy` holds only two relaxed atomics. `BacklogRunState` is owned by the caller.

### Config keys the code reads

- `[queue]` / `[queue.<name>]` (queue profiles; `RendererProfileConfig::ResolveQueue`, default queue through `VideoProcessorApp` / `QueueConfiguration.h`): `target_latency_ms` (0-1000), `target_frames` (0-16; legacy alias `steady_reserve_frames`), `queue_size` (also legacy `[command_line]` / `[general]`), `active_picture_lookahead_frames` (0-8), `startup_preroll_frames`, `low_latency_path` (absent: on only for a profile named `Low_Latency`), `reset_after_render_restart_seconds`, `reset_queue_too_large_percent`. The last two can also come from the legacy `[queue_recovery]` section; that is a section, not a key. `lead_frames` is DirectShow's presentation lead; the VP renderer does not implement it.
- `[general]`: `use_frame_queue`, `queue_auto_reset`.
- VP Renderer profile sections (`ApplyDisplayRuleOverrides`): `queue_timing_fixes`, `pulldown_fixes`, `cadence_class_fixes` (locked-film queue scaling, detector H1/M1/L3), `timing_continuity_fixes` (film-clock re-base), `general_rate_fixes` (`QueueLatency::MaxFramesForLatency`). The Original pathway forces all five off.
- Read here but owned elsewhere: `dejudder_pulldown` (turns pulldown detection on) and `pipeline_original_pathway`.
- Planned, not parsed yet: `queue_policy` (`vp` | `upstream`; patch step P-c).
- There is no `low_latency` key. Low latency is the `Low_Latency` queue profile and `low_latency_path`.

### What the user sees (fallbacks)

Engaged in `UpdateQueueDelayHold`. All of them are under "Fallbacks engaged" (Ctrl+I, Config status, Ctrl+Alt+D), with the GUI's one-time notice per id (`CVideoProcessorDlg::UpdateFallbacksStatus`):

- `delay budget (display refresh not known yet; frame-count queue)`.
- `delay budget (too short for 2 queued frames at this rate; frame-count queue)`.
- `delay budget (queue_size N holds X of Y ms at R Hz (needs M); raise queue_size)`: the hold is cut to what the FIFO holds.
- `delay budget (render-ahead on: the total across frame rates is not verified yet (PENDING-RA))`: shown whenever render-ahead is active, even though nothing is downgraded.
- `look-ahead (n of 4 queued frames (delay budget T ms))`: the 3/2 rungs of the ladder.

Log-only, not entries:
- the frame-count target clamped to `queue_size` ("Alpha queue policy: ..." line);
- "target < the motion look-ahead's" note;
- backlog flushes and sheds (logged and recorded as `DropBacklogFlush`; a transient correction, not a downgrade);
- pulldown lock changes.

The low-latency path and the Original pathway turn the budget off by design. That is not a fallback, and the log line says "hold off (low latency / no budget)".

### What is needed to lift the module

- Pure headers: the C++ standard library, `GeneralRate.h` (QueueLatency), `DisplayRefreshRatePolicy.h` (`CaptureRateBounds`, `ValidFrameRateOrDefault` for VpQueueRules), and Bill's `vprenderer/AlphaQueuePolicy.h` (depth helpers, preview window).
- `VpFixedBudgetPolicy` also needs `vprenderer/QueueTimingPolicy.h` and DelayBudget.
- The host side (queue, waits, drops, status) stays in `LibplaceboVideoRenderer.cpp`. To lift the module fully, apply the call-site patch and then delete the compat shim.

### Test it standalone

Build x64 Release (the AGENTS.md command). Run tests single-threaded and read the TRX for the summary:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Tests:<Class> /Logger:"trx;LogFileName=x.trx"`

- Delay budget: `DelayGate` (src/VP-V/tests/DelayGateTests.cpp: 4/3/2 x 8 rates, same total, harness queue), `RenderAheadDelayBudgetTests` (R6BaselineTests.cpp).
- Queue rules and harness: `QueueTimingFixesTests`, `QueueStressTests`, `QueueStressRepros`, `QueueStressLaneTests`, `QueueLatencyMatrix` (GeneralRateMatrixTests.cpp), `QueueLookaheadTests` (IntentInterpreterTests.cpp), `H3TimingTests`.
- Pulldown and film clock: `PulldownReliabilityTests`, `PulldownCorpusTests`, `PulldownLockChangeAdjacencyTests`, `Hunt2CadenceClassPulldownTests`, `Hunt2CadenceClassFilmClockTests`, `FilmClockRebaseTests`, `FilmClockToggleTests`.
- Policy interface: `QueueTimingPolicy` (src/VP-V/tests/QueueTimingPolicyTests.cpp: frozen-host equality, ladder, held shed, upstream equality, one walk). It runs only after the call-site patch has registered it in the test project.

## Contract

- Input: captured frames with timestamps
- Output: frames released at a constant end-to-end delay, with cadence/film-clock phase
- Geometry: n/a
- Delay share: TOTAL-DELAY BUDGET (ADR-0020): queue frames = floor((total - fixed costs) / frame period), capped at 4, ladder 4/3/2; fixed costs = swapchain depth (2 refreshes) + display phase (1 refresh) + render-ahead's residual lead + processing allowance (8 ms) + one signal frame of capture transfer (decision O1, on); padding makes the total identical at every rate. DelayBudget.h / DelayBudgetLadder.h. Not in the low-latency path or the Original pathway (frame-count queue).
- Look-ahead: provides the shared look-ahead window: queue depth = deepest single look-ahead + hiccup spare (features share, never add)

## Settings

- Existing keys (checked 2026-10-25; sections in "How the code works"): `target_latency_ms`, `target_frames`, `queue_size`, `active_picture_lookahead_frames`, `startup_preroll_frames`, `low_latency_path`, `reset_after_render_restart_seconds`, `reset_queue_too_large_percent`, `use_frame_queue`, `queue_auto_reset`, `queue_timing_fixes`, `pulldown_fixes`, `cadence_class_fixes`, `timing_continuity_fixes`, `general_rate_fixes`. (`queue_recovery` is a legacy section and `low_latency` a queue profile name, not keys.)
- Planned keys: `queue_policy` (vp | upstream; call-site patch step P-c)
- Tiers: not tiered; one user-set total delay (recommended = 4 frames at the slowest rate)

## Fallbacks

- "Fallbacks engaged" entries (as the renderer writes them): `look-ahead (<n> of 4 queued frames (delay budget <T> ms))` and `delay budget (<reason>)` (refresh not known / too short / queue_size cut / PENDING-RA).
- Behaviour: With 2-3 frames available, every look-ahead consumer shortens its look-ahead or falls back; the delay never changes. When the budget cannot be realised the queue runs as a frame-count queue (or a cut hold) and says so.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.capture`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests (classes; see "Test it standalone"):
- QueueStressTests, QueueStressRepros, QueueStressLaneTests (QueueStressHarness), QueueTimingFixesTests, QueueLatencyMatrix, QueueLookaheadTests
- H3TimingTests
- PulldownReliabilityTests, PulldownCorpusTests, FilmClockRebaseTests, FilmClockToggleTests, Hunt2CadenceClassFilmClockTests
- QueueTimingPolicy (after the call-site patch)

Contract tests:
- queue minimum follows the configured/time depth (no forced 5-frame minimum)

Chain tests:
- delay gate (`DelayGate`): the recommended total and lower totals (4/3/2 frames) at 23.976/24/25/30/50/60p; identical measured total across rates; no stalls; recovery when frames return

## Known issues / open items

- DONE: the forced 5-frame minimum is gone (r6 fix 2; `ApplyFrameMinimum` removed).
- DONE: motion_ahead is time-based (r6 fix 3; `MotionLookAheadTimeTests`).
- PARTLY: render-ahead runs only inside the delay budget (r6 fix 4, `FitDelayBudget`). Its constant total across rates is not verified (PENDING-RA), and the budget model keeps 2 swapchain frames while render-ahead's chain uses 3 (see findings).
- QueueLatency.h still rounds UP (ceil) on the frame-count path (no budget, low latency, Original pathway, other renderers): 200 ms at 23.976 = 5 frames. With a budget the VP renderer uses the hold instead (barrier B2).
- The host does not call the policy interface yet: apply docs/design/motion-timing-callsites.patch (commit P-a), then rename the callers and delete AlphaQueuePolicyVpCompat.h.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/core/DelayBudget.h`
- `src/VP-V/modules/input/queue_timing/QueueLatency.h`
- `src/VideoProcessor-Lib/QueueConfiguration.h`
- `src/VP-V/modules/input/queue_timing/QueueLookahead.h`
- `src/VideoProcessor-Lib/CaptureFrameQueue.h`
- `src/VideoProcessor-Lib/VideoTimingController.cpp`
- `src/VideoProcessor-Lib/TimingClock.cpp`
- `src/VideoProcessor-Lib/UnifiedProfileRuntime.cpp`
- `src/VP-V/modules/input/queue_timing/PulldownDetector.h`
- `src/VideoProcessor-Lib/vprenderer/AlphaQueuePolicy.h`
- `src/VideoProcessor-GUI/VideoProcessorDlg.cpp`
- `src/VP-V/modules/input/queue_timing/VpQueueRules.h`
- `src/VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h`
- `src/VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h`
- `src/VideoProcessor-Lib/vprenderer/QueueTimingPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/QueueLookaheadWalk.h`

Forwarding headers (old paths kept for Bill's files, the renderer host and RendererCaptureHarnessTests.cpp; removed at M2 step 0):

- `src/VideoProcessor-Lib/QueueLatency.h`
- `src/VideoProcessor-Lib/QueueLookahead.h`
- `src/VideoProcessor-Lib/vprenderer/PulldownDetector.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0003, ADR-0020 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `active_picture_lookahead_frames`, `cadence_class_fixes`, `general_rate_fixes`, `low_latency_path`, `pulldown_fixes`, `queue_auto_reset`, `queue_size`, `queue_timing_fixes`, `reset_after_render_restart_seconds`, `reset_queue_too_large_percent`, `startup_preroll_frames`, `target_frames`, `target_latency_ms`, `timing_continuity_fixes`, `use_frame_queue`.
- Tiers: not tiered; one user-set total delay (recommended = 4 frames at the slowest rate).
- VP-V page, Input tab: not tiered (Always on: capture and timing have no quality steps, only paths (the toggles below).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#2-input](../../../../../docs/settings/VP-settings-reference.md#2-input).
- Fallback entry ("Fallbacks engaged"): `look-ahead: <n> of 4 queued frames (delay budget <ms>); delay budget: <why the total is not exact>`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): QueueStressHarness; H3TimingTests; film clock tests.
- Contract (formats, geometry, fallback entry): queue minimum follows the configured/time depth (no forced 5-frame minimum).
- Chain (total delay at the 4/3/2-frame budgets): delay gate: the recommended total and lower totals (4/3/2 frames) at 23.976/24/25/30/50/60p; identical measured total across rates; no stalls; recovery when frames return.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
