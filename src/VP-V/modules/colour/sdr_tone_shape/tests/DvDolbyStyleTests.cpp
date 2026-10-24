/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// feat/dv-l8 task 5 (2026-10-24): the Dolby-style DV tone mapping (DvDolbyStyle.h). The L1-driven
// intensity curve (monotonic, hits its anchors, follows scene changes, identity when the scene
// fits the target, ordered above the scene peak), ICtCp, the trims in ICtCp (neutral identity,
// intensity / chroma / hue behaviour, no new below-black, below-black carried), the libplacebo
// function and switch, the fallback reasons, and the reference viewing conditions record.
#include "pch.h"
#include "CppUnitTest.h"

#include <libplacebo/tone_mapping.h>
#include <libplacebo/shaders/colorspace.h>
#include <VP-V/modules/colour/sdr_tone_shape/DvDolbyStyle.h>

#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace DvDolby
	{
		inline double Pq(double nits) { return DolbyVisionTrims::PqFromNits(nits); }

		inline DvDolbyStyle::IctcpSignal Sdr(double peak = 48.0)
		{
			DolbyVisionTrims::Signal s;
			s.pq = false;
			s.gamma = 2.4;
			s.peakNits = peak;
			const double r[2] = { 0.708, 0.292 }, g[2] = { 0.170, 0.797 }, b[2] = { 0.131, 0.046 }, w[2] = { 0.3127, 0.3290 };
			return DvDolbyStyle::MakeSignal(s, r, g, b, w);
		}
	}

	TEST_CLASS(DvDolbyStyleTests)
	{
	public:
		TEST_METHOD(CurveIsMonotonicAndHitsItsAnchors)
		{
			using namespace DvDolby;
			const double tMin = Pq(0.005), tMax = Pq(48.0);
			for (double sMaxNits : { 600.0, 1000.0, 4000.0 })
				for (double sAvgNits : { 1.0, 10.0, 40.0, 150.0 })
				{
					const DvDolbyStyle::Curve c = DvDolbyStyle::Build(Pq(0.001), Pq(sAvgNits), Pq(sMaxNits), tMin, tMax);
					Assert::IsFalse(c.identity);
					Assert::AreEqual(tMin, c.Map(c.sMin), 1e-9, L"scene min -> target min");
					Assert::AreEqual(tMax, c.Map(c.sMax), 1e-9, L"scene max -> target max");
					Assert::AreEqual(c.tMid, c.Map(c.sMid), 1e-9, L"scene mid -> target mid");
					const double chord = tMin + (c.sMid - c.sMin) * (tMax - tMin) / (c.sMax - c.sMin);
					Assert::IsTrue(c.tMid >= chord - 1e-12 && c.tMid <= c.sMid + 1e-12, L"the mid between the chord and the source mid");
					Assert::IsTrue(c.tMid <= tMin + (tMax - tMin) * DvDolbyStyle::kMidCeiling + 1e-12);
					double last = -1.0;
					for (int k = 0; k <= 2000; ++k)
					{
						const double x = k / 2000.0;
						const double y = c.Map(x);
						Assert::IsTrue(y >= last - 1e-12, L"monotonic over the whole PQ range");
						last = y;
					}
					Assert::AreEqual(0.0, c.Map(0.0), 1e-12, L"black stays black");
					Assert::IsTrue(c.Map(c.sMax + 0.02) > tMax, L"above the scene peak: ordered (WTW kept)");
				}
		}

		TEST_METHOD(IdentityWhenTheSceneFitsTheTarget)
		{
			using namespace DvDolby;
			const DvDolbyStyle::Curve c = DvDolbyStyle::Build(Pq(0.01), Pq(20.0), Pq(90.0), Pq(0.005), Pq(100.0));
			Assert::IsTrue(c.identity);
			for (double x : { 0.0, 0.1, 0.4, 0.51, 0.7, 1.0 })
				Assert::AreEqual(x, c.Map(x));
			// Same source and target range: identity too.
			Assert::IsTrue(DvDolbyStyle::Build(Pq(0.005), Pq(10.0), Pq(1000.0), Pq(0.005), Pq(1000.0)).identity);
			// Degenerate inputs: identity, never a NaN.
			Assert::IsTrue(DvDolbyStyle::Build(0.5, 0.5, 0.5, 0.0, 0.5).identity);
		}

		TEST_METHOD(CurveFollowsSceneChangesInL1)
		{
			using namespace DvDolby;
			DolbyVisionTunnelFrameMetadata dm;
			dm.valid = true;
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = static_cast<uint16_t>(Pq(4000.0) * 4095.0);
			const DvDolbyStyle::Curve noL1 = DvDolbyStyle::ForDm(dm, 0.005, 48.0);
			Assert::AreEqual(Pq(4000.0), noL1.sMax, 1.0 / 4095.0, L"no L1: the source range");
			dm.hasSceneMetadata = true;
			dm.sceneMinPq = 0;
			dm.sceneMaxPq = static_cast<uint16_t>(Pq(1000.0) * 4095.0);
			dm.sceneAvgPq = static_cast<uint16_t>(Pq(5.0) * 4095.0);
			const DvDolbyStyle::Curve dark = DvDolbyStyle::ForDm(dm, 0.005, 48.0);
			dm.sceneAvgPq = static_cast<uint16_t>(Pq(60.0) * 4095.0);
			const DvDolbyStyle::Curve bright = DvDolbyStyle::ForDm(dm, 0.005, 48.0);
			Assert::AreEqual(Pq(1000.0), dark.sMax, 1.0 / 4095.0, L"L1 max is the scene peak");
			Assert::IsTrue(bright.tMid > dark.tMid, L"a brighter scene average maps to a brighter target mid");
			const double x = Pq(20.0);
			Assert::IsTrue(std::fabs(bright.Map(x) - dark.Map(x)) > 1e-4, L"the curve changes with the scene");
			// A scene whose peak already fits the target: untouched (no expansion).
			dm.sceneMaxPq = static_cast<uint16_t>(Pq(40.0) * 4095.0);
			dm.sceneAvgPq = static_cast<uint16_t>(Pq(4.0) * 4095.0);
			Assert::IsTrue(DvDolbyStyle::ForDm(dm, 0.0, 48.0).identity);
		}

		TEST_METHOD(IctcpRoundTripsAndGreyIntensityIsPqOfLuminance)
		{
			const DvDolbyStyle::IctcpSignal s = DvDolby::Sdr(100.0);
			std::mt19937 rng(4);
			std::uniform_real_distribution<double> u(0.01, 100.0);
			for (int k = 0; k < 500; ++k)
			{
				const double nits[3] = { u(rng), u(rng), u(rng) };
				double ictcp[3], back[3];
				DvDolbyStyle::ToIctcp(nits, s.toLms, ictcp);
				DvDolbyStyle::FromIctcp(ictcp, s.fromLms, back);
				for (int c = 0; c < 3; ++c)
					Assert::AreEqual(nits[c], back[c], nits[c] * 1e-7 + 1e-9);
			}
			const double grey[3] = { 30.0, 30.0, 30.0 };
			double ictcp[3];
			DvDolbyStyle::ToIctcp(grey, s.toLms, ictcp);
			Assert::AreEqual(DvDolby::Pq(30.0), ictcp[0], 2e-4, L"grey I = PQ(Y)");
			Assert::AreEqual(0.0, ictcp[1], 2e-4);
			Assert::AreEqual(0.0, ictcp[2], 2e-4);
		}

		TEST_METHOD(TrimsInIctcpActOnIntensityAndChroma)
		{
			const DvDolbyStyle::IctcpSignal s = DvDolby::Sdr();
			auto ictcpOf = [&](const double rgb[3], double out[3])
			{
				double n[3];
				for (int c = 0; c < 3; ++c) n[c] = DolbyVisionTrims::ToNits(rgb[c], s.signal);
				DvDolbyStyle::ToIctcp(n, s.toLms, out);
			};
			// Neutral: identity.
			std::mt19937 rng(8);
			std::uniform_real_distribution<double> u(0.0, 1.0);
			for (int k = 0; k < 300; ++k)
			{
				double px[3] = { u(rng), u(rng), u(rng) }, ref[3] = { px[0], px[1], px[2] };
				DvDolbyStyle::ApplyPixelIctcp(px, DvTrimSelection::ExtParams(), s);
				for (int c = 0; c < 3; ++c)
					Assert::AreEqual(ref[c], px[c], 1e-6, L"neutral trims: identity in ICtCp");
			}
			// Slope on intensity: a grey brightens by the curve, stays grey.
			DvTrimSelection::ExtParams slope;
			slope.base.slope = 1.2;
			double grey[3] = { 0.5, 0.5, 0.5 }, g0[3] = { 0.5, 0.5, 0.5 };
			double before[3], after[3];
			ictcpOf(g0, before);
			DvDolbyStyle::ApplyPixelIctcp(grey, slope, s);
			ictcpOf(grey, after);
			const double peakPq = DolbyVisionTrims::PqFromNits(s.signal.peakNits);
			const double want = DvTrimSelection::Curve(before[0] / peakPq, slope, DolbyVisionTrims::kOffsetFadePq / peakPq) * peakPq;
			Assert::AreEqual(want, after[0], 1e-6, L"I' = the trim curve of I");
			Assert::IsTrue(std::fabs(grey[0] - grey[1]) < 1e-6 && std::fabs(grey[1] - grey[2]) < 1e-6, L"grey stays grey");
			// Saturation gain scales (Ct, Cp), intensity kept.
			DvTrimSelection::ExtParams satp;
			satp.base.saturationGain = 1.3;
			double col[3] = { 0.6, 0.45, 0.4 }, c0[3] = { 0.6, 0.45, 0.4 };
			ictcpOf(c0, before);
			DvDolbyStyle::ApplyPixelIctcp(col, satp, s);
			ictcpOf(col, after);
			Assert::AreEqual(before[0], after[0], 1e-6, L"intensity kept");
			Assert::AreEqual(before[1] * 1.3, after[1], std::fabs(before[1]) * 1e-4 + 1e-7);
			Assert::AreEqual(before[2] * 1.3, after[2], std::fabs(before[2]) * 1e-4 + 1e-7);
			// Hue vector: rotation in the (Ct, Cp) plane, chroma magnitude and intensity kept.
			DvTrimSelection::ExtParams hue;
			for (int k = 0; k < 6; ++k) hue.hueDegrees[k] = 10.0;
			double h[3] = { 0.6, 0.45, 0.4 };
			DvDolbyStyle::ApplyPixelIctcp(h, hue, s);
			ictcpOf(h, after);
			const double a0 = std::atan2(before[2], before[1]), a1 = std::atan2(after[2], after[1]);
			Assert::AreEqual(10.0, (a1 - a0) * 180.0 / DvTrimSelection::kPi, 1e-3, L"rotated 10 degrees");
			Assert::AreEqual(std::hypot(before[1], before[2]), std::hypot(after[1], after[2]), 1e-6);
			// Strong saturation never pushes a channel below black; below-black parts are carried.
			DvTrimSelection::ExtParams strong;
			strong.base.saturationGain = 1.5;
			for (int k = 0; k < 6; ++k) { strong.saturation[k] = 1.5; strong.hueDegrees[k] = 25.0; }
			for (int k = 0; k < 2000; ++k)
			{
				double px[3] = { u(rng), u(rng) * u(rng), u(rng) * u(rng) * u(rng) };
				DvDolbyStyle::ApplyPixelIctcp(px, strong, s);
				for (int c = 0; c < 3; ++c)
					Assert::IsTrue(DolbyVisionTrims::ToNits(px[c], s.signal) >= -1e-9, L"no new below-black");
			}
			double btb[3] = { 0.5, 0.5, -0.05 };
			DvDolbyStyle::ApplyPixelIctcp(btb, DvTrimSelection::ExtParams(), s);
			Assert::AreEqual(-0.05, btb[2], 1e-6, L"a below-black channel is carried unchanged by neutral trims");
		}

		TEST_METHOD(LibplaceboFunctionSwitchAndFallback)
		{
			using namespace DvDolby;
			const pl_tone_map_function& f = DvDolbyStyle::Function();
			Assert::IsTrue(f.scaling == PL_HDR_PQ && f.map != nullptr && f.map_inverse == nullptr);
			pl_tone_map_params p = {};
			p.input_scaling = p.output_scaling = PL_HDR_PQ;
			p.input_min = static_cast<float>(Pq(0.001));
			p.input_max = static_cast<float>(Pq(1000.0));
			p.input_avg = static_cast<float>(Pq(10.0));
			p.output_min = static_cast<float>(Pq(0.005));
			p.output_max = static_cast<float>(Pq(48.0));
			std::vector<float> lut(256);
			for (size_t i = 0; i < lut.size(); ++i)
				lut[i] = p.input_min + (p.input_max - p.input_min) * static_cast<float>(i) / 255.0f;
			p.lut_size = lut.size();
			f.map(lut.data(), &p);
			Assert::AreEqual(p.output_min, lut.front(), 1e-5f);
			Assert::AreEqual(p.output_max, lut.back(), 1e-5f);
			for (size_t i = 1; i < lut.size(); ++i)
				Assert::IsTrue(lut[i] >= lut[i - 1]);
			// The switch: DV pictures get the function, others the configured one back.
			pl_tone_map_function configured = {};
			pl_color_map_params cmp = {};
			cmp.tone_mapping_function = &configured;
			DvDolbyStyle::ToneSwitch sw;
			sw.Apply(cmp, &f);
			Assert::IsTrue(cmp.tone_mapping_function == &f);
			sw.Apply(cmp, nullptr);
			Assert::IsTrue(cmp.tone_mapping_function == &configured);
			sw.Apply(cmp, &f);
			pl_tone_map_function reapplied = {};
			cmp.tone_mapping_function = &reapplied;   // a settings apply in between
			sw.Apply(cmp, nullptr);
			Assert::IsTrue(cmp.tone_mapping_function == &reapplied, L"a re-applied setting is kept");
			// Fallback reasons (the renderer lists "DV tone mapping" and uses bt2390 + trims).
			Assert::AreEqual(std::string(), DvDolbyStyle::CannotRun(false, true, PL_COLOR_TRC_LINEAR, false), L"not asked: no fallback");
			Assert::AreEqual(std::string(), DvDolbyStyle::CannotRun(true, false, PL_COLOR_TRC_GAMMA24, true));
			Assert::AreEqual(std::string("a calibration LUT is active"), DvDolbyStyle::CannotRun(true, true, PL_COLOR_TRC_PQ, true));
			Assert::AreEqual(std::string("unsupported output transfer"), DvDolbyStyle::CannotRun(true, false, PL_COLOR_TRC_HLG, true));
			Assert::AreEqual(std::string("DV tone mapping: Dolby-style (approx.)"),
				DvDolbyStyle::StatusText(DvDolbyStyle::Mode::DolbyStyle, true, "bt2390", ""));
			Assert::IsTrue(DvDolbyStyle::StatusText(DvDolbyStyle::Mode::DolbyStyle, false, "spline", "a calibration LUT is active")
				.find("bt2390 + trims") != std::string::npos);
			Assert::AreEqual(std::string("DV tone mapping: libplacebo (spline)"),
				DvDolbyStyle::StatusText(DvDolbyStyle::Mode::Libplacebo, false, "spline", ""));
			const auto off = DvDolbyStyle::ShaderParameters(false, nullptr);
			Assert::AreEqual<size_t>(1, off.size());
			Assert::AreEqual(0.0, off[0].second);
			Assert::AreEqual<size_t>(10, DvDolbyStyle::ShaderParameters(true, nullptr).size());
		}

		TEST_METHOD(ReferenceConditionsDescribeWhatTheGradeAssumed)
		{
			using namespace DvDolby;
			DolbyVisionTunnelFrameMetadata dm;
			dm.valid = true;
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = static_cast<uint16_t>(Pq(4000.0) * 4095.0 + 0.5);
			auto l8 = [](uint8_t index, uint16_t slope) {
				DolbyVisionTunnelFrameMetadata::Trim t;
				t.level = 8; t.targetDisplayIndex = index; t.slope = slope; t.offset = t.power = t.chromaWeight = t.saturationGain = 2048;
				return t;
			};
			dm.trims[dm.trimCount++] = l8(1, 2300);
			dm.trims[dm.trimCount++] = l8(27, 2100);
			auto& td = dm.targetDisplays[dm.targetDisplayCount++];
			td.index = 27; td.maxPq = static_cast<uint16_t>(Pq(600.0) * 4095.0 + 0.5); td.minPq = static_cast<uint16_t>(Pq(0.005) * 4095.0 + 0.5);
			dm.hasL6 = true; dm.l6MaxDisplayMasteringLuminance = 4000; dm.l6MinDisplayMasteringLuminance = 50;
			dm.hasL11 = true; dm.l11ContentType = 1; dm.l11ReferenceMode = true;
			dm.hasL9 = true; dm.l9SourcePrimaryIndex = 0;
			const DvTrimSelection::Selection sel = DvTrimSelection::Select(dm, 300.0);
			DvDolbyStyle::ReferenceConditions r = DvDolbyStyle::Conditions(dm, sel, true, "bt2390");
			Assert::IsTrue(r.valid && r.trimLevel == DvTrimSelection::Level::L8 && r.trimApplied);
			Assert::AreEqual(300.0, r.trimTargetMaxNits, 3.0, L"the interpolated trim target follows the tone-mapping target");
			Assert::IsTrue(r.trimTargetMinNits > 0.0 && r.trimTargetMinNits < 0.005);
			Assert::IsTrue(r.masteringFromL6 && r.masteringPeakNits == 4000.0 && std::fabs(r.masteringMinNits - 0.005) < 1e-12);
			Assert::IsTrue(r.contentType == 1 && r.referenceMode && r.sourcePrimaryIndex == 0);
			Assert::IsTrue(r.dolbyStyleCurve && r.curve == "dolby_style");
			// Below every target: the 100-nit trim's own target; libplacebo's curve named.
			r = DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 48.0), false, "bt2390");
			Assert::AreEqual(100.0, r.trimTargetMaxNits, 0.5);
			Assert::IsTrue(!r.dolbyStyleCurve && r.curve == "bt2390");
			// No L6: the DM's source range (L0).
			dm.hasL6 = false;
			r = DvDolbyStyle::Conditions(dm, sel, false, "bt2390");
			Assert::IsFalse(r.masteringFromL6);
			Assert::AreEqual(4000.0, r.masteringPeakNits, 4000.0 * 0.01);
		}
	};
}
