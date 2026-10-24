# LED compensation (adaptive gamma) (`drive.led_compensation`)

Unit: **drive** | side: output | order: 230 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

The Dynamic Drive core (`LedDriveModule`) and the picture compensation it drives. The module runs the controllers and the projector driver, and gives each frame a gain. The renderer applies that gain with shaders/LED compensation.glsl at PRE_OUTPUT so content light stays constant while the LED (and CNTR) move; CNTR never above need. The same hook also carries the gamma boost (even with the drive off), the ANSI shadow lift (veil) and the script-mode CNTR smoothing. This folder also holds the drive-ownership mutex (`LedDriveArbiter`) and the between-files hold (`LedDriveHoldover`).

## Contract

- Input: target-light picture at PRE_OUTPUT (after `drive.led_measure`'s hook), plus the frame's `CompensationGain` from `LedDriveModule::BeginFrame`
- Output: compensated picture. Compensate: a linear-light gain = flux(ceiling) / flux(modelled LED) / modelled CNTR light, limited by the highlight cap and knee. Script with `script_smoothing`: an encoded gain (at most 1) bridging whole CNTR steps. Below-black and above-white pass through (`led_btb_passthrough`, `led_wtw_passthrough`)
- Geometry: output size (the PRE_OUTPUT texture), whole picture
- Delay share: no picture delay. The gain follows the modelled LED: a send lands `led_latency_ms` after the driver wrote it (`led_landing_ms` on; else in whole frames), then settles with `led_settle_ms`; CNTR the same with `cntr_latency_ms` / `cntr_settle_ms`. With render-ahead the frame's time is its predicted display time (`BeginFrameAhead`)
- Look-ahead: compensate aims each command at the predicted stats of the frame it lands on (latency in source frames, from `drive.led_measure`'s predictions). When the look-ahead covers that frame the floor is `led_floor`, otherwise also `led_floor_reactive`. No queue (Low Latency): the latest exact stats

## How the code works

### Public API

- `LedDriveModule` (LedDriveModule.h):
  - Lifetime: constructor (`ProjectorConfig`, `LedDriveConfig`, port factory, clock), `Start`, `Stop` (restores full light; safe for concurrent callers), `Reconfigure` (live settings; returns false when a new module is needed: connection changed, or driving starts or stops).
  - Per frame (render thread): `BeginFrame` / `BeginFrameAhead` / `BeginFrameAt` return the `CompensationGain`; `SubmitMeasurement` takes the exact stats; `SetDisplayGamma`, `SetSourceRange`, `SetRawTransfer`.
  - Look-ahead: `Lookahead()` (the observer to subscribe to the queued-frame walk), `WantsPredictions`, `WantedLookaheadFrames`, `SubmitPrediction`, `ResetLookahead`.
  - Intent limits: `SetIntentLimits` (LED floor from `intent_black: led_protect` and ambient adaptation).
  - Diagnostics: `ProjectorStatus`, `DrivingProjector`, `CurrentGain`, `ModelledLight`, `AssistContrast`, `Recovering`, `MsSinceLastFrame`, `Config`.
- `CompensationGain`: `active`, `gain`, `domain` (encoded / linear), `knee`, `eotf`, `veil`. The renderer copies these, plus the gamma boost and passthrough flags, into the hook parameters (`gain`, `knee`, `gain_domain`, `compensation_eotf`, `boost`, `boost_knee`, `boost_knee_width`, `shadow_lift`, `veil`, `wtw_passthrough`, `btb_passthrough`).
- shaders/LED compensation.glsl: an mpv-style PREOUTPUT hook loaded by the renderer as rule `led_compensation`. It applies the boost first, then the shadow lift, then the gain with a soft knee below white; sign-preserving; negatives pass through when `btb_passthrough` is on.
- `ArbitratedSerialPort` / `ArbitratedSerialPortFactory` (LedDriveArbiter.h): wraps the serial port so `Open` first takes the machine-wide mutex `Global\VideoProcessorLedDrive`. A second owner (VP.exe vs the VP Renderer filter) fails `Open` and runs as a dry run.
- `Holdover<Drive>` / `ProcessLedDriveHoldover()` / `EndHeldLedDrive` (LedDriveHoldover.h): in the filter process (`source=file`) the outgoing renderer parks its running drive for `file_led_holdover_ms`. The next renderer adopts it when its LED settings match; otherwise it is stopped (full light).

### Data flow

1. The renderer (`UpdateLedDrive` in LibplaceboVideoRenderer.cpp) builds the module with the arbitrated port factory (or adopts a held one), starts it, and subscribes its look-ahead observer.
2. Each frame: `BeginFrame` lands the driver's writes into the LED/CNTR models, steps `CompensateController` + `CntrPairing` + cut recovery + custom-dim fade (compensate), posts `SetLight` / `SetContrastAsync`, and returns the gain.
3. The renderer sets the hook parameters and pushes the hook after the measurement hook (if slot `drive.led_compensation` runs). After the render it feeds `SubmitMeasurement`.
4. Script mode decides on the module thread (`Run`) every `tick_ms`, or in the script's own pacing, and sends with `SendScript`.

### Threading

- Render thread: `BeginFrame*`, `SubmitMeasurement`, the look-ahead observer calls, `SetDisplayGamma` / `SetSourceRange` (atomics).
- Module thread (`Run`, started by `Start`): port open and handshake, script ticks, the compensate log (so the render thread never writes it), `led_idle_restore_s`, and the exit restore (`RestoreFullLight`) before the thread ends.
- One `mutex` guards the module state. Render-thread calls take it briefly and never wait on I/O: serial sends happen outside it, and compensate only posts asynchronous driver calls.
- Atomics: `compensating`, `applyGain`, `displayGamma`, `sourceRange`, `lookaheadEpoch`.
- `Reconfigure` and `SetIntentLimits` may come from any thread.
- The driver has its own worker thread (`drive.projector_control`). The arbiter holds its mutex on a thread of its own. The holdover runs one process-wide thread that stops a parked drive at its deadline.

### Config keys actually read

- `[led_drive.default]` (alias `[dynamic_drive.default]`; validated by `LedDriveRules` in led/LedDriveConfig.h):
  - Mode: `enabled` (off), `mode` (compensate | script | log_only | picture_only; `listen` retired), `command_source` (internal_script | compensate | none; `python` retired), `adaptive_gamma` (picture gain on/off; LED drive unaffected).
  - Look-ahead and timing: `lookahead` (on), `led_lookahead_ms` (0), `led_landing_ms` (on), `led_idle_restore_s` (10).
  - Controller: `led_floor` / `led_floor_reactive` / `led_floor_black` / `led_ceiling`, `rate_*`, `rate_preset`, `deadband`, `early_ramp`, `cut_jump`, `bright_cut_dump`, `highlight_tolerance_stops`, `highlight_area_small` / `_large`, `highlight_release_ms`.
  - CNTR assist: `cntr_assist` (on), `cntr_assist_max`, `software_first`, `cntr_pair_predict`, `cut_cntr_dump`, `cut_cntr_level`, `shaped_recovery`, `max_cut_dip`, `recovery_bias_pct`, `recovery_rate_*`, `black_prelift_max`, `preposition_black_only`, `cut_lead`.
  - Custom dim: `ftb_*`.
  - Correction: `correction_weight_by_time`.
  - Hook parameters: `knee` (0.08), `compensation_eotf` (2.41), `gamma_boost` (off), `gamma_boost_knee`, `gamma_boost_knee_width`, `shadow_lift_ansi` (only with `adaptive_gamma`) with `shadow_lift_ansi_ramp_lo` / `_hi` / `_rate`, `led_wtw_passthrough` (on), `led_btb_passthrough` (on).
  - `profile_file` (an LED profile merged under this section).
- `[projector.default]`: `led_latency_ms`, `led_settle_ms`, `cntr_latency_ms`, `cntr_settle_ms`, `cntr_max_usable`, `dmd_clip_cntr`, `flux_table`.
- `[general] file_led_holdover_ms` (3000; VP Renderer filter only).
- Renderer profile: `intent_black: led_protect` and ambient adaptation set the floor through `SetIntentLimits`.
- Mismatch: `led_compensation` (Settings below, slots.json, MANIFEST) is not a config key; it is only the shader rule name in LibplaceboVideoRenderer.cpp.

### What the user sees when it falls back

- The shader does not load, or libplacebo disables it: no "Fallbacks engaged" entry. Only when the gamma boost is on, a log warning: "Dynamic Drive: WARNING gamma boost NOT applied - shaders\LED compensation.glsl did not load or was disabled".
  - The module is not told. In compensate it keeps dimming the LED with no picture compensation, so the picture goes dark.
  - The listed entry `LED compensation: off (shader did not load)` and the behaviour "drive held at full" are not implemented.
- Slot `drive.led_compensation` off while `drive.projector_control` runs: the same (LED dimmed, no compensation), with no notice.
- Dry run (no port, or the drive mutex is held by another owner): `applyGain` is off, so no picture gain; log only.
- Invalid `[led_drive]` / `[projector]` settings: log "Dynamic Drive: configuration rejected (...); module and gamma boost off". An invalid LED profile is ignored with a log warning.
- Look-ahead starved (frames queued but no predictions): log "Dynamic Drive: WARNING - ... every cut and exit from black is reactive". Low Latency (no queue) is logged as reactive, not as a fault.
- Hold-over park, adopt and stop: log lines only ("LED drive: ... holding the LED drive for N ms", "adopted", "ending the held LED drive").

### Dependencies (to lift it)

- `drive.projector_control`: ProjectorDriver, SerialPort, LedController, LedClock.
- `drive.led_measure`: LedMeasurement, LedLookahead.
- led/LedDriveConfig.h; DebugLog.h; Win32 (arbiter mutex, `GetTickCount64` in the holdover).
- The renderer builds the look-ahead transfer with led/LedRawTransfer.* (legacy path).
- The shader file shaders/LED compensation.glsl and the renderer's hook chain (`CustomHookChain`, `GlslHookLoader`) to apply the gain. The module itself has no libplacebo dependency.
- Callers: LibplaceboVideoRenderer.cpp (`UpdateLedDrive`, the per-frame block), LibplaceboPluginExports.cpp (`EndHeldLedDrive`), VideoProcessorDlg.cpp (shutdown restore, connection test).

### Test it standalone

- `LedDriveCompensateTests`, `LedDriveDelta1Tests` (compensate controller, CNTR pairing never above the need, cut recovery, flux table, config reading).
- `LedDriveHunt2Tests`, `LedDriveHunt3Tests` (resends, render-ahead landing, stream restarts, mode switches with the thread running).
- `LedIntentLimitsTests`.
- `LedDriveArbiterTests`, `LedDriveHoldoverTests`.
- `LedScriptLookaheadTests`.
- Shader (GPU, in `LibplaceboLutParserTests`): `LedCompensationShaderAppliesGainWithASoftKnee`, `LedCompensationKeepsBelowBlackAndLightExact`, `LedBoostToeAndShadowLift`. They run on WARP; `VP_TEST_GPU=hardware` uses the real GPU.
- Module tests use a fake port and `ManualClock` (LedDriveTestSupport.h); no projector needed.
- Run one class: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LedDriveCompensateTests" /Logger:"trx;LogFileName=x.trx"`. Tests run single-threaded; read the TRX.

## Settings

- Existing keys: the `[led_drive.default]` and `[projector.default]` keys listed in "Config keys actually read", plus `[general] file_led_holdover_ms`. The listed `led_compensation` is not a config key.
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: LED compensation: off (shader did not load). Not implemented in code (no `EngageFallback` call).
- Behaviour: Designed: failure -> drive held at full, no compensation. Actual: the drive keeps dimming the LED with no compensation; a log warning appears only when the gamma boost is on.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `drive.led_measure`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA` (the shader tests run on WARP by default; `VP_TEST_GPU=hardware` for the real GPU).

Placeholder tests:
- (none yet)

Isolation tests:
- LED compensation shader tests (`LibplaceboLutParserTests`: `LedCompensation*`, `LedBoostToeAndShadowLift`)

Contract tests:
- CNTR never above need (`LedDriveDelta1Tests`, e.g. `CntrPairPredictNeverLandsAboveTheNeed`; `LedDriveCompensateTests`)

Chain tests:
- LED + Stage 13 + LUT + black level (no dedicated test found)

## Known issues / open items

- Shader failure or the slot switched off leaves the LED dimming without compensation, with no "Fallbacks engaged" entry (see Fallbacks).
- `LedDriveArbiter.*` is projector-port ownership and `LedDriveHoldover.*` is host lifetime; both live here for now.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/drive/led_compensation/LedDriveModule.cpp`
- `src/VP-V/modules/drive/led_compensation/LedDriveArbiter.cpp`
- `src/VP-V/modules/drive/led_compensation/LedDriveModule.h`
- `src/VP-V/modules/drive/led_compensation/LedDriveArbiter.h`
- `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.cpp`
- `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.h`
- `shaders/LED compensation.glsl`

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/led/LedDriveArbiter.h`
- `src/VideoProcessor-Lib/led/LedDriveHoldover.h`
- `src/VideoProcessor-Lib/led/LedDriveModule.h`

`LedRawTransfer.*` stays at the legacy path for now (`VideoProcessor-Test.vcxproj`, changed by active branches, compiles its `.cpp`); `shaders/LED compensation.glsl` stays in the runtime shader folder.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `led_compensation`.
- Tiers: not tiered.
- VP-V page, Dynamic Drive tab: not tiered (Not a quality/cost choice: Dynamic Drive is set up on its own page (projector, script/compensate).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#7-dynamic-drive](../../../../../docs/settings/VP-settings-reference.md#7-dynamic-drive).
- Fallback entry ("Fallbacks engaged"): `LED compensation: off (shader did not load)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): LED compensation shader tests.
- Contract (formats, geometry, fallback entry): CNTR never above need.
- Chain (total delay at the 4/3/2-frame budgets): LED + Stage 13 + LUT + black level.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
