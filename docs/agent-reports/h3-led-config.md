# Hunt 3 (hunter D): Dynamic Drive and Config fixes

Branch `fix/h3-led-config`, from local `fix/cntr-bright-cut` (c1acec6b: the
injected LED clock and the CNTR bright-cut fix). Findings:
`D:/codex/hunt3-scratch/d/findings.md`, `sub-led.md`, `sub-config.md`.
The LED arbiter (`LedDriveArbiter.cpp`, fix/h3-signal) is untouched.

## Dynamic Drive

| Item | Fix | Test |
|---|---|---|
| LED1 look-ahead clock went backwards | `LookaheadObserver` keeps its own monotonic clock: each newly measured frame advances it by the frames since the last one at the current period (bounded); a sequence jump back restarts the builder (`led/LedLookahead.cpp` ObserveFrame). `WindowMinimum` and the fade window restart when time goes back and keep at most `StatsBuilder::MAX_WINDOW_SAMPLES` (4096) (`led/LedMeasurement.cpp`). Predictions are capped at `MAX_PREDICTIONS` (4096). | `LedDriveHunt3Tests::LookaheadClockSurvivesARateChange` (3000 frames at 23.976, then 59.94, one black frame, highlights: sustained highlight recovers within highlight_persist_ms; and back to film), `StatsBuilderWindowsRestartAndStayBounded` |
| LED2 adopted hold-over drive kept old look-ahead state | `LedDriveModule::ResetLookahead()` (predictions, sequences, look-ahead epoch); the observer drops its sequence, windows and clock when the epoch changes. `BeginFrame` calls it when the source sequence restarts (back to 1-2, or further back than the 64-frame prediction history), so an adopted module resets with no renderer change; the adopt site can also call it before the new file's first walk. | `AdoptedDriveLooksAheadAtTheNewFileAtOnce` (explicit reset and sequence restart, after a 300-frame file), `SmallSequenceStepBackKeepsTheLookahead` |
| LED3 exit restore order | `RestoreFullLight`: LDMM, CNTR 50, then LPW1 (the bright-cut order); the shared deadline still reserves each later command's gap (`led/ProjectorDriver.cpp`). | `LedDriveProtocolTests::ScriptStartAndExitSetContrastTo50`, `ExitRestoreKeepsTheCommandGap` (updated) |
| LED4 Apply in script_measure window | When new settings cut a schedule short before its LPW1 was written, the LPW1 is completed too (`completeLight`), first - before the completed CNTR or the next tick's own schedule - and the controller carries on from it (`led/LedDriveModule.cpp` Run). | `ApplyInTheMeasureWindowKeepsTheDarkStepOrder` (pacing, 1200 ms measure window, Apply 600 ms into it) |
| LED5 compensating / applyGain race | Both `std::atomic<bool>` (`led/LedDriveModule.h`). | `ModeSwitchesWhileTheScriptThreadRuns` (smoke: 200 switches against running frames) |

Each test except the two smoke/negative ones was checked to fail with its fix
disabled (6 of 8 failed, as expected), then passed with it.

These are correctness fixes, so no new switch. The base has no MPC hold-over
(`file_led_holdover_ms`); after the merge, the adopt site in
`LibplaceboVideoRenderer.cpp` may call `ResetLookahead()` before the new file's
first walk so its very first walk is measured (without it, the restart is
caught at the first `BeginFrame`).

## Config

| Item | Fix | Test (ConfigEditorWindowTests unless noted) |
|---|---|---|
| CFG1 default-on bools could not be turned off | `ConfigBoolSwitches.h` (new): the table of configuration bools VP turns into switches, each with its default; a configured false of a default-on key emits `<switch> false` (ReadBooleanOption takes it). Audit in the header: only `newlldv_sdr_veto` is default-on (dv_tunnel_handoff is not on this base; add it with defaultOn when merged). `VideoProcessorApp.cpp` AppendConfigBoolOption uses it. | VideoProcessor-Test `ConfigFileTests::Hunt3DefaultOnBooleansCanBeTurnedOff` (every key, false and true; the table follows HdrInferenceOptions' default) |
| CFG2 latch fields showed Auto | The three latch keys are in the placeholder list; greyed unless Source resolution Auto with the latch fixes on. | `hunt 3: latch timing fields show defaults and grey` |
| CFG3 retired even rates | `retiredChoiceTarget()`: `30` / `30_and_29.97` show "(retired -> maps to ...)"; the Motion slider reads them as `whole` / `whole_and_1001`. | `hunt 3: retired even rates say what they map to` |
| CFG4 interpolation_grain greyed under hiccup repair | Greyed only when interpolation is off and Fill dropped frames is off; tooltip says so. | `hunt 3: interpolation grain follows hiccup repair` |
| CFG5 content-gated profiles | In a profile whose `when` reads `$content` (cadence_class_fixes on), intent_content, source_class, dejudder_pulldown and cadence_class_fixes are greyed with the reason; undone on the next profile load before the refreshes. | `hunt 3: content-gated profile greys classifier inputs` |
| CFG6 queue fold target | The fold targets `[queue]` when present (VP's default), and the root rename (Config saves only `[queue.*]`) now moves the renamed root to the front so it stays VP's default (it silently made the first named profile the default). | `hunt 3: legacy queue fold targets [queue]` |
| CFG7 remote Discard | `foldLegacyInputPolicy()` extracted from loadConfiguration; remote Discard runs it with the same dirty / migration / backup state. | `hunt 3: remote discard folds like a load` (fake RPC server) |
| CFG9 [general] queue_size | Read: `QueueConfiguration::TryGetHardCapacityString` (default queue profile, `[command_line]`, then `[general]`), logged when from a legacy section; Config folds `[general]` queue_size into the queue profile; allow-list reason corrected. RetiredSettings is not on this base. | VideoProcessor-Test `ConfigFileTests::Hunt3GeneralQueueSizeIsRead`; the fold in the CFG6 test |
| CFG8 docs | CONFIGURATION.html and `docs/configuration-public-fields.tsv` / `-values.tsv`: capture_path / capture_upload, the optical-flow extras, hiccup repair + interpolation_grain, intent_content (+ hold), the latch keys, the timing switches and render-ahead keys, `file_av_offset_ms` / `file_led_holdover_ms`; `content` and `source` in the rule-variable table; the sdr_veto text says how false reaches VP. | `ConfigFileTests` docs-inventory test |

Not on this base (so not documented or wired here): the detail-restore keys,
RetiredSettings, dv_tunnel_handoff, the MPC hold-over adopt site.

Also: `LedDriveProtocolTests::ResyncNeverOvertakesANewerScriptSend` (from the
injected-clock branch) failed once in three runs under load: it advanced 4 s of
manual clock without waiting for the driver thread. The clock now moves only
while the driver thread is parked on it (15 more loops, no failure).

## Runs (CPU only, single-threaded, TRX)

- Builds: full x64 Release solution, rc=0 (slot runner, /m:4, CL_MPCount=2).
- All LED classes (`LedDrive*`, `LedScript*`, 175 tests) 3x on the final DLL:
  175/175 each run.
- ConfigFileTests + ConfigurationReadCacheTests + ConfigEditorCoreTests: 246/246.
- Config tests off screen, one shard: 149 passed, 0 failed, 1 skipped (needs a
  real window), gate passed (1242 s).
