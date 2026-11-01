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
