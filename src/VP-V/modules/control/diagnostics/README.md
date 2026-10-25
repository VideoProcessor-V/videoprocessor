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
2. `CVideoProcessorDlg::SaveStateReport` (GUI thread) calls the adapter's `VpDiagnostics::ReportRun` (DiagnosticsReportRun.h, WP-17): `TryBegin` (a second request while one runs shows "already saving"; the flag frees itself after 2 min), `NewInputs` (EXE/plugin paths, both DiagnosticHistory rings, `CrashState::Global().Text()`, the config file paths, the recent log lines), then adds what only the dialog has (build identity, live settings including the "Fallbacks engaged" heading), `RequestThumbnail`, and `Start`. `MaybeAutoReport` asks `AllowAutomatic` (auto_report, busy, limiter; skips are logged).
3. `ReportRun::Start`'s detached worker thread reads the log tail (512 KiB), waits up to 4 s for the optional thumbnail (via the renderer's screenshot path, scaled to 480 px by WIC; the folder honours `VP_TEST_SCREENSHOT_DIR`), then calls `StateReport::Write`. At exit the dialog calls `WaitForExit` (the thumbnail wait is cut short; the write is finished, bounded by the shutdown deadline).
4. `Write` redacts every text (paths outside the VP folder, secret-like keys, e-mail, URLs, user and computer names), builds a store-only ZIP in memory (StoreZipWriter.h), writes it atomically (`.partial`, flush, rename) to `<vp>\logs\reports\vp-report-<yyyymmdd-hhmmss-mmm>.zip`, then applies retention.
5. The worker posts the result back; `ReportRun::Finish` logs it, records a `state-report-saved` decision and returns the toast the dialog shows: "saved <file>" or "failed (<error>)".

### Diagnostic history rings (migrated: `DiagnosticHistory.h`, `RendererDiagnosticCodes.h`)

