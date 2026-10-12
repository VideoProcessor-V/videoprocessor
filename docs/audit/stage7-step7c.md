# Audit — Stage 7 step 7c (rest of 7c-1, 7c-2 with the owner's correction, 7c-3)

Commits: 835aceae (notes), f6bb3c10 (answers), c539459c (7c-1 fixes), 728ebc24 (7c-2), 93678ba8 (owner
correction), 468c844f (recovery-rate answers), 9d469649 (7c-3). Final state reviewed at 9d469649.
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 9d469649, 0 errors, no warnings in LED or Config code.
Tests: native 2513 total, 2511 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). All LED tests pass, including
MaxCutDipSetsTheSceneFloor, CntrDumpIsFullOrJustEnough, CutRecoveryFollowsTheRampUnderTheLed,
ModuleBrightCutDumpsContrastAndFadesUp, ExitFromBlackPreliftsCappedThenJumpsOnTheCut,
ExitFromBlackPrepositionsContrastAndCapsTheLed, CntrHoldRaisesAtOnceUntilReleased,
RegressionRecipeUsesTheFixedRates. VideoProcessorConfigTests 102 passed, 2 failed (the two known RPC
tests). No Device Guard block. Same as your results.

Specs, in this order of precedence: recovery-rate answers, cut-correction answers (with the
implementer note), 7c-2 answers, 7c-1 answers, control policy, module guide §4.2 box.

## Blockers
- None.

## Should fix
1. (2) **The bright cut's CNTR can lose the race with the LPW1.**
   - In `StepCompensateLocked` the LED jump is queued first
     (`SetLightJump`) and the cut's CNTR a few lines later
     (`SetContrastAsync`).
   - The driver puts a waiting CNTR first only if both are waiting at once.
     If the driver's thread is idle, it can wake on the LPW1 and send it
     before the CNTR is queued. The CNTR then goes out a command gap
     (200 ms) later and lands after the cut frame, where the change is no
     longer hidden by the cut.
   - Fix: queue the CNTR before the LPW1 on a cut, or set both under one
     driver lock.
2. (10) **The pre-lift from black can brighten a fading picture.**
   - The controller pre-lifts the LED whenever the landing frame has APL < 20
     and a bright scene is ahead. That includes the end of a fade-out,
     where `stats.fade` is still true.
   - During a fade the module *holds* the gain (`heldGain`), so nothing
     compensates the LED's rise. A very dark but not black picture gets up
     to 2× brighter (LED 60 → 120) for the last ~167 ms of the fade.
   - The CNTR pre-positioning already waits for `!holdGain`; the LED
     pre-lift does not.
   - Fix: do not pre-lift while the fade's gain hold is on unless the frame
     is truly black, or end the hold when the pre-lift starts.

## For the planner
- (4) **A highlight appearing in a scene that is using CNTR above 59.**
  - Without highlights the assist may go up to 74. When a highlight
    appears in the same scene, the limit drops to `dmd_clip_cntr`, and
    since CNTR now goes down at once, it drops 74 → 59 in one command.
    That is ≈ 1.7× in light.
  - Content stays constant only if VP's gain steps up at exactly the
    modelled landing time, and the highlight is compressed beyond the
    tolerance until the LED floor climbs at the scene rate.
  - Policy §3.2 says no CNTR step happens within a scene.
  - Options: step down gradually, one value per command, paired with the
    LED's rise; or accept the white clip until the scene ends. The owner's
    call.

## Notes
- (3) A dark cut stops a running fade-up as soon as the cut enters the
  look-ahead window, up to `led_latency_ms` before the cut frame. If a
  fade-up is still running then, the last frames before the dark cut jump
  to full brightness. This only happens with a bright cut followed by a
  dark cut within about a second.
- (3) An unseen cut can only start its fade-up on the frame after it is
  detected. The cut frame itself still has the old scene's gain, with the
  knee containing it. This is inherent to the reactive case.
- (10) False alarm out of black (the bright scene leaves the window):
  - CNTR returns to 50 at once.
  - The LED, already at up to 120, comes down at the scene's dimming rate:
    about 24 s from 120 to 60 at the script rates. Black stays raised
    meanwhile.
  - The custom dim cannot start again until the LED is back at
    `led_floor_black`.
- (6) The `max_cut_dip` floor still assumes a full dump to 50. With the cut
  level at 59, the start after a cut is ≈ 1.5× higher, so the 102 floor is
  more cautious than it needs to be. That is as specified.
- (10) Head start: at the script rates the early ramp moves less than half a
  level in a 167 ms window, so the real head start is the seen cut's one
  LED command landing on the cut frame. That meets §3.5 in effect.
- (0) The 2% up-margin lets VP's gain exceed its headroom by up to 2%
  until CNTR steps up. The test was adjusted to allow that; it is the
  intended price of no chatter.
