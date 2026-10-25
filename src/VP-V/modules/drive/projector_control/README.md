# Projector control (`drive.projector_control`)

Unit: **drive** | side: control | order: 900 | status: **built** | hook point: host

## Purpose / why

RS-232 projector transport: the driver (Barco ASCII: LPW1 light, CNTR contrast, LDMM/LDCx custom dim), the serial port, the connection test, the bounded exit restore, the clock, and the cross-process port ownership (`LedDriveArbiter`). Since the WP-15 LED re-cut this folder is transport only: the controllers that decide the commands (`LedController`) are in `drive.led_compensation`. Serves `mode: script` and `mode: compensate`; `log_only` and `picture_only` never open the port. Latency timing comes from `led_latency_ms` / `cntr_latency_ms`.

## Contract

- Input: LED drive decisions (light and CNTR targets from the controllers, posted by `LedDriveModule` in `drive.led_compensation`)
- Output: serial commands (`:LPW1 n`, `:CNTR n`, `:LDMM n` with a CR) on one COM port, plus a `DriverStatus` (values written, write times, next send slot)
- Geometry: n/a
- Delay share: no picture delay. `led_latency_ms` / `cntr_latency_ms` (dead time) and `led_settle_ms` / `cntr_settle_ms` (first-order settle) model when a command takes effect; the module uses them to time the picture gain and to pick the frame a command is for
- Look-ahead: none in this folder; the controllers (`drive.led_compensation`) take queued-frame statistics from `drive.led_measure` (`LedLookahead`)

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
- `ArbitratedSerialPort` / `ArbitratedSerialPortFactory` (LedDriveArbiter.h): wraps the serial port so `Open` first takes the machine-wide mutex `Global\VideoProcessorLedDrive`. A second owner (VP.exe vs the VP Renderer filter) fails `Open` and its module runs as a dry run. `LedDriveModule` gets the factory from `DynamicDriveHost` (the WP-15 host adapter).

### Data flow

`LedDriveModule` (`drive.led_compensation`) owns one driver (created through `CreateProjectorDriver` with the arbitrated port factory) and the controllers. In compensate it steps `CompensateController` / `CntrPairing` inside `BeginFrame` and posts `SetLight` / `SetContrastAsync`. In script it ticks `ScriptController` on its own thread and calls `SendScript`. The driver writes the bytes and reports through `Status()` what was written and when; the module lands each write `led_latency_ms` / `cntr_latency_ms` later.

### Threading

- `BarcoAsciiDriver` starts one worker thread in `Open`. It sends queued light, CNTR and sequences, keeping `min_command_gap_ms` and the burst pause.
- `stateMutex` guards the queue and status; `portMutex` (a timed mutex) serialises port access. Synchronous calls take `portMutex` on the caller's thread.
- Generation counters (`lightGeneration`, `contrastGeneration`) drop a value superseded while it waited for the port.
- After repeated write failures (or a failed read) the port is closed and reopened with a back-off of 2 s doubling to 30 s. After a reopen the worker repeats the ECHO handshake and re-sends the light and CNTR it believes.
- Asynchronous calls never block on I/O, so the render thread may call them. `SendScript` can block up to `ack_timeout_ms` (read-back on): module thread only.
- Reply waits use the real clock; gaps and back-offs use `Clock`.
- The connection test runs on the caller's thread: VP's dialog after `SuspendLedDriveForTest` (the renderer forwards to `DynamicDriveHost::SuspendForTest`, which stops the module and frees the port), or Config's background task.
- `LedDriveArbiter` keeps its named mutex on a thread of its own (a Win32 mutex belongs to the thread that took it).

### Config keys actually read

- `[projector.default]` (strict rules in VP-V/modules/drive/led_compensation/LedDriveConfig.h, `ProjectorRules`). Read by the driver: `protocol` (`barco_ascii` | `none`, default none), `port` (COM1), `baud` (19200), `light_min` / `light_max` (auto = query), `ack_timeout_ms` (300), `min_command_gap_ms` (200), `max_levels_per_command` (12), `burst_commands` (0) / `burst_pause_ms` (5000), `read_back` (off), `echo` (on), `terminator` (auto | cr | literal_cr | script).
- Same section, read by the module and controllers: `led_latency_ms` (80), `led_settle_ms` (300), `cntr_latency_ms` (80), `cntr_settle_ms` (0), `cntr_max_usable` (74), `dmd_clip_cntr` (59), `flux_table`.
- `[led_drive.default]` (alias `[dynamic_drive.default]`; `[led_drive.*]` wins when both exist) holds the controller keys, read by `drive.led_compensation` (see its README). The driver itself reads only `enabled` / `mode` / `command_source` through `EffectiveProtocol`.
- The module talks to the port only when `enabled` is on and `mode` is compensate or script (`EffectiveProtocol`); otherwise the dry-run driver is used.
- slots.json / MANIFEST list `projector` (the `[projector.default]` group). The old `led_compensation` entry (only a shader rule name) is gone.

### What the user sees when it falls back

- No port, a port that fails to open, or another VP/filter holding the drive mutex (`LedDriveArbiter`): the module continues as a dry run. Nothing is sent and the picture gain stays off (the gamma boost still applies). `LedDriveModule::FallbackEntries()` holds "projector (not connected: <error>; LED drive off (dry run))"; with the WP-15 call-site patch applied, `DynamicDriveFrame` merges it into "Fallbacks engaged" every frame (Ctrl+I, Config status, Ctrl+Alt+D, one-time notice). Also the log line "Dynamic Drive: <reason>; continuing as a dry run" and the "Dynamic Drive" status line ("not driving the projector (dry run)").
- Repeated write failures: log lines ("projector writes failed in a row; ... reopening in N ms", then "reopened"). The drive recovers by itself.
- Slot `drive.projector_control` off (`pipeline_slots_off` or the Original pathway): the module is not started; log "Dynamic Drive: disabled (slot drive.projector_control off ...)".
- Exit restore not confirmed or timed out: log only ("LED restore: not confirmed / timed out").
- Connection test: a PASS/FAIL summary on the Config page; refused while video plays.

