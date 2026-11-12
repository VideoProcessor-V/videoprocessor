# Tests for `repair.grain_split` (draft)

The CPU model is tested through its first reader: `src/VP-V/modules/repair/temporal_detail/tests/TemporalDetailModelTests.cpp`
(`GrainSplitAddBackIsExact`: Y = clean + G exactly, clean = base + f D1 + f D2, Wiener share in 0..1, determinism;
the grain-free transfer and the grain guard tests use the split through the model).
The GPU passes: `src/VP-V/modules/repair/temporal_detail/tests/TemporalDetailGpuTests.cpp` (`GrainSplitGpuMatchesModel`, WARP).

`tests/GrainSplitV3Tests.cpp` (CPU, deterministic, refs/116 s4-s6): the v3 law (`GrainSplit::Compute`, low bands at half resolution)
against the v2 law (`GrainSplit::ComputeV2`, A/B baseline only). Grain-keep metric on paired frames S and S + Gt per region class
(flat, gradient, dark-flat, texture, edges; scales 1 and 2; white and film-like grain; 1 and 3 code RMS): keep, leak, v3 not worse than v2,
clean/N/e3 closeness, exact re-add, and the half-resolution law pieces. Open owner decision D4 rows are logged as 'D4 pending', not asserted.
