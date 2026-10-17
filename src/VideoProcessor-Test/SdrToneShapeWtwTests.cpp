#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/SdrToneShape.h>

#include <cmath>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Audit 2026-10-16 (owner rule: below-black and above-white preserved in all
// cases): the SDR tone shape's knee keeps the source's above-white above the
// shaped white. Pure CPU checks of SdrToneShape::Knee/Map: in-range values
// identical to the pre-audit curve, strictly monotonic over [-0.1, 1.5], C1
// at the knee start and at the white, y(1 + e) > y(1), below-black untouched.
// The shader's agreement with Map is the GPU test
// SdrToneShapeShaderMatchesItsReference (LibplaceboLutParserTests.cpp).
namespace VideoProcessorTest
{
	namespace
	{
		namespace SdrWtw
		{
			// The pre-audit knee, verbatim, as the in-range reference.
			double LegacyKnee(double y, double start, double strength)
			{
				if (strength <= 0.0 || y <= start)
					return y;
				const double k = 1.0 - start;
				const double e = std::exp(-(y - start) / k);
				const double rolled = start + k * (1.0 - e);
				return y + (rolled - y) * strength;
			}

			double LegacyMap(double v, double gamma, double headroom, double start, double strength)
			{
				const double sign = v < 0.0 ? -1.0 : 1.0;
				const double linear = std::pow(std::fabs(v), gamma) * (1.0 - headroom);
				const double shaped = v < 0.0 ? linear : LegacyKnee(linear, start, strength);
				return sign * std::pow(shaped, 1.0 / gamma);
			}

			const double kStrengths[] = { 0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 };
			const double kStarts[] = { 0.5, 0.65, 0.8, 0.9, 0.99 };
			const double kHeadrooms[] = { 0.0, 0.05, 0.15, 0.3, 0.5 };
			const double kGammas[] = { 2.2, 2.4 };

			std::wstring Label(double gamma, double headroom, double start, double strength, double v)
			{
				return L"gamma " + std::to_wstring(gamma) + L" headroom " + std::to_wstring(headroom) +
					L" start " + std::to_wstring(start) + L" strength " + std::to_wstring(strength) +
					L" v " + std::to_wstring(v);
			}
		}
	}

