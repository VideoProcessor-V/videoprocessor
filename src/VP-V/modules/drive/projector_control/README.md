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

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `projector.ack_timeout_ms` | [projector] ([projector.default]) | int | 20 to 5000 (ms) | 300 | Dynamic Drive page > Advanced projector card > Reply timeout | n/a (VP-only) | How long a reply to a command is waited for (read_back on) | not tiered |
| `projector.baud` | [projector] ([projector.default]) | int | 1200 to 921600 | 19200 | Dynamic Drive page > Advanced projector card > Baud rate | n/a (VP-only) | Serial speed of the projector's RS-232 port | not tiered |
| `projector.burst_commands` | [projector] ([projector.default]) | int | 0 to 1000 | 0 (no pause) | Dynamic Drive page > Advanced projector card > Burst commands | n/a (VP-only) | Pause after every this many commands (Barco documents 5 s after 20); 0 = never pause | not tiered |
| `projector.burst_pause_ms` | [projector] ([projector.default]) | int | 0 to 60000 (ms) | 5000 | Dynamic Drive page > Advanced projector card > Burst pause | n/a (VP-only) | Length of the pause after burst_commands commands | not tiered |
| `projector.cntr_latency_ms` | [projector] ([projector.default]) | int | 0 to 2000 (ms) | 80 | Dynamic Drive page > Projector connection card > Contrast latency | n/a (VP-only) | Dead time before a CNTR command takes effect; script smoothing times VP's gain with it and compensate models CNTR with it | not tiered |
| `projector.cntr_max_usable` | [projector] ([projector.default]) | int | 50 to 100 | 74 | Dynamic Drive page > Projector response card > Highest useful contrast | n/a (VP-only) | Highest CNTR that still adds light (light-meter step); the most the CNTR assist may use | not tiered |
| `projector.cntr_settle_ms` | [projector] ([projector.default]) | int | 0 to 5000 (ms) | 0 (dead time only) | Dynamic Drive page > Projector response card > CNTR settle | n/a (VP-only) | Time constant of CNTR's settling towards a new level after the dead time; 0 = dead time only | not tiered |
| `projector.dmd_clip_cntr` | [projector] ([projector.default]) | int | -1 (= cntr_max_usable) or 50 to 100 | 59 | Dynamic Drive page > Projector response card > White clips at contrast | n/a (VP-only) | The CNTR at which code 235 reaches the DMD's maximum at full LED; above it more CNTR adds no highlight light (the highlight floor, the assist and the fade-up ceiling stop there); 59 until measured | not tiered |
| `projector.echo` | [projector] ([projector.default]) | bool | on, off | on | Dynamic Drive page > Advanced projector card > Echo | n/a (VP-only) | Asks the projector to echo commands (ECHO 1) while detecting the line framing and is re-sent after the serial port reopens (skipped with terminator script) | not tiered |
| `projector.flux_table` | [projector] ([projector.default]) | string | lpw:relative_flux pairs, lpw ascending 0-255, flux rising, above 0 and at most 10, e.g. 60:0.55, 80:0.78, 100:1.00 | empty (flux proportional to lpw) | Dynamic Drive page > Advanced projector card > Flux table | n/a (VP-only) | The LED's measured relative flux per LPW1 level; the model inverts it to a level | not tiered |
| `projector.led_latency_ms` | [projector] ([projector.default]) | int | 0 to 2000 (ms) | 80 | Dynamic Drive page > Projector connection card > LED latency | n/a (VP-only) | Command transmission plus projector response as pure dead time: the LED reaches a commanded level this long after the command (measured once with a camera) | not tiered |
| `projector.led_settle_ms` | [projector] ([projector.default]) | int | 0 to 5000 (ms) | 300 | Dynamic Drive page > Projector response card > LED settle | n/a (VP-only) | After the dead time the LED settles towards the new level as a first-order lag with this time constant (63% after it); 0 = dead time only; fast-biased until measured | not tiered |
| `projector.light_max` | [projector] ([projector.default]) | string | auto, or a level 0 to 255 | auto (query the projector, ?M) | Dynamic Drive page > Advanced projector card > Highest light level | n/a (VP-only) | Highest LPW1 level the projector accepts; auto asks the projector | not tiered |
| `projector.light_min` | [projector] ([projector.default]) | string | auto, or a level 0 to 255 | auto (query the projector, ?N) | Dynamic Drive page > Advanced projector card > Lowest light level | n/a (VP-only) | Lowest LPW1 level the projector accepts; auto asks the projector | not tiered |
| `projector.max_levels_per_command` | [projector] ([projector.default]) | int | 0 to 255 (0 = no limit) | 12 | Dynamic Drive page > Advanced projector card > Largest drop per command | n/a (VP-only) | Largest LPW1 drop in one command so the black floor never falls in big steps; later commands continue the ramp | not tiered |
| `projector.min_command_gap_ms` | [projector] ([projector.default]) | int | 0 to 5000 (ms) | 200 | Dynamic Drive page > Advanced projector card > Command gap | n/a (VP-only) | Never two commands closer than this (Barco documents at least 500 ms; the old script ran 200 ms gaps) | not tiered |
| `projector.port` | [projector] ([projector.default]) | string | COM1 to COM256 | COM1 | Dynamic Drive page > Projector connection card > Port | n/a (VP-only) | The projector's RS-232 serial port; the optional light sensor (ambient.port) must use another port | not tiered |
| `projector.protocol` | [projector] ([projector.default]) | enum | barco_ascii, none | none | Dynamic Drive page > Projector connection card > Protocol | n/a (VP-only) | The projector protocol; none = the dry-run driver, nothing is sent (Config sets barco_ascii when a command source other than none is chosen) | not tiered |
| `projector.read_back` | [projector] ([projector.default]) | bool | on, off | off | Dynamic Drive page > Projector connection card > Read back | n/a (VP-only) | On: wait for each reply, query the range and state; off: send only, never wait for or ask the projector anything (the old script's way) | not tiered |
| `projector.terminator` | [projector] ([projector.default]) | enum | auto, cr, literal_cr, script | auto | Dynamic Drive page > Advanced projector card > Line ending | n/a (VP-only) | Command framing: cr, literal cr+CR, or the old script's exact bytes; auto probes cr then literal cr and falls back to the script's bytes without waiting for replies | not tiered |


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

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/led/ConnectionTest.h`
- `src/VideoProcessor-Lib/led/SerialPort.h`

## Config cards (`config_ui/`)

`config_ui/ProjectorCards.h/.cpp` builds the projector's cards of the Dynamic Drive page (the Config editor's former `createLedDrivePage`, M2 lane C, P7-3): "Projector connection" (protocol, COM port, LED and contrast response times, read-back, the RS-232 connection test button and its result), "Projector response" (settle times, the contrast limits) and the collapsed "Advanced projector" serial details. `led_compensation/config_ui/LedDrivePage` places them in its card grid at their old positions and reads the few controls its show/hide rules use. `DriveConfigBase.h/.cpp` holds what both builders share (the editor's field helpers through `ConfigPageHost::EditorKit`, the document write path, the clamp flag and the number boxes). The connection test runs on a helper thread that outlives page rebuilds; the provider's `OnEditorClosing` joins it (`ProjectorCards::JoinTest`). Tests: `led_compensation/config_ui/tests/LedDrivePageTests.cpp`.

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
