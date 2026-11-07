# SourceQuality (shared unit U12, `VpUnit::SourceQuality`)

The one source-quality law (doc 62, section 1.2 U12): `SourceQuality.h`, `kUnitVersion = 1`, the pure core under `SourceQualityView` (the P14 read API), `SourceIdVote` and `SourceIdPipeline`. Carried in P4 field `quality` (no new product id).

- `TierOf`, `Combine` (min of classical quality and the record level), `Compute` (CPU reference), `DitherNeed`, `ContrastRecoveryActive`; the defaults (`DefaultTierEdges`, `kClassicalConfidence`, `kCleanQuality`) are the single source for `VoteSettings` and the view.
- `Of` / `Store` / `CopyClassical`: the one access to a monitor snapshot's classical quality; `SourceResolutionMonitor` uses them.
- The ABR step detector (`AbrStepDetector.h`) stays the event source. No GLSL (the plan has none for U12). LVR call sites (Bill's file) move with the P14 / SA3 call-site package.

Unit: input/source_analysis (identity/shared)

## Settings
| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics
| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products
P4 field `quality` (tier, quality, confidence, ABR drop seen). Unit version 1. Consumers: deband, TD admission, dither need, contrast recovery via `SourceQualityView`; record absent / stale / another version: the classical value.

## Status and fallbacks
Existing, unchanged: FallbackCatalog `product.source_quality` (status text per consumer in `SourceQualityView`), Ctrl+I "Source identity" line.

## Tests
`VideoProcessor-Test.dll`, class `SharedUnitsW1Tests` (CPU only; frozen-copy identity proofs and law cases): `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~SharedUnitsW1Tests /Logger:trx`. Checker: `python -I tools/check_shared_units.py --strict`. Also `SourceIdPipelineTests`, `SourceIdConsensusTests`.
