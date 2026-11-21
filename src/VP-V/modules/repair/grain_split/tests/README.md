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
determinism; `Step(0..5)` bit-identical to `Compute`; component 1 of an RG16 texture bit-identical to component 0 of an R16 texture
(`V3ComponentOneEqualsComponentZero`); and the consumer-level check (`V3P11RegionFlagsAgree`, CPU-only: the approved form of
WAIVER-P10V3-02; measured 99.75 % at B16 and 99.80 % at B32, disagreements only edgeContrast on the grain classes): the P11 region
classification from the v3 split against the v2 split agrees on >= 99.5 % of blocks (WAIVER-P10V3-02: band d3 has one live reader (P11);
TD will read the split later; re-check at the TD build).
`tests/GrainSplitV3CostBench.cpp` (category GpuHardware, logs only): per-pass pl_timer mean / p95 and total mean / p95 / max of the lane
kernel at 1080p and 2160p (Compact and Full, 20 warm-up + 120 timed frames) plus the dispatcher's live-form wall time.

`tests/GrainLevelLawTests.cpp` and `tests/GrainLevelDetailLossTests.cpp` (CPU only, GrainLevel CPU law v5, brief-S-3c T2): seconds-based half-life at six rates, bit-identical unit-4 fallback per reset/fallback cause with visible reason, histogram reset, skip-copy guard, temporal invariants (N_b <= N, f up, clean + grain = Y), mirror gate (default OFF, only lowers, OFF equals a gate that cannot fire), bicubic cross form phase bias at 0.3 / 1.7 px, DCT orthonormality, the B neighbour Wiener with 4 planted-grain neighbours, AR(1) 0.3 reporting, window sizes, cost rows, frozen-gain detail loss. Numeric thresholds are logged first-run values awaiting the first build.

`tests/GrainLevelShiftBench.cpp` (B-S0 measurement, synthetic frames, numbers logged only): 2 versus 4 DCT half-shift grids of the neighbour Wiener (error, structure kept, time) over K = 2/4/8/12 neighbours and three grain levels, and 8x8 versus 16x16 block-field warp error on affine motions. `Params::dctShifts` (default 4, bit-identical) is a measurement knob, not a user setting.

`tests/GrainLevelRepeatGateTests.cpp` (P10g C8 repeated-frame gate, new tests only): a repeat leaves the B set, independent grain never fires (0 false fires at three grain levels), a frozen picture takes the L1+L2 gains exactly, `grain_sep_repeat_gate` / `grain_sep_repeat_threshold` parse with visible problem texts, the `grain_sep.repeat_gate_off` fallback entry.

`tests/GrainLevelRepeatFactsTests.cpp` (C8 with the per-neighbour RepeatFact tri-state, new file): the block test is primary (a Repeat fact only adds a drop, a Distinct fact never vetoes the test), frozen-12 and dup2 rows on both paths (facts=Repeat, facts=Unknown), still and aerial-crop rows with 0 false fires on both paths, threshold clamp (0.05 rejected / clamped), gate off drops nothing and engages `grain_sep.repeat_gate_off`, the "repeat facts unavailable: block test" status text (no registry entry, so no one-time notice).

`tests/GrainLevelFillTests.cpp` (P10g T3, CPU tier, new file): the kP10gGrainLevel owner fill (`GrainLevelGpu.h`). The reserve is the declared block grid (RGBA32F per 16-px block, ceil) and equals the texture the fill makes. The fill is Derived, frame-scoped, default variant, and registered under the catalog owner `repair/grain_split`. The module registers the P10 variant owner once and P10g at the largest live size, re-registers P10g on a size change only, and does not retry a refused registration every frame. Visible, counted declines: no GPU context, no P13 source plane, the P10 split unavailable (never a private split). The Fallbacks texts (the routed lines, review-cb-p10g-r1 MUST-1) name the single-frame law, the declines with the last reason and count (also from the module's own fill), and a refused P10g or P10 owner registration in words; an AlreadyRegistered P10 owner (15b A1) is benign (served, no line, never retried). The GPU parity check (fill vs the CPU blocks and the f^2 block mean) belongs to the T5 GPU run.