- The comment above the pairing still says "the lower of now and
  `cntr_latency_ms` ahead". The code now uses the modelled LED light now
  (policy 3.4, never ahead of the LED); only the comment is out of date.

## Verified OK
- (0) **Fixes:**
  - The pairing goes down at once to the smallest covering value, with the
    2% margin only on the way up. A need of 0.99 at 51 gives 50.
  - When a fade's gain hold or the custom dim begins, a waiting CNTR is
    dropped and the pairing is set back to the last value really sent.
  - While compensate waits for CNTR 50, the gain is inactive (no smoothing
    gain).
- (1) **Rates:**
  - `rate_*` defaults: up 0.25 / 2.5 / 17.5, down 0.15 / 2.5 / 12.5.
  - `rate_preset` sets both families, and puts `recovery_rate_*` back to
    4 / 20 / 400 in either preset. Explicit keys are read after the preset,
    so they override it.
  - `rate_gain` 0 means exactly the fixed `rate_up` / `rate_down` (400 / 120,
    unchanged).
  - `recovery_rate_*` is used only by `CutRecovery`'s shortfall ramp.
  - The cut's LED command is one jump, and CNTR after a cut follows the
    LED's modelled light.
- (2) **Bright cut:**
  - Once per cut (deduplicated by the cut's source sequence), CNTR goes to
    `DumpTarget` = cut level, raised only to keep `max_cut_dip`. It never
    goes above the current value and never to 50 because of the cut.
  - `cut_cntr_level` defaults to `dmd_clip_cntr` (59).
  - During the fade-up the pairing's ceiling stops CNTR from rising, and
    `recovery_bias_pct` keeps its descent on the bright side.
  - `cut_cntr_dump: off` holds CNTR.
  - A seen cut's fade-up starts on the cut frame itself; an unseen one
    starts at once (see Notes).
- (3) **Fade-up:**
  - The R ramp runs at `recovery_rate_*`; gain = min(ceiling, R / (LED ×
    CNTR light)), taking the lower of that and the normal gain.
  - The ceiling shrinks by Light(clip) / Light(CNTR) when CNTR is above the
    clip.
  - It starts from the CNTR just sent, so the limit trims until the dump
    lands. It ends only when R = 1 and 1 / light fits under the ceiling;
    the limit then equals the normal gain, so nothing jumps.
  - A fade's hold overrides it.
  - A new bright cut restarts it on that cut (hidden by the cut).
- (4) **dmd_clip_cntr:**
  - While VP's headroom is limited (highlights), CNTR is capped at 59.
  - The clip is not in the need, so there is no escalation: each step does
    not ask for the next.
  - The highlight floor uses CNTR's headroom only up to the clip.
  - Without highlights, the assist may go to `cntr_max_usable`.
  - The reasoning is right. For the one remaining step, see "For the
    planner".
- (5) **Dark cut:** no fade-up; CNTR pairs on the modelled LED light now, so
  it is never above the paired value.
- (6) **Scene floor:**
  - max(`led_floor` or `led_floor_reactive`, the floor from `max_cut_dip`).
  - At 0.5: 0.5 / 2^0.33 = 0.397 of ceiling flux, which is level 102.
  - `led_floor_black` keeps its own path.
- (7) **cntr_assist_max:** defaults to 100; the key stays (50–100), and the
  lower of it and `cntr_max_usable` applies.
- (8) **Regression recipe:** both helpers (`Compensate()` in the compensate
  and delta-1 tests) set `cntr_assist` off, `rate_gain` 0, `max_cut_dip` 0,
  `shaped_recovery` off and `black_prelift_max` 255, and `Port()` sets
  `led_settle_ms` 0. Under those settings every 7c path is inactive. The
  pre-7c-1 tests changed only these settings and the documented 2%
  allowance, so they still prove the previous build.
- (9) **Thread safety:** the fade-up, the pairing's ceiling / bias / hold, the
  cut sequence and the limit are all used under the module mutex (frame
  path and `StepCompensateLocked`). The getters lock too.
- (10) **Exit from black:**
  - One LED command up to `black_prelift_max` (120), never further while
    dark.
  - CNTR goes to the cut level in one command and is held (`SetHold`),
    including while the custom dim is leaving (not while it is deep).
  - The custom-dim exit jumps to the controller's capped level.
  - The cut's own command (seen cut) jumps to the scene's level, so black
    stays within 120 until then.
  - 255 = no pre-lift and no pre-positioning.
- (11) **Config page:**
  - "Contrast and cuts", "LED speeds" and "Projector response" give every
    7c key a control, in the right section (`led_drive` / `projector`), with
    the code's defaults.
  - "preset" (and "auto" for the cut level) removes the key; nothing is
    written until a value changes.
  - Every number control sets its accessible name, and the ranges agree
    with the validation lists.
