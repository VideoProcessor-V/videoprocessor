# Audit — Stage 2 addendum: upscaling shaders and automatic queue switching

Diff audited: ef83121c..8db148cf (cb1885e1 docs, 8db148cf code/config)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: brief Stage 2 addendum, constraint 3 (queue auto-select), §9
references, and the owner-recorded entries in the brief's Open questions.

## Blockers
- None.

## Should fix
- None.

## Notes
- shaders/FSRCNNX_x2_16-0-4-1.glsl, shaders/ArtCNN_C4F16.glsl — upstream
  `//!WHEN` changed from "both axes ≥ 1.3×" to "height ≥ 1.3×" so they engage
  with the 1.33× lens (1080p → ~1.48× vertical, ~1.11× horizontal). The change
  is documented at the top of each file and in the release manifest; recorded
  in the brief. For any upstream PR, call these "modified".
- The brief says "use the output-height variable, never a literal": all four
  rules use live texture sizes (`OUTPUT.h`, `NATIVE_CROPPED.h`,
  `POSTKERNEL.h`); the only literal is the authors' 1.3 ratio.
- FSRCNNX/ArtCNN hook LUMA, so they do nothing on RGB capture (noted in the
  config comment and the brief).
- `[queue.Low_Latency] when: ${source_rate} > 59` adds a selection rule to
  the Low_Latency section; its frame counts are unchanged. Constraint 3 asks
  for exactly this auto-selection, so it is treated as allowed.

## Verified OK
- FSRCNNX (igv, LGPL-3.0), ArtCNN C4F16 (MIT, licence file shipped) and
  SSimSuperRes (igv, LGPL-3.0, unmodified) installed as `[shader.standard.*]`
  entries, all disabled by default; config comment says enable at most one of
  FSRCNNX/ArtCNN.
- SSimSuperRes engages only when the source is shorter than the output;
  SSimDownscaler (unmodified upstream) only when taller — the mirror the
  brief asks for.
- Release manifest, project copy items and the package shader count (16)
  updated together.
- Tests: AllBundledStandardShadersLoadAndUpscalersEngageByHeight parses all six
  bundled shaders on a real libplacebo (WARP) device and checks the stages
  and rules; CinemaTestConfigurationPicksLowLatencyForHighFrameRates checks
  59.94 fps → Low_Latency and 23.976 fps → Normal from the shipped test config.
- No renderer code changed in this step.
