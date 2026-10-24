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
