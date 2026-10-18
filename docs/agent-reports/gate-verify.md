# Gate verify 2026-10-17 (test/gate-verify)

Adversarial check of the four gate-fix verdicts (fix/gate-1017, dc0b296b; tip 4523ea25).
Worktree `D:\codex\videoprocessor\gate-verify`, branch `test/gate-verify` from 4523ea25. Local only.
GPU: RTX 3050 Ti laptop (hardware runs only after the coordinator confirmed the GPU free; earlier
runs were WARP / CPU). Base = 1592d39d's shaders (`git show 1592d39d:shaders/...`) with the same
test DLL and the same libplacebo DLLs (360 stable, 377 Beta): the sigma-delta is the shader.

| # | Verdict | Result |
|---|---|---|
| 1 | Matrix 8-in-10: test assumption, no sigma-delta bias | CONFIRMED for the gate failure; a real snap bias found on the 7.360 rgba16f path and fixed |
| 2 | Mixed screen: brightness-only jump is a flash | CONFIRMED |
| 3 | Rebase layout: capture raced the crop behind a cold compile | CONFIRMED |
| 4 | Gap fill: gaps inside the cold-compile window | CONFIRMED |

## 1. Sigma-delta bias

New GPU test `SigmaDeltaFlatFieldMeanIsUnbiasedOnTheGpu` (probe `SigmaDeltaBiasProbe.h`, shared with
the Beta program, `VP_BETA_SIGMA_BIAS=1`). Flat 4x4 patches, 16 warm-up + 192 static refreshes:
family A = every 10-bit level 0..1023; family B = 21 (17 at 10 bits) codes x 32 offsets of 1/32 code,
made exactly representable in rgba16f. For a static pixel the loop's mean is within
2 x 0.65 / 192 = 0.0068 code of its input, so anything larger is bias. Modes: 8-in-10 limited (FL35),
8-in-10 full, 10-bit limited, 10-bit full; the sigma-delta alone and after the LED hook (+ repair on 7.360).

**Beta 377, output pass rgba32f (the deployed renderer): unbiased.** Branch and fix identical.
Every mode, alone and with the LED hook: family A exact (max 0.000 10-bit codes), family B
max |error| 0.014 10-bit codes (8-in-10) / 0.004 (10-bit), band means within +-0.003.

**Stable 7.360, rgba16f output pass: a real snap bias (base = branch, 1592d39d did not change it).**
NVIDIA and WARP gave identical numbers (both store into rgba16f by truncation). Sigma-delta alone,
errors in 10-bit codes (mean / max per band):

| mode, family B (off-grid) | base / branch | fix |
|---|---|---|
| 8-in-10 limited, near-black | +0.002 / 0.515 | +0.000 / 0.016 |
| 8-in-10 limited, mid | -0.007 / 0.504 | +0.016 / 0.504 |
| 8-in-10 limited, near-white | +0.040 / 0.510 | +0.050 / 0.489 |
| 8-in-10 full, near-black | +0.001 / 0.515 | +0.000 / 0.017 |
| 10-bit, near-black | +0.000 / 0.126 | +0.002 / 0.063 |
| family A (10-bit grid) 8-in-10 max | 1.000 (x.25 levels truncated into the window) | 0.500 |
| worst error / rgba16f step at the level | 6.44 | 0.97 |

Signed error by offset from the code (8-in-10 limited, base): an S-curve, +0.44 at -0.125 code,
-0.30 at +0.125, zero beyond +-0.19 code. Cause: `Temporal sigma-delta.glsl` snapped any value within
0.13 code of a whole code to that code, at every brightness (8 rgba16f steps near black). Fix: the
snap window is the store's own uncertainty at the value's octave, one step below (truncation) to half
a step above (rounding), floor 1e-3 code. The rest (max one store step, +-0.5 10-bit near white in
8-in-10) is the rgba16f texture's resolution; the Beta's rgba32f removes it. Assertions: loop error
away from codes <= 0.02 code (worst 0.0033), every level within 1.05 store steps (base 6.44, fix 0.97).

**What "vs shaping, signed -0.489" is:** the mean over the moving texture of (whole chain: repair,
LED hook, mask, sigma-delta) minus (the shaping hooks alone, no sigma-delta), in 10-bit codes. It is a
real negative offset in the 7.360 output, not a floor/round convention, and not the sigma-delta: with
the fixed snap it is unchanged (-0.489 / -0.490 / -0.460). The flat-field LED chain on rgba16f shows
the same offset (-0.23 to -0.48 mean in limited modes; 0 with the sigma-delta alone) and it is 0 on the
Beta's rgba32f output pass (also asserted by RunBetaChainBiasRegression). Code 900: three-frame patch
mean 974.34 vs reference 974.39; the test's patch-mean change is right.

## 2. Mixed screen / cut recall

Latch real-clip tests on D:\hfr: 6/6 passed (LatchFixesCalibration: fades/flashes 0 cuts, in-shot 0,
real cuts 1 missed of 7; FadesAndFlashes 7 of 7 real; look-ahead engages on the cut frame).
Real films (`RealFilmCutDecisions`, opt-in, `tools/score_film_cuts.py`): frames at the monitor's 0.5 s
spacing, each sample aligned to its source frame, cuts = scene-score spikes:

| film | real cuts | latch fixes | old tile-mean jump |
|---|---|---|---|
| Tears of Steel, last 3 min (live action part) | 9 | 8 (89 %) | 2 (22 %) |
| Meridian, first 7 min | 54 | 45 (83 %) | 8 (15 %) |

No miss was a similar composition differing mainly in brightness: every miss had decorrelation
0.78-0.90 with a level jump under 0.2 (shot/reverse-shot in Meridian's interrogation), i.e. below
the 0.9 "alone" threshold. Missing a cut only delays the engage (safe side). Steps flagged with no
source cut: ToS 8 (fast action / hidden cuts), Meridian 11 of 840 (lightning in shot, a figure leaving
frame) - a possible follow-up, not this verdict.

## 3. Rebase layout

Warm cache: the crop is applied at source frame 4-5 in native and 1080 alike (9 harness runs, captures
at frames 118-162). The gate-fix report's "re-base engages the crop ~70 frames later" was the cold
compile, not the re-base. Start-up: the first frame (uncropped) stays on screen through the renderer's
start-up stalls (226-694 ms render cycles with backlog flushes even with a warm cache): native shows
the crop by 1.0 s, 1080 by 1.5 s. A short start-up glitch, not a layout error once the crop lands.

## 4. Gap fill

Warm cache: early gaps (frames 90, 150-151) and late gaps were both repaired 2/2 in every run, auto and
standalone (interpolation off). The only failure was the first run of a build (28 compile lines, cold
cache). Production keeps its shader cache beside the exe, so early gaps fail only on the first launch
after an install or cache clear. Note: `interpolation_fill_gaps` still defaults to off.

## Runs
GPU chain gate (SigmaDelta|WholeHookChainCombinationMatrix|ExtendedRange) 48/48; WARP SigmaDelta,
OutputEncodingRepair and ExtendedRange 47/47; the 3 harness gate tests 3x 3/3; diagnostics opt-in with
`VP_TEST_GATE_VERIFY=1`.
