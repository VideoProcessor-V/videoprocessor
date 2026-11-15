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
  `BusyEdge::SobelMagnitude` is owed before the first reader row (102b KEDGEMAP-RULE3).
- `RegionEvidenceSynthetic.h`: deterministic synthetic frames.
