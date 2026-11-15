# Tests for `repair.texture_restore`

Standalone module tests land here as the module migrates. Until then the tests listed in ../README.md live in the legacy test projects (VideoProcessor-Test, VideoProcessor-BetaTests, VideoProcessor-ConfigTests).

- `SoftResetTests.cpp` (VideoProcessor-Test, class `SoftResetTests`; WAIVER-C5-SOFTRESET-ON, plan D:\codex\vp-handover\lead19\c5-softreset.md item 5, 2026-11-15): `cut_soft_reset` default on pinned (constant, both `Params`, the loaded renderer default, vpv-tiers); off = no soft reset and the same pictures (CPU models); on = only a confirmed candidate at the bar, never provisional / cut / flash / fade / 0.99, never on cut-free clips under the real U7 law; keep 0.5; a cut still restarts; the status text for off and for the M3 "inactive: provisional verdicts only" state. CPU only except `SettingsDefaultOnThroughTheLoader` (the renderer DLL's settings exports, no device).
- `D8SceneJudgeAgreementTests.cpp`: the D8 truth harness (GPU, real clips).
