# Bug hunt round 3: integration audit of integ/2026-10-17 (17e534c3)

FINDINGS ONLY: no product code was changed. Local only.

**Status: interim (forced hand-back).**
- Hunter C reported in full.
- Hunters A, B and D were still running at hand-back. Their findings come from their scratch notes in `D:\codex\hunt3-scratch\{a,b,d}` and may be incomplete.
- The lead's Stage 2 sweep was partial: X1, X2 and the known items K1-K4.

Builds and tests:
- C: full x64 Release rc=0; CPU tests 302/302.
- D: full x64 Release rc=0; CPU tests 662/665. The 3 failures are LedDriveDelta1 wall-clock tests under load. The injected-clock fix is in round4 (92e1e8c5, pending).

Areas: A time, B picture, C signal, D control, X lead interaction. Sorted by severity. "pending" means the finding interacts with integ/round4.

| ID | Sev | Area | File:line | Finding |
|---|---|---|---|---|
| C1 | H | signal | VPRendererFilter.cpp:589-623, 393-398; FilterTimingGlue.cpp:71-97; LibplaceboVideoRenderer.cpp:23866 | MPC filter stalls. After start, the PTS rate verifier (after 5 samples) or a media-type change produces a new DisplayMode. OnVideoState refuses it and there is no restart path, so every DeliverSample fails. Seeks re-trigger it. |
| B6 | M/H | picture | DetailPulse APPLY_BODY; LibplaceboVideoRenderer.cpp:22757-22793; DetailPulseTests.cpp:583 | On a repeat apply, detail pulse reuses A's stored low band on a different picture (Stage B phase, gap fill, warp extras). This leaves a ghost of A's low band, up to x0.35. The GPU path differs from the CPU model, and the test only repeats the same picture. |
| X1 (=B1) | M | **interaction** latch x BTB/WTW | LibplaceboVideoRenderer.cpp:22457, 22809-22848; ExtendedRange.h:215, 234-242 | The latch crossfade and release fade use pl_render_image_mix. ExtendedRange::ForTemporal and mixClipsBtb check only temporalRenderer. As a result, BTB is clipped on every fade refresh, and the LateRestore modes (calibration LUT, libplacebo gamut mapping) restore from the last frame drawn instead of the mix. Pending: calibration LUTs make LateRestore common. |
| X2 | M | **interaction** latch x detail/deband | LibplaceboVideoRenderer.cpp:22696, 22809-22818, 1212 | The crossfade's "from" path is the frame before the debander, detail pulse and texture restore. These stages therefore drop out in one step at the start of a fade (and of a release). detail_pulse_stabilise is on by default. |
| K2 | M | latch | SourceResolutionDetector.h:443-445, 459-476 | Lightning gives false cuts: shape change plus a level jump, with no flash or return test. The engage can land in-shot and extra shots are counted. Meridian: 11/840 flagged. Fix: a cut whose pattern re-correlates with the pre-cut pattern within about 0.5 s (look-ahead) is a flash. |
| K3 | M | time/crop | LibplaceboVideoRenderer.cpp:29723-29742 | The uncropped first frame stays on screen through start-up stalls: 1.0 s native, 1.5 s at 1080 (gate-verify). BeginQueueGeneration resets the timeline, so this recurs at every renderer reset or live queue reset. Fix: hold the first present until the first decision, with a bounded timeout. |
| C2 | M | signal | VideoProcessorDlg.cpp:9306-9310, 12984-13125; LibplaceboVideoRenderer.cpp:24060 | The newlldv veto returns to SDR 1-5 frames after the cut. Its statistics run after OnVideoFrame, sampled about every 62 ms. |
| C3 | M | **interaction** signal x LED | LedDriveModule.cpp:1173-1182; LedDriveArbiter.cpp:213-238; LibplaceboVideoRenderer.cpp:25131 | The LED arbiter never hands over, and the side that loses stays a dry run. VP.exe drives LPW1/CNTR from the capture picture while MPC plays, so CNTR can sit above need. |
| B2 | M | picture | LibplaceboRenderParameters.cpp:138-147 | Ambient adaptation is inert for SDR input: blackNits is ignored, while the log says the target black changed. |
| A1 | M | time | LibplaceboVideoRenderer.cpp:~14957-14969; DejudderWarp.h:520 | The global vector is converted to pixels with a fixed 8, ignoring search halvings. At 4K it reads 2x low, so intent motion is wrong and the fast-pan switch needs 1.0 h/s. |
| A3 | M (L-M) | time | AlphaCadenceCorrectionPolicy.cpp:12-60 | The arrival-rate fit is not reset when a pulldown lock change changes the rate hint. The capture rate is wrong for about 15 min and drift correction is blocked (dejudder_pulldown on). |
| A4 | M | **interaction** time x hiccup | LibplaceboVideoRenderer.cpp:14603-14668, 14871, 20941-20970 | Hiccup repair is on by default. With a de-judder profile on a 50/60p source, the MotionWarp and NVOF engine are created and released on the render thread at every hiccup, so the repair can be late. |
| D-LED1 | M | control | LedLookahead.cpp:~279; LedMeasurement.cpp:~455 | The look-ahead time base (sequence x current period) goes backwards on a pulldown lock change or a hold-over adopt. WindowMinimum then sticks, highlightHistory is not cleared at a cut and grows without bound, and the sustainedHighlight protection is lost. |
| D-LED2 | M | control | LibplaceboVideoRenderer.cpp:25136; LedLookahead.cpp:252-254 | A hold-over adopt keeps the old observer sequence, so the new file's frames are skipped and stale predictions are used. |
| D-LED3 | M | control | ProjectorDriver.cpp RestoreFullLight | Restore sends LPW1 and then CNTR 200 ms later. CNTR is the command dropped by the bounded exit budget, so it can stay above need. Unchanged in round4 (pending). |
| D-S1 | M | control | VideoProcessorDlg (no WM_QUERYENDSESSION/WM_ENDSESSION handler) | On Windows shutdown or logoff there is no LED restore and no orderly teardown. |
| D-S2 | M | control | no OnCancel override | Esc goes to IDCANCEL and EndDialog, bypassing OnClose. |
| D-S3 | M | control | VideoProcessorApp.cpp:2241 vs VideoProcessorDlg.cpp:5324 | The crash state is stopped as clean before the dialog destructor's unbounded joins, so a hang there leaves no crash state. |
| D-S4 | M | control | VideoProcessorApp.cpp:2319 | Forced exit blocks on the RestoreDisplayTopology mutex while the UI thread is hung. |
| D-S5 | M | control | ConnectionTest.cpp:52-55; VideoProcessorDlg.cpp:3229 | The detached RS-232 test worker is killed at exit, leaving the LED at 60. |
| D-S6 | M | control | low-level keyboard hook | Shortcuts are consumed or fired system-wide: Ctrl+Shift+S/U are swallowed, and Alt+Enter fires in MPC. |
| D-S7 | M | control | ShortcutDefaults.h | The bare-key defaults (Esc, R, V, Shift+R/V, F4) break the owner's rule. Still present in round4 (pending). |
| D-C1 | M | control | VideoProcessorApp.cpp:657-662, 834 | newlldv_sdr_veto=false is ignored because only true is appended. |
| A2 | L-M | time | LibplaceboVideoRenderer.cpp:29846-29855; QueueLookahead.h | The auto lookahead (target-1) cancels the locked-film queue target when the target is above 5. |
| D-C4 | L-M | **interaction** config x hiccup | ConfigEditorWindow.cpp:8775; LibplaceboVideoRenderer.cpp:14603, 6073 | interpolation_grain is greyed out while hiccup repair (on by default) uses it. |
| B4 | L-M | picture | LibplaceboVideoRenderer.cpp:22779-22793 | The detail-pulse look-ahead is measured before deband but Apply is measured after it, giving a systematic gain above 1 when deband is on. |
| B7 | L-M | picture | TextureRestore.cpp:640-645 | A repeat overwrites the low history with an in-between picture, so the change veto and cut detection read the wrong frame. |
| B9 | L-M | **interaction** picture x subtitles | LibplaceboVideoRenderer.cpp:18550; SubtitleCutPasteShader.h:926 | With subtitle composition, detail pulse is skipped and its history restarts at each cue, so GOP breathing returns while subtitles are on screen. |
| K1 | L | latch | SourceResolutionDetector.h:445 | Same-brightness shot/reverse-shot cuts are missed: decorrelation 0.78-0.90 with a jump under 0.2, below the 0.9 alone threshold. This only delays the engage, at most until the 20 s crossfade. Fix: an alone floor around max(0.7, motion+0.3) with a persistence check. |
| K4 | L | latch/crop | gate-verify section 3 | The crop engaging 2-3 s late under re-base is a cold shader compile, not the re-base. With a warm cache the crop lands at frame 4-5. It still happens on the first launch after an install or cache clear. |
| A5 | L | **interaction** hiccup x de-judder x intent | LibplaceboVideoRenderer.cpp:14603-14612 | A standalone gap pair forces de-judder strength to 1.0 for one frame. |
| A6 | L | time | IntentInterpreterObserver.h:208-262 | A skipped frame is treated as contiguous, so the pan reads 2x for that pair. |
| B3 | L | **interaction** latch x de-judder | LibplaceboVideoRenderer.cpp:~15828, 15982 | Warp extras skip the crossfade, giving refresh-rate flicker during the fade. |
| B5 | L | picture | LibplaceboVideoRenderer.cpp:22757-22777 | The Stage B pulse of A runs without the debander, so the history mixes domains. |
| B8 | L | picture | DetailPulse.cpp:687 | Detail pulse is silently skipped on native RGB, while the status says it is on. |
| C4-C11 | L | signal | see Hunter C list below | Minor filter, signal and state-report issues (listed below). |
| D-C2,3,5-9 | L | control | ConfigEditorWindow.cpp:8127-8140, 7593, 2939, 2335; VideoProcessorApp.cpp:798 | Minor Config issues (listed below). |
| D-S8-12 | L | control | | Minor shortcut, exit and instance issues (listed below). |
| P1/P2 | pending | signal | VideoProcessorDlg.cpp:12866-12876; round4 DolbyVisionTunnelGate.h | A DV Tunneling profile blocks newlldv inference (fixed in round4). In round4, a tunnel hand-off starts a new epoch, so PQ is inferred again. |

