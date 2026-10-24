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
