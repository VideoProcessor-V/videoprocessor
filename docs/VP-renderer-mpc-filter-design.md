# VP Renderer: DirectShow renderer filter for MPC-HC/BE (design, 2026-10-17)

Status: approved by the owner and being built (queue item 13, cloud CI) on branches
`feat/vp-renderer-filter` (ABI 35 contract: NV12/P010/P016 encodings, FileFrameTiming, file-frame
IVideoRenderer methods), `feat/vp-renderer-filter-a` (ABI 35 file frames in the libplacebo
renderer), `-b`, `-c`, `-d` and `-f` (LAV IMediaSideData -> HDR10 / HDR10+ / Dolby Vision). None
is merged here. File and line references below are at merge-v1.4.02 (2026-10-17; plugin API 34).

**Renumbered at integration (2026-10-17, `integ/mpc-state`):** the filter's contract is plugin API **37**, not 35.
Integ took 35 (the required build-ID export) and the report-a-problem hotkey took 36
(`IVideoRenderer::WriteDiagnosticHistory`); the four file-frame methods follow it in the vtable. The filter now
also applies the executable's build-ID pair rule to the renderer DLL it loads. `feat/intent-remainder` (which
also claimed 36) becomes 38 when it merges. "ABI 35" below means that contract.

## Decided (owner, 2026-10-17)

- Local-file playback on the HTPC through "VP Renderer", a DirectShow video renderer filter for
  MPC-HC/BE on the madVR model: the player decodes, plays audio, seeks and handles subtitles; VP
  processes everything after decode.
- One shared core (the same renderer DLL, settings, profiles, Config, Dynamic Drive, tests, repo,
  build and gate) with two front ends (the capture exe and the DirectShow filter), so both stay
  updated.
- The cinema stays on the native capture app. An optional later "VP Capture Source" filter
  (capture inside MPC) is not for now.
- No CLSID impersonation of another renderer. MPCVR (GPL-3) is read only, never copied.
- An upstream MPC-HC "custom renderer" PR is a long shot: ask the owner before any PR.

## Spike result (2026-10-17), replaces risk 1 below

- MPC-HC/BE's renderer list is fixed; select VP Renderer through External Filters -> Prefer (works,
  no upstream change).
- With an external renderer MPC's internal subtitles, OSD and shaders are lost; subtitles come back
  through XySubFilter (`ISubRenderConsumer2`).
- LAV D3D11 output via `ID3D11DecoderConfiguration` / `IMediaSampleD3D11`.
- Dolby Vision side data: `IID_MediaSideDataDOVIMetadataV2`
  {F1949F1C-7474-4F0F-B110-FB734C08851D}.

## Open

- Hotkeys inside MPC, the Dynamic Drive port arbiter and hold-over, and the `file_av_offset_ms`
  key are proposals until built. Owner-present HTPC sessions (end of this document) are pending.

## Design report (design agent, 2026-10-17, read-only, merge-v1.4.02)

### Summary
- **Your proposal works.** One shared core with two front ends fits the code well. `LibplaceboVideoRenderer` already does all the work behind one narrow interface, `IVideoRenderer` (which takes a `VideoState` plus a stream of `VideoFrame`s), and it already owns its own HWND swapchain. The new filter is a thin COM front end that loads the same VPRenderer DLL through the same factory.
- **Existing DirectShow code runs the other way.** `CLiveSource` / `CBufferedLiveSourceVideoOutputPin` push VP's processed frames *into* madVR, MPCVR or EVR as a source. There is no renderer-filter code to reuse directly. What can be reused: the baseclasses project, the media-type translations (inverted), the IMediaSideData HDR reading, and the madVR interface headers.
- **Main technical gap:** frames are timed by capture arrival. File frames need an explicit presentation time and an exact film rate. The renderer already has the right slot for this: the pulldown "film clock" fields `filmHz` and `arrivalQpc`.
- **Main product risk:** I believe MPC-HC/BE's Output → Video Renderer list is a fixed set (EVR-CP, Sync, madVR, MPCVR, Null…). I couldn't confirm this from their source (the fetches failed). If it holds, "VP Renderer" will not show in that list without an upstream player patch. The likely workaround is External Filters → VP Renderer → "Prefer". A day-one spike should settle this.
- **Effort:** about 1.5–2.5 days of agent wall-clock time with parallel agents, plus 3–4 owner sessions on the HTPC.

