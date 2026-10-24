# Projector control (`drive.projector_control`)

Unit: **drive** | side: control | order: 900 | status: **built** | hook point: host

## Purpose / why

RS-232 projector driver (Barco ASCII: LPW1 light, CNTR contrast, LDMM/LDCx custom dim) plus the pure LED/CNTR controllers that decide the commands. Serves `mode: script` (VP's copy of the beta_22 script, `command_source: internal_script`) and `mode: compensate`; `log_only` and `picture_only` never open the port. Latency timing comes from `led_latency_ms` / `cntr_latency_ms`.

## Contract

- Input: LED drive decisions (light and CNTR targets from the controllers, posted by `LedDriveModule` in `drive.led_compensation`)
- Output: serial commands (`:LPW1 n`, `:CNTR n`, `:LDMM n` with a CR) on one COM port, plus a `DriverStatus` (values written, write times, next send slot)
- Geometry: n/a
- Delay share: no picture delay. `led_latency_ms` / `cntr_latency_ms` (dead time) and `led_settle_ms` / `cntr_settle_ms` (first-order settle) model when a command takes effect; the module uses them to time the picture gain and to pick the frame a command is for
- Look-ahead: none in this folder; the controllers take queued-frame statistics from `drive.led_measure` (`LedLookahead`) through `LedDriveModule`

## How the code works

### Public API

- `IProjectorDriver` (ProjectorDriver.h). `Open` (transport + handshake; a driver whose `Open` failed never sends), `QueryLightRange`, `ReadState`.
  - Asynchronous, newest value wins: `SetLight`, `SetLightJump` (not split by `max_levels_per_command`), `SetContrastAsync` (goes ahead of a waiting LPW1; negative drops a waiting one), `CancelLight`, `QueueSequence` / `QueueSequenceNow` (custom-dim command lists).
  - Synchronous: `SetLightNow`, `SendNow` (startup and exit, bounded by a timeout), `SendScript` (one LPW1/CNTR in the script's own timing: no command gap, not coalesced), `SetContrast`.
  - `BeginShutdown` (drop queued work so the exit restore is last), `Close`, `Status`.
- `CreateProjectorDriver(protocol, config, portFactory, clock)` (ProjectorDriver.cpp): `barco_ascii` gives `BarcoAsciiDriver`; `none` gives an internal dry-run `NoneDriver` that logs what it would send and never opens a port.
- `BringToKnownState` (startup: LPW1 to `led_ceiling`, CNTR 50 when it is not 50, optional LDMM 0) and `RestoreFullLight` (exit: LDMM 0, LPW1 to the ceiling, CNTR 50 when driven; bounded to about 1 s per command plus the command gap) returning `RestoreOutcome`.
- `ISerialPort` / `Win32SerialPort` (SerialPort.h): 8N1, no flow control, `ReadLine` up to CR. Tests substitute a fake port.
- `RunConnectionTest` / `RunConnectionTestOnNewConnection` (ConnectionTest.h): LED 60, read back, hold 5 s, LED 255 (or the reported maximum), read back, pass/fail. `ConnectionTestPlan::cancel` ends the hold and still puts the light back.
- `RunBoundedRestore` (LedShutdownRestore.h): runs a stop function on a detached thread and waits at most `timeoutMs` (forced shutdown).
- `Clock` / `SteadyClock()` / `ManualClock` (LedClock.h): the time source for command gaps and deadlines. Production uses the steady clock; tests drive `ManualClock`.
- Controllers (LedController.h, pure, no I/O, no clock): `ScriptController` (beta_22 port: light curve, step tables, CNTR bands), `PlanScriptLookahead`, `PaceScriptTick`, `CompensateController` (per-frame LED target, rate law, deadband, highlight protection), `CntrPairing` (CNTR assist, never above the need), `CutRecovery` (fade-up after a bright cut), `FluxModel` (`flux_table`), `SettleModel`, and the gain helpers (`LedGain`, `CompensationGainFor*`). `FrameStats` is the input type shared with `drive.led_measure`.

### Data flow

`LedDriveModule` owns one driver and both controllers. In compensate it steps `CompensateController` / `CntrPairing` inside `BeginFrame` and posts `SetLight` / `SetContrastAsync`. In script it ticks `ScriptController` on its own thread and calls `SendScript`. The driver writes the bytes and reports through `Status()` what was written and when; the module lands each write `led_latency_ms` / `cntr_latency_ms` later.

### Threading

- `BarcoAsciiDriver` starts one worker thread in `Open`. It sends queued light, CNTR and sequences, keeping `min_command_gap_ms` and the burst pause.
- `stateMutex` guards the queue and status; `portMutex` (a timed mutex) serialises port access. Synchronous calls take `portMutex` on the caller's thread.
- Generation counters (`lightGeneration`, `contrastGeneration`) drop a value superseded while it waited for the port.
- After repeated write failures (or a failed read) the port is closed and reopened with a back-off of 2 s doubling to 30 s. After a reopen the worker repeats the ECHO handshake and re-sends the light and CNTR it believes.
- Asynchronous calls never block on I/O, so the render thread may call them. `SendScript` can block up to `ack_timeout_ms` (read-back on): module thread only.
- Reply waits use the real clock; gaps and back-offs use `Clock`.
- The connection test runs on the caller's thread: VP's dialog after `SuspendLedDriveForTest`, or Config's background task.

### Config keys actually read

- `[projector.default]` (strict rules in led/LedDriveConfig.h, `ProjectorRules`). Read by the driver: `protocol` (`barco_ascii` | `none`, default none), `port` (COM1), `baud` (19200), `light_min` / `light_max` (auto = query), `ack_timeout_ms` (300), `min_command_gap_ms` (200), `max_levels_per_command` (12), `burst_commands` (0) / `burst_pause_ms` (5000), `read_back` (off), `echo` (on), `terminator` (auto | cr | literal_cr | script).
- Same section, read by the module and controllers: `led_latency_ms` (80), `led_settle_ms` (300), `cntr_latency_ms` (80), `cntr_settle_ms` (0), `cntr_max_usable` (74), `dmd_clip_cntr` (59), `flux_table`.
- `[led_drive.default]` (alias `[dynamic_drive.default]`; `[led_drive.*]` wins when both exist) holds the controller keys: `enabled` (off), `mode` / `command_source`, `tick_ms`, `curve`, `steps_*`, `led_floor*`, `led_ceiling`, `rate_*`, `deadband`, `highlight_*`, `script_*`, `cntr_assist*`, `cut_*`, `ftb_*` and more (full list in `LedDriveRules`). `profile_file` names an LED profile merged under the main section.
- The module talks to the port only when `enabled` is on and `mode` is compensate or script (`EffectiveProtocol`); otherwise the dry-run driver is used.
- Mismatch: `led_compensation` (Settings below, slots.json, MANIFEST) is not a config key. It is only the internal shader rule name in LibplaceboVideoRenderer.cpp.

### What the user sees when it falls back

- No port, a port that fails to open, or another VP/filter holding the drive mutex (`LedDriveArbiter`): the module continues as a dry run. Nothing is sent and the picture gain stays off (the gamma boost still applies). Visible only as the log line "Dynamic Drive: <reason>; continuing as a dry run" and the crash-state "Dynamic Drive" line ("not driving the projector (dry run)").
- The listed "Fallbacks engaged" entry `Projector: not connected (LED drive off)` is not implemented: the renderer never engages any LED fallback id.
- Repeated write failures: log lines ("projector writes failed in a row; ... reopening in N ms", then "reopened"). The drive recovers by itself.
- Slot `drive.projector_control` off (`pipeline_slots_off` or the Original pathway): the module is not started; log "Dynamic Drive: disabled (slot drive.projector_control off ...)".
- Exit restore not confirmed or timed out: log only ("LED restore: not confirmed / timed out").
- Connection test: a PASS/FAIL summary on the Config page; refused while video plays.

### Dependencies (to lift it)

- led/LedDriveConfig.h (`ProjectorConfig`, `LedDriveConfig`, parsing and validation; pulls in ConfigFile.h, ConfigSchema.h, DisplayRuleExpression.h).
- DebugLog.h, pch.h, and Win32 for SerialPort.cpp. No renderer or libplacebo dependency.
- LedController.h depends only on LedDriveConfig.h. It is control logic, not transport; `drive.led_measure` and `drive.led_compensation` include it.
- Compiled into VideoProcessor-Lib. SerialPort, BarcoAsciiDriver, ProjectorDriver and ConnectionTest are also compiled standalone by VideoProcessor-Config and VideoProcessor-ConfigTests.

### Test it standalone

No hardware needed: the tests use a fake serial port and `ManualClock`.

- `LedDriveProtocolTests`: Barco framing, gaps, read-back, reopen, startup and exit restore, connection test.
- `LedDriveControllerTests`: `ScriptController` against beta_22, tick for tick.
- `LedScriptLookaheadTests`: `PlanScriptLookahead`.
- `LedScriptCorrelationTests`: a no-op unless `VP_TEST_LED_CORRELATION=1` and `VP_TEST_REAL_FOOTAGE` are set (it runs tools\led_script_sim.py).
- `BugHunt3ShutdownTests`: bounded restore, connection-test cancel.
- Run one class: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LedDriveProtocolTests" /Logger:"trx;LogFileName=x.trx"`. Tests run single-threaded; read the TRX.

## Settings

- Existing keys: `[projector.default]` (all transport and latency keys) and the controller keys of `[led_drive.default]`; see "Config keys actually read". The listed `led_compensation` is not a config key.
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Projector: not connected (LED drive off). Not implemented in code (no `EngageFallback` call); today the dry run shows only in the log and the crash-state line.
- Behaviour: No serial projector, or the port is held by another owner -> dry run: nothing sent, no picture gain (gamma boost still applied).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA` (none of this slot's tests use the GPU).

Placeholder tests:
- (none yet)

Isolation tests:
- BarcoAsciiDriver tests (`LedDriveProtocolTests`)
- LedController tests (`LedDriveControllerTests`, `LedScriptLookaheadTests`)

Contract tests:
- (none yet)

Chain tests:
- (none yet)

## Known issues / open items

- Separate CNTR latency already exists in code (`cntr_latency_ms` 80, `cntr_settle_ms` 0 in `[projector.default]`); still to measure (phone slow-mo) and set if it differs from the LED's.
- The "Fallbacks engaged" entry is not wired (see Fallbacks).
- LedController.* (control logic) lives here and LedDriveArbiter.* (the port mutex) lives in `drive.led_compensation`; the split does not follow transport vs control.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/drive/projector_control/LedController.cpp`
- `src/VP-V/modules/drive/projector_control/BarcoAsciiDriver.cpp`
- `src/VP-V/modules/drive/projector_control/ProjectorDriver.cpp`
- `src/VP-V/modules/drive/projector_control/SerialPort.cpp`
- `src/VP-V/modules/drive/projector_control/LedController.h`
- `src/VP-V/modules/drive/projector_control/BarcoAsciiDriver.h`
- `src/VP-V/modules/drive/projector_control/ProjectorDriver.h`
- `src/VP-V/modules/drive/projector_control/SerialPort.h`
- `src/VP-V/modules/drive/projector_control/ConnectionTest.cpp`
- `src/VP-V/modules/drive/projector_control/ConnectionTest.h`
- `src/VP-V/modules/drive/projector_control/LedClock.h`
- `src/VP-V/modules/drive/projector_control/LedShutdownRestore.h`

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/led/ConnectionTest.h`
- `src/VideoProcessor-Lib/led/SerialPort.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `led_compensation`.
- Tiers: not tiered.
- VP-V page, Dynamic Drive tab: not tiered (Not a quality/cost choice: Dynamic Drive is set up on its own page (projector, script/compensate).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#7-dynamic-drive](../../../../../docs/settings/VP-settings-reference.md#7-dynamic-drive).
- Fallback entry ("Fallbacks engaged"): `Projector: not connected (LED drive off)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): BarcoAsciiDriver tests; LedController tests.
- Contract (formats, geometry, fallback entry): MISSING.
- Chain (total delay at the 4/3/2-frame budgets): n/a.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
