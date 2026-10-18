# Bug hunt round 3: signal-area fixes (fix/h3-signal)

Base: `integ/2026-10-17` at `17e534c3`. Findings: `docs/agent-reports/hunt3.md`
(branch `hunt/round3`, `1b17919d`), hunter C's list. Local only, no GPU.

## Fixed

### C1 (H) MPC filter stalls - PARKED (WIP commit, owner 2026-10-18)

Causes (all three led to `OnVideoState` refusing the new state with no way out,
so every later `DeliverSample` failed):

1. **The PTS verifier disagreed with every Matroska file.** 1 ms timestamps
   give deltas of 41 or 42 ms, never within one 100 ns unit of 417083. After
   five samples the "measured" rate became 500/21. That is a new
   `DisplayMode`, which the renderer refuses.
   *Red/green:* the verifier on 1 ms stamps with a 417083 header returns
   500/21 at `17e534c3` and 24000/1001 now (standalone check against both
   headers).
2. **Playback rates changed the source rate.** DirectShow sources rate-adjust
   their timestamps, so at 2x the deltas halve and 48 fps was sent as a new mode.
3. **No restart path.** A real change (new size or pixel format, or a profile
   whose effective settings differ for the new state) could never be taken.

Fixes:

- `filesource/RationalRate.h` (`PtsRateVerifier`): a mean period over every
  frame since the last discontinuity, with gaps counted as whole periods. Its
  interval is the timestamps' own quantization spread divided by the periods.
  `Check` agrees when the median or the mean matches.
  `MeasuredRate` picks the integer or k*1000/1001 rate closest to the mean
  inside that interval, else the simplest rational in it. No table of rates.
- `FilterTimingGlue.cpp` `FileTimingModel::ObserveSample`: deltas are scaled
  back by the playback rate. The rate the renderer already has is kept while
  the timestamps agree with it (hysteresis).
- `FilterTimingGlue.{h,cpp}` `FileVideoStateTracker` + `IFileVideoStateSink`:
  - Sends the state when the format generation, the rate or the static HDR10
    metadata changed.
  - An invalid rate keeps the sent rate, but a format or HDR change is still
    sent (before, it was silently skipped).
  - A refusal restarts the renderer once and offers the state to the fresh one
    as its first state.
- `VPRendererFilter.cpp`:
  - `CreateRenderer` is split into `CreateRendererObject` and
    `StartRendererObject`.
  - `EnsureVideoState` uses the tracker through a sink. The restart is
    destroy, create, send, then Build/Start.
  - After a restart, `DeliverSample` waits for the new renderer's slot, so the
    frame (or a paused pre-roll) is not dropped.

Tests: `VPRendererFilterStreamChangeTests` (10 methods). A stand-in renderer
refuses exactly as `LibplaceboVideoRenderer::OnVideoState` does, and the filter
model runs `DeliverSample`'s steps on 1 ms-quantized, rate-adjusted timestamps.
Coverage:

- quantized stamps
- 1.0/2.0/0.5/4.0 rate changes
- size and pixel-format changes
- static HDR arriving later with a profile-change refusal
- seeks and flushes in several orders
- pause/resume and a seek while paused
- all 24 orders of {format, rate, seek, pause}, twice each
- a header-less stream
- a failed restart that recovers later

Gated graph test added for Claude:
`VPRendererFilterGraphTests.FileSourceVpRendererSurvivesRateSeekAndPauseOrders`.
Its gate is the same as the existing one (`VP_TEST_GPU=hardware`,
`VP_TEST_FILTER_CLIP`). It runs three orders of rate, seek and pause events
plus a stop/replay, and asserts after each event that frames reach VP again,
the graph never aborts and nothing is dropped.

### C3 (M) LED arbiter never hands over - PARKED (WIP commit; new switch `led_handover`)

- `led/LedDriveArbiter.{h,cpp}`:
  - `DriveHandoverChannel`: a machine-wide auto-reset request event beside the
    drive mutex, plus `DriveFree()`, a probe that takes nothing. An abandoned
    mutex (a crashed owner) counts as free.
  - Refusals carry `kDriveHeldMarker`, so "held by another owner" can be told
    from a port failure.
  - `SimulateCrashForTest` abandons the mutex the way a crashed process does.
- `led/LedDriveModule.cpp` `Run` (`serviceHandover`, at every tick):
  - **Holder, on a request:**
    1. Stops touching the picture.
    2. Sends CNTR 50 first, so it is never left above the need.
    3. Restores full light (and LDMM 0 when the custom dim applies).
    4. Releases the port and becomes a dry-run standby.
    5. Resets the models and the controller to the ceiling.
  - **Standby, while playing** (a frame within 2 s):
    - asks once per playing spell;
    - a side that yielded never asks, and waits 2 s of grace instead;
    - when the drive is free it opens the port, clears stale requests and runs
      `BringToKnownState` (full light, CNTR 50, LDMM 0), then drives.
  - A side that is not playing never takes the projector.
- Switch and wiring:
  - `led/LedDriveConfig.h` adds `led_handover` (schema and reader).
  - The Config control is next to "Full light after no video".
  - `LibplaceboVideoRenderer.cpp` gives every new module the channel, so
    VP.exe and the filter both get it.

Tests:

