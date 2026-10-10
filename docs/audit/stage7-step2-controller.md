# Audit — Stage 7 step 2: controller, mode: script

Diff audited: b3c77214..3769fa00
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: guide §4.1, answers-1 §1–§3, §5; README §5 step 2; beta_22.py
compared line by line.

## Blockers
- None.

## Should fix
- None.

## Notes
- led/LedController.cpp `CurveLevel` — uses the exact 25/30 slope between
  APL 70 and 100, so the target is one level higher than the script at APL
  76, 82, 88, 94 (script's `0.8333` truncates). Recorded by the implementer in
  the brief's Open questions, so treated as accepted; the tick-for-tick test
  skips those four APLs.
- The guide's "contrast 62 at APL 30" is 61 in the script; the port follows
  the script (also recorded in the brief).
- `highlightdetect`: at this step the test used the 99.5th percentile; step 5
  (design review SF-4) switched it to the brightest 3×3 block, which is closer
  to the script's max-of-downscale. HEAD is the better version.

## Verified OK (each against beta_22.py)
- Light target: 255 at APL ≥ 100, `(m−70)·0.8333+230` (see note), `m−40+200`,
  `(m−20)·5+100`, 100 below 20; APL truncated like `mean2 = int(mean)`.
- Steps: `steps_up_dark` 7/5/4/1/1 for APL < 40, `steps_up_bright` 7/4/2/1/1,
  `steps_down` 5/3/2/1/1 — exactly the script's branches; thresholds 100,
  50, 25, 10, 0.
- Contrast targets: 50 at ≥ 60; 50/51 for 45–59 by highlight; `int(|m−45|·0.7333+51)`;
  `int(|m−30|·1.2+62)`; 74 below 20 — same double arithmetic as Python.
- Contrast dynamics: +1 up at distance ≥ 10; with a highlight, 100 ms fast
  steps down to 10 then one more step; without, −1; the script's stale
  `difference2` reproduced; second fast dump (7–9 above target, any highlight
  state) down to 6, then the fall-through into the 6-band step (ends 5 above);
  anti-pump within 3: highlight blocks rises, no highlight blocks drops.
- Order and timing: light command first (+200 ms), fast steps at 100 ms, final
  contrast after 200 ms; the next tick waits for the schedule.
- `sudden_flag` path omitted.
- `gain_encoded = contrast_actual / 50`, encoded domain.
- Deterministic; test ControllerReproducesBeta22TickForTickOnSyntheticSequences
  runs 5 × 3000 ticks against a literal C++ transcription of the script and
  asserts every LED command, every contrast send and the state each tick.