### Low-severity items behind the grouped rows

- **C4-C11 (signal):**
  - The filter always loads stable 7.360, never Beta 377.
  - The .ax is not covered by the build-identity check.
  - A REC709 container does not stop PQ inference.
  - Unknown transfer with HDR10 side data is shown as SDR.
  - DiagnosticHistory can cut UTF-8 mid-character.
  - presented_fps is always 0.
  - stateReportBusy can stick on.
  - The filter log is opened deny-write.
- **D-C2,3,5-9 (Config):**
  - Latch fields show "Auto".
  - The retired even_rates value is shown raw.
  - Content-gated warnings are hidden.
  - The legacy queue fold target differs from VP's.
  - Discarding remote edits skips the fold.
  - The docs lack the new keys.
  - [general] queue_size is never read.
- **D-S8-12 (shortcuts, exit, instances):**
  - display_rules_auto has no Config control.
  - The shortcut grammar diverges between Config and VP.
  - The state-report worker is detached at exit.
  - Two instances share crash-state.txt.

## Known items

| Item | Verdict | Rating |
|---|---|---|
| K1: shot/reverse-shot cuts missed (0.9 threshold) | Confirmed | L |
| K2: lightning causes false cuts | Confirmed | M |
| K3: uncropped first frame for up to about 1 s at start-up | Confirmed; recurs at every reset | M |
| K4: crop engages 2-3 s later under re-base on a cold start | Cold shader compile, not the re-base | L |

