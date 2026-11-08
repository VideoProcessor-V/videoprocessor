<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# M2 P2-0 inventory: the 22 IVideoRenderer virtuals and their callers

Base: 40356bed (P5a-rest audited tip on integ 852424a7). Bill base a591ad3b. Design: doc 61 section 1.

## The 22 methods (Bill's IRenderer.h at a591ad3b has none of them; `diff -w` shows only additions)

Dejudder/LedDrive/SourceResolution/Fallbacks status (4), Suspend/Resume/StopLedDrive (3), GetContentIntentStatus,
GetIntentStatus, SetAmbientLux (3), WriteDiagnosticHistory (1), the 4 timed-file-frame methods, Get/SetCalibration
(2), SetActivePictureLookaheadAutomatic, SetQueueDelayBudget, ToggleDvCompareCapture (3), GetModuleStatus,
InvokeModuleCommand (2) = 22. The 20 of doc 61 F1 plus the two module entries.

## Callers (non-test)

| Method group | Caller | Thread |
|---|---|---|
| GetModuleStatus | `VP-V/gui/VpStatusAdapter.h` Poll (the dialog's stats timer, crash state, Ctrl+Alt+D) | UI |
| InvokeModuleCommand | none outside tests | |
| WriteDiagnosticHistory | `DiagnosticsReportRun.h` NewInputs | UI (key press) |
| Suspend/ResumeLedDrive | `VideoProcessorDlg.cpp` StartProjectorTest / the test's end | UI |
| StopLedDriveForShutdown | `VideoProcessorDlg.cpp` shutdown restore, inside `LedDrive::RunBoundedRestore`'s own thread | bounded worker |
| GetContentIntentStatus | `VideoProcessorDlg.cpp` UpdateContentIntent | UI |
| GetIntentStatus, SetAmbientLux | `VideoProcessorDlg.cpp` UpdateIntentInterpreter (1 Hz) | UI |
| SetQueueDelayBudget, SetActivePictureLookaheadAutomatic | `VideoProcessorDlg.cpp` (profile apply, ApplyStartupActivePictureLookahead) | UI |
| ToggleDvCompareCapture | `VideoProcessorDlg.cpp` OnCommandDvCompareCapture | UI |
| Get/SetCalibration | `CalibrationController.cpp` RendererPatternSink | calibration worker |
| Supports/OnTimed/WaitSlot/Cancel | `VPRendererFilter.cpp` (create, Receive, flush, stop) | filter streaming and control threads |
| GetSourceResolutionStatus | `tools/vp-scale-render/VpScaleRenderCore.h` (tool, not in the solution) | tool |

`FileHost` (applies settings through Bill's `ApplyApplicationState`) and `FileSideData` (a comment) call none of them.
Every other implementer of `IVideoRenderer` (the DirectShow renderers, test fakes) overrides none of the 22.

## Exports, Bill vs VP

Bill (a591ad3b): VersionExport, Create/Destroy, IsBeta, HDR status, startup readiness, diagnostic sink.
VP-V adds: GetBuildId (hunt 2), EndHeldLedDrive (filter), settings fingerprint (tests), and now
`VideoProcessorLibplaceboVpvAbi` (P2).

## Bill's plugin API number

`VP_LIBPLACEBO_PLUGIN_API_VERSION`: 34 at a591ad3b (this branch's base). Bill's tips after it:
origin/codex/source-ticket-20261104 and origin/codex/ar-temporal-preview-20261104 are at 35
(d985c21fe, 2026-11-01, "Retain detected aspect", an AspectRatioOsd snapshot layout change that is not in this
base). This branch carries 34, the value that matches the vtable and snapshot layouts it actually has; the merge
that brings d985c21fe brings 35 with it.

## SetActivePictureLookaheadAutomatic provenance

VP-V's own: commit ca651d582 (bug hunt 3 timing fixes, hunter A, F2), merge 0dc83f0ee. `LockedFilmQueueTarget` /
`LockedFilmLookaheadFloor` do not exist at a591ad3b or at Bill's later tips; the method is not a row of
`docs/design/upstream-divergence-register.md`. Result: it stays a method (of `vpv.presentation_ctl.v1`), not an
upstream pull-request candidate unless Bill adopts the behaviour.
