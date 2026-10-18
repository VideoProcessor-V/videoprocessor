# Automatic calibration: phase 2 (feat/calibration-phase2)

Branch `feat/calibration-phase2`, from local `feat/calibration-remainder` (c68a2d1d). Local
only. User guide: docs/calibration/AUTO-CALIBRATION.md (new section 12, limits now 13); design
status box: docs/VP-auto-calibration-design.md.

`feat/dd-projector-profiles` and `feat/intent-remainder` are not in this base. The LED run
writes its result both as the `[projector.default]` keys (what Apply writes; they override a
device profile) and as `.vpprojector` sections (`projector_profile_measured.txt`), and the
ambient check targets the intent key `[vprenderer] ambient_screen_reflect`, which this base's
schema does not know: Apply is refused with that reason until the intent work is merged.

## What was built

| Part | Files | Notes |
|---|---|---|
| Run plumbing | `calibration/CalibrationRunBase.h/.cpp` | `ICalibrationRun` (the LUT session implements it too) and `MeterRun`: open / sensor calibration / placement / warm-up shared with phase 1; `OnFinishing` before the patterns end on every exit; generic key-only Apply (`PreviewKeyEdits` / `ApplyKeyEdits`: diff, validate, beside + run backups, SaveSafely) with an injectable candidate validator; run folders `<timestamp>-led/-ambient/-hdr`. New steps `user_action` and `done` (appended), status `kind:`. |
| Dynamic Drive run | `calibration/LedRun.h/.cpp` | `IProjectorControl` + `DriverProjectorControl` (the module's barco_ascii driver on its own connection; the renderer has stopped the Dynamic Drive while patterns show, so the port is free; write times from the driver's `last*SentAt`). `LedLimits` from light_min/max, the projector's reported range, led_floor_black, led_ceiling, led_cntr_sweep_max (never above 100); every command clamped. Plan: 11-step greyscale (compensation_eotf), LPW1 sweep down and up (black every 4th level), CNTR 50..max with 235 / WTW / black, then (fast sensor only) LED, CNTR and picture steps. Fits: phase 1's `FitFluxTable` / `FitCntrCurve` / `FitStepResponse` / `SummarizeSteps`, plus `FitGreyscaleGamma`. Restore (LPW1 ceiling, LDMM 0, CNTR 50, retried once) at start, end, cancel, any failure, destructor, before patterns end. Crash marker `luts\calibration\led-run-active.txt`; `RecoverInterruptedLedRun` at VP start (VideoProcessorDlg constructor, before the renderer). Outputs: `led_characterisation.cfg`, `projector_profile_measured.txt` (.vpprojector `[light] curve`, `[contrast] max_usable/dmd_clip`, `[timing]`), `led.json`, raw.jsonl, report.html. |
| Fast sensor | `SerialTimedLightSensor` (LedRun) | `led_sensor_port`: one reading per line, value or device-microseconds + value; device times aligned by the least-delayed arrival. Refused on the projector's port or the room sensor's port (mode sensor). |
| Ambient check | `calibration/AmbientCheck.h/.cpp` | Lights off / on (`user_action` + Continue), black and white each, lux from VP's room sensor (new `roomLux` hook) or the owner's value (Continue with `lux: n`, the page's Room light box; dark = 0). `reflect = d black / d lux`, white cross-check, steadiness / disagreement notes, min 5 lux. Suggestion in `ambient.cfg`; Apply to `[vprenderer]` with phase 1's rules. |
| HDR verify | `calibration/HdrVerify.h/.cpp`, `CalibrationPattern.h`, `LedRawTransfer.*`, renderer | ICtCp (BT.2100), dE ITP (BT.2124), BT.2390 EETF, `ToneCurve` (VP's sampled curve / BT.2390 / none). The renderer now samples libplacebo's tone curve for the frame's colour mapping (`SampleToneCurve`, same inference as the LED raw transfer; rebuilt on a signature change, PQ outputs only) and reports it with the sent HDR metadata in `PatternOutputInfo` (64 points, evenly spaced in PQ). Plan: PQ 10000 peak probe, greys 0..source peak (203 reference white), P3 primaries / secondaries at 100 and 400 cd/m2. Targets on the exact codes; above-peak patches marked and not counted; verdict mean <= 2, max <= 4. Ends in `done`; `hdr_verify.json`, report.html. |
| Transcript capture | `calibration/TranscriptRecorder.h/.cpp`, controller | `record_transcripts` wraps the ConPTY in a `RecordingTerminal`; on close it writes `recorded_<meter>_<stamp>.txt` in the fake_spotread / ScriptedTerminal script format (OUT / PROMPT / KEY / SLEEP / EXIT, header with meter, Argyll version, arguments, no install path). data/spotread/README.txt updated. |
| Settings / UI | `CalibrationConfig.h`, `ConfigEditorWindow.cpp/.h` | 12 new `[calibration]` keys, each with a control (new card "Dynamic Drive, room light and HDR runs"); wizard Run selector (`kind:` on Start), Room light box (`lux:` on Continue), titles for the new steps, review shown for `done`. |
| Controller | `CalibrationController.*`, `VideoProcessorDlg.cpp` | `start` takes `kind: calibration / led / ambient / hdr_verify`; LED checks (protocol, sensor port clashes) before anything runs; arguments forwarded to the run; a run interrupted by VP closing is stepped to its finish (restore). Hooks for tests: projector and sensor factories. |

The plugin API number stays 36 (PatternOutputInfo grew): renumber at merge.

## Tests

`CalibrationPhase2Tests.cpp` (CPU: 29 tests in `CalibrationPhase2Tests`, one ConPTY test, two gated hardware tests):

- Settings: defaults, round trip, range refusals; status `kind` and the new steps; arguments.
- LED: limits (configured / reported range, ceiling, CNTR never above 100) and plan; the full run
  on a simulated FL35 (flux with up/down hysteresis, CNTR gain with the DMD clip, LED / CNTR /
  picture dynamics through a fake 250 Hz sensor): flux within 1.5% of the model, hysteresis seen,
  dmd_clip_cntr within 1 of the model's 61.1, LED dead time 80 ms and settle 120 ms within
  10 ms / 10%, CNTR 60 / 30 ms, LED after picture 50 ms; every command inside the limits; the
  restore is the projector's last command (LPW1 255, CNTR 50, closed); marker gone; Apply
  backups byte-identical, key-only, the result validates and `ReadProjectorConfig` reads it back.
  Without a sensor (timings untouched); cancel in the flux sweep and in the CNTR sweep; meter
  failure; projector command failure; projector that will not open (nothing sent); run destroyed
  mid-sweep (restored); unconfirmed restore (retried, marker kept) then `RecoverInterruptedLedRun`
  through the real Barco driver on a fake port (LPW1255, LDMM0, CNTR50 written, marker removed).
  Fit helpers: greyscale gamma, edits and the .vpprojector patch; sensor line parsing and
  device-clock alignment.
- Ambient: the maths (veil, white cross-check, too little lux, negative rise); the run with a
  simulated room veil (sensor dark, entered lit lux, asks when neither): suggestion within 5%,
  Apply refused on this build with the reason, nothing written; with a permissive validator the
  diff is shown and SaveSafely still refuses (last guard); the generic key-only apply with known
  keys (backups, in-place edit, a value VP rejects is refused before any backup).
- PQ: ICtCp neutrality, dE ITP (1% at 100 cd/m2 about 1 JND, colour > luminance, symmetry),
  BT.2390 (identity, peak to peak, monotone), the sampled curve vs BT.2390 within 1%, curve
  choice (vp / pass-through / bt2390 with the measured peak / none), exact PQ codes; the run on a
  simulated PQ display (VP maps 1000 to 600 cd/m2 on a 650 cd/m2 display: mean dE ITP 0.38, max
  0.60, the 1000 cd/m2 grey targets 600), patches above a 400 cd/m2 display marked and not
  counted, SDR output refused.
- Transcripts: the script format from events; record through a scripted terminal and replay to
  the same readings; the file written on close; under the real ConPTY with fake_spotread,
  recorded then replayed by fake_spotread to the same readings and keys.
- Controller: run kinds, unknown kind, no renderer (the projector is never opened), protocol
  none and a sensor on the projector's port refused.

Results (single-threaded, TRX):
- Calibration filter (phase 1 + phase 2): 127/127 passed; gated tests logged SKIP
  (CalibrationPatternsAreExactCodesInTheBackbuffer, CalibrationDryRunThroughTheRenderer,
  CalibrationToneCurveReportedWithTheOutput, RecordRealSpotreadTranscript,
  RealMeterReadsThroughSpotread, ProjectorRestoreOnTheRealPort).
- Phase 1's schema key count test updated (22 -> 34 keys).
- Config tests (`run_config_tests.ps1 -Shards 1`, off screen): 141 passed, 1 skipped (needs a real
  window); 2 failed in the full run ("remote target switch keeps window responsive", "remote-only
  client opens its sole discovered VP": the remote connection timed out while other agents' test
  runs loaded the machine) and both passed when rerun alone (3/3 with `-Match remote`). The
  schema coverage test covers the 12 new keys.
- Full VideoProcessor-Test: FULL_RESULTS_PLACEHOLDER

## Gated tests (for Claude)

- GPU (`VP_TEST_GPU=hardware`): `RendererCaptureHarnessTests::CalibrationToneCurveReportedWithTheOutput`
  (needs an HDR output for the curve checks; SDR checks the fields are empty), plus phase 1's
  `CalibrationPatternsAreExactCodesInTheBackbuffer` and `CalibrationDryRunThroughTheRenderer`
  (the renderer change touches the render loop).
- Meter (`VP_TEST_METER=1`): `CalibrationTranscriptHardwareTests::RecordRealSpotreadTranscript`
  (records to %TEMP%\vp-recorded-transcript.txt), phase 1's
  `CalibrationHardwareTests::RealMeterReadsThroughSpotread`.
- Projector (`VP_TEST_PROJECTOR=COMn`, VP closed): `CalibrationLedHardwareTests::ProjectorRestoreOnTheRealPort`.

## Owner steps

1. Dynamic Drive run on the FL35: Config > Calibration, Run = Dynamic Drive characterisation,
   a source playing, meter on the tripod; stay near the projector the first time. About
   10-15 min. Apply writes the measured `[projector.default]` keys.
2. For LED latency / settle: a fast sensor (photodiode on an ESP32 ADC, or VEML7700 fast mode)
   on its own COM port, `led_sensor_port`.
3. Ambient check once the intent work is merged (or copy `ambient.cfg` by hand): lights off,
   Continue, lights on (enter lux without the room sensor), Continue.
4. HDR verify with an HDR source and VP's HDR output on.
5. Turn on `record_transcripts` for the first real meter sessions and send the
   `recorded_*.txt` files; they replace the modelled transcripts.

## Not built / open

- Custom dim (LDMM 1, LDCR/G/B 0-40) characterisation; the run keeps LDMM 0.
- The CNTR sweep probes 235 and the top legal code only, so `cntr_max_usable` comes from those
  (a lower code could keep gaining after they clip; the review says when the curve was still
  rising at the sweep maximum).
- The HDR verify applies VP's luminance curve per channel to colour patches (exact for greys);
  libplacebo's gamut mapping is not modelled.
- OLED rest breaks and room-light monitoring during the LUT calibration (phase 1 list).
