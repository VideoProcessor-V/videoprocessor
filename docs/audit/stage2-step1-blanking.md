# Audit — Stage 2 step 1: edge blanking

Diff audited: 1.4-beta-rc1..84883ae5 (0f7143d9, c9ddc15f, 84883ae5)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: brief constraint 8, Stage 2 item 0, §8 test config.

## Blockers
- None.

## Should fix
- None.

## Notes
- docs/cinema/VideoProcessor-test.cfg `[vprenderer.viewport.cinema]` —
  `automatic_crop: false`, still false at HEAD. Brief §8 asks for automatic
  crop "approximating the zoom169/zoom1 rules". Not a code problem; the test
  config simply does not do it yet. Owner to decide.
- vprenderer/EdgeBlanking.h `Apply` — insets leaving less than half of either
  axis are ignored (one log line). Sensible guard; not in the spec.

## Verified OK
- Four keys `blank_left/right/top/bottom`, whole numbers 0–4000, default 0, in
  the viewport profile (RendererProfileConfig.h validation and parse;
  renderer parse; Config app fields with tooltips and "0" defaults).
- Blanking shrinks `target.crop` before `configureViewport`, so crop, NLS and
  screen fitting all lay the picture out inside the lit area; the blanked
  strips stay part of the surface clear (black). Picture is scaled, not cut.
- NLS target aspect (`outputPanelAspect`) uses the blanked aspect
  (`BlankedAspect`), so the anamorphic stretch is computed for 2535×1600.
- Live change of the keys is detected (`edgeBlanking !=` in the change check)
  and logged once per change.
- Own commits (c9ddc15f code, 84883ae5 test config) before the hook chain, as
  required. Test config sets 10 / 15 / 0 / 0 with an F4 "no blanking" A/B
  profile; `Low_Latency` copied unchanged.
- Tests: FL35 geometry (lit area 10..2545, 16:9 / 1.85 / 2.39 fits stay
  inside it with the 4:3 lens), zero / excessive / negative insets leave the
  output unchanged, config survives the profile runtime, invalid values
  rejected naming the key.
- Still correct at HEAD (LibplaceboVideoRenderer.cpp ~13451).
