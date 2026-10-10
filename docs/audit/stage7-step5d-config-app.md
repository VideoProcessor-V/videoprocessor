# Audit — Config app switches, LED drive page, LED profile file, zoom profiles

Diff audited: 15e202ec..f38ba56e (a648ce02, 986b4e3d, f38ba56e)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 2d3bf2d3, 0 errors, no new warnings (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596).
Tests: native suite (vstest) 2056 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known); VideoProcessorConfigTests 91 passed, 2 failed (the two known RPC tests). One run at HEAD, covering all three units.

Spec used: brief Open questions entries "Black-bar zoom" and "Config app
(owner 2026-10-10)", docs/cinema/LED-PROFILE-GUIDE.md, README §4, owner
checks (a) and (b).

## Blockers
- Queue frame-rate switch (ConfigEditorWindow.cpp, `rate_rule` control, and
  the test config's `[queue.Low_Latency] when: ${source_rate} > 59`) — VP's
  `${source_rate}` is a **whole number**: StateVariables.cpp ~176 floors the
  input rate, so a 59.94 Hz source reads as 59, and `59 > 59` is false. A
  59.94 Hz game or 60p video therefore stays in the Normal (cinema) queue;
  only an exact 60.000 Hz source switches. The new control writes the same
  kind of rule (default "above 59 fps"), so it inherits the problem, and the
  fps box accepts decimals that an integer variable can never match.
  Brief constraint 3 (madVR's "fps 59–61" rule). Round 1 missed this: the
  test CinemaTestConfigurationPicksLowLatencyForHighFrameRates feeds the text
  "59.94" into `source_rate`, a value VP never produces. Fix: use
  `${cadence} > 59` (exact rate) or `${source_rate} >= 59`, and test with the
  value VP really reports.

## Should fix
- led/LedDriveConfig.h `Validate` (called by MainConfigSchema) — a missing or
  invalid LED profile file now makes the **whole configuration** invalid. At
  start-up VideoProcessorApp.cpp ~623 throws ("Invalid … configuration"), so
  VP does not start at all; on a live reload every other edit is refused too
  ("retained last-known-good"). An optional LED tuning file should only stop
  the LED drive (log it and run without the profile), not the player.
  Constraint 4 (fail safe).
- ConfigEditorWindow.cpp ~9024, LED profile card help text — says a profile
  can change "LED latency, flux table". It cannot: those keys belong to
  `[projector.Default]`, and a profile containing them is rejected (guide:
  "Projector settings … are not read from a profile"). Combined with the item
  above, following this help text stops VP from starting. Remove those two
  words.

## Notes
- (a) checked: every new control writes the key VP reads —
  `led_drive.default`: enabled, mode, led_floor, led_floor_black,
  highlight_tolerance_stops, gamma_boost, profile_file;
  `projector.default`: protocol, port, read_back; shader settings write
  `param_<name>` (read by MadVRShaderLoader `LoadShaderParameters` and set on
  the hook by name); the queue rule writes `when`. Existing values outside
  the choices (e.g. tolerance 0.4) are kept and shown, not overwritten.
- The LED page's "Lowest LED level" box is 60–255 while the file accepts
  0–255; a configured value below 60 shows as 60 but is only rewritten if the
  owner touches the box.
- Test config has two identical "Off (whole picture)" zoom rows
  (`[vprenderer.zoom]` and `[vprenderer.zoom.off]`).
- With a remote Config app, the profile file is validated on the Config
  computer's side (the validation copy is written beside the configuration);
  not tested here.

## Verified OK
- (a) Live "Switch on/off in VP" / "Use in VP" buttons on the profile pages
  (queue, screen, zoom, NLS, standard shaders) run the same command as the
  right-click menu; disabled with a tooltip when VP is not running. The zoom's
  Shift+Z key was dropped; zoom, screen and queue profiles all have a button,
  so nothing is keyboard-only.
- Shader settings box is built from the shader file's own `//!PARAM` blocks
  (description, range, default); "Reset to shader defaults" removes the keys.
- LED profile: built-in values = beta_22 script (test
  ShippedExampleProfileIsValidAndEqualsTheBuiltInValues); layering built-in →
  profile → main `[led_drive.Default]`; `enabled`/`profile_file` refused inside
  a profile; cross-key rules checked on the merged result; editing the profile
  file is picked up (its contents are part of the renderer's change signature).
- (b) One `compensation_eotf`: all consumers read it from the same merged
  `ReadLedDriveConfig` — the hook parameter (module gain or the renderer's
  copy when the module is off), the boost exponent for the measurement and
  the hook, the compensate controller's highlight maths, and the raw-frame
  path's boosted transfer (rebuilt when settings change). No hardcoded gamma
  elsewhere in the LED code.
- Zoom profiles: `black_bars` (automatic crop), `fill` (also crop narrower
  content from 1.7:1), `off`; VP starts with zoom off, as the brief records.
- Renderer touched only in `UpdateLedDrive` (profile contents in the change
  signature); no per-frame change. `Low_Latency` frame counts unchanged.
- Tests: LED drive page loads/saves the keys above, floor-black can't exceed
  the floor, live switches change text/state, rate-rule control follows and
  steps aside for custom rules, shader settings come from the shader file.
