# CutFadeJudge (shared unit U7, `VpUnit::CutFadeJudge`)

The one cut / fade / flash decision (doc 62, section 1.2 U7): `CutFadeJudge.h`, `kUnitVersion = 1`. Bill's SceneDetector stays the
Original pathway and is untouched.

- `ClassifyEvents` (moved unchanged from the lane): two P4 records -> cut / "cut possible" / fade / flash; `StrengthOf` maps it to the
  Indicator strengths (FULL = cut, MINOR = cut possible).
- `GridHistCut` + `MeanAbsDifference`: the HDR veto detector's rule (`HdrVetoCutDetector::Observe` now calls it, thresholds unchanged).
- `FadeFit`: CPU twin of the matcher's M4 fade law (gain / offset / fading) with its switch in `FadeParams::enabled`; the matcher's
  GLSL stays in `DejudderWarp.cpp` (GPU).
- Not migrated (GPU copies, recorded as residuals in `tools/shared_units_baseline.json`): matcher M3 histogram cut and M4 fade GLSL,
  AdaptiveDownscale 20-code cut. SRM `FrameCutJudge`, LED look-ahead and the SA3 reader switch (`source_stats_authority`) come later.
