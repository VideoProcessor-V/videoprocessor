# VP Renderer DirectShow filter: build report (2026-10-17)

This report covers the integration branch `feat/vp-renderer-filter`, based on
`2f2474f3` with `ci/github-actions` (`bc0ed443`) merged in. The goal is for
MPC-HC and MPC-BE to play local files through VP's full processing chain, the
way madVR is used: the player keeps its graph and VP does the rendering.

## Status

| | |
|---|---|
| Merged and wired | Sub-branches A, B, C, D and F are merged into one branch. The filter now uses C's timing helpers, F's side-data parsers and D's FileHost. |
| Built locally | The filter project. |
| Verified by CI | Each part built in the cloud on its own branch. The fully integrated branch has **not** had a complete CI run. |
| Last complete integrated run | `39067602414`. Everything compiled except one line in the new graph test. That line is fixed in `dac27183`. |
| Blocked | The run for `dac27183` (`39068830686`) was in progress when GitHub began answering every call with **"Sorry. Your account was suspended" (HTTP 403)**, at about 21:35 UTC. Since then nothing can be read from or pushed to the fork. |
| Not on the fork | Local commit `2022309d`, the LED guard (it reached the fork only on the temporary `feat/vp-renderer-filter-next` branch, whose run was cancelled), and this report. |

## What was built

### ABI 35 contract (`9e50b451`)

- `VideoFrameEncoding` gains NV12, P010 and P016. They are appended at the end, so no existing value changes.
- `VideoState::BytesPerRow` and `BytesPerFrame` handle these two-plane 4:2:0 formats, including odd widths and heights.
- `FileFrameTiming.h` (new) carries the frame's timing. Fields:
  - `presentationQpc`: when the frame is due on screen
  - the exact source rate as a rational, plus a measured rate in Hz
  - `playbackRate`
  - flags for discontinuity, paused and file source
- Four new `IVideoRenderer` methods are added at the end of the interface. Their defaults leave capture unchanged:
  - `SupportsTimedFileFrames`
  - `OnTimedVideoFrame`
  - `WaitForFileFrameSlot`
  - `CancelFileFrameWait`
- The plugin API version is now 35. Both the executable's proxy and the filter refuse a plugin with any other version.

### Renderer side (A, `c61c14f6`)

All in `LibplaceboVideoRenderer.cpp`:

- **`OnTimedVideoFrame` (:22718)** handles file frames:
  - No capture timing: no rate/PPM measurement and no pulldown detection.
  - The film clock is the source rate times the playback rate.
  - A discontinuity invalidates the motion vectors and the source-resolution evidence.
  - Motion is off when the playback rate is not 1 or the picture is paused.
- **`WaitForFileFrameSlot` (:22810)** blocks the filter until VP's queue has room, so nothing is dropped. It uses a small gate class, `FileFrameSlotGate.h`.
- **`WaitForDequeueLocked` (:22850)** presents each file frame at its due time.
- **Capture is not affected** by any of the following:
  - The capture branch waits exactly as before.
  - Drift correction (drop/repeat) and backlog recovery are skipped only for file frames.
  - The NV12/P010/P016 upload goes through a new `CTwoPlane420VideoFrameFormatter`.
- The executable's proxy forwards the new methods (`LibplaceboPluginVideoRenderer.cpp:250`).

### The filter (B, `a364be2a`, plus integration)

The project is `src/VideoProcessor-VPRendererFilter` and builds `VPRendererFilter.ax`.

- It derives from `CBaseRenderer`. `ShouldDrawSampleNow` hands every sample to VP at once.
- `Receive` (`VPRendererFilter.cpp:685`) waits for a queue slot outside the filter's locks.
- Accepted media types: P010, P016, NV12, v210 and RGB32, in VIDEOINFOHEADER or VIDEOINFOHEADER2. Samples are copied from system memory and repacked: padded strides are removed and bottom-up RGB is flipped.
- COM interfaces:
  - IVideoWindow: VP draws into a child window of MPC's window.
  - IBasicVideo2
  - IQualProp
  - IVideoFrameStep, which sends `EC_STEP_COMPLETE` after a step.
- Registration is per user (HKCU), CLSID `{54DEA624-D69C-43BF-BAB2-6E04D51F1B95}`, merit "unlikely". It does not reuse madVR's or MPC-VR's CLSIDs.

**Integration (`8a2548eb`).** `FilterTimingGlue.cpp` replaces B's stand-in adapters:

- `FileTimingModel` combines C's helpers:
  - `FilePlaybackState`: graph state, segment rate, discontinuities, held picture
  - `FileClockMapper`: one reference-clock correlation per frame, smoothed fit, mapped to QPC
  - `PtsRateVerifier`: AvgTimePerFrame snapped to a rational (417083 → 24000/1001) and checked against the sample timestamps. The measured rate wins when the header is 0 or disagrees.
