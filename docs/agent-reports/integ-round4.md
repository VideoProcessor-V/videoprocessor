# Integration round 4 (2026-10-18)

Branch `integ/round4`, worktree `D:\codex\videoprocessor\integ-r4`, from local
`integ/2026-10-17` at 17e534c3. Local only. No GPU. Nothing was built or run in `integ-1017`.

## Merges (all --no-ff, first-parent order)

| Commit | Branch tip | Notes |
|---|---|---|
| ce91cca0 | fix/cntr-bright-cut c1acec6b (includes fix/led-flaky-tests 92e1e8c5) | clean |
| 67188b43 | fix/dv-input-1018 7d164c22 (includes test/dv-tunnel-auto 048b8477) | 4 files in conflict |
| 7df8e987 | feat/calibration-remainder c68a2d1d | 9 files in conflict; plugin API 39 |
| 7de4cc40 | feat/no-fixed-constants b1f02c4c | 3 files in conflict; intent follow-up |
| a751ae9c | (integration) | API 39 in the filter plugin-check probes and the state-report script |
| 92ffc884 | feat/calibration-phase2 a99fdecf (on top of c68a2d1d, added by the coordinator) | 1 file in conflict; ambient Apply |
| 0373be4c | (integration) | LED gap-wait test hardening |

## Conflicts and resolutions

### DV input (67188b43)

- **IDs.** State report and raw capture both used command 33207 and `WM_APP + 25`.
  - Raw capture is now 33208.
  - The DV tunnel hand-off message is now `WM_APP + 26`.
- **VideoProcessorDlg.cpp.** Kept from both sides: includes, message map, handlers, and the capture ingress. At
  ingress the diagnostic frame record comes first, then the hand-off hold-back.
- **Shortcuts.** VP's accelerator table and Config's Shortcuts page now read `ShortcutDefaults.h`.
  - `state_report` Ctrl+Alt+D moved from `kReservedShortcuts` into the defaults table. The reserved list is now empty
    (`std::array<..., 0>`).
  - New test `StateReportDefaultMatchesTheTable`: the table and `StateReportSettings::kShortcutDefault*` agree.
  - The no-clash test passes.
- **Tooltip.** The `dv_tunnel_handoff` tooltip now also says that YCbCr LLDV/HDR10 is declined by the tunnel, even
  under a DV Tunneling profile, and gets the normal LLDV/newlldv handling.

### Calibration (7df8e987)

- **Plugin API 38 -> 39.** `GetCalibrationOutput` / `SetCalibrationPattern` are appended after `GetIntentStatus` /
  `SetAmbientLux` in `IVideoRenderer`.
- **API pins updated:**
  - `ConfigFileTests`;
  - `FileFrameTests::PluginApiVersionIs39`;
  - `LibplaceboPluginProxyTests`, plus a new `IntentAndCalibrationHaveExplicitProxyOverrides`;
  - the `VPRendererFilterPluginCheck` probes: current 39, older 38 (a751ae9c);
  - `tools/test_state_report_onscreen.ps1`.
- **Messages.** `WM_MESSAGE_CALIBRATION_APPLIED` is now `WM_APP + 27`.
- **Render loop (LVR).** The preview wait keeps integ's `WaitForDequeueLocked`, which paces file heads by their due
  time.
  - It gained a `wakeOnCalibrationPattern` flag, so a patch start still wakes the wait, as on the branch.
  - The dequeue wait is unchanged.
  - Both the intent black and the calibration target transfer are kept.
- **Solution, Lib vcxproj, includes.** Both sides kept. I restored a missing `EndProject` in the sln.

### No fixed constants (7de4cc40)

- **Fill dropped frames.** Integ's default ON is kept.
  - The label is now "On (default; pair across gaps up to about 1/6 s)".
  - The tooltip uses the branch's time wording and drops its "Under test / Off (default)".
