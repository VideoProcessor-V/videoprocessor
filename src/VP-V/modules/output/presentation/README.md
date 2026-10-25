# Presentation (`output.presentation`)

Unit: **output** | side: output | order: 270 | status: **built** | hook point: present

## Purpose / why

Swapchain, refresh, render-ahead (inside the delay budget), subtitle placement, transient confirmations (no always-on OSD).

## How the code works

Checked against the code at M2 step 18b (2026-10-25).

This module's VP code covers two things today:

- render-ahead: the de-judder's in-between pictures are queued straight after their frame;
- the display rate that render-ahead plans with when DXGI gives no frame statistics.

The renderer host and Bill's files do the rest: they create the swapchain, present, collect the statistics, switch the refresh rate, place subtitles and show the transient confirmations. Those parts are listed here only where this module calls them or is called from them.

### Public API

- `RenderAhead` (RenderAheadPolicy.h; pure, no clock):
  - chain shape: `SwapchainBuffers`, `MaxExtras` and `ChainDepthFor`. Without extras, or in low latency, the chain keeps 3 buffers and latency 2. Otherwise it has ceil(ratio) + 2 buffers (at most 6) and latency `kQueuedFrameLatency` 3.
  - lead and credit: `PlannedLead`, `CreditFrames`, `QueueCredit` and `CreditLatch`.
  - the delay-budget fit: `FitDelayBudget` -> `BudgetDecision`.
  - the frame planner: `FrameInput` and `PlanFrame` -> `FramePlan` (slots, intervals, drops).
  - the present chain: `Schedule` (anchor L, closed or open loop, `NoteLate`, `Break`).
  - the gate (`dejudder_gate`): `ResolveGate`, `RefreshGateHolds`, `CostWindow`, `KnownCost`, `Gate` (the step-down / step-up ladder), `SustainedLateness`, `GateEpoch` and `LevelName`.
- `OpenLoopDisplayRate` (OpenLoopDisplayRate.h):
  - `Select(Candidates, renderAhead, wholeHzLast)` -> `Choice`. The statistics' rate always wins. Without it, and only when render-ahead is on, the order is: the monitor's exact mode, then DWM (only if within 2 % of the mode), then the application's rate, then the vblank cadence, then the whole-hertz mode.
  - `VBlankCadence`: the median of `WaitForVBlank` intervals.
  - `VBlankGridRate`: a weighted fit of DWM vblank times that refines the planned rate.
  - `LateRefreshes` (QPC lateness in open loop) and `Changed`.
- Bill's files (read-only):
  - `AlphaPresentationTelemetry`: DXGI frame statistics -> `measuredDisplayHz`.
  - `DisplayRefreshRatePolicy`: mode selection and the switch decision. It reads no configuration itself.
  - `NativeStatsOverlayBitmap.h`: the Ctrl+I bitmap.

### Data flow

1. Settings apply: `render_ahead`, `render_ahead_queue_credit`, `render_ahead_utilisation` and `dejudder_gate` go into `RendererSettings`.
2. Per frame on the de-judder path (`LibplaceboVideoRenderer.cpp`):
   - `UpdateRenderAheadBudget(captureRateHz)` calls `FitDelayBudget(P_s, P_d, earlyUploadQueueTarget, credit on)`. If the lead does not fit, render-ahead is vetoed for this source, the frame takes the plain per-refresh path, and the `render-ahead` fallback is engaged.
   - `SelectDejudderDisplayRate` collects the `Candidates` and calls `Select`. The candidates are the statistics, `QueryDisplayConfig`, DWM, `SetRenderLoadDisplayRefreshRate`, `EnumDisplaySettings`, and a vblank sample (taken only when nothing else is known). With `queue_timing_fixes` on, an open-loop rate is refined by `VBlankGridRate`.
   - The gate picks a level from the measured costs.
   - `PlanFrame` lays A's extras on the vsync grid. The plan is committed to the `Schedule`, and `renderAheadCreditFrames` is published from `CreditLatch::CommitFrames`.