### 1. What exists today
- **Renderer interface.** `src/VideoProcessor-Lib/IRenderer.h:89-634`: `OnVideoState` (:106), `OnVideoFrame` (:116), `ResetLiveQueue` (:182), `ApplyApplicationState` (:249), plus status getters for de-judder, LED and source resolution (:420-461).
- **Plugin ABI.** `vprenderer/LibplaceboRendererPluginApi.h:25` (version 34). The factory signature (:36-45) takes an HWND, an `ITimingClock*` and a config path. Exports are in `src/VideoProcessor-VPRenderer/LibplaceboPluginExports.cpp:43-88`. The DLL is loaded in `LibplaceboPluginVideoRenderer.cpp:87-135`.
- **How VideoProcessor.exe feeds frames.** `VideoProcessorDlg.cpp:8925` calls `OnVideoFrame`; `:10215/10382/12753` call `OnVideoState`. Inside the renderer, `LibplaceboVideoRenderer.cpp:22367-22699`:
  - derives `arrivalQpc` from the capture clock (:22374-22396);
  - estimates rate and PPM (:22409);
  - runs pulldown detection, which produces `filmHz` and a film-clock `arrivalQpc` (:22461-22582);
  - enqueues with AlphaQueuePolicy, dropping frames on overflow (:22591-22643).
  - The render loop treats `filmHz>0 && arrivalQpc>0` as an exact film timeline: motion-ahead at :26386, the VP per-vsync timeline at :26932-26946.
- **Frame and format types.** `VideoFrame.h:17-86` holds a data pointer, a counter, a timestamp, a discontinuity flag and an IUnknown that keeps the buffer alive. `VideoFrameEncoding.h:20-61` has **no NV12/P010/P016**. The internal converted payload is already 2-plane (`LibplaceboVideoRenderer.cpp:13958-13979`; `LibplaceboVideoRenderer.h:159-189`), so planar upload already exists.
- **Swapchain and fullscreen.** VP's own `CreateSwapChainForHwnd` is FLIP_DISCARD and windowed (`LibplaceboVideoRenderer.cpp:8660-8700`). `SetFullscreenState` is never called, so a borderless fullscreen player window gets independent flip. That is the right model inside MPC.
- **Dynamic Drive lifetime.** `VP-V/modules/drive/led_compensation/LedDriveModule.h:44` is held per renderer instance (`LibplaceboVideoRenderer.cpp:5996`). It is created or reconfigured in `UpdateLedDrive` (:23143) and restores full light on Stop (`StopLedDriveForShutdown`, :24548).
- **Profiles and rules.** Rule variables are listed in **three** places: `RendererProfileConfig.h:1477`, `:2280` and `UnifiedProfileRuntime.cpp:184`. Their values come from `StateVariables.cpp:176` and `LibplaceboVideoRenderer.cpp:1519-1536`.
- **Reusable DirectShow pieces.**
  - `3rdparty/microsoft_directshow_baseclasses` (already in the solution).
  - `microsoft_directshow/DirectShowTranslations.h:22-36` (VideoState → DXVA; needs an inverse).
  - `3rdparty/lavfilters/IMediaSideData.h`: HDR :33, CLL :51, HDR10+ :66. This copy is older than DOVI side data. Its sample-level use is already shown at `CBufferedLiveSourceVideoOutputPin.cpp:4786`.
  - `MadVRRuntimeInterfaces.h` and `MadVROsdServices.h` as a pattern for declaring player-facing interfaces.

