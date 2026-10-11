# Audit — Stage 7 step 7a (LED sign fix, delta 4), step 7b (script mode real CNTR), merge-rc3 fix

Commits: 710ce9d2, ad5b738c, 23734e0c (each via `git show`).
The other Claude session was busy at first; I waited until it was idle. The checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 23734e0c, 0 errors, no warnings in changed files.
Tests: native 2495 total, 2493 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). All LED tests pass, including
LedCompensationKeepsBelowBlackAndLightExact, GammaBoostIsIndependentOfTheProjectorExponent, the
three ScriptSmoother tests, ScriptModeSendsTheScriptsLightAndContrast, ScriptSmoothingBridgesTheRealContrastSteps,
ScriptSendsIgnoreTheCommandGapAndStopAtShutdown, ScriptStartAndExitSetContrastTo50.
VideoProcessorConfigTests 102 passed, 2 failed (the two known RPC tests). No Device Guard block.
Same as your results.

Specs: VP-stage7-step7-proposal.md §1–2, VP-stage7-step7-answers.md, VP-led-drive-delta-4.md,
reference/beta_22.py, VP-led-drive-module.md §6 "7a" and §4.1 "Stage 7 step 7b".

## Blockers
- None.

## Should fix
- (6) **A live switch from script to compensate is not "CNTR 50 first".**
  - `Reconfigure` turns compensate on at once (`compensating = true`), so
    the render thread starts compensating on the next frame.
  - The CNTR 50 restore is sent by the script thread only at the top of its
    next loop. Its waits (`WaitUntil`) wake only for a stop, not for new
    settings, so the rest of the current script tick still goes out first.
    That can include fast-dump CNTR steps.
  - For up to one tick (400 ms by default, about 1–1.5 s with a dump),
    compensate runs while the projector is still at the script's CNTR (e.g.
    60 = 20% more signal), and may even receive more script CNTRs.
  - Fix: leave compensate off until the script thread has sent CNTR 50, or
    let the wait end on a settings change and restore before anything else.
    Rare (a live mode switch) and brief, but it is what the spec rules out.

## Notes
- (4) If `tick_ms` is below 200 (allowed down to 50), a tick that only
  changes the LED no longer waits the script's 200 ms after LPW1 before the
  next tick. The old code waited the schedule's length, which included it.
  Script sends also skip the command gap, so LPW1s could go out faster than
  the script ever sent them. The default 400 and the example profile (400)
  are not affected.
- (2) The boost now follows the output gamma, so it switches itself off for
  an output of 2.4 or BT.1886 (2.4 / 2.4 = 1). The owner's `output_gamma: 2.2`
  keeps exactly 2.2/2.4 as before; HDR outputs keep 2.2.
- (5) `SendScript` limits LPW1 to the projector's light range (`light_min`).
  The script sent raw values, but the curve never goes below 100, so this
  only matters if light_min is set above 100.
- (7) The smoother times the gain by when a frame is rendered, not when it
  reaches the screen. The difference is a frame or two, which
  `cntr_latency_ms` absorbs; that is what the 20 ms tuning advice is for.
- (5) After a switch from compensate to script, the custom-dim exit sequence
  can be interleaved with a script CNTR. They are different commands, so no
  harm; LPW1 is held back while the dim is engaged.
- The renderer's starting `compensation_eotf` values are still 2.2 until the
  configuration is published, which happens at once. Harmless.

## Verified OK
- (0) 710ce9d2: with no target, the empty screen enables only page 21
  (Updates). The new lines in "no-target startup is quiet" check that
  Updates is clickable and every other page entry is not. Passes.
- (1) **Sign fix.** Every power in "LED compensation.glsl" now keeps the
  sign (`spow`): the boost, the move into linear light and back. The gain
  multiplies negatives in both domains. The knee and the `min(x, 1)` only
  act on the bright end, and the "unity" early return is unchanged. Nothing
  else in the shader cuts below 0. The measurement hook and the look-ahead
  clamp to 0..1 only for **statistics** (below black makes no light), never
  for the picture.
- (2) **Delta 4:**
  - `compensation_eotf` is 2.41 in the shader, LedDriveConfig, the Config
    app (default and help) and the example profile; the test config uses
    the default.
  - Boost exponent = output gamma / gamma_boost. The output gamma is worked
    out per frame from the output transfer (`LedDisplayGamma`) and given to
    the module.
  - The renderer's hook, the measurement hook (same per-frame value) and the
    look-ahead table (rebuilt when the exponent changes) all use the same
    exponent.
  - The highlight-linear calculation uses `compensation_eotf` on the
    measured, already-boosted code.
  - The two compensate tests used 2.2 because that was the old default. The
    code being tested uses `compensation_eotf`, so following it is right and
    hides no change. Nothing else assumes 2.2 for the projector.
- (3) **GPU test:** the real shader on a 10-bit limited target, with VP's
  encoding repair.
  - Code 40 with boost and gain 1.3 comes out below 64 and within 1.5 codes
    of the expected value.
  - Grey 126: light at LED 100 with gain 2.55 equals light at LED 255,
    within 1%, with the boost on and off.
  - It proves both claims for the shader. "Light" is the 2.41 model, as
    delta 4 defines it.
- (4) **Against beta_22.py:**
  - Order: LPW1 at the tick start, then the script's sleeps, each counted
    from the end of the previous send. That gives 200 ms after LPW1 (only
    when LPW1 was sent), 100 ms before each fast-dump step (both dumps, and
    the 7–9 dump falling into the anti-pump band), and 200 ms before the
    final CNTR.
  - Every scheduled CNTR is sent, repeats included (the anti-pump "+0").
  - Built-in curve: the literal formulas (≥100 → 255, 0.8333 slope, 1, 5,
    <20 → 100). A custom curve still interpolates. Compensate is unchanged.
  - Startup sends LPW1 255 and then CNTR 50, as the script.
- (5) **SendScript:**
  - Holds the port for one command, with no gap or burst pause.
  - Synchronous (returns after the reply, or the timeout with read-back).
  - Never coalesced or split.
  - Returns false once shutdown has begun, including after waiting for the
    port.
  - In script mode all LPW1s go through it, so the LED worker is idle.
- (6) **CNTR 50:**
  - Sent at start in script mode even when the projector reports 50.
  - On exit or disable, after the light, within one second: 300 + 250 + 350
    ms with the dim reset, 650 + 350 without.
  - A live change within script mode keeps the current CNTR.
  - log_only uses the dry-run driver (protocol "none"), so CNTR is only
    logged. A switch to or from log_only restarts the module, with the exit
    restore.
- (7) **script_smoothing:**
  - Off by default, and then the gain stays inactive.
  - On: gain = smoothed / landed, never above 1. A decrease is ramped before
    it lands, an increase after it lands. Once bridged, the gain returns to
    1 (the picture equals the projector).
  - Landings are planned at send time + `cntr_latency_ms` (80), and moved to
    the real send times.
  - All smoother calls are under the module mutex, on one clock (both
    threads use NowMs).
- (8) **Config app:** "Smooth the script's contrast steps" writes
  `script_smoothing` to the LED drive section. "Contrast response time"
  writes `cntr_latency_ms` (0–500, default 80) to the projector section. Both
  keys are in the validation lists (boolean; integer 0–2000) and in the
  Config test's save check.
