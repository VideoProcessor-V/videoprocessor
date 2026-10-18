# Flaky tests: LED / Dynamic Drive made deterministic (2026-10-18)

Branch `fix/led-flaky-tests`, from local `integ/2026-10-17` (ca7e3670). CPU only, no GPU.

## Cause

The LED module (`LedDriveModule`) and its RS-232 driver (`BarcoAsciiDriver`) read
`std::chrono::steady_clock` directly: the module's own time (`NowMs`: the walk's 1 s
staleness, idle restore, the statistics' time stamps, landing of sends on the frame
clock), the driver's command gaps, Barco's 2 s after a missing reply, reopen back-offs,
sequence steps and write times. The tests fed frames with an explicit frame time
(`BeginFrameAt`) but everything above ran on the wall clock, and they synchronised with
sleeps and `WaitFor` polls. Under load the wall clock between two test frames
stretched, so for example a 30-frame render took more than 1 s and the look-ahead walk
went stale (`EffectiveLookaheadIsClampedToTheFramesActuallyQueued`), or a send's write
time slipped a frame against the frame clock (`With(out)LookaheadThe...`,
`LandingIsTimedFromTheWriteInMilliseconds`).

## Method

- **Clock seam (production default unchanged).** New `led/LedClock.h`: `Clock`
  (`Now`, `WaitUntil`, `WaitOnce`), `SteadyClock()` = exactly `steady_clock::now()` and
  `condition_variable::wait_until`, and `ManualClock` for tests. `LedDriveModule`,
  `BarcoAsciiDriver`, the dry-run driver and `CreateProjectorDriver` take an optional
  `ClockPtr` that defaults to `SteadyClock()`; the module passes its clock to its
  driver. Every `steady_clock::now()` / `wait_until` in those files goes through it,
  except real I/O bounds, which stay on the real clock: a reply's `ack_timeout_ms`, the
  port lock's timed wait, and the bounded exit restore's budget (its gap wait is also
  capped in real time, so a test clock can never hold up an exit).
- **ManualClock.** Time moves only on `Advance`/`AdvanceTo`, which wakes every waiter
  (2 ms backstop). It records which threads are parked having seen the current time
  (`SettledCount`, `ThreadSettled`).
- **Test harness (`LedDriveTestSupport.h`).** `TestClock` moves the clock frame by
  frame (`To`), or in steps that wait until the module's thread is parked again and the
  driver is idle or parked in a gap (`Settle`, `Run`, `RunUntil`); `During` runs a
  blocking driver call (one that waits out a gap) on its own thread while the clock
  steps. The fake projector stamps writes on the same clock. Frame loops re-send the
  renderer's walk each frame (`SetLookaheadDepth`), as the renderer does. Remaining
  real-time waits are only for things that need no drive time (a port write and its
  echo), bounded at 10 s instead of 3 s.

Converted: every LED test that drives the module or driver through time - 22 in
`LedDriveDelta1Tests`, 5 in `LedDriveHunt2Tests`, 6 in `LedDriveCompensateTests`, 5 in
`LedDriveMeasurementTests`, 9 in `LedDriveProtocolTests` (gaps, back-offs, reopen,
resync, interruptions inside the gap wait). `LedScript*` and `LedDriveControllerTests`
have no real-time dependence.

Left on real time on purpose (they test real-time bounds, with margins):
`ClosingDoesNotWaitOutAPause`, `ExitRestoreIsNeverOvertakenByADimCommand`,
`RestoreOnExitNeverBlocksForLong`, `ExitRestoreKeepsTheCommandGap`,
`ExitRestoreIsNotLockedOutByAWaitingCommand`, `ScriptSendsIgnoreTheCommandGap...`,
`MissingReplyIsCounted...`, the bounded-restore / concurrent-stop tests, and
`LookaheadPeakCostAt4K` (a CPU cost bound, fastest of 7). None failed in any run.

## Results

LED classes (`LedDrive*|LedScript*|LedController*`, 166 tests), 10 consecutive runs,
single-threaded vstest with TRX, machine loaded by other agents (CPU 85-90 %, several
MSBuild and vstest processes):

| | runs failing | test failures | run time |
|---|---|---|---|
| before (base build ca7e3670) | 2 of 10 | 4 (`EffectiveLookahead...`, `WithoutLookahead...`, `WithLookahead...` together in one run; `LandingIsTimed...` in another) | ~195 s |
| after | 0 of 10 | 0 | ~70-110 s |

Tonight's TRX files elsewhere on D: show the same LED tests failing (6, 5, 4, 3 times),
plus `SettingChangesCarryOnFromTheCurrentLedLevel` once.

## Finding for the owner (not changed)

`ModuleBrightCutDumpsContrastAndFadesUp` asserts that CNTR never rises during the
fade-up after a bright cut. It only passes when frames are fed much faster than real
time (the module's statistics are time-stamped on the module clock, which then barely
moves between frames). With the clock running at the real frame rate, CNTR goes
55 54 52 51 50 and then rises to 54 again before the fade-up ends. The test keeps its
original (compressed) timing, now deterministic (module clock at a tenth of the frame
clock), with a comment; the real-time behaviour needs a look in the CNTR pairing /
recovery ceiling.

## Other flaky tests seen tonight

- **remote-only client opens its sole discovered VP** and **remote target switch keeps
  window responsive** (Config; 3 failures in tonight's shard logs: "Successful remote
  connection did not open the target configuration"). Cause: real-time bounds - the
  stub server's fixed 450 ms stall (checks had to land inside it), a 6 s accept timeout
  between the window's requests, and an 8 s completion bound against a flow that takes
  9-12 s under load. Fixed: the stall is held until the test has checked the loading
  screen and then released; the server runs until the test ends; completion bound 30 s;
  the UI-blocking check is 5 s (a blocking connect would now wait for the held reply).
  5 runs: all pass.
- **scene detection defaults off** and **decklink 12-bit YCbCr control** ("crashed
  once"): no crash event in the Application log and no dump; neither test has a timer,
  thread or sleep, and both passed 5 of 5 here. Most likely a crash from an earlier
  test's background work landing during them; not reproduced, not changed.
- **DeckLinkConvertersSustained4K60PerformanceComparison** (10 failures tonight): a CPU
  throughput bound (average / p95 under one 60 fps frame) - fails whenever other
  agents load the CPU. Needs a quiet machine or an informational mode in parallel runs.
- **unfocused number boxes ignore the wheel** (Config): started without a result once
  (hunt2-config-b); cause not determined.
- The rest of tonight's failures (whole-chain, real-clip, renditions, intent keys,
  recorder, srcres) are GPU tests or branch work in progress, not timing flakes.

## Note

A vstest run of an intermediate build (PID 48224, started 01:54 by this agent) is stuck
in a test loop from that build and keeps one core busy; stopping it was not permitted
here. It can be ended safely.
