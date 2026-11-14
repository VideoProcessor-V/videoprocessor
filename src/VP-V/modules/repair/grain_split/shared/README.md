# BandFromBase (shared unit U2, `VpUnit::BandFromBase`)

The one separable-Gaussian band-split law of the repair stack (doc 62, section 1.2 U2). Header-only:
`BandFromBase.h`, version `kUnitVersion = 1`.

- `Radius(sigma, factor, minOne, eps)`: tap radius `ceil(factor sigma - eps)`, at least 1 when `minOne`.
- `Weight(k, sigma, r)`: normalised weight, the sum taken in index order `-r..r` (bit-identical to the private copies it replaced).
- `HalfSigma(sigma)`: `sqrt(sigma^2 - 0.25) / 2`, the half-resolution sigma of a full-resolution split.
- `kSigmaFine / kSigmaBase / kSigmaLow` (1 / 2.5 / 6 px at <= 2160 lines): the band sigma table, owned here; `GrainSplit::Params`
  defaults, `TextureRestore::kSigma1/2/3` and `DetailPulse::kSigmaFine` read it (WAIVER-B2-U2-UNITMOVE).
- `BandRadius(sigma)` / `BandWeight(k, sigma)`: the one band Gaussian of the full-resolution split, `Radius(sigma, 3, true, 0)`
  and `Weight()` of that radius (FIX-B2-ND03; `BandFromBaseUnitMoveTests` pins identity with the former private rules).
- `Blur(plane, w, h, sigma)`: CPU reference, edges replicated, rows across then columns down, fixed order (this is `GrainSplit::Gauss`).
- `Params{sigmaRel, activeHeight, ...}` and `Glsl(Params)`: sigma as a fraction of the active-picture height, emitting
  `vpu_band_from_base_*` (radius define, weight table from `Weight()`, across/down tap loops). Not wired into a pass yet.

Consumers migrated (identity, proven in their test files): GrainSplit CPU `Gauss` and `GrainSplit::Gpu::GaussRadius/GaussWeight`,
`DetailPulse::FineRadius/FineWeight`, `TextureRestore::Radius1/Weight1/HalfSigma/HalfRadius/HalfWeight`.
Python mirror: `tools/eval/units/band_from_base_ref.py`, pinned by `tests/BandFromBaseGolden.generated.h`
(`tools/eval/units/gen_band_from_base_golden.py`).

Determinism: fixed summation order, no threads, no atomics. Any change of the law or the GLSL entry names bumps `kUnitVersion`.

## Design goal, philosophy and decisions

- Goal: one separable-Gaussian law for every band split, so a band of the picture means the same thing in every module.
- The unit owns the law AND its parameters (the sigma table and the radius rule); consumers read them by name and never
  keep a private copy, so the GPU emitter, the CPU reference and every reader cannot drift apart.
- Moves into the unit are identity-only: values and expressions unchanged, proven bit for bit by a test, and the law
  version is bumped only when the law, the radius rule or the GLSL entry names change (adding the table did not).
- Grain constants (block edge, sub-blocks, calibration, kappa) are U1's (the grain split law, `GrainSplit::DefaultParams`),
  not this unit's.