3. Back to the queue (`input.queue_timing`):
   - `PrefillTargetLocked` subtracts `renderAheadCreditFrames` (`CombineQueueTarget`).
   - `UpdateQueueDelayHold` counts the residual lead (`PlannedLead - credit x P_s`) as a fixed cost of the delay budget.
4. Swapchain: once a change of `ChainDepthFor` has settled, the VP-owned chain is re-created with the new buffer count and `SetMaximumFrameLatency` is called.

### Threading

- Everything above runs on the renderer's render loop thread (`m_renderThread`, `RenderLoop`). The planner, gate, schedule and telemetry state belong to that thread and take no locks.
- Atomics read from other threads:
  - `renderAheadCreditFrames`: read for the queue target, under `m_queueMutex`.
  - `renderLoadDisplayRefreshHz`: set by the host application.
  - `earlyUploadQueueTarget`.
- The vblank-cadence sample blocks the render thread for about 9 refreshes, at most once every 10 s. It runs only when no other rate source exists.

### Config keys the code reads

- VP Renderer profile sections (`ApplyDisplayRuleOverrides`):
  - `render_ahead` (on/off);
  - `render_ahead_queue_credit` (on/off);
  - `render_ahead_utilisation` (0.5-1.0, default 0.85);
  - `dejudder_gate` (auto | period | refresh; `RenderAhead::ResolveGate`).
- `queue_timing_fixes` is read here but owned by `input.queue_timing`. It selects the whole-Hz-last order and turns on the DWM grid refinement.
- The refresh switch keys (`[general] switch_refresh_rate` and the `[display_refresh_rate_override]` section) are read by the GUI (VideoProcessorDlg.cpp), not by this module's files.

### What the user sees (fallbacks)

- Visible under "Fallbacks engaged", with a one-time notice: `render-ahead (off: its <lead> ms lead does not fit the <budget> ms delay budget; <reason>)`. It is released when the lead fits again or `render_ahead` is turned off.
- Log-only today (an ADR-0001 gap):
  - **open loop**: "Render-ahead: open loop, display rate ... from <source>", plus a DiagnosticHistory record;
  - the de-judder hold "timing not known: display rate";
  - render-ahead gate step-downs (fewer extras, then hold);
  - a refresh switch that kept another rate.
- `FallbackCatalog::Presentation` (id `presentation`) and `FallbackCatalog::RefreshRate` (src/VP-V/core/FallbackCatalog.h) already have the wording, but no host call site uses them. Their test file is not registered in the test project either. So the manifest's entry "Presentation: open loop (no display statistics)" is never produced yet.

### What is needed to lift the module

- `RenderAheadPolicy.h` needs only the C++ standard library.
- `OpenLoopDisplayRate.h` needs `DisplayRefreshRatePolicy.h` (`DisplayRefreshBounds`).
- The candidate reads (`QueryDisplayConfig`, DWM, `WaitForVBlank`), the present calls and the chain re-creation stay in the host.

### Test it standalone

