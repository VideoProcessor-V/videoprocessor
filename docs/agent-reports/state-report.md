# Report-a-problem hotkey (state report)

Date: 2026-10-17. Branch `feat/state-report` (local only), from local `integ/2026-10-17` at `0642f68c`.
Worktree `D:\codex\videoprocessor\state-report`. Design: memory `vp-state-dump-and-handovers`.

## What it does

Ctrl+Alt+D (`[shortcuts] state_report`), or "Save problem report" in VP's window system menu, writes
`logs\reports\vp-report-<yyyymmdd-hhmmss-ms>.zip` (store-only zip, newest 20 kept) with:

| Entry | Contents |
|---|---|
| README.md | field guide for Claude: every file, every JSONL field, stages, flags, stats names, hold codes, privacy |
| manifest.json | format 1, local time, trigger, build ID, commit, version, plugin API, renderer, EXE/DLL SHA-256, contents |
| state-history.jsonl | EXE rings (`src: exe`) + renderer DLL rings (`src: renderer`), `t_ms` relative to the key press (shared QPC clock) |
| log-excerpt.txt | last 512 KB of vp.log (in-memory recent lines when file logging is off) |
| crash-state.txt / crash-state-last.txt | live crash-state values; newest kept "crash state (N).txt" if any (no minidump) |
| live-settings.json | renderer, window, overlay, profile selections, presentation/de-judder/stall/output status, report switches |
| config/* | VideoProcessor.cfg (and VideoProcessorRenderer.cfg if used), redacted, comments kept |
| system.json | OS, CPU, memory, power, GPUs (PCI ID, driver), outputs (mode, bits, HDR colour space, luminance), display paths (monitor, connection, exact refresh) |
| thumbnail.png | only with `report_thumbnail` on: next rendered frame via the screenshot path, scaled to 480 px |

A toast ("Problem report: saving... / saved <file>") goes through `ShowProfileOverlayItems`; when the profile
overlay is off it goes to the status line. The UI thread snapshots the rings and settings at the key press;
a worker hashes the binaries, reads the log/config, writes atomically (`.partial` then rename) and applies retention.

Redaction: VP folder -> `<vp>`, Windows folder -> `<windows>`, any other absolute path -> `<path>`, user and
computer names -> `<user>`/`<host>`, e-mail -> `<email>`, URLs -> `<url>`, values of secret-like keys
(password, token, api_key, serial, host, address, url, user, ...; short words only as whole tokens) -> `<redacted>`.

## Recording (DiagnosticHistory.h)

Lock-free seqlock rings, 128-byte slots (8-byte sequence + 120-byte record): frames 4096, decisions 1024,
LED 256, capture 256, stats 256 = 736 KB per module. Writers: one `fetch_add`, odd sequence, 15 relaxed word
stores, even sequence (release). Readers accept a slot only if the sequence is the expected even value before
and after the copy (fences), so torn records are never returned.

- EXE (VideoProcessorDlg.cpp): `capture_ingress` / `capture_rejected` frame rows in `OnCaptureDeviceVideoFrame`;
  capture events (video-state, device-state, device-lost, device-error); decisions (reset completed/failed/cancelled
  with reason, profile overlay items, state-report trigger/saved); 1 Hz stats row from the crash-state block.
- Renderer DLL (sub-agent, opus): `renderer_ingest`, `presented`, `dropped`, `repeated` frame rows with timings,
  flags, hold codes (`vprenderer/RendererDiagnosticCodes.h`); decisions (render-ahead gate/loop/re-anchor/flush,
  hold reason changes, de-judder mode, motion engine, source resolution, content intent, display rule, profiles,
  output mode, renderer state); LED rows in `led/BarcoAsciiDriver.cpp::Transact` (every command, failures, port events).
  Display-rule and profile rows carry the owner's own rule names (no paths).

## Switches and Config wording

| Key | Default | Config control |
|---|---|---|
| `[shortcuts] state_report` | Ctrl+Alt+D (empty = none) | Shortcuts page: "Save problem report" |
| `[logging] auto_report` | false | Logs page: "Save a problem report automatically" (liveness-recovery / source-gap-recovery resets and failed resets; at most one per 10 min, five per run) |
| `[logging] report_thumbnail` | false | Logs page: "Include a small picture in problem reports" |

Logs page also has a "Problem reports" heading, a help line and an "Open reports folder" button.
Both [logging] keys are validated Booleans (MainConfigSchema.h) and apply live. Documented in CONFIGURATION.html
and docs/configuration-public-fields.tsv.

## Plugin API

`VP_LIBPLACEBO_PLUGIN_API_VERSION` 35 -> **36** for `IVideoRenderer::WriteDiagnosticHistory(VpDiagnosticHistorySink,
void*, int64_t referenceQpc)` (last vtable slot). **The MPC renderer branch also claimed 35 and must be renumbered
(37 or later) when it merges.** The build-ID pair check is unchanged and still enforced
(`LibplaceboPluginProxyTests::RendererDllExportsTheHostBuildId` passes once EXE, test DLL and renderer DLLs are
built from the same sources). Stale, pre-existing: `tools/test_libplacebo_variants.py` asserts API 26.

## Files

- New: `src/VideoProcessor-Lib/DiagnosticHistory.h` (602 lines), `StoreZipWriter.h` (263), `StateReport.h` (192),
  `StateReport.cpp` (~1230), `StateReportSettings.h` (154; shortcut chord grammar moved here from the GUI, unchanged),
  `vprenderer/RendererDiagnosticCodes.h` (147), tests `DiagnosticHistoryTests.cpp` (411), `StateReportTests.cpp` (646),
  `tools/test_state_report_onscreen.ps1` (248).
- Changed: `VideoProcessorDlg.cpp` (shortcut table ~634; report functions ~8585-8900; frame records ~9280-9300;
  system menu ~15516), `VideoProcessorDlg.h`, `Resource.h` (ID_COMMAND_STATE_REPORT 33207), `IRenderer.h` (~639),
  `LibplaceboPluginVideoRenderer.*`, `LibplaceboVideoRenderer.*`, `LibplaceboRendererPluginApi.h`, `led/BarcoAsciiDriver.cpp`,
  `MainConfigSchema.h`, `ConfigEditorWindow.cpp` (~1089, Logs page ~14270-14320), `ConfigEditorWindowTests.cpp`,
  `ConfigFileTests.cpp`, `LibplaceboPluginProxyTests.cpp`, CONFIGURATION.html, docs/configuration-public-fields.tsv.

## Tests (x64 Release, single-threaded vstest, TRX)

- `D:\codex\videoprocessor\state-report-lead-trx\final.trx`: DiagnosticHistoryTests, StoreZipWriterTests,
  StateReportTests, StateReportSettingsTests, LibplaceboPluginProxyTests, ConfigFileTests (incl. the API-version pin
  and the CONFIGURATION.html inventory), ConfigEditorCoreTests, CrashStateTests: **257 / 257 passed**.
- Config app tests (off screen, `run_config_tests.ps1 -Shards 1`, including the new state_report / auto_report /
  report_thumbnail control checks): **143 passed, 0 failed, 1 skipped** (an on-screen-only update test), GATE PASSED.
- Measured cost (`DiagnosticHistoryTests::RecordCostIsBounded`, best of 3 x 1M `RecordFrame` incl. the QPC stamp,
  laptop): **31.6 - 47.6 ns per frame record** over five runs (limit 100 ns).
- Fixed during the run: a test secret ("abc123") occurred by chance inside a hex SHA-256 in the manifest; the test
  now uses a non-hex secret.

## Not run here (Claude runs GPU / on-screen)

Gated test **`tools\test_state_report_onscreen.ps1`** (`-Run` or `VP_TEST_STATE_REPORT_ONSCREEN=1`): starts VP from
`-VpRoot` (working directory = VP folder) unless running, waits for playback (crash-state shows a source mode and the
VP Renderer), presses Ctrl+Alt+D with SendInput (`-UseCommand` posts WM_COMMAND 33207), then validates the bundle:
entries, manifest (format 1, 32-hex build ID, plugin API 36, EXE/DLL SHA-256 equal to the files), exe capture rows,
stats rows, renderer `presented` rows, no user-profile path / user / computer name / raw VP path, thumbnail only when
enabled, retention <= 20, and the "Problem report saved" log line. Closes VP only if it started it.
`-ValidateOnly <zip>` validates an existing bundle without VP.

## Notes

- Sub-agents: opus (renderer/ABI), sonnet (tests, Config test, docs). A shared scratch `build.sh` was overwritten
  by one agent while the lead's build started; that run aborted harmlessly. Agents now use their own scratch folders.
- The base branch showed no build or test breaks.
