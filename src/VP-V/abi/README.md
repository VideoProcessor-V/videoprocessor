<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# VP-V extension ABI (M2 P2)

Everything VP-V adds across the renderer plugin boundary goes through ONE trailing virtual of Bill's
`IVideoRenderer`, `QueryExtension(iid)`, and the pure-abstract interfaces in `VpvInterfaces.h`. Bill's vtable
is Bill's again (plus that one VPV-HOOK virtual), and `VP_LIBPLACEBO_PLUGIN_API_VERSION` is Bill's number
(34 at the a591ad3b base; it follows Bill's tip when his changes merge).

Design: `D:\codex\vp-handover\61-design-p2-sourceid-camera-k3-k10.md` section 1 (decisions D-P2a..c).

## The seven interfaces (iid -> methods; the 22 former virtuals)

| iid | Interface | Methods (was) | Host callers |
|---|---|---|---|
| `vpv.modules.v1` | `IVpvModules` | `Status` (GetModuleStatus), `Command` (InvokeModuleCommand) | `VP-V/gui/VpStatusAdapter.h` (Poll) |
| `vpv.status.v1` | `IVpvStatus` | `GetDejudderStatus`, `GetSourceResolutionStatus`, `GetFallbacksStatus`, `WriteDiagnosticHistory` | dialog stats and Ctrl+Alt+D (through the module entry), crash state, `DiagnosticsReportRun.h` |
| `vpv.led.v1` | `IVpvLed` | `Status` (GetLedDriveStatus), `SuspendForTest`, `ResumeAfterTest`, `StopForShutdown` | `VideoProcessorDlg.cpp` (projector test, forced shutdown) |
| `vpv.intent.v1` | `IVpvIntent` | `GetIntentStatus`, `GetContentIntentStatus`, `SetAmbientLux` | `VideoProcessorDlg.cpp` |
| `vpv.calibration.v1` | `IVpvCalibration` | `GetOutput` (GetCalibrationOutput), `SetPattern` (SetCalibrationPattern) | `CalibrationController.cpp` |
| `vpv.timed_file_frames.v1` | `IVpvTimedFileFrames` | `Supports`, `OnTimedVideoFrame`, `WaitSlot` (WaitForFileFrameSlot), `Cancel` (CancelFileFrameWait) | `VPRendererFilter.cpp` |
| `vpv.presentation_ctl.v1` | `IVpvPresentationCtl` | `SetQueueDelayBudget`, `SetActivePictureLookaheadAutomatic`, `ToggleDvCompareCapture` | `VideoProcessorDlg.cpp` |

`FileHost` and `FileSideData` call none of the 22 (FileHost applies settings through `ApplyApplicationState`, which
is Bill's); they need no change.

## Rules

- **No STL type and no CString across the boundary.** Text is UTF-8 through `IVpvTextSink` (`VpvTextSink.h`);
  `VpvText.h` converts `CString` on both sides.
- **A changed interface gets a new iid, never an edit.** Add `vpv.<name>.v2` beside the old one for as long as an
  older host or DLL of the supported pairing exists; `kVpvExtensionAbi` (`VpvAbi.h`) counts such changes and the
  loader refuses a pairing with a different number.
- **Never cache an interface pointer (D-P2c).** It lives in the plugin DLL for the renderer's lifetime. Host code
  calls `Vpv::Query<T>(renderer)` (`VpvRendererAccess.h`) at each use; the one-line callers are in
  `VpvHostCalls.h` (`VpvCalls::...`), which also return exactly what the old `IVideoRenderer` defaults returned
  when the renderer has no such interface (DirectShow renderers, no renderer, a Bill DLL). `QueryNoCacheTests`
  scans the sources for a stored result.
- **Threading.** The old header stated no thread contract (except the LED shutdown stop and the file-frame slot wait);
  `VpvInterfaces.h` lists the CALLERS TODAY per method. The bridge is stateless beyond a back-pointer, takes no lock and
  calls the renderer's own public method on the calling thread.
- **Pairing.** The plugin exports `VideoProcessorLibplaceboVpvAbi` (defined in `RendererAbiBridge.cpp`, so Bill's
  export file is untouched);
  `Vpv::PluginGuard::Check` (`VpvPluginGuard.h`) refuses a DLL without it or with another number, with a visible
  reason (the same path as an API mismatch). The executable's proxy and the DirectShow filter's loader both call it.
  The build-ID pairing (`VpBuildIdentity.h`) is unchanged.
- **Attach handshake (extension ABI 2).** The plugin API number is Bill's, but this DLL's shared layouts are not
  Bill's (`DolbyVisionLuminanceStatus` 24 -> 28 bytes, `VideoState` members inserted mid-class). A Bill executable
  that loaded it would corrupt memory. So `VideoProcessorLibplaceboCreateRenderer` returns nullptr until the loader
  calls `VideoProcessorLibplaceboVpvAttach(kVpvExtensionAbi)` (`VpvAttachGate.h`, defined in `RendererAbiBridge.cpp`);
  the VP loaders (proxy, filter) do it right after `PluginGuard::Check`. A host that does not know it (Bill's) gets
  its own visible "failed to create a renderer" and never an object of the extended layout. Test code that creates
  renderers itself must call `PluginGuard::Attach` first.
- **Only the seven Vpv iids cross the boundary.** `LibplaceboVideoRenderer::QueryExtension` serves the bridge and
  nothing else; the DLL's module iids (`vpv.vector_engine.v1`, `vpv.camera.v1`, `vpv.resync/1`) stay inside, and DLL
  code keeps calling `RendererExtension::Query` directly.
- **One versioned pair.** `VideoProcessor.exe` and `vprenderer\VideoProcessorVPRenderer.dll` (and the Beta pair) are one
  runtime: deploy them, and back them up, together from the same build. The guard and the attach exist so a mixed pair
  is refused with a visible reason instead of running.
- **C++14.** The Lib, renderer and Test projects compile these headers.

## Files

| File | Role |
|---|---|
| `VpvAbi.h` | `kVpvExtensionAbi` (2), the export and attach names, the retired 25..45 counter history |
| `VpvTextSink.h` | `IVpvTextSink`, `StringSink` |
| `VpvInterfaces.h` | the seven interfaces |
| `VpvText.h` | `CString` <-> UTF-8 |
| `VpvRendererAccess.h` | `Vpv::Query<T>` |
| `VpvHostCalls.h` | `VpvCalls::*` one-line host calls with the old defaults |
| `VpvPluginGuard.h` | `Vpv::PluginGuard::Check` (extension ABI export) and `Attach` (the handshake) |
| `VpvAttachGate.h` | the DLL-side attach state |
| `RendererAbiBridge.h/.cpp` | the renderer side: `RendererAbiBridgeT<Host>` (header-only, testable with a fake host), `MakeRendererAbiBridge` for `LibplaceboVideoRenderer`, and the `VideoProcessorLibplaceboVpvAbi` export (renderer projects only) |

## Provenance notes

- `SetActivePictureLookaheadAutomatic` (bug hunt 3 F2, commit ca651d582, merge 0dc83f0ee) is VP-V's own fix: it
  is not in Bill's tree at a591ad3b or at his later tips, and it is not a row of
  `docs/design/upstream-divergence-register.md`. It therefore lives in `vpv.presentation_ctl.v1`. If Bill adopts an
  equivalent, it becomes an upstream pull-request candidate and moves with his name for it.
- The bridge keeps `LibplaceboVideoRenderer`'s method bodies (they only lose `virtual`/`override`); P11 moves them.

## Tests

`VpvAbiBridgeTests` (every iid, unknown/null iid, text round trip, all 22 methods forward, old defaults without an
interface), `PluginGuardTests` (missing, mismatched, matching export; the built DLLs pass), `QueryNoCacheTests`
(static scan), plus the adapted `VpStatusAdapterTests`, `FileFrameTests`, `LibplaceboPluginProxyTestsVpv` and the
renderer capture harness.
