# quality_pattern (P15 / P15c): frame-role record, DECLARATION ONLY

Records the layout and the read API of the frame-role detector's output; no detector exists yet (P15-DECL).

- `QualityPatternRecord.h`: the 64-byte POD (size pinned to `VpSkeleton::kQualityPatternBytes`), role / score / rank / step summary / block-grid period and phase / exact-copy flags, spare words zero, and the P16 block-grid texture RESERVE constants (4 channels x 2 textures, cells derived from the dimensions; nothing allocated).
- `QualityPatternView.h`: the one pure read: absent / never written / other unit version = `frame pattern: not produced` with the engaged `product.quality_pattern` entry; the causal record (P15c) = `frame pattern: provisional`. Both consumers (temporal-detail engine status, dither need) are status-only.
- Inputs (declared in `core/Products.h` on both products): coding evidence, the P5 monitor tiles at lag 0 (FrameQuality is read as a VIEW, `identity/FrameQuality.h`; `ProductFlags()` item C9b) and P14 source identity at lag 1 (held state, via `SourceQualityView`).
- The lane slot: `SkeletonModule::SetQualityPatternProducers(p15, p15c)`, registered on a new lane right after p1 / p4 and before its threads start; null today, so the lane is unchanged.
- Tests: `identity/tests/QualityPatternRecordTests.cpp`, `QualityPatternViewTests.cpp`.
- The temporal-detail engine status segment (row 792) is NOT wired here: it arrives with the TD engine. Today only the view API (StatusOnly for both consumers) exists.