- `LedDriveHandoverTests` (6 methods). Two real modules share one stand-in FL35
  through the arbiter, and every write is attributed to its owner. Scenarios:
  - the channel semantics, the crash probe and the held marker;
  - VP hands over to MPC: CNTR 51 becomes 50, light 255, VP silent while MPC
    owns the port, VP takes it back after MPC stops;
  - MPC hands over when VP starts playing, and a side that is not playing never
    takes it;
  - a holder crash: the abandoned drive is taken and CNTR 74 is corrected;
  - a requester crash: the drive is handed over, then taken back;
  - `led_handover` off: the old behaviour.
- `LedDriveDelta1Tests.HandoverSwitchIsReadAndOnByDefault`.

### C2 (M) newlldv veto switches 1-5 frames after the cut

Before, the statistics ran after `OnVideoFrame` at most every 50 ms, and the
switch went through a posted message, so the cut frame and the next ones were
queued as PQ.

- New `HdrVetoCaptureGate.h`:
  - When the veto arms, the UI thread hands it the effective SDR state:
    `BuildHdrVetoSdrState`, which is `BuildPushVideoStateCore` in a preview
    mode that only builds the state, logs nothing and pushes nothing.
  - While armed, the capture thread measures every frame *before* the renderer
    has it. The frame on which the cut detector fires gets
    `renderer->OnVideoState(sdr)` first, so the cut frame is the first SDR frame.
  - The UI still commits the veto (same cut flag) and pushes the same state.
  - A capture-side switch that the UI does not confirm triggers a resync.
- Re-arming: `BuildPushVideoState` re-arms the gate after every push, because
  settings, profiles or the capture state may have changed.
  `UpdateHdrVetoMeasurement` disarms it.

Tests:

- `HdrInferenceTests.ArmedVetoSwitchesOnTheCutFrameAtEveryRate`: rates
  23.976/24/25/29.97/50/59.94/120, cut at frames 1-13. The switch lands exactly
  on the cut frame.
- `VetoGateNeverSwitchesUnarmedDisarmedOrForAnotherGeneration`.

### Low items fixed

| Item | Fix | Test |
|---|---|---|
| C6 container Rec.709 override did not stop PQ inference | `HdrSettingsPermitInference` (HdrInference.h); `HdrModesPermitInference` uses it. Any non-BT.2020 container override forbids the inference. | `HdrInferenceTests.ContainerOverrideToRec709ForbidsTheInference` |
| C7 unknown transfer + HDR10 side data shown as SDR | `DecideColor(format, hasStaticHdr)` gives PQ (assumed) | `VPRendererFilterStreamChangeTests.UnsignalledTransferWithHdr10MetadataIsPq`, `StaticHdrArrivingLater...` |
| C8 DiagnosticHistory cut UTF-8 mid-character | `CopyText` drops an incomplete trailing sequence | `DiagnosticHistoryTests.TruncationNeverSplitsAUtf8Character` |
| C10 stateReportBusy stuck after a failed post | the post is retried while the window lives; a busy flag older than 2 min is released | none (UI only) |
| C11 vprfilter.log opened deny-write | `_SH_DENYNO` | none |

## Deferred

- **C4** (L): the filter always loads stable. Selecting Beta needs a filter
  setting read before FileHost starts, because the plugin loads in
  `CheckMediaType`.
- **C5** (L): the `.ax` is not in the build identity or packaging. This touches
  packaging and the identity generator; it belongs with packaging work.
- **C9** (L): `presented_fps` is never written. No presented-frame counter
  crosses the plugin ABI; it needs an ABI addition.
- **P1/P2** (pending): DV tunnel interactions are fixed or changed in round4.
- **Not done for C1 at the renderer:** an in-place rate change in the renderer.
  The restart path covers it.
- **Not done for C3:**
  - D-LED3, the restore order at exit, is unchanged (area D). The handover path
    itself sends CNTR first.
  - A crashed requester costs one handover and a take-back after the 2 s grace.

## Parked MPC work: state and remaining steps

The owner parked all MPC filter work (C1, C3, and C7/C11 inside the filter) on
2026-10-18. It is committed as its own WIP commit on top of the core commit, so
it can be dropped or finished alone. State at parking:

- Everything builds (full x64 Release, rc=0).
- In the first targeted run (408 tests), the filter, handover and arbiter
  classes all passed: `VPRendererFilterStreamChangeTests` 10/10 and
  `LedDriveHandoverTests` 6/6.
- One regression: `FileSourceRationalRateTests.PtsVerifierToleratesVariableFrameRateOutliers`
  measures a rate with numerator 149 instead of 30000/1001. With frames missing, the new mean
  period is skewed by gap deltas folded against an early median.

Remaining steps:

1. Fix that: fold only deltas within a quarter period of the median into the
   jitter and span, or keep the median snap when the mean interval holds no
   integer or 1001-family rate.
2. Rerun the filter, FileSource, LED and Config classes.
3. Owner GPU run of the two gated graph tests.

## Build and tests

- Full x64 Release build: rc=0.
- Core classes after the veto test fix (HdrInference, DiagnosticHistory,
  DolbyVision): 127/127 passed, `h3-signal-nonmpc.trx`.
- `DiagnosticHistoryTests.ConcurrentWritersNeverYieldATornRecord` failed once
  under load in the first run (the reader thread was not scheduled) and passed
  on the rerun. It is unrelated to the `CopyText` change.
