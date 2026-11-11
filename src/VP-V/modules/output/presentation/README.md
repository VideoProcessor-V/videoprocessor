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
  - the delay-budget fit: `FitDelayBudget` -> `BudgetDecision`. The 6-argument form (P6 rest S1) takes the analysis floor (`floorFrames`) and a `FloorPolicy` (default `CreditWins` = today; `FloorWins`; `RaiseQueue`, the TD-on choice where the floor may lift the queue above the desired frames). `BudgetDecision` adds `floorFrames`, `floorCompetes`, `realTotalMs`, `queueFrames` and `raisedQueueFrames`; the 4-argument form is the floor-0 / `CreditWins` call.
  - the frame planner: `FrameInput` and `PlanFrame` -> `FramePlan` (slots, intervals, drops).
  - the present chain: `Schedule` (anchor L, closed or open loop, `NoteLate`, `Break`).
  - the gate (`dejudder_gate`): `ResolveGate`, `RefreshGateHolds`, `CostWindow`, `KnownCost`, `Gate` (the step-down / step-up ladder), `SustainedLateness`, `GateEpoch` and `LevelName`.
  - the Presenter API (M2 P6-4, interface first): `Presenter` owns the render-ahead state (chain, gate and its cost windows, epoch, drift lateness, credit latch, chain depth) with `Break`, `ResetGate`, `UpdateGate(now, PresenterGateFrame)` -> `PresenterGateResult` and `TakeMainInterval`. The renderer keeps one reference alias per former member and logs from the results; P6 rest moves the present clusters onto it. P6 rest adds (S1) the analysis-floor inputs `analysisFloor` (atomic, frames; 0 = none), `floorPolicy` (default `CreditWins`) and `spareSlotMs` (atomic; the declared spare-slot GPU ms per source frame, `GateCosts::spareSlotMs`, added to the gate's average check only), `FitBudget` (FitDelayBudget with those inputs) and `FloorReport` (the visible line when floor and credit compete, shown as the `queue analysis floor` fallback; empty otherwise), `Active`, `Now`, `ClearBudget` / `UpdateBudget` -> `PresenterBudgetChange` / `ApplyBudgetVeto` and `ResolveTimers` (the GPU timers stay renderer objects passed in) with `TakeExtraTimerMode` / `NoteExtraTimerMode`; (S2) `BeginFrame`, `Commit(PresenterCommit, plannedIds)` for both commit sites and `PublishEstimatedLead`; (S4) `RestartChain` (the schedule's break only, not `Break(now)`); (S3) the present bookkeeping the de-judder extension reads (extra ids, main refreshes, ids swapchain, display rate, expected present, lead, counters, interval / shown-error diagnostics, open-loop counts) with `NoteIdsSwapchain`, `ClearPresentIds`, `NoteExtraPresent`, `NoteMainRefresh`, `NoteStatisticsPresent`, `NotePresentInterval`, `NoteObservation`.
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
- Analysis-floor controls (P6 rest S1; audit S2): no settings key; set by the TD/owner wiring, which has no production caller yet:
  - `Presenter::floorPolicy` - no settings key; default `CreditWins`; set by the TD/owner wiring. `RaiseQueue` must be set together with
    `VpQueueTiming::VpFixedBudgetPolicy::SetRaiseQueueForAnalysis(true)` (and `Presenter::queueCapacity`); the test
    `TheTwoRaiseQueueSwitchesAgreeOnTheRaisedQueue` pins that both give the same raised queue, capped at the capacity.
  - `Presenter::queueCapacity` - no settings key; default 0 (not known: no cap); set by the TD/owner wiring.
  - `Presenter::spareSlotMs` - no settings key; default 0.0 (no spare-slot GPU work); set by the TD/owner wiring.
  - `Presenter::analysisFloor` - fed by the host from `VpQueueRules::AnalysisFloorFrames`; the declared analysis lead the host passes is 0
    today (LibplaceboVideoRenderer.cpp, the analysis-floor `// VPV-HOOK` block), so the floor is the motion look-ahead's alone.

### Original pathway (PHASE 1 of the host seams plan, refs/107 s2.4 item 1; owner E3 2026-11-11)

- `RenderAhead::Presenter` carries the pathway fact (`originalPathway`, set by the host's settings seam `UpdatePipelineSlots` through `SetOriginalPathway(on, now)`): in the Original pathway `Break`, `ResetGate`, `UpdateGate` and `TakeMainInterval` return at once (`// VPV-GATED`; Bill's present path has no render-ahead: SyncInterval 1, no gate, no credit). Entering Original hands over ONCE (chain broken, credit returned, gate epoch reset), so no VP-V plan outlives the switch. Tests: `OriginalPathwayGateTests`, `PresenterApiTests`.

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

- Render-ahead (src/VideoProcessor-Test/RenderAheadPolicyTests.cpp): `RenderAheadPlannerTests`, `RenderAheadJitterSimulationTests`, `RenderAheadScheduleTests`, `RenderAheadGateTests`, `RenderAheadSchedulerSimulationTests`, `RenderAheadLookaheadWalkTests`, `RenderAheadGapFillTests`, `RenderAheadLedTests`, `RenderAheadOpenLoopRateTests`, `RenderAheadGateFixTests`, `PresenterApiTests` (the Presenter against the pre-P6-4 renderer bodies), `PresenterRestApiTests` (the P6 rest methods against the r2/p6-4 renderer bodies), `PresenterFloorTests` (the analysis floor: the three policies, the cap at the desired queue, `FloorReport`, `spareSlotMs`).
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

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `dejudder_gate` | profile ([vprenderer.Default] and matching profiles) | enum | auto, period, refresh | auto | VP-V > Motion > Advanced > Render ahead: tuning | n/a (VP-only): unchanged by the Original pathway | Which time the de-judder's GPU budget gate counts per frame: auto = per source period with render_ahead, else per display refresh | same on all (not tiered) |
| `render_ahead` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Motion > Main > Render ahead (under test); also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Render-ahead (under test): the de-judder's extras are queued straight after their frame (no vblank waits) so the next frame starts rendering while they are shown; runs only inside the delay budget; owns presentation, either/or with Bill's present-every-vsync. status line: presentation: open loop: no display statistics, estimated rate | same on all (not tiered) |
| `render_ahead_queue_credit` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Motion > Advanced > Render ahead: tuning | n/a (VP-only): unchanged by the Original pathway | The queue target gives back the planned render-ahead lead so the total delay stays the same (lip sync) | same on all (not tiered) |
| `render_ahead_utilisation` | profile ([vprenderer.Default] and matching profiles) | float | 0.5 - 1.0 | 0.85 | VP-V > Motion > Advanced > Render ahead: tuning | n/a (VP-only): unchanged by the Original pathway | The GPU share of a source period the render-ahead gate budgets | same on all (not tiered) |
| `repeat_render_reuse` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP Renderer > Rendering > Temporal dither | Original pathway sets `off` (ADR-0019) | Draft (WP-24): a captured repeat (24p in 30, 30p in 60) is drawn from the rendered-picture cache with only the output stage; anything that changes the picture refuses it | same on all (not tiered) |


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