- `DecideColor` uses `FileMediaTypeAdapter`. An unsignalled colour space falls back to the same height rule the capture path uses.
- `NewSegment` on the input pin feeds the timing model.

**Side data.** `ReadSideData` (`:564`) queries LAV's `IMediaSideData` on each sample and passes it to F's parsers. A change in the static HDR10 metadata (mastering display, MaxCLL/MaxFALL) sends a new VideoState carrying `HDRData`. Dolby Vision and HDR10+ metadata are parsed and logged, but not delivered to the renderer yet (see "What remains").

**FileHost** (`StartHost` `:470`, `OnHostApplied` `:505`):

- Finds `VideoProcessor.cfg` through `HKCU\Software\VideoProcessor\InstallDir`, then the `.ax` folder, then `%LOCALAPPDATA%\VideoProcessor`.
- Applies the profile snapshot through `ApplyApplicationState`. Rules see `source=file`.
- Applies the queue profile's `queue_size`.
- Passes `VideoProcessorRenderer.cfg` from the same folder to the plugin.
- Follows `file_av_offset_ms` while playing.
- Takes the renderer away from FileHost before the renderer is destroyed, so the LED drive is parked for the hold-over.

### Settings and LED (D, `d90711d7`)

- **`source` rule variable** (`file` or `capture`). It is in the three variable lists and the lookups. The capture app always reports `capture`.
- **New settings, each with a Config control**:
  - `file_av_offset_ms`, -2000..2000, default 0
  - `file_led_holdover_ms`, 0..30000, default 3000
- **LED drive arbiter**: a named mutex in `led/LedDriveArbiter`, so VP.exe and the filter never drive the COM port at the same time.
- **Hold-over** (`led/LedDriveHoldover`): when the renderer goes away between files, the drive is parked and the next file adopts it, so MPC's per-file graph rebuild does not flash full light. It applies only when `source=file`.

**Fixed during integration (`cc871abd`).** FileHost now registers its file-change watch before it first reads the configuration. Before, an edit made just after start could be missed.

**Added (`2022309d`, local only): `VPRendererLedGuard`.** The hold-over holder is never torn down. So a player closed inside the hold-over window would leave the projector at the held LED level, with CNTR above need. The guard watches MPC's top-level window (WinEvent hook, pinned thread). When that window is destroyed, it calls the plugin's `VideoProcessorLibplaceboEndHeldLedDrive` export, which restores full light through the normal stop path.

### Side data parsers (F, `42aec7b1`)

- `3rdparty/lavfilters/IMediaSideData.h` is updated from upstream LAV commit `0e6b8133`. LAV is GPL-2.0-or-later, which is compatible with this GPL-3 project.
- `filesource/FileSideData.*` parses:
  - HDR10 mastering display (LAV stores the primaries in G-B-R order)
  - content light level
  - HDR10+
  - DOVI V1 and V2 into `DolbyVisionTunnelFrameMetadata`: L1/L2/L5/L6/L8/L9/L11 and the reshaping curves
- Every parser requires a minimum buffer size, never an exact one.

## Tests

| Test classes | Owner | Methods |
|---|---|---|
| FileFrameEncodingTests, FileFrameClockTests, FileFrameSlotGateTests, FileFrameFormatterTests | A | 25 |
| FileSourceRationalRateTests, FileSourceClockMapperTests, FileSourcePlaybackStateTests, FileSourceMediaTypeTests | C | 34 |
| FileSideDataTests | F | 12 |
| FileHostTests, FileSourceRuleVariableTests, LedDriveArbiterTests, LedDriveHoldoverTests | D | 33 |
| VPRendererFilterMediaTypeTests, VPRendererFilterRepackTests, VPRendererFilterPluginCheckTests, VPRendererFilterRegistrationTests | B | 23 |
| VPRendererFilterTimingGlueTests | integration | 6 |

- VPRendererFilterTimingGlueTests covers the VideoState with HDR, the rate from the header or the timestamps, the presentation QPC through pause, run, offset, seek, rate 2.0, frame step and stop, a reference-clock correlation, the side-data tracker, and the LED guard.
- The proxy and config API-version asserts now expect 35.

**Gated GPU/graph test, for the owner to run:**
`VPRendererFilterGraphTests.FileSourceLavVpRendererPlaysSeeksStepsAndStops`

