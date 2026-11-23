// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/colour/appearance/Cam16.h>
#include <VP-V/modules/colour/appearance/Cam16Uniforms.h>

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 CAM-1: the CPU reference CAM16 and the uniform pack. Pure CPU, no pipeline, no switch.
namespace Tests
{
	namespace
	{
		using Cam16::Vec3;

		// Linear Rec.709 -> XYZ (D65), Y = 100 for white.
		Vec3 Rec709ToXyz(double r, double g, double b)
		{
			return { { 100.0 * (0.412391 * r + 0.357584 * g + 0.180481 * b),
				100.0 * (0.212639 * r + 0.715169 * g + 0.072192 * b),
				100.0 * (0.019331 * r + 0.119195 * g + 0.950532 * b) } };
		}

		Cam16::Conditions Published()
		{
			Cam16::Conditions c;
			c.whiteXyz = { { 95.05, 100.0, 108.88 } };
			c.adaptingNits = 318.31;
			c.backgroundY = 20.0;
			c.surround = Cam16::kSurroundAverage;
			return c;
		}

		double CodeToY(int code) { return std::pow(code / 4095.0, 2.4); }
	}

	TEST_CLASS(Cam16ModelTests)
	{
	public:
		// Li et al. 2017 worked example (also the reference value of common CAM16 libraries).
		TEST_METHOD(PublishedWorkedExample)
		{
			const Cam16::Prepared p = Cam16::Prepare(Published());
			const Cam16::Appearance a = Cam16::Forward(p, { { 19.01, 20.0, 21.78 } });
			Assert::AreEqual(41.7311, a.J, 0.01);
			Assert::AreEqual(0.10336, a.C, 0.0002);
			Assert::AreEqual(0.107437, a.M, 0.0002);
			Assert::AreEqual(217.068, a.h, 0.02);
		}

		TEST_METHOD(IdentityAtEqualConditions)
		{
			const Cam16::Prepared p = Cam16::Prepare(Published());
			const double rgb[][3] = { { 0.2, 0.5, 0.8 }, { 0.9, 0.1, 0.1 }, { 0.05, 0.6, 0.2 }, { 0.5, 0.5, 0.5 }, { 0.8, 0.7, 0.1 } };
			for (const auto& c : rgb)
			{
				const Vec3 xyz = Rec709ToXyz(c[0], c[1], c[2]);
				const Cam16::Appearance a = Cam16::Forward(p, xyz);
				const Vec3 back = Cam16::Inverse(p, a.J, a.M, a.h);
				for (int i = 0; i < 3; ++i)
					Assert::AreEqual(xyz[i], back[i], 1e-6);
			}
		}

		TEST_METHOD(GreyRampJIsMonotonicAndBlackIsZero)
		{
			Cam16::Conditions c;
			c.adaptingNits = 20.0;
			c.surround = Cam16::kSurroundDim;
			const Cam16::Prepared p = Cam16::Prepare(c);
			double last = -1.0;
			for (int code = 0; code <= 4095; ++code)
			{
				const double y = CodeToY(code);
				const Cam16::Appearance a = Cam16::Forward(p, { { c.whiteXyz[0] * y, c.whiteXyz[1] * y, c.whiteXyz[2] * y } });
				Assert::IsTrue(a.J >= last, L"J must not fall along the grey ramp");
				last = a.J;
			}
			Assert::AreEqual(0.0, Cam16::Forward(p, { { 0.0, 0.0, 0.0 } }).J, 1e-9);
			Assert::AreEqual(100.0, last, 0.01);
		}

		// Forward under the reference conditions, inverse under the screen conditions (the appearance pass):
		// the colour read back under the screen conditions keeps its hue to within a degree.
		TEST_METHOD(HuePreservedAcrossConditions)
		{
			const Cam16::Prepared ref = Cam16::Prepare(Cam16::MakeConditions(100.0, Cam16::kSurroundDim, 0.2, 20.0));
			const Cam16::Prepared scr = Cam16::Prepare(Cam16::MakeConditions(48.0, Cam16::kSurroundDark, 0.2, 20.0));
			const double rgb[][3] = { { 0.8, 0.1, 0.1 }, { 0.1, 0.7, 0.2 }, { 0.1, 0.2, 0.9 }, { 0.8, 0.7, 0.1 }, { 0.6, 0.3, 0.5 }, { 0.2, 0.5, 0.5 } };
			for (const auto& c : rgb)
			{
				const Cam16::Appearance a = Cam16::Forward(ref, Rec709ToXyz(c[0], c[1], c[2]));
				const Vec3 xyz = Cam16::Inverse(scr, a.J, a.M, a.h);
				const Cam16::Appearance b = Cam16::Forward(scr, xyz);
				double dh = std::fabs(a.h - b.h);
				if (dh > 180.0)
					dh = 360.0 - dh;
				Assert::IsTrue(dh < 1.0, L"hue moved more than 1 degree");
				Assert::AreEqual(a.J, b.J, 1e-6);
			}
		}

		// Forward then inverse, same conditions, over all 4096 12-bit codes of the grey and the three primaries:
		// the error stays far below a quarter of a 12-bit code (0.25 / 4095 of the white).
		TEST_METHOD(RoundTripUnderQuarterCodeOver4096Codes)
		{
			const Cam16::Prepared p = Cam16::Prepare(Cam16::MakeConditions(100.0, Cam16::kSurroundDim, 0.2, 20.0));
			const double budget = 0.25 / 4095.0 * 100.0; // Y units (white = 100)
			double worst = 0.0;
			for (int ch = 0; ch < 4; ++ch)
				for (int code = 0; code <= 4095; ++code)
				{
					const double y = CodeToY(code);
					const Vec3 xyz = Rec709ToXyz(ch == 0 || ch == 3 ? y : 0.0, ch == 1 || ch == 3 ? y : 0.0, ch == 2 || ch == 3 ? y : 0.0);
					const Cam16::Appearance a = Cam16::Forward(p, xyz);
					const Vec3 back = Cam16::Inverse(p, a.J, a.M, a.h);
					for (int i = 0; i < 3; ++i)
						worst = std::max(worst, std::fabs(back[i] - xyz[i]));
				}
			Assert::IsTrue(worst < budget, L"round trip error above a quarter 12-bit code");
		}

		// Doc 115 B13: trim saturation gain x Hunt gain never exceeds the larger of the two alone.
		TEST_METHOD(TrimTimesHuntIsCapped)
		{
			const double gains[] = { 0.5, 0.9, 1.0, 1.1, 1.3, 1.6, 2.0 };
			for (double t : gains)
				for (double h : gains)
				{
					const double capped = Cam16::CappedHuntGain(t, h);
					Assert::IsTrue(capped <= h + 1e-12, L"the cap never raises the Hunt gain");
					Assert::IsTrue(t * capped <= std::max(t, h) + 1e-9, L"trim x Hunt above the larger alone");
				}
			Assert::AreEqual(1.0, Cam16::CappedHuntGain(1.3, 1.2), 1e-12); // trim already carries more: Hunt adds nothing
			Assert::AreEqual(1.5, Cam16::CappedHuntGain(1.0, 1.5), 1e-12); // no trim: Hunt untouched
		}
	};

