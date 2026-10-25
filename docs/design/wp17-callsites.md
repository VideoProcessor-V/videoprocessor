# WP-17: VP diagnostics and captures behind one adapter (call sites)

Work package WP-17 of `docs/design/integration-batch-plan.md` (branch `docs/integration-batch-plan`).
Branch `feat/wp17-diag`, started from `chore/early-migrations` ac49a1de, **rebased onto `integ-group-c`
d73b2ac0** (integ/batch1 43ebff50 with feat/dv-l8, plus WP-15, WP-12 and WP-11 applied; plugin API 44). **VP-ONLY** (nothing here is offered
upstream).

The rule is "host = call sites only": the adapter, the moved files and the tests are committed on
this branch; the changes to Bill's files are this patch, `wp17-callsites.patch`, for the integrator.

## Apply

```
git apply --check --cached docs/design/wp17-callsites.patch   # passes at the commit that adds it
git apply docs/design/wp17-callsites.patch
```

Generated against this branch on `integ-group-c` d73b2ac0 (no file the patch touches differs between
d73b2ac0 and this branch, so `git apply --check` passes on d73b2ac0 as well). On a later base use `git apply -3`. Files: `LibplaceboVideoRenderer.cpp`,
`VideoProcessorDlg.cpp`, `VideoProcessorDlg.h`. No plugin API change, no new export, no new setting.

## Commits on the branch

| Commit | What |
|---|---|
| c6ed2cab | M2 move: `DiagnosticHistory.h`, `RendererDiagnosticCodes.h`, `StoreZipWriter.h` -> `src/VP-V/modules/control/diagnostics/` |
| 5ee1599b | M2 move: `RawCaptureRecorder.h/.cpp`, `RawCaptureCorpus.h` -> same folder |
| 2ef4d3b9 | `DiagnosticsAdapter.h` (renderer side), `DiagnosticsReportRun.h` (GUI side), `tests/DiagnosticsAdapterTests.cpp`, README pass, slots/manifest keys |
| 81500a5b, a8c66d3d | first patch + this file; `docs/design/.gitattributes` keeps `*.patch` LF on checkout |
| 19e7d414, 7d9ba653 | audit fixes (first auditor and second reader) incl. forwarders for all five moved headers |
| (prev.) | patch on 43ebff50 without the entrance hunk; DV comparison capture through the adapter (`DvCompareCaptureSink.h`) |
| this one | rebased onto d73b2ac0, patch regenerated on top of WP-11/12/15's applied call sites; WP-11's `VpStatusAdapter.h` includes the raw capture recorder by its new path |

All moved files are VP's: none exists in `origin/v1.4.02-beta` (`git cat-file -e`), the drift
guard passes. `RawCaptureRecorder.*` / `RawCaptureCorpus.h` carry a copied "Bill Slack / GPL-3.0-only"
header although they were written for VP (048b8477); the move leaves it unchanged (owner decision).

## What the patch changes (hook points)

### LibplaceboVideoRenderer.cpp

| Hook point | Before | After |
|---|---|---|
| include | the codes header through its forwarder | `VP-V/modules/control/diagnostics/DiagnosticsAdapter.h` (brings the codes) |
| env-gated test switch | `ScreenshotDirectory`, `ReadUserShader`, `ShaderCacheTestFolder`: three inline `GetEnvironmentVariable` blocks | `VpDiagnostics::TestFolderOverride(...)` each |
| env-gated test switch | `VP_TEST_ADAPTER`: 24-line DXGI enumeration in device creation | `VpDiagnostics::TestAdapterFromEnvironment(&testAdapter)` |
| capture requested | rendered-output capture sidecar: VP's `scene_min_present` / `scene_min_nits` expression inside Bill's JSON chain | `<< VpDiagnostics::DvSceneMinFields{...}` in the same chain (same stream, same locale/precision) |
| frame rendered | `DejudderHold`: row + decision block, per-reason pending list, 5 s tick, `FlushDejudderHolds` loop; three members | `RecordDejudderHold({...}, diagLastHoldCode)` + `dejudderHoldLog.OnHold(...)`; one member `VpDiagnostics::HoldLog`; `FlushDejudderHolds()` is one line |
| frame rendered | presented row: 40-line `FrameRecord` build | `PresentedFacts` fields from the values at hand + `RecordPresented` (flag/latency logic in the adapter) |
| frame rendered | `RecordRendererFrame` (anonymous namespace) | `using VpDiagnostics::RecordRendererFrame;` (the 12 call sites unchanged) |
| frame rendered | drift-correction repeat row (11 lines) | `RecordCadenceRepeat(...)` |
| state report | `WriteDiagnosticHistory` body | `VpDiagnostics::WriteRendererHistory(...)` |
| capture requested / frame rendered | feat/dv-l8: `DvCompareCapture::Recorder dvCompare` member, `Configure` + `Enabled` + `OnRender` in the render path, `Toggle` in `ToggleDvCompareCapture` | `VpDiagnostics::RenderedFrameCaptures captures = DvCompareCapture::RendererCaptures()`; `captures.Configure(...)`, `if (captures.AnyEnabled()) captures.OnFrameRendered({...})` (the context, which consumes the trim selection, is built only while a capture is on, as before); `captures.Invoke(kCommand, reply)` |

