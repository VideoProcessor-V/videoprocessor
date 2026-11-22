// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// U7 unit version 3 (102b U7-MULTIFRAME-FADE-LAW; version 2 is U7-ACCURACY's cut law): the multi-frame fade law (CutFadeJudge::FadeRun / FoldFadeRun). Real P4 records
// (SourceStats::ComputeRecord) of a synthetic half plane: a horizontal ramp with a deterministic per-frame noise, scaled about black
// (4096, 8-bit 16) by the fade gain. The pair law (ClassifyEvents) is unchanged and checked alongside. CPU only.

#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CutFadeJudgeFadeRunTestsNs
{
	using Record = VpUnit::SourceStats::Record;
	using Events = VpUnit::CutFadeJudge::EventSignals;
	using VpUnit::CutFadeJudge::FadeRun;

	constexpr uint32_t kW = 160, kH = 90;
	constexpr double kBlack = 4096.0;

	inline int Noise(uint32_t x, uint32_t y, uint32_t k)
	{
		uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (k * 83492791u);
		h ^= h >> 13;
		h *= 0x5bd1e995u;
		h ^= h >> 15;
		return static_cast<int>(h % 513u) - 256;
	}

	// gain about black; shift > 0: the ramp is twice the plane wide and moves left by `shift` px (a pan, no fade).
	inline Record Picture(double gain, uint32_t k, uint32_t shift = 0, bool pan = false)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const int64_t base = 8192 + (int64_t(x + shift) * 48000) / (pan ? int64_t(2 * kW) : int64_t(kW - 1));
				const double v = kBlack + gain * (double(base) - kBlack) + double(Noise(x, y, k));
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()), L"the record");
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r), L"the record reads back");
		return r;
	}

	inline Record Flat(uint32_t k)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
				half[size_t(y) * kW + x] = static_cast<uint16_t>(6000 + Noise(x, y, k));
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
		return r;
	}

	inline Record Black(uint32_t k, bool noisy = false)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const double v = kBlack + (noisy ? double(Noise(x, y, k)) / 4.0 : 0.0);
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
		return r;
	}

	// `pre` static frames, a fade over `n` frames from gain 1 to `g1`, then `post` static frames at g1.
	inline std::vector<Record> FadeSequence(int n, int pre, int post, double g1)
	{
		std::vector<Record> r;
		uint32_t k = 0;
		for (int i = 0; i < pre; ++i)
			r.push_back(Picture(1.0, k++));
		for (int i = 1; i <= n; ++i)
			r.push_back(Picture(1.0 - (1.0 - g1) * i / n, k++));
		for (int i = 0; i < post; ++i)
			r.push_back(Picture(g1, k++));
		return r;
	}

	struct Pair
	{
		bool pairFade = false, runFade = false, cut = false, flash = false;
		bool Fade() const { return pairFade || runFade; }
	};

	// Every pair in order through one run, as the skeleton's seal does.
	inline std::vector<Pair> Play(const std::vector<Record>& recs)
	{
		std::vector<Pair> out(recs.size());
		FadeRun run;
		for (size_t k = 1; k < recs.size(); ++k)
		{
			const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false);
			out[k].pairFade = e.fade;
			out[k].cut = e.cut;
			out[k].flash = e.flash;
			out[k].runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], e, run);
		}
		return out;
	}

	inline int Count(const std::vector<Pair>& p, size_t from, size_t to, bool pairLawOnly = false)
	{
		int n = 0;
		for (size_t k = from; k < to && k < p.size(); ++k)
			n += (pairLawOnly ? p[k].pairFade : p[k].Fade()) ? 1 : 0;
		return n;
	}

	TEST_CLASS(CutFadeJudgeFadeRunTests)
	{
	public:
		TEST_METHOD(UnitVersionIsThree)
		{
			Assert::AreEqual(3u, VpUnit::CutFadeJudge::kUnitVersion, L"the multi-frame fade law is a registered law change (3, after U7-ACCURACY's 2)");
		}

		// A 300-frame fade (0.25 % of gain per frame): the pair law finds none of it; the run finds it once the gain moved 2 %.
		TEST_METHOD(ASlowFadeIsFoundByTheRunWhereThePairLawMissesIt)
		{
			const int pre = 20, n = 300, post = 20;
			const std::vector<Pair> p = Play(FadeSequence(n, pre, post, 0.25));
			Assert::AreEqual(0, Count(p, 1, size_t(pre)), L"no fade on the static frames before");
			Assert::IsTrue(Count(p, size_t(pre), size_t(pre + n), true) <= n / 10, L"the pair law alone misses the slow fade (unit version 1)");
			Assert::IsTrue(Count(p, size_t(pre), size_t(pre + n)) >= n - 15, L"the run finds the fade after a 2 % gain");
			Assert::IsTrue(Count(p, size_t(pre + n), p.size()) <= 2, L"at most a frame or two of lag after the fade ends");
			for (const Pair& q : p)
			{
				Assert::IsFalse(q.cut, L"a fade is never a cut");
				Assert::IsFalse(q.flash);
			}
		}

		// The same gain path ten times faster: the same law, no rate or length in it.
		TEST_METHOD(AFastFadeIsFoundByTheSameLaw)
		{
			const int pre = 20, n = 30, post = 20;
			const std::vector<Pair> p = Play(FadeSequence(n, pre, post, 0.25));
			Assert::AreEqual(0, Count(p, 1, size_t(pre)));
			Assert::IsTrue(Count(p, size_t(pre), size_t(pre + n)) >= n - 2, L"every fade pair after the first two");
			Assert::IsTrue(Count(p, size_t(pre + n), p.size()) <= 2);
		}

		TEST_METHOD(AStaticPictureAndAPanAreNoFade)
		{
			std::vector<Record> still, pan;
			for (uint32_t k = 0; k < 120; ++k)
				still.push_back(Picture(1.0, k));
			for (uint32_t k = 0; k < 100; ++k)
				pan.push_back(Picture(1.0, k, 2 * k, true));
			Assert::AreEqual(0, Count(Play(still), 1, still.size()), L"noise is no fade");
			Assert::AreEqual(0, Count(Play(pan), 1, pan.size()), L"a pan shifts the picture; it does not scale it about black");
		}

		// Every fade frame shown twice (a repeat cadence): a repeated record is never a fade and the run holds through it.
		TEST_METHOD(RepeatedRecordsHoldTheRun)
		{
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < 20; ++i)
				recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= 60; ++i)
			{
				const Record r = Picture(1.0 - 0.5 * i / 60, k++);
				recs.push_back(r);
				recs.push_back(r);
			}
			const std::vector<Pair> p = Play(recs);
			int repeatFades = 0, moving = 0, movingFades = 0;
			for (size_t j = 21; j < p.size(); j += 2)
				repeatFades += p[j].Fade() ? 1 : 0;
			for (size_t j = 22; j < p.size(); j += 2)
			{
				++moving;
				movingFades += p[j].Fade() ? 1 : 0;
			}
			Assert::AreEqual(0, repeatFades, L"a repeated record changed nothing");
			Assert::IsTrue(movingFades >= moving - 4, L"the fade goes on across the repeats");
		}

		// A hard cut inside a fade stays a cut (the pair law, unchanged) and ends the run.
		TEST_METHOD(ACutInsideAFadeStaysACutAndEndsTheRun)
		{
			std::vector<Record> recs = FadeSequence(30, 20, 0, 0.25);
			recs.resize(35);
			recs.push_back(Flat(35));
			recs.push_back(Flat(36));
			FadeRun run;
			bool fadeBeforeCut = false;
			for (size_t k = 1; k < recs.size(); ++k)
			{
				const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false);
				const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], e, run);
				if (k == 34)
					fadeBeforeCut = runFade || e.fade;
				if (k == 35)
				{
					Assert::IsTrue(e.cut, L"the jump to another picture is a cut");
					Assert::IsFalse(e.fade);
					Assert::IsFalse(runFade, L"a cut is never a fade");
					Assert::AreEqual(0, run.direction, L"the cut ends the run");
					Assert::AreEqual(0u, run.pairs);
				}
			}
			Assert::IsTrue(fadeBeforeCut, L"the fade before the cut was found");
		}

		// The run is continued only by the pair that starts at its end record (a skipped frame or a restarted numbering starts over).
		TEST_METHOD(ARunContinuesOnlyFromItsOwnEndRecord)
		{
			const std::vector<Record> recs = FadeSequence(30, 20, 20, 0.25);
			FadeRun run;
			bool last = false;
			for (size_t k = 1; k <= 30; ++k)
				last = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false), run);
			Assert::IsTrue(last, L"the run is a fade by frame 30");
			Assert::IsTrue(VpUnit::CutFadeJudge::FadeRunEndsAt(run, recs[30]));
			Assert::IsFalse(VpUnit::CutFadeJudge::FadeRunEndsAt(run, recs[31]));
			const bool skipped = VpUnit::CutFadeJudge::FoldFadeRun(recs[31], recs[32], VpUnit::CutFadeJudge::ClassifyEvents(recs[31], recs[32], false, false), run);
			Assert::IsFalse(skipped, L"one pair of a new run is no fade yet");
			Assert::AreEqual(1u, run.pairs, L"a new run started at the pair");
		}

		// A hard cut from a static dim shot directly into black stays a cut (never reclassified as a fade).
		TEST_METHOD(AHardCutFromDimShotIntoBlackStaysACut)
		{
			const double gains[] = { 0.15, 0.015 };
			const bool noisyFlags[] = { false, true };
			for (double gain : gains)
			{
				for (bool noisy : noisyFlags)
				{
					std::vector<Record> recs;
					uint32_t k = 0;
					for (int i = 0; i < 10; ++i)
						recs.push_back(Picture(gain, k++));
					for (int i = 0; i < 10; ++i)
						recs.push_back(Black(k++, noisy));

					FadeRun run;
					for (size_t i = 1; i < recs.size(); ++i)
					{
						const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
						const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run);
						if (i == 10)
						{
							Assert::IsTrue(e.cut, L"hard cut from dim shot into black must be a cut");
							Assert::IsTrue(e.cutPossible, L"hard cut into black sets cutPossible");
							Assert::IsFalse(e.fade, L"hard cut into black must not be a pair fade");
							Assert::IsFalse(runFade, L"hard cut into black must not be a run fade");
							Assert::AreEqual(0, run.direction, L"cut into black without active fade leaves no run");
							Assert::AreEqual(0u, run.pairs, L"cut into black without active fade resets run pairs");
						}
						else
						{
							Assert::IsFalse(e.fade || runFade, L"static dim or static black frames must not fade");
						}
					}
				}
			}
		}

		// A hard cut from black directly into a static dim shot stays a cut (never reclassified as a fade).
		TEST_METHOD(AHardCutFromBlackIntoDimShotStaysACut)
		{
			const double gains[] = { 0.15, 0.015 };
			for (double gain : gains)
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 10; ++i)
					recs.push_back(Black(k++));
				for (int i = 0; i < 10; ++i)
					recs.push_back(Picture(gain, k++));

				FadeRun run;
				for (size_t i = 1; i < recs.size(); ++i)
				{
					const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
					const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run);
					if (i == 10)
					{
						Assert::IsTrue(e.cut, L"hard cut from black into dim shot must be a cut");
						Assert::IsTrue(e.cutPossible, L"hard cut from black sets cutPossible");
						Assert::IsFalse(e.fade, L"hard cut from black must not be a pair fade");
						Assert::IsFalse(runFade, L"hard cut from black must not be a run fade");
						if (gain >= 0.1)
						{
							Assert::AreEqual(0, run.direction, L"large jump from black does not seed a fade run");
							Assert::AreEqual(0u, run.pairs, L"large jump from black resets run pairs");
						}
					}
					else
					{
						Assert::IsFalse(e.fade || runFade, L"static black or static dim frames must not fade");
					}
				}
			}
		}

		// A genuine multi-frame fade-out to black and fade-in back from black is still detected across the black boundaries.
		TEST_METHOD(AGenuineMultiFrameFadeToBlackAndBackFromBlackIsStillAFade)
		{
			const int pre = 10, nOut = 48, holdBlack = 12, nIn = 48, post = 10;
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < pre; ++i)
				recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= nOut; ++i)
				recs.push_back(i == nOut ? Black(k++) : Picture(1.0 - double(i) / double(nOut), k++));
			for (int i = 0; i < holdBlack; ++i)
				recs.push_back(Black(k++));
			for (int i = 1; i <= nIn; ++i)
				recs.push_back(Picture(double(i) / double(nIn), k++));
			for (int i = 0; i < post; ++i)
				recs.push_back(Picture(1.0, k++));

			const size_t outBegin = size_t(pre);
			const size_t outEnd = outBegin + size_t(nOut);
			const size_t blackEnd = outEnd + size_t(holdBlack);
			const size_t inEnd = blackEnd + size_t(nIn);

			FadeRun run;
			int staticPreFades = 0, fadeOutFades = 0, staticBlackFades = 0, fadeInFades = 0, staticPostFades = 0;
			bool hitBlackIsFade = false;
			double fadeOutKeepProd = 1.0;

			for (size_t i = 1; i < recs.size(); ++i)
			{
				const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
				double runKeep = 1.0;
				const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run, &runKeep);
				const bool isFade = e.fade || runFade;
				if (i < outBegin)
					staticPreFades += isFade ? 1 : 0;
				else if (i < outEnd)
				{
					fadeOutFades += isFade ? 1 : 0;
					if (i == outEnd - 1)
						hitBlackIsFade = isFade;
					fadeOutKeepProd *= VpUnit::CutFadeJudge::FrameFadeKeep(recs[i - 1], recs[i], e.fade, runFade, runKeep);
				}
				else if (i < blackEnd)
					staticBlackFades += isFade ? 1 : 0;
				else if (i < inEnd)
					fadeInFades += isFade ? 1 : 0;
				else
					staticPostFades += isFade ? 1 : 0;
			}

			Assert::AreEqual(0, staticPreFades, L"no fade on static pre-fade frames");
			Assert::AreEqual(0, staticBlackFades, L"no fade on static black hold frames");
			Assert::AreEqual(0, staticPostFades, L"no fade on static post-fade frames");
			Assert::IsTrue(fadeOutFades >= 46, L"genuine 48-frame fade-out to black must fire on >= 46/48 frames");
			Assert::IsTrue(hitBlackIsFade, L"final fade-out frame hitting black must stay a fade");
			Assert::IsTrue(fadeInFades >= 46, L"genuine 48-frame fade-in from black must fire on >= 46/48 frames");
			Assert::IsTrue(fadeOutKeepProd < 0.5, L"fade-out to black must decay history keep well below 0.5");
		}
	};
}
