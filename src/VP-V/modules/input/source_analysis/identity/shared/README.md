# SourceQuality (shared unit U12, `VpUnit::SourceQuality`)

The one source-quality law (doc 62, section 1.2 U12): `SourceQuality.h`, `kUnitVersion = 1`, the pure core under `SourceQualityView`
(the P14 read API), `SourceIdVote` and `SourceIdPipeline`. Carried in P4 field `quality` (no new product id).

- `TierOf` (tier edges 0.25 / 0.5 / 0.75), `Combine` (min of classical quality and the record level), `Compute` (CPU reference),
  `DitherNeed`, `ContrastRecoveryActive`.
- `Of` / `Store` / `CopyClassical`: the one access to a monitor snapshot's classical quality; `SourceResolutionMonitor` uses them.
- Migrated (identity): `SourceQualityView`, `SourceIdVote::BitrateQualityVote`, `SourceIdPipeline`, `SourceResolutionMonitor`
  (proof: `SourceIdPipelineTests`, `SharedUnitsW1Tests`). The ABR step detector (`AbrStepDetector.h`) stays the event source.
- No GLSL (the plan has none for U12). LVR call sites (Bill's file) move with the P14 / SA3 call-site package.