### VideoProcessorDlg.cpp / .h

| Hook point | Before | After |
|---|---|---|
| state report requested | `MaybeAutoReport`: busy/limiter checks + logs | `m_reportRun.AllowAutomatic(...)` |
| state report requested | `SaveStateReport`: busy flag with 2-min expiry, rings, crash state, binaries, config files, recent log, thumbnail request, `VP_TEST_SCREENSHOT_DIR`, the 70-line worker thread | `TryBegin`, `NewInputs`, the dialog's own facts (build identity, live settings: unchanged lines), `RequestThumbnail`, `Start` |
| state report done | `OnStateReportDone` body | `m_reportRun.Finish(lParam)` -> toast |
| exit | worker cancel + bounded wait | `m_reportRun.WaitForExit(deadline, timeoutMs)` |
| members (.h) | limiter, busy flag, busy-since, `StateReportWorkerState`, worker pointer | `VpDiagnostics::ReportRun m_reportRun` |
| helpers | `ExecutableFolder`, `WidenAnsi`, `NewestScreenshotSince` (anonymous namespace) | in the adapter |

Unchanged in the dialog (already one line each): the raw capture feed (`OnCardState`,
`OnVideoState`, `OnFrame`, `SetProcessingNote`), the `DiagnosticHistory::Global().Record...` capture
events and the 1 Hz stats row, the crash-state `SetValue` calls (WP-11 moves those keys).

### Effect on the patch set against Bill (end-state rule: smaller or no larger)

Host lines: 80 added, 419 removed (net -339) in the three files. Our diff against Bill's beta in these
files shrinks by the same amount; what remains per hook point is one call.

## Byte identity

`src/VP-V/modules/control/diagnostics/tests/DiagnosticsAdapterTests.cpp` holds verbatim copies of
the replaced host code and compares:

- the sidecar `dolby_vision` tail for DV on/off x scene-minimum present/absent x 8 values, in a
  stream with the capture's state (classic locale, precision 10, after a hex/fill section): equal
  strings. The rest of the sidecar is Bill's code, untouched, so the whole sidecar keeps its
  content and order;
- presented, renderer (ingest/drop) and cadence-repeat rows: `memcmp` of the ring records;
- de-judder hold rows and decisions over reason changes and the 5 s interval (stamped `qpc`
  excluded), and the "De-judder: hold" lines including the teardown flush (DebugLog sink);
- the "Entrance cost" / "Entrance stages" lines over first-run, batch and interval triggers (the
  adapter function S1 will call; the host hunk is not in this patch);
- the DV comparison capture through the adapter: registered under `dv_compare_capture`, off
  without a folder, toggle reply identical to the recorder's own;
- the test-switch reads (unset, set, 1024+ characters) for all three folder variables;
- report run: busy expiry at exactly 120000 ms vs 120001 ms, automatic-report skips (busy: no slot
  spent; rate limit) with their exact log lines, `Finish` texts and log line, thumbnail file search.

The raw capture's `capture.txt` / `frames.csv` and the report bundle are produced by moved files
whose source is byte-identical (pure `git mv`); `DvRawCorpusTests` and `StateReportTests` cover them.

Behaviour notes (deliberate, not visible in any output):

- `dejudderHoldsNotLogged` was counted and reset but never read: dropped.
- The report's config-file list and recent-log text are now taken in `NewInputs`, i.e. before the
  dialog collects its live settings instead of after (same order between themselves). The live
  settings collection calls status getters that do not log, so the recent-log text is the same.
- `VP_TEST_SCREENSHOT_DIR` is read through the same adapter call in the renderer and the GUI.

## Conflicts to expect at integration

- **WP-11 / WP-12 / WP-15**: already applied on d73b2ac0; this patch is generated on top of them
  (WP-11's `VpvGui::AppendBundleSettings` line in `SaveStateReport` stays; WP-11 routes the
  `state_report` command to `SaveStateReport`, which calls the adapter).
- **WP-7 / motion-timing**: no overlapping hunk (checked against `motion-timing-callsites.patch`).
  `LogRenderAhead` (the `Render-ahead:` line) stays in the host for WP-7: its ~30 inputs are the
  render-ahead state that WP-7 extracts; the line moves with it.
