# Scaling-chain estimator (`input.source_analysis (identity/chain/)`, SID-C1)

Pure units that find out from the picture lines alone whether, and from which size, the delivered picture was upscaled by the player, and with which kernel (doc 78 sections 2-4). Header-only C++14, deterministic, not wired yet: SID-C2 adds the worker, the P14g record and the consensus voter; the latch state machine is NOT here, these units return evidence and reason bits.

Unit: input/source_analysis (identity/chain)

## Settings

No keys: the estimator is not user-configurable. Its hidden tunables are listed under "Tuning (not in Config)" below.

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| (none) | | | |

## Products

None yet. P14g `source_geometry` and the consensus voter come in SID-C2; these units return `VerifyResult`, `EvidenceUpdate` / `AxisSummary` and `ProbeResult` with reason bits (`kReason*` in `ChainCommon.h`).

## Status and fallbacks

Every abstention carries reason bits that C2 turns into the Ctrl+I line, the Config status and Ctrl+Alt+D: `LowDetail`, `Flat`, `Dark`, `Soft`, `NoPower`, `Mixed` (HOLD hint), `Masked`, `NoCandidates`, `ShortLines`, `NoReference`, `CoarseOnly`, `BadInput`. A native verdict is never produced without power, and a flat, dark or soft picture is UNSURE, never native.

## Tests

