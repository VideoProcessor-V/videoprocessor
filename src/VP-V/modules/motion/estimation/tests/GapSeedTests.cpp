// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV hiccup gap seed. CPU: when GapSeed::Make seeds and when it must not (a resync, a seek, more than the gap, no pair, an ambiguous
// pair). GPU (WARP unless hardware): a synthetic pan with one dropped frame - the fill pair seeded from the last valid shared pair has an
// error no larger than the unseeded one - and an invalid seed leaves the result byte-identical to the unseeded estimate.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/GapSeed.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(GapSeedTests)
	{
	public:
		TEST_METHOD(SeedsOnlyWhenTheStateMovedForTheGapAndThePairIsUsable)
		{
			using VpMotion::GapSeed;
			// A pair (9 -> 10) with 1.5 cells of horizontal motion per frame, usable (ambiguity < 0.9) both ways; the fill pair is 10 -> 12.
			float g[2][4] = { { 1.5f, 0.25f, 0.2f, 1.0f }, { -1.5f, -0.25f, 0.2f, 1.0f } };
			GapSeed s = GapSeed::Make(g, 10, 10, 12, 7, 8, true);
			Assert::IsTrue(s.valid);
			Assert::AreEqual(3.0, static_cast<double>(s.global[0][0]), 1e-6, L"scaled by the two periods the gap spans");
			Assert::AreEqual(-0.5, static_cast<double>(s.global[1][1]), 1e-6);
			Assert::AreEqual(0.5, static_cast<double>(s.global[0][2]), 1e-6);
			Assert::IsFalse(GapSeed::Make(nullptr, 10, 10, 12, 7, 8, true).valid, L"no shared pair");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 11, 7, 8, true).valid, L"not a gap pair");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 10 + GapSeed::kMaxPeriods + 1, 7, 8, true).valid, L"too long to be a hiccup");
			Assert::IsFalse(GapSeed::Make(g, 9, 10, 12, 7, 8, true).valid, L"the pair does not end at the gap's first frame");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 8, false).valid, L"the last cause was a resync / seek / format change, not the gap");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 9, true).valid, L"the epoch moved for more than the gap");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 7, true).valid, L"the epoch did not move: not a gap");
			g[1][2] = 1.0f;
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 8, true).valid, L"an ambiguous whole-frame motion is no prior");
			// The SAD-vs-zero rejection (the matcher's check): the seed must explain both directions far better than the zero vector.
			Assert::IsTrue(GapSeed::Beats(1000.0f, 10.0f, 20.0f), L"a seed that removes nearly all the zero mismatch holds");
			Assert::IsFalse(GapSeed::Beats(1000.0f, 900.0f, 10.0f), L"worse than zero in one direction: dropped");
			Assert::IsFalse(GapSeed::Beats(1000.0f, 1100.0f, 1100.0f), L"worse than zero: dropped");
			Assert::IsFalse(GapSeed::Beats(0.0f, 0.0f, 0.0f), L"identical frames: nothing to seed");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(SeededFillPairIsNoWorseThanUnseededAndAnInvalidSeedIsByteIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SeededFillPairIsNoWorseThanUnseededAndAnInvalidSeedIsByteIdentical)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 768, height = 384, perFrame = 40, dropped = 2;   // frames 0 and 2: one frame dropped between
			const int truth = perFrame * dropped;
			auto frame = [&](int shift) {
				return MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
			};
			const DejudderFrame a = frame(0), b = frame(truth);
			DejudderGpuPlanes pa, pb;
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
			auto settings = [] {
				Dejudder::Settings s = DejudderTestSettings();
				s.globalPrior = true;
				s.estCandidates = true;
				return s;
			};
			auto error = [&](const DejudderField& f) {
				double sum = 0.0;
				long n = 0;
				for (int by = 2; by < f.blocksY - 2; ++by)
					for (int bx = 2; bx < f.blocksX - 2; ++bx)
					{
						// An untrusted block counts as fully wrong: a seed that fails to help must show.
						sum += f.trusted(bx, by) ? std::hypot(f.vx(bx, by) - truth, f.vy(bx, by)) : static_cast<double>(truth);
						++n;
					}
				return n ? sum / n : 1e9;
			};
			Dejudder::MotionWarp unseeded(fixture.Log(), gpu, settings());
			Assert::IsTrue(unseeded.Estimate(pa.planes, pb.planes));
			const DejudderField fieldUnseeded = DownloadField(gpu, unseeded);
			// The shared pair before the gap: perFrame source px of motion, i.e. perFrame / 8 cells; the gap spans two periods.
			float g[2][4] = { { perFrame / 8.0f, 0.0f, 0.2f, 1.0f }, { -perFrame / 8.0f, 0.0f, 0.2f, 1.0f } };
			const VpMotion::GapSeed seed = VpMotion::GapSeed::Make(g, 10, 10, 10 + dropped, 7, 8, true);
			Assert::IsTrue(seed.valid);
			Dejudder::MotionWarp seeded(fixture.Log(), gpu, settings());
			seeded.SetSearchSeed(seed.global);
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes));
			Assert::IsTrue(seeded.SearchSeedUsed(), L"D1 started from the gap seed");
			const DejudderField fieldSeeded = DownloadField(gpu, seeded);
			Assert::IsTrue(error(fieldSeeded) <= error(fieldUnseeded) + 1e-6, L"seeded error <= unseeded error");
			// An invalid seed is never handed over: the estimate is the unseeded one, byte for byte; and a seed is consumed by one estimate.
			const VpMotion::GapSeed none = VpMotion::GapSeed::Make(g, 9, 10, 10 + dropped, 7, 8, true);
			Assert::IsFalse(none.valid);
			Dejudder::MotionWarp invalid(fixture.Log(), gpu, settings());
			Assert::IsTrue(invalid.Estimate(pa.planes, pb.planes));
			Assert::IsFalse(invalid.SearchSeedUsed());
			Assert::IsTrue(DownloadField(gpu, invalid).texels == fieldUnseeded.texels, L"no seed: byte-identical to today's estimate");
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes));
			Assert::IsFalse(seeded.SearchSeedUsed(), L"the seed was spent by the first estimate");
		}

		// A seed handed to the OUTER matcher reaches the instance that searches: at lowRes (a 4K-class input) the search runs in the half-size
		// inner, so the outer forwards the seed (like the pair computed ahead) and reports the inner's "used". The seed is in the searching
		// instance's own input pixels (half of the source's), as the shared pair's whole-frame motion is.
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResSeedReachesTheInnerAndIsNoWorseThanUnseeded)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResSeedReachesTheInnerAndIsNoWorseThanUnseeded)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			for (int all = 0; all < 2; ++all)   // dejudder_low_res on (the inner searches, the outer refines) and all (the inner makes the field)
			{
				const CaseResult r = RunCase(fixture, 2048, 1536, true, all == 1, kPreGapTruth);
				Assert::IsTrue(r.lowResEngaged, L"the 1536-line input halves for the search");
				Assert::IsTrue(r.seededUsed, L"D1 started from the gap seed in the inner (the outer reports it)");
				Assert::IsTrue(r.seeded.trustedError <= r.unseeded.trustedError + 1e-6, L"lowRes: seeded trusted-block error <= unseeded");
				Assert::IsTrue(r.seeded.untrustedShare <= r.unseeded.untrustedShare + 1e-9, L"lowRes: seeded untrusted share <= unseeded");
				Assert::IsFalse(r.spentSeedUsedAgain, L"the seed is spent by the one estimate (the inner too)");
			}
		}

		// A wrong seed (the motion CHANGED during the hiccup) must not make the fill pair worse than the unseeded estimate. Two bounds, kept
		// SEPARATE so a loss in one cannot hide behind the other: the mean error of the blocks the matcher TRUSTS (source pixels) may exceed the
		// unseeded one by at most kErrorEpsilonPx, and the share of interior blocks left untrusted by at most kShareEpsilon. Cases: the pan
		// reversed, stopped, and ran at half and at double the pre-gap speed. If any of this fails the seed has to be rejected, not shipped.
		BEGIN_TEST_METHOD_ATTRIBUTE(AWrongSeedIsNoWorseThanUnseededPlusEpsilon)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AWrongSeedIsNoWorseThanUnseededPlusEpsilon)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			for (const int truth : { -kPreGapTruth, 0, kPreGapTruth / 2, kPreGapTruth * 2 })
			{
				const CaseResult full = RunCase(fixture, 768, 384, false, false, truth);
				const std::wstring name = L"full resolution, true motion " + std::to_wstring(truth) + L" px: ";
				Assert::IsTrue(full.seeded.trustedError <= full.unseeded.trustedError + kErrorEpsilonPx, (name + L"trusted-block error").c_str());
				Assert::IsTrue(full.seeded.untrustedShare <= full.unseeded.untrustedShare + kShareEpsilon, (name + L"untrusted share").c_str());
			}
			for (const int truth : { -kPreGapTruth, 0 })
			{
				const CaseResult low = RunCase(fixture, 2048, 1536, true, false, truth);
				Assert::IsTrue(low.lowResEngaged);
				const std::wstring name = L"lowRes, true motion " + std::to_wstring(truth) + L" px: ";
				Assert::IsTrue(low.seeded.trustedError <= low.unseeded.trustedError + kErrorEpsilonPx, (name + L"trusted-block error").c_str());
				Assert::IsTrue(low.seeded.untrustedShare <= low.unseeded.untrustedShare + kShareEpsilon, (name + L"untrusted share").c_str());
			}
		}

	private:
		static constexpr int kPerFrame = 40, kDropped = 2, kPreGapTruth = kPerFrame * kDropped;   // the pre-gap pan, and what it would span over the gap
		static constexpr double kErrorEpsilonPx = 0.5, kShareEpsilon = 0.05;

		struct FieldScore
		{
			double trustedError = 1e9;     // mean |v - truth| over the trusted interior blocks (source px); 0 when none is trusted
			double untrustedShare = 1.0;   // share of the interior blocks that are not trusted
		};
		struct CaseResult
		{
			FieldScore unseeded, seeded;
			bool seededUsed = false, spentSeedUsedAgain = true, lowResEngaged = false;
		};

		static FieldScore Score(const DejudderField& f, int truth)
		{
			FieldScore score;
			double sum = 0.0;
			long trusted = 0, total = 0;
			for (int by = 2; by < f.blocksY - 2; ++by)
				for (int bx = 2; bx < f.blocksX - 2; ++bx)
				{
					++total;
					if (!f.trusted(bx, by))
						continue;
					sum += std::hypot(f.vx(bx, by) - truth, f.vy(bx, by));
					++trusted;
				}
			score.trustedError = trusted ? sum / trusted : 0.0;
			score.untrustedShare = total ? 1.0 - static_cast<double>(trusted) / total : 1.0;
			return score;
		}

		// One fill pair over a dropped frame (frames 0 and 2): the content moves `truth` source px between them (kPreGapTruth = the pan
		// continued as before the gap, the seed is right; anything else = the motion changed during the hiccup, the seed from the pre-gap pair
		// is wrong). Estimated unseeded, then seeded from the pre-gap motion.
		static CaseResult RunCase(TargetLutGpuFixture& fixture, int width, int height, bool lowRes, bool lowResAll, int truth)
		{
			pl_gpu gpu = fixture.Gpu();
			auto frame = [&](int shift) {
				return MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
			};
			DejudderGpuPlanes pa, pb;
			UploadDejudderFrame(gpu, frame(0), pa);
			UploadDejudderFrame(gpu, frame(truth), pb);
			auto settings = [&] {
				Dejudder::Settings s = DejudderTestSettings();
				s.globalPrior = true;
				s.estCandidates = true;
				s.lowRes = lowRes;
				s.lowResAll = lowResAll;
				return s;
			};
			CaseResult r;
			Dejudder::MotionWarp unseeded(fixture.Log(), gpu, settings());
			Assert::IsTrue(unseeded.Estimate(pa.planes, pb.planes), std::wstring(unseeded.LastError().begin(), unseeded.LastError().end()).c_str());
			r.lowResEngaged = unseeded.SearchHalvings() > 0;
			r.unseeded = Score(DownloadField(gpu, unseeded), truth);
			// The shared pair before the gap: kPerFrame source px per frame (always +), in the searching instance's input pixels: half of the
			// source's at lowRes; D1 cells are 1/8 of those. The gap spans kDropped periods.
			const float cell = (lowRes ? 0.5f : 1.0f) * kPerFrame / 8.0f;
			float g[2][4] = { { cell, 0.0f, 0.2f, 1.0f }, { -cell, 0.0f, 0.2f, 1.0f } };
			const VpMotion::GapSeed seed = VpMotion::GapSeed::Make(g, 10, 10, 10 + kDropped, 7, 8, true);
			Assert::IsTrue(seed.valid);
			Dejudder::MotionWarp seeded(fixture.Log(), gpu, settings());
			seeded.SetSearchSeed(seed.global);
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes), std::wstring(seeded.LastError().begin(), seeded.LastError().end()).c_str());
			r.seededUsed = seeded.SearchSeedUsed();
			r.seeded = Score(DownloadField(gpu, seeded), truth);
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes));
			r.spentSeedUsedAgain = seeded.SearchSeedUsed();
			return r;
		}
	};
}
