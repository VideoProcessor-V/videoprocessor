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

		TEST_METHOD(CurveIsMonotonicAndKeepsAboveWhite)
		{
			DolbyVisionTrims::Params p;
			p.slope = 0.85; p.offset = 0.03; p.power = 1.2;
			double last = -1.0;
			for (int i = 0; i <= 1200; ++i)
			{
				const double t = i / 1000.0;
				const double v = DolbyVisionTrims::Curve(t, p);
				Assert::IsTrue(v >= last, L"monotonic");
				last = v;
			}
			Assert::AreEqual(DolbyVisionTrims::Curve(1.0, p) + 0.1, DolbyVisionTrims::Curve(1.1, p), 1e-12,
				L"above the peak: slope 1 from f(1) (nothing clipped)");
			DolbyVisionTrims::Params neutral;
			for (const double t : { 0.0, 0.25, 0.8, 1.0, 1.3 })
				Assert::AreEqual(t, DolbyVisionTrims::Curve(t, neutral), 1e-12);
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
	};
}
