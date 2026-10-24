# Module findings 2026-10-24: fixes and renderer hand-off

The documentation pass on the nine early-migrated modules (commit "Module READMEs checked against
the code") found baseline loose ends. Each is fixed on `chore/early-migrations` as a separate
`fix:` commit after the behaviour-free migration commits, one finding per commit, in the files this
branch may touch. Some parts need files that other active branches are changing:
`LibplaceboVideoRenderer.cpp`/`.h`, `ConfigEditorWindow.cpp`, `VideoProcessorDlg.cpp`/`.h`,
`IRenderer.h`, `RendererProfileConfig.h`, `docs/design/slots.json` and the other files on their
change lists. For those parts this document gives **the exact change needed**, for the skeleton
lead (or whoever owns the file) to apply.

Status key: **done** (fixed on this branch, with a test where feasible), **hand-off** (exact
change below), **done + hand-off** (module side done, wiring below).

## 1. The LED drive must never dim without compensation (done + hand-off)

**Problem.** In mode `compensate`, the LED dims because the compensation hook brightens the
picture by the same amount. If the shader fails to load, libplacebo disables it, or
`pipeline_slots_off` turns `drive.led_compensation` off, `LedDriveModule` is never told. It keeps
dimming and the picture goes dark. slots.json says "Failure -> drive held at full"; nothing
implemented that.

**Module side (done).** `LedDriveModule` (src/VP-V/modules/drive/led_compensation):

- `SetCompensationAvailable(bool available, reason)`: while unavailable in mode `compensate`, the
  LED is held at `led_ceiling`. The hold is the intent floor at the ceiling, so it reaches every
  controller target, the cut jumps and the custom-dim fade. When the hold starts, the LED jumps
  straight to full drive instead of ramping.
- Mode `script` is not held: its dimming is paired with the projector's own CNTR, and it only
  loses `script_smoothing`.
- `CompensationUnavailableReason(slotOn, shaderLoaded, shaderEnabled)` builds the reason string.
- `FallbackEntries()` returns the module's entries. While the hold is on, that is
  `LED drive (held at full: picture compensation unavailable (<reason>))`.
- Default: available, so behaviour is unchanged until the renderer calls it.
- Tests (LedIntentLimitsTests):
  - `CompensationUnavailableHoldsFullDriveAndShowsTheFallback`
  - `CompensationLostWhileDimmedJumpsToFullDrive`
  - `CompensationHoldIsCompensateOnlyWithReasons`

**Slot rule.** The auto-hold covers `drive.led_compensation` being switched off alone: the
renderer reports the slot state as one of the three facts. No order rule is needed.
`OrderValidator` checks order, and the hold makes the switch safe.

**Renderer change needed (LibplaceboVideoRenderer.cpp):**

1. In the per-frame LED block, where the LED compensation hook is pushed (or skipped), after
   `BeginFrame*`, tell the module whether its compensation will run this frame:

   ```cpp
   // module findings 2026-10-24 item 1: compensate holds full drive while nothing compensates.
   const bool slotOn = /* the registry runs drive.led_compensation this frame */;
   const bool shaderLoaded = /* the "LED compensation" shader is in the hook chain */;
   const bool shaderEnabled = /* libplacebo has not disabled it (pl hook status) */;
   const std::string why = LedDrive::LedDriveModule::CompensationUnavailableReason(slotOn, shaderLoaded, shaderEnabled);
   ledDrive->SetCompensationAvailable(why.empty(), why);   // cheap; returns at once when unchanged
   ```

   This must also run when the hook is skipped (slot off, or shader missing), not only when it
   is pushed. A Dynamic Drive module that is adopted from the hold-over keeps the last state
   until the next frame sets it.

2. Where the renderer publishes its heading (`GetFallbacksStatus`, the `fallbacks` registry), merge
   the module's entries every time it publishes:

   ```cpp
   for (const char* id : { LedDrive::LedDriveModule::kHoldFallbackId, LedDrive::LedDriveModule::kDryRunFallbackId })
   {
       const auto entries = ledDrive ? ledDrive->FallbackEntries() : std::vector<FallbackStatus::Entry>();
       const auto it = std::find_if(entries.begin(), entries.end(), [&](const FallbackStatus::Entry& e) { return e.id == id; });
       if (it != entries.end()) EngageFallback(id, it->reason); else ReleaseFallback(id);
   }
   ```

   (`EngageFallback` exists; add a `ReleaseFallback` that calls `fallbacks.Release(id)`.) The GUI's
   one-time notice then comes for free.

## 5. Crash state: a lock failure must not create false crash states (done)

**Problem.** If `CrashState::Recorder::Start` could not create its instance lock, it carried on
as if it had. Another instance's `PreserveLeftover` then saw a live file with no held lock and
filed the running instance's state as a fake "crash state (N)". The per-instance lock (SH12)
exists to prevent exactly that.

**Fix.** `Start` retries the lock three times, 50 ms apart. If it still fails, it logs the error and
names the live files `crash-state-<id>.unlocked.txt` / `.dmp`, which `PreserveLeftover` never
treats as leftovers. `InstanceLocked()` reports the failure. The trade-off, documented in the
header: an unlocked instance's own crash is not kept as a numbered crash state.

The header comments are also corrected: the live name is `crash-state-<id>.txt`, and
`LinesSource` is also called on the caller's thread (`Text()`, `Stop`).

Test: `CrashStateTests::UnlockedInstanceIsNeverKeptAsAFalseCrash`.

**Optional GUI follow-up (VideoProcessorDlg.cpp, on dv-l8's change list):** show
`CrashState::Global().InstanceLocked() == false` as a state line, e.g.
`crash state: unlocked (not kept after a crash)`.

## 6. `diagnostic_allow_full_g22` had no effect (note for Bill; no change)

**Finding.** `MakePlan` never reads `Request::allowFullG22Experiment` and never sets
`Plan::strictContract`, so every strict-contract branch in `Finalize` was dead. History: the strict
Full/pure-2.2 experiment (e8d5d077) was superseded upstream by Bill in 9941b80c. A Full-range
gamma 2.2 display is now an ordinary display target (`TargetTransfer::GAMMA22` under the standard
Full/sRGB declaration), and the flag was left behind.

**Not changed: LibplaceboOutputPolicy is Bill's code** (owner principle: Bill's code stays where he
has it). A fix was drafted, removing the dead branches and adding a test, then withdrawn. This is a
note for Bill: the flag and `Plan::strictContract` can go, together with the key's retirement
below.

**Hand-off (retire the key):**

- `LibplaceboVideoRenderer.cpp`: stop reading `diagnostic_allow_full_g22` (settings struct, the
  `readBoolean` / `TryGetDisplayBool` sites, the settings hash) and stop setting
  `outputRequest.allowFullG22Experiment` / `requestedTransport.allowFullG22Experiment`. Drop
  `status.strictContract` and the `outputPlan.strictContract` log field.
- `RendererOutputContractStatus.h`: remove `strictContract`.
- `ConfigEditorWindow.cpp`: remove the "Full-range 2.2 transport experiment (Bill, diagnostic)"
  control and the key from its lists.
- `VideoProcessorDlg.cpp`: remove its `SetKnown(section, "diagnostic_allow_full_g22", ...)`.
- `RendererConfigView.h` and `ConfigurationApplyPolicy.h`: move the key to the retired-keys list,
  so old files load silently and Config drops it on save.
- Then remove `allowFullG22Experiment` and `strictContract` from LibplaceboOutputPolicy.h (Bill).

## 7. Smaller items

- **`.ccss` letter case (done).** `CalibrationController::CreateMeter` matched only ".ccss" and
  ".CCSS", so ".Ccss" got past the "takes only .ccmx" refusal. Now `IsSpectralSampleFile`, which
  ignores case. Test: `SpectralSampleFileIsRecognisedInAnyCase`.
- **Look-ahead measuring with `lookahead: off` (done).** `LedDriveModule::WantsPredictions`
  returned true for compensate even with `lookahead: off`, so the renderer's queued-frame walk
  measured every new raw frame (about 2–3 ms per 4K frame) for predictions nothing read. It now
  returns `LookaheadWantedLocked()`. The renderer already re-reads `WantsPredictions` on every
  `Reconfigure`. Test: `CompensateWithoutLookaheadWantsNoPredictions`.
- **Subtitle temporal-fill margins (note for Bill; no change).** `SubtitleTemporalFill::Matches` (Bill's
  code) searches ±192 / ±120 px around the cleanup box with a 4 px guard, whatever the resolution
  (ADR-0018). A fix (fractions of the picture, identical at 1920x1080) was drafted and withdrawn
  under the owner principle that Bill's code stays as he has it. Suggested to Bill:
  `(192*width+960)/1920`, `(120*height+540)/1080`, `max(1,(4*height+540)/1080)`. The 120 / 32
  thresholds are code differences and need no change. The renderer's default
  `subtitleNearBarDistance = 20` source pixels has the same issue.
- **Misleading comment in `CalibrationLutStage.h` (done).** It said `OutputHookOrder` ranks the hook
  after the user shaders and before the de-judder cache. That position actually comes from the
  render path's push order, and `pipeline_order` can move this slot ahead of LED compensation. The
  comment now says so. Whether `output.calibration_lut` should be movable at all is an owner
  question; the slot rule lives in `slots.json` / `SlotContracts.h`, both on other branches' change
  lists.
- **Misleading comment in `RendererProfileConfig.h` (hand-off; dv-l8 changes that file).** Above
  `MIN_SUBTITLE_HOLD_SECONDS`, "sampled every third rendered frame" is wrong. Replace it with:
  "subtitle detection inspects every new source frame on the measurement worker
  (`SubtitleBoxDetector::Detect`)".
- **LED measurements stamped at collection time (deferred; needs a build to validate).**
  `LedDriveModule::SubmitMeasurement` builds `FrameStats` with `NowMs()`, so frames collected
  together share a time, and under render-ahead it is not the display time. The fix is to stamp
  with the frame clock time `BeginFrame*` recorded for that sequence: a small bounded map
  sequence -> frame time, falling back to `lastFrameMs` for sequence 0. But it changes the time base
  of every persistence and fade window, and the LED tests feed synthetic frame times. It needs the
  LED test classes run while it is being written, so it is left for a session with builds.
- **`CalibrationController::Start` blocks the RPC thread (deferred).** `FindSpotread` (drive and
  profile scans) runs synchronously on the RPC thread, and `worker` / `currentVpRoot` are written
  outside `mutex`. The fix is to move discovery and session construction onto the worker, report
  "starting" immediately, and guard `worker` / `currentVpRoot` with `mutex`. That restructures the
  start path and its error reporting (the RPC currently returns the start error synchronously),
  so it needs Config-side agreement and the calibration tests. Not done blind.
- **Calibration config path (hand-off; VideoProcessorDlg.cpp, dv-l8).** In
  `HandleCalibrationRpc`, set `hooks.configPath = [this] { return <the path VP loaded its config
  from>; };` so the controller never falls back to `ConfigFile::DEFAULT_FILENAME` relative to the
  working directory.
- **`MeasurementHook::Dropped()` is never read (hand-off; LibplaceboVideoRenderer.cpp).** In the LED
  block after `Collect`, when `Dropped()` grows, log it once per change. If it keeps growing
  (readbacks lost every frame), engage `LED measurement` / "readbacks dropped: the LED follows the
  last statistics".

## 2. Every listed fallback visible (partly done, rest hand-off)

**Done in module code:** the LED dry run. `LedDriveModule::Run`, which used to log only "continuing
as a dry run", now engages `projector (not connected: <error>; LED drive off (dry run))` in the
module's `FallbackEntries()`. It was committed with item 1, which adds the module's fallback list,
and the renderer merges it as in item 1.

**Hand-off (all in LibplaceboVideoRenderer.cpp, which owns the registry).** Each line below is a
log-only downgrade today. Add the `EngageFallback` call shown next to the existing log line, and
`ReleaseFallback` where it recovers:

| Site | Engage |
|---|---|
| `RejectDisplayLutAfterRenderFailure` | `EngageFallback("calibration LUT", "dropped after a render error: <error>; no projector LUT for this renderer")` |
| `PresentCalibrationPattern`, windowed patch falls back to full field | `EngageFallback("calibration patch", "full field (window unavailable: <why>); LCD/OLED readings are affected")`, and report `windowed = false` in the pattern result so `CalibrationSession` records it per patch |
| `EnsureAutoLimitedFallback` | `EngageFallback("output range", "AUTO Limited unavailable: composed Full/sRGB swapchain (<reason>)")` |
| Initialise: rejected or unsupported output request renders on Full/sRGB | `EngageFallback("output request", "<requested> rejected: Full/sRGB (<Actual.reason>)")` |
| Re-base failure pause ("Re-base failed ...", 5 s doubling to 60 s) | `EngageFallback("re-base", "paused after a failure: native frame for <n> s (<error>)")`; release when a pass succeeds |
| NLS hook failure (`SetShaderStatus("NLS: Safe fit (hook failure)")` in the `RenderOwned` callback) | `EngageFallback("geometry", "NLS safe fit (hook failure)")` |
| Subtitle cue lost: `Consume` reset with "preview-unavailable"; jobs dropped at `SubtitleMeasurementWorker::Submit` capacity; exceptions swallowed in `SubtitleMeasurementWorker::Run` | `EngageFallback("subtitle detection", "worker late: cue dropped")` (or "jobs dropped" / "worker error"), released at the next good cue; the counters already exist |

**Listed entries that can never occur.** Remove them from slots.json `fallback.entry` and the
manifests' `fallback_entry`, or keep them marked "planned":

- `Tone map: libplacebo (combined LUT missing)`: there is no combined LUT yet; mark planned.
- `Source analysis: labels unknown (<reason>)`: labels are not implemented; mark planned.
- `Re-base: off (<reason>)`: replace with the real `re-base precision` entry, plus the pause entry
  above.
- `Calibration LUT: libplacebo target LUT (VP stage unavailable)`: the code's entry is
  `calibration LUT (the VP stage failed; libplacebo's target LUT (before the LED compensation))`.
  Use that text.
- `LED: drive held (measurement unavailable)` (`drive.led_measure`): no code produces it. Either add
  it with the `Dropped()` hand-off (item 7), or remove it.
- `LED compensation: off (shader did not load)` (`drive.led_compensation`): superseded by item 1's
  `LED drive (held at full: picture compensation unavailable (...))`. Use that.
- `Projector: not connected (LED drive off)` (`drive.projector_control`): now produced as
  `projector (not connected: ...)`. Align the text.

slots.json is on dv-l8's change list and in the skeleton worktree, so these edits are handed off.
After editing slots.json, run `python tools/check_module_manifests.py --write` and
`python -I tools/vpv_tests/seed_module_readmes.py`.

## 3. VP LUT stage status (hand-off)

LibplaceboVideoRenderer.cpp, render path:

- `targetLutApplied` / `calibrationLutAttached` is computed from `target.lut != nullptr`. With
  `calibration_lut_stage = vp`, `target.lut` is null on purpose, so the LUT reads as not attached
  in `ActiveProfileStatus`, the Ctrl+I limited-range text, the "(inactive: calibration LUT)" marker,
  and Config's `liveCalibrationLutAttached_` / `usableLut`.
  Change: `calibrationLutAttached = target.lut != nullptr || <the VP calibration LUT hook was
  pushed this frame>`. The render path already knows when it pushes `calibrationLutStage`; set a
  local flag there.
- `ConfigureDisplayLutForTarget` writes `displayLutStatus = "Active (VP stage): ..."` every frame,
  which overwrites `Active (reload rejected: ...)`. Change: keep the libplacebo branch's guard, and
  only write the VP-stage text when the status does not already carry `reload rejected`.

## 4. slots.json and manifest key lists (hand-off; slots.json is on dv-l8's list)

The drift check requires each manifest's `config_keys` to equal slots.json `settings_keys` +
`planned_keys`, so both change together in one commit. Proposed lists (the "Config keys actually
read" sections of the module READMEs on this branch hold the full detail):

| Slot | Remove | Add | Move |
|---|---|---|---|
| `drive.projector_control` | `led_compensation` (only a shader rule name) | `projector` (the `[projector.default]` group) | |
| `drive.led_measure` | `led_compensation`, `led_protect` (a value of `intent_black`) | `lookahead`, `led_lookahead_ms`, `highlight_percentile`, `highlight_persist_ms`, `apl_persist_ms`, `cut_threshold`, `fade_ms`, `led_peak_full_coverage`, `led_picture_only`, `led_highlight_grain_suppress` | |
| `drive.led_compensation` | `led_compensation` | `led_drive` (the `[led_drive.default]` group), `adaptive_gamma`, `gamma_boost`, `shadow_lift_ansi`, `intent_black` | |
| `output.calibration_lut` | `calibration_lut` (no such key), `calibration_lut_input_gamma` (retired) | `calibration_lut_bt709`, `calibration_lut_p3_d65`, `calibration_lut_bt2020` | `calibration_lut_stage`: planned -> settings (implemented) |
| `scaling.geometry` | `nls_active`, `nls_curve`, `nls_geometry`, `nls_protect`, `nls_scale` | `nls_sampling`, `screen_aspect` (`nls` stays: the selection group) | |
| `colour.tone_gamut` | `tone_mapping_tuning` (a log name) | `output_presentation`, `output_range`, `output_gamma`, `output_transport_gamma`, `sdr_target_primaries`, `report_bt2020_to_display`, `sdr_adjust_gamma`, `sdr_input_transfer`, `hdr_tone_map_target_gamma` | |
| `control.diagnostics` | | `state_report`, `auto_report`, `report_thumbnail` | |
| `input.source_analysis` | `subtitle_type` (the real key is `type` in `[vprenderer.subtitles.*]`), `subtitle_reposition` (parsed and ignored) | `subtitle_near_bar_px`, `subtitle_hold_ms`, `subtitle_script_hint` | |
| `scaling.rebase` | | | `entrance_rebase` stays planned (not read) |

**`subtitle_detection_optimization` (hand-off; decision: remove).** It is parsed from legacy rule
and display sections, then reset to the default by the renderer's "Discard legacy subtitle
settings" block. So `SetOptimizationMode` always gets 0, while Config still accepts the key.
Making it work would revive a setting upstream deliberately discarded, so remove it:
- delete its validation in `RendererProfileConfig.h` (`ValidateProfileSetting`);
- move it to the retired keys (`RendererConfigView.h` / `ConfigurationApplyPolicy.h`) so old files
  load silently;
- drop the dead parse in LibplaceboVideoRenderer.cpp.
`SubtitleDetectionOptimization.h` can then go.

## 8. README settings-guide link (done)

`tools/vpv_tests/seed_module_readmes.py` generated `../../../../docs/settings/...` from
`src/VP-V/modules/<unit>/<slot>/`, which resolves under `src/`. It now uses `../../../../../`, and
the 27 affected generated blocks are regenerated. `run_chain_gate.ps1` runs `seed --check`. After
merging a branch that changes slots.json, re-run the seed script.

## 9. LED module boundaries (done, in the M2 plan)

See "LED module boundaries" in `migration-m2-plan.md`: `projector_control` becomes transport
only, `led_compensation` becomes control (with `LedController`), and `led_measure` becomes
measurement, with `FrameStats` split out and an `IPredictionSink` interface.

---

# Part 2: blocked modules (findings only, not fixed)

This is a read-only review of the modules early migration could not move. Other branches own these
files, so nothing here is fixed on this branch; the M2 plan's mandatory per-move step picks them
up. Line numbers are at `240d582b`. **LVR** = `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`,
**Cfg** = `src/VideoProcessor-Config/ConfigEditorWindow.cpp`, **Dlg** =
`src/VideoProcessor-GUI/VideoProcessorDlg.cpp`.

The renderer's only `EngageFallback` ids today are: entrance once, re-base precision, the three
GPU unpacks, the Dolby Vision ones, calibration LUT, pipeline settings and order, render-ahead,
delay budget and look-ahead. Every other documented "Fallbacks engaged" entry is unimplemented.

## High

1. **Sigma-delta dropped with only a log line** (`output.dither`; ADR-0001). LVR:24405-24431 erases
   the hook for an unsupported code domain. CustomHookChain.cpp:231/:258 (shader not loaded) and
   :353 (disabled by libplacebo) log only.
   - Scenario: on the 8-bit FL35 link the picture quietly switches to libplacebo's dither; Ctrl+I
     says "none".
   - Fix: `EngageFallback("dither", ...)` at each site; release on recovery.
2. **BTB/WTW clipped, log-only** (`colour.extended_range`; ADR-0002 + ADR-0001). The hook failure is
   latched until restart (LVR:24563, 24910-24914). `ForTemporal` turns it off with a LUT or gamut
   mapping (LVR:24567). Plain smooth motion clips BTB on mixed refreshes, logged once
   (LVR:24585-24592).
   - Scenario: below-black is lost on screen with no visible entry.
   - Fix: `EngageFallback("BTB/WTW", Describe(extended))` for HookFailed / TemporalMix /
     `mixClipsBtb`.
3. **Turning the `scaling.rebase` slot off still sets every downstream stage up for a re-based
   picture.** LVR:23150-23151 computes `rebaseFactor` / `rebaseFactorX` from `source_resolution`
   alone, without `pipelineSlots.Runs(ScalingRebase)`. The factors feed the debander
   field/radius scales (23218-23229), detail pulse and texture restore (23272-23276), the adaptive
   upscale decision (24207) and the status line.
   - Scenario: a 4K-from-1080p source with `pipeline_slots_off: scaling.rebase` gets vector and
     trust lookups at twice the position, so detail pulse measures at the wrong grain scale.
   - Fix: force both factors to 1 when the slot does not run.
4. **A VP debander failure, or its slot switched off, leaves no deband at all.**
   `LibplaceboDebandYieldsToVp` (LVR:2244-2247, used at 9623) turns libplacebo's deband off whenever
   `deband_vp` is not off, and only temporal frames turn it back on (24677). A debander failure is
   logged once (6252-6254), and `pipeline_slots_off: repair.deband` has the same hole.
   - Scenario: banding returns with nothing shown.
   - Fix: re-enable libplacebo's deband when VP's fails or is off, and engage "Deband: libplacebo
     deband (...)".
5. **NVIDIA optical-flow failure, matcher fallback log-only** (`motion.estimation`). The sticky
   `opticalFlowFailed` (DejudderWarp.cpp:3353-3356) is only logged when the source string changes
   (LVR:16853-16857). The look-ahead producer stops itself after three failures
   (MotionAheadProducer.cpp:290-297), or fails to start, and both are log-only (LVR:23009-23023).
   - Fix: `EngageFallback("motion", "matcher only: ...")`.
6. **Open-loop presentation silent; de-judder held with no display rate silent**
   (`output.presentation`).
   - Without DXGI statistics, render-ahead goes open loop with only a log line and
     DiagnosticHistory "ra-open-loop" (LVR:6895-6910, 17282-17291).
   - With render_ahead off, the de-judder holds every frame ("timing not known: display rate",
     LVR:17112-17119), which is plain judder.
   - Scenario: window or iGPU panel; Ctrl+I says "none".
   - Fix: engage "presentation" for both.
7. **Failed refresh-rate switch only logged.** LVR:4881-4884, 4917-4921, 4927-4933 and the
   per-candidate failures return with DebugLog only.
   - Scenario: 23.976p on a display with no matching mode stays at 60 Hz, giving 3:2 judder with
     nothing shown.
   - Fix: `EngageFallback("refresh rate", "kept <hz> for <input> Hz: <why>")`.
8. **SDR tone shape shader failing to load is log-only.** LVR:23860 (state 3) logs only, though
   the manifest promises "SDR tone shape: not applied (shader did not load)".
   - Scenario: harsh SDR highlight clipping with "none" shown.
   - Fix: copy the DV-trims pattern (LVR:23946-23953).

## Medium-High

9. **Hooks that libplacebo disables at runtime are never detected for SDR tone shape, DV trims
   or OutputEncodingRepair.** The `pl_renderer_get_errors` scan (LVR:24903-24930) checks custom hooks,
   LED compensation, extended range, calibration LUT and LED measurement only.
   - Scenario: the GPU rejects a shader; the hook is dropped every frame while status says active.
   - On stable libplacebo 7.360, a failed OutputEncodingRepair (OutputEncodingRepair.cpp:65-95)
     silently loses output levels. Beta 377 is not affected.
10. **Adaptive downscale failures fall back silently to plain SSIM (w = 1).**
    AdaptiveDownscale.cpp:592-866 returns the input unchanged on every GPU failure; `Failures()` is
    never read; the status line (LVR:13135) says "idle (not downscaling)". SSimDownscaler missing
    (LVR:24375-24379) is log plus status only.
    - Scenario: grain sharpened into noise and edge haloes, with a misleading status.
11. **The short-queue (look-ahead) fallback shows only under a delay budget.** "look-ahead" engages
    only inside the active hold (LVR:26786-26790). With the shipped frame-count target, a target
    below the motion look-ahead is only logged (Dlg:14969-14980). Config's "Balanced" preset
    (Cfg:6236, target 2) at 24p needs 3. Also, `QueueLatency::ClampToCapacity` (Dlg:14956) is
    called without its `clamped` output, so a target above queue_size is cut silently.
12. **Render-ahead's budget check uses the wrong queue count under a delay budget.**
    `UpdateRenderAheadBudget` (LVR:7902-7904) passes `earlyUploadQueueTarget` (the frame-count target,
    6 at 250 ms/24p) instead of the hold's `targetFrames` (3). `FitDelayBudget` passes, and
    `CombineQueueTarget` (AlphaQueuePolicy.h:123-128) then steers to 3 - 1 = 2, below the promised
    minimum.
    - Scenario: repeats or drops at 24p.
    - Also, the budget counts 2 swapchain frames while render-ahead runs latency 3
      (RenderAheadPolicy.h:31), so the total is one refresh short.
13. **The recommended 250 ms gives only 3 frames at 23.976, 24 and 25p.** Fixed costs at 60 Hz are
    99.7 ms, so floor(150.3 / 41.7) = 3, and "look-ahead (3 of 4)" engages at the owner's main rate.
    The 4-frame value is 267 ms (DelayBudgetLadder.h, slots.json), yet the docs
    (VP-settings-reference.md:86, :460), the Cfg:6902 tooltip and the queue_timing README say 250.

## Medium

14. **Shipped config says the link is 10-bit; the FL35 link is 8-bit.** VideoProcessor.cfg:142
    `display_bit_depth: 10`, and the Config seed writes 10 (Cfg:2526). The docs and renderer default
    to auto (LVR:1372, :3734). `ApplyDisplayBitDepth` (LibplaceboRenderParameters.cpp:201-219) then
    quantises the sigma-delta at 10 bits and the driver truncates to 8.
    - Scenario: banding the sigma-delta exists to prevent.
15. **The shipped defaults never run the sigma-delta or the hybrid scaler.** VideoProcessor.cfg sets
    no `temporal_dither`, so auto means libplacebo's dither, although its comment at :160-163 says
    banding is "the sigma-delta's job". The Low/Mid Scaling tiers (VpvTierTable.generated.h:201-211)
    set `adaptive_downscale on` but don't add SSimDownscaler, so they do nothing until the user picks
    the shader. `temporal_dither` auto-loads its shader (LVR:13009); `adaptive_downscale` does not.
16. **`render_ahead on` is a no-op with defaults, and Config says otherwise.** `FitDelayBudget`
    refuses any lead when `render_ahead_queue_credit` is off (RenderAheadPolicy.h:124-127), and it
    defaults off (LVR:1336, Cfg:10878). The Cfg:8856 / :8885 tooltips promise reported latency
    instead. VP-settings-reference.md:494 calls the credit superseded.
17. **Config's delay note uses the old ceil rule.** Cfg:471-491 (`FramesForLatency`) shows "24p 6
    frames (250 ms)"; the renderer queues 3 plus padding inside a total. The Cfg:6905 tooltip and the
    docs caveat (VP-settings-reference.md:96-97) say the budget is not wired, but it is (Dlg:14967,
    LVR:26694).
    - Scenario: wrong AVR lip-sync numbers.
18. **ADR-0020's single total delay is off in the shipped config.** VideoProcessor.cfg `[queue.Normal]`
    (lines 67-72) has `target_frames: 5` and no `target_latency_ms`: 208 ms at 24p, 83 ms at 60p. Its
    comment about 4 frames for motion is stale (MotionAhead.h:29-46). The Config "Reliable" preset
    (Cfg:6237, 11474) does the same.
19. **Refresh-mode ranking hard-codes a rate band** (ADR-0018). DisplayRefreshRatePolicy.cpp:163 tries
    the doubled rate only for 24.1 < input < 31.
    - Scenario: 23.976p with a 48 Hz mode but no 24 Hz is never tried.
20. **Chroma siting wrong on the CPU RGB->P010 path.** LVR:19734-19740 tags every CPU 4:2:0 picture
    TOP_LEFT. CDeckLinkRGBToP010VideoFrameFormatter.cpp:313-315 (and its AVX2 path) and
    CARGBtoP010VideoFrameFormatter.cpp:202-204 average 2x2, which is centre-sited.
    - Scenario: r210/R12 with `native_rgb_ingest=off`, or R12 at a width not divisible by 8, gives
      colour fringing.
21. **Debander vectors used across a hiccup-repair gap.** No `OnePeriodField` gate on
    `vpDebandParams.field` / `grainError` (LVR:23222-23226); F10 added it only to texture restore
    (23310). The hold reads `lp - f.xy` k+1 periods too far (VpDebander.cpp:91-97).
22. **Texture restore carries its reference with no motion when there are no vectors.** With
    `useField` false and a consecutive sequence, the REF pass reads `ref_old` at the same tile
    (TextureRestore.cpp:339-346, 776).
    - Scenario: on a slow pan at 60p with the de-judder idle, a neighbour's peak lifts texture by up
      to 1.5x.
    - Fix: restart, or skip the carry, without a field.
23. **"re-base precision" never clears.** `formatNote` (SourceRebase.cpp:242) is never reset, so the
    entry stays all session (LVR:6182-6190).
24. **A failed re-base is log-only, and on extras it is not even logged.** Main path: log once, then
    a 5-60 s pause (LVR:6193-6194, 23180). Extra path (6202-6211): the native frame silently.
25. **Detail pulse and texture restore failures reach only the Ctrl+I description text**
    (LVR:6356-6383, 13157-13160), not "Fallbacks engaged".
26. **Silent fallbacks in capture:**
    - Early upload unavailable is log-only (LVR:5480-5484), and the init log still claims "uploaded
      early" (12042-12044).
    - R12B/R12L at widths not divisible by 8 skip native ingest silently (30404-30410).
27. **Capture fallback text says 4:2:0, but the CPU path is lossless 4:2:2** (LVR:6618 vs
    `CreateAlphaFormatter` 900-918). The README item 44 treats the better behaviour as a defect.
28. **Defaults disagree:**
    - `dv_tunnel_gpu_unpack`: renderer and Config on (LVR:1097, Cfg:10870), but docs :117,
      vpv-tiers.json:67 and the tier table say off. So VP-V shows "default: off" while the renderer
      runs it.
    - `dv_trims`: on in code, off in docs :128.
    - `queue_size`: docs "auto", schema integer, default 32.
    - `lead_frames`: docs "auto", default 1.

## Low

29. Log-only 60 Hz assumptions: render-stall threshold (LVR:31795-31797, `DEFAULT_FRAME_RATE_HZ`),
    PulldownDetector.h:333, the adaptive-downscale frame period (LVR:13565-13570, a literal 24 Hz).
    The last makes smoothing about 2.5x slow at 24p on 60 Hz.
30. Automatic look-ahead and the GUI short-target log use the frame-count target, not the hold
    (Dlg:15004-15008, 14972).
31. `source_range` full/limited ignored on the CPU RGB path, logged once (LVR:19560-19570).
    SDR tone shape off under a calibration LUT is log-only (23833), unlike DV trims.
32. One undithered frame when libplacebo disables the sigma-delta: `quantiserInFrame` is set before
    the render (LVR:24766-24771), and the failure is marked after it (24905).
33. With `downscale_light gamma`, an anamorphic frame that shrinks x but grows y upscales in gamma
    against `upscale_light linear` (DownscaleLight.h; LVR:24694-24700).
34. An invalid `source_resolution` is dropped with no log (LVR:2483-2488).
35. Misleading comments:
    - LVR:5428-5436 says unpack failures are "logged once"; they are now engaged.
    - LVR:23268-23271 says the detail check is "exactly the debander's" (it omits the live-field
      clause).
    - shaders/Temporal sigma-delta.glsl:855-856 says "2^(n-8) - 1" reserved codes (it is 2^(n-8)).
    - OutputHookOrder.h says the renderer uses `Order(tagged)` (it uses `OrderSlotsInPlace`).
    - HDRData.cpp:9 has `#pragma once` in a .cpp.
    - TranslatePrimaries maps P3_D60 to Display P3, a D65 white point (LVR:778).
36. Key-list and README errors:
    - dither lists `sigma_delta` (a value), not `temporal_dither` / `occlusion_mask`.
    - scaling.output lists `hybrid_refine_d` (unparsed).
    - sdr_tone_shape omits `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`,
      `sdr_contrast_recovery`.
    - input.signal lists `dv_trims` (it belongs to sdr_tone_shape) and omits `dv_tunnel_gpu_unpack`.
    - queue_timing lists `low_latency` (a profile) and `queue_recovery` (a legacy section).
    - motion lists `dejudder_est`, `dejudder_hold` (log labels) and `motion_ahead_ms` (unparsed).
    - `motion_engine`, `smooth_motion` and `frame_mixer` are missing from the settings reference.
    - capture/signal docs call the unpack fallbacks "log-only", but they are engaged.
    - slots.json says "Lanczos fallback" and "polar retired" for scaling.output. Without SSIM the
      quality-preset downscaler runs, and `adaptive_faithful auto` still uses polar
      (AdaptiveDownscale.cpp:631-689).
    - The re-base description ("1-D ... r16") is wrong: luma uses polar EWA, with float fallbacks.
    - The open items "forced 5-frame minimum", "time-based motion_ahead" and "render-ahead inside
      the budget" are already done.

**Checked and found consistent:**
- the sigma-delta code limits against the shader;
- the quantiser kept last among OUTPUT hooks;
- the BTB/WTW restore under gamma-light downscale;
- SSIM seeing the clipped picture with the remainder carried;
- per-axis vector mapping in the debander, texture restore, detail pulse, warp and interpolator;
- R210 limited mapping keeping BTB/WTW;
- V210/UYVY GPU unpack bit-exact with the P210 contract;
- the DV hand-off window derived from the measured rate;
- motion/deband/texture defaults across Config, renderer, cfg and docs;
- no races in the queue-hold paths.

---

# Part 3: efficiency of the migrated modules (findings only, no code changed)

This was a reading-only pass: nothing was built or measured. Savings are estimates from loop bounds,
pixel counts x taps and file sizes x frequency; each module README's "Cost" section says how each
was estimated. Ranked by estimated saving. "Bill" marks code that stays Bill's (notes for Bill, or a
VP-V adapter).

| # | Module | Finding | Est. saving (how) | Fix | Byte-identical | Risk |
|---|---|---|---|---|---|---|
| 1 | `drive.led_measure` | `LedMeasurementHook::Collect` reads the slot submitted that same frame. On D3D11, `pl_buf_poll` always says "ready" and `pl_buf_read` is CopyResource plus a blocking Map, so the render thread waits for the whole GPU frame. The ring of 4 never fills and `Dropped` never counts | Up to one GPU frame time of render-thread wait per output frame (call order, libplacebo 7.360.1 d3d11 source). Confirm with QPC around `Collect` | A VP-owned staging buffer plus an event query, mapped with DO_NOT_WAIT 1-3 frames later; or collect before the next submit | Picture: yes in log_only/script; compensate stats arrive 1-3 frames later | Medium (check Beta 377 too) |
| 2 | `scaling.rebase` (VP-side, legacy `SourceResolutionMonitor`) | `Analyse` holds the monitor mutex through two `KernelInverse::FitTile` calls (about 24 ms each at 2160 lines); the render thread takes that mutex 3-5 times per frame | Render stalls up to about 50 ms, twice a second, while a 2:1 source is gathered or declared (24 M MACs per tile at 1 ns per tap) | Fit outside the lock; gate the fit on `rebase_kernel_match` and `source_resolution` (about 100 ms CPU per second when unused); several accumulators (3-4x) | Yes | Low |
| 3 | `drive.led_measure` | The full-coverage peak is computed in compensate and picture_only, where nothing reads `FrameStats::maximum` | 2-3 ms CPU per new 4K frame (`LookaheadPeakCostAt4K`), i.e. 48-72 ms/s at 24p, plus a GPU peak pass of 0.05-0.35 ms per frame | Enable `peakFullCoverage` only in modes that read it | Yes | Low |
| 4 | `output.calibration_lut` (calibration run) | Patch wait: `GetCalibrationOutput` try-locks `renderMutex` 100 x `Sleep(2)`, while the render thread holds it through the vsync-paced present | 0.2-1.6 s wall time per patch (100 sleeps x timer resolution), so minutes per run | Read the generation atomics only, or use the cache while patterns are active | Yes | Low |
| 5 | `input.source_analysis` (Bill) | The near-bar retry re-reads every pixel; `Sample()` re-validates with 64-bit divisions on every call; `Measure` scans the same planes twice on one path; caption probes build a fresh detector (4.7 MB zero-fill) on the render thread | Worker 1-4 ms per no-subtitle frame; 1-3 ms per pass for non-planar formats; one whole `Analyze` (2-6 ms) on that path; 0.5-1.5 ms render-thread spikes | Use the existing `shared_samples` mode (tested identical); unchecked sampler after one validation; reuse the probe result; keep a member detector | Yes | Low (notes for Bill) |
| 6 | `drive.led_compensation` | The identity compensation pass runs (log_only, dry run, gain 1 with no boost or lift) | About 66 MB per frame at 2560x1600: 0.07 ms on the 3090, 0.35 ms on the 3050 Ti, double at 4K | Don't push the hook when it is the identity | Yes while measurement runs | Low |
| 7 | `scaling.rebase` (GPU, legacy `SourceRebase`) | Kernel-inverse passes fetch about 311 M texels per frame (4K->1080, 50 taps) | 0.3-0.5 ms per frame on the 3090, 2-3 ms on the 3050 Ti (about 10x fewer fetches with shared-memory tiling) | Shared-memory tiling | Yes in principle (chart test) | Medium |
| 8 | `output.calibration_lut` | The VP stage adds a full-frame pass; libplacebo folds the LUT into its colour pass | 0.07-0.14 ms on the 3090, 0.34-0.7 ms on the 3050 Ti (bytes per frame / bandwidth) | Fold the clamp, LUT and restore into the compensation hook when the slots are adjacent | No (one fp16 rounding fewer) | Medium |
| 9 | `output.calibration_lut` | LUT load on the render thread: one `istringstream` per line for directive checks | 20-50 ms (33^3) to 0.15-0.4 s (65^3) per load or reload (lines x stream cost) | One hand-written pass; skip the re-read when the file is unchanged; optionally parse on a worker | Yes | Low |
| 10 | `drive.led_measure` | Slot off still runs the look-ahead; raw grid through `Sample()`; grain path recomputes block means 9 times; per-frame rebuild of the peak accumulator and tables | 2-3 ms per new 4K frame (slot off); 0.25-0.5 ms; 0.03-0.15 ms GPU; 20-60 µs | No predictions with the slot off; row readers; block means once; cache by geometry | Yes | Low / low-medium |
| 11 | `control.diagnostics` | Crash state rewritten every second: two opens and two forced flushes, 2-60 KB, 86,400 times a day | 0.2-5 GB/day of writes plus antivirus scans (size x frequency) | One handle; skip unchanged writes (keep the power-loss flush) | Yes / no (the timestamp header) | Low |
| 12 | `colour.tone_gamut` (Bill) | The rendered-output capture (screenshot or report thumbnail) analyses and hashes 16 MB on the render thread | 30-80 ms per capture at 2560x1600 | Move the stats, FNV, SHA and JSON to the capture worker | Yes | Low |
| 13 | `scaling.geometry` (Bill), `colour.tone_gamut` (Bill), LED, calibration | Per-frame strings: parameter maps, string-stream signatures, status labels, compensate log lines built and discarded | 3-40 µs per frame each, about 1-2.5 ms CPU per second for NLS | Cache per generation; compare small keys; format only when logged | Yes | Low |

**Cost when off:**
- LED disabled: zero (log_only and picture_only still run both GPU passes, the stalling readback and
  the identity pass).
- Calibration LUT absent: a few ns per frame.
- Subtitle detection off (`type` off/classic): zero detection. It runs only for `black` and
  `generated_gray`.
- NLS off: under 1 µs.
- The output policy and the crash-state writer have no off; they cost µs per frame and the
  per-second write.

Shared with the separate pipeline-wide audit: `SourceResolutionMonitor` and `AdaptiveDownscale` use
the same blocking poll/read pattern as item 1.
