# Automatic calibration: the remainder (feat/calibration-remainder)

Branch `feat/calibration-remainder`, from local `integ/2026-10-17` (93369c39), with
`feat/calibration-core` (de1d92a0) merged. Local only. The design is
docs/VP-auto-calibration-design.md (status box updated); the user guide is
docs/calibration/AUTO-CALIBRATION.md.

## What was built

| Part | Files | Notes |
|---|---|---|
| Settings | `calibration/CalibrationConfig.h` (header-only) | `[calibration]`: 22 keys, schema in `MainConfigSchema` (owned + validated), `ReadSettings`. Window and settle derive from `display_kind` when left at 0. |
| Meter catalogue | `calibration/MeterCatalog.h/.cpp` | i1Display/Calibrite (incl. Plus HL), ColorMunki Display, i1Display 2, Spyder 4/5/X/X2, DTP94, i1Pro 1/2/3, ColorMunki Photo, JETI, Klein K10. Matches spotread's instrument names; sets sensor-calibration need, CCSS vs CCMX, low-light floor. Any other Argyll instrument runs with generic handling. |
| spotread text | `calibration/Spotread.h/.cpp` | VT-stripping line assembler (split sequences, CSI H/C/X/G, CR redraws), event parser (ready prompt, calibration prompt, reading, read failed / too dark, needs calibration, fatal kinds), locale-free numbers, `XYZ:` and `Yxy:` layouts, usage parser (version, `-c` instruments, `-y` types), command line (-e -x, -c, -y, -Y r/n, -X) with CommandLineToArgvW quoting, install guidance text. |
| ConPTY | `calibration/Terminal.h`, `ConPtyTerminal.cpp` | CreatePseudoConsole loaded at run time, reader thread, kill-on-close job, null std handles (without them a host with redirected stdio leaked the child's output past the console: found by the fake-spotread tests). `RunCaptured` for `spotread -?`, bounded. |
| Argyll discovery | `calibration/ArgyllInstall.h/.cpp` | argyll_path (exe / bin / install folder), ARGYLL_PATH, PATH, `Argyll*` folders in Program Files, LocalAppData\Programs, profile, Downloads, system drive (highest version). A wrong explicit setting is reported, never replaced. Never bundles or downloads. |
| Meter | `calibration/Meter.h/.cpp` | `IMeter`; `SpotreadMeter` state machine (open, sensor calibration via Continue, read, close with q, q, then end its own child), timeouts close a hung instrument, `Abort()` for cancel / exit, redrawn results not taken twice. |
| Patterns (Lib) | `calibration/CalibrationPattern.h`, `PatternCodes.h/.cpp` | Container codes for 8-bit, 8-in-10 (the sigma-delta's domain: limited 4k/1023, full k/255) and 10-bit; limited/full; legal codes 1..254 / 4..1019 so BTB/WTW pass; recorded at the code's exact signal. PQ encode/decode. Centred window by area (any resolution). Settle by display kind plus a dark-drop extra. Near-black strategy: average up to `near_black_max_reads`, stop when two agree, flag below the meter floor. |
| Patterns (renderer) | `vprenderer/LibplaceboVideoRenderer.cpp/.h`, `IRenderer.h`, plugin proxy, API 35 -> 36 | `SetCalibrationPattern` / `GetCalibrationOutput`. The render loop presents the patch every refresh (pl_tex_clear + 1:1 blit for the window) on the VP-owned or libplacebo swapchain, no libplacebo pass: sigma-delta, dither, LUT, tone/gamut mapping, hooks bypassed for the patch frames only; swapchain, DXGI colour space and HDR signalling unchanged. Incoming frames are dropped and the queue regenerated at start/end. Dynamic Drive is stopped (full light) for the patches and restarted after. Optional backbuffer readback (tests). |
| Session | `calibration/CalibrationSession.h/.cpp` | Steps: opening meter, meter calibration, placement (live aim reading), warm-up (skip), measuring (white drift references, interpolated drift correction), fitting (3 LUT slots, display_profile.cfg), verifying (through the LUT, ~60 patches not used for the fit), review (summary + config diff), apply / discard; cancel and failure end patterns and close the meter. Status text format for the RPC. |
| Outputs / apply | `calibration/CalibrationRun.h/.cpp` | raw.jsonl journal, cubes, verify.json, report.html, index.json. Apply: timestamped byte backup beside the config (`.before-calibration.<stamp>.bak`), byte copy in the run's before\, key-only edits (`calibration_lut_enabled` + three slots in the live Color / Output profile), ValidateCandidate, SaveSafely. |
| Controller / RPC | `calibration/CalibrationController.h/.cpp`, `ConfigurationRpcProtocol.h` (Operation::Calibration = 10), client, server, `VideoProcessorDlg` | Text commands status / detect / start / continue / skip / cancel / apply / discard; never blocks the UI thread. Renderer teardown cancels and blocks the sink; Config silent 60 s during measuring cancels; VP exit cancels and joins. After Apply the dialog runs ApplySavedConfiguration. `meter_model: simulated` = dry run with real patches. |
| Config UI | `ConfigEditorWindow.cpp/.h` | Page 23 "Calibration": Meter, Display and patches, Target and LUT cards (a control for every key; coverage test extended), Calibrate card (step, instruction, progress with time left, live reading, review + diff, Start / Continue / Skip / Apply / Discard / Cancel), Detect. |
| Fake spotread | `src/VideoProcessor-FakeSpotread` (new project, test-tools only) | Plays transcript scripts with real console key reads (_getch). |

## Tests

- `CalibrationRemainderTests.cpp` (CPU): spotread text (terminal, results, transcripts, usage, meter names,
  command line, Argyll search), meter state machine on scripted transcripts (colorimeter, calibration prompt,
  failed reading, no instrument / init failure, hang -> timeout, crash, redraw, abort), pattern codes
  (8-bit, 8-in-10, 10-bit, full range, PQ reference values, window geometry, settle, near black, plan order),
  session on a simulated display + meter (full run to Apply: verify dE2000 mean 0.39 / max 0.98 on a 5^3
  plan with a 17^3 LUT; key-only diff, both backups byte-identical, config still validates; Discard;
  Cancel; meter calibration mid-run; read failures and a timeout; full range skips BTB/WTW; FP16 refused;
  drift), schema, index/journal, controller.
- `CalibrationMeterTests.cpp` (CPU, real ConPTY + fake spotread): readings and keys, calibration prompt,
  failed reading, no instrument, hang (timeout, process ended, meter reopens), unexpected exit, probe via
  `spotread -?`, RunCaptured timeout.
- Gated: `CalibrationHardwareTests::RealMeterReadsThroughSpotread` (VP_TEST_METER=1; saves the transcript to
  %TEMP%\vp-spotread-transcript.txt). GPU (VP_TEST_GPU=hardware):
  `RendererCaptureHarnessTests::CalibrationPatternsAreExactCodesInTheBackbuffer`,
  `RendererCaptureHarnessTests::CalibrationDryRunThroughTheRenderer`. Not run here (no GPU, no meter).
- Results: calibration filter 95/95 passed (TRX); the full suite and the Config tests are listed in the
  commit message of this report's commit.

## Not built / open

- LED characterisation runs (flux table, CNTR curve, LED latency) are in the core but not in the wizard.
- Ambient-light re-measure, OLED rest breaks, crash-time "calibration-active" marker restore (VP's own
  shutdown LED restore and the renderer's pattern end cover exit and cancel).
- Patches need the VP Renderer running (any source playing); there is no pattern window without a renderer.
- Verify on a PQ output is implemented but untested; SDR is the tested path.
- The transcripts are modelled on Argyll 3.x output; replace or extend them with real recordings from the
  gated hardware test.
- GPU check needed: that no renderer watchdog reacts to the paused video while patches show.
