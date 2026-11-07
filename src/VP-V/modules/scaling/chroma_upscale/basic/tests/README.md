# Tests for `scaling/chroma_upscale/basic`

`BasicChromaTests.cpp` (VideoProcessor-Test): `BasicChromaModelTests` (CPU): phase tables sum exactly and respect the int32 bound; position logic
against double math for every factor and siting; co-sited outputs equal the input exactly; siting honoured on a ramp; mirror symmetry; the kernel
against a double-precision reference (worst error logged, <= 4 codes); flat exact, BTB / WTW overshoot kept; bars flat and no read across the picture
boundary; composed 12-bit IPT 4:2:2; refused geometries; the engine read path inert / visible. `BasicChromaGpuTests` (GpuWarp): the GLSL pass is
BYTE-identical to the CPU model for 9 geometries (4:2:0 left / top-left / centre, 4:2:2 IPT, factor 4, odd size, letterbox, extremes, one-axis),
refuses what the CPU refuses, and the GLSL arithmetic is integer only.