`DiagnosticHistory.h` (header-only, C++14): five lock-free seqlock rings per module (frames 4096, decisions 1024, LED 256, capture 256, stats 256 records of 120 bytes; ~736 KB), `Global()` one recorder per module (executable and renderer DLL each), `Recorder::RecordFrame/RecordDecision/RecordLed/RecordCapture/RecordStats` (never block, never allocate, ~20-40 ns), `ToJsonl` / `WriteJsonlToSink` (the report's `state-history.jsonl`, tagged `exe` / `renderer`, times relative to the report moment). `RendererDiagnosticCodes.h`: the stable `holdReason` codes of renderer frame rows (drops 1-10, holds 20-38; append, never renumber) and `ClassifyDejudderHold`. Writers: the GUI (capture ingress/rejects, 1 Hz stats, device events), the renderer (display rule, profiles, output mode, render-ahead gate, de-judder, ingest/drop/presented/repeated rows), `BarcoAsciiDriver` (LED commands), `ContentIntentObserver`.

### Raw capture recording (migrated: `RawCaptureRecorder.h/.cpp`, `RawCaptureCorpus.h`)

Opt-in per process: nothing happens unless `VP_RAW_CAPTURE_PATH` names a folder (never a source checkout; captured pixels are never committed). Other variables: `VP_RAW_CAPTURE_BURST` / `_SKIP` (automatic bursts after each new capture state), `_EVERY`, `_MAX_SECONDS` (30), `_MIN_FREE_GB` (30), `_QUEUE_MB` (2048), `_NOTE`. Continuous recording toggles with the `[shortcuts] raw_capture` shortcut (default Ctrl+Alt+W) or a `RECORD` / `STOP` file in the root. Layout `<root>\<yyyymmdd-hhmmss>-<kind>\segNN\`: `capture.txt` (format, card, capture state, HDR values, note), `frames.csv`, `frame_NNNNNN.bin` (raw bytes); a capture-state change starts the next segment. Frames are copied on the capture thread into pooled buffers and written by one background thread. The GUI feeds it (`OnCardState`, `OnVideoState`, `OnFrame`, `SetProcessingNote`; one line each in VideoProcessorDlg.cpp); the shortcut handler is `Toggle()`. `RawCaptureCorpus.h` reads these recordings (and StreamRecorder `--raw` folders) back for the DV corpus tests and tools (`tools/dv_synth`, `tools/dv_compare`). The DV comparison capture (feat/dv-l8, `DvCompareCapture.h` in input.signal) records its clips through a `RawCaptureRecorder` session.

### VP diagnostics adapter (WP-17: `DiagnosticsAdapter.h`, `DiagnosticsReportRun.h`)

One adapter (namespace `VpDiagnostics`, header-only, C++14) so the host files (Bill's) keep one call per hook point; the host lines are `docs/design/wp17-callsites.patch` (+ `.md`). Hook points:

1. **Env-gated test switches:** `TestFolderOverride(Screenshots | Shaders | ShaderCache)` reads `VP_TEST_SCREENSHOT_DIR` / `VP_TEST_SHADER_DIR` / `VP_TEST_SHADER_CACHE_DIR` (process environment, under 1024 characters; unset = the normal path). `TestAdapterFromEnvironment` picks the GPU named by `VP_TEST_ADAPTER` and logs the choice. Tests only; nothing in an ordinary launch sets them.
2. **Capture requested:** `DvSceneMinFields` streams VP's two fields (`scene_min_present`, `scene_min_nits`) into the rendered-output capture sidecar's `dolby_vision` object, in the host's stream state (the capture and the rest of its sidecar are Bill's). `RenderedFrameCaptures` holds the captures that ride on rendered frames (`Register`, `Configure(command, folder)`, `OnFrameRendered(RenderedFrameContext)`, `Invoke(command, reply)`); the DV comparison capture is one (`DvCompareCaptureSink.h` in input.signal; the renderer builds the list with `DvCompareCapture::RendererCaptures()` and registers nothing after construction).
3. **Frame rendered:** `RecordPresented(PresentedFacts)`, `RecordDejudderHold(HoldFacts, lastHoldCode)`, `RecordCadenceRepeat`, `RecordRendererFrame` (the report's frame rows), `HoldLog` ("De-judder: hold" lines, one per reason, at most every 5 s, flushed at teardown), `LogEntranceCounters` ("Entrance cost" / "Entrance stages" lines: first run, every 2400 legacy frames of time, every 120 input-side runs; called by the S1 entrance segment once feat/m2-entrance and WP-17 are merged, not by the host).
4. **State report:** `WriteRendererHistory` (the renderer's rings into the report sink; no exception crosses the plugin boundary); GUI: `ReportRun` (above).

Every row, line and sidecar fragment is byte-identical to the host code it replaced (`DiagnosticsAdapterTests`). Stays in the host for now: the `Render-ahead:` line (`LogRenderAhead`, ~30 render-ahead state fields; it moves with the render-ahead state in WP-7) and the one-line `RecordDecision` calls. Stays Bill's: the rendered-output capture itself, the subtitle capture (`VP_SUBTITLE_CAPTURE_PATH`), the analysis/local-boundary/crop/black-level traces (`VP_*_TRACE*`, `VP_LOCAL_BOUNDARY_TRACE`, ...).

### Fallbacks engaged (live elsewhere: `src/VP-V/core/FallbackStatus.h`)

Header-only, pure. `FallbackStatus::Registry` (thread-safe: `Engage`, `Release`, `Engaged`, `Entries`, `Heading`, `Clear`); `Format` gives "Fallbacks engaged: a (why); b (why)" or "Fallbacks engaged: none"; `Ids` parses a heading; `Notices::Take` returns the ids not yet announced this session. The renderer publishes `Heading()` through `IVideoRenderer::GetFallbacksStatus`. The GUI polls it with the crash-state lines (`UpdateFallbacksStatus`), shows it in the Ctrl+I overlay (StatsOverlayWindow.cpp), re-publishes the Config status, adds it to the report's live settings, and shows one transient notice per new id (or a status line "Fallback engaged: <id>" when the overlay is switched off).

Ids engaged in code today (LibplaceboVideoRenderer.cpp): `capture GPU unpack`, `native RGB GPU unpack`, `render-ahead`, `delay budget`, `look-ahead`, `entrance once`, `pipeline order`, `pipeline settings`, `re-base precision`, `calibration LUT`, and six `Dolby Vision ...` entries. The DirectShow renderer publishes none.

`SlotTiming.h` (core, header-only) is the per-slot GPU-time accumulator and 3090 projection; it is used only by PipelineSkeletonTests so far.

### Config keys actually read

- `[shortcuts] state_report`: default Ctrl+Alt+D; empty = no shortcut; an invalid chord binds none and reports an error.
- `[logging] auto_report`: Boolean, default false.
- `[logging] report_thumbnail`: Boolean, default false. The only way a report holds pixels.
- `[shortcuts] raw_capture`: default Ctrl+Alt+W (ShortcutDefaults.h); the raw capture toggle (needs `VP_RAW_CAPTURE_PATH`).
- Environment only (no config key, never set in an ordinary launch): `VP_RAW_CAPTURE_*` (raw capture), `VP_TEST_SCREENSHOT_DIR`, `VP_TEST_SHADER_DIR`, `VP_TEST_SHADER_CACHE_DIR`, `VP_TEST_ADAPTER` (tests).
- An invalid Boolean keeps the default and is reported by `StateReportSettings::Read`.
- The crash state has no keys; its folder follows the log file path.

### User-visible fallbacks / notices

This slot has no fallback of its own. It owns the heading format and the once-per-session notices. Report outcomes are toasts ("saving...", "saved <file>", "failed (<error>)", "already saving"). A kept crash state is logged at the next start ("Crash state: the previous run did not exit cleanly; kept ...").

### Dependencies to lift it

- `CrashState`: Win32 only (Psapi `K32GetProcessMemoryInfo`, dbghelp loaded at run time) and the C++ standard library; `pch.h`.
- `StateReport`: `StoreZipWriter.h`, `DiagnosticHistory.h` (one constant), bcrypt, dxgi, windowscodecs (WIC), registry and display-config APIs.
- `StateReportSettings`: `ConfigFile.h`, `ShortcutChord.h`, `windows.h`.
- `FallbackStatus.h`: C++ standard library only.
- `DiagnosticHistory.h`, `RendererDiagnosticCodes.h`, `StoreZipWriter.h`: header-only, `windows.h` (QPC) and the C++ standard library.
- `RawCaptureRecorder`: `VideoFrame.h`, `VideoState.h`, `DebugLog.h`, `EOTF.h`, `ColorSpace.h`, `VideoFrameEncoding.h`, ATL `CStringA`; `RawCaptureCorpus.h`: `VideoState.h`, `dv_loopback/DvCaptureContainer.h`.
- `DiagnosticsAdapter.h`: the rings, `DebugLog.h`, `GeneralRate.h`, `dxgi.h` (`CreateDXGIFactory1` only when `TestAdapterFromEnvironment` is used). `DiagnosticsReportRun.h` adds `StateReport.h`, `CrashState.h`, `IRenderer.h`, `ConfigFile.h`, `vprenderer/OptionalRendererLayout.h`, ATL strings.
- The .cpp files (CrashState, StateReport, RawCaptureRecorder) are compiled into VideoProcessor-Lib; the headers are used by the Lib, the GUI, the renderer DLL and the tests.

### Test it standalone

Classes in VideoProcessor-Test.dll: `DiagnosticsAdapterTests` (this folder's tests/: byte identity of every adapter row, line and sidecar fragment against copies of the old host code; report-run gates), `DiagnosticHistoryTests`, `DvRawCorpusTests` (raw capture record/replay), `CrashStateTests`, `StateReportTests`, `StateReportSettingsTests`, `StoreZipWriterTests`, `FallbacksEngagedTests` (R6BaselineTests.cpp), `BugHunt3ShutdownTests` (`CrashStateFilesArePerInstance`, `SessionEndRestoresTheLightTopologyAndCrashState`), `ShortcutDefaultsTests` (the `state_report` default). None needs a GPU; they write only to `%TEMP%`. Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~DiagnosticsAdapterTests|FullyQualifiedName~DiagnosticHistoryTests|FullyQualifiedName~DvRawCorpusTests|FullyQualifiedName~CrashStateTests|FullyQualifiedName~StateReport|FullyQualifiedName~StoreZipWriterTests|FullyQualifiedName~FallbacksEngagedTests" /Logger:"trx;LogFileName=diagnostics.trx"`

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

7. **Diagnostic history rows** (render thread, per frame): one presented row per frame plus drop/hold rows, ~20-40 ns each (measured by `DiagnosticHistoryTests`). The WP-17 adapter only moved them; same work.
8. **Raw capture / test switches:** nothing unless `VP_RAW_CAPTURE_PATH` / a `VP_TEST_*` variable is set (one environment read where the host read it before).

Cost when off: the crash state has no switch, so item 1 always runs. The report costs nothing until triggered; `auto_report` off and `report_thumbnail` off cost nothing.

## Settings

- Existing keys: `[shortcuts] state_report`, `[shortcuts] raw_capture`, `[logging] auto_report`, `[logging] report_thumbnail` (slots.json `settings_keys`, manifest `config_keys`: corrected in WP-17; they were empty)
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
- `DiagnosticsAdapterTests`, `DiagnosticHistoryTests`, `DvRawCorpusTests`, `FallbacksEngagedTests` (FallbackStatus), `CrashStateTests`, `StateReportTests`, `StateReportSettingsTests`, `StoreZipWriterTests`

Contract tests:
- every slot's fallback entry reaches the overlay, Config status and Ctrl+Alt+D (planned; no test yet)

Chain tests:
- (none yet)

## Known issues / open items

- audit all log-only fallbacks (item 45); still log-only today include the re-base failure pause, the AUTO Limited -> composed swapchain switch and the NLS hook-failure safe fit (see those slots' READMEs)
- the registry is `FallbackStatus::Registry` in src/VP-V/core/FallbackStatus.h (from fix/r6-baseline); reuse it, do not duplicate
- WP-17 host lines are a patch (`docs/design/wp17-callsites.patch`) for the integrator; until it is applied the host still has its own copies of the adapter's code
- retire `ToggleDvCompareCapture` for WP-11's module command (`InvokeModuleCommand("vp", "dv_compare_capture")` answered by `RenderedFrameCaptures::Invoke`)
- `Render-ahead:` line (`LogRenderAhead`) still composed in the host: moves with the render-ahead state (WP-7)
- `RawCaptureRecorder.h/.cpp` and `RawCaptureCorpus.h` carry a "Copyright Bill Slack / GPL-3.0-only" header although they were written for VP (048b8477) and are not in Bill's beta: owner to decide the header (left unchanged by the move)
- GPU / on-screen checks not run by WP-17 (list for the big test): Ctrl+Alt+D bundle snapshot with a running renderer, report thumbnail, rendered-output capture sidecar on DV content, `VP_TEST_ADAPTER` on a two-GPU machine

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

Forwarding headers (WP-17; removed in M2 step 0 with their includers): `src/VideoProcessor-Lib/DiagnosticHistory.h` (included by `IRenderer.h` and `VideoProcessorDlg.cpp`, Bill's files), `src/VideoProcessor-Lib/vprenderer/RendererDiagnosticCodes.h` (included by `LibplaceboVideoRenderer.cpp`), `src/VideoProcessor-Lib/RawCaptureRecorder.h` (included by `VideoProcessorDlg.cpp`; on active branches also by `DvCompareCapture.h` and WP-11's `VpStatusAdapter.h`) and `src/VideoProcessor-Lib/RawCaptureCorpus.h` (so Bill's test files `LibplaceboLutParserTests.cpp` / `LibplaceboPluginProxyTests.cpp` keep their includes untouched). The `StoreZipWriter.h` forwarder was deleted in step 0 commit A (no Bill file includes it).

- `src/VP-V/modules/control/diagnostics/StateReport.cpp`
- `src/VP-V/modules/control/diagnostics/CrashState.cpp`
- `src/VP-V/modules/control/diagnostics/CrashState.h`
- `src/VP-V/modules/control/diagnostics/StateReport.h`
- `src/VP-V/modules/control/diagnostics/StateReportSettings.h`
- `src/VP-V/modules/control/diagnostics/DiagnosticHistory.h`
- `src/VP-V/modules/control/diagnostics/RendererDiagnosticCodes.h`
- `src/VP-V/modules/control/diagnostics/StoreZipWriter.h`
- `src/VP-V/modules/control/diagnostics/RawCaptureRecorder.cpp`
- `src/VP-V/modules/control/diagnostics/RawCaptureRecorder.h`
- `src/VP-V/modules/control/diagnostics/RawCaptureCorpus.h`
- `src/VP-V/modules/control/diagnostics/DiagnosticsAdapter.h`
- `src/VP-V/modules/control/diagnostics/DiagnosticsReportRun.h`
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

- Keys: `state_report`, `raw_capture`, `auto_report`, `report_thumbnail`.
- Tiers: n/a.
- Fallback entry ("Fallbacks engaged"): none (control slot).

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): FallbackRegistry unit tests.
- Contract (formats, geometry, fallback entry): every slot's fallback entry reaches the overlay, Config status and Ctrl+Alt+D.
- Chain (total delay at the 4/3/2-frame budgets): n/a.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
