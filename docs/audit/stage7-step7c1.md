# Audit — Stage 7 step 7c-1 (LED/CNTR models, rate law, CNTR assist) + fixes for stage7-step7ab.md

Commits: bb1a1231 (fixes), 01b6cf51 (docs only), 91216a2a (7c-1).
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 91216a2a, 0 errors, no warnings in LED code.
Tests: native 2503 total, 2502 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted —
known). New tests pass: SwitchFromScriptToCompensateResetsContrastFirst, RateLawIsProportionalBetweenMinAndMax,
SettleModelIsDeadTimeThenFirstOrderLag, CntrPairingIsSoftwareFirstAndChangesOnlyOnCrossings,
CntrPairingFollowsAFloorRamp, ModuleCntrAssistPairsWithTheModelledLed, CntrAssistOffNeverSendsContrast.
VideoProcessorConfigTests 102 passed, 2 failed (the two known RPC tests). No Device Guard block.
Same as your results.

Specs: control policy §2, §3.1, §3.2, §6, §7, §9; step 7 answers Q1, Q2; proposal §3; module guide
§4.2 (7c-1 box).

## Fixes for stage7-step7ab.md
- Should fix 6 (script → compensate) — **resolved.**
  - Compensate now waits (`awaitingContrastReset`): no compensate step, no
    LPW1, no CNTR and no compensate gain, until the script thread has sent
    CNTR 50.
  - The script's waits now also end on new settings, so the rest of a tick
    is not sent under the new mode. The test checks that at most the command
    already in flight goes out before the reset.
  - Small leftover (Note): with `script_smoothing: on`, the script's last
    bridging gain (≤ 1, usually exactly 1) stays on the picture during that
    short wait, because nothing clears it on this path.
- Note 4 (LPW1-only tick below tick_ms 200) — **resolved.** The next tick
  waits at least the script's 200 ms after the LPW1.
- Side effects of the new wake: none harmful. A stop still ends the loop
  at its top. A settings change in the middle of a script tick starts the
  next tick at once, so that one tick's first command can follow the last
  one without the script's spacing. This happens once per Apply.

## Blockers
- None.

## Should fix
- (3) **A CNTR waiting for the port is not dropped when a fade's gain hold
  or the custom dim starts.**
  - The pairing stops asking for new values then, as required. But a value
    queued in the last frame before still goes out.
  - With the custom dim it is held behind the dim sequence and sent *after*
    it, during the dim.
  - The CNTR model then lands it while the gain is held, so the picture
    moves by a CNTR step in the middle of the fade to black.
  - Fix: `SetContrastAsync(-1)` when the hold or the custom dim begins. This
    is an edge case.
- (3) **CNTR can stay at 51 when 50 is enough. The owner does not accept
  this: a stuck 51 is visible.** (Re-rated 2026-10-11 on the owner's
  report; I first listed it as a harmless note.) CNTR's gain on near-black
  codes changes how the dark parts of the picture, and the black next to
  them, are seen.
  - `CntrPairing::Update` steps down only when one step lower covers the
    need with a 2% margin. When the need is between 0.98 and 1.0 (VP's gain
    right at its limit), 50 already covers it fully, but 51 is kept.
  - The same margin keeps CNTR one step above the need while the need
    falls (e.g. during a slow brighten), up to one step per crossing.
  - Fix: no margin for the final step to 50 (go to 50 as soon as
    Light(50) ≥ need), and keep CNTR above 50 no longer than needed (for
    example, put the anti-chatter margin on the way up instead of the way
    down).
  - Test: a need of 0.99 with CNTR at 51 must send 50.

## Owner's decision (2026-10-11)
- Keep the split between software and CNTR compensation as the policy
  has it.
- The owner's clarification: CNTR does not raise black itself. It changes
  the gain on near-black codes, and that changes how the black next to
  them is seen.
- What is not allowed is CNTR left above the value the pairing needs, as
  in the stuck-51 case above.

## Notes
- (4) **Question for the planner: how far the LED may now dim under highlights.**
  - With the assist on, the highlight floor is lowered by the full CNTR
    headroom, (74/50)^2.41 ≈ 2.57×. So in highlight scenes the LED dims up
    to 2.57× further and CNTR makes up the difference.
  - §3.1 says CNTR covers only "the remainder", and that its reserve
    should be kept for the bright cut. Lowering the floor *creates* that
    remainder on purpose.
  - It is a reasonable reading, but the planner should confirm it, or cap it
    with a separate setting.
  - In dark scenes without sustained highlights nothing changes (floor 0,
    CNTR never moves).
