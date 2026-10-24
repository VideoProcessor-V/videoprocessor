# Status, fallbacks + diagnostics (`control.diagnostics`)

Unit: **control** | side: control | order: 920 | status: **partial** | hook point: host

## Purpose / why

One "Fallbacks engaged: (list)" heading in the Ctrl+I overlay, the Config status and the problem report (Ctrl+Alt+D); the crash state ring (a rolling state file, kept as "crash state (N)" after an unclean exit, at most 10); the report-a-problem bundle. Logging itself (`DebugLog`) stays in src/VideoProcessor-Lib.

## Contract

- Input: fallback events (`Engage(id, reason)` / `Release(id)`). Today only the VP Renderer DLL publishes them (`FallbackStatus::Registry` in LibplaceboVideoRenderer.cpp). State values from the GUI (`CrashState::Recorder::SetValue`). Recent log lines from `DebugLog::RecentLines`.
- Output: the "Fallbacks engaged: ..." heading; one brief transient notice per fallback id per session; the live crash-state file and kept "crash state (N).txt/.dmp" files; `logs\reports\vp-report-<time>.zip`.
- Geometry: n/a
- Delay share: n/a
- Look-ahead: n/a

## How the code works

### Crash state ring (migrated: `CrashState.h/.cpp`)

Public API (namespace `CrashState`):

- `PreserveLeftover(directory, keep = 10)`: at start-up, renames every leftover live file whose instance is gone to "crash state (N).txt" (and its ".dmp"), oldest first, N one above the highest kept; then deletes the oldest beyond `keep`. Also keeps the single `crash-state.txt` of older versions. A running instance's files are never touched.
- `KeptNumbers(directory)`, `InstanceFileName(id, ext)`.
- `Recorder`: `Start(directory, recentLines, periodMs = 1000, instanceId = 0)`, `SetValue(key, value)`, `WriteNow()`, `Stop(clean)`, `Text()`, `LivePath()`, `LiveDumpPath()`, `InstallCrashHandler()`, `WriteMinidump()`, `WriteFromHandler()`.
- `Global()`: the process-wide recorder the app uses.

Data flow:

1. VideoProcessorApp.cpp calls `PreserveLeftover` on the log folder (the folder of `DebugLog::GetLogFilePath()`), logs each kept file, sets the `build` value, calls `Start` and `InstallCrashHandler`.
2. The live file is `crash-state-<pid>.txt`. Each instance holds `crash-state-<pid>.lock` open exclusively with delete-on-close; `PreserveLeftover` treats an instance as running while that lock cannot be opened.
3. Every second the writer thread composes a header (times, pid, working set), a `[state]` section (the values) and a `[recent log]` section, and writes it atomically: `.tmp`, `FlushFileBuffers`, `MoveFileEx` replace with write-through.
4. VideoProcessorDlg.cpp sets values (`source`, `exit`, ...) from its diagnostics tick. A clean exit calls `Stop(true)`, which deletes the live file and dump. Forced shutdown, session end with a kept state, or a crash leaves it for the next start.
5. `InstallCrashHandler` hooks the unhandled-exception filter, `std::terminate`, `SIGABRT`, CRT invalid-parameter and pure-call. Each writes a final state with a `crash` value and a minidump (`crash-state-<pid>.dmp`, dbghelp loaded on demand), then chains on. /GS and heap-corruption fail-fast exits bypass all handlers and leave no `crash` line.

Threading: one writer thread per `Recorder`. `values` and the last recent lines are under one mutex; writes are under a second mutex. The handler path never blocks: it tries each lock for a bounded time (200 ms / 500 ms), ignores re-entry on the same thread, and reuses the recent lines of the last periodic write instead of calling the log. `Text()` (used by the report and by `Stop(false)`) calls the lines source on the caller's thread.

### Report-a-problem bundle (migrated: `StateReport.h/.cpp`, `StateReportSettings.h`)

Public API (namespace `StateReport`): `Write(Inputs)` returns a `Result` (path, entries, removed files, error). Helpers: `Redact`, `IsSecretKey`, `CurrentRedactionContext`, `FileName`, `ListReports`, `ApplyRetention` (newest 20), `RemoveStalePartials` (`.zip.partial` older than 5 min), `Sha256File`, `ReadFileTail`, `NewestKeptCrashState`, `CollectSystemJson`, `MakeThumbnailPng`, `ManifestJson`, `LiveSettingsJson`, `ReadmeText`, `JsonEscape`, and `AutoReportLimiter` (at most one automatic report per 10 min, 5 per run; a busy writer spends no slot). `StateReportSettings`: `Read` (the two `[logging]` switches), `TryParseChord`, `ResolveShortcut`.

Data flow:

