#include "pch.h"
#include "CppUnitTest.h"
#include <vprenderer/DolbyVisionTrims.h>

#include <cmath>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// dv_trims (2026-10-17): the L2 trim maths of DolbyVisionTrims.h (a
// non-certified reimplementation; the shader DV trims.glsl mirrors ApplyPixel).
namespace Tests
{
	namespace
	{
		using Trim = DolbyVisionTunnelFrameMetadata::Trim;

		Trim L2(uint16_t targetPq, uint16_t slope = 2048, uint16_t offset = 2048, uint16_t power = 2048,
			uint16_t chroma = 2048, uint16_t saturation = 2048)
		{
			Trim t;
			t.level = 2;
			t.targetMaxPq = targetPq;
			t.slope = slope;
			t.offset = offset;
			t.power = power;
			t.chromaWeight = chroma;
			t.saturationGain = saturation;
			return t;
		}

		DolbyVisionTunnelFrameMetadata Dm(std::initializer_list<Trim> trims, uint16_t sourceMaxPq = 3696)
		{
			DolbyVisionTunnelFrameMetadata dm;
			dm.valid = true;
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = sourceMaxPq;
			for (const Trim& t : trims)
				dm.trims[dm.trimCount++] = t;
			return dm;
		}

		double NitsAtCode(double code) { return DolbyVisionTrims::NitsFromPq(code / 4095.0); }
	}

	TEST_CLASS(DolbyVisionTrimsTests)
	{
	public:
		TEST_METHOD(FieldsDecodeWith2048Neutral)
		{
			const auto neutral = DolbyVisionTrims::Decode(L2(2081));
			Assert::IsTrue(DolbyVisionTrims::Neutral(neutral));
			const auto p = DolbyVisionTrims::Decode(L2(2081, 2048 + 410, 2048 - 205, 2048 + 1024, 2048 + 512, 2048 - 1024));
			Assert::AreEqual(1.0 + 410 / 4096.0, p.slope, 1e-12);
			Assert::AreEqual(-205 / 4096.0, p.offset, 1e-12);
			Assert::AreEqual(1.25, p.power, 1e-12);
			Assert::AreEqual(0.125, p.chromaWeight, 1e-12);
			Assert::AreEqual(0.75, p.saturationGain, 1e-12);
		}

		TEST_METHOD(PqRoundTrips)
		{
			for (const double nits : { 0.005, 0.1, 1.0, 48.0, 100.0, 203.0, 1000.0, 4000.0, 10000.0 })
				Assert::AreEqual(nits, DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::PqFromNits(nits)), nits * 1e-9 + 1e-12);
			Assert::AreEqual(0.0, DolbyVisionTrims::NitsFromPq(0.0), 1e-12);
			Assert::AreEqual(1.0, DolbyVisionTrims::PqFromNits(10000.0), 1e-12);
		}