Build x64 Release (the AGENTS.md command). Run tests single-threaded and read the TRX:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Tests:<Class> /Logger:"trx;LogFileName=x.trx"`

- Render-ahead (src/VideoProcessor-Test/RenderAheadPolicyTests.cpp): `RenderAheadPlannerTests`, `RenderAheadJitterSimulationTests`, `RenderAheadScheduleTests`, `RenderAheadGateTests`, `RenderAheadSchedulerSimulationTests`, `RenderAheadLookaheadWalkTests`, `RenderAheadGapFillTests`, `RenderAheadLedTests`, `RenderAheadOpenLoopRateTests`, `RenderAheadGateFixTests`.
- Budget: `RenderAheadDelayBudgetTests` (R6BaselineTests.cpp) and `DelayGate`.
- Bill's side: `AlphaPresentationTelemetryTests`, `DisplayRefreshRatePolicyTests`, `DisplayRefreshRateEstimatorTests`, `DisplayRefreshRateWindowTests` and `DisplayRateHoldTests`.
- `FallbackCatalogTests` (src/VP-V/tests/FallbackCatalogTests.cpp) checks the entry wording, but it is not in VideoProcessor-Test.vcxproj yet.

## Contract

- Input: final frame
- Output: display
- Geometry: display mode
- Delay share: render-ahead counted inside the fixed delay (FitDelayBudget veto + queue credit; the residual lead is a fixed cost in DelayBudget; constant total across rates not verified yet, PENDING-RA)
- Look-ahead: render-ahead

## Settings

- Existing keys (checked 2026-10-25): `render_ahead`, `render_ahead_queue_credit`, `render_ahead_utilisation`, `dejudder_gate` (the render-ahead gate; was owned by no slot)
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Presentation: open loop (no display statistics) - NOT produced yet: open loop is log-only (FallbackCatalog::Presentation is unwired). Produced today: `render-ahead (off: ... does not fit the ... delay budget; ...)`.
- Behaviour: Window / iGPU panel (no DXGI frame statistics) -> render-ahead plans open loop on the best other rate source; render_ahead off -> the de-judder holds until a display rate is known.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `output.dither`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests (classes; see "Test it standalone"):
- RenderAheadPlannerTests, RenderAheadScheduleTests, RenderAheadGateTests, RenderAheadOpenLoopRateTests, RenderAheadGateFixTests and the other RenderAhead* classes
- AlphaPresentationTelemetryTests

Contract tests:
- render-ahead fits the delay budget (RenderAheadDelayBudgetTests)

Chain tests:
- delay gate (`DelayGate`)

## Known issues / open items
- WP-24 captured-repeat render reuse (`repeat_render_reuse`, draft default off; Original pathway off): RepeatReuseGlue.h / RepeatReusePolicy.h / RepeatReuseAdapter.h / TemporalShaders.h / RepeatReuseRegistryKey.h here. It depends on motion.consumers' rendered-picture cache (DejudderRendered::Cache, RenderedWarp.h). Host call sites: docs/design/repeat-reuse-callsites.patch.

- Open loop, the display-rate hold, gate step-downs and a kept refresh rate are log-only (wire FallbackCatalog::Presentation / RefreshRate).
- The delay budget counts 2 swapchain frames while render-ahead's chain runs at latency 3 (PENDING-RA).
- FitDelayBudget judges against the frame-count target, not the delay budget's hold.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/output/presentation/RenderAheadPolicy.h`
- `src/VideoProcessor-Lib/vprenderer/AlphaPresentationTelemetry.cpp`
- `src/VP-V/modules/output/presentation/OpenLoopDisplayRate.h`
- `src/VideoProcessor-Lib/DisplayRefreshRatePolicy.cpp`
- `src/VideoProcessor-Lib/vprenderer/NativeStatsOverlayBitmap.h`

Forwarding headers (old paths kept for Bill's files, the renderer host and RendererCaptureHarnessTests.cpp; removed at M2 step 0):

- `src/VideoProcessor-Lib/vprenderer/OpenLoopDisplayRate.h`
- `src/VideoProcessor-Lib/vprenderer/RenderAheadPolicy.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0003 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `dejudder_gate`, `render_ahead`, `render_ahead_queue_credit`, `render_ahead_utilisation`, `repeat_render_reuse`.
- Tiers: not tiered.
- VP-V page, Output tab: tiers Classic / Low / Mid; tier keys `temporal_dither`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#8-output](../../../../../docs/settings/VP-settings-reference.md#8-output).
- Fallback entry ("Fallbacks engaged"): `presentation: open loop: no display statistics, estimated rate / de-judder held: display rate not known`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): RenderAheadPolicy tests; AlphaPresentationTelemetry tests.
- Contract (formats, geometry, fallback entry): render-ahead at 24p fits 200 ms.
- Chain (total delay at the 4/3/2-frame budgets): delay gate.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