`identity/tests/ChainUnitTests.cpp` (CPU only, synthetic only: a 1/f-like scene from value-noise octaves in the ratio 0.59 plus grain, fixed-seed LCG): `ChainKernelBankTests`, `ChainLineSamplerTests`, `ChainVerifierTests`, `ChainProposerTests`, `ChainEvidenceTests`, `ChainProbeTests`, `ChainTimingTests` (logs milliseconds for ONE evaluation at 3840x2160 with 1 and 3 candidates, cold and warm; no timing assertion). Chains: 1/2 with every kernel family, 2/3, 1/3, 5/12, 8- and 10-bit rounding after the upscale, USM 0.6, double scaling 1/3 to 1/2, letterbox; the loop-night case (textured 8-bit 1/2 bilinear and bicubic reach c1 within 15 evaluations); natives at several detail levels including an 8-px blocky one; flat frames abstain; the probe is seen on textured pictures and unseen on flat, soft and dark ones; two runs, other candidate order and a warm or tiny cache give identical bytes. Run: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:"FullyQualifiedName~ChainKernelBankTests|FullyQualifiedName~ChainLineSamplerTests|FullyQualifiedName~ChainVerifierTests|FullyQualifiedName~ChainProposerTests|FullyQualifiedName~ChainEvidenceTests|FullyQualifiedName~ChainProbeTests|FullyQualifiedName~ChainTimingTests" /Logger:trx`. No GPU. The class set builds about 25 synthetic 1080p scenes and runs about 100 analyses: allow a few minutes in Release.

## Tuning (not in Config)

Every tunable is a member of a settings struct with its default in the header (hidden tuning, doc 78 section 7). The Key column is the planned calibration name (`chain_` + snake case of the member; the next column names the struct member). Nothing reads them from a config file yet, so they are deliberately not listed under Settings.

| Key | Struct member | Type | Range / values | Default | Fallback / budget role |
|---|---|---|---|---|---|
| chain_rows | SamplerSettings::rows | int | 0..64 | 32 (16 via SamplerSettings::Latched() while a value is latched) | fewer lines = less cost and less power; plan invalid below chain_min_run; tiers: budget: C2 may lower to 16 (F0: margin kept down to 16) |
| chain_cols | SamplerSettings::cols | int | 0..64 | 32 | same as chain_rows; tiers: same as chain_rows |
| chain_margin | SamplerSettings::margin | int px | 0.. | 48 | bars/edges stay out of the lines; tiers: same on all |
| chain_min_run | SamplerSettings::minRun | int px | 16.. | 64 | inner extent below this: plan invalid (kReasonShortLines); tiers: same on all |
| chain_coarse_half_width | VerifierSettings::coarseHalfWidth | int | 0..16 | 4 | coarse stage scans n +- this; tiers: same on all |
| chain_coarse_step | VerifierSettings::coarseStep | int | 1..8 | 1 | a short-window notch is only a few integers wide: step 1 (doc 78 said 4; measured, see Method); tiers: same on all |
| chain_coarse_lines | VerifierSettings::coarseLines | int | 4..64 | 8 | lines of the coarse stage (it only refines and ranks candidates; F0: the size is right down to 8 lines); tiers: budget-governed |
| chain_coarse_window_fraction | VerifierSettings::coarseWindowFraction | float | 0.05..0.5 | 0.125 | coarse window = N x this; tiers: same on all |
| chain_coarse_min_window | VerifierSettings::coarseMinWindow | int px | 64.. | 160 | lower bound of the coarse window; tiers: same on all |
| chain_exact_half_width | VerifierSettings::exactHalfWidth | int | 0..8 | 1 | exact stage scans the refined n +- this (the coarse stage already centres it); tiers: same on all |
| chain_max_exact_candidates | VerifierSettings::maxExactCandidates | int | 1..8 | 2 | candidates promoted to full lines; the rest are reported coarse-only (kReasonCoarseOnly); tiers: budget-governed (1 when latched with power) |
| chain_exact_families | VerifierSettings::exactFamilies | int | 1..4 | 2 | kernel families evaluated in full at the best integer; tiers: budget-governed |
| chain_ref_fraction_1 | VerifierSettings::refFraction1 | float | 0.02..0.1 | 0.04 | baseline sizes n(1 +- f1), n(1 +- f2); fewer than two valid = kReasonNoReference; tiers: same on all |
| chain_ref_fraction_2 | VerifierSettings::refFraction2 | float | 0.04..0.12 | 0.08 | see chain_ref_fraction_1; tiers: same on all |
| chain_trim_fraction | VerifierSettings::trimFraction | float | 0..0.4 | 0.2 | trimmed mean of the per-line values; tiers: same on all |
| chain_eps_codes | VerifierSettings::epsCodes | float codes | 1e-4..0.5 | 0.02 | numerical floor added in quadrature to a line rms; tiers: same on all |
| chain_min_latent | VerifierSettings::minLatent | int | 2.. | 16 | smaller candidate sizes are rejected (kReasonBadInput); tiers: same on all |
| chain_max_latent_ratio | VerifierSettings::maxLatentRatio | float | 0.9..0.999 | 0.985 | n/N above this is not an upscale; tiers: same on all |
| chain_phase_count | VerifierSettings::phaseCount | int (odd) | 1..7 | 1 | sub-sample phases tried at the best integer (1 = centre aligned only); tiers: budget-governed |
| chain_phase_step | VerifierSettings::phaseStep | float | 0.05..0.5 | 0.25 | latent samples between phases; tiers: same on all |
| chain_cache_limit | VerifierSettings::cacheLimit | int | 1..2048 | 512 | operators kept, oldest evicted first (time only, never changes a result); about 70 MB at 3840x2160 with 3 candidates per axis; tiers: budget: memory about 30-400 KB per operator |
| chain_q_max | ProposerSettings::qMax | int | 2..32 | 16 | longest phase period tested; ratios needing a longer period come from the caller candidates; tiers: budget-governed (cost grows with q_max) |
| chain_proposer_lines | ProposerSettings::maxLines | int | 1..64 | 16 | lines of a LineSet used per proposer evaluation; tiers: budget-governed |
| chain_ratio_min | ProposerSettings::ratioMin | float | 2..20 | 5 | per-degree-of-freedom gain a period must reach (natives measured below 3.6); tiers: same on all |
| chain_max_periods | ProposerSettings::maxPeriods | int | 1..8 | 3 | periods accepted per axis; tiers: same on all |
| chain_min_n_ratio | ProposerSettings::minNRatio | float | 0.05..0.5 | 0.2 | smallest candidate n/N; tiers: same on all |
| chain_max_n_ratio | ProposerSettings::maxNRatio | float | 0.9..0.999 | 0.985 | largest candidate n/N; tiers: same on all |
| chain_window_evaluations | ProposerSettings::windowEvaluations | int | 2..1000 | 16 | statistics are halved beyond this many evaluations; tiers: same on all |
| chain_min_samples | ProposerSettings::minSamples | int | 100.. | 2000 | predicted samples needed before a period is reported; tiers: same on all |
| chain_caller_slots | ProposerSettings::callerSlots | int | 0..8 | 4 | caller-supplied sizes kept; tiers: same on all |
| chain_proposer_min_latent | ProposerSettings::minLatent | int | 2.. | 16 | caller sizes below this are dropped; tiers: same on all |
| chain_target_rate_per_hour | EvidenceSettings::targetRatePerHour | float | 1e-6..1 | 0.01 | confident-wrong target (1 per 100 title-hours); sets c1; tiers: same on all |
| chain_threshold_interval_s | EvidenceSettings::thresholdIntervalSec | float s | 1..60 | 10 | T in alpha = rate x T / 3600; tiers: same on all |
| chain_candidate_count | EvidenceSettings::candidateCount | int | 1..64 | 8 | candidates searched (multiplicity of c1); tiers: same on all |
| chain_kernel_count | EvidenceSettings::kernelCount | int | 1..64 | 10 | kernels searched (multiplicity of c1); tiers: same on all |
| chain_c2_extra_nats | EvidenceSettings::c2ExtraNats | float | 0..10 | 3 | c2 = c1 + this (contrary evidence needed to leave a latched value); tiers: same on all |
| chain_shot_cap_fraction | EvidenceSettings::shotCapFraction | float | 0.1..1 | 0.5 | one shot summed evidence is capped at this x c1; tiers: same on all |
| chain_agreement_bonus | EvidenceSettings::agreementBonus | float nats | 0..5 | 2 | two-axis agreement prior (+ when ratios agree, - when both are positive and disagree, 0 when one axis abstains); tiers: same on all |
| chain_agreement_tolerance | EvidenceSettings::agreementTolerance | float | 0.001..0.1 | 0.015 | relative n/N difference counted as agreeing; tiers: same on all |
| chain_line_gate_factor | EvidenceSettings::lineGateFactor | float | 1..10 | 1.5 | a line is informative when its detail baseline >= this x the rounding/dither floor (a cliff: 3 x abstains on every 8-bit output, so 1.5); tiers: same on all |
| chain_min_gate_fraction | EvidenceSettings::minGateFraction | float | 0.05..1 | 0.25 | informative lines needed (8 of 32; floor chain_min_gate_lines_floor); tiers: same on all |
| chain_min_gate_lines_floor | EvidenceSettings::minGateLinesFloor | int | 1..16 | 3 | absolute minimum of informative lines; tiers: same on all |
| chain_mask_limit | EvidenceSettings::maskLimit | float | 0.05..0.5 | 0.20 | contradicting lines/segments: masked below, HOLD hint (kReasonMixed) above; tiers: same on all |
| chain_contradiction_min | EvidenceSettings::contradictionMin | float ln units | 0.05..1 | 0.15 | smallest distance from the median that contradicts the majority; tiers: same on all |
| chain_majority_dip | EvidenceSettings::majorityDip | float | -0.2..0 | -0.03 | median below this = the majority sees a dip; tiers: same on all |
| chain_increment_max | EvidenceSettings::incrementMax | float nats | 1..20 | 5 | clamp of one evaluation increment; tiers: same on all |
| chain_increment_min | EvidenceSettings::incrementMin | float nats | -20..0 | -3 | clamp of one evaluation increment; tiers: same on all |
| chain_native_max | EvidenceSettings::nativeMax | float nats | 0..10 | 2 | native increment clamp (only with proven power); tiers: same on all |
| chain_native_min | EvidenceSettings::nativeMin | float nats | -10..0 | -3 | native increment clamp; tiers: same on all |
| chain_native_no_candidate | EvidenceSettings::nativeNoCandidate | float nats | 0..5 | 0.5 | native increment when the probe saw its injection and there was nothing to verify; tiers: same on all |
| chain_evidence_trim_fraction | EvidenceSettings::trimFraction | float | 0..0.4 | 0.2 | trimmed mean over the unmasked lines; tiers: same on all |
| chain_max_hypotheses | EvidenceSettings::maxHypotheses | int | 2..64 | 16 | hypotheses kept per axis (weakest replaced); tiers: same on all |
| chain_probe_ratio | ProbeSettings::ratio | float | 0.3..0.9 | 0.61 | probe latent/synthetic ratio; off every simple rational and k/8, k/16; tiers: same on all |
| chain_probe_kernel | ProbeSettings::kernel | int (KernelBank index) | 0..9 | 1 | probe kernel: bicubic B=0, C=0.5; tiers: same on all |
| chain_probe_seen_threshold | ProbeSettings::seenThreshold | float | -1..-0.02 | -0.15 | statistic at or below this = the injected upscale was seen; tiers: same on all |
| chain_probe_dark_mean_codes | ProbeSettings::darkMeanCodes | float codes | 0..400 | 100 | mean line level below this names the reason dark; tiers: same on all |
| chain_probe_flat_std_codes | ProbeSettings::flatStdCodes | float codes | 0..50 | 3 | median line std below this names flat (else soft); tiers: same on all |
| chain_probe_line_gate_factor | ProbeSettings::lineGateFactor | float | 1..10 | 1.5 | gate as the evidence stage; tiers: same on all |
| chain_probe_min_gate_fraction | ProbeSettings::minGateFraction | float | 0.05..1 | 0.25 | gate as the evidence stage; tiers: same on all |
| chain_probe_min_gate_lines_floor | ProbeSettings::minGateLinesFloor | int | 1..16 | 3 | gate as the evidence stage; tiers: same on all |
| chain_probe_trim_fraction | ProbeSettings::trimFraction | float | 0..0.4 | 0.2 | trimmed mean of the probe statistic; tiers: same on all |
| chain_probe_max_lines | ProbeSettings::maxLines | int | 4..64 | 16 | lines the probe uses, evenly spread (F0: the size is right down to 8 lines); tiers: budget-governed |

## Files

| File | Unit | Content |
|---|---|---|
| `ChainCommon.h` | shared | constants, reason bits, `LineSet`, deterministic helpers (median, trimmed mean, hash) |
| `LineSampler.h` | 1 | `LineSampler::Plan(active, seq)`: 32 rows + 32 columns (`SamplerSettings::Latched()`: 16 + 16), one per stratum of the inner area (48 px from the bars), position from an integer hash of the sequence number; `Fill` copies one axis out of a plane into a `LineSet` in 10-bit code units; the `LineSet` carries the geometry of the scaler axis (`axisLen`, `origin`), by default the active rectangle, or the whole frame when the bars were part of the raster the player scaled (`scalerRect`). It skips nothing: the caller skips repeats and blanks |
| `PolyphaseProposer.h` | 2 | `PolyphaseProposer`: cheap candidate sizes per axis, `AddEvaluation`, `Periods`, `AllScores`, `Propose` (at most 8, caller sizes first, then round robin over the periods in strength order), `Reset` |
| `KernelBank.h` | 3a | 10 kernels (bilinear, bicubic B/C 0/0.5, 0/0.6, 1/3 1/3, lanczos2/3/4, spline16/36/64), `TapWeights`, `DescaleOperator` (banded Cholesky with reciprocal diagonal, memoised tap weights, per-256-px segment sums) |
| `DescaleVerifier.h` | 3b | `DescaleVerifier::Evaluate`: coarse scan, exact stage, kernel and family margins, floor, per-line values, segment map; operator cache keyed by (N, n, kernel, phase step, window) |
| `ChainNullTables.h` | 4 | `NullTables` (placeholder, conservative), `ClassifyLatent` (k/8, k/16, rationals <= 6, generic) |
| `ChainEvidence.h` | 4 | `ChainEvidence`: line gate, spatial agreement, LLR increments, clustering, per-shot cap, two-axis prior, thresholds c1/c2 |
| `PowerProbe.h` | 5 | `PowerProbe::Run`: synthetic upscale of the sampled lines at ratio 0.61, "seen" / "unseen" with reason; give it a verifier instance of its own |

## Method (analysis by synthesis)

For a candidate original size n of an N-pixel axis and a kernel k, every sampled line is shrunk back to n samples by least squares ("descale") and blown up again with the same kernel. If the picture really was upscaled from n by k, almost nothing is lost (rounding noise only); a native picture always loses its finest detail. Per line the statistic is `v = ln(s(n,k) / baseline)`, `s = sqrt((ms + eps^2) / dof)` with `ms` the mean square residual over the inner rows and `dof = 1 - nodes/rows` (the unknowns of the fit; natives then sit at 0), the baseline being the median of `s` at the reference sizes n(1 +- 4%), n(1 +- 8%) (smallest over the four representative kernels). The candidate statistic `T` is the 20%-trimmed mean of `v` over lines (negative = a dip at n). Mapping, centre aligned: `p = (i + 0.5) n / N - 0.5 + phase`.

### Proposer: polyphase predictor gain (deviation from doc 78 section 3, accepted by the lead)

Doc 78 asked for the periodogram of the squared residual of ONE pooled predictor. On synthetic chains with the final rounding that statistic did not work: for the commonest case, a centre-aligned 2x upscale, the two phases are mirror images and a pooled (symmetric) predictor has the SAME residual variance at both, so e^2 shows no periodicity at all (it also vanished for 3x, where the residual is pure rounding noise); a causal 5-tap predictor saw 1/2 and 2/3 but only weakly 1/3 and 5/12. The proposer therefore fits the 5-tap predictor PER PHASE CLASS (period q = 2..16, phase = pixel index mod q, neighbours -3..+2, line mean removed) and compares the summed residual with the pooled one: `gain = N ln(RSS_pooled/RSS_q)`, `ratio = gain / (5(q-1))`, `dBIC = gain - 5(q-1) ln N`. On the synthetic prototype natives stayed below ratio 3.6 and chains reached 15-2700; `ratioMin` is 5 and the test logs the largest native ratio and asserts it stays below 4.5. A period q says the ratio is m/q with m coprime to q; every such n = round(N m / q) is a candidate (nearest 1/2 first inside a period). The slots are filled by strength: periods in dBIC order, one size per period per round, after the caller's sizes. `lowVarPhase` reports the phase class with the smallest residual variance. Only sufficient statistics are kept, so evaluations accumulate at no extra cost (the phase class of a sample is a rolling counter, no division per sample). Limits: a ratio needing a period above `qMax`, or an N m / q that is not an integer (the phase then drifts along the line), is only found through the caller candidates (previous latched n, rationals of the active size).

### Verifier (deviations from doc 78 section 3, measured; accepted by the lead)

- The coarse scan is NOT step 4: on a window of N/8 rows at the START or END of the lines the notch is alias free and about +-1.5 integers wide (a centred window shows a comb of aliases at every even offset), so it scans every integer within +-4 (step 1) on both windows. It refines the candidate and ranks candidates.
- The exact stage uses the full lines (or `VerifyOptions::exactLines`, 16 when latched): integers n^ +-1 with the representative kernels, then at the best integer every kernel of the best two families (optionally at several sub-sample phases). Only the best `maxExactCandidates` (2) candidates AND the caller's latched size (`forceExactN`) get it; the rest are reported coarse-only. A kernel is judged by its T against the SAME baseline (the smallest residual wins). At exactly 2x bilinear and bicubic are nearly indistinguishable (F0 saw the same); tests accept either there.
- All kernels use the same inner rows (margin 1.5 supports of a radius-4 kernel at each window end) so they compare like for like. Phases are quantised to 1/1024 before an operator is built, tap weights to 2^-30, so the cache never changes a result.
- `CandidateResult::floorCodes` is the RAW rms residual at the best (n, kernel); `AxisObservation::floorCodes` (the gate input) is the dof-normalised rounding/dither sigma, 1.155 for an 8-bit output and 0.289 for 10-bit. They are different quantities.

### Evidence

Per candidate: the line gate (baseline >= 1.5 x the rounding/dither floor on at least 8 of 32 lines, else `kReasonLowDetail`; the gate is a cliff because baselines of one picture cluster within about 8%, and 3 x demanded more detail than any 8-bit output of this synthetic content has), spatial agreement (lines at a robust distance from the median in the opposite direction are masked below 20%, above it the candidate is mixed; the per-256-px segment map is judged the same way along the line, informative segments only). Only the TOP hypothesis (the informative candidate with the lowest median line value) can hold the evaluation: if it is mixed the evaluation is a HOLD hint `kReasonMixed` and nothing accumulates; a wrong candidate that is mixed is just dropped. Then the LLR increment of the masked T from the `NullTables` class of n, clamped to [-3, +5]. Exact sizes within +-1 of each other are one hypothesis (the best per evaluation counts; the hypothesis value follows the latest best exact size), so refinement jitter cannot split the evidence. The native hypothesis only earns evidence when the power probe SAW its injection (otherwise `kReasonNoPower` and 0). Per shot (id from the caller, bumped at every cut) the summed increments are capped at c1/2, so c1 needs at least two saturated shots (two exactly reach it). Two axes: when both best hypotheses are positive, agreeing ratios add +2 nats to each and disagreeing ones subtract 2; a silent axis changes nothing. `c1 = -ln(alpha) + ln(candidates x kernels)`, `alpha = rate/hour x 10 s / 3600` (about 14.9 for 1 per 100 h), `c2 = c1 + 3`.

NullTables are PLACEHOLDERS (conservative: wide and deep nulls where natives are known to dip, a wide shallow alternative, clamped increments). They can only make the estimator abstain more than calibrated tables would; the calibration run writes `NullTables.generated.h` (same struct). With them a decisive chain reaches c1 within a few evaluations over two to three shots, a weak grainy dip never does.

### Power probe

The sampled lines are upscaled at ratio 0.61 (bicubic, rounded to the delivered output step) and the verifier is run on that with the known latent size N. Seen (T <= -0.15 on the informative lines) = a "native" verdict of the real candidates is positive evidence. Unseen = abstain with the reason: dark (mean level), flat (almost no line structure), soft (structure but no fine detail above the floor). A really upscaled picture is usually unseen by the probe (its detail stops at the latent Nyquist): that is fine, its dip evidence needs no probe.

### Cost (estimate, NOT measured; one thread, 3840x2160, +-2x; the timing test logs the real numbers)

One full line solve is about 40 us at 3744 samples. Per evaluation (both axes), coarse stage on 8 lines and probe on 16 lines: latched, one candidate, exact stage only on 16 lines: about 63 ms cold and 40 ms warm (operator cache) including the probe; acquiring, 3 candidates, exact stage on 32 lines: about 270 ms cold and 190 ms warm (proposer 16, coarse 3 x 9 x 2, exact 2 x (60 + 33), probe 16). The result reports its own cost: `VerifyResult::costMs`, `cold`, `operatorBuilds`, `solves` (and `ProbeResult::costMs`, `cold`), with the wall time read through the optional `VerifierSettings::nowMs` hook (the units never read a clock; C2 passes steady_clock). A CPU-share governor in C2 sets the interval to max(nominal, cost / share budget): at 15% while acquiring that is about 1.3 to 1.8 s, at 2% latched about 2 to 3 s, shown in Ctrl+Alt+D. Cache memory is about 70 MB at 3 candidates per axis (`cacheLimit` 512).
