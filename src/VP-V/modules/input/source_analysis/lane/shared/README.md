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

# EngineLumaGpu (the GPU twin of the optical-flow engine luma, `VpUnit::EngineLumaGpu`)

refs/105 phase 5a (owner O2 / O3). `EngineLumaGpu.h` / `.cpp` / `.hlsl`, `kUnitVersion = 1`: a cs_5_0 compute shader
(compiled at run time; the `.hlsl` is included into the `.cpp` as one raw string) that reads ONE uploaded raw frame (an
`EarlyUpload` slot opened on the producer's own D3D11 device, `EarlyUpload::AcquireReader`) and writes the 8-bit engine
view (R8_UNORM, half or quarter) the NVOF engine takes.

- Law: the CPU path's, integer only - the 10-bit code (P010 / P210 luma plane `word >> 6`, v210 Y field, UYVY / HDYC
  `byte << 2`), the U3 taps and `FinishEngine8` for the half view, the producer's `(a + b + c + d + 2) / 4` quarter step.
- Formats (O2): limited-range P010 / P210 / v210 / UYVY / HDYC. RGB, decoded / packed DV, the SDR tunnel and full-range
  YUV stay on the CPU path (the caller counts and shows it).
- `Kernel`: `Open(device)`, `Run(context, raw, packing, width, height, quarter, out)`, `Close`, `LastError`; per device.
- Phase 5a ships the kernel and its proof only; the producer switches to it in 5b.
- Tests: `lane/tests/EngineLumaGpuTwinTests.cpp` (byte identity against `MotionAhead::HalfLuma8` + the quarter step for
  every twin format, odd half / quarter edges, v210 widths ending mid-group, padded strides, 4K; one case through an
  `EarlyUpload` slot on a second reader device).

## 5b design note (producer switch; not built yet)

- Seam: the EXISTING S1 AfterPublish (LibplaceboVideoRenderer.cpp, the `RendererExtension::AfterPublish` call after
  the conversion thread's publish, ~line 30721). 0 new Bill lines (rule 32, hook freeze). Its dispatch condition
  is the VP-V predicate on the line before (`Vpv::Capture::EarlyUploadDue`); 5b relaxes it INSIDE VP-V (a
  publish is due when an early hand-off exists OR the motion producer has a second reader attached), so the Bill
  line keeps its text. Original pathway: the settings overlay forces `capture_upload render` and the motion
  producer is off, so the predicate is false and nothing is dispatched (as today).
- Hand-off: the hub fans AfterPublish out in registration order. `input.capture` runs first and uploads the raw
  frame into its `EarlyUpload` slot (the one upload, O3). The motion module, registered after it, reads that
  slot's `SlotHandle` from the payload extension, `AcquireReader` on its own device, runs
  `EngineLumaGpu::Kernel::Run` into its engine texture and calls `UsedReader`. It submits to the producer queue;
  it never uploads a second time.
- CPU path stays (visible, never log-only): no slot on this publish (capture_upload render, pool full), a format
  outside O2 (RGB, DV, SDR tunnel, full range), `AcquireReader` / `Run` failure, or a reader detach that timed out
  (`readerDetachTimeouts`). The frame then goes to the existing CPU `HalfLuma8` path. The reason is counted per
  cause and shown: the Ctrl+I motion line ("engine luma: CPU (reason)"), a one-time notice on the first fallback
  per source, the Config status row and the Ctrl+Alt+D dump. A timed-out detach also stops the twin for that
  source until the next Open (no retry loop).
- Ordering safety: a reader that dies keeps its slot (`kReaderPending`) until `DetachReader` on producer Stop or
  GpuReleased, so a slot is never overwritten while the twin reads it. The cost of a dead reader is one slot of
  the pool (`noFreeReader`), never torn bytes.
- Tests: the motion module's AfterPublish on WARP equals the CPU product byte for byte. Fallback counters for each
  cause. Original pathway dispatches nothing. Hook gating and the hook budget unchanged.
- Expected cost: about 3-4 h agent time, one incremental Test + renderer build (about 40 min), focused WARP run
  (under 10 min), one GPU-QUIET hardware chunk. GPU: one compute dispatch per frame instead of the CPU half-luma
  pass plus its upload; the CPU saving is to be measured with ProducerThroughputBench.

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
- Migrated (GPU text): the matcher's M3 coarse luma histogram (16 bins of 16 codes of the 1/32 luma, L1 of the integer counts over the
  picture's texels): `CoarseBin` / `CoarseL1` (CPU twin) and `CoarseHistHeader` / `CoarseHistBody` (the GLSL, moved byte-for-byte from
  `DejudderWarp.cpp` and spliced into its frame-trust kernel; the cut rule that reads it is U7 `CutFadeJudge::TrustCut`). No unit version
  bump (additive, byte-identical); fragment hashes pinned in `SharedUnitsW1Tests`.
- Not migrated: the `VpFrameAnalysis` Reducer (studio-code domain, flat-decile grain law: a different law, kept), LED look-ahead (output domain),
  SRM histograms, picture-only peak and the LED 32x18 / HDR grid cells (SA3, needs SK-W).
- Deferred key: `source_stats_authority` (original|p4) belongs to SA3 and the SCHEMA lock (RendererProfileConfig.h / MainConfigSchema.h).
- Plan deviation: `OpticalFlowTrust.h` (a 256-bin histogram of match COSTS, median / MAD) is reclassified out of U6 as a known false
  positive: it is not a luma histogram.

Unit: input/source_analysis (lane/shared)

## Settings
| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `source_stats_authority` | profile (deferred) | enum | original, p4 | original | not in Config (deferred to SA3; needs the SCHEMA lock, RendererProfileConfig.h / MainConfigSchema.h) | Original = Bill's SceneDetector and analysis | Declared: deferred to SA3, not implemented in this package (the key is documented once, here; events/shared shares it) | same on all |

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
