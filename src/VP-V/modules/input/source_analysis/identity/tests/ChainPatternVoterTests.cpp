/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team adversarial synthetic-physics unit tests for ChainPatternVoter (SID-E, Stage S5).
// Pure CPU C++14, synthetic only, deterministic SplitMix64 PRNG, zero clocks/sleeps.
// Every cap/ambiguity/detection invariant includes an in-test naive/unprotected negative control
// executed on the exact same synthetic input (Rule 3).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainPatternVoter.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace PatternVoterFx
	{
		using namespace SourceIdentity::Chain;

		constexpr double kFloor10Bit = 0.28867513459481288; // 1.0 / sqrt(12.0)

		inline PatternVoterSettings Enabled()
		{
			PatternVoterSettings s;
			s.enabled = true;
			return s;
		}

		// Deterministic SplitMix64 PRNG.
		struct DetRng
		{
			uint64_t state;
			explicit DetRng(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double UniformCentered()
			{
				const double u = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				return u - 0.5;
			}

			double Normal01()
			{
				double u1 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				const double u2 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				if (u1 < 1e-12)
					u1 = 1e-12;
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
			}
		};

		// Exact 1D upscale using KernelBank centre-aligned mapping.
		inline std::vector<double> Upscale1D(const std::vector<double>& src, int N, int kernel)
		{
			const int n = static_cast<int>(src.size());
			const int r = Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> out(static_cast<size_t>(N), 0.0);
			double w[8] = {};
			for (int i = 0; i < N; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(n) / static_cast<double>(N) - 0.5;
				const double fl = std::floor(p);
				TapWeights(kernel, p - fl, w);
				double acc = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - r + 1 + s;
					if (c < 0) c = 0;
					else if (c >= n) c = n - 1;
					acc += w[s] * src[static_cast<size_t>(c)];
				}
				out[static_cast<size_t>(i)] = acc;
			}
			return out;
		}

		// Builds a synthetic LineSet containing genuine codec block-boundary steps of exact period `periodPx`
		// and exact phase `phasePx` (discontinuity between pixel i-1 and pixel i at i % periodPx == phasePx).
		// Uses independent random block DC levels so the ONLY true periodicity is `periodPx`.
		inline LineSet MakeNativeBlockGridLines(int N, int lines, int periodPx, int phasePx, DetRng& rng)
		{
			LineSet ls;
			ls.axis = 0;
			ls.axisLen = N;
			ls.origin = 0;
			ls.Resize(lines, N);
			for (int li = 0; li < lines; ++li)
			{
				const int numBlocks = (N / periodPx) + 3;
				std::vector<double> levels(static_cast<size_t>(numBlocks), 512.0);
				for (int b = 0; b < numBlocks; ++b)
					levels[static_cast<size_t>(b)] = ClampD(512.0 + 55.0 * rng.Normal01(), 120.0, 900.0);

				double* y = ls.Line(li);
				for (int i = 0; i < N; ++i)
				{
					const int shifted = i - phasePx;
					const int b = (shifted >= 0) ? (shifted / periodPx + 1) : 0;
					const double smooth = 4.0 * std::sin(2.0 * kPi * static_cast<double>(i) / 137.0 + static_cast<double>(li));
					const double v = levels[static_cast<size_t>(b)] + smooth + 0.5 * rng.Normal01();
					y[i] = ClampD(std::floor(v + 0.5), 0.0, 1023.0);
				}
			}
			return ls;
		}

		// Builds a synthetic 720p -> 2160p (3:1 Lanczos3) LineSet from an 8-px blocked latent source,
		// yielding a true stretched 24-px block grid at exact phase `phasePx` (0 <= phasePx < 24).
		inline LineSet MakeScaled720To2160GridLines(int N, int nLatent, int lines, int phasePx, DetRng& rng)
		{
			constexpr int kLanczos3 = 5;
			constexpr int kSourceBlock = 8;
			const int extraLatent = 16; // margin for clean phase shift without edge artifacts
			const int fullLatent = nLatent + extraLatent;
			const int fullOut = (fullLatent * N) / nLatent;

			LineSet ls;
			ls.axis = 0;
			ls.axisLen = N;
			ls.origin = 0;
			ls.Resize(lines, N);

			for (int li = 0; li < lines; ++li)
			{
				std::vector<double> lat(static_cast<size_t>(fullLatent), 512.0);
				const int numBlocks = (fullLatent / kSourceBlock) + 2;
				std::vector<double> levels(static_cast<size_t>(numBlocks), 512.0);
				for (int b = 0; b < numBlocks; ++b)
					levels[static_cast<size_t>(b)] = ClampD(512.0 + 70.0 * rng.Normal01(), 120.0, 900.0);

				for (int k = 0; k < fullLatent; ++k)
				{
					const int b = k / kSourceBlock;
					lat[static_cast<size_t>(k)] = levels[static_cast<size_t>(b)] + 0.4 * rng.Normal01();
				}

				const std::vector<double> up = Upscale1D(lat, fullOut, kLanczos3);
				// In `up`, block boundaries sit at output indices 24*m.
				// Reading from `start = 24 - phasePx` places boundaries at output indices i % 24 == phasePx!
				const int periodOut = (kSourceBlock * N) / nLatent; // 24
				const int pNorm = ((phasePx % periodOut) + periodOut) % periodOut;
				const int start = (pNorm == 0) ? periodOut : (periodOut - pNorm);
				double* y = ls.Line(li);
				for (int i = 0; i < N; ++i)
					y[i] = ClampD(std::floor(up[static_cast<size_t>(start + i)] + 0.5), 0.0, 1023.0);
			}
			return ls;
		}
	}

	TEST_CLASS(ChainPatternVoterTests)
	{
	public:
		TEST_METHOD(DefaultOffAndP15MissingOrWrongVersionAbstainVisibly)
		{
			using namespace PatternVoterFx;

			// Default settings must be OFF and visibly report Disabled.
			PatternVoterSettings def;
			Assert::IsFalse(def.enabled);
			Assert::AreEqual(kMaxBlockGridVoteNats, def.maxVoteNats, 1e-12);

			PatternVoterCounters counters;
			const PatternConsumeResult cOff = ConsumeQualityPattern(nullptr, def, &counters);
			Assert::IsFalse(cOff.available);
			Assert::IsTrue(cOff.reason == PatternVoterReason::Disabled);
			Assert::IsNotNull(PatternVoterReasonText(cOff.reason));

			LineSet dummy;
			dummy.axisLen = 512;
			dummy.Resize(8, 512);
			const BlindBlockGridResult bOff = MeasureBlindBlockGridPeriod(dummy, kFloor10Bit, def);
			Assert::IsFalse(bOff.valid);
			Assert::IsTrue(bOff.reason == PatternVoterReason::Disabled);

			const BlockGridVoteResult vOff = VoteFromBlockGrid(720, 2160, 24, 5.0, def, &counters);
			Assert::IsFalse(vOff.valid);
			Assert::IsTrue(vOff.reason == PatternVoterReason::Disabled);
			Assert::AreEqual(0.0, vOff.llrNats, 1e-12);

			// Native codec block period helper sanity checks.
			Assert::IsTrue(IsNativeCodecBlockPeriod(8));
			Assert::IsTrue(IsNativeCodecBlockPeriod(16));
			Assert::IsTrue(IsNativeCodecBlockPeriod(32));
			Assert::IsFalse(IsNativeCodecBlockPeriod(0));
			Assert::IsFalse(IsNativeCodecBlockPeriod(12));
			Assert::IsFalse(IsNativeCodecBlockPeriod(24));

			// Enabled: null, unit==0, wrong unit==99, and empty validityBits must all abstain visibly!
			const PatternVoterSettings on = Enabled();
			ChainPatternVoter voter(on);

			const PatternConsumeResult cNull = voter.Consume(nullptr);
			Assert::IsFalse(cNull.available);
			Assert::IsTrue(cNull.reason == PatternVoterReason::P15NotProduced);
			Assert::AreEqual(static_cast<uint64_t>(1), voter.Counters().p15Unavailable);

			QualityPattern::Record zeroRec;
			std::memset(&zeroRec, 0, sizeof(zeroRec));
			const PatternConsumeResult cZero = voter.Consume(&zeroRec);
			Assert::IsFalse(cZero.available);
			Assert::IsTrue(cZero.reason == PatternVoterReason::P15NotProduced);
			Assert::AreEqual(static_cast<uint64_t>(2), voter.Counters().p15Unavailable);

			QualityPattern::Record badVer = zeroRec;
			badVer.unit = 99u;
			badVer.validityBits = QualityPattern::kValidRole;
			const PatternConsumeResult cBadVer = voter.Consume(&badVer);
			Assert::IsFalse(cBadVer.available);
			Assert::IsTrue(cBadVer.reason == PatternVoterReason::P15VersionMismatch);
			Assert::AreEqual(static_cast<uint64_t>(3), voter.Counters().p15Unavailable);

			QualityPattern::Record emptyBits = zeroRec;
			emptyBits.unit = QualityPattern::kUnitVersion;
			emptyBits.validityBits = 0u;
			const PatternConsumeResult cEmpty = voter.Consume(&emptyBits);
			Assert::IsFalse(cEmpty.available);
			Assert::IsTrue(cEmpty.reason == PatternVoterReason::P15NotProduced);
			Assert::AreEqual(static_cast<uint64_t>(4), voter.Counters().p15Unavailable);
			Assert::AreEqual(static_cast<uint64_t>(0), voter.Counters().p15Consumed);

			// Valid P15 Record decodes all fields accurately (including Q8.8 steps / 256.0) and increments p15Consumed.
			QualityPattern::Record validRec = zeroRec;
			validRec.unit = QualityPattern::kUnitVersion;
			validRec.validityBits = QualityPattern::kValidRole | QualityPattern::kValidScore |
				QualityPattern::kValidSteps | QualityPattern::kValidGrid | QualityPattern::kValidCopy;
			validRec.seq = 42ull;
			validRec.role = static_cast<uint8_t>(QualityPattern::FrameRole::Peak);
			validRec.peakValley = static_cast<uint8_t>(QualityPattern::PeakValley::Peak);
			validRec.score = 0.875f;
			validRec.stepMean = 384u;  // 1.50 in Q8.8
			validRec.stepP90 = 1024u;  // 4.00 in Q8.8
			validRec.stepMax = 2560u;  // 10.00 in Q8.8
			validRec.gridPeriodX = 24u;
			validRec.gridPeriodY = 24u;
			validRec.gridPhaseX = 5u;
			validRec.gridPhaseY = 9u;
			validRec.exactCopyFlags = QualityPattern::kCopyOfPrev1;

			const PatternConsumeResult cOk = voter.Consume(&validRec);
			Assert::IsTrue(cOk.available);
			Assert::IsFalse(cOk.provisional);
			Assert::IsTrue(cOk.role == QualityPattern::FrameRole::Peak);
			Assert::IsTrue(cOk.hasScore);
			Assert::AreEqual(0.875, cOk.qualityScore, 1e-5);
			Assert::IsTrue(cOk.hasSteps);
			Assert::AreEqual(1.5, cOk.stepMean, 1e-6);
			Assert::AreEqual(4.0, cOk.stepP90, 1e-6);
			Assert::AreEqual(10.0, cOk.stepMax, 1e-6);
			Assert::IsTrue(cOk.hasGrid);
			Assert::AreEqual(24, cOk.gridPeriodX);
			Assert::AreEqual(24, cOk.gridPeriodY);
			Assert::AreEqual(5, cOk.gridOffsetX);
			Assert::AreEqual(9, cOk.gridOffsetY);
			Assert::IsTrue(cOk.isExactCopy);
			Assert::AreEqual(static_cast<int>(QualityPattern::kCopyOfPrev1), static_cast<int>(cOk.exactCopyFlags));
			Assert::AreEqual(static_cast<uint64_t>(1), voter.Counters().p15Consumed);

			// Provisional flag must surface visibly.
			QualityPattern::Record provRec = validRec;
			provRec.validityBits |= QualityPattern::kProvisional;
			const PatternConsumeResult cProv = voter.Consume(&provRec);
			Assert::IsTrue(cProv.available);
			Assert::IsTrue(cProv.provisional);

			// Reset clears counters cleanly.
			voter.Reset();
			Assert::AreEqual(static_cast<uint64_t>(0), voter.Counters().p15Consumed);
			Assert::AreEqual(static_cast<uint64_t>(0), voter.Counters().p15Unavailable);
		}

		TEST_METHOD(BlockGridVoterStrictlyCappedAtTwoNatsEvenUnderExtremeSignalAndMisconfig)
		{
			using namespace PatternVoterFx;

			// Adversarial misconfiguration trying to raise maxVoteNats far above the doc 78 D15 2.0 nat ceiling.
			PatternVoterSettings mis = Enabled();
			mis.maxVoteNats = 25.0;

			PatternVoterCounters counters;

			// Extreme positive scaled block-grid signal: 720 -> 2160 (S = 3), 8-px grid scaled to 24-px, contrast = 50.0.
			const BlockGridVoteResult pos = VoteFromBlockGrid(720, 2160, 24, 50.0, mis, &counters);
			Assert::IsTrue(pos.valid);
			Assert::IsTrue(pos.wasCapped);
			Assert::AreEqual(kMaxBlockGridVoteNats, pos.llrNats, 1e-9);
			Assert::IsTrue(pos.llrNats <= 2.0);
			Assert::AreEqual(8, pos.matchedCodecBlockPx);
			Assert::IsTrue(counters.gridVotesCappedAt2Nats >= 1u);
			Assert::IsTrue(counters.gridVotesPositive >= 1u);

			// MANDATORY Rule 3 Negative Control on EXACT same input:
			// Independent naive uncapped log-contrast voter (computed directly from boundaryContrastRatio = 50.0,
			// 100% independent of production's rawUncappedNats) exceeds 2.0 nats on a single evaluation and blows
			// past c1 = 14.87 over a 10-eval window, proving the hard 2.0-nat spec cap is actively enforced!
			const double naiveUncappedPosNats = 2.0 * std::log(50.0);
			Assert::IsTrue(naiveUncappedPosNats > kMaxBlockGridVoteNats);
			Assert::IsTrue(10.0 * naiveUncappedPosNats > 14.87);
			Assert::IsTrue(pos.rawUncappedNats > kMaxBlockGridVoteNats);
			Assert::IsTrue(10.0 * pos.rawUncappedNats > 14.87);

			// Extreme negative veto: native 8-px fine grid with contrast = 50.0 against 720p candidate.
			const BlockGridVoteResult neg = VoteFromBlockGrid(720, 2160, 8, 50.0, mis, &counters);
			Assert::IsTrue(neg.valid);
			Assert::IsTrue(neg.wasCapped);
			Assert::AreEqual(-kMaxBlockGridVoteNats, neg.llrNats, 1e-9);
			Assert::IsTrue(neg.llrNats >= -2.0);
			const double naiveUncappedNegNats = -2.0 * std::log(50.0);
			Assert::IsTrue(naiveUncappedNegNats < -kMaxBlockGridVoteNats);
			Assert::IsTrue(neg.rawUncappedNats < -kMaxBlockGridVoteNats); // Negative control: uncapped exceeds -2.0!
			Assert::IsTrue(counters.gridVotesNegative >= 1u);
			Assert::IsTrue(counters.gridVotesCappedAt2Nats >= 2u);
		}

		TEST_METHOD(Native4KCodecGridNeverAwardsFalseUpscaleNats)
		{
			using namespace PatternVoterFx;
			const PatternVoterSettings on = Enabled();

			constexpr int kN = 1024;
			constexpr int kLines = 32;
			constexpr int kCand1080 = 512; // S = 2
			constexpr int kCand720 = 341;  // S ~ 3

			DetRng rng(2026112320ull);
			PatternVoterCounters counters;

			const int nativePeriods[3] = { 8, 16, 32 };
			for (int pi = 0; pi < 3; ++pi)
			{
				const int period = nativePeriods[pi];
				const LineSet ls = MakeNativeBlockGridLines(kN, kLines, period, 0, rng);

				const BlindBlockGridResult blind = MeasureBlindBlockGridPeriod(ls, kFloor10Bit, on);
				Assert::IsTrue(blind.valid);
				Assert::IsTrue(blind.gridDetected);
				Assert::AreEqual(period, blind.dominantPeriodPx);
				Assert::IsTrue(blind.boundaryContrastRatio > 2.0);

				// MANDATORY Rule 3 Negative Control on EXACT same 16-px native 4K block grid:
				// A naive period-matcher checking `observedPeriod == 8 * (N / candidateN)` without the
				// native-codec ambiguity guard sees 16 == 8 * (1024 / 512) and WOULD falsely award positive
				// upscale nats to 1080p on native 4K!
				if (period == 16)
				{
					const double expectedScaled8 = 8.0 * (static_cast<double>(kN) / static_cast<double>(kCand1080));
					const bool naiveMatches1080 = std::fabs(static_cast<double>(blind.dominantPeriodPx) - expectedScaled8) <=
						on.periodMatchTolerance * expectedScaled8;
					const double naiveUnprotectedNats = naiveMatches1080 ? std::log(blind.boundaryContrastRatio) : 0.0;
					Assert::IsTrue(naiveMatches1080);
					Assert::IsTrue(naiveUnprotectedNats > 0.5); // Hazard proven real on exact same 16-px lines!
				}

				const BlockGridVoteResult v1080 = VoteFromBlockGrid(kCand1080, kN, blind.dominantPeriodPx, blind.boundaryContrastRatio, on, &counters);
				const BlockGridVoteResult v720 = VoteFromBlockGrid(kCand720, kN, blind.dominantPeriodPx, blind.boundaryContrastRatio, on, &counters);

				// Invariant: native 4K 8/16/32-px codec grids NEVER award positive upscale nats!
				Assert::IsTrue(v1080.llrNats <= 0.0);
				Assert::IsTrue(v720.llrNats <= 0.0);

				if (period == 16 || period == 32)
				{
					Assert::AreEqual(0.0, v1080.llrNats, 1e-12);
					Assert::IsTrue(v1080.reason == PatternVoterReason::AmbiguousNativeCodecGrid);
				}
				else if (period == 8)
				{
					// Fine 8-px native grid contradicts 720p upscale with a bounded negative veto in [-2.0, 0.0).
					Assert::IsTrue(v720.llrNats < 0.0 && v720.llrNats >= -kMaxBlockGridVoteNats);
					Assert::IsTrue(v720.reason == PatternVoterReason::NativeFineGridContradictsUpscale ||
						v720.reason == PatternVoterReason::ClampedToMaxNats);
				}
			}

			Assert::IsTrue(counters.gridVotesAbstainedAmbiguous >= 2u);
			Assert::AreEqual(static_cast<uint64_t>(0), counters.gridVotesPositive);
		}

		TEST_METHOD(ScaledNonNativeBlockGrid24PxDetectedBlindAndSupports720p)
		{
			using namespace PatternVoterFx;
			const PatternVoterSettings on = Enabled();

			constexpr int kN = 1080;
			constexpr int kCand720 = 360;  // S = 3 -> 8-px codec block stretches to 24-px
			constexpr int kCand1080 = 540; // S = 2 -> wrong candidate (expects 16/32-px, NOT 24-px)
			constexpr int kLines = 32;

			DetRng rng(2026112321ull);
			const int testPhases[2] = { 0, 7 };

			for (int ti = 0; ti < 2; ++ti)
			{
				const int phase = testPhases[ti];
				const LineSet ls = MakeScaled720To2160GridLines(kN, kCand720, kLines, phase, rng);

				const BlindBlockGridResult blind = MeasureBlindBlockGridPeriod(ls, kFloor10Bit, on);
				Assert::IsTrue(blind.valid);
				Assert::IsTrue(blind.gridDetected);
				Assert::AreEqual(24, blind.dominantPeriodPx);
				Assert::AreEqual(phase, blind.dominantPhasePx);
				Assert::IsTrue(blind.boundaryContrastRatio > 1.5);

				PatternVoterCounters counters;
				const BlockGridVoteResult v720 = VoteFromBlockGrid(kCand720, kN, blind.dominantPeriodPx, blind.boundaryContrastRatio, on, &counters);
				Assert::IsTrue(v720.valid);
				Assert::IsTrue(v720.llrNats > 0.0 && v720.llrNats <= kMaxBlockGridVoteNats);
				Assert::AreEqual(8, v720.matchedCodecBlockPx);
				Assert::IsTrue(v720.reason == PatternVoterReason::ScaledGridSupportsCandidate ||
					v720.reason == PatternVoterReason::ClampedToMaxNats);

				// Stateful ChainPatternVoter::VoteBlind path matches on the exact same lines.
				ChainPatternVoter voter(on);
				const BlockGridVoteResult vBlind = voter.VoteBlind(ls, kCand720, kFloor10Bit);
				Assert::IsTrue(vBlind.valid);
				Assert::IsTrue(vBlind.llrNats > 0.0 && vBlind.llrNats <= kMaxBlockGridVoteNats);
				Assert::AreEqual(8, vBlind.matchedCodecBlockPx);

				// MANDATORY Rule 3 Negative Control on EXACT same 24-px lines:
				// Wrong candidate (n = 540, S = 2) must receive ZERO upscale nats (not confused with 720p!).
				const BlockGridVoteResult vWrong = VoteFromBlockGrid(kCand1080, kN, blind.dominantPeriodPx, blind.boundaryContrastRatio, on, &counters);
				Assert::AreEqual(0.0, vWrong.llrNats, 1e-12);
			}

			// Additional negative control: smooth non-blocked 720p -> 2160p lines have NO 24-px grid!
			LineSet smooth;
			smooth.axis = 0;
			smooth.axisLen = kN;
			smooth.origin = 0;
			smooth.Resize(kLines, kN);
			for (int li = 0; li < kLines; ++li)
			{
				std::vector<double> lat(static_cast<size_t>(kCand720), 512.0);
				for (int k = 0; k < kCand720; ++k)
					lat[static_cast<size_t>(k)] = 512.0 + 120.0 * std::sin(2.0 * kPi * static_cast<double>(k) / 41.0 + static_cast<double>(li));
				const std::vector<double> up = Upscale1D(lat, kN, 5);
				double* y = smooth.Line(li);
				for (int i = 0; i < kN; ++i)
					y[i] = ClampD(std::floor(up[static_cast<size_t>(i)] + 0.5), 0.0, 1023.0);
			}
			const BlindBlockGridResult noGrid = MeasureBlindBlockGridPeriod(smooth, kFloor10Bit, on);
			Assert::IsFalse(noGrid.gridDetected);
		}

		TEST_METHOD(DivisionFreeLcm96BlindGridMatchesNaiveModuloReferenceExact)
		{
			using namespace PatternVoterFx;
			const PatternVoterSettings on = Enabled();

			// Independent naive direct `% P` reference implementing the exact blind block-grid specification.
			struct NaiveRefResult
			{
				bool gridDetected = false;
				int dominantPeriodPx = 0;
				int dominantPhasePx = 0;
				double boundaryContrastRatio = 1.0;
			};

			auto computeNaiveModuloRef = [](const LineSet& ls, double floorCodes, const PatternVoterSettings& s, int foldBugStride) -> NaiveRefResult {
				NaiveRefResult out;
				const double lineGate = s.lineGateFactor * floorCodes;
				std::vector<int> active;
				for (int li = 0; li < ls.count; ++li)
				{
					const double* y = ls.Line(li);
					double mean = 0.0;
					for (int j = 0; j < ls.length; ++j)
						mean += y[j];
					mean /= static_cast<double>(ls.length);
					double ss = 0.0;
					for (int j = 0; j < ls.length; ++j)
					{
						const double d = y[j] - mean;
						ss += d * d;
					}
					if (std::sqrt(ss / static_cast<double>(ls.length)) >= lineGate)
						active.push_back(li);
				}
				if (active.empty() || ls.length < 17)
					return out;

				std::vector<double> meanAbsDiff(static_cast<size_t>(ls.length), 0.0);
				const double invActive = 1.0 / static_cast<double>(active.size());
				for (int j = 1; j < ls.length; ++j)
				{
					double acc = 0.0;
					for (size_t ai = 0; ai < active.size(); ++ai)
					{
						const double* y = ls.Line(active[ai]);
						acc += std::fabs(y[j] - y[j - 1]);
					}
					meanAbsDiff[static_cast<size_t>(j)] = acc * invActive;
				}

				const int periods[6] = { 8, 12, 16, 24, 32, 48 };
				double bestRatio = 1.0;
				int bestP = 0;
				int bestPh = 0;
				for (int pi = 0; pi < 6; ++pi)
				{
					const int P = periods[pi];
					if (ls.length < 2 * P + 1)
						continue;
					for (int ph = 0; ph < P; ++ph)
					{
						double bSum = 0.0, iSum = 0.0;
						int bCnt = 0, iCnt = 0;
						for (int j = 1; j < ls.length; ++j)
						{
							const int rawCoord = ls.origin + j;
							// When foldBugStride > 0, simulate a broken fold stride (e.g. 95 != LCM 96) as negative control.
							const int coord = (foldBugStride > 0) ? (((rawCoord % foldBugStride) + foldBugStride) % foldBugStride) : rawCoord;
							const int mod = ((coord % P) + P) % P;
							if (mod == ph)
							{
								bSum += meanAbsDiff[static_cast<size_t>(j)];
								++bCnt;
							}
							else
							{
								iSum += meanAbsDiff[static_cast<size_t>(j)];
								++iCnt;
							}
						}
						if (bCnt < 2 || iCnt < 2)
							continue;
						const double bMean = bSum / static_cast<double>(bCnt);
						const double iMean = iSum / static_cast<double>(iCnt);
						if (bMean < lineGate)
							continue;
						const double denom = iMean > floorCodes ? iMean : floorCodes;
						const double ratio = bMean / denom;
						if (ratio > bestRatio)
						{
							bestRatio = ratio;
							bestP = P;
							bestPh = ph;
						}
					}
				}
				if (bestP > 0 && bestRatio > s.lineGateFactor)
				{
					out.gridDetected = true;
					out.dominantPeriodPx = bestP;
					out.dominantPhasePx = bestPh;
					out.boundaryContrastRatio = bestRatio;
				}
				return out;
			};

			// Test EVERY period in {8, 12, 16, 24, 32, 48} with non-zero phases AND prime non-zero origin = 13
			// (13 % P != 0 for all 6 periods!) plus non-multiple-of-96 length = 931 and 2 flat decoy lines.
			const int periods[6] = { 8, 12, 16, 24, 32, 48 };
			const int phases[6] = { 3, 5, 11, 17, 23, 37 };
			constexpr int kAxisLen = 960;
			constexpr int kOrigin = 13;
			constexpr int kLength = 931;
			constexpr int kActiveLines = 24;
			constexpr int kDecoyFlatLines = 2;

			DetRng rng(2026112322ull);

			for (int pi = 0; pi < 6; ++pi)
			{
				const int P = periods[pi];
				const int ph = phases[pi];

				LineSet ls;
				ls.axis = 0;
				ls.axisLen = kAxisLen;
				ls.origin = kOrigin;
				ls.Resize(kActiveLines + kDecoyFlatLines, kLength);

				for (int li = 0; li < kActiveLines; ++li)
				{
					const int numBlocks = (kAxisLen / P) + 4;
					std::vector<double> levels(static_cast<size_t>(numBlocks), 512.0);
					for (int b = 0; b < numBlocks; ++b)
						levels[static_cast<size_t>(b)] = ClampD(512.0 + 55.0 * rng.Normal01(), 120.0, 900.0);

					double* y = ls.Line(li);
					for (int j = 0; j < kLength; ++j)
					{
						const int coord = kOrigin + j;
						// Euclidean block index so discontinuity lands strictly at coord % P == ph.
						const int shifted = coord - ph;
						const int b = (shifted >= 0) ? (shifted / P + 1) : 0;
						const double smooth = 3.0 * std::sin(2.0 * kPi * static_cast<double>(coord) / 173.0 + static_cast<double>(li));
						y[j] = ClampD(std::floor(levels[static_cast<size_t>(b)] + smooth + 0.5 * rng.Normal01() + 0.5), 0.0, 1023.0);
					}
				}
				// Add 2 constant flat decoy lines (must be excluded by lineGateFactor equally in both paths).
				for (int li = kActiveLines; li < kActiveLines + kDecoyFlatLines; ++li)
				{
					double* y = ls.Line(li);
					for (int j = 0; j < kLength; ++j)
						y[j] = 512.0;
				}

				const BlindBlockGridResult opt = MeasureBlindBlockGridPeriod(ls, kFloor10Bit, on);
				const NaiveRefResult ref = computeNaiveModuloRef(ls, kFloor10Bit, on, 0);

				Assert::IsTrue(opt.valid && opt.gridDetected);
				Assert::IsTrue(ref.gridDetected);
				Assert::AreEqual(P, opt.dominantPeriodPx);
				Assert::AreEqual(ph, opt.dominantPhasePx);
				Assert::AreEqual(ref.dominantPeriodPx, opt.dominantPeriodPx);
				Assert::AreEqual(ref.dominantPhasePx, opt.dominantPhasePx);
				Assert::IsTrue(std::fabs(opt.boundaryContrastRatio - ref.boundaryContrastRatio) < 1e-12);

				// MANDATORY Rule 3 Negative Control #1 on EXACT same lines:
				// Hardcoding origin = 0 (ignoring ls.origin = 13) yields the WRONG phase for every single period P!
				LineSet wrongOriginLs = ls;
				wrongOriginLs.origin = 0;
				const NaiveRefResult wrongOriginRef = computeNaiveModuloRef(wrongOriginLs, kFloor10Bit, on, 0);
				Assert::IsTrue(wrongOriginRef.gridDetected);
				Assert::AreNotEqual(ph, wrongOriginRef.dominantPhasePx);

				// MANDATORY Rule 3 Negative Control #2 on EXACT same P = 24 lines:
				// Using a broken fold stride (95 instead of LCM 96) scrambles boundary alignment across phases
				// and collapses boundaryContrastRatio to < 50% of the true naive reference!
				if (P == 24)
				{
					const NaiveRefResult brokenFold = computeNaiveModuloRef(ls, kFloor10Bit, on, 95);
					Assert::IsTrue(!brokenFold.gridDetected ||
						brokenFold.boundaryContrastRatio < 0.5 * ref.boundaryContrastRatio);
				}
			}
		}
	};
}
