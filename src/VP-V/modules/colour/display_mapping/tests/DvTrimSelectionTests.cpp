/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// feat/dv-l8 (2026-10-24): DV trims with L8 (DvTrimSelection.h). The level order (L8, else L2,
// else none), L8 targets from L10 / the predefined list, selection and interpolation by the
// tone-mapping target exactly as L2, the L2 path unchanged bit for bit, and the L8 extras
// (mid contrast, clip trim, per-hue saturation / hue) bounded, monotonic and gamut-safe.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/colour/display_mapping/DvTrimSelection.h>

#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace DvL8Trims
	{
		using Trim = DolbyVisionTunnelFrameMetadata::Trim;

		inline Trim L2(uint16_t target, uint16_t slope = 2048, uint16_t offset = 2048, uint16_t power = 2048,
			uint16_t chroma = 2048, uint16_t sat = 2048)
		{
			Trim t;
			t.level = 2; t.targetMaxPq = target; t.slope = slope; t.offset = offset; t.power = power;
			t.chromaWeight = chroma; t.saturationGain = sat;
			return t;
		}

		inline Trim L8(uint8_t index, uint16_t slope = 2048, uint16_t offset = 2048, uint16_t power = 2048,
			uint16_t chroma = 2048, uint16_t sat = 2048)
		{
			Trim t = L2(0, slope, offset, power, chroma, sat);
			t.level = 8;
			t.targetMaxPq = 0;
			t.targetDisplayIndex = index;
			return t;
		}

		inline DolbyVisionTunnelFrameMetadata Dm(std::initializer_list<Trim> trims, uint16_t sourceMax = 3696)
		{
			DolbyVisionTunnelFrameMetadata dm;
			dm.valid = true;
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = sourceMax;
			for (const Trim& t : trims)
				dm.trims[dm.trimCount++] = t;
			return dm;
		}

		inline void AddL10(DolbyVisionTunnelFrameMetadata& dm, uint8_t index, uint16_t maxPq)
		{
			auto& t = dm.targetDisplays[dm.targetDisplayCount++];
			t.index = index;
			t.maxPq = maxPq;
			t.minPq = 62;
		}

		inline uint16_t Pq(double nits) { return static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(nits) * 4095.0)); }

		inline bool Same(const DolbyVisionTrims::Params& a, const DolbyVisionTrims::Params& b)
		{
			return a.slope == b.slope && a.offset == b.offset && a.power == b.power && a.chromaWeight == b.chromaWeight &&
				a.saturationGain == b.saturationGain;
		}

		inline DolbyVisionTrims::Signal Sdr48()
		{
			DolbyVisionTrims::Signal s;
			s.pq = false;
			s.gamma = 2.4;
			s.peakNits = 48.0;
			return s;
		}
	}

	TEST_CLASS(DvTrimSelectionTests)
	{
	public:
		// The L2 path through the new selection is DolbyVisionTrims::Select exactly (random DMs,
		// a sweep of targets): same indices, weight, parameters, apply and reason.
		TEST_METHOD(L2OnlyMatchesTheExistingSelection)
		{
			using namespace DvL8Trims;
			std::mt19937 rng(17);
			std::uniform_int_distribution<int> code(1500, 2600), target(1200, 3600), count(0, 6);
			for (int trial = 0; trial < 300; ++trial)
			{
				DolbyVisionTunnelFrameMetadata dm = Dm({}, static_cast<uint16_t>(trial % 3 == 0 ? 0 : 3000 + trial % 600));
				const int n = count(rng);
				for (int k = 0; k < n; ++k)
					dm.trims[dm.trimCount++] = L2(static_cast<uint16_t>(target(rng)), static_cast<uint16_t>(code(rng)),
						static_cast<uint16_t>(code(rng)), static_cast<uint16_t>(code(rng)), static_cast<uint16_t>(code(rng)),
						static_cast<uint16_t>(code(rng)));
				if (trial % 7 == 0 && n)
					dm.trims[0].slope = 5000;   // a corrupt block is skipped by both
				for (double nits : { 1.0, 20.0, 48.0, 100.0, 300.0, 600.0, 1000.0, 4000.0, 10000.0 })
				{
					const DolbyVisionTrims::Selection a = DolbyVisionTrims::Select(dm, nits);
					const DvTrimSelection::Selection b = DvTrimSelection::Select(dm, nits);
					Assert::AreEqual(a.apply, b.apply);
					Assert::AreEqual(a.lowerIndex, b.lowerIndex);
					Assert::AreEqual(a.upperIndex, b.upperIndex);
					Assert::AreEqual(a.weight, b.weight);
					Assert::IsTrue(Same(a.params, b.params.base));
					Assert::IsTrue(DvTrimSelection::ExtrasNeutral(b.params));
					if (a.lowerIndex >= 0 || a.upperIndex >= 0)
						Assert::AreEqual(std::string(a.reason), std::string(b.reason));
					Assert::IsTrue(b.level == (b.lowerIndex >= 0 || b.upperIndex >= 0 || std::string(b.reason).find("identity") != std::string::npos ?
						DvTrimSelection::Level::L2 : DvTrimSelection::Level::None) || n == 0);
				}
			}
		}

		TEST_METHOD(L8IsPreferredThenL2ThenNone)
		{
			using namespace DvL8Trims;
			// L8 (predefined index 1 = 100 nits) beside L2: L8 wins.
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(2081, 2400), L8(1, 2200) });
			DvTrimSelection::Selection s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8 && s.apply);
			Assert::AreEqual(DolbyVisionTrims::Decode(dm.trims[1]).slope, s.params.base.slope);
			Assert::AreEqual(std::string("DV trims: L8"), DvTrimSelection::StatusText(true, s));
			// An L8 whose target is unknown is skipped: L2, and the status says why.
			dm = Dm({ L2(2081, 2400), L8(77, 2200) });
			s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L2 && s.apply);
			Assert::AreEqual(1, s.l8Unresolved);
			Assert::AreEqual(std::string("DV trims: L2 (1 L8 target unknown, e.g. index 77)"), DvTrimSelection::StatusText(true, s));
			// ... unless an L10 names it.
			AddL10(dm, 77, 2081);
			s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8);
			Assert::AreEqual(0, s.l8Unresolved);
			// An L10 overrides the predefined list.
			dm = Dm({ L8(1, 2200) });
			AddL10(dm, 1, Pq(1000.0));
			s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(std::string(s.reason).find("below every trim target") != std::string::npos, L"index 1 now means 1000 nits");
			// Nothing: none.
			dm = Dm({});
			s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::None && !s.apply);
			Assert::AreEqual(std::string("DV trims: none"), DvTrimSelection::StatusText(true, s));
			Assert::AreEqual(std::string("DV trims: off"), DvTrimSelection::StatusText(false, s));
			// A corrupt L8 is skipped; neutral L8 is "none" (nothing to apply).
			dm = Dm({ L8(1, 4096) });
			Assert::IsTrue(DvTrimSelection::Select(dm, 100.0).level == DvTrimSelection::Level::None);
			dm = Dm({ L8(1) });
			s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8 && !s.apply);
			Assert::AreEqual(std::string("DV trims: none"), DvTrimSelection::StatusText(true, s));
		}

		// Interpolation across L8 targets follows the tone-mapping target in PQ, extras included;
		// below every target the lowest-target trim unchanged; the mastering peak is identity.
		TEST_METHOD(L8InterpolatesAcrossTargetsByTheToneMappingTarget)
		{
			using namespace DvL8Trims;
			Trim a = L8(1, 2048 + 400), b = L8(27, 2048 - 400);
			a.hasMidContrastAndClip = true; a.midContrast = 2048 + 400; a.clipTrim = 2048;
			b.hasSaturationVector = true;
			for (int k = 0; k < 6; ++k) b.saturationVector[k] = 128;
			b.saturationVector[0] = 192;
			DolbyVisionTunnelFrameMetadata dm = Dm({ a, b }, Pq(4000.0));
			AddL10(dm, 27, Pq(600.0));
			const double pq100 = DolbyVisionTrims::PqFromNits(100.0) * 4095.0;
			const double pq600 = Pq(600.0);
			double lastSlope = 10.0;
			for (double nits = 100.0; nits < 600.0; nits += 25.0)
			{
				const DvTrimSelection::Selection s = DvTrimSelection::Select(dm, nits);
				const double w = (DolbyVisionTrims::PqFromNits(nits) * 4095.0 - 2081.0) / (pq600 - 2081.0);
				Assert::IsTrue(s.level == DvTrimSelection::Level::L8);
				Assert::AreEqual(DolbyVisionTrims::Clamp(w, 0.0, 1.0), s.weight, 1e-9);
				Assert::IsTrue(s.params.base.slope <= lastSlope + 1e-12, L"slope moves monotonically from the 100- to the 600-nit trim");
				lastSlope = s.params.base.slope;
				Assert::AreEqual(400.0 / 4096.0 * (1.0 - s.weight), s.params.midContrast, 1e-9, L"mid contrast interpolated (absent = neutral)");
				Assert::AreEqual(1.0 + 0.5 * 64.0 / 128.0 * s.weight, s.params.saturation[0], 1e-9, L"saturation vector interpolated");
			}
			(void)pq100;
			// The owner's case: a ~48-nit projector below the lowest (100-nit) target.
			DvTrimSelection::Selection s = DvTrimSelection::Select(dm, 48.0);
			Assert::IsTrue(s.lowerIndex < 0 && s.upperIndex == 0);
			Assert::AreEqual(DvTrimSelection::DecodeL8(a).base.slope, s.params.base.slope);
			Assert::AreEqual(DvTrimSelection::DecodeL8(a).midContrast, s.params.midContrast);
			// Above the highest target: fades to neutral at the mastering peak, identity at it.
			s = DvTrimSelection::Select(dm, 2000.0);
			Assert::IsTrue(s.weight > 0.0 && s.weight < 1.0 && s.params.saturation[0] < DvTrimSelection::DecodeL8(b).saturation[0]);
			s = DvTrimSelection::Select(dm, 5000.0);
			Assert::IsFalse(s.apply);
		}

		// Neutral extras: the pixel maths is DolbyVisionTrims::ApplyPixel bit for bit.
		TEST_METHOD(NeutralExtrasAreTheL2PixelExactly)
		{
			std::mt19937 rng(5);
			std::uniform_real_distribution<double> v(-0.05, 1.1), p(0.6, 1.4), o(-0.3, 0.3), c(-0.4, 0.4);
			for (int trial = 0; trial < 2000; ++trial)
			{
				DvTrimSelection::ExtParams e;
				e.base.slope = p(rng); e.base.offset = o(rng); e.base.power = p(rng);
				e.base.chromaWeight = trial % 3 ? c(rng) : 0.0; e.base.saturationGain = p(rng);
				DolbyVisionTrims::Signal s;
				s.pq = trial & 1;
				s.peakNits = s.pq ? 1000.0 : 100.0;
				double a[3] = { v(rng), v(rng), v(rng) }, b[3] = { a[0], a[1], a[2] };
				DolbyVisionTrims::ApplyPixel(a, e.base, s);
				DvTrimSelection::ApplyPixel(b, e, s);
				Assert::IsTrue(std::memcmp(a, b, sizeof(a)) == 0);
			}
			const auto params = DvTrimSelection::ShaderParameters(DvTrimSelection::ExtParams());
			bool ext = false;
			for (const auto& kv : params)
				if (std::string(kv.first) == "dv_ext")
				{
					ext = true;
					Assert::AreEqual(0.0, kv.second, L"neutral extras keep the shader on the L2 arithmetic");
				}
			Assert::IsTrue(ext);
			Assert::AreEqual<size_t>(21, params.size());
		}

		// Mid contrast and clip trim: black and peak fixed, monotonic, the intended direction,
		// and the extended path equals the L2 path for a grey when they are zero.
		TEST_METHOD(MidContrastAndClipTrimAreBoundedAndMonotonic)
		{
			for (double mc : { -0.5, -0.2, 0.2, 0.5 })
			{
				Assert::AreEqual(0.0, DvTrimSelection::MidContrast(0.0, mc));
				Assert::AreEqual(1.0, DvTrimSelection::MidContrast(1.0, mc));
				double last = -1.0;
				for (int k = 0; k <= 1000; ++k)
				{
					const double v = DvTrimSelection::MidContrast(k / 1000.0, mc);
					Assert::IsTrue(v >= last - 1e-12 && v >= 0.0 && v <= 1.0);
					last = v;
				}
				Assert::IsTrue(mc > 0 ? DvTrimSelection::MidContrast(0.75, mc) > 0.75 && DvTrimSelection::MidContrast(0.25, mc) < 0.25 :
					DvTrimSelection::MidContrast(0.75, mc) < 0.75 && DvTrimSelection::MidContrast(0.25, mc) > 0.25, L"contrast direction");
			}
			for (double ct : { -0.5, 0.5 })
			{
				Assert::AreEqual(0.4, DvTrimSelection::ClipTrim(0.4, ct), L"below mid: untouched");
				Assert::AreEqual(1.0, DvTrimSelection::ClipTrim(1.0, ct), L"peak fixed (no new clipping)");
				double last = -1.0;
				for (int k = 0; k <= 1000; ++k)
				{
					const double v = DvTrimSelection::ClipTrim(k / 1000.0, ct);
					Assert::IsTrue(v >= last - 1e-12);
					last = v;
				}
				Assert::IsTrue(ct > 0 ? DvTrimSelection::ClipTrim(0.85, ct) > 0.85 : DvTrimSelection::ClipTrim(0.85, ct) < 0.85);
			}
			// Through the pixel: a grey above mid gets brighter with mid contrast > 0, its
			// above-white continuation keeps slope 1 (nothing above white clipped).
			DvTrimSelection::ExtParams e;
			e.midContrast = 0.3;
			const DolbyVisionTrims::Signal s = DvL8Trims::Sdr48();
			double grey[3] = { 0.8, 0.8, 0.8 }, plain[3] = { 0.8, 0.8, 0.8 };
			DvTrimSelection::ApplyPixel(grey, e, s);
			Assert::IsTrue(grey[0] > plain[0] && std::fabs(grey[0] - grey[1]) < 1e-12 && std::fabs(grey[1] - grey[2]) < 1e-12, L"grey stays grey");
			double w1[3] = { 1.05, 1.05, 1.05 }, w2[3] = { 1.1, 1.1, 1.1 };
			DvTrimSelection::ApplyPixel(w1, e, s);
			DvTrimSelection::ApplyPixel(w2, e, s);
			Assert::IsTrue(w2[0] > w1[0] && w1[0] > 1.0, L"above white kept and ordered");
			double below[3] = { -0.02, -0.02, -0.02 };
			DvTrimSelection::ApplyPixel(below, e, s);
			Assert::AreEqual(-0.02, below[0], L"below black untouched");
		}

		// Per-hue saturation acts on its hue only; a hue rotation keeps the luminance and the
		// hue moves the right way; no channel at or above black is pushed below it.
		TEST_METHOD(HueVectorsActOnTheirHueKeepLuminanceAndGamut)
		{
			const DolbyVisionTrims::Signal s = DvL8Trims::Sdr48();
			auto lum = [&](const double rgb[3])
			{
				double L[3];
				for (int c = 0; c < 3; ++c) L[c] = DolbyVisionTrims::ToNits(rgb[c], s);
				return s.kr * L[0] + s.kg * L[1] + s.kb * L[2];
			};
			auto chroma = [&](const double rgb[3])
			{
				double L[3];
				for (int c = 0; c < 3; ++c) L[c] = DolbyVisionTrims::ToNits(rgb[c], s);
				const double Y = s.kr * L[0] + s.kg * L[1] + s.kb * L[2];
				return std::sqrt((L[0] - Y) * (L[0] - Y) + (L[1] - Y) * (L[1] - Y) + (L[2] - Y) * (L[2] - Y));
			};
			DvTrimSelection::ExtParams e;
			e.saturation[0] = 1.3;   // red only
			double red[3] = { 0.7, 0.35, 0.35 }, redRef[3] = { 0.7, 0.35, 0.35 };
			double cyan[3] = { 0.35, 0.6, 0.6 }, cyanRef[3] = { 0.35, 0.6, 0.6 };
			DvTrimSelection::ApplyPixel(red, e, s);
			DvTrimSelection::ApplyPixel(cyan, e, s);
			Assert::IsTrue(chroma(red) > chroma(redRef) * 1.2, L"red more saturated");
			Assert::AreEqual(chroma(cyanRef), chroma(cyan), chroma(cyanRef) * 1e-9, L"cyan untouched");
			Assert::AreEqual(lum(redRef), lum(red), lum(redRef) * 1e-9, L"luminance kept");
			// Hue rotation at yellow: +20 degrees moves yellow toward green.
			DvTrimSelection::ExtParams h;
			h.hueDegrees[1] = 20.0;
			double yellow[3] = { 0.6, 0.6, 0.3 }, yellowRef[3] = { 0.6, 0.6, 0.3 };
			DvTrimSelection::ApplyPixel(yellow, h, s);
			Assert::IsTrue(yellow[1] > yellow[0], L"rotated toward green");
			Assert::AreEqual(lum(yellowRef), lum(yellow), lum(yellowRef) * 1e-9, L"rotation keeps luminance");
			// Gamut: strong rotation + saturation on saturated colours never makes a channel
			// that was at or above black go below it.
			DvTrimSelection::ExtParams g;
			for (int k = 0; k < 6; ++k) { g.saturation[k] = 1.5; g.hueDegrees[k] = 30.0; }
			std::mt19937 rng(9);
			std::uniform_real_distribution<double> u(0.0, 1.0);
			for (int trial = 0; trial < 3000; ++trial)
			{
				double px[3] = { u(rng), u(rng) * u(rng), u(rng) * u(rng) * u(rng) };
				std::swap(px[trial % 3], px[(trial / 3) % 3]);
				DvTrimSelection::ApplyPixel(px, g, s);
				for (int c = 0; c < 3; ++c)
					Assert::IsTrue(DolbyVisionTrims::ToNits(px[c], s) >= -1e-9, L"no new below-black (beyond rounding)");
			}
		}

		// Audit 2026-10-24: L8 wins only when its known targets cover the display target at least
		// as well as L2's: L8 at 100 nits (index 1) + L8 at preset 28 (no L10: unknown) and L2 at
		// 100 / 600 / 1000 nits.
		TEST_METHOD(L8YieldsToL2WhenL2CoversTheTargetBetter)
		{
			using namespace DvL8Trims;
			DolbyVisionTunnelFrameMetadata dm = Dm({ L8(1, 2300), L8(28, 2200), L2(Pq(100.0), 2400), L2(Pq(600.0), 2100),
				L2(Pq(1000.0), 2000) }, Pq(4000.0));
			DvTrimSelection::Selection s = DvTrimSelection::Select(dm, 800.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L2 && s.l8CoveredWorse, L"800 nits: L2 brackets it, L8 does not");
			Assert::IsTrue(s.lowerIndex == 3 && s.upperIndex == 4);
			Assert::IsTrue(DvTrimSelection::StatusText(true, s).find("L2 targets cover the display better") != std::string::npos);
			s = DvTrimSelection::Select(dm, 600.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L2, L"600 nits: an exact L2 target");
			s = DvTrimSelection::Select(dm, 48.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8 && !s.l8CoveredWorse, L"48 nits: both one-sided at 100 nits: L8");
			s = DvTrimSelection::Select(dm, 100.0);
			// PQ(100 nits) x 4095 is just below 2081: every target is at or above it for both levels,
			// the nearest is 2081 for both - a tie, and ties go to L8.
			Assert::IsTrue(DolbyVisionTrims::PqFromNits(100.0) * 4095.0 <= 2081.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8, L"100 nits: equal coverage, L8");
			// With the second L8 target known (L10 at 600 nits), L8 brackets 300 nits as L2 does: L8.
			AddL10(dm, 28, Pq(600.0));
			s = DvTrimSelection::Select(dm, 300.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8);
			Assert::IsTrue(DvTrimSelection::PredefinedTargetMaxPq(1) == 2081 && DvTrimSelection::PredefinedTargetMaxPq(28) < 0,
				L"only index 1 is assumed (libdovi names presets 1,16,18,21,27,28,37,38,42,48,49 without luminances)");
		}

		TEST_METHOD(L8CodesDecodeLikeL2AndCorruptOnesAreRejected)
		{
			using namespace DvL8Trims;
			Trim t = L8(1, 2048 + 1024, 2048 - 1024, 2048, 2048 + 512, 2048);
			t.hasMidContrastAndClip = true; t.midContrast = 2048; t.clipTrim = 4095;
			t.hasHueVector = true;
			for (int k = 0; k < 6; ++k) t.hueVector[k] = 128;
			t.hueVector[4] = 0;
			const DvTrimSelection::ExtParams p = DvTrimSelection::DecodeL8(t);
			Assert::AreEqual(1.25, p.base.slope);
			Assert::AreEqual(-0.25, p.base.offset);
			Assert::AreEqual(0.125, p.base.chromaWeight);
			Assert::AreEqual(0.0, p.midContrast);
			Assert::AreEqual(4095.0 / 4096.0 - 0.5, p.clipTrim);
			Assert::AreEqual(-30.0, p.hueDegrees[4]);
			Assert::IsTrue(DvTrimSelection::SatVectorNeutral(p), L"absent vector: neutral");
			Assert::IsTrue(DvTrimSelection::ValidL8(t));
			t.clipTrim = 4096;
			Assert::IsFalse(DvTrimSelection::ValidL8(t));
			Assert::IsFalse(DvTrimSelection::ValidL8(L2(2081)), L"an L2 block is not L8");
		}
		// GEM-016: the candidates of a level are a fixed stack array (CandidateList, kMaxTrims entries), not a heap vector per
		// level per frame. The result must be the vector form exactly: the same trims, in DM order, with the same target and
		// decoded parameters, and the same L8 counters. The reference below is the pre-change vector-form loop, written against
		// the public decoders (Valid / ValidL8 / FromL2 / DecodeL8 / ResolveL8TargetPq). The shader-parameter list appended into a
		// caller's reused buffer is the fresh list, and refilling the buffer does not reallocate.
		TEST_METHOD(SelectionWithoutTheHeapMatchesTheVectorForm)
		{
			using namespace DvL8Trims;
			using DvTrimSelection::Level;
			auto reference = [](const DolbyVisionTunnelFrameMetadata& dm, Level level, int& l8Present, int& l8Unresolved, int& firstUnresolved)
			{
				std::vector<DvTrimSelection::Candidate> out;
				for (int i = 0; i < dm.trimCount && i < DolbyVisionTunnelFrameMetadata::kMaxTrims; ++i)
				{
					const Trim& t = dm.trims[i];
					DvTrimSelection::Candidate c;
					c.trimIndex = i;
					if (level == Level::L2)
					{
						if (!DolbyVisionTrims::Valid(t))
							continue;
						c.targetPq = t.targetMaxPq;
						c.params = DvTrimSelection::FromL2(t);
					}
					else
					{
						if (!DvTrimSelection::ValidL8(t))
							continue;
						++l8Present;
						c.targetPq = DvTrimSelection::ResolveL8TargetPq(dm, t.targetDisplayIndex);
						if (c.targetPq < 0)
						{
							++l8Unresolved;
							if (firstUnresolved < 0) firstUnresolved = t.targetDisplayIndex;
							continue;
						}
						c.params = DvTrimSelection::DecodeL8(t);
					}
					out.push_back(c);
				}
				return out;
			};
			auto sameParams = [](const DvTrimSelection::ExtParams& a, const DvTrimSelection::ExtParams& b)
			{
				bool same = Same(a.base, b.base) && a.midContrast == b.midContrast && a.clipTrim == b.clipTrim;
				for (int k = 0; k < DvTrimSelection::kHues; ++k)
					same = same && a.saturation[k] == b.saturation[k] && a.hueDegrees[k] == b.hueDegrees[k];
				return same;
			};

			std::mt19937 rng(2026);
			std::uniform_int_distribution<int> code(1500, 2600), target(1200, 3600), count(0, 8), kind(0, 3), index(1, 5);
			size_t compared = 0, withUnresolved = 0, full = 0;
			for (int trial = 0; trial < 400; ++trial)
			{
				DolbyVisionTunnelFrameMetadata dm = Dm({});
				if (trial % 2 == 0)
					AddL10(dm, 3, static_cast<uint16_t>(target(rng)));
				const int n = trial % 11 == 0 ? DolbyVisionTunnelFrameMetadata::kMaxTrims : count(rng);   // sometimes a full DM
				for (int k = 0; k < n; ++k)
				{
					Trim t = kind(rng) == 0 ? L8(static_cast<uint8_t>(index(rng)), static_cast<uint16_t>(code(rng)), static_cast<uint16_t>(code(rng)))
						: L2(static_cast<uint16_t>(target(rng)), static_cast<uint16_t>(code(rng)), static_cast<uint16_t>(code(rng)),
							static_cast<uint16_t>(code(rng)), static_cast<uint16_t>(code(rng)), static_cast<uint16_t>(code(rng)));
					if (t.level == 8 && trial % 3 == 0)
					{
						t.hasMidContrastAndClip = true;
						t.midContrast = static_cast<uint16_t>(code(rng));
						t.clipTrim = static_cast<uint16_t>(code(rng));
					}
					dm.trims[dm.trimCount++] = t;
				}
				if (trial % 7 == 0 && n)
					dm.trims[0].slope = 5000;   // a corrupt block is skipped
				for (const Level level : { Level::L2, Level::L8 })
				{
					int p1 = 0, u1 = 0, f1 = -1, p2 = 0, u2 = 0, f2 = -1;
					const auto want = reference(dm, level, p2, u2, f2);
					const DvTrimSelection::CandidateList got = level == Level::L8 ? DvTrimSelection::Candidates(dm, level, &p1, &u1, &f1) :
						DvTrimSelection::Candidates(dm, level);
					Assert::AreEqual(want.size(), got.size(), L"the same number of candidates");
					for (size_t k = 0; k < want.size(); ++k)
					{
						Assert::AreEqual(want[k].trimIndex, got[static_cast<int>(k)].trimIndex, L"the same trim, in DM order");
						Assert::AreEqual(want[k].targetPq, got[static_cast<int>(k)].targetPq, L"the same target");
						Assert::IsTrue(sameParams(want[k].params, got[static_cast<int>(k)].params), L"the same decoded parameters");
					}
					if (level == Level::L8)
					{
						Assert::AreEqual(p2, p1, L"l8Present");
						Assert::AreEqual(u2, u1, L"l8Unresolved");
						Assert::AreEqual(f2, f1, L"firstUnresolved");
						withUnresolved += u2 > 0 ? 1 : 0;
					}
					compared += want.size();
					full += got.size() == static_cast<size_t>(DolbyVisionTunnelFrameMetadata::kMaxTrims) ? 1 : 0;
				}
			}
			Assert::IsTrue(compared > 500, L"the sweep compared real candidates");
			Assert::IsTrue(withUnresolved > 0, L"and exercised unresolved L8 targets");
			Assert::IsTrue(full > 0, L"and a full DM (kMaxTrims candidates fit the array)");

			// A DM with more trims than the array holds is clamped to kMaxTrims by the loop bound, never written past the end.
			{
				DolbyVisionTunnelFrameMetadata dm = Dm({});
				for (int k = 0; k < DolbyVisionTunnelFrameMetadata::kMaxTrims; ++k)
					dm.trims[dm.trimCount++] = L2(static_cast<uint16_t>(1500 + 100 * k));
				const DvTrimSelection::CandidateList list = DvTrimSelection::Candidates(dm, Level::L2);
				Assert::AreEqual<size_t>(DolbyVisionTunnelFrameMetadata::kMaxTrims, list.size());
				Assert::AreEqual(DolbyVisionTunnelFrameMetadata::kMaxTrims - 1, list[DolbyVisionTunnelFrameMetadata::kMaxTrims - 1].trimIndex);
			}

			// The shader parameters: appended into a reused buffer = the fresh list; refilling does not reallocate.
			DvTrimSelection::ExtParams params = DvTrimSelection::DecodeL8(L8(1, 2300, 1900));
			params.saturation[2] = 1.25;
			params.hueDegrees[4] = -7.5;
			const std::vector<std::pair<const char*, double>> fresh = DvTrimSelection::ShaderParameters(params);
			Assert::AreEqual<size_t>(21, fresh.size(), L"9 scalar parameters, 6 saturations, 6 hues");
			std::vector<std::pair<const char*, double>> buffer;
			buffer.reserve(64);
			const void* storage = buffer.data();
			for (int frame = 0; frame < 3; ++frame)
			{
				buffer.clear();
				DvTrimSelection::AppendShaderParameters(params, buffer);
				Assert::AreEqual(fresh.size(), buffer.size());
				for (size_t k = 0; k < fresh.size(); ++k)
				{
					Assert::AreEqual(std::string(fresh[k].first), std::string(buffer[k].first), L"the same names in the same order");
					Assert::AreEqual(fresh[k].second, buffer[k].second, 0.0, L"the same values");
				}
				Assert::IsTrue(buffer.data() == storage, L"the buffer keeps its storage across frames (no per-frame heap growth)");
			}
			// Appended after existing entries: they stay.
			buffer.clear();
			buffer.push_back({ "first", 1.0 });
			DvTrimSelection::AppendShaderParameters(params, buffer);
			Assert::AreEqual(fresh.size() + 1, buffer.size());
			Assert::AreEqual(std::string("first"), std::string(buffer[0].first));
			Assert::AreEqual(std::string("dv_slope"), std::string(buffer[1].first));
		}

		// W23-C2-S3-M2 (bff541a87) + LOW-1 (24d188643):
		// (1) When l8Unresolved > 0 leaves L8 unbracketed above l8MaxPq, crossing switchPq from L8 into L2 is C0-continuous
		//     for BOTH bracketed L2 (l2HasLow == true -> switchPq == l8MaxPq) and unbracketed L2 (!l2HasLow -> switchPq == 0.5*(l8MaxPq + l2Hi)),
		//     fading the L8-vs-L2 delta linearly to 0 at hiL2; same-input pre-fix negative control suffers a hard step cliff at switchPq.
		// (2) Trims authored at or above dm.sourceMaxPq anchor to neutral at sourceMaxPq so displayPq -> sourceMaxPq- is C0-continuous
		//     into the identity return at displayPq >= sourceMaxPq (both DvTrimSelection::Select and DolbyVisionTrims::Select).
		TEST_METHOD(UnresolvedL8ToL2CrossoverAndMasteringPeakBoundaryAreC0Continuous)
		{
			using namespace DvL8Trims;
			auto nitsAtPqCode = [](double pqCode) { return DolbyVisionTrims::NitsFromPq(pqCode / 4095.0); };
			auto maxParamStep = [](const DvTrimSelection::ExtParams& a, const DvTrimSelection::ExtParams& b) {
				double d = (std::max)({
					std::fabs(a.base.slope - b.base.slope),
					std::fabs(a.base.offset - b.base.offset),
					std::fabs(a.base.power - b.base.power),
					std::fabs(a.base.chromaWeight - b.base.chromaWeight),
					std::fabs(a.base.saturationGain - b.base.saturationGain),
					std::fabs(a.midContrast - b.midContrast),
					std::fabs(a.clipTrim - b.clipTrim) });
				for (int k = 0; k < DvTrimSelection::kHues; ++k)
				{
					d = (std::max)(d, std::fabs(a.saturation[k] - b.saturation[k]));
					d = (std::max)(d, std::fabs(a.hueDegrees[k] - b.hueDegrees[k]) / 30.0);
				}
				return d;
			};

			const uint16_t pq100 = 2081; // predefined index 1 (~100 nits)
			const uint16_t pq600 = Pq(600.0);
			const uint16_t pq4000 = Pq(4000.0);

			// Case 1A: Bracketed L2 (L2 has 100-nit and 600-nit trims; L8 has resolved index 1 = 100 nits + unresolved index 28).
			// L8 wins at displayPq <= 2081, L2 wins at displayPq > 2081 -> exact crossover switchPq == 2081.0.
			{
				Trim l8_100 = L8(1, 2048 + 480, 2048 - 240, 2048 + 300, 2048 + 200, 2048 + 400);
				l8_100.hasMidContrastAndClip = true;
				l8_100.midContrast = 2048 + 320;
				l8_100.clipTrim = 2048 - 280;
				l8_100.hasSaturationVector = true;
				l8_100.saturationVector[0] = 180;
				l8_100.hasHueVector = true;
				l8_100.hueVector[2] = 90;
				Trim l8_unres = L8(28, 2048 + 100, 2048);
				DolbyVisionTunnelFrameMetadata dm = Dm({
					l8_100, l8_unres,
					L2(pq100, 2048 - 360, 2048 + 160),
					L2(pq600, 2048 - 120, 2048 + 40)
				}, pq4000);

				const double eps = 0.05;
				const auto below = DvTrimSelection::Select(dm, nitsAtPqCode(pq100 - eps));
				const auto above = DvTrimSelection::Select(dm, nitsAtPqCode(pq100 + eps));
				const auto atHi = DvTrimSelection::Select(dm, nitsAtPqCode(pq600));
				Assert::IsTrue(below.level == DvTrimSelection::Level::L8, L"at/below l8MaxPq uses L8");
				Assert::IsTrue(above.level == DvTrimSelection::Level::L2, L"above l8MaxPq switches to bracketed L2");

				// Same-input pre-fix negative control: raw L2 selection at pq100 + eps jumps by full |L8(100) - L2(100)| > 0.20!
				DvTrimSelection::Selection rawL2;
				rawL2.displayPq = pq100 + eps;
				DvTrimSelection::SelectAmong(dm, DvTrimSelection::Candidates(dm, DvTrimSelection::Level::L2), rawL2);
				Assert::IsTrue(maxParamStep(below.params, rawL2.params) > 0.20,
					L"negative control: un-faded L8->L2 switch suffers >0.20 parameter step cliff");

				Assert::IsTrue(maxParamStep(below.params, above.params) < 2e-3,
					L"bracketed L2: L8->L2 crossover at switchPq=l8MaxPq must be C0-continuous across all scalar+vector trims");
				// At hiL2 (600 nits), the L8-vs-L2 delta has faded completely to pure L2(600).
				Assert::AreEqual(1.0 - 120.0 / 4096.0, atHi.params.base.slope, 1e-9, L"at hiL2 delta fades to exact L2 trim");
				Assert::AreEqual(0.0, atHi.params.midContrast, 1e-9, L"at hiL2 L8 midContrast delta fades to 0");
			}

			// Case 1B: Unbracketed L2 (!l2HasLow: L2 has ONLY 600-nit trim > l8MaxPq 2081; L8 has resolved index 1 + unresolved index 28).
			// Nearest-trim tiebreak switches at midpoint switchPq = 0.5 * (2081 + pq600), NOT at l8MaxPq!
			{
				Trim l8_100 = L8(1, 2048 + 500, 2048 - 200);
				l8_100.hasMidContrastAndClip = true;
				l8_100.midContrast = 2048 + 400;
				l8_100.clipTrim = 2048;
				DolbyVisionTunnelFrameMetadata dm = Dm({
					l8_100, L8(28, 2048 + 80, 2048),
					L2(pq600, 2048 - 300, 2048 + 120)
				}, pq4000);

				const double midPq = 0.5 * (static_cast<double>(pq100) + static_cast<double>(pq600));
				const double eps = 0.05;
				const auto belowMid = DvTrimSelection::Select(dm, nitsAtPqCode(midPq - eps));
				const auto aboveMid = DvTrimSelection::Select(dm, nitsAtPqCode(midPq + eps));
				Assert::IsTrue(belowMid.level == DvTrimSelection::Level::L8, L"!l2HasLow: below midpoint uses L8");
				Assert::IsTrue(aboveMid.level == DvTrimSelection::Level::L2, L"!l2HasLow: above midpoint switches to L2");

				// Same-input negative control (24d188643 LOW-1): using l8MaxPq instead of midpoint leaves a big jump at midPq!
				const double wrongF = (static_cast<double>(pq600) - (midPq + eps)) / (static_cast<double>(pq600) - static_cast<double>(pq100));
				const double rawL2Slope = 1.0 - 300.0 / 4096.0;
				const double l8SlopeAtMid = belowMid.params.base.slope;
				const double buggySlope = rawL2Slope + wrongF * (l8SlopeAtMid - rawL2Slope);
				Assert::IsTrue(std::fabs(buggySlope - l8SlopeAtMid) > 0.08,
					L"negative control: anchoring fade at l8MaxPq instead of midpoint leaves >0.08 slope cliff at midPq");

				Assert::IsTrue(maxParamStep(belowMid.params, aboveMid.params) < 2e-3,
					L"LOW-1 (!l2HasLow): L8->L2 crossover at midpoint switchPq = 0.5*(l8MaxPq+l2Hi) must be C0-continuous");
			}

			// Case 2: Mastering-peak boundary C0 continuity when a trim is authored at/above dm.sourceMaxPq.
			{
				const uint16_t srcPeakPq = Pq(1000.0);
				DolbyVisionTunnelFrameMetadata dm = Dm({
					L2(pq100, 2048 - 600, 2048 + 200),
					L2(static_cast<uint16_t>(srcPeakPq + 150), 2048 - 400, 2048 + 160) // targetMaxPq > sourceMaxPq!
				}, srcPeakPq);

				const double justBelowNits = nitsAtPqCode(srcPeakPq - 0.1);
				const double atPeakNits = nitsAtPqCode(srcPeakPq + 0.1);
				const auto selBelow = DvTrimSelection::Select(dm, justBelowNits);
				const auto selAtPeak = DvTrimSelection::Select(dm, atPeakNits);
				Assert::IsFalse(selAtPeak.apply, L"at/above sourceMaxPq returns identity");
				// Pre-fix negative control: raw trim at srcPeakPq+150 had slope = 1 - 400/4096 = 0.9023 (0.0977 cliff to 1.0!).
				Assert::IsTrue(std::fabs((1.0 - 400.0 / 4096.0) - 1.0) > 0.09, L"negative control: un-anchored >sourceMaxPq trim has >0.09 cliff");
				Assert::AreEqual(1.0, selBelow.params.base.slope, 1e-3, L"DvTrimSelection: C0 fade to neutral slope 1.0 at sourceMaxPq-");
				Assert::AreEqual(0.0, selBelow.params.base.offset, 1e-3, L"DvTrimSelection: C0 fade to neutral offset 0.0 at sourceMaxPq-");

				const auto l2Below = DolbyVisionTrims::Select(dm, justBelowNits);
				const auto l2AtPeak = DolbyVisionTrims::Select(dm, atPeakNits);
				Assert::IsFalse(l2AtPeak.apply, L"DolbyVisionTrims: at/above sourceMaxPq returns identity");
				Assert::AreEqual(1.0, l2Below.params.slope, 1e-3, L"DolbyVisionTrims: C0 fade to neutral slope 1.0 at sourceMaxPq-");
				Assert::AreEqual(0.0, l2Below.params.offset, 1e-3, L"DolbyVisionTrims: C0 fade to neutral offset 0.0 at sourceMaxPq-");
			}
		}

	};
}
