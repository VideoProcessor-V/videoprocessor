# Hunt 2: platform group (GUI lifecycle, capture, EXE/DLL ABI)

Lead branch: `fix/hunt2-platform`. It is based on merge-v1.4.02 `cd544707` with `ci/github-actions` merged.

## Sub-branches merged

- **A (GUI)**: `fix/hunt2-platform-gui`, commit `f52ab578`. Tests: BugHunt2GuiTests.
  - H1, M1 and L5: there is now one shutdown deadline (20 s). It is armed on the first close request, polled whatever blocks shutdown, and backed by a watchdog thread.
    - The forced exit runs a bounded LED restore on every reachable renderer and restores the target-only topology.
    - It keeps the crash state, then terminates the process.
    - The worker join after DoModal is bounded at 5 s.
    - Code: VideoProcessorDlg.cpp:9549-9705, GuiLifecyclePolicy.h and VideoProcessorApp.cpp (after DoModal).
  - H2: the `content` value is held across renderer restarts (ContentRuleHold) and only changes on a real classification change. Code: VideoProcessorDlg.cpp:14032.
  - M6: content changes are read from the 25 ms poll.
  - M5: the retry count clears only after rendering has been sustained for twice the longest back-off. Code: RendererResetPolicy.h, VideoProcessorDlg.cpp:9465.
  - M4: fullscreen-off is honoured while not capturing. Code: VideoProcessorDlg.cpp:9519.
  - M2 and M3, behind `capture_display_fixes`:
    - An inactive target is now activated.
    - The maximum mode is capped at the display's native (preferred/EDID) size, so DSR/VSR modes are not chosen. Code: DisplayTopologySession.h:85-110.
  - L2: display refresh bounds are used. L3: log reopen. L6: shortcut collisions are logged. L10: target-only is re-entered when fullscreen is turned back on.
- **B (capture)**: `fix/hunt2-platform-capture`, commit `1a55afec`. Tests: BugHunt2CaptureTests.
  - M1: a DeckLink signal flagged Rec.601 on any raster no longer throws.
    - The 601 primaries are chosen from the measured rate family (25 Hz family gives 625, otherwise 525).
    - This is a correctness fix, so it applies with `capture_display_fixes` on or off. Code: BlackMagicDeckLinkTranslate.cpp:445-510.
  - L10: reserved EOTF values 4-7 are treated as SDR and logged. Code: BlackMagicDeckLinkTranslate.cpp:412.
  - M4: the `[decklink] ycbcr_12bit_capture` default (and `auto`) is now v210, which keeps BTB/WTW.
    - Setting `rgb12` restores the earlier full-range R12B choice.
    - The existing Config control was relabelled. Code: BlackMagicDeckLinkTranslate.h:45.
  - M2, L11 and L12: clock conversions are now overflow-free 128-bit arithmetic, and the whole-second fallback is gone. Code: IntegerMath.h, TimingClock.cpp, DirectShowTimingClock.cpp, WallClock.cpp, VideoTimingController.cpp.
  - M3: Restart capture stays enabled after a failed start. Code: CaptureStatePolicy.h, VideoProcessorDlg.cpp:6337.
  - L1: DeckLink re-start teardown. L2 (partial): m_state is atomic.
- **C (ABI)**: `fix/hunt2-platform-abi`, commits `b18d49be` and `556d01f6`. Tests: pairing helper, export check and proxy forward, plus tools/test_build_identity.ps1, which runs in CI.
  - The build ID is a content hash of the sources and build inputs. It is generated once per build by Lib, via Directory.Build.targets and tools/generate_build_identity.ps1.
  - The ID is linked into the EXE and both renderer DLLs. The DLLs export `VideoProcessorLibplaceboGetBuildId`.
  - The loader refuses a mismatched DLL and logs both IDs plus the fix (LibplaceboPluginVideoRenderer.cpp:145-152). There is no override, and the API counter moves to 35.
  - Runtime records schema 2 carry the build ID. Packaging fails on a mismatch (`Assert-VpPairedBuildIdentity`, runtime_packaging.ps1:38; release-manifest `buildIdentity`).
  - LOW-1: the proxy now forwards the call it dropped. LOW-2: version.h is generated per project. LOW-3: the CRT handlers now record crash state. INFO-1: guard added.