## Proposed fix groups

1. **File filter robustness** (C1, C4, C5, C7): restart when the state is refused, or keep the DisplayMode rate fixed for file frames.
2. **Mix paths and BTB/WTW** (X1, X2, B3): run ForTemporal and mixClipsBtb for the crossfade, draw the from-path through the same VP stages, and crossfade the extras.
3. **Detail stages on non-A pictures** (B6, B7, B4, B5, B8, B9): compute bands per picture, measure in one domain, and handle RGB and composed pictures.
4. **Latch and crop** (K1-K4): add a flash test and a lower alone floor, hold the first present until the crop decision, and prewarm the shader cache.
5. **Timing** (A1-A6, C2): scale vectors with the halvings, reset the estimator when the hint changes, keep the matcher for gap fill, treat a sequence gap as a discontinuity, and measure the veto before the frame is queued.
6. **LED** (C3, D-LED1-3, D-S5): let the arbiter re-acquire, use a monotonic time base, reset on adopt, and send CNTR first on restore.
7. **Shutdown and shortcuts** (D-S1-S4, D-S6-S7, D-S8-12).
8. **Config** (D-C1-C9).

## GPU repro ideas (for Claude)

- **X1:** extended-range chain test that forces an auto crossfade; check the BTB codes, with and without a LUT target.
- **C1:** filter graph test with a header-rate mismatch, then a seek.
- **B6:** GPU-vs-model test on Stage B pictures with motion.
- **A4:** render_ms of the gap frames with a 60p de-judder profile.
- **K3:** time the first cropped present after a cold start and after a queue reset.
