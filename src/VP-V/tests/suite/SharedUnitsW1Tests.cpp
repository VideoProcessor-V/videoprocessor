// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// W1-SU (VP-V reset 1, wave 1): the shared units U6 SourceStats, U7 CutFadeJudge, U8 PictureRect and U12 SourceQuality.
// Every migration is proven here against a FROZEN copy of the private implementation it replaced (identity: bit-exact), the
// unit laws themselves against hand-computed cases. CPU only.

#include <VP-V/modules/input/signal/HdrInference.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/identity/SourceQualityView.h>
#include <VP-V/modules/input/source_analysis/identity/shared/SourceQuality.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/modules/input/source_analysis/picture/shared/PictureRect.h>
#include <VP-V/core/ProductCatalog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SharedUnitsW1TestsNs
{
	// ---- frozen private implementations (copied from base bcde6fc2 before the migration) ------------------------------------
	HdrVetoFrameStats FrozenMeasureHdrVetoGrid(const uint16_t* grid, int columns, int rows,
		const HdrVetoThresholds& thresholds)
	{
		HdrVetoFrameStats stats;
		if (!grid || columns <= 0 || rows <= 0)
			return stats;
		stats.measured = true;
		stats.samples = static_cast<uint32_t>(columns) * static_cast<uint32_t>(rows);

		std::array<double, HdrVetoFrameStats::GRID_COLUMNS * HdrVetoFrameStats::GRID_ROWS> sums{};
		std::array<uint32_t, HdrVetoFrameStats::GRID_COLUMNS * HdrVetoFrameStats::GRID_ROWS> counts{};
		std::array<uint32_t, HdrVetoFrameStats::HISTOGRAM_BINS> bins{};
		const uint32_t activeThreshold = std::max<uint32_t>(1, static_cast<uint32_t>(
			std::ceil(thresholds.activeRowFraction * columns)));
		int firstActive = -1;
		int lastActive = -1;
		for (int y = 0; y < rows; ++y)
		{
			const uint16_t* row = grid + static_cast<size_t>(y) * columns;
			const int cy = y * HdrVetoFrameStats::GRID_ROWS / rows;
			uint32_t aboveBlack = 0;
			for (int x = 0; x < columns; ++x)
			{
				const uint16_t code = std::min<uint16_t>(row[x], 1023);
				const int cx = x * HdrVetoFrameStats::GRID_COLUMNS / columns;
				const size_t cell = static_cast<size_t>(cy) * HdrVetoFrameStats::GRID_COLUMNS + cx;
				sums[cell] += code;
				++counts[cell];
				++bins[std::min<int>(code / 64, HdrVetoFrameStats::HISTOGRAM_BINS - 1)];
				if (code > thresholds.blackCode)
					++aboveBlack;
			}
			if (aboveBlack >= activeThreshold)
			{
				if (firstActive < 0)
					firstActive = y;
				lastActive = y;
			}
		}
		for (size_t i = 0; i < sums.size(); ++i)
			stats.grid[i] = counts[i] ? static_cast<float>(sums[i] / counts[i]) : 0.0f;
		for (size_t i = 0; i < bins.size(); ++i)
			stats.histogram[i] = static_cast<float>(bins[i]) / stats.samples;

		if (firstActive < 0 ||
			lastActive - firstActive + 1 < thresholds.minimumActiveRows * rows)
			return stats;

		std::array<uint32_t, 1024> histogram{};
		for (int y = firstActive; y <= lastActive; ++y)
		{
			const uint16_t* row = grid + static_cast<size_t>(y) * columns;
			for (int x = 0; x < columns; ++x)
				++histogram[std::min<uint16_t>(row[x], 1023)];
		}
		stats.activePicture = true;
		stats.activeSamples = static_cast<uint32_t>(lastActive - firstActive + 1) *
			static_cast<uint32_t>(columns);
		for (int code = thresholds.highCode; code < 1024; ++code)
			stats.atOrAboveHigh += histogram[code];
		stats.fractionAtHigh = static_cast<double>(stats.atOrAboveHigh) / stats.activeSamples;
		const double target = 0.999 * stats.activeSamples;
		double cumulative = 0.0;
		for (int code = 0; code < 1024; ++code)
		{
			cumulative += histogram[code];
			if (cumulative >= target)
			{
				stats.p999 = static_cast<uint16_t>(code);
				break;
			}
		}
		stats.sdrEvidence = stats.fractionAtHigh >= thresholds.highFraction ||
			stats.p999 >= thresholds.p999Code;
		return stats;
	}

	// HdrVetoCutDetector::Observe's rule, frozen.
	inline void FrozenCutRule(const HdrVetoFrameStats& stats, const HdrVetoFrameStats& previous, float& gridOut, float& histOut, bool& cut)
	{
		float grid = 0.0f;
		for (size_t i = 0; i < stats.grid.size(); ++i)
			grid += std::fabs(stats.grid[i] - previous.grid[i]);
		grid /= static_cast<float>(stats.grid.size());
		float histogram = 0.0f;
		for (size_t i = 0; i < stats.histogram.size(); ++i)
			histogram += std::fabs(stats.histogram[i] - previous.histogram[i]);
		histogram *= 0.5f;
		gridOut = grid;
		histOut = histogram;
		cut = (grid >= HdrVetoCutDetector::GRID_DIFFERENCE && histogram >= HdrVetoCutDetector::HISTOGRAM_DISTANCE) ||
			histogram >= HdrVetoCutDetector::HISTOGRAM_DISTANCE_ALONE;
	}

	// VpFrameAnalysis Percentile, frozen (histogram of uint32 over `bins` codes).
	inline uint16_t FrozenFramePercentile(const std::vector<uint32_t>& histogram, uint64_t samples, double q)
	{
		const double target = (std::max)(1.0, q * static_cast<double>(samples));
		uint64_t cumulative = 0;
		for (size_t code = 0; code < histogram.size(); ++code)
		{
			cumulative += histogram[code];
			if (static_cast<double>(cumulative) >= target)
				return static_cast<uint16_t>(code);
		}
		return static_cast<uint16_t>(histogram.size() - 1);
	}

	// The lane's HistDistance, frozen.
	inline uint32_t FrozenHistDistance(const VpUnit::SourceStats::Record& a, const VpUnit::SourceStats::Record& b)
	{
		const double na = a.pixelCount > 0 ? double(a.pixelCount) : 1.0;
		const double nb = b.pixelCount > 0 ? double(b.pixelCount) : 1.0;
		double l1 = 0.0;
		for (uint32_t i = 0; i < VpUnit::SourceStats::kHistBins; ++i)
			l1 += std::fabs(double(a.hist[i]) / na - double(b.hist[i]) / nb);
		const double frac = l1 * 0.5;
		const double v = frac * 65535.0 + 0.5;
		return v >= 65535.0 ? 65535u : uint32_t(v);
	}

	// The tier loop of SourceQualityView::TierOf / BitrateQualityVote, frozen.
	inline int FrozenTier(double quality, const std::vector<double>& edges)
	{
		int tier = 0;
		for (size_t i = 0; i < edges.size(); ++i)
			if (quality >= edges[i])
				++tier;
		return tier;
	}

	inline uint64_t Fnv1a(const std::string& text)
	{
		uint64_t h = 0xcbf29ce484222325ull;
		for (char c : text)
		{
			if (c == '\r')
				continue;   // the checkout's line ending must not matter
			h ^= static_cast<uint8_t>(c);
			h *= 0x100000001b3ull;
		}
		return h;
	}

	inline std::vector<uint16_t> RandomGrid(std::mt19937& rng, int columns, int rows, int barRows)
	{
		std::vector<uint16_t> g(static_cast<size_t>(columns) * rows);
		std::uniform_int_distribution<int> code(0, 1100);
		for (int y = 0; y < rows; ++y)
			for (int x = 0; x < columns; ++x)
				g[static_cast<size_t>(y) * columns + x] = (y < barRows || y >= rows - barRows) ? uint16_t(64 + (code(rng) & 7)) : uint16_t(code(rng));
		return g;
	}

	inline VpUnit::SourceStats::Record RandomRecord(std::mt19937& rng, bool concentrated)
	{
		VpUnit::SourceStats::Record r;
		std::uniform_int_distribution<int> bin(0, 1023), cnt(0, 40);
		uint64_t n = 0;
		for (int i = 0; i < 300; ++i)
		{
			const uint32_t b = concentrated ? uint32_t(300 + (bin(rng) & 63)) : uint32_t(bin(rng));
			const uint32_t c = uint32_t(cnt(rng));
			r.hist[b] += c;
			n += c;
		}
		r.pixelCount = static_cast<uint32_t>(n);
		return r;
	}
}