	TEST_CLASS(SdrToneShapeWtwTests)
	{
	public:
		TEST_METHOD(InRangeIsIdenticalToThePreAuditCurve)
		{
			for (double g : SdrWtw::kGammas)
				for (double h : SdrWtw::kHeadrooms)
					for (double s : SdrWtw::kStarts)
						for (double st : SdrWtw::kStrengths)
							for (int i = 0; i <= 1000; ++i)
							{
								const double v = i / 1000.0;
								Assert::AreEqual(SdrWtw::LegacyMap(v, g, h, s, st), SdrToneShape::Map(v, g, h, s, st), 0.0,
									SdrWtw::Label(g, h, s, st, v).c_str());
							}
		}

		TEST_METHOD(StrictlyMonotonicFromBelowBlackToWellAboveWhite)
		{
			for (double g : SdrWtw::kGammas)
				for (double h : SdrWtw::kHeadrooms)
					for (double s : SdrWtw::kStarts)
						for (double st : SdrWtw::kStrengths)
						{
							double last = SdrToneShape::Map(-0.1, g, h, s, st);
							for (int i = 1; i <= 1600; ++i)
							{
								const double v = -0.1 + i * (1.6 / 1600.0);
								const double out = SdrToneShape::Map(v, g, h, s, st);
								Assert::IsTrue(out > last, SdrWtw::Label(g, h, s, st, v).c_str());
								last = out;
							}
						}
		}

		TEST_METHOD(AboveWhiteStaysAboveTheShapedWhite)
		{
			for (double g : SdrWtw::kGammas)
				for (double h : SdrWtw::kHeadrooms)
					for (double s : SdrWtw::kStarts)
						for (double st : SdrWtw::kStrengths)
						{
							const double white = SdrToneShape::Map(1.0, g, h, s, st);
							for (double e : { 1e-4, 0.01, 0.0913, 0.25, 0.5 })
							{
								const double over = SdrToneShape::Map(1.0 + e, g, h, s, st);
								Assert::IsTrue(over > white, SdrWtw::Label(g, h, s, st, 1.0 + e).c_str());
								// Never above the unshaped (headroom-only) value: nothing created.
								Assert::IsTrue(over <= SdrToneShape::Map(1.0 + e, g, h, s, 0.0) + 1e-12,
									SdrWtw::Label(g, h, s, st, 1.0 + e).c_str());
							}
							// The WTW is not squeezed: in linear light the step above the
							// placed white keeps at least exp(-1) of its size.
							const double linWhite = std::pow(white, g);
							const double linOver = std::pow(SdrToneShape::Map(1.0913, g, h, s, st), g);
							const double source = (std::pow(1.0913, g) - 1.0) * (1.0 - h);
							Assert::IsTrue(linOver - linWhite >= std::exp(-1.0) * source - 1e-12,
								SdrWtw::Label(g, h, s, st, 1.0913).c_str());
						}
			// The pre-audit curve at strength 1 is what this fixes: code 255 sat
			// far closer to the shaped white than the tangent continuation puts it.
			const double g = 2.2;
			const double v255 = (255.0 - 16.0) / 219.0;
			const double oldGap = std::pow(SdrWtw::LegacyMap(v255, g, 0.0, 0.8, 1.0), g) - std::pow(SdrWtw::LegacyMap(1.0, g, 0.0, 0.8, 1.0), g);
			const double newGap = std::pow(SdrToneShape::Map(v255, g, 0.0, 0.8, 1.0), g) - std::pow(SdrToneShape::Map(1.0, g, 0.0, 0.8, 1.0), g);
			Assert::IsTrue(newGap > oldGap * 1.3, L"strength 1: the WTW keeps more of its range than before");
		}

		TEST_METHOD(C1AtTheKneeStartAndAtTheWhite)
		{
			const double d = 1e-7;
			for (double h : SdrWtw::kHeadrooms)
				for (double s : SdrWtw::kStarts)
					for (double st : SdrWtw::kStrengths)
					{
						const double white = 1.0 - h;
						// Points: the knee start (when it lies inside the white) and the white.
						for (int which = 0; which < 2; ++which)
						{
							const double x = which == 0 ? s : white;
							if (which == 0 && s + d >= white)
								continue;
							const double y0 = SdrToneShape::Knee(x, s, st, nullptr, white);
							const double yl = SdrToneShape::Knee(x - d, s, st, nullptr, white);
							const double yr = SdrToneShape::Knee(x + d, s, st, nullptr, white);
							const double left = (y0 - yl) / d;
							const double right = (yr - y0) / d;
							const std::wstring label = SdrWtw::Label(0.0, h, s, st, x) + (which == 0 ? L" knee start" : L" white");
							Assert::IsTrue(std::fabs(yr - yl) < 4.0 * d, label.c_str());  // C0
							Assert::AreEqual(left, right, 1e-5, label.c_str());          // C1
							double slope = 0.0;
							SdrToneShape::Knee(x + d, s, st, &slope, white);
							Assert::AreEqual(right, slope, 1e-5, label.c_str());         // analytic slope agrees
							Assert::IsTrue(slope >= std::exp(-1.0) - 1e-12 && slope <= 1.0 + 1e-12, label.c_str());
						}
						// Slope 1 at the knee start.
						if (s < white)
						{
							double slope = 0.0;
							SdrToneShape::Knee(s, s, st, &slope, white);
							Assert::AreEqual(1.0, slope, 1e-12);
						}
					}
		}

		TEST_METHOD(BelowBlackPassesThroughTheKneeUnchanged)
		{
			for (double g : SdrWtw::kGammas)
				for (double s : SdrWtw::kStarts)
					for (double st : SdrWtw::kStrengths)
					{
						for (double v : { -0.1, -0.05, -0.01, -1e-4 })
						{
							// Headroom 0: identity.
							Assert::AreEqual(v, SdrToneShape::Map(v, g, 0.0, s, st), 1e-12, SdrWtw::Label(g, 0.0, s, st, v).c_str());
							// Headroom: only the linear placement (the knee never touches it).
							for (double h : SdrWtw::kHeadrooms)
								Assert::AreEqual(SdrToneShape::Map(v, g, h, s, 0.0), SdrToneShape::Map(v, g, h, s, st), 0.0,
									SdrWtw::Label(g, h, s, st, v).c_str());
						}
					}
		}

		TEST_METHOD(KneeStartAtOrAboveThePlacedWhiteLeavesAboveWhiteAlone)
		{
			// Headroom 0.3 places white at 0.7: a knee start of 0.8 lies in the
			// above-white, which is now preserved (identity) instead of rolled.
			for (double st : SdrWtw::kStrengths)
				for (double v : { 1.0, 1.05, 1.0913, 1.3 })
					Assert::AreEqual(SdrToneShape::Map(v, 2.2, 0.3, 0.8, 0.0), SdrToneShape::Map(v, 2.2, 0.3, 0.8, st), 1e-12);
		}
	};
}
