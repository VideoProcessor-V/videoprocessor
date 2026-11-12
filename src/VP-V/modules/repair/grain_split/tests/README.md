# Tests for `repair.grain_split` (draft)

The CPU model is tested through its first reader: `src/VP-V/modules/repair/temporal_detail/tests/TemporalDetailModelTests.cpp`
(`GrainSplitAddBackIsExact`: Y = clean + G exactly, clean = base + f D1 + f D2, Wiener share in 0..1, determinism;
the grain-free transfer and the grain guard tests use the split through the model).
The GPU passes: `src/VP-V/modules/repair/temporal_detail/tests/TemporalDetailGpuTests.cpp` (`GrainSplitGpuMatchesModel`, WARP).

`tests/GrainSplitV3Tests.cpp` (CPU, deterministic, refs/116 s4-s6): the v3 law (`GrainSplit::Compute`, low bands at half resolution)
against the v2 law (`GrainSplit::ComputeV2`, A/B baseline only). Grain-keep metric on paired frames S and S + Gt per region class
(flat, gradient, dark-flat, texture, edges; scales 1 and 2; white and film-like grain; 1 and 3 code RMS): keep, leak, v3 not worse than v2,
clean/N/e3 closeness, the law identity grain == (1 - f)(Y - base) to 1e-9 (both laws), the 4K lane law (scale 2) keeping grain in texture (>= 0.90) and film-like flat (>= 0.95), and the half-resolution law pieces. Open owner decision D4 rows are logged as 'D4 pending', not asserted.

`tests/GrainSplitV3GpuTests.cpp` (GPU on WARP, refs/116 s5): the unit-4 six-pass kernel in Full and Compact against `GrainSplit::Compute`
(odd and even planes, B 8 / 9 / 16 / 17 / 32, scale B / 16): |s1|, |s2|, |base| <= 0.05 code, f <= 1/255, N and e3 / e1 / e2 relative <= 1e-3;
determinism; `Step(0..4)` bit-identical to `Compute`; component 1 of an RG16 texture bit-identical to component 0 of an R16 texture
(`V3ComponentOneEqualsComponentZero`); and the consumer-level check (`V3P11RegionFlagsAgree`, CPU-only: the approved form of
WAIVER-P10V3-02; measured 99.75 % at B16 and 99.80 % at B32, disagreements only edgeContrast on the grain classes): the P11 region
classification from the v3 split against the v2 split agrees on >= 99.5 % of blocks (WAIVER-P10V3-02: band d3 has one live reader (P11);
TD will read the split later; re-check at the TD build).
`tests/GrainSplitV3CostBench.cpp` (category GpuHardware, logs only): per-pass pl_timer mean / p95 and total mean / p95 / max of the lane
kernel at 1080p and 2160p (Compact and Full, 20 warm-up + 120 timed frames) plus the dispatcher's live-form wall time.