- It creates the filter straight from `x64\Release\VPRendererFilter.ax`, so no registration is needed.
- The graph is the clip's source, then LAV Video if installed, then VP Renderer, with a test window acting as MPC.
- It plays, seeks, pauses, steps one frame, runs at rate 2 and stops. It asserts that frames reach VP, none are dropped, the video size is non-zero and the graph never aborts.
- Environment variables:
  - `VP_TEST_GPU=hardware`
  - `VP_TEST_FILTER_CLIP=<file>`
  - optional `VP_TEST_FILTER_SECONDS`
- Without them it logs `SKIP-NO-GPU` or `SKIP-NO-CLIP` and passes. With `VP_TEST_REQUIRE_GPU=1` a skip becomes a failure.

## Cloud CI as the verifier

**Baseline** (integration run `39059919013`, ci-branch run `39059386364`):

| Step | Time |
|---|---|
| Queue wait | 4 s to 2.5 min |
| Toolchain (v142 ATL/MFC installed every run) | 4–5 min |
| Full-solution build | 6–10 min |
| CPU tests, one vstest process | 22–35 min |
| Qt config tests | 6–11 min |
| End to end | 50–61 min |

- Baseline result: 3092 tests, 4 hardware-bound failures. The owner chose a SKIP-NO-HARDWARE marker, being added on `ci/hardware-skip`.

**Sub-branch runs (all compiled):**

| Branch | Run | Result |
|---|---|---|
| A | 39061456395 | 3118 tests. 3 new failures: formatter tests built a DisplayMode under its 100-pixel minimum. Fixed in `cc871abd`. |
| D | 39061887325 | 3125 tests. FileHost watcher race (fixed); the API-34 assert, already fixed by A on the merged branch. |
| F | 39061054096 | 3104 tests. One timing-flaky AnalysisDiagnosticTrace test that F's change does not touch; the same API-34 assert. |
| C | 39061078036 | Failed, fixed by `8b8b779d` (a `small` macro collision). Its rerun 39062642283 had not finished when last checked; result unknown. |
| B | 39062149741 | 1 hour in, its config-test step had not finished when last checked. |

- **Parallelism:** up to 6 of our runs at once, and 9–13 on the fork including other teams. Queues of up to 5 runs formed and waits reached about 2.5 min.
- **Superseded pushes:** cancelled automatically by the per-branch concurrency group.

**Pain points:**

1. Each round trip takes about an hour.
2. The REST API rate limits (core and secondary, HTTP 403) were exhausted by several agents polling with the 3-second default of `gh run watch`. Artifact and log downloads stayed blocked for about 30 minutes.
3. The fork owner's GitHub account was then suspended, ending all cloud verification. The rate-limit abuse may be related; this is not confirmed.
4. "Remote" agent isolation actually ran on the laptop, in worktrees under `C:\dev\videoprocessor\.claude\worktrees`.

## What remains

- **A green integrated run.** Push `2022309d` and this report, then run CI once GitHub access is restored.
- **D3D11 textures from LAV.** Implement `ID3D11DecoderConfiguration` on the input pin and take `IMediaSampleD3D11` textures with zero copies. Today samples are copied back to system memory.
- **Dolby Vision from files.** The metadata is already parsed into `DolbyVisionTunnelFrameMetadata`. Delivering it needs an ABI 36 per-frame entry point; HDR10+ needs a consumer.
- **Subtitles.** Implement XySubFilter's `ISubRenderConsumer2` consumer interface (BSD-licensed `SubRenderIntf.h`).
- **YUY2 and RGB48.** No `VideoFrameEncoding` exists for them yet.
- **Window rectangles.** IBasicVideo source and destination rectangles are recorded but not applied; VP does its own aspect handling.
- **Pause timing.** On a pause from running, frames already queued still present before the picture holds.
- **Installer.** Registration and putting the filter next to `vprenderer\` are not done.
- **Video range.** NV12/P010 are always treated as video (limited) range. VideoState has no field for full range yet.

## Owner-present steps

1. Run the gated graph test on the RTX 3090 with a 24p HDR10 clip and LAV installed:
   - set `VP_TEST_GPU=hardware` and `VP_TEST_FILTER_CLIP`
   - run vstest with the filter `FullyQualifiedName~VPRendererFilterGraphTests`
2. Register the filter for your user only: `regsvr32 VPRendererFilter.ax`, run from the deploy folder with `vprenderer\` beside it.
3. In MPC-HC or MPC-BE: Options → External Filters → Add Filter → "VP Renderer" → Prefer. Set LAV Video's hardware decoding to D3D11, or to none for now since the copy-back path is in use. Subtitles come through XySubFilter or VSFilter; the MPC OSD and shader menus are not available with this renderer.
4. LED: play two files back to back and confirm there is no full-light flash between them. Close MPC within 3 s of a file ending and confirm full light comes back. Check that VP.exe does not drive the COM port while MPC holds it.