1. Triggers: the `state_report` shortcut ("hotkey"), the system-menu item "Save problem report" ("menu"), and with `auto_report` on, renderer resets for liveness recovery, source-gap recovery, or a failed reset ("auto:<reason>", through `MaybeAutoReport` and the limiter).
2. `CVideoProcessorDlg::SaveStateReport` (GUI thread) gathers the inputs: build identity, EXE/plugin paths, both DiagnosticHistory rings, `CrashState::Global().Text()`, live settings (including the "Fallbacks engaged" heading), the config file paths, the recent log lines. A second request while one runs shows "already saving" (the flag frees itself after 2 min).
3. A detached worker thread reads the log tail (512 KiB), waits up to 4 s for the optional thumbnail (via the renderer's screenshot path, scaled to 480 px by WIC), then calls `StateReport::Write`.
4. `Write` redacts every text (paths outside the VP folder, secret-like keys, e-mail, URLs, user and computer names), builds a store-only ZIP in memory (StoreZipWriter.h), writes it atomically (`.partial`, flush, rename) to `<vp>\logs\reports\vp-report-<yyyymmdd-hhmmss-mmm>.zip`, then applies retention.
5. The worker posts the result back; the GUI logs it and shows a toast "saved <file>" or "failed (<error>)".

### Fallbacks engaged (live elsewhere: `src/VP-V/core/FallbackStatus.h`)

Header-only, pure. `FallbackStatus::Registry` (thread-safe: `Engage`, `Release`, `Engaged`, `Entries`, `Heading`, `Clear`); `Format` gives "Fallbacks engaged: a (why); b (why)" or "Fallbacks engaged: none"; `Ids` parses a heading; `Notices::Take` returns the ids not yet announced this session. The renderer publishes `Heading()` through `IVideoRenderer::GetFallbacksStatus`. The GUI polls it with the crash-state lines (`UpdateFallbacksStatus`), shows it in the Ctrl+I overlay (StatsOverlayWindow.cpp), re-publishes the Config status, adds it to the report's live settings, and shows one transient notice per new id (or a status line "Fallback engaged: <id>" when the overlay is switched off).

Ids engaged in code today (LibplaceboVideoRenderer.cpp): `capture GPU unpack`, `native RGB GPU unpack`, `render-ahead`, `delay budget`, `look-ahead`, `entrance once`, `pipeline order`, `pipeline settings`, `re-base precision`, `calibration LUT`, and six `Dolby Vision ...` entries. The DirectShow renderer publishes none.

`SlotTiming.h` (core, header-only) is the per-slot GPU-time accumulator and 3090 projection; it is used only by PipelineSkeletonTests so far.

### Config keys actually read

- `[shortcuts] state_report`: default Ctrl+Alt+D; empty = no shortcut; an invalid chord binds none and reports an error.
- `[logging] auto_report`: Boolean, default false.
- `[logging] report_thumbnail`: Boolean, default false. The only way a report holds pixels.
- An invalid Boolean keeps the default and is reported by `StateReportSettings::Read`.
- The crash state has no keys; its folder follows the log file path.

### User-visible fallbacks / notices

This slot has no fallback of its own. It owns the heading format and the once-per-session notices. Report outcomes are toasts ("saving...", "saved <file>", "failed (<error>)", "already saving"). A kept crash state is logged at the next start ("Crash state: the previous run did not exit cleanly; kept ...").

### Dependencies to lift it

- `CrashState`: Win32 only (Psapi `K32GetProcessMemoryInfo`, dbghelp loaded at run time) and the C++ standard library; `pch.h`.
- `StateReport`: `StoreZipWriter.h`, `DiagnosticHistory.h` (one constant), bcrypt, dxgi, windowscodecs (WIC), registry and display-config APIs.
- `StateReportSettings`: `ConfigFile.h`, `ShortcutChord.h`, `windows.h`.
- `FallbackStatus.h`: C++ standard library only.
- All are compiled into VideoProcessor-Lib today.

### Test it standalone

Classes in src/VideoProcessor-Test (VideoProcessor-Test.dll): `CrashStateTests`, `StateReportTests`, `StateReportSettingsTests`, `StoreZipWriterTests`, `FallbacksEngagedTests` (R6BaselineTests.cpp), `BugHunt3ShutdownTests` (`CrashStateFilesArePerInstance`, `SessionEndRestoresTheLightTopologyAndCrashState`), `ShortcutDefaultsTests` (the `state_report` default). None needs a GPU; they write only to `%TEMP%`. Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~CrashStateTests|FullyQualifiedName~StateReport|FullyQualifiedName~StoreZipWriterTests|FullyQualifiedName~FallbacksEngagedTests" /Logger:"trx;LogFileName=diagnostics.trx"`

Read the TRX for the result.

## Cost

Efficiency review 2026-10-24 (code reading only; estimates from loop bounds and file sizes x frequency, not measured). Ranked by saving.

1. **Crash-state write, once per second** (writer thread, never the render or GUI thread). Each write composes the header, ~20 `[state]` values and up to 300 recent lines from the last 5 s (`DebugLog::RecentLines(5000, 300)`): about 2-60 KB depending on log volume. It writes `.tmp` through `std::ofstream`, reopens it with `CreateFileA` only to call `FlushFileBuffers`, then `MoveFileEx(... MOVEFILE_WRITE_THROUGH)`. That is two opens and two forced flushes per second, 86,400 rewrites a day (~0.2-5 GB/day written), and an antivirus scan of each closed `.tmp`.
   - Fix a: write the `.tmp` through one `CreateFile` handle (`WriteFile`, then `FlushFileBuffers` on the same handle). Saves one open/close and one scan trigger per second. Byte-identical file: yes. Risk: low.
   - Fix b: skip the rewrite when `[state]` and `[recent log]` are unchanged since the last write (paused, idle, no signal). Removes most writes when nothing happens. Byte-identical: no (the `written:` / `uptime:` header stays at the last change). Risk: low.
   - Keep the flush and the write-through: they are the power-loss guarantee (audit 2026-10-16).
2. **Problem report, GUI-thread part** (per report, `CVideoProcessorDlg::SaveStateReport`). Two `ConfigFile::Load` parses (only the paths are used), both DiagnosticHistory rings serialised, `CrashState::Text()`, and a 600-line `recentLog` string that is used only when the log tail cannot be read. Estimated a few ms to ~20 ms GUI hitch per report (not measured). Fix: pass the config paths without parsing them and build the fallback log text on the worker. Byte-identical report: yes (same inputs). Risk: low.
3. **Report thumbnail** (`report_thumbnail` on): the rendered-output capture blocks the render thread for ~30-80 ms per capture. Costed and fixed under colour.tone_gamut (`AnalyzePackedR10` is there).
4. **Problem report, worker part** (per report, off the GUI thread): SHA-256 of `VideoProcessor.exe` and each renderer DLL (BCrypt, ~0.5-2 GB/s: tens of ms for tens of MB), redaction line by line (four passes plus word replacement on each of ~4,000 lines of the 512 KiB log tail, a `substr` copy per line), the in-memory store ZIP, flush and write-through rename. Acceptable for an event; no change.
5. **`WriteNow` copies the recent lines twice** (into `lines`, then into `lastRecent` under the state mutex): up to 300 string copies a second, ~30 µs/s. Fix: compose, then move `lines` into `lastRecent`. Byte-identical: yes. Risk: low.
6. **GUI 1-second tick**: ~17 `SetValue` calls (map insert under the mutex, `CStringA` conversions), ~10-20 µs/s. No change. The 600-line ring that feeds the recent lines (`DebugLog::RememberRecent`, a timestamp and a copy per log line, kept even with file logging off) is logging's cost, not this slot's.

Cost when off: the crash state has no switch, so item 1 always runs. The report costs nothing until triggered; `auto_report` off and `report_thumbnail` off cost nothing.

## Settings

- Existing keys: `[shortcuts] state_report`, `[logging] auto_report`, `[logging] report_thumbnail`
- Planned keys: (none)
- Tiers: n/a

## Fallbacks

- "Fallbacks engaged" entry: (n/a)
- Behaviour: Owns the format: 'Fallbacks engaged: none' when clean.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_GPU=hardware` (`VP_TEST_ADAPTER=NVIDIA` picks the GeForce). This slot's tests need no GPU.

Placeholder tests:
- (none yet)

Isolation tests:
- `FallbacksEngagedTests` (FallbackStatus), `CrashStateTests`, `StateReportTests`, `StateReportSettingsTests`, `StoreZipWriterTests`

Contract tests:
- every slot's fallback entry reaches the overlay, Config status and Ctrl+Alt+D (planned; no test yet)

Chain tests:
- (none yet)

## Known issues / open items

- audit all log-only fallbacks (item 45); still log-only today include the re-base failure pause, the AUTO Limited -> composed swapchain switch and the NLS hook-failure safe fit (see those slots' READMEs)
- the registry is `FallbackStatus::Registry` in src/VP-V/core/FallbackStatus.h (from fix/r6-baseline); reuse it, do not duplicate

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/control/diagnostics/StateReport.cpp`
- `src/VP-V/modules/control/diagnostics/CrashState.cpp`
- `src/VP-V/modules/control/diagnostics/CrashState.h`
- `src/VP-V/modules/control/diagnostics/StateReport.h`
- `src/VP-V/modules/control/diagnostics/StateReportSettings.h`
- `src/VP-V/core/SlotTiming.h`
- `src/VP-V/core/FallbackStatus.h`
- `src/VideoProcessor-GUI/StatsOverlayWindow.cpp`
- `src/VideoProcessor-Lib/DebugLog.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0001 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: none yet.
- Tiers: n/a.
- Fallback entry ("Fallbacks engaged"): none (control slot).

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): FallbackRegistry unit tests.
- Contract (formats, geometry, fallback entry): every slot's fallback entry reaches the overlay, Config status and Ctrl+Alt+D.
- Chain (total delay at the 4/3/2-frame budgets): n/a.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
