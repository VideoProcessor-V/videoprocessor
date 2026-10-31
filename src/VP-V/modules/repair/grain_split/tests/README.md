# Tests for `repair.grain_split` (draft)

The CPU model is tested through its first reader: `src/VP-V/modules/repair/temporal_detail/tests/TemporalDetailModelTests.cpp`
(`GrainSplitAddBackIsExact`: Y = clean + G exactly, clean = base + f D1 + f D2, Wiener share in 0..1, determinism;
the grain-free transfer and the grain guard tests use the split through the model).
