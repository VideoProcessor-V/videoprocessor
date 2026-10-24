/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// feat/dv-l8 (2026-10-24): DV trims with L8 (DvTrimSelection.h). The level order (L8, else L2,
// else none), L8 targets from L10 / the predefined list, selection and interpolation by the
// tone-mapping target exactly as L2, the L2 path unchanged bit for bit, and the L8 extras
// (mid contrast, clip trim, per-hue saturation / hue) bounded, monotonic and gamut-safe.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h>

#include <cmath>
#include <cstring>
#include <random>
#include <string>

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
	};
}
