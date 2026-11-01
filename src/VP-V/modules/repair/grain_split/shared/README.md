# BandFromBase (shared unit U2, `VpUnit::BandFromBase`)

The one separable-Gaussian band-split law of the repair stack (doc 62, section 1.2 U2). Header-only:
`BandFromBase.h`, version `kUnitVersion = 1`.

- `Radius(sigma, factor, minOne, eps)`: tap radius `ceil(factor sigma - eps)`, at least 1 when `minOne`.
- `Weight(k, sigma, r)`: normalised weight, the sum taken in index order `-r..r` (bit-identical to the private copies it replaced).
- `HalfSigma(sigma)`: `sqrt(sigma^2 - 0.25) / 2`, the half-resolution sigma of a full-resolution split.
- `Blur(plane, w, h, sigma)`: CPU reference, edges replicated, rows across then columns down, fixed order (this is `GrainSplit::Gauss`).
- `Params{sigmaRel, activeHeight, ...}` and `Glsl(Params)`: sigma as a fraction of the active-picture height, emitting
  `vpu_band_from_base_*` (radius define, weight table from `Weight()`, across/down tap loops). Not wired into a pass yet.

Consumers migrated (identity, proven in their test files): GrainSplit CPU `Gauss` and `GrainSplit::Gpu::GaussRadius/GaussWeight`,
`DetailPulse::FineRadius/FineWeight`, `TextureRestore::Radius1/Weight1/HalfSigma/HalfRadius/HalfWeight`.
Python mirror: `tools/eval/units/band_from_base_ref.py`, pinned by `tests/BandFromBaseGolden.generated.h`
(`tools/eval/units/gen_band_from_base_golden.py`).

Determinism: fixed summation order, no threads, no atomics. Any change of the law or the GLSL entry names bumps `kUnitVersion`.
