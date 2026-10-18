# Dynamic Drive: CNTR rising again during a bright cut's fade-up (2026-10-18)

Branch `fix/cntr-bright-cut`, from local `fix/led-flaky-tests` (92e1e8c5, the manual LED
clock). CPU only, no GPU, local only.

## Finding

`ModuleBrightCutDumpsContrastAndFadesUp` passed only when frames were fed much faster than
real time. At the real frame rate (the module's clock moving one frame period per frame)
CNTR goes up again during the fade-up after a bright cut.

## Reproduction (real rate, manual clock)

Compensate, dark scene with highlights (APL 19, highlight 200, CNTR assist at 59), then a cut
to APL 121 with 1% of the picture at white. With the original test's port (no command gap,
so every CNTR the pairing asks for is written), 24 fps:

| ms after cut | CNTR | sustained highlight | vpHeadroom | fade-up gain ceiling | modelled LED |
|---|---|---|---|---|---|
| 0 | 55 (dump) | 200 | 1.90 | 1.257 | 0.36 |
| 83 | 51 | 200 | 1.98 | 1.257 | 0.36 |
| 125 | 50 | 200 | 2.02 | 1.257 | 0.44 |
| 250 | **54** | 235 | 1.25 | 1.257 | 0.63 |
| 292 to 417 | 52, 51, 50 | 235 | 1.25 | 1.257 | 0.68 to 0.79 |

60 fps gives the same shape (55 51 50 then 54 or 55 at 250 ms, then 54 down to 50). With
cut_cntr_dump off (CNTR held), the same happens. With the owner's port defaults (200 ms gap,
no read-back) the 200 ms gap merges some steps, but the projector still gets a rise (for
example 50 then 51 then 50 at 23.976 fps, held). The rise does not depend on the frame
rate: it always comes at highlight_persist_ms (250 ms) after the cut.

internal_script, with and without script_lookahead, at 23.976/24/50/59.94: no rise. The
script's CNTR comes from its own table (ScriptController::ContrastTargetFor,
LedController.cpp:345). With the look-ahead it goes 74 to 50 in one command that lands by
the cut's first frame. Without it, it steps down at the script's own rhythm and never goes
back up. Nothing changed there.

## Is CNTR 54 needed?

Taken alone, 54 matches the new scene's real statistics: the highlights really are white,
and the LED is still at 0.63. The fault is the drop to 50 just before it, which came from
stale statistics. Then the window catches up and CNTR rises again, so it pumps 55, 50, 54
during the fade-up. The 7c-2 design forbids that pump ("During the fade-up CNTR never rises",
LedDriveModule.cpp:818-819).

- The pairing's need is `compensation / max(1, vpHeadroom) x (1 + bias)`
  (CntrPairing::Update, LedController.cpp:848-851).
- `vpHeadroom = 2^allowedStops / (highlightLinear x protection)` (LedController.cpp:760-761)
  comes from the **sustained** highlight. That is the minimum over highlight_persist_ms
  (LedMeasurement.cpp:485-486), and unlike the sustained APL (LedMeasurement.cpp:508) it is
  not restarted at a cut. So for 250 ms after the cut it still holds the dark scene's 200.
  Protection also decays against that value (LedController.cpp:571-572). Together they give
  a headroom of 1.9 to 2.1.
- During the fade-up, VP's gain is capped at the fade-up's gain ceiling: GainLocked takes
  `min(recoveryLimit, ...)` (LedDriveModule.cpp:1073), and CutRecovery::Step returns at most
  that ceiling (LedController.cpp:842). The ceiling comes from the cut frame's own highlight
  (LedController.cpp:693-698) and is 1.257. So the pairing counted software headroom (about
  2.0) that the gain could not use. CNTR dropped to 50 while the gain sat at its ceiling and
  the picture stayed under the ramp.
- At 250 ms the window sees 235, the headroom falls to 1.25, and the pairing asks for 54. The
  only cap is the dump point (`SetCeiling(pairing.Contrast())`, LedDriveModule.cpp:836), not
  "never rises", so CNTR goes back up.

When frames come much faster than real time, the window never ages during the fade-up, which
is why the old test missed it.

## Fix (behind the existing switch cntr_pair_predict, default on)

LedDriveModule.cpp, StepCompensateLocked, just before the pairing update (916-932). While a
bright cut's fade-up is pending or running:

1. The headroom passed to the pairing is `min(vpHeadroom, fade-up gain ceiling)`. This counts
   only the software gain the fade-up allows, so the software/CNTR split stays as designed.
2. The pairing's ceiling follows each descent (`SetCeiling(min(ceiling, contrast))`), so CNTR
   never rises during the fade-up (7c-2).

After the fade-up the ceiling is cleared as before. Timing, the dump, cut_cntr_level, the LED
and the script source are unchanged. Off: exactly the previous pairing. Comments were updated
in LedDriveConfig.h and LedController.h, and the Config tooltip in ConfigEditorWindow.cpp.

After the fix, original port, 60 fps: dump 55 54 53 52 51 50; held 58 57 56 55 54 53 52 51 50.
Owner port, 24 fps: dump 55 54 50; held 56 51 50. All only go down.

## Tests (LedDriveCompensateTests)

- `ModuleBrightCutDumpsContrastAndFadesUp`: now at the real frame rate (60 Hz, its original
  port). It checks that CNTR never rises and is never above need, plus its earlier checks.
- `BrightCutContrastNeverRisesAtRealFrameRates`: 23.976/24/50/59.94, reactive and look-ahead
  (seen cut), dump and held, owner port defaults. It checks: CNTR never rises; CNTR is 50
  whenever the modelled LED alone carries the new scene (above need otherwise); the fade-up
  only climbs; the gain stays under the ceiling; the fade-up ends; and CNTR ends at 50.
- `BrightCutPairPredictOffIsThePreviousPairing`: with the switch off, the rise is still there
  (the earlier behaviour).
- `ScriptBrightCutContrastNeverRisesAtRealFrameRates`: internal_script at the 4 rates, with
  script_lookahead on and off. CNTR never rises and ends at the script's 50. With the
  look-ahead it is one command that lands by the cut's first frame.

With only the fix block disabled (cntr_pair_predict on), the first two tests fail (rises
50->54 at 250 ms, and 50->51 on the projector at 23.976 held).

LED classes (`LedDrive*|LedScript*|LedController*`, 169 tests), single-threaded vstest with
TRX, 3 runs in a row: 169/169 each time, about 1.1 min per run.

## Not changed / for the owner

- The sustained highlight is still not restarted at a cut. Restarting it would also change
  the LED's highlight floor and flash rejection at cuts. The fix above does not need it.
- With a 200 ms gap and the look-ahead, the cut's CNTR can be replaced by the next paired
  descent before it is written (for example 56 then 54, never 55). Values only go down.