		TEST_METHOD(SelectionPicksInterpolatesAndFades)
		{
			// Trims for 100 and 600 nits (PQ codes), mastering peak about 4000 nits.
			const uint16_t t100 = static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(100.0) * 4095.0));
			const uint16_t t600 = static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(600.0) * 4095.0));
			const auto dm = Dm({ L2(t600, 2048 - 400), L2(t100, 2048 - 800), [] { Trim l8; l8.level = 8; l8.slope = 100; return l8; }() });
			// Exactly at a target: that trim.
			{
				const auto s = DolbyVisionTrims::Select(dm, NitsAtCode(t100));
				Assert::IsTrue(s.apply);
				Assert::AreEqual(1.0 - 800 / 4096.0, s.params.slope, 1e-9, L"the 100-nit trim as sent");
			}
			// Between: linear in PQ.
			{
				const double mid = (t100 + t600) / 2.0;
				const auto s = DolbyVisionTrims::Select(dm, NitsAtCode(mid));
				Assert::AreEqual(0.5, s.weight, 1e-6);
				Assert::AreEqual(1.0 - 600 / 4096.0, s.params.slope, 1e-6, L"halfway between the slopes");
				Assert::AreEqual(1, s.lowerIndex);
				Assert::AreEqual(0, s.upperIndex);
			}
			// Below every target: the lowest-target trim.
			Assert::AreEqual(1.0 - 800 / 4096.0, DolbyVisionTrims::Select(dm, 48.0).params.slope, 1e-9);
			// Above every target, below the mastering peak: toward neutral.
			{
				const double halfway = (t600 + 3696) / 2.0;
				const auto s = DolbyVisionTrims::Select(dm, NitsAtCode(halfway));
				Assert::AreEqual(0.5, s.weight, 1e-6);
				Assert::AreEqual(1.0 - 200 / 4096.0, s.params.slope, 1e-6);
			}
			// At or above the mastering peak: identity.
			{
				const auto s = DolbyVisionTrims::Select(dm, 10000.0);
				Assert::IsFalse(s.apply);
				Assert::IsTrue(std::string(s.reason).find("identity") != std::string::npos);
			}
			// No L2 (the L8 block is not an L2 trim), no DM, no peak: nothing.
			Assert::IsFalse(DolbyVisionTrims::Select(Dm({ [] { Trim l8; l8.level = 8; return l8; }() }), 100.0).apply);
			DolbyVisionTunnelFrameMetadata invalid;
			Assert::IsFalse(DolbyVisionTrims::Select(invalid, 100.0).apply);
			Assert::IsFalse(DolbyVisionTrims::Select(dm, 0.0).apply);
			// A neutral trim selects nothing to apply.
			Assert::IsFalse(DolbyVisionTrims::Select(Dm({ L2(t100) }), 100.0).apply);
		}

		// test/dv-synthetic 2026-10-24 (owner: the trim follows VP's tone-mapping target): the renderer
		// passes libplacebo's target peak (sdr_target_nits for an SDR output, re-read every frame, so a
		// changed setting is followed live) to Select. Trims authored at 100 / 600 / 1000 nits:
		//   48 nits  (below every target): the 100-nit trim as sent, no extrapolation;
		//   100 nits: the 100-nit trim exactly;
		//   200 nits: interpolated in PQ between 100 and 600 (weight = (PQ(200) - PQ(100)) / (PQ(600) - PQ(100))).
		TEST_METHOD(SelectionFollowsTheToneMappingTarget)
		{
			const uint16_t t100 = static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(100.0) * 4095.0));
			const uint16_t t600 = static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(600.0) * 4095.0));
			const uint16_t t1000 = static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(1000.0) * 4095.0));
			const auto dm = Dm({ L2(t1000, 2048 + 30), L2(t100, 2048 + 160), L2(t600, 2048 + 60) }, static_cast<uint16_t>(t1000 + 300));
			const double s100 = 1.0 + 160 / 4096.0, s600 = 1.0 + 60 / 4096.0;
			{
				const auto s = DolbyVisionTrims::Select(dm, 48.0);
				Assert::IsTrue(s.apply);
				Assert::AreEqual(1, s.upperIndex, L"48 nits: the lowest-target (100-nit) trim");
				Assert::AreEqual(-1, s.lowerIndex);
				Assert::AreEqual(s100, s.params.slope, 1e-9, L"48 nits: the 100-nit trim as sent (no extrapolation)");
			}
			{
				const auto s = DolbyVisionTrims::Select(dm, 100.0);
				Assert::AreEqual(s100, s.params.slope, 1e-3, L"100 nits: the 100-nit trim");
			}
			{
				const auto s = DolbyVisionTrims::Select(dm, 200.0);
				const double w = (DolbyVisionTrims::PqFromNits(200.0) * 4095.0 - t100) / static_cast<double>(t600 - t100);
				Assert::AreEqual(1, s.lowerIndex);
				Assert::AreEqual(2, s.upperIndex);
				Assert::AreEqual(w, s.weight, 1e-9, L"200 nits: weight in PQ");
				Assert::AreEqual(s100 + (s600 - s100) * w, s.params.slope, 1e-9, L"200 nits: interpolated");
				Logger::WriteMessage(("target 200 nits: weight toward the 600-nit trim " + std::to_string(w) + "\n").c_str());
			}
		}

		TEST_METHOD(CurveIsMonotonicAndKeepsAboveWhite)
		{
			DolbyVisionTrims::Params p;
			p.slope = 0.85; p.offset = 0.03; p.power = 1.2;
			double last = -1.0;
			for (int i = 0; i <= 1200; ++i)
			{
				const double t = i / 1000.0;
				const double v = DolbyVisionTrims::Curve(t, p, 0.004);
				Assert::IsTrue(v >= last, L"monotonic");
				last = v;
			}
			Assert::AreEqual(DolbyVisionTrims::Curve(1.0, p, 0.004) + 0.1, DolbyVisionTrims::Curve(1.1, p, 0.004), 1e-12,
				L"above the peak: slope 1 from f(1) (nothing clipped)");
			DolbyVisionTrims::Params neutral;
			for (const double t : { 0.0, 0.25, 0.8, 1.0, 1.3 })
				Assert::AreEqual(t, DolbyVisionTrims::Curve(t, neutral, 0.004), 1e-12);
		}

		TEST_METHOD(PixelTrimIsNeutralAtNeutralAndActsAsDescribed)
		{
			for (const bool pq : { true, false })
			{
				DolbyVisionTrims::Signal signal;
				signal.pq = pq;
				signal.peakNits = pq ? 1000.0 : 100.0;
				if (!pq) { signal.kr = 0.2126; signal.kg = 0.7152; signal.kb = 0.0722; }
				const double colors[][3] = { { 0.5, 0.4, 0.3 }, { 0.2, 0.6, 0.1 }, { 0.9, 0.9, 0.9 }, { 0.0, 0.0, 0.0 }, { -0.02, 0.01, 0.0 } };
				DolbyVisionTrims::Params neutral;
				for (const auto& c : colors)
				{
					double v[3] = { c[0], c[1], c[2] };
					DolbyVisionTrims::ApplyPixel(v, neutral, signal);
					for (int k = 0; k < 3; ++k)
						Assert::AreEqual(c[k], v[k], 1e-9, L"neutral = identity");
				}
				// Slope below 1 darkens; hue (channel ratios in light) is kept.
				DolbyVisionTrims::Params darker;
				darker.slope = 0.8;
				double v[3] = { 0.5, 0.4, 0.3 };
				DolbyVisionTrims::ApplyPixel(v, darker, signal);
				Assert::IsTrue(v[0] < 0.5 && v[1] < 0.4 && v[2] < 0.3);
				const double before = DolbyVisionTrims::ToNits(0.5, signal) / DolbyVisionTrims::ToNits(0.4, signal);
				const double after = DolbyVisionTrims::ToNits(v[0], signal) / DolbyVisionTrims::ToNits(v[1], signal);
				Assert::AreEqual(before, after, 1e-9, L"ratios kept");
				// Saturation gain moves channels away from the luminance; grey stays grey.
				DolbyVisionTrims::Params saturated;
				saturated.saturationGain = 1.3;
				double s[3] = { 0.5, 0.4, 0.3 };
				DolbyVisionTrims::ApplyPixel(s, saturated, signal);
				Assert::IsTrue(s[0] > 0.5 && s[2] < 0.3, L"more saturated");
				double grey[3] = { 0.6, 0.6, 0.6 };
				DolbyVisionTrims::ApplyPixel(grey, saturated, signal);
				Assert::AreEqual(grey[0], grey[2], 1e-9, L"grey stays grey");
				// Black and below-black untouched.
				double black[3] = { -0.01, -0.01, -0.01 };
				DolbyVisionTrims::ApplyPixel(black, darker, signal);
				Assert::AreEqual(-0.01, black[0], 1e-12);
			}
		}
	
		TEST_METHOD(CurveIsContinuousThroughBlackWithAPositiveOffset)
		{
			// Review C2: a positive offset fades in over the first PQ codes, so
			// f(0) = 0 (no step from the untouched below-black) and the curve
			// rises monotonically through the fade.
			DolbyVisionTrims::Params lift;
			lift.offset = 0.1;
			const double fadeT = DolbyVisionTrims::kOffsetFadePq / DolbyVisionTrims::PqFromNits(100.0);
			Assert::AreEqual(0.0, DolbyVisionTrims::Curve(0.0, lift, fadeT), 1e-12);
			double last = 0.0;
			for (int i = 1; i <= 400; ++i)
			{
				const double v = DolbyVisionTrims::Curve(fadeT * i / 200.0, lift, fadeT);
				Assert::IsTrue(v >= last, L"monotonic through the fade");
				Assert::IsTrue(v - last <= 0.1 / 200.0 + 0.01 / 200.0 + 1e-9, L"no step");
				last = v;
			}
			// Through the pixel path: a pixel just above black stays next to black.
			DolbyVisionTrims::Signal signal;
			signal.pq = false;
			signal.peakNits = 100.0;
			signal.kr = 0.2126; signal.kg = 0.7152; signal.kb = 0.0722;
			double tiny[3] = { 1e-4, 1e-4, 1e-4 };
			DolbyVisionTrims::ApplyPixel(tiny, lift, signal);
			Assert::IsTrue(tiny[0] < 0.01, L"0+ is not lifted to the offset");
		}

		TEST_METHOD(MixedSignNearBlackPixelsStayBounded)
		{
			// Review C1: a tiny positive luminance from mixed-sign channels (below-
			// black in one channel) must not be multiplied into a bright speck by
			// Y'/Y; luminance becomes Y' exactly and the chroma gain is bounded.
			DolbyVisionTrims::Signal signal;
			signal.pq = true;
			signal.peakNits = 600.0;
			DolbyVisionTrims::Params lift;
			lift.offset = 0.2;
			lift.saturationGain = 1.5;
			const double nits[3] = { 0.08, -0.02, -0.01 };   // Y about 0.0005 nits
			double rgb[3];
			for (int c = 0; c < 3; ++c)
				rgb[c] = DolbyVisionTrims::FromNits(nits[c], signal);
			const double Y = signal.kr * nits[0] + signal.kg * nits[1] + signal.kb * nits[2];
			Assert::IsTrue(Y > 0.0 && Y < 0.01);
			DolbyVisionTrims::ApplyPixel(rgb, lift, signal);
			double out[3];
			for (int c = 0; c < 3; ++c)
				out[c] = DolbyVisionTrims::ToNits(rgb[c], signal);
			// Luminance becomes the curve's Y' exactly; the chroma (L - Y) is
			// scaled by at most sat x kMaxChromaRatio (the old Y'/Y gain here was
			// about 175: a 20-nit speck from a 0.08-nit channel).
			const double Y2 = signal.kr * out[0] + signal.kg * out[1] + signal.kb * out[2];
			const double peakPq = DolbyVisionTrims::PqFromNits(signal.peakNits);
			const double expectY2 = DolbyVisionTrims::NitsFromPq(peakPq * DolbyVisionTrims::Curve(
				DolbyVisionTrims::PqFromNits(Y) / peakPq, lift, DolbyVisionTrims::kOffsetFadePq / peakPq));
			Assert::AreEqual(expectY2, Y2, expectY2 * 1e-6 + 1e-9, L"luminance = Y'");
			for (int c = 0; c < 3; ++c)
			{
				Assert::IsTrue(std::fabs(out[c] - Y2) <= 1.5 * DolbyVisionTrims::kMaxChromaRatio * std::fabs(nits[c] - Y) + 1e-6,
					(L"chroma gain bounded, channel " + std::to_wstring(c)).c_str());
				Assert::IsTrue(out[c] < Y2 + 1.0, (L"no speck, channel " + std::to_wstring(c)).c_str());
			}
			// A dark but positive colourful pixel: chroma gain at most sat x kMaxChromaRatio.
			const double dark[3] = { 0.5, 0.1, 0.05 };
			for (int c = 0; c < 3; ++c)
				rgb[c] = DolbyVisionTrims::FromNits(dark[c], signal);
			const double Yd = signal.kr * dark[0] + signal.kg * dark[1] + signal.kb * dark[2];
			DolbyVisionTrims::ApplyPixel(rgb, lift, signal);
			double Yo = 0.0, o[3];
			for (int c = 0; c < 3; ++c) o[c] = DolbyVisionTrims::ToNits(rgb[c], signal);
			Yo = signal.kr * o[0] + signal.kg * o[1] + signal.kb * o[2];
			Assert::IsTrue(std::fabs(o[0] - Yo) <= 1.5 * DolbyVisionTrims::kMaxChromaRatio * std::fabs(dark[0] - Yd) + 1e-6);
		}

		TEST_METHOD(CorruptTrimCodesAreRejected)
		{
			// Review C3: any L2 field beyond 12 bits skips the block; decoding clamps.
			const uint16_t t100 = static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(100.0) * 4095.0));
			Trim corrupt = L2(t100, 60000);
			Assert::IsFalse(DolbyVisionTrims::Valid(corrupt));
			Assert::IsFalse(DolbyVisionTrims::Select(Dm({ corrupt }), 100.0).apply, L"a corrupt block alone: nothing");
			for (int field = 0; field < 5; ++field)
			{
				Trim t = L2(t100, 2048 - 400);
				uint16_t* f[] = { &t.slope, &t.offset, &t.power, &t.chromaWeight, &t.saturationGain };
				*f[field] = 4096;
				Assert::IsFalse(DolbyVisionTrims::Valid(t));
			}
			const auto s = DolbyVisionTrims::Select(Dm({ corrupt, L2(t100, 2048 - 400) }), 100.0);
			Assert::IsTrue(s.apply);
			Assert::AreEqual(1.0 - 400 / 4096.0, s.params.slope, 1e-9, L"the valid block is used");
			const auto clamped = DolbyVisionTrims::Decode(corrupt);
			Assert::IsTrue(clamped.slope <= 1.5 && clamped.slope >= 0.5);
		}

		TEST_METHOD(LuminanceWeightsFollowThePrimaries)
		{
			// Review C4: Y row of RGB -> XYZ from the primaries.
			const double d65[2] = { 0.3127, 0.3290 };
			double kr, kg, kb;
			{
				const double r[2] = { 0.640, 0.330 }, g[2] = { 0.300, 0.600 }, b[2] = { 0.150, 0.060 };
				DolbyVisionTrims::LuminanceWeights(r, g, b, d65, kr, kg, kb);
				Assert::AreEqual(0.2126, kr, 2e-4); Assert::AreEqual(0.7152, kg, 2e-4); Assert::AreEqual(0.0722, kb, 2e-4);
			}
			{
				const double r[2] = { 0.708, 0.292 }, g[2] = { 0.170, 0.797 }, b[2] = { 0.131, 0.046 };
				DolbyVisionTrims::LuminanceWeights(r, g, b, d65, kr, kg, kb);
				Assert::AreEqual(0.2627, kr, 2e-4); Assert::AreEqual(0.6780, kg, 2e-4); Assert::AreEqual(0.0593, kb, 2e-4);
			}
			{
				const double r[2] = { 0.680, 0.320 }, g[2] = { 0.265, 0.690 }, b[2] = { 0.150, 0.060 };   // P3-D65
				DolbyVisionTrims::LuminanceWeights(r, g, b, d65, kr, kg, kb);
				Assert::AreEqual(0.2290, kr, 2e-4); Assert::AreEqual(0.6917, kg, 2e-4); Assert::AreEqual(0.0793, kb, 2e-4);
			}
			Assert::AreEqual(1.0, kr + kg + kb, 1e-9);
		}

		TEST_METHOD(ShoulderRollsOffInsteadOfClippingBelowThePeak)
		{
			// Review S3: slope + offset > 1 reaches 1 exactly at the peak, smoothly.
			DolbyVisionTrims::Params p;
			p.slope = 1.3;
			p.offset = 0.1;
			double last = -1.0, lastStep = 1e9;
			for (int i = 600; i <= 1000; ++i)
			{
				const double v = DolbyVisionTrims::Curve(i / 1000.0, p, 0.004);
				Assert::IsTrue(v > last || i == 1000, L"strictly rising below the peak (no clip)");
				if (last >= 0.0 && i > 700)
					Assert::IsTrue(v - last <= lastStep + 1e-9, L"the steps only shrink in the shoulder");
				if (last >= 0.0) lastStep = v - last;
				last = v;
			}
			Assert::AreEqual(1.0, DolbyVisionTrims::Curve(1.0, p, 0.004), 1e-12, L"1 at the peak");
			Assert::IsTrue(DolbyVisionTrims::Curve(0.99, p, 0.004) < 1.0, L"below 1 before the peak");
			Assert::AreEqual(1.05, DolbyVisionTrims::Curve(1.05, p, 0.004), 1e-12, L"above-white continues");
		}
	
		TEST_METHOD(SaturationNeverPushesAChannelBelowBlack)
		{
			// Gamut: a strong saturation gain on a saturated pixel caps where the
			// smallest non-negative channel would cross black.
			DolbyVisionTrims::Signal signal;
			signal.pq = true;
			signal.peakNits = 600.0;
			DolbyVisionTrims::Params p;
			p.slope = 1.3; p.offset = 0.1; p.saturationGain = 1.5;
			const double nits[3] = { 4.2, 244.0, 40.1 };
			double rgb[3];
			for (int c = 0; c < 3; ++c)
				rgb[c] = DolbyVisionTrims::FromNits(nits[c], signal);
			DolbyVisionTrims::ApplyPixel(rgb, p, signal);
			for (int c = 0; c < 3; ++c)
				Assert::IsTrue(rgb[c] >= -1e-9, (L"channel " + std::to_wstring(c) + L" stays at or above black").c_str());
			Assert::IsTrue(DolbyVisionTrims::ToNits(rgb[1], signal) > nits[1], L"still lifted");
		}
	};
}
