/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Slice 21 W21-2: the CPU analysis consumers on the 12-bit tap. Reduce (histogram bins on the 1024-code axis, Q6 mean / min / max /
// percentile companions), the source-resolution look-ahead's normalisation by the sampler's code range, the CodingEvidence step-derived
// constants (R1/R2) and the LED value table read at a Q6 value. 10-bit (and lower) sources are bit-identical by construction: the
// scale factors are exactly 1 there. New classes in a new file (rule 28: no existing test, tolerance or baseline is edited). CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/CodedLumaSampler.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/regions/CodingEvidence.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AnalysisWideningConsumerTestsNs
{
	using namespace SyntheticAnalysisFrames;
	namespace Axis = Vpv::AnalysisDomain;

	// 4096 r210 words (alignment 2, big endian): pixel `code` carries intensity code, I = hi8 << 4 | lo4. Limited range, so the tap is the code.
	// `mappedCodes`: pixel `code` carries FullLumaToLimited12(code) instead (what a full-range tap of the same ramp is converted to).
	struct Ramp
	{
		std::vector<uint8_t> bytes;
		AnalysisLumaSource source;
		explicit Ramp(bool mappedCodes = false)
		{
			const int width = 64, height = 64;
			bytes.assign(static_cast<size_t>(width) * height * 4, 0);
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const uint32_t intensity = mappedCodes ? DvTunnelTapLaw::FullLumaToLimited12(static_cast<int>(code)) : code;
				const uint32_t word = ((intensity >> 4) << 10) | (intensity & 15u);
				for (unsigned k = 0; k < 4; ++k) bytes[code * 4 + k] = static_cast<uint8_t>(word >> (24 - 8 * k));
			}
			source.format = AnalysisLumaFormat::PackedDolbyVision422;
			source.encoding = VideoFrameEncoding::R210;
			source.width = width;
			source.height = height;
			source.rgbBitAlignment = 2;
			source.fullRangeYuv = false;
			source.data = bytes.data();
			source.dataBytes = bytes.size();
			source.rowBytes = static_cast<size_t>(width) * 4;
		}
	};

	// Every published field of two Reduce results is equal (the histogram, the counts, both scales of every statistic, the occupancy).
	bool SameMeasured(const VpFrameAnalysis::Measured& a, const VpFrameAnalysis::Measured& b)
	{
		const FrameAnalysis::LumaStats& x = a.luma;
		const FrameAnalysis::LumaStats& y = b.luma;
		return x.histogram == y.histogram && x.samples == y.samples && x.mean == y.mean && x.apl == y.apl &&
			x.minimum == y.minimum && x.p1 == y.p1 && x.p50 == y.p50 && x.p99 == y.p99 && x.maximum == y.maximum &&
			x.minQ6 == y.minQ6 && x.p1Q6 == y.p1Q6 && x.p50Q6 == y.p50Q6 && x.p99Q6 == y.p99Q6 && x.maxQ6 == y.maxQ6 &&
			x.meanQ6 == y.meanQ6 && a.range.btb == b.range.btb && a.range.wtw == b.range.wtw &&
			a.range.nearBlack == b.range.nearBlack && a.range.clipped == b.range.clipped;
	}

	// An independent ten-bit oracle for Reduce: every pixel is read through Sample() (not the shared plane, LumaRowReader or Reducer),
	// the studio code is the range law's (the identity unless the sampled codes are full range), and the statistics come straight from
	// their definitions: the percentile is the smallest code whose cumulative count reaches q of the samples (at least one sample).
	struct TenBitOracle
	{
		std::vector<uint32_t> histogram = std::vector<uint32_t>(1024, 0);
		uint64_t samples = 0;
		int minimum = 1023, maximum = 0, p1 = 0, p50 = 0, p99 = 0;
		double mean = 0.0, apl = 0.0;

		bool Build(const AnalysisLumaSource& source)
		{
			const bool full = source.CodesFullRange();
			uint64_t sum = 0;
			for (int y = 0; y < source.height; ++y)
				for (int x = 0; x < source.width; ++x)
				{
					AnalysisLumaSample sample;
					if (!source.Sample(x, y, sample)) return false;
					const int code = full ? Vpv::AnalysisDomain::FullLumaToLimited(sample.luma, 1) : sample.luma;
					++histogram[static_cast<size_t>(code)];
					sum += static_cast<uint64_t>(code);
					++samples;
				}
			if (samples == 0) return false;
			for (int code = 0; code < 1024; ++code)
				if (histogram[static_cast<size_t>(code)])
				{
					minimum = (std::min)(minimum, code);
					maximum = (std::max)(maximum, code);
				}
			mean = static_cast<double>(sum) / static_cast<double>(samples);
			apl = (mean - 64.0) / 876.0;
			p1 = Percentile(1, 100);
			p50 = Percentile(1, 2);
			p99 = Percentile(99, 100);
			return true;
		}

		int Percentile(uint64_t numerator, uint64_t denominator) const
		{
			uint64_t cumulative = 0;
			for (int code = 0; code < 1024; ++code)
			{
				cumulative += histogram[static_cast<size_t>(code)];
				if (cumulative > 0 && cumulative * denominator >= numerator * samples)
					return code;
			}
			return 1023;
		}
	};

	// One 64 x 64 tile of banded content: every 8 x 8 block holds two levels `step` axis codes apart; block bases climb 16 axis codes per
	// block. `bits` 10 writes the axis codes; 12 writes the same tile at four times the codes (the same content on the deeper axis).
	std::vector<float> BandedTile(int bits)
	{
		const int n = 64, scale = bits == 12 ? 4 : 1;
		const float maximum = bits == 12 ? 4095.0f : 1023.0f;
		std::vector<float> tile(static_cast<size_t>(n) * n);
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const int base = 200 + 16 * ((x / 8) + (y / 8));
				const int code10 = base + (x % 8 >= 4 ? 2 : 0);
				tile[static_cast<size_t>(y) * n + x] = static_cast<float>(code10 * scale) / maximum;
			}
		return tile;
	}

	// The same tile geometry with a chosen native step between the two levels of every block (step native codes = step / scale axis codes).
	std::vector<float> StepTile(int bits, int stepNative)
	{
		const int n = 64, scale = bits == 12 ? 4 : 1;
		const float maximum = bits == 12 ? 4095.0f : 1023.0f;
		std::vector<float> tile(static_cast<size_t>(n) * n);
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const int base = (200 + 16 * ((x / 8) + (y / 8))) * scale;
				tile[static_cast<size_t>(y) * n + x] = static_cast<float>(base + (x % 8 >= 4 ? stepNative : 0)) / maximum;
			}
		return tile;
	}

	TEST_CLASS(AnalysisWideningConsumerTests)
	{
	public:
		// Frozen literal oracle (not derived through the production path): a P010 frame whose luma is 64 + 8 * (x mod 16), 16 levels of
		// 24 * 216 samples each. Reduce must read exactly these literals.
		TEST_METHOD(TenBitFrozenLiteralStats)
		{
			Frame frame = Make(Kind::P010, 0);
			std::vector<uint8_t>& data = *frame.bytes;
			for (int y = 0; y < kHeight; ++y)
				for (int x = 0; x < kWidth; ++x)
					Put16(&data[static_cast<size_t>(y) * frame.source.rowBytes + static_cast<size_t>(x) * 2],
						static_cast<uint16_t>((64 + 8 * (x % 16)) << 6));
			VpFrameAnalysis::Measured m;
			Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, FrameAnalysis::Rect{ 0, 0, kWidth, kHeight }, m));
			Assert::AreEqual<uint64_t>(82944, m.luma.samples);
			for (int bin = 0; bin < 1024; ++bin)
			{
				const bool used = bin >= 64 && bin <= 184 && ((bin - 64) % 8) == 0;
				Assert::AreEqual<uint32_t>(used ? 5184u : 0u, m.luma.histogram[static_cast<size_t>(bin)]);
			}
			Assert::AreEqual<int>(64, m.luma.minimum);
			Assert::AreEqual<int>(184, m.luma.maximum);
			Assert::AreEqual(124.0, m.luma.mean, 1e-12);
			Assert::AreEqual(60.0 / 876.0, m.luma.apl, 1e-12);
			Assert::AreEqual<uint32_t>(64u << 6, m.luma.minQ6);
			Assert::AreEqual<uint32_t>(184u << 6, m.luma.maxQ6);
			Assert::AreEqual(124.0 * 64.0, m.luma.meanQ6, 1e-9);
		}

		// The discriminating thresholds: a band is a 2..8 AXIS-code step. At 12 bit one axis code is 4 native codes: a 4-native step is
		// NOT a band, an 8-native (2 axis code) step IS; the 10-bit tile reads the same (1 code no, 2 codes yes).
		TEST_METHOD(TwelveBitStepThresholdsAreAxisCodes)
		{
			const int n = 64;
			const auto edges = [n](int bits, int step) {
				const std::vector<float> t = StepTile(bits, step);
				return VpUnit::CodingEvidence::MeasureTile(t.data(), n, n, bits == 12 ? 4095 : 1023).bandEdges;
			};
			Assert::AreEqual(0, edges(10, 1), L"one axis code is no band at 10 bits");
			Assert::AreEqual(64, edges(10, 2), L"two axis codes are a band at 10 bits");
			Assert::AreEqual(0, edges(12, 4), L"one axis code (4 native) is no band at 12 bits");
			Assert::AreEqual(64, edges(12, 8), L"two axis codes (8 native) are a band at 12 bits");
			Assert::AreEqual(64, edges(12, 32), L"eight axis codes (32 native) are still a band");
			Assert::AreEqual(0, edges(12, 36), L"nine axis codes are above the band range");
		}

		// W6: the DV ramp through Reduce. NOTE: no production consumer of the DV effective-bits reading exists yet (W21-4 adds it), so Reduce
		// is the consumer under test here. Reduce reads the same whether it decodes the tap itself or is handed the frame's one decode:
		// both give the exact 12-bit extremes and mean, and the two results are equal field for field. The 12-bit view uses all 4096
		// codes (12 effective bits); the 1024-bin axis histogram Reduce publishes reads 10.
		TEST_METHOD(EffectiveBitsThroughReduceOnDvRamp)
		{
			Ramp ramp;
			const FrameAnalysis::Rect picture = { 0, 0, 64, 64 };
			VpSkeleton::SourceFrameView view;
			Assert::IsTrue(VpFrameAnalysis::SourceViewOf12(ramp.source, view));
			VpFrameAnalysis::Measured own, shared;
			Assert::IsTrue(VpFrameAnalysis::Reduce(ramp.source, picture, own));
			Assert::IsTrue(VpFrameAnalysis::Reduce(ramp.source, picture, shared, &view));
			const VpFrameAnalysis::Measured* both[2] = { &own, &shared };
			for (const VpFrameAnalysis::Measured* m : both)
			{
				Assert::AreEqual<uint32_t>(65520, m->luma.maxQ6, L"4095 << 4");
				Assert::AreEqual<uint32_t>(0, m->luma.minQ6);
				Assert::AreEqual(32760.0, m->luma.meanQ6, 1e-9);
				Assert::AreEqual<uint64_t>(4096, m->luma.samples);
			}
			Assert::IsTrue(SameMeasured(own, shared), L"Reduce's own decode and the shared view read identically");
			std::vector<unsigned long long> h12(4096, 0), axis(shared.luma.histogram.begin(), shared.luma.histogram.end());
			for (uint32_t i = 0; i < 4096u; ++i)
				++h12[view.luma[i]];
			Assert::AreEqual(12.0, VpUnit::CodingEvidence::EffectiveBitsOf(h12), 1e-9);
			Assert::AreEqual(10.0, VpUnit::CodingEvidence::EffectiveBitsOf(axis), 1e-9);
		}

		// W6: a DV frame that is full range by its range tag alone (fullRangeYuv false: the tap itself is not converted) gets the same full ->
		// limited law at 12 bits that the ten-bit read applies: Reduce equals Reduce over a ramp whose codes are FullLumaToLimited12(c),
		// the histogram is the one built from those codes' bins, and every bin stays within one code of the ten-bit pathway's.
		TEST_METHOD(FullRangeTagReducesThroughTheTwelveBitMap)
		{
			Ramp raw;
			raw.source.range = AnalysisLumaRange::Full;
			Ramp mapped(true);
			Assert::IsTrue(raw.source.CodesFullRange(), L"full range by the tag alone");
			Assert::IsFalse(mapped.source.CodesFullRange());
			const FrameAnalysis::Rect picture = { 0, 0, 64, 64 };
			VpFrameAnalysis::Measured a, b;
			Assert::IsTrue(VpFrameAnalysis::Reduce(raw.source, picture, a));
			Assert::IsTrue(VpFrameAnalysis::Reduce(mapped.source, picture, b));
			Assert::IsTrue(SameMeasured(a, b), L"the tag's map is FullLumaToLimited12 on the tap codes");
			TenBitOracle ten;
			Assert::IsTrue(ten.Build(raw.source));
			std::vector<uint32_t> bins(1024, 0);
			int worstBin = 0;
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const int bin12 = DvTunnelTapLaw::FullLumaToLimited12(static_cast<int>(code)) >> 2;
				++bins[static_cast<size_t>(bin12)];
				AnalysisLumaSample sample;
				Assert::IsTrue(raw.source.Sample(static_cast<int>(code % 64), static_cast<int>(code / 64), sample));
				worstBin = (std::max)(worstBin, std::abs(bin12 - static_cast<int>(Vpv::AnalysisDomain::FullLumaToLimited(sample.luma, 1))));
			}
			for (size_t bin = 0; bin < 1024; ++bin)
				Assert::AreEqual<uint32_t>(bins[bin], a.luma.histogram[bin], L"the histogram bin is the 12-bit code's bin");
			Assert::IsTrue(worstBin <= 1, L"every sample's bin is within one code of the ten-bit pathway's");
			Assert::IsTrue(std::abs(static_cast<int>(a.luma.minimum) - ten.minimum) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(a.luma.maximum) - ten.maximum) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(a.luma.p1) - ten.p1) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(a.luma.p50) - ten.p50) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(a.luma.p99) - ten.p99) <= 1);
			Assert::IsTrue(std::fabs(a.luma.mean - ten.mean) <= 1.0, L"the mean is within one code of the ten-bit pathway's");
			Assert::IsTrue(a.luma.maxQ6 > a.luma.minQ6, L"the ramp is not flat");
		}

		// W4: every non-DV source reads exactly as before the widening. The oracle is independent of the producer: every pixel through
		// Sample(), the statistics from their definitions (TenBitOracle). Its Q6 companions are exactly code * 64 (an integer-code source has
		// no sub-code part). NOTE: literals frozen from the base commit need a run of the base build (the P010 literal is
		// TenBitFrozenLiteralStats).
		TEST_METHOD(TenBitSourcesBitIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (kind == Kind::PackedDvR210) continue;
				Frame frame = Make(kind, 3);
				const FrameAnalysis::Rect picture = { 0, 0, kWidth, kHeight };
				TenBitOracle want;
				Assert::IsTrue(want.Build(frame.source), KindName(kind));
				Assert::IsTrue(want.minimum < want.maximum, KindName(kind));
				VpFrameAnalysis::Measured got;
				Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, picture, got), KindName(kind));
				Assert::AreEqual<uint64_t>(want.samples, got.luma.samples, KindName(kind));
				int binMismatches = 0;
				for (size_t bin = 0; bin < 1024; ++bin)
					if (want.histogram[bin] != got.luma.histogram[bin]) ++binMismatches;
				Assert::AreEqual(0, binMismatches, KindName(kind));
				Assert::IsTrue(std::fabs(want.mean - got.luma.mean) < 1e-12, KindName(kind));
				Assert::IsTrue(std::fabs(want.apl - got.luma.apl) < 1e-12, KindName(kind));
				Assert::AreEqual(want.minimum, static_cast<int>(got.luma.minimum), KindName(kind));
				Assert::AreEqual(want.maximum, static_cast<int>(got.luma.maximum), KindName(kind));
				Assert::AreEqual(want.p1, static_cast<int>(got.luma.p1), KindName(kind));
				Assert::AreEqual(want.p50, static_cast<int>(got.luma.p50), KindName(kind));
				Assert::AreEqual(want.p99, static_cast<int>(got.luma.p99), KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(want.minimum) << 6, got.luma.minQ6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(want.maximum) << 6, got.luma.maxQ6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(want.p1) << 6, got.luma.p1Q6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(want.p50) << 6, got.luma.p50Q6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(want.p99) << 6, got.luma.p99Q6, KindName(kind));
				Assert::IsTrue(std::fabs(got.luma.meanQ6 - want.mean * 64.0) < 1e-9, KindName(kind));
			}
			// The look-ahead's normalisation at 1023 is exactly the former `code / 1023.0`.
			for (int code = 0; code <= 1023; ++code)
				Assert::IsTrue(SourceResolutionMonitor::NormalisedLuma(code, 1023) == static_cast<double>(code) / 1023.0);
		}

		// W4: the source-resolution look-ahead (the SourceResolutionHookTests look-ahead fixture, three scenarios) gives an identical
		// result when fed through CodedLumaSampler{read, 1023} as through the plain sampler: both monitors see the same frames in lockstep,
		// every auto decision and the look-ahead state agree, and the plain monitor still shows the fixture's known outcomes (engaged at the
		// cut, cancelled on a native next shot, engaged on a black frame), so the comparison is not between two idle monitors.
		TEST_METHOD(LookAheadCodedSamplerAtTenBitsEqualsPlainSampler)
		{
			using Monitor = SourceResolutionMonitor::Monitor;
			const int FW = 1024, FH = 512;
			enum class Shot { Upscaled, Native, Black };
			auto makeFrame = [&](Shot kind, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::normal_distribution<double> noise(0.0, 0.08);
				std::vector<double> f(static_cast<size_t>(FW) * FH, 64.0 / 1023.0);
				if (kind == Shot::Black) return f;
				if (kind == Shot::Native)
				{
					for (double& v : f) v = level + noise(rng);
					return f;
				}
				const int lw = FW / 2, lh = FH / 2;
				std::vector<double> low(static_cast<size_t>(lw) * lh);
				for (double& v : low) v = level + noise(rng);
				for (int y = 0; y < FH; ++y)
					for (int x = 0; x < FW; ++x)
					{
						const double u = (x + 0.5) * 0.5 - 0.5, w = (y + 0.5) * 0.5 - 0.5;
						const int x0 = (std::max)(0, (std::min)(lw - 2, static_cast<int>(std::floor(u))));
						const int y0 = (std::max)(0, (std::min)(lh - 2, static_cast<int>(std::floor(w))));
						const double fx = (std::min)(1.0, (std::max)(0.0, u - x0)), fy = (std::min)(1.0, (std::max)(0.0, w - y0));
						auto at = [&](int xx, int yy) { return low[static_cast<size_t>(yy) * lw + xx]; };
						f[static_cast<size_t>(y) * FW + x] = (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
					}
				return f;
			};
			auto code = [](double v) { return static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v)) * 1023.0)); };
			const int T = Monitor::kTile;
			auto analyse = [&](Monitor& m, const std::vector<double>& f, double t) {
				std::vector<float> tiles(static_cast<size_t>(8) * T * T);
				for (int i = 0; i < 8; ++i)
				{
					const int x0 = (i % 4) * (FW - T) / 3, y0 = (i / 4) * (FH - T);
					for (int y = 0; y < T; ++y)
						for (int x = 0; x < T; ++x)
							tiles[(static_cast<size_t>(i) * T + y) * T + x] = static_cast<float>(code(f[static_cast<size_t>(y0 + y) * FW + x0 + x]) / 1023.0);
				}
				m.AnalyseTilesForTest(tiles, 8, t, FW, FH);
			};
			auto aheadBoth = [&](Monitor& plain, Monitor& coded, uint64_t seq, const std::vector<double>& f) {
				auto read = [&f, &code, FW](int x, int y) { return code(f[static_cast<size_t>(y) * FW + x]); };
				const Monitor::LumaSampler plainSampler = read;
				plain.LookAheadFrame(seq, FW, FH, plainSampler, 0, 0, FW, FH);
				SourceResolutionMonitor::CodedLumaSampler tenBit;
				tenBit.read = read;
				tenBit.codeMaximum = 1023;
				coded.LookAheadFrame(seq, FW, FH, tenBit, 0, 0, FW, FH);
			};
			auto waitReady = [](Monitor& m) {
				for (int tries = 0; tries < 400 && !m.Ahead().ready; ++tries) std::this_thread::sleep_for(std::chrono::milliseconds(5));
				return m.Ahead().ready;
			};
			auto sameAhead = [](const Monitor& a, const Monitor& b) {
				const Monitor::AheadState x = a.Ahead(), y = b.Ahead();
				return x.eventSequence == y.eventSequence && x.targetSequence == y.targetSequence && x.ready == y.ready &&
					x.informative == y.informative && std::fabs(x.ratio - y.ratio) < 1e-12 && x.verified == y.verified && x.cancelled == y.cancelled;
			};
			// Upscaled shots, fed to both monitors in lockstep, until auto waits for the next cut; returns the time reached.
			auto untilWaiting = [&](Monitor& plain, Monitor& coded, uint64_t& seq) {
				double t = 0.0;
				std::string whyPlain, whyCoded;
				for (int shot = 0; shot < 12; ++shot)
					for (int i = 0; i < 8; ++i)
					{
						const std::vector<double> f = makeFrame(Shot::Upscaled, 50 + shot * 13 + i, shot % 2 ? 0.55 : 0.35);
						analyse(plain, f, t);
						analyse(coded, f, t);
						const double a = plain.AutoFactor(FH, 0.8, whyPlain, t, ++seq);
						const double b = coded.AutoFactor(FH, 0.8, whyCoded, t, seq);
						Assert::AreEqual(a, b, 1e-12, L"the same factor while the shots run");
						Assert::AreEqual(whyPlain, whyCoded);
						t += 0.5;
						if (whyPlain == "waiting for the next cut or black") return t;
					}
				return -1.0;
			};
			for (int scenario = 1; scenario <= 3; ++scenario)
			{
				Monitor plain, coded;
				plain.SetLatchFixes(false);
				coded.SetLatchFixes(false);
				uint64_t seq = 0;
				const double t = untilWaiting(plain, coded, seq);
				Assert::IsTrue(t > 0.0, L"auto reached waiting");
				std::string whyPlain, whyCoded;
				double fPlain = 0.0, fCoded = 0.0;
				if (scenario == 1)
				{
					aheadBoth(plain, coded, seq + 1, makeFrame(Shot::Upscaled, 999, 0.35));
					aheadBoth(plain, coded, seq + 2, makeFrame(Shot::Upscaled, 1000, 0.75));
				}
				else if (scenario == 2)
				{
					aheadBoth(plain, coded, seq + 1, makeFrame(Shot::Upscaled, 1999, 0.35));
					aheadBoth(plain, coded, seq + 2, makeFrame(Shot::Native, 2000, 0.75));
				}
				else
				{
					const std::vector<double> black = makeFrame(Shot::Black, 0, 0.0);
					aheadBoth(plain, coded, seq + 1, black);
					aheadBoth(plain, coded, seq + 2, black);
					aheadBoth(plain, coded, seq + 3, makeFrame(Shot::Upscaled, 3000, 0.45));
				}
				Assert::IsTrue(waitReady(plain), L"the plain monitor analysed the frame ahead");
				Assert::IsTrue(waitReady(coded), L"the coded monitor analysed the frame ahead");
				Assert::IsTrue(sameAhead(plain, coded), L"identical look-ahead state");
				if (scenario == 1)
				{
					Assert::AreEqual(1.0, plain.AutoFactor(FH, 0.8, whyPlain, t, seq + 1), 1e-9);
					Assert::AreEqual(1.0, coded.AutoFactor(FH, 0.8, whyCoded, t, seq + 1), 1e-9);
					fPlain = plain.AutoFactor(FH, 0.8, whyPlain, t + 0.04, seq + 2);
					fCoded = coded.AutoFactor(FH, 0.8, whyCoded, t + 0.04, seq + 2);
					Assert::AreEqual(0.5, fPlain, 1e-9, L"engaged on the cut frame");
					Assert::AreEqual(std::string("engaged at the cut / black (look-ahead verified)"), whyPlain);
					Assert::AreEqual<unsigned long long>(1ull, plain.Ahead().verified);
				}
				else if (scenario == 2)
				{
					Assert::AreEqual(1.0, plain.AutoFactor(FH, 0.8, whyPlain, t, seq + 1), 1e-9);
					Assert::AreEqual(1.0, coded.AutoFactor(FH, 0.8, whyCoded, t, seq + 1), 1e-9);
					fPlain = plain.AutoFactor(FH, 0.8, whyPlain, t, seq + 2);
					fCoded = coded.AutoFactor(FH, 0.8, whyCoded, t, seq + 2);
					Assert::AreEqual(1.0, fPlain, 1e-9, L"stays native");
					Assert::AreEqual(std::string("look-ahead: the next shot disagrees - staying"), whyPlain);
					Assert::AreEqual<unsigned long long>(1ull, plain.Ahead().cancelled);
				}
				else
				{
					fPlain = plain.AutoFactor(FH, 0.8, whyPlain, t, seq + 1);
					fCoded = coded.AutoFactor(FH, 0.8, whyCoded, t, seq + 1);
					Assert::AreEqual(0.5, fPlain, 1e-9, L"engaged on the black frame");
				}
				Assert::AreEqual(fPlain, fCoded, 1e-12, L"the same decision");
				Assert::AreEqual(whyPlain, whyCoded, L"the same reason");
				Assert::IsTrue(sameAhead(plain, coded), L"identical look-ahead state after the decision");
			}
		}

		// The 12-bit DV ramp (one pixel per tap code, 4096 codes): the
		// histogram is one bin per whole axis code (four tap codes each), the Q6 mean is the exact sub-code mean, min / max are exact,
		// and the percentiles rise with the picture.
		TEST_METHOD(TwelveBitRampMonotonic)
		{
			Ramp ramp;
			VpFrameAnalysis::Measured m;
			Assert::IsTrue(VpFrameAnalysis::Reduce(ramp.source, FrameAnalysis::Rect{ 0, 0, 64, 64 }, m));
			Assert::AreEqual<uint64_t>(4096, m.luma.samples);
			for (int bin = 0; bin < 1024; ++bin)
				Assert::AreEqual<uint32_t>(4, m.luma.histogram[static_cast<size_t>(bin)], L"four tap codes per axis code");
			Assert::AreEqual<uint32_t>(0, m.luma.minQ6);
			Assert::AreEqual<uint32_t>(4095u << 4, m.luma.maxQ6);
			Assert::AreEqual(32760.0, m.luma.meanQ6, 1e-9);                 // (0 + ... + 4095) / 4096 = 2047.5 tap codes, x 16
			Assert::AreEqual(32760.0 / 64.0, m.luma.mean, 1e-9);            // the axis-code mean carries the sub-code part
			Assert::AreEqual((32760.0 / 64.0 - 64.0) / 876.0, m.luma.apl, 1e-9);
			Assert::IsTrue(m.luma.minQ6 <= m.luma.p1Q6 && m.luma.p1Q6 <= m.luma.p50Q6 && m.luma.p50Q6 <= m.luma.p99Q6 &&
				m.luma.p99Q6 <= m.luma.maxQ6);
			Assert::IsTrue(std::abs(static_cast<int>(m.luma.p1) - 10) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(m.luma.p50) - 511) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(m.luma.p99) - 1013) <= 1);
			// Each percentile's Q6 value is the centre of the bin it falls in.
			Assert::AreEqual<uint32_t>(Axis::BinCenterQ6(m.luma.p50), m.luma.p50Q6);

			// The look-ahead's normalisation: monotone across the whole 12-bit range, white = 1.
			double last = -1.0;
			for (int code = 0; code <= 4095; ++code)
			{
				const double v = SourceResolutionMonitor::NormalisedLuma(code, 4095);
				Assert::IsTrue(v > last);
				last = v;
			}
			Assert::AreEqual(1.0, SourceResolutionMonitor::NormalisedLuma(4095, 4095), 1e-15);

			// CodingEvidence: a tile that grows tap code by tap code reads a monotone gradient.
			const int n = 64;
			double lastGradient = -1.0;
			for (int slope = 1; slope <= 4; ++slope)
			{
				std::vector<float> tile(static_cast<size_t>(n) * n);
				for (int y = 0; y < n; ++y)
					for (int x = 0; x < n; ++x)
						tile[static_cast<size_t>(y) * n + x] = static_cast<float>(1000 + slope * x) / 4095.0f;
				const double g = VpUnit::CodingEvidence::MeasureTile(tile.data(), n, n, 4095).gradient;
				Assert::IsTrue(g > lastGradient);
				lastGradient = g;
			}
		}

		// R1/R2: the same banded content written at 10-bit and at four times the codes at 12-bit reads the same blocks, the same band
		// edges (minimum 2 / maximum 8 axis codes = 8 / 32 native) and, scaled by four, the same gradient; the blockiness ratio is equal
		// (the quarter-code floor scales with the codes). Nothing about 10-bit moves: the scale is exactly 1 there.
		TEST_METHOD(TwelveBitScaleKeepsAxisMeaning)
		{
			const int n = 64;
			const std::vector<float> t10 = BandedTile(10), t12 = BandedTile(12);
			const VpUnit::CodingEvidence::TileMeasure a = VpUnit::CodingEvidence::MeasureTile(t10.data(), n, n, 1023);
			const VpUnit::CodingEvidence::TileMeasure b = VpUnit::CodingEvidence::MeasureTile(t12.data(), n, n, 4095);
			Assert::AreEqual(64, a.blocks);
			Assert::AreEqual(a.blocks, b.blocks);
			Assert::AreEqual(64, a.bandEdges, L"every block is a two-level band edge");
			Assert::AreEqual(a.bandEdges, b.bandEdges);
			Assert::AreEqual(a.flatBlocks, b.flatBlocks);
			Assert::AreEqual(a.bandEdgesRobust, b.bandEdgesRobust);
			Assert::AreEqual(a.gradient * 4.0, b.gradient, 1e-9);
			Assert::AreEqual(a.blockiness, b.blockiness, 1e-9);
		}
	};

	TEST_CLASS(LedWideTableTests)
	{
	public:
		// LedInterpolationIntegerCodesUnchanged: the table read at an integer axis code (Q6 = code * 64, or a 12-bit code that is a multiple
		// of four) is the table's own entry bit for bit; the 10-bit lane (shift 6) reproduces the table; a 12-bit code between two whole
		// codes lands between their entries, monotone when the table is.
		TEST_METHOD(LedInterpolationIntegerCodesUnchanged)
		{
			LedDrive::LumaValueTable table{};
			for (int code = 0; code < LedDrive::LUMA_CODES; ++code)
				table[static_cast<size_t>(code)] = static_cast<float>(std::pow(code / 1023.0, 1.7));
			const std::vector<float> lane10 = LedDrive::WideValueTable(table, 1023, 6);
			Assert::AreEqual<size_t>(1024, lane10.size());
			for (int code = 0; code < 1024; ++code)
			{
				Assert::IsTrue(lane10[static_cast<size_t>(code)] == table[static_cast<size_t>(code)]);
				Assert::IsTrue(LedDrive::TableValueQ6(table, static_cast<uint32_t>(code) << 6) == table[static_cast<size_t>(code)]);
			}
			const std::vector<float> tap12 = LedDrive::WideValueTable(table, 4095, 4);
			Assert::AreEqual<size_t>(4096, tap12.size());
			for (int code12 = 0; code12 < 4096; ++code12)
			{
				const float v = tap12[static_cast<size_t>(code12)];
				const int whole = code12 >> 2;
				if ((code12 & 3) == 0)
					Assert::IsTrue(v == table[static_cast<size_t>(whole)]);
				else
					Assert::IsTrue(v >= table[static_cast<size_t>(whole)] && v <= table[static_cast<size_t>(whole + 1)]);
				if (code12 > 0)
					Assert::IsTrue(v >= tap12[static_cast<size_t>(code12 - 1)]);
			}
			// Past the last whole code the last entry is held.
			Assert::IsTrue(LedDrive::TableValueQ6(table, Axis::kAxisQ6Max + 63u) == table[1023]);
		}
	};
}
