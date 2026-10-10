# Audit — Stage 7 step 4: mode: compensate, highlight policy, gamma boost

Diff audited: ea11aeb7..ef83121c
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: guide §4.2–4.4, §6, §9 (step 4 owner decisions), answers-1 §6–§8;
README §5 step 4. Step 5 replaced the one-pole LED model, the cut jumps and
the tick-based compensate loop; those parts are judged in the step 5 files.

## Blockers
- vprenderer/LibplaceboVideoRenderer.cpp `UpdateLedDrive` (~line 15519, still
  at HEAD) — *any* change to `[led_drive.Default]` or `[projector.Default]`
  stops the module and starts a new one. Stopping restores the LED to
  `led_ceiling`; the new module starts again from the ceiling. So:
  - switching `mode` between `script` and `compensate` jumps the LED to full
    and restarts — answers-1 §6 says "no restore; the controller continues
    from the current level";
  - live-tuning `highlight_tolerance_stops` (guide §4.3: "edited live in the
    Config app"), or any other key, makes the black floor jump up to full and
    then ramp back down over a few seconds — visible on the projector every
    time the owner tweaks a setting.
  Fix: apply tuning keys to the running module (or carry the current LED
  level and controller state into the new one), and only restore when the
  module is disabled, set to `log_only`, or the port/protocol changes.

## Should fix
- None beyond the blocker.

## Notes
- shaders/LED compensation.glsl `hook` — clamps negative values to 0 whenever
  the boost or gain is active. At PRE_OUTPUT libplacebo's own gamma encode
  has already clamped below-black in most cases, so this is unlikely to
  change anything; mentioned because brief constraint 6 says BTB/WTW are
  preserved.
- The highlight cap is applied as a weighted blend (`capWeight`) rather than
  a plain `min(gain_led, gain_highlight)`; this is the area blend the owner
  accepted in guide §9.

## Verified OK
- Hook at `PL_HOOK_PRE_OUTPUT`. Value domain checked in the bundled
  libplacebo 7.360.1 renderer.c: `pass_output_target` runs the PRE_OUTPUT
  hooks (line 2742) after `pass_convert_colors` (tone map, gamut map,
  normalized/conversion target LUT) and before `pl_shader_encode_color`
  (limited-range encode, line 2816) and the OUTPUT hooks (line 2833). Values
  are full-range normalised 0–1 in the target transfer (VP's
  `output_gamma`). A NATIVE-type target LUT would come after (documented).
  The shader file spells the stage `PREOUTPUT` (mpv syntax) — correct.
- `compensation_eotf` (2.2) linearises; `gain_domain` 0 = encoded (script),
  1 = linear (compensate); knee engages continuously (slope 1 at
  `1 − knee`, asymptote 1.0), only when the gain is above 1.
- Gamma boost (owner decision, guide §9): `v^(eotf/gamma_boost)` first, then
  the gain; measurement applies the same exponent; `gamma_boost: off`
  disables it; test config has `output_gamma: 2.2` and `gamma_boost: 2.4`.
- `gain_led = flux(ceiling)/flux(light_actual)`; gain follows the modelled
  LED level, never the target (one-pole model at this step; replaced by dead
  time in step 5).
- Highlight policy: persistence gate (sustained highlight = window minimum
  over `highlight_persist_ms`), area blend by smoothstep between
  `highlight_area_small`/`_large`, continuous protection (engage ~100 ms,
  relax over `highlight_release_ms`), `highlight_tolerance_stops`. Tests:
  ThreeFrameFlashChangesNothing, SustainedSmallHighlightEngagesSmoothlyThroughTheKnee,
  LargeSustainedHighlightLowersTheGlobalGain, HighlightsUnderHalfAPercentCostNothing.
- Fade: LED toward `led_floor_black`, gain held at its pre-fade value
  (FadeToBlackTracksDownAndHoldsTheGain). Cut to black uses `led_floor_black`.
- Exit, disable and switch to `log_only`: light restored to `led_ceiling`
  (bounded); startup brings the projector to a known state and logs it.
- Test config defaults `enabled: false`, `mode: log_only`.