- **WP-8 / feat/m2-entrance (S1 entrance)**: S1 moves `LogEntranceCounters`, its counters and its
  interval into `src/VP-V/segments/EntranceSegment.cpp` (`Segment::LogCounters`). This patch
  therefore leaves the host's `LogEntranceCounters` alone; once both are merged, `Segment::LogCounters`
  calls `VpDiagnostics::LogEntranceCounters(...)` (one VP-side line, done by the m2-entrance agent;
  `EntranceCounterLinesAreByteIdentical` pins the lines).

## Plan goal items not in this patch

- Render-ahead logs: `LogRenderAhead` stays for WP-7 (above).
- Crash-state contributions of VP modules: the crash-state keys move with WP-11's GUI status
  adapter (`RecordCrashState`); the crash state itself is this module (migrated earlier).
- B3 history-break counter readout: no such counter exists at ac49a1de (B3's downgrades are
  `Fallbacks engaged` entries and the entrance counters above). Nothing to move; a future counter
  should be a `VpDiagnostics` row or decision, not host code.
- The dialog header now includes `DiagnosticsReportRun.h` instead of `StateReport.h` (audit LOW-6:
  a forward-declared `unique_ptr` member would need constructor/destructor edits in Bill's files for
  a small compile-time gain; not done).
- Audit: `D:\codex\videoprocessor\skeleton-notes\audit-feat-wp17-diag.md` (0 HIGH, 3 MED, 8 LOW);
  fixes in the commit after this file's first version.

## DV comparison capture (feat/dv-l8, in integ/batch1)

`src/VP-V/modules/input/signal/DvCompareCaptureSink.h` wraps `DvCompareCapture::Recorder` as a
`VpDiagnostics::RenderedFrameCapture` (same calls, same order, same files) and `RendererCaptures()`
builds the renderer's capture list at construction (the list is read-only afterwards: no lock; each
capture locks its own state). Cost while off: one `Configure` (unchanged folder: a compare) and one
`Enabled()` per rendered frame, as before. With WP-11's generic entry, `ToggleDvCompareCapture` can
become `InvokeModuleCommand("vp", "dv_compare_capture", reply)` answered by `captures.Invoke` and
leave the vtable (WP-11 step 3).

## Modularity: diagnostics off = picture and slots unchanged

Nothing in the adapter touches a texture, a hook, a render parameter or a slot decision: it records
ring rows and decisions from values the host already computed, writes log lines, reads test-only
environment variables where the host read them before, and (only when a capture is switched on)
copies frames to disk. With the captures off and no `VP_TEST_*` / `VP_RAW_CAPTURE_PATH` /
`VP_DV_COMPARE_PATH` set, the render path does exactly what it did before. Byte identity of the
picture is not affected by any hunk; the frozen-reference GPU check is still listed for WP-20.

## Forwarding headers (removed in M2 step 0 with their includers)

Five, listed in the module's MANIFEST.json `forwarding_headers`: the `DiagnosticHistory.h` forwarder
(included by `IRenderer.h`, `VideoProcessorDlg.cpp`), the `RendererDiagnosticCodes.h` forwarder
(`LibplaceboVideoRenderer.cpp`; this patch removes that includer) and the `RawCaptureRecorder.h`
forwarder (`VideoProcessorDlg.cpp`; `DvCompareCapture.h` and `VpStatusAdapter.h` now use the new
path), and the `StoreZipWriter.h` / `RawCaptureCorpus.h` forwarders (so Bill's test files keep
their includes).
Old-to-new paths: `docs/design/migration-path-map.md`.

## Verify

- Build (Release x64, slot runner): before the rebase, moves alone: Lib, Test, GUI, VPRenderer
  clean (`D:\codex\videoprocessor\wp17-build1.log`); adapter + patch applied: clean
  (`wp17-build2.log`, `wp17-build3.log`). On 43ebff50 with the patch applied: clean (`wp17-build5/6.log`). On d73b2ac0 with this patch
  applied: `wp17-build7.log`.
- CPU tests (targeted, per the 2026-10-25 policy; the full filtered suite runs per integration
  group) on d73b2ac0 + patch: area classes, PipelineSkeletonTests, DvSignalL8HoldTests, WP-11's
  VpStatusAdapterTests/StatusTextTests: 163/163 (TRX `D:\codex\videoprocessor\wp17-trx\targeted2` with
  LibplaceboPluginProxyTests re-run in `targeted3` after the renderer relink: 16/16).
- Not run here (GPU / on-screen, list for WP-20): Ctrl+Alt+D bundle with a running renderer (the
  bundle's VP sections: state-history rows from both modules), report thumbnail, a rendered-output
  capture on DV content (sidecar fields), `VP_TEST_ADAPTER` on the two-GPU laptop.
