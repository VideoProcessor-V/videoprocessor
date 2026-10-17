# Hunt 2, Dynamic Drive (LED) fix group - report (2026-10-17)

Branch `fix/hunt2-led`, worktree `D:\codex\videoprocessor\hunt2-led`. Base merge-v1.4.02 70d668d7
plus ci/github-actions. Local only: the fork account (vmcpherson2012-dev) was suspended mid-run,
so the last commits are not pushed and have had no cloud CI.

## Fixes (03-led.md and 15-intent.md)
- **#1 CNTR stuck above need after Apply mid-tick (HIGH).** A schedule cut short by new settings is
  completed at the next tick with the believed CNTR. The LPW1/resend sends also stop on new settings.
  Script smoothing restarts from the port value. LedDriveModule.cpp ~1260-1280, ~1560-1610 (bb978b87, e4628017).
- **#2 Failed idle-restore and leave-compensate writes.** The write results are used, and failed
  writes are re-sent or retried. A failed leave-script CNTR 50 goes to the driver's retrying queue.
  LedDriveModule.cpp ~1355, ~1370-1400, 1419.
- **#3/#4 Full-coverage, picture-only peak.** The peak is the largest area mean over boxes of
  screen/854 x screen/480 (the owner's INTER_AREA reference; the scale is unchanged), on the GPU
  pass and in the look-ahead. The detected subtitle and its moved place are masked (eeb8feba).
  The look-ahead now counts every pixel exactly through AreaPeakAccumulator (LedMeasurement.h:162)
  and FullCoveragePeak (LedLookahead.cpp:78). Timing: 3.3 ms at 4K P010, 1.4 ms at 1080p,
  10.5 ms at 4K r210 (2f977f2b). The correlation test now judges against an independent
  INTER_AREA view.
- **#5 BTB:** below black passes through the compensation hook in both gain domains
  (`shaders/LED compensation.glsl`).
- **#6/#7 Render-ahead:** the command frame and the CNTR horizon are counted from now. A queued,
  unwritten LPW1 is kept in the landings (LedDriveModule.cpp:189, 46-56).
- **#8 Stop() is safe for concurrent callers. #9** The restore result is reported.
  **#10** No script LPW1 is sent after a settings change. **#11** Comment fixed.
- **Rename** to "Dynamic Drive" in the UI, logs, crash state and current docs. `[dynamic_drive*]` is
  accepted as an alias of `[led_drive*]`; if both are present, `[led_drive]` wins and a warning is logged.
- **script_lookahead** (LedController.cpp:104 PlanScriptLookahead; module ~1316-1460):
  - **Decision frame:** the frame on screen when the command lands. For a rise, the frame where it
    will have settled (+led_settle_ms); a fall lands on its frame.
  - **Cuts:** a brighter cut in the queue jumps, landing on its first frame (a falling CNTR goes a
    command gap before the LPW1). A cut already on screen that the queue missed also jumps at once.
  - **Out of black:** the light may pre-lift up to black_prelift_max while the picture is black.
  - **Short queue:** a queue shorter than the landing time uses its last frame.
  - **No queue:** behaves exactly as today.
- **Floor:** led_floor only when the queue covers latency+settle, otherwise led_floor_reactive.
- **Queue = look-ahead:** WantedLookaheadFrames returns WHOLE_QUEUE in both modes, and
  led_lookahead_ms is only a cap. The warning is logged only on a real fault.
- **LedDriveConfig.h:~300** comment corrected. Docs: VP-led-core-sources.md intent clause.

## Switches (all default on, Config controls)
script_lookahead, led_peak_full_coverage, led_picture_only, led_btb_passthrough.

## Tests
- **LedScriptLookaheadTests:** stream identical when off or at depth 0; landing within one tick at
  24/50/60 Hz and 50-150 ms (rise, fall, cut); short-queue jump; out of black on time with no early
  brightening; deep floor only with coverage; depth per queue profile; cap.
- **Other suites:** LedDriveHunt2Tests (6), LedDriveMeasurementTests (+full-coverage reference at
  every pixel, accumulator, row reader, 4K cost), ConfigEditorCoreTests, Delta1 (alias, concurrent
  Stop, restore result) and the Config editor tests.
- **Local full CPU run (single process, excluding RendererCaptureHarnessTests and
  LibplaceboLutParserTests):** 2931 tests, 2930 pass. The 1 failure is the known
  RendererMetadataCyclesReuseAcceptedSnapshotWithoutDiskReads.
- **Cloud:** led-a 39063875567 had only the 4 known failures. Run 39067072974 on 30d35782 had 3
  LED failures, fixed in d0cd13b4. Run 39070190180 on d0cd13b4 could not be read because of the
  suspension.

## GPU tests for Claude
`LibplaceboLutParserTests` filter
`FullyQualifiedName~LedCompensationKeepsBelowBlackAndLightExact|FullyQualifiedName~LedMeasurementHookFullCoveragePeakAndPictureOnlyMask`,
plus the Beta host RunBetaFeatureGpuTests (LED compensation BTB) and the chain probe.

## Deferred
- Cut jumps from a dark (not black) scene land on the cut with no settle lead, so the dark scene is
  not lifted early.
- 4K packed RGB look-ahead costs about 10 ms per frame.
- Push and CI wait for the fork account to come back.