## Lead sweep

1. **Merge breakage.** The A/B test vcxproj and filters files conflicted; resolved.
2. **Merge breakage.** A string literal in `DebugLog.h` contained a raw newline (C2001). This broke A's CI build. Fixed in `44b1ddbf`.
3. **Forced exit vs build-ID refusal** (`e2b1d563`).
   - A refused plugin made `IsAvailable` false, so the VP Renderer vanished from the renderer list. The configured renderer then fell back silently to the first DirectShow renderer, and the refusal only appeared in the log.
   - A present-but-refused plugin now stays listed. Starting it shows the refusal text in the renderer box (`RendererId::IsLibplaceboRefused`, VideoProcessorDlg.cpp:12385).
   - No automatic retry is scheduled for a refused plugin, because the probe result is cached for the whole process (VideoProcessorDlg.cpp:10498).
   - The forced exit itself is unaffected. With no renderer there is nothing to restore, the topology restore still runs, and the refusal never blocks shutdown.
4. **Magewell retry vs the 10-attempt cap.** No conflict.
   - B added no automatic capture retry, only the Restart button.
   - Renderer retries need CAPTURING, so they stay due (and uncounted) until capture returns.
   - A failed capture start does not affect the shutdown deadline.
5. **12-bit capture vs picture-b** (`fix/hunt2-picture-b`, `b340a2fb`).
   - picture-b fixes the same DeckLink Rec.601 throw. Its version is height-based (`Rec601ColorSpace`) and keeps the throw when `capture_display_fixes` is off.
   - A trial merge conflicts in BlackMagicDeckLinkTranslate.cpp and BugHunt20261017CaptureTests.cpp.
   - **Resolution:** keep platform's `Translate` (rate family, both settings) and keep picture-b's `Rec601ColorSpace`, `DecodeAviColorimetry` and the Magewell changes. Keep both test blocks. picture-b's assertions (720/1080/2160 give 525 when no rate is passed) pass with platform's code.
   - **Remaining gap:** a 50 Hz HD BT.601 signal gets 625 primaries on DeckLink but 525 on Magewell (picture-b maps by height). Align them in the integration.
   - The v210 default feeds limited-range YCbCr into picture's extended-range carry, which is consistent with keeping BTB/WTW. R12B was full range and clipped.
6. **Stale test.** `ConfigFileTests::DeckLinkYCbCr12CaptureChoice` still asserted the rgb12 default; it now expects v210. docs/audit-scope.md was updated (`0fa9e02c`).

## Verification (local; GitHub account suspended, no CI)

- Local x64 Release build without the Magewell SDK (`/p:VpCiNoMagewell=true`, the same as CI; the SDK is absent on this machine) of tip `ec36bfac`. GUI, Test, Config and ConfigTests built with 0 errors.
- CPU tests, single-threaded, with no GPU classes. The filter covered BugHunt2*, BugHunt20261017*, LibplaceboPluginProxy, ConfigFileTests, RendererReset*, CaptureLayerAudit, DisplayTopology*, TimingClock*, DisplayRefresh*, DisplayMode*, ContentIntent*, PostMergeAudit*, Integration20261017*, CrashState* and RendererId*: 355 of 355 passed.
- tools/test_build_identity.ps1: 27 of 27 passed.
- tools/run_config_tests.ps1 -Shards 1, off screen: GATE PASSED.
- Not run: the full CPU suite. Locally the libplacebo tests may use the real GPU, and GPU tests are the owner's/Claude's.
- The last CI run, 39068079647 for tip 0fa9e02c, ended non-zero. The account was suspended before its log could be read.
- No GPU tests were added.

## Deferred

- GUI: L1, L4, L7, L8, L9, L11.
- Capture: L2 (rest), L3-L9, L13, L14.
- ABI: INFO-2.
- 15-intent C3 and C11 belong to the config/timing groups.
