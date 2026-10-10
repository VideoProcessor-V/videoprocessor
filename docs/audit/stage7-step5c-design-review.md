# Audit — Stage 7 step 5: design review 1 fixes

Diff audited: f7814c07..3542215b
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: guide §9 "Design review 1 responses" (SF-1 to SF-4) and §5,
brief Open questions; plus the owner's extra checks for this audit.

## Blockers
- None in this step. Still open at HEAD from earlier steps: restart on every
  LED setting edit (stage7-step4-compensate.md).

## Should fix
- None.

## Notes
- GammaBoostIsConfiguredInTheModule tests the boost *exponent* for every
  `enabled`/`mode` combination. Nothing tests that the renderer actually
  installs the compensation hook when the module is off (the code does —
  LibplaceboVideoRenderer.cpp ~13908 `if (led || boostExponent != 1.0)`).
- `UpdateLedDrive` turns the boost off if the LED sections fail validation
  (logged: "module and gamma boost off"). The main configuration check
  (MainConfigSchema → `LedDrive::Validate`) rejects such a file first, so in
  practice this is hard to reach.
- `max_levels_per_command` caps relative to the last level *sent*; with a
  pending remainder and a newer, higher target, the newer target wins (as
  documented).

## Verified OK
- SF-1 / owner check: the boost exponent depends only on `gamma_boost` and
  `compensation_eotf` (`BoostExponent`); the renderer runs the compensation
  hook whenever the exponent ≠ 1, with or without the module, in every mode
  and in Low_Latency.
- Owner check: a compensation hook load failure is logged — the chain logs
  "Custom hooks: … not loaded: <reason>", and the renderer adds once, loudly,
  "LED drive: WARNING gamma boost NOT applied …", plus a line when it
  recovers. A runtime disable by libplacebo is logged and leads to the same
  warning.
- SF-2: default gap 200 ms; `max_levels_per_command` 12 caps each LED drop
  (remainder follows in later commands); rises uncapped
  (DimmingIsCappedPerCommandSoTheFloorNeverStepsFar).
- SF-3: `apl_persist_ms` (100) separate from `highlight_persist_ms` (250); a
  cut restarts the APL window so a new scene acts at once
  (SustainedAplRejectsFlashesButFollowsACutAtOnce).
- SF-4 / owner check: the brightest-block reduction (`max_block`,
  `atomicMax`) is the sixth variable of the same storage buffer as the
  histogram, cell sums and totals (one buffer, one readback; layout size
  checked against `RawMeasurement`); checked on the GPU in
  LedMeasurementHookMeasuresWithoutTouchingThePicture. `script` mode uses it
  for its highlight test; `compensate` keeps the percentile; the raw path
  fills the same field.
- Owner check: QueuedFrameStatistics.h and the queued-frame walk changes
  (AnalyzeActivePictureLookahead) contain no LED reference in identifiers or
  comments.