- (4) **Bright cut while CNTR is high, before 7c-2:** not worse than before.
  - The lowered floor exactly matches what CNTR adds, so at the cut the
    light available equals the old LED-only floor.
  - CNTR comes down only as the modelled LED light rises (the lower of now
    and ahead), so the light is never less than before.
  - Steps are one CNTR value (≈ 4.9%), covered by the gain. Model timing
    errors show as a brief step; that is what 7c-2's dump replaces.
- (3) Hysteresis at the bottom: moved to Should fix (owner: CNTR 51
  visibly raises the black level).
- (6) Live switch from compensate to script: the compensate gain is
  removed at once, but CNTR 50 lands only after the script thread's
  command plus CNTR's latency (about 100 ms). The picture can be brighter by
  the CNTR excess for that moment. It is the mirror of Should fix 6, short
  now that the thread wakes at once. Operator-initiated and rare.
- (1) The gain now divides by the settled light. If a flux table ever gave 0
  light, the old code used gain 1 but the new code divides by 1e-6. LPW1
  never goes that low with the floors, so this cannot happen in practice.

## Verified OK
- (1) **Models:**
  - LED: dead time in whole frames as before (`led_latency_ms`), then a
    first-order settle (`led_settle_ms`, default 300; 0 = exactly the old
    dead-time-only model).
  - The gain uses the settled light; `ModelledLight` returns the landed
    level once settled.
  - CNTR: its own dead time (`cntr_latency_ms`) and settle
    (`cntr_settle_ms`, 0), driven by the CNTR values the driver really
    wrote.
- (2) **Software first:**
  - `vpHeadroom` = 2^allowedStops / (sustained highlight × protection): the
    largest gain that lifts the sustained highlight by no more than the
    allowed stops.
  - CNTR's need = compensation / headroom. CNTR is the smallest whole value
    with (CNTR/50)^E ≥ need, up to `cntr_max_usable` (74).
  - VP gain = LED compensation / modelled CNTR light, then the highlight cap.
  - `software_first: off` lets CNTR go first.
  - With no sustained highlight the headroom is unlimited, so CNTR never
    moves in a dark scene. Tested.
- (3) **Commands:**
  - Only when the whole value changes: up at once, down with the 2% margin.
    A boundary does not chatter (tested 100 alternations, 0 commands).
  - CNTR always returns to 50 once the need is gone (the fixed bug).
  - The async CNTR takes the next slot ahead of a waiting LPW1, after the
    usual gap.
  - Timing uses the lower of the modelled LED light now and
    `cntr_latency_ms` ahead.
  - Nothing new is asked for during a fade's gain hold or the custom dim
    (see Should fix for one already waiting).
  - The log line carries `cntr=` and the `lpw1:cntr=` counts.
- (5) **Rate law:** clamp(k·|d|, min, max) per direction. Defaults: k 4.0 / 1.2,
  min 20 / 10, max 0 = rate_up / rate_down; `rate_gain` 0 = the fixed rate.
  The step is never more than the remaining distance, so the ramp lands
  exactly; `rate_min` keeps it from creeping. Tested, including 60 frames of
  holding still.
- (6) **CNTR back to 50:**
  - On exit or disable, after the light, within the one-second budget (the
    7b restore, now also when compensate raised CNTR).
  - On a switch to script, a waiting async CNTR is dropped first, then the
    script thread sends 50 before its first tick.
- (7) **Regression:**
  - With `cntr_assist` off, `led_settle_ms` 0 and `rate_gain` 0, every new
    path reduces to the old code: fixed rates, floor divisor 1, CNTR need 0
    (never sent), CNTR light 1, settled light = landed level, and the same
    gain formula.
  - The old tests change only those settings plus a longer log line, so
    they are a valid proof.
- (8) **Thread safety:**
  - The pairing, both models, the command lists and `assistContrastDriven`
    are touched only under the module mutex.
  - `pendingContrast` and the send counters are under the driver's
    stateMutex.
  - The lock order (module mutex, then driver state) is the existing one,
    and the driver never calls back into the module.
