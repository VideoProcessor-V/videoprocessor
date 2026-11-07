# CutFadeJudge (shared unit U7, `VpUnit::CutFadeJudge`)

The one cut / fade / flash decision (doc 62, section 1.2 U7): `CutFadeJudge.h`, `kUnitVersion = 1`. Bill's SceneDetector stays the Original pathway and is untouched.

- `ClassifyEvents` (moved unchanged from the lane): two P4 records -> cut / "cut possible" / fade / flash; `StrengthOf` maps it to the Indicator strengths (FULL = cut, MINOR = cut possible).
- `GridHistCut` + `MeanAbsDifference`: the HDR veto detector's rule; its thresholds (`kGridDifference`, `kHistogramDistance`, `kHistogramDistanceAlone`) are the single source and `HdrVetoCutDetector` aliases them.
- `FadeFit`: CPU twin of the matcher's M4 fade law with its switch in `FadeParams::enabled`. The law's numbers are named constants (`kFadeMinStepCodes`, `kFadeMaxStepCodes`, `kFadeMinBlack`, `kFadeMaxBlack`, `kFadeScaleEps`, `kFadeGain*`, ...); the matcher takes its per-frame step window from `FadeParams` (scaled to the measured source rate, never a literal rate).
- GLSL (moved byte-for-byte from the consumers; the consumers splice or dispatch the unit's text): `FadeKernelHeader` / `FadeKernelBody` (the matcher's M4 fade test, `DejudderWarp.cpp`), `TrustCutRegionalLine` / `TrustCutPlainLine` (the matcher's frame-trust cut; the histogram sweep it reads is `SourceStats::CoarseHistBody`, U6), `MeanStepCutLine` (AdaptiveDownscale's scalar-pass cut). CPU twins: `FadeFit`, `TrustCut`, `MeanStepCut`.
- Law of the two added cuts: matcher trust cut = share of trusted blocks below the minimum share AND (coarse histogram distance above the cut threshold OR share below `kTrustCutShareAlone` = 0.05); without the regional hold the share alone. AdaptiveDownscale cut = no state yet, or the frame's mean luma (8-bit codes) moved more than `kMeanStepCutCodes` = 20.
- Identity: no unit version bump (additive, byte-identical). The assembled matcher and AdaptiveDownscale kernels were proven textually identical to the base kernels; the fragment hashes are pinned in `SharedUnitsW1Tests` (`ResidualKernelTextIsTheTextTheConsumersCarried`) and the existing GPU tests (`DejudderHookTests`, `EstimatorHookTests`, the AdaptiveDownscale GPU tests) are the behavioural identity gate.
- Not migrated: SRM `FrameCutJudge`, LED look-ahead and the SA3 reader switch come later. The matcher's fade APPLICATION (B x gain + offset in its half-luma pass, reading `fade_state`) is a consumer of the fade state this unit writes, not a second judgement.

Unit: input/source_analysis (events/shared)

## Settings
| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics
| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products
P4 event (cut / fade / flash are derived per consumer, never stored). Unit version 1; consumers: lane classifier, HDR veto detector (live); matcher, camera, LED, SRM later. Absent or stale: the consumer's own reduced path.

## Status and fallbacks
No downgrade of its own. The Original (Bill) SceneDetector remains available as the original authority.

## Tests
`VideoProcessor-Test.dll`, class `SharedUnitsW1Tests` (CPU only; frozen-copy identity proofs, kernel-text hashes and law cases; GPU identity: `DejudderHookTests`, `EstimatorHookTests`, the AdaptiveDownscale GPU tests, run by the train owner on a mains-powered GPU): `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~SharedUnitsW1Tests /Logger:trx`. Checker: `python -I tools/check_shared_units.py --strict`.