### 2. The filter
- **New project.** `src/VideoProcessor-VPRendererFilter` builds `VPRendererFilter.ax`. It derives from `CBaseRenderer` with `ShouldDrawSampleNow` overridden: every sample is handed over immediately and VP owns presentation. `Receive` blocks when VP's queue is full. This back-pressure throttles LAV, so file playback never drops on overflow.
- **COM interfaces to implement:**
  - `IVideoWindow`: `put_Owner`, `SetWindowPosition`, `put_MessageDrain`. VP creates a child HWND of MPC's window and passes it to the factory. Fullscreen returns E_NOTIMPL so MPC handles it.
  - `IBasicVideo2`: native size and aspect.
  - `IMFGetService` + `IMFVideoDisplayControl`, optional (aspect and zoom).
  - `IQualProp`: dropped and late frames for MPC statistics.
  - `IVideoFrameStep`.
  - `ISubRender` (madVR's `SubRenderIntf.h`), so XySubFilter delivers subtitles to any renderer that exposes it.
  - `IKsPropertySet` / IMadVR-style settings: skip.
- **Input media types.** NV12, P010, P016, YUY2, v210, RGB32, RGB48 with VIDEOINFOHEADER2.
  - Colorimetry comes from `dwControlFlags` / DXVA_ExtendedFormat.
  - Per-sample metadata comes from `IMediaSideData` (HDR10, CLL, HDR10+).
  - Phase 2: LAV's native D3D11 output, which needs the renderer to expose `ID3D11DecoderConfiguration` and read `IMediaSampleD3D11` textures (LAV supports this for MPCVR). Copy-back to system memory comes first; on a 3090 it is comfortable at 4K60 P010.
- **ABI change (API 35).** Add the NV12/P010/P016 encodings. Extend `VideoFrame` with `presentationQpc`, `sourceRateHz`, `playbackRate` and `isFileSource`, or add a new `OnTimedVideoFrame`.

### 3. Timing
- **Timestamp mapping:** `presentationQpc = QPC(tStart + rtStart/rate)`, using a QPC↔`IReferenceClock::GetTime` correlation refreshed each sample.
- **Exact film rate** from AvgTimePerFrame snapped to a rational (417083 → 24000/1001), checked against PTS deltas.
- **File frames bypass capture logic:** `UpdateFrameRateAndPPM`, pulldown detection and capture arrival estimation are skipped. `filmHz` and `arrivalQpc` are filled directly (:22547-22566 shows the slot). De-judder, render-ahead and the VP timeline then run unchanged.
- **Seek/flush:** `BeginFlush` calls `ResetLiveQueue()`, and the first sample after it carries `SetSourceDiscontinuity`, which drives MotionValidity's gap logic.
- **Pause:** keep presenting the last frame, the timeline holds, nothing is drained.
- **Frame-step:** one frame, marked as a discontinuity.
- **Rate ≠ 1:** scale `filmHz`; de-judder and motion stay off.
- **New segment:** re-base.
- **Lead and latency:** VP holds a lead equal to its queue depth before each target. A config `file_av_offset_ms` (display chain latency) shifts targets earlier. Late and drop counts go to `IQualProp`.

### 4. Shared core vs front ends
- **Shared:** LibplaceboVideoRenderer, VideoState/VideoFrame, RendererProfileConfig, UnifiedProfileRuntime, the LED module, overlays, DebugLog.
- **Capture-only:** capture devices and clock, the PPM calibrator, the pulldown detector, the DV HDMI tunnel unpack, the stream recorder, CLiveSource.
- **File-only:** the DirectShow file ingress (a graph-free adapter: media type → VideoState, sample → timed frame), the player-clock mapper, flush/seek handling, ISubRender, IVideoWindow.
- **Dolby Vision from files:**
  - LAV Video 0.76 added "passing DoVi metadata to video renderers" and 0.80 added extension metadata. MPCVR consumes it.
  - The vendored header has no DOVI GUID. It needs updating from current LAV (exact GUID and struct to verify).
  - The current tunnel path fills only the DM matrices (`LibplaceboVideoRenderer.cpp:16487-16528`). Files need full `pl_dovi_metadata` reshaping (`comp[]`), mapped the way libplacebo's `pl_map_avdovi_metadata` does it.
  - Profiles 5 and 8 are feasible. For profile 7 only the base layer is used (FEL ignored, like MPCVR). `DolbyVisionLuminancePolicy` is reused.
- **Subtitles:** use ISubRender for clean subtitle bitmaps, composited as a libplacebo overlay *after* VP's viewport logic. VP's placement (aspect retention, CIH relocation) then applies to clean subtitles. The burned-in subtitle detector is not used.

### 5. Settings
- **New rule variable.** Add `source` (`file` | `capture`) to the three variable lists and both lookups. This lets `when: source == file` pick different profiles.
- **Shared config.** The filter finds `VideoProcessor.cfg` through an installer-written key, `HKCU\Software\VideoProcessor\InstallDir`.
- **Profile ownership.** The GUI owns UnifiedProfileRuntime and live-apply today. The filter needs a small `FileHost` that does the same: owns the runtime, watches the config, and hosts the Config RPC server (`VideoProcessor-GUI/ConfigurationRpcServer.cpp`, moved into Lib) so VideoProcessor-Config can edit it live.
- **Hotkeys.** MPC swallows keys. Map them through MPC's message drain or leave them to Config.
- **Dynamic Drive.** Two changes are needed:
  - A machine-wide named-mutex arbiter for the COM port, for when VP.exe and the filter both run.
  - A hold-over of a few seconds in the filter DLL. MPC rebuilds the graph per file, and without it every file change would restore full light and flash.

### 6. Packaging and tests
- **Build.** Same solution and build. The `.ax` ships next to `VideoProcessor-VPRenderer.dll`, and filter version = plugin API version (a mismatched pair fails closed).
- **Registration.** `DllRegisterServer` writes HKCU CLSID entries, user-scoped, via `regsvr32 /n /i:user`; HKLM only if MPC needs it. Add it to `packaging/installer/VideoProcessor.iss` and `install-support.ps1`.
- **CPU tests:**
  - adapter: media type → VideoState;
  - side data → HDRData and DV;
  - timestamp → QPC mapping including rate, pause and flush;
  - rational rate snapping;
  - `source` rule resolution.
- **GPU test (gated):** test graph of File Source → LAV (skipped if not installed) → VP Renderer, checking frames presented, no drops, and the timeline engaged.
- **CI:** build only.

### 7. Risks
1. **MPC renderer list.** MPC's internal subtitles and some OSD only work with renderers it knows. The fallback is XySubFilter + External Filter "Prefer", or an upstream patch to MPC-BE.
2. **DirectShow state machine.** Flush, step and segment edge cases, plus deadlocks between a blocked `Receive` and `Stop`.
3. **HDR signalling.** VP switches the display's HDR mode itself; MPC's own "auto HDR / display mode change" must be off.
4. **Two renderers fighting over refresh rate.** VP's display rules and MPC's auto refresh change can both switch the mode; disable one.
5. **Laptop hybrid graphics.** The swapchain is on whichever adapter MPC's window sits on. LAV copy-back avoids cross-adapter textures; native D3D11 needs a same-adapter check.
6. **Single-instance assumptions** in the renderer: globals such as `g_runtimeManualDisplayRule` (:22195) and MadVRShaderLoader generations.

### Effort (agent wall-clock)
| Step | Time |
|---|---|
| MPC selection spike | 1–2 h |
| ABI 35, encodings, timed frame | 3–4 h |
| Filter skeleton, pin, IVideoWindow / IBasicVideo2 / IQualProp | 4–6 h, in parallel |
| Timing adapter and tests | 3–4 h, in parallel |
| FileHost: profiles, RPC, `source` variable | 3–4 h |
| LED arbiter | 2 h |
| ISubRender overlay | 3–4 h |
| DOVI side data → `pl_dovi_metadata` | 4–6 h |
| Installer and test gate | 2–3 h |
| Phase 2: native D3D11 | +4–6 h |

### Owner-present steps
1. Confirm how MPC-HC/BE lets you select the renderer (from the spike).
2. HTPC session with SDR 23.976 and 24p files: seek, pause, frame-step, A/V sync with `file_av_offset_ms`, de-judder.
3. HDR10, HDR10+ and DV profile 5/8 files: tone mapping, and Dynamic Drive behaviour across file changes.
4. XySubFilter subtitles in a CIH setup, plus checks of fullscreen, refresh switching and the laptop's hybrid graphics.

### Critical files for implementation
- src\VideoProcessor-Lib\vprenderer\LibplaceboVideoRenderer.cpp
- src\VideoProcessor-Lib\vprenderer\LibplaceboRendererPluginApi.h
- src\VideoProcessor-Lib\IRenderer.h (with VideoFrame.h and VideoFrameEncoding.h)
- src\VideoProcessor-Lib\RendererProfileConfig.h (with UnifiedProfileRuntime.cpp)
- src\VP-V\modules\drive\led_compensation\LedDriveModule.h

Sources: [LAV Filters changelog](https://www.codecguide.com/changelogs_lavfilters.htm), [MPC-HC 2.3.0 release notes](https://newreleases.io/project/github/clsid2/mpc-hc/release/2.3.0), [Doom9 renderer-selection thread](https://forum.doom9.org/showthread.php?p=2000720)
