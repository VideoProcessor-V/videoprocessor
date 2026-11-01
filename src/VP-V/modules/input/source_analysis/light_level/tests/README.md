# Tests for `input.light_level` (draft)

Planned (written separately): settings read and fallback notes; LUT golden values against `tools/eval/tonemap_ref.py`
(HdrToSdr), BT.1886 and sRGB closed forms (Sdr); BTB codes (J = S = V = 0); Fetch index clamp and interpolation;
fingerprint stability and cache rebuild only on change; PackTexels layout; GLSL text contains the fetch and J functions.

WP2 (block product): `LightLevelBlocksTests.cpp` (CPU: LUT-driven stats, bars never change the surround, geometry sigma at 1080p/4K/8K,
one-pole and resets, every fallback mode and status, accessor, BTB/WTW, determinism, storage per tier, catalog version, python
golden) and `LightLevelBlocksWarpTests.cpp` (GpuWarp: stats pass with the LUT, block pass and accessor against the float mirrors).