- **Settings and steering.**
  - The fingerprint keeps the intent fields and adds `general_rate_fixes`.
  - Steering keeps the file-frame handling and adds `SetSpanInTime`.
- **Follow-up: film check in the intent code.** `ContentIntent::FrameContext` and `IntentInterpreter::FrameContext`
  have a new `generalRates` field, set by the renderer from `general_rate_fixes`.
  - Two calls now pass it to `IsFilmLike`: the content observer's `FilmLike` and the motion-intent shots.
  - New test: `ObserverFilmCheckFollowsGeneralRateFixes`.
  - Not changed: the motion-intent shots still use the default `classFixes`.

### Calibration phase 2 (92ffc884)

- **Plugin API stays 39.** `PatternOutputInfo` grew inside 39. No build with 39 has shipped yet.
- **Ambient Apply.** Intent is merged, so VP's validation now knows `[vprenderer] ambient_screen_reflect` (0..1).
  - The run no longer claims the build lacks the intent work.
  - The two ambient tests now expect the edit to be offered and written.
  - New test `AmbientReflectKeyIsAcceptedByVpValidation`: the default validator accepts the key, Apply writes only
    that key after the backups, and an out-of-range value is refused before any backup.
  - The guide (section 12) and a header comment are updated.
- **LED run.** Unchanged: it still writes the `[projector.default]` keys plus `.vpprojector` text.

## Verification (binaries from 0373be4c)

- **Build.** Full x64 Release solution, /m:4 CL_MPCount=2: rc=0.
  - After the phase 2 merge, one build hit LNK1103.
  - I deleted the 18 stale .iobj/.ipdb files and two Lib objects (`MagewellCaptureDeviceDiscoverer.obj`, `cie.obj`).
    The rebuild passed.
- **Build identity.** 28/28. The EXE and both renderer DLLs carry `17c2f63fd4029930ad195fae5bf20e76`.
- **CPU suite.** Single-threaded vstest. Excluded: RendererCaptureHarnessTests, `NlsHookFailureRetry...`,
  DvAutotestGpu*, and `DolbyVisionGpuUnpack...`.
  - **Part 1.** 2279 passed, 2 failed. Console log: `integ-r4-trx\cpu2-part1.log`.
    - It hung in `LedDriveProtocolTests` after `SendScriptReportsWhetherTheWriteHappened`, with no progress for over
      20 min. I stopped it, so no TRX was written.
    - The two gap-wait tests that had not run pass alone, and the class passed 3 times in a row.
    - The tests assumed a fixed 80 ms ordering. Under load this can fail, and the failure can hang the teardown on
      the manual clock. Caused by merge 1 (the led-flaky-tests conversion).
    - Fixed in 0373be4c: the helper now waits for the CNTR write. After the fix, the two tests passed 10 runs in a
      row.
  - **Part 2.** The remaining 109 classes plus LedDriveProtocolTests: 1516/1516 (`cpu2-part2.trx`).
  - **Total.** 3762 distinct tests: 3760 passed, 2 failed under load.
    - The two failures were `DeckLinkConvertersSustained4K60PerformanceComparison` and
      `CDeckLinkRGBToP010VideoFrameFormatter4KSmokeTest`. Both pass when rerun alone (`perf.trx`). They are
      environmental: no converter changed in this round.
- **Config tests.** Off screen, 1 shard: 148 passed, 0 failed, 1 skipped (needs a real window). The gate passed.

## GPU tests for Claude (`VP_TEST_GPU=hardware`)

- DV autotest: `FullyQualifiedName~DvAutotestGpu`, `DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer`.
- Calibration: `CalibrationPatternsAreExactCodesInTheBackbuffer`, `CalibrationDryRunThroughTheRenderer`, and from
  phase 2 `CalibrationToneCurveReportedWithTheOutput`. The render-loop wait changed, so run these.
- `tools\test_state_report_onscreen.ps1` now expects plugin_api 39.