namespace SharedUnitsW1TestsNs2
{
	using namespace SharedUnitsW1TestsNs;
	using PRect = VpUnit::PictureRect::Rect;

	TEST_CLASS(SharedUnitsW1Tests)
	{
	public:
		TEST_METHOD(UnitVersionsAreStampedAndMatchTheCatalog)
		{
			Assert::AreEqual(VpSkeleton::Catalog::kExpectedStatsUnit, VpUnit::SourceStats::kUnitVersion);
			Assert::AreEqual(1u, VpUnit::CutFadeJudge::kUnitVersion);
			Assert::AreEqual(1u, VpUnit::PictureRect::kUnitVersion);
			Assert::AreEqual(1u, VpUnit::SourceQuality::kUnitVersion);
			Assert::AreEqual(VpUnit::SourceStats::kUnitVersion, VpUnit::SourceStats::kRecordVersion);
		}

		// ---- U6 / U7: the veto grid ------------------------------------------------------------------------------------------
		TEST_METHOD(VetoGridMeasureIsIdenticalToTheFrozenCopy)
		{
			std::mt19937 rng(20261107);
			const int sizes[][3] = { { 16, 9, 0 }, { 64, 36, 4 }, { 100, 57, 8 }, { 33, 21, 0 }, { 320, 180, 20 } };
			for (const auto& s : sizes)
				for (int rep = 0; rep < 6; ++rep)
				{
					const std::vector<uint16_t> g = RandomGrid(rng, s[0], s[1], rep % 2 ? s[2] : 0);
					const HdrVetoFrameStats a = MeasureHdrVetoGrid(g.data(), s[0], s[1]);
					const HdrVetoFrameStats b = FrozenMeasureHdrVetoGrid(g.data(), s[0], s[1], HdrVetoThresholds());
					Assert::AreEqual(b.measured, a.measured);
					Assert::AreEqual(b.activePicture, a.activePicture);
					Assert::AreEqual(b.sdrEvidence, a.sdrEvidence);
					Assert::AreEqual(b.samples, a.samples);
					Assert::AreEqual(b.activeSamples, a.activeSamples);
					Assert::AreEqual(b.atOrAboveHigh, a.atOrAboveHigh);
					Assert::AreEqual(b.fractionAtHigh, a.fractionAtHigh);
					Assert::AreEqual(b.p999, a.p999);
					for (size_t i = 0; i < a.grid.size(); ++i)
						Assert::AreEqual(b.grid[i], a.grid[i]);
					for (size_t i = 0; i < a.histogram.size(); ++i)
						Assert::AreEqual(b.histogram[i], a.histogram[i]);
				}
		}

		TEST_METHOD(VetoCutDetectorRuleIsIdenticalToTheFrozenCopy)
		{
			std::mt19937 rng(7);
			int cuts = 0, steady = 0;
			for (int rep = 0; rep < 200; ++rep)
			{
				// a pair: an unrelated frame, or the same frame with a little noise
				const std::vector<uint16_t> g1 = RandomGrid(rng, 64, 36, 4);
				std::vector<uint16_t> g2 = g1;
				if (rep % 3 == 0)   // an unrelated, much darker picture: a cut
					for (auto& c : g2)
						c = static_cast<uint16_t>(c / 4);
				if (rep % 3 != 0)
					for (size_t i = 0; i < g2.size(); i += 7)
						g2[i] = static_cast<uint16_t>((std::min)(1023, g2[i] + (rep % 5)));
				const HdrVetoFrameStats s1 = MeasureHdrVetoGrid(g1.data(), 64, 36);
				const HdrVetoFrameStats s2 = MeasureHdrVetoGrid(g2.data(), 64, 36);
				HdrVetoCutDetector det;
				det.Observe(s1);
				const bool cut = det.Observe(s2);
				if (s1.activePicture != s2.activePicture)
				{
					Assert::IsTrue(cut, L"entering or leaving black is a cut");
					continue;
				}
				if (!s2.activePicture)
					continue;
				float grid = 0, hist = 0;
				bool frozenCut = false;
				FrozenCutRule(s2, s1, grid, hist, frozenCut);
				Assert::AreEqual(frozenCut, cut);
				Assert::AreEqual(grid, det.LastGridDifference());
				Assert::AreEqual(hist, det.LastHistogramDistance());
				(frozenCut ? cuts : steady)++;
			}
			Assert::IsTrue(cuts > 10 && steady > 10, (L"both outcomes must occur: cuts " + std::to_wstring(cuts) + L" steady " + std::to_wstring(steady)).c_str());
		}

		// ---- U6: percentile scan and the lane histogram distance --------------------------------------------------------------
		TEST_METHOD(PercentileScanIsIdenticalToTheFrozenCopy)
		{
			std::mt19937 rng(11);
			std::uniform_int_distribution<int> cnt(0, 9);
			for (int rep = 0; rep < 40; ++rep)
			{
				std::vector<uint32_t> h(1024, 0);
				uint64_t n = 0;
				for (size_t i = 0; i < h.size(); ++i)
					if (rng() % 4 == 0)
					{
						h[i] = static_cast<uint32_t>(cnt(rng));
						n += h[i];
					}
				if (n == 0)
					continue;
				for (double q : { 0.0, 0.01, 0.5, 0.99, 0.999, 1.0 })
				{
					const double target = (std::max)(1.0, q * static_cast<double>(n));
					const int c = VpUnit::SourceStats::FirstBinReaching<double>(h.data(), 1024, target);
					const uint16_t unit = static_cast<uint16_t>(c >= 0 ? c : 1023);
					Assert::AreEqual(FrozenFramePercentile(h, n, q), unit);
				}
			}
		}

		TEST_METHOD(LaneStatsRecordPercentilesAreBinCentres)
		{
			// 1000 samples, a flat ramp over 16-bit codes: p50 sits at the centre of the bin holding sample 500
			std::vector<uint16_t> half(1000);
			for (size_t i = 0; i < half.size(); ++i)
				half[i] = static_cast<uint16_t>(i * 65);
			std::vector<uint8_t> out(VpUnit::SourceStats::kRecordBytes, 0);
			Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), 40, 25, out.data(), out.size()));
			VpUnit::SourceStats::Record r;
			Assert::IsTrue(VpUnit::SourceStats::ReadRecord(out.data(), out.size(), &r));
			Assert::AreEqual(1000u, r.pixelCount);
			Assert::AreEqual(uint32_t((half[499] >> 6) << 6 | 32u), r.p50);
			Assert::IsTrue(r.p01 < r.p50 && r.p50 < r.p99);
			Assert::IsFalse(VpUnit::SourceStats::ComputeRecord(half.data(), 40, 25, out.data(), 16), L"too small a buffer");
		}

		TEST_METHOD(HistBin10MatchesTheVetoBinLawAndTheLaneShift)
		{
			for (uint32_t code = 0; code < 1200; ++code)
			{
				Assert::AreEqual((uint32_t)(std::min)(int(code) / 64, 15), VpUnit::SourceStats::HistBin10(code, 16));
				Assert::AreEqual((uint32_t)(std::min)(code, 1023u), VpUnit::SourceStats::HistBin10(code, 1024));
			}
		}

		// ---- U7 ---------------------------------------------------------------------------------------------------------------
		TEST_METHOD(RecordPairHistDistanceIsIdenticalToTheFrozenCopy)
		{
			std::mt19937 rng(23);
			for (int rep = 0; rep < 60; ++rep)
			{
				const VpUnit::SourceStats::Record a = RandomRecord(rng, rep % 2 == 0);
				const VpUnit::SourceStats::Record b = RandomRecord(rng, rep % 3 == 0);
				Assert::AreEqual(FrozenHistDistance(a, b), VpUnit::CutFadeJudge::HistDistance(a, b));
			}
			const VpUnit::SourceStats::Record empty;
			Assert::AreEqual(FrozenHistDistance(empty, empty), VpUnit::CutFadeJudge::HistDistance(empty, empty));
		}

		TEST_METHOD(CutFadeJudgeClassifiesCutFadeAndFlash)
		{
			using namespace VpUnit::CutFadeJudge;
			VpUnit::SourceStats::Record dark, bright;
			dark.pixelCount = bright.pixelCount = 1000;
			dark.hist[100] = 1000;
			bright.hist[800] = 1000;
			dark.p01 = 6000; dark.p99 = 8000; dark.mean = 7000;
			bright.p01 = 50000; bright.p99 = 60000; bright.mean = 55000;
			EventSignals e = ClassifyEvents(dark, bright, false, false);
			Assert::IsTrue(e.cut && e.cutPossible && !e.flash);
			Assert::IsTrue(StrengthOf(e) == Strength::Full);
			e = ClassifyEvents(dark, dark, true, false);
			Assert::IsTrue(e.cut, L"a FULL indicator between the frames is a cut");
			e = ClassifyEvents(dark, dark, false, true);
			Assert::IsTrue(!e.cut && e.cutPossible && StrengthOf(e) == Strength::Minor);
			e = ClassifyEvents(dark, dark, false, false);
			Assert::IsTrue(!e.cut && !e.cutPossible && !e.fade && StrengthOf(e) == Strength::None);
			// a shifted histogram of unchanged spread: a flash
			VpUnit::SourceStats::Record shifted = dark;
			std::fill(std::begin(shifted.hist), std::end(shifted.hist), 0u);
			shifted.hist[700] = 1000;
			shifted.mean = 7000 + 10000;
			shifted.p01 = dark.p01 + 10000; shifted.p99 = dark.p99 + 10000;
			e = ClassifyEvents(dark, shifted, false, false);
			Assert::IsTrue(e.flash && !e.cut && e.cutPossible);
		}

		TEST_METHOD(GridHistRuleAndMeanAbsDifference)
		{
			using namespace VpUnit::CutFadeJudge;
			const float a[4] = { 1, 2, 3, 4 }, b[4] = { 2, 2, 5, 0 };
			Assert::AreEqual(7.0f / 4.0f, MeanAbsDifference(a, b, 4));
			GridHistThresholds t;
			Assert::IsTrue(GridHistCut(30.0f, 0.2f, t));
			Assert::IsFalse(GridHistCut(30.0f, 0.1f, t));
			Assert::IsFalse(GridHistCut(10.0f, 0.2f, t));
			Assert::IsTrue(GridHistCut(0.0f, 0.4f, t), L"histogram alone");
		}

		TEST_METHOD(FadeFitFollowsAFadeAndNotAPan)
		{
			using namespace VpUnit::CutFadeJudge;
			FadeIn fade;   // B = 0.9 x A about black 16: a fade down by 10 percent
			fade.meanA = 100; fade.spreadA = 30;
			fade.meanB = 16 + 0.9f * (100 - 16); fade.spreadB = 27;
			fade.previousStep = fade.meanB - fade.meanA;
			FadeOut o = FadeFit(fade);
			Assert::IsTrue(o.fading);
			Assert::IsTrue(std::fabs(o.gain - 30.0f / 27.0f) < 1e-5f);
			Assert::IsTrue(std::fabs((o.gain * fade.meanB + o.offset) - fade.meanA) < 1e-3f, L"gain and offset map B back to A");
			FadeParams off;
			off.enabled = false;
			Assert::IsFalse(FadeFit(fade, off).fading, L"the switch");
			FadeIn pan = fade;   // the mean moves, the spread stays: the implied black is implausible
			pan.spreadB = 30;
			Assert::IsFalse(FadeFit(pan).fading);
			FadeIn reversed = fade;   // the step reverses against the previous one
			reversed.previousStep = -reversed.previousStep;
			Assert::IsFalse(FadeFit(reversed).fading);
		}

		// ---- U8 ---------------------------------------------------------------------------------------------------------------
		TEST_METHOD(PictureRectFindsLetterboxAndHonoursTheGivenRect)
		{
			using namespace VpUnit::PictureRect;
			const int w = 40, h = 24;   // a 1/8 plane of a 320 x 192 frame
			std::vector<float> plane(static_cast<size_t>(w) * h, 16.0f);
			for (int y = 3; y < 21; ++y)
				for (int x = 0; x < w; ++x)
					plane[static_cast<size_t>(y) * w + x] = 90.0f;
			In in;
			in.plane = plane.data(); in.planeW = w; in.planeH = h; in.frameW = 320; in.frameH = 192;
			PRect r = Compute(in, ForRange(false));
			Assert::AreEqual(0.0f, r.x0); Assert::AreEqual(24.0f, r.y0); Assert::AreEqual(320.0f, r.x1); Assert::AreEqual(168.0f, r.y1);
			// bars at full-range black 0: the same plane is all picture (16 > 0 + 4)
			r = Compute(in, ForRange(true));
			Assert::AreEqual(0.0f, r.y0); Assert::AreEqual(192.0f, r.y1);
			// a given rect wins and is clamped to the frame
			in.given.x0 = -5; in.given.y0 = 10; in.given.x1 = 400; in.given.y1 = 180;
			r = Compute(in, ForRange(false));
			Assert::AreEqual(0.0f, r.x0); Assert::AreEqual(10.0f, r.y0); Assert::AreEqual(320.0f, r.x1); Assert::AreEqual(180.0f, r.y1);
			// no bar, a fade to black: all picture
			std::vector<float> black(static_cast<size_t>(w) * h, 16.0f);
			In dark;
			dark.plane = black.data(); dark.planeW = w; dark.planeH = h; dark.frameW = 320; dark.frameH = 192;
			r = Compute(dark, ForRange(false));
			Assert::AreEqual(0.0f, r.x0); Assert::AreEqual(0.0f, r.y0); Assert::AreEqual(320.0f, r.x1); Assert::AreEqual(192.0f, r.y1);
		}

		TEST_METHOD(PictureRectReaders)
		{
			using namespace VpUnit::PictureRect;
			PRect r;
			r.x0 = 0; r.y0 = 24; r.x1 = 320; r.y1 = 168;
			Assert::IsTrue(InPicture(r, 10, 24));
			Assert::IsFalse(InPicture(r, 10, 168));
			Assert::AreEqual(1.0f, Coverage(r, 0, 32, 32, 64));
			Assert::AreEqual(0.0f, Coverage(r, 0, 0, 32, 16));
			Assert::AreEqual(0.75f, Coverage(r, 0, 16, 32, 48));   // rows 24..48 of 16..48: 24 of 32 inside
			const PRect whole = Effective(PRect(), 320, 192);
			Assert::AreEqual(320.0f, whole.x1); Assert::AreEqual(192.0f, whole.y1);
			const int in[4] = { 5, 7, 11, 13 };
			int half[4];
			Halved(in, half);
			Assert::AreEqual(2, half[0]); Assert::AreEqual(3, half[1]); Assert::AreEqual(6, half[2]); Assert::AreEqual(7, half[3]);
			Assert::IsTrue(Glsl().find("vpu_in_picture") != std::string::npos);
		}

		TEST_METHOD(PictureKernelTextIsTheTextTheMatcherCarried)
		{
			// FNV-1a (carriage returns ignored) of PICTURE_HEADER / PICTURE_BODY as they stood in DejudderWarp.cpp at bcde6fc2.
			Assert::AreEqual(0xefcbe9a17545f64cull, Fnv1a(VpUnit::PictureRect::KernelHeader()));
			Assert::AreEqual(0x6b50f3e422483fbeull, Fnv1a(VpUnit::PictureRect::KernelBody()));
		}

		// ---- U12 --------------------------------------------------------------------------------------------------------------
		TEST_METHOD(QualityTierIsIdenticalToTheFrozenLoop)
		{
			const std::vector<double> edges = { 0.25, 0.5, 0.75 };
			SourceIdentity::VoteSettings vs;
			for (int i = -5; i <= 105; ++i)
			{
				const double q = i / 100.0;
				Assert::AreEqual(FrozenTier(q, edges), VpUnit::SourceQuality::TierOf(q, edges));
				Assert::AreEqual(FrozenTier(q, vs.qualityTierEdges), SourceIdentity::TierOf(q, vs));
			}
		}

		TEST_METHOD(QualityComputeClassicalAndConsensus)
		{
			using namespace VpUnit::SourceQuality;
			Params p;
			In in;
			in.reading = true; in.bitrateQuality = 0.6;
			Out o;
			Compute(in, p, o);
			Assert::IsFalse(o.consensus);
			Assert::AreEqual(0.6, o.quality);
			Assert::AreEqual(2, o.tier);
			Assert::AreEqual(0.6, o.confidence);
			in.reading = false;
			Compute(in, p, o);
			Assert::AreEqual(-1, o.tier);
			Assert::AreEqual(0.0, o.confidence);
			in.reading = true; in.consensus = true; in.recordLevel = 0.4; in.recordTier = 1; in.recordConfidence = 0.8; in.abrDropSeen = true;
			Compute(in, p, o);
			Assert::IsTrue(o.consensus && o.abrDropSeen);
			Assert::AreEqual(0.4, o.quality, L"min(classical, level)");
			Assert::AreEqual(1, o.tier);
			Assert::AreEqual(0.8, o.confidence);
			Assert::AreEqual(0.0, DitherNeed(1.2));
			Assert::AreEqual(1.0, DitherNeed(-0.1));
			Assert::IsFalse(ContrastRecoveryActive(3, 0.95, p), L"a clean top-tier source: get out of the way");
			Assert::IsTrue(ContrastRecoveryActive(3, 0.8, p));
			Assert::IsTrue(ContrastRecoveryActive(2, 0.95, p));
		}

		TEST_METHOD(QualityViewWithoutConsensusEqualsTheUnitClassicalOutput)
		{
			SourceIdentity::VoteSettings vs;
			for (int i = 0; i <= 100; i += 5)
			{
				SourceIdentity::ClassicalQuality snap;
				snap.reading = true;
				snap.bitrateQuality = i / 100.0;
				const SourceIdentity::QualityView v = SourceIdentity::View(nullptr, snap, false, 0, vs);
				VpUnit::SourceQuality::In in;
				in.reading = true; in.bitrateQuality = snap.bitrateQuality;
				VpUnit::SourceQuality::Out o;
				VpUnit::SourceQuality::Compute(in, VpUnit::SourceQuality::Params(), o);
				Assert::AreEqual(o.quality, v.quality);
				Assert::AreEqual(o.tier, v.tier);
				Assert::AreEqual(VpUnit::SourceQuality::DitherNeed(v.quality), SourceIdentity::DitherNeed(v));
			}
		}

		TEST_METHOD(QualitySnapshotAccessors)
		{
			struct Snap { double bitrateQuality = 1.0; double effectiveBits = 0.0; } s;
			VpUnit::SourceQuality::Store(s, 0.7, 5.5);
			Assert::AreEqual(0.7, s.bitrateQuality);
			Assert::AreEqual(5.5, s.effectiveBits);
			Assert::AreEqual(0.7, VpUnit::SourceQuality::Of(s));
			SourceIdentity::ClassicalReading r;
			VpUnit::SourceQuality::CopyClassical(r, s);
			Assert::AreEqual(0.7, r.bitrateQuality);
		}
	};
}