### Dependencies (to lift it)

- VP-V/modules/drive/led_compensation/LedDriveConfig.h (`ProjectorConfig`, parsing and validation; pulls in ConfigFile.h, ConfigSchema.h, DisplayRuleExpression.h).
- DebugLog.h, pch.h, and Win32 for SerialPort.cpp. No renderer or libplacebo dependency.
- LedDriveConfig.h lives in `drive.led_compensation` (the drive's config schema); ProjectorDriver.h includes it for `ProjectorConfig`, so lifting this folder needs that header too (WP-18 may move the schema to `control.config`).
- LedDriveArbiter.cpp: Win32 named mutex and a thread; DebugLog.h.
- Compiled into VideoProcessor-Lib. SerialPort, BarcoAsciiDriver, ProjectorDriver and ConnectionTest are also compiled standalone by VideoProcessor-Config and VideoProcessor-ConfigTests.

### Test it standalone

No hardware needed: the tests use a fake serial port and `ManualClock`.

- `LedDriveProtocolTests`: Barco framing, gaps, read-back, reopen, startup and exit restore, connection test.
- `LedDriveArbiterTests`: one owner at a time across ports and processes.
- `BugHunt3ShutdownTests`: bounded restore, connection-test cancel.
- Run one class: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LedDriveProtocolTests" /Logger:"trx;LogFileName=x.trx"`. Tests run single-threaded; read the TRX.

## Cost

Efficiency review 2026-10-24 (code reading only, nothing measured). This module is cheap: the serial side is a handful of commands per second.

1. **Compensate log string every output frame.** Moved with `LedController` to `drive.led_compensation` (see its Cost section).
2. **`FluxModel::LevelFor` linear scans.** Moved with `LedController` to `drive.led_compensation`.
3. **Serial side: nothing to gain.** Checked: the worker sleeps on a condition variable, `stateMutex` is released before port I/O and during gap waits (render-thread `Status()` and `Set*` never wait on I/O), `Status()` copies a small struct with a short (SSO) string, and `ReadLine` sets `SetCommTimeouts` once per read (a few per command, at most about 5 commands/s at `min_command_gap_ms` 200).

When off: LED disabled creates no module, driver or thread (zero cost). log_only and picture_only use the dry-run `NoneDriver` (no port, no worker); the module thread wakes every `tick_ms` (400 ms).

## Settings

- Existing keys: `projector` (the `[projector.default]` group: all transport and latency keys); see "Config keys actually read".
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: projector (not connected: <error>; LED drive off (dry run)). Produced by `LedDriveModule::FallbackEntries()`; shown once the WP-15 call-site patch is applied (`DynamicDriveFrame` merges it).
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
- LedDriveArbiter tests (`LedDriveArbiterTests`)

Contract tests:
- dry run engages the projector fallback (`DynamicDriveAdapterTests.DryRunShowsTheProjectorFallback`)

Chain tests:
- (none yet)

## Known issues / open items

- Separate CNTR latency already exists in code (`cntr_latency_ms` 80, `cntr_settle_ms` 0 in `[projector.default]`); still to measure (phone slow-mo) and set if it differs from the LED's.
- The dry-run entry reaches the heading only after the WP-15 call-site patch (docs/design/wp15-led-callsites.md) is applied by the integrator.
- Re-cut done (WP-15): transport only here; `LedController` went to `drive.led_compensation`, `LedDriveArbiter` came here.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/drive/projector_control/BarcoAsciiDriver.cpp`
- `src/VP-V/modules/drive/projector_control/ProjectorDriver.cpp`
- `src/VP-V/modules/drive/projector_control/SerialPort.cpp`
- `src/VP-V/modules/drive/projector_control/BarcoAsciiDriver.h`
- `src/VP-V/modules/drive/projector_control/ProjectorDriver.h`
- `src/VP-V/modules/drive/projector_control/SerialPort.h`
- `src/VP-V/modules/drive/projector_control/ConnectionTest.cpp`
- `src/VP-V/modules/drive/projector_control/ConnectionTest.h`
- `src/VP-V/modules/drive/projector_control/LedClock.h`
- `src/VP-V/modules/drive/projector_control/LedShutdownRestore.h`
- `src/VP-V/modules/drive/projector_control/LedDriveArbiter.cpp`
- `src/VP-V/modules/drive/projector_control/LedDriveArbiter.h`

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/led/ConnectionTest.h`
- `src/VideoProcessor-Lib/led/SerialPort.h`
- `src/VideoProcessor-Lib/led/LedDriveArbiter.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `projector`.
- Tiers: not tiered.
- VP-V page, Dynamic Drive tab: not tiered (Not a quality/cost choice: Dynamic Drive is set up on its own page (projector, script/compensate).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#7-dynamic-drive](../../../../../docs/settings/VP-settings-reference.md#7-dynamic-drive).
- Fallback entry ("Fallbacks engaged"): `projector (not connected: <error>; LED drive off (dry run))`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): BarcoAsciiDriver tests; LedDriveArbiter tests.
- Contract (formats, geometry, fallback entry): dry run engages the projector fallback (DynamicDriveAdapterTests).
- Chain (total delay at the 4/3/2-frame budgets): n/a.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
