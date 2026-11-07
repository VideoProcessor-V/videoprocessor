# CutFadeJudge (shared unit U7, `VpUnit::CutFadeJudge`)

The one cut / fade / flash decision (doc 62, section 1.2 U7): `CutFadeJudge.h`, `kUnitVersion = 1`. Bill's SceneDetector stays the Original pathway and is untouched.

- `ClassifyEvents` (moved unchanged from the lane): two P4 records -> cut / "cut possible" / fade / flash; `StrengthOf` maps it to the Indicator strengths (FULL = cut, MINOR = cut possible).
- `GridHistCut` + `MeanAbsDifference`: the HDR veto detector's rule; its thresholds (`kGridDifference`, `kHistogramDistance`, `kHistogramDistanceAlone`) are the single source and `HdrVetoCutDetector` aliases them.
- `FadeFit`: CPU twin of the matcher's M4 fade law with its switch in `FadeParams::enabled`; the matcher's GLSL stays in `DejudderWarp.cpp`.
- Not migrated (GPU copies, residuals in `tools/shared_units_baseline.json`): matcher M3 histogram cut and M4 fade GLSL, AdaptiveDownscale 20-code cut. SRM `FrameCutJudge`, LED look-ahead and the SA3 reader switch come later.

Unit: input/source_analysis (events/shared)

## Settings
| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `source_stats_authority` | profile (deferred) | enum | original, p4 | original | not in Config (deferred to SA3; needs the SCHEMA lock, RendererProfileConfig.h / MainConfigSchema.h) | Original = Bill's SceneDetector and analysis | DEFERRED, not implemented in this package | same on all |

## Environment and diagnostics
| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products
P4 event (cut / fade / flash are derived per consumer, never stored). Unit version 1; consumers: lane classifier, HDR veto detector (live); matcher, camera, LED, SRM later. Absent or stale: the consumer's own reduced path.

## Status and fallbacks
No downgrade of its own. The Original (Bill) SceneDetector remains available as the original authority.

## Tests
`VideoProcessor-Test.dll`, class `SharedUnitsW1Tests` (CPU only; frozen-copy identity proofs and law cases): `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~SharedUnitsW1Tests /Logger:trx`. Checker: `python -I tools/check_shared_units.py --strict`.
