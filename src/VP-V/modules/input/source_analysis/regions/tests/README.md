# Tests for `input.source_analysis/regions`

- `RegionEvidenceTests.cpp` (CPU): block size from the frame; grain level read from the shared split (N, E1 = the split's
  D1 band); reduced form without a split; letterbox, pillarbox and off-grid letterbox (bar blocks carry no evidence, the
  bar edge never reads as an edge); synthetic ringing (ringing step vs clean step vs grain); banding (smooth ramp vs
  texture vs steep ramp vs subtitle box); text/overlay from rects, text-guard grid and camera Overlay labels; camera
  confidence/labels/tiny movers and their status lines; HDR/PQ visibility after tone mapping; determinism; the float
  mirror against the double reference; the accessor; skeleton products in the graph and the VRAM governor with the 4K/8K
  figures (log lines `REGION_*`).
- `RegionEvidenceWarpTests.cpp` (Category GpuWarp: WARP in the CPU tier; on hardware with `VP_TEST_GPU=hardware`): the
  stats pass against `StatsBlock<float>` for SDR/PQ/HLG plus run-to-run bit identity; the fused accessor against
  `DeriveBlock<float>` including the out-of-grid neutral sample.
- `EdgeMapFillTests.cpp` (CPU): the kEdgeMap owner fill: reserve derived from the declared size at the source luma size,
  registration under the catalog owner, the module's re-registration on the largest live size (forget, no per-frame retry),
  and the worded declines (no GPU context, no source luma view, above the reserve, texture refused). GPU parity vs
  `BusyEdge::SobelMagnitude`: `EdgeMapStageGpuParityTests.cpp` (102b KEDGEMAP-RULE3; its first run is owed).
- `EdgeMapStageTests.cpp` (CPU, CB slice 18 S2b): the kEdgeMap stage variant: R = `SobelMagnitude` of the measure luma
  (`VpMeasure::MeasureLumaCode`) is X5's former private Sobel x 4 exactly and G is its luma (Laplacian exact), for the SDR,
  linear and PQ / HLG measure domains; taps clamp to the whole picture, not a crop; the fill's axes, defaults, stage key and
  reserve (8 B/px); RG32F at the stage picture's own size and ordinal; the worded declines (code unit, no size, not offered
  with P13's cause, size without a stage, above the reserve, crop undeclared).
- `EdgeMapStageGpuParityTests.cpp` (Category GpuWarp: WARP in the CPU tier; on hardware with `VP_TEST_GPU=hardware`, GPU-QUIET;
  102b EDGEMAP-GPU-PARITY-TEST): the kEdgeMap owner fill on a libplacebo device, inputs through the memo's P13 views, against the
  CPU law: the stage variant's R = `SobelMagnitude` of `MeasureLumaCode` and G = that luma for SDR, linear, PQ and HLG (and R = the
  Sobel of the fill's own G), the default variant's R = `SobelMagnitude` of the 10-bit source luma; odd sizes (partial tiles).
  Written in CB slice 18 before any GPU run (code-only window): its first run is owed with the C5+A build.
- `RegionEvidenceSynthetic.h`: deterministic synthetic frames.
