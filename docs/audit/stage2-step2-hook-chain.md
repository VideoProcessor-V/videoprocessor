# Audit — Stage 2 step 2: custom GLSL hook chain

Diff audited: 84883ae5..5dcfc088 (8f9be0d1, 5dcfc088)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: brief Stage 2 items 1–8, §8 config contract, constraint 4.

## Blockers
- None.

## Should fix
- None.

## Notes
- vprenderer/CustomHookChain.cpp `SelectRules` — orders by profile list
  order first, then `stage`/`order`. The brief (Stage 2 item 1) says
  "ordered by stageOrder then profileSelectionOrder". The implementation
  follows the fork's own madVR rule (MadVRShaderLoader.h comment). Only
  matters if two enabled shaders hook the same stage; libplacebo still runs
  each hook at its own stage. Owner/designer to confirm which order is wanted.
- LibplaceboVideoRenderer.cpp `MonitorRefreshHz` — `{{refresh_hz}}` comes
  from Windows' integer refresh, so 59.97 Hz reads as 59. No bundled shader
  uses it today.
- `saturation: +5` maps to libplacebo saturation 1.05. Whether that equals
  madVR's "+5" is unverified (already an open question in the brief).
- Test config: the sigma-delta entry says `stage: post_resize`; the shader
  itself hooks OUTPUT (the config `stage` only affects ordering). Harmless but
  could confuse when reading the config.

## Verified OK
- Rules with `!nls && !none && filename` collected in
  `SetConfiguredShaderSelection`; previous hooks destroyed first; file read,
  `{{…}}` templates (six built-ins + `param_<name>`), parsed with
  `pl_mpv_user_shader_parse`, `//!PARAM` values set by name and clamped to the
  declared range; one log line per load or failure.
- Hook list rebuilt per frame: custom hooks + NLS when active. The old
  `num_hooks == 1` test replaced by an explicit check that the NLS hook is the
  one submitted. NLS runtime failure now withdraws only the NLS hook
  (NlsHookRecovery.h).
- Dither suppression only for frames where a submitted custom hook owns
  `PL_HOOK_OUTPUT`; `dithering:` otherwise applies (true A/B).
- Templates are re-resolved (and the hook rebuilt) only when a value the
  shader uses changes, so storage textures follow resolution changes without
  needless rebuilds.
- `cinema_only: true` skips the hook in `Low_Latency`; stats overlay lists one
  line per active hook.
- Fail safe: read, template, parse and runtime (libplacebo-disabled) failures
  drop only that hook, log once, and the configured dither resumes. Never a
  black screen.
- `.glsl` accepted in `[shader.standard.*]` (Stage 2 item 8) — covered by
  CustomGlslStandardShaderCarriesParamsAndCinemaOnlyToVPRenderer.
- All three shaders ship disabled; release manifest and package shader count
  updated together; licence files included.
- Sigma-delta storage textures are sized by `{{output_width}}`/`{{output_height}}`
  (the hardcoded `//!SIZE` is gone).
- Tests cover chain order, dither suppression, parameter application,
  load-failure fallback, reload on template change, cinema_only, runtime
  failure latch and the no-hook path.