	TEST_CLASS(Cam16UniformsTests)
	{
	public:
		TEST_METHOD(EqualConditionsPackEqualSides)
		{
			Cam16::UniformInputs in;
			in.referenceWhiteNits = 100.0;
			in.screenWhiteNits = 100.0;
			in.referenceSurround = Cam16::kSurroundDim;
			in.screenSurround = Cam16::kSurroundDim;
			const Cam16::Cam16Uniforms u = Cam16::PackUniforms(in);
			Assert::AreEqual(u.reference.fl, u.screen.fl);
			Assert::AreEqual(u.reference.aw, u.screen.aw);
			Assert::AreEqual(u.reference.cz, u.screen.cz);
			for (int i = 0; i < 3; ++i)
				Assert::AreEqual(u.reference.dc[i], u.screen.dc[i]);
		}

		TEST_METHOD(StrengthClampedAndVeilIsAFractionOfScreenWhite)
		{
			Cam16::UniformInputs in;
			in.screenWhiteNits = 50.0;
			in.veilNits = 0.5;
			in.strength = 3.0;
			Cam16::Cam16Uniforms u = Cam16::PackUniforms(in);
			Assert::AreEqual(1.0f, u.strength);
			Assert::AreEqual(0.01f, u.veil, 1e-7f);
			in.strength = -1.0;
			in.veilNits = -2.0;
			u = Cam16::PackUniforms(in);
			Assert::AreEqual(0.0f, u.strength);
			Assert::AreEqual(0.0f, u.veil);
		}

		TEST_METHOD(DarkerScreenLowersFlAndTheSidesDiffer)
		{
			Cam16::UniformInputs in;
			in.referenceWhiteNits = 100.0;
			in.screenWhiteNits = 48.0;
			const Cam16::Cam16Uniforms u = Cam16::PackUniforms(in);
			Assert::IsTrue(u.screen.fl < u.reference.fl, L"a dimmer screen has a lower luminance-level factor");
			Assert::IsTrue(u.screen.cz != u.reference.cz || u.screen.dc[0] != u.reference.dc[0] || u.screen.fl != u.reference.fl);
		}
	};
}
