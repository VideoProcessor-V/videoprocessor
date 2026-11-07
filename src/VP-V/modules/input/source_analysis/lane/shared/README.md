# HalfLuma (shared unit U3, `VpUnit::HalfLuma`)

The one 2x2 box law of the half / quarter luma pyramid behind product P1 (doc 62, section 1.2 U3). Header-only:
`HalfLuma.h`, `kUnitVersion = 1` (the lane's `kHalfLumaVersion` is an alias of it).

- Grid: `HalfDim = ceil(w/2)`, `QuarterDim = ceil(w/4)`, byte sizes of the P1 layout (half plane then quarter plane, uint16).
- Taps: `Tap0(x, n) = min(2x, n-1)`, `Tap1(x, n) = min(2x+1, n-1)` (odd edges replicate), summed `Box4(a, b, c, d)` in that fixed order.
- Finishing: `FinishP1(sum) = (sum + 2) >> 2` (half and quarter, 16-bit-scaled samples) and `FinishEngine8(sum) = min(255, sum >> 4)`
  (10-bit codes to the 8-bit mean code / 4, the optical-flow engine view).
- `Glsl()`: the accessors `vpu_half_luma` / `vpu_quarter_luma` (contract text, not wired into a pass yet).

Consumers migrated (identity, proven in their test files): the lane producer (`LaneCpuProducts.h` HalfLumaRows / QuarterFromHalf,
`LaneProductsTests.cpp`) and the NVOF producer's 8-bit box (`MotionAheadProducer.cpp` HalfLuma8, `VectorStoreTests.cpp`).
Not migrated (GPU float shaders, no byte-identity proof without a GPU run): the matcher's DOWNSAMPLE_BODY (`DejudderWarp.cpp`)
and the interpolator's LOW_HALF_BODY (`FrameInterpolator.cpp`); recorded as residuals in `tools/shared_units_baseline.json`.

Determinism: integer arithmetic, fixed tap order. Any change of the grid, tap order, rounding or GLSL entry names bumps `kUnitVersion`.

# SourceStats (shared unit U6, `VpUnit::SourceStats`)

The one law of the P4 source-stats record (doc 62, section 1.2 U6): `SourceStats.h`, `kUnitVersion = 1` (= the record version and
`kExpectedStatsUnit`). Moved unchanged from `LaneCpuProducts.h`; the lane names (`SourceStatsRecord`, `StatsAccumulator`,
`ComputeSourceStats`, ...) are aliases of it.

- Record (8 KB): 1024-bin histogram (top 10 bits of the 16-bit half plane), mean, min/max, p1/p50/p99 (bin centre), evidence rect
  (rows/cols above p1 + (p99 - p1) / 16; NOT Bill's active picture, that is U8), grain (median 8x8 block RMS second difference).
- Shared small laws: `FirstBinReaching` (percentile / p99.9 scan), `HistBin10` (N-bin histogram of 10-bit codes), `Histogram10`,
  `HalfL1Distance` (the input of U7).
- Migrated (identity): the lane producer and live fill (`LaneProductsTests`, `AnalysisLaneTests`), the `VpFrameAnalysis` percentile scan,
  the HdrInference veto grid (bin law, active-rows histogram, p99.9, histogram distance), proven in `SharedUnitsW1Tests.cpp`.
- Not migrated: the `VpFrameAnalysis` Reducer (studio-code domain, flat-decile grain law: a different law, kept), the matcher M3 16-bin
  GPU histogram, LED look-ahead (output domain), SRM histograms, picture-only peak and the LED 32x18 / HDR grid cells (SA3, needs SK-W).
- Deferred key: `source_stats_authority` (original|p4) belongs to SA3 and the SCHEMA lock (RendererProfileConfig.h / MainConfigSchema.h).
- Plan deviation: `OpticalFlowTrust.h` (a 256-bin histogram of match COSTS, median / MAD) is reclassified out of U6 as a known false
  positive: it is not a luma histogram.

Unit: input/source_analysis (lane/shared)

## Settings
| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `source_stats_authority` | profile (deferred) | enum | original, p4 | original | not in Config (deferred to SA3; needs the SCHEMA lock, RendererProfileConfig.h / MainConfigSchema.h) | Original = Bill's SceneDetector and analysis | DEFERRED, not implemented in this package | same on all |

## Environment and diagnostics
| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products
P1 half + quarter luma (U3, version 1) and P4 source stats (U6, version 1 = `kExpectedStatsUnit`). A reader built against another
version engages the consumer's reduced path (FallbackCatalog `product.*`); the veto and the lane classifier have no reduced path of their own.

## Status and fallbacks
No downgrade of its own; the P4 walk either/or (`WalkSource`, Original = Bill's walk) shows `FallbackCatalog::SourceStatsWalk` when Lane is requested without P4.

## Tests
`LaneProductsTests`, `AnalysisLaneTests`, `HalfLumaUnitTests`, `SharedUnitsW1Tests` (CPU only), e.g.
`vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~SharedUnitsW1Tests`.
