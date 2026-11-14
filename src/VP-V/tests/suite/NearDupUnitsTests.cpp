// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/LumaWeights.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocks.h>
#include <VP-V/nets/RgbLumaDelta.h>
#include <VP-V/core/OnePole.h>
#include <VP-V/core/Ramp.h>
#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Rule 20 near-duplicate consolidation (ND02, ND03, ND05, ND06, ND07): every unit is compared bit for bit with the
// private copy it replaced (the copies are reproduced below as the reference).
namespace NearDupUnitsTest
{
	bool SameBits(double a, double b)
	{
		return std::memcmp(&a, &b, sizeof(double)) == 0;
	}

	bool SameBits(float a, float b)
	{
		return std::memcmp(&a, &b, sizeof(float)) == 0;
	}

	// ---- legacy copies (reference) ----
	double OldMedianMean(std::vector<double> v)
	{
		if (v.empty()) return 0.0;
		std::sort(v.begin(), v.end());
		const size_t n = v.size();
		return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
	}

	double OldMedianUpper(std::vector<double> v)
	{
		if (v.empty()) return 1.0;
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		return v[v.size() / 2];
	}

	double OldSmoothLightLevel(double e0, double e1, double x)
	{
		if (e0 == e1) return x >= e0 ? 1.0 : 0.0;
		const double t = (std::min)((std::max)((x - e0) / (e1 - e0), 0.0), 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	double OldSmoothDither(double e0, double e1, double x)
	{
		if (!(e1 > e0)) return x >= e0 ? 1.0 : 0.0;
		double t = (x - e0) / (e1 - e0);
		t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
		return t * t * (3.0 - 2.0 * t);
	}

	int OldReflect(int i, int n)
	{
		if (i < 0) i = -i;
		if (i >= n) i = 2 * (n - 1) - i;
		return i < 0 ? 0 : (i >= n ? n - 1 : i);
	}

	void OldBlurReflect(const std::vector<float>& src, std::vector<float>& dst, int w, int h, const std::vector<float>& taps)
	{
		const int r = static_cast<int>(taps.size() / 2);
		std::vector<float> tmp(src.size());
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				float a = 0.f;
				for (int k = -r; k <= r; ++k)
					a += taps[static_cast<size_t>(k + r)] * src[static_cast<size_t>(y) * w + OldReflect(x + k, w)];
				tmp[static_cast<size_t>(y) * w + x] = a;
			}
		dst.resize(src.size());
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				float a = 0.f;
				for (int k = -r; k <= r; ++k)
					a += taps[static_cast<size_t>(k + r)] * tmp[static_cast<size_t>(OldReflect(y + k, h)) * w + x];
				dst[static_cast<size_t>(y) * w + x] = a;
			}
	}

	std::vector<double> Series(size_t n, unsigned seed)
	{
		std::vector<double> v(n);
		uint32_t s = seed * 2654435761u + 12345u;
		for (size_t i = 0; i < n; ++i)
		{
			s = s * 1664525u + 1013904223u;
			v[i] = static_cast<double>(s >> 8) / 16777216.0 * 40.0 - 10.0;
		}
		return v;
	}

	TEST_CLASS(NearDupUnits)
	{
	public:
		TEST_METHOD(OnePoleMatchesTheOldForms)
		{
			const double dts[] = { -1.0, 0.0, 0.004, 0.0167, 0.1, 2.5 };
			const double taus[] = { 0.0, 0.05, 0.25, 3.0 };
			for (double dt : dts)
				for (double tau : taus)
				{
					// DitherNeed rule.
					const double oldDither = (tau > 0.0 && dt > 0.0) ? 1.0 - std::exp(-dt / tau) : 1.0;
					Assert::IsTrue(SameBits(oldDither, OnePole::AlphaFor(dt, tau)));
					// Ambient rule: dt as given (callers clamp dt >= 0).
					if (dt >= 0.0)
					{
						const double oldAmbient = tau > 0.0 ? 1.0 - std::exp(-dt / tau) : 1.0;
						Assert::IsTrue(SameBits(oldAmbient, OnePole::AlphaFor(dt, tau, OnePole::DtRule::AsGiven)));
					}
				}
			for (double tau : { 0.1, 0.25, 1.0 })
				for (double rate : { 24.0, 30.0, 50.0, 60.0, 120.0 })
					Assert::IsTrue(SameBits(1.0 - std::exp(-1.0 / (tau * rate)), OnePole::AlphaPerFrame(tau, rate)));
		}

		TEST_METHOD(RobustStatsMatchTheOldCopies)
		{
			for (size_t n : { size_t(1), size_t(2), size_t(3), size_t(8), size_t(31), size_t(64), size_t(121) })
				for (unsigned seed = 1; seed <= 4; ++seed)
				{
					const std::vector<double> v = Series(n, seed);
					Assert::IsTrue(SameBits(OldMedianMean(v), RobustStats::Median(v)));
					Assert::IsTrue(SameBits(OldMedianUpper(v), RobustStats::Median(v, RobustStats::MedianRule::Upper, 1.0)));
					Assert::IsTrue(SameBits(OldMedianMean(v), RobustStats::MedianOf(v.data(), static_cast<int>(n))));
					// MAD about the median, both rules.
					const double med = OldMedianMean(v);
					std::vector<double> d;
					for (double x : v) d.push_back(std::fabs(x - med));
					Assert::IsTrue(SameBits(OldMedianMean(d), RobustStats::MadAbout(v, med)));
					Assert::IsTrue(SameBits(1.4826 * OldMedianMean(d), RobustStats::MadSigma(RobustStats::MadAbout(v, med))));
					// Floor-index quantile (the frame-quality reference).
					std::vector<double> s = v;
					std::sort(s.begin(), s.end());
					Assert::IsTrue(SameBits(s[static_cast<size_t>(0.8 * static_cast<double>(s.size() - 1))],
						RobustStats::Quantile(v, 0.8, RobustStats::RankRule::FloorIndex)));
				}
			Assert::AreEqual(0.0, RobustStats::Median(std::vector<double>()));
			Assert::AreEqual(1.0, RobustStats::Median(std::vector<double>(), RobustStats::MedianRule::Upper, 1.0));
			// The two median rules differ on even n by design (a registered axis, not merged).
			const std::vector<double> even = { 1.0, 2.0, 3.0, 10.0 };
			Assert::AreEqual(2.5, RobustStats::Median(even));
			Assert::AreEqual(3.0, RobustStats::Median(even, RobustStats::MedianRule::Upper));
			// Nearest-rank and interpolated rules.
			const std::vector<double> ten = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
			Assert::AreEqual(10.0, RobustStats::Quantile(ten, 0.95, RobustStats::RankRule::NearestRank));
			Assert::AreEqual(9.55, RobustStats::Quantile(ten, 0.95, RobustStats::RankRule::Interpolated), 1e-12);
		}

		TEST_METHOD(RampMatchesTheOldForms)
		{
			const double edges[] = { -1.0, 0.0, 0.25, 0.5, 1.0, 2.0 };
			const double xs[] = { -2.0, -0.5, 0.0, 0.1, 0.5, 0.75, 1.0, 1.5, 3.0 };
			for (double e0 : edges)
				for (double e1 : edges)
					for (double x : xs)
					{
						Assert::IsTrue(SameBits(OldSmoothLightLevel(e0, e1, x),
							RampMath::Smoothstep(e0, e1, x, RampMath::SmoothRule::StepWhenEqual)));
						Assert::IsTrue(SameBits(OldSmoothDither(e0, e1, x), RampMath::Smoothstep(e0, e1, x)));
						Assert::IsTrue(SameBits(OldSmoothDither(0.5f, 0.5f, x), RampMath::Smoothstep(0.5, 0.5, x)));
					}
			Assert::AreEqual(0.0, RampMath::Clamp01NanToZero(std::nan("")));
			Assert::AreEqual(1.0, RampMath::Clamp01NanToZero(7.0));
			Assert::AreEqual(0.0, RampMath::Clamp01(-3.0));
			Assert::AreEqual(1.0, RampMath::Ramp(5.0, 2.0, 2.0));   // degenerate: a step at hi
			Assert::AreEqual(0.0, RampMath::Ramp(1.0, 2.0, 2.0));
			Assert::AreEqual(0.5, RampMath::Ramp(1.5, 1.0, 2.0));
			Assert::IsTrue(SameBits(2.0 + (6.0 - 2.0) * 0.3, RampMath::Mix(2.0, 6.0, 0.3)));
		}

		TEST_METHOD(LumaWeightsTextEqualsTheConstants)
		{
			Assert::AreEqual(LumaCoeff::kBt709Kr, std::stod(VP_LUMA_BT709_KR_TEXT));
			Assert::AreEqual(LumaCoeff::kBt709Kg, std::stod(VP_LUMA_BT709_KG_TEXT));
			Assert::AreEqual(LumaCoeff::kBt709Kb, std::stod(VP_LUMA_BT709_KB_TEXT));
			Assert::AreEqual(std::string("vec3(0.2126, 0.7152, 0.0722)"), std::string(VP_LUMA_BT709_VEC3_TEXT));
		}

		TEST_METHOD(LumaWeightsTableIsUnchanged)
		{
			double k[3];
			LumaCoeff::ForSystem(PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, k);
			Assert::IsTrue(SameBits(0.2126, k[0]) && SameBits(1.0 - 0.2126 - 0.0722, k[1]) && SameBits(0.0722, k[2]));
			LumaCoeff::ForSystem(PL_COLOR_SYSTEM_BT_601, PL_COLOR_PRIM_BT_709, k);
			Assert::IsTrue(SameBits(0.299, k[0]) && SameBits(0.114, k[2]));
			LumaCoeff::ForSystem(PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_709, k);
			Assert::IsTrue(SameBits(0.2627, k[0]) && SameBits(0.0593, k[2]));
			LumaCoeff::ForSystem(PL_COLOR_SYSTEM_SMPTE_240M, PL_COLOR_PRIM_BT_709, k);
			Assert::IsTrue(SameBits(0.212, k[0]) && SameBits(0.087, k[2]));
			// An unknown tag follows the primaries.
			LumaCoeff::ForSystem(PL_COLOR_SYSTEM_UNKNOWN, PL_COLOR_PRIM_BT_2020, k);
			Assert::IsTrue(SameBits(0.2627, k[0]) && SameBits(0.0593, k[2]));
			LumaCoeff::ForSystem(PL_COLOR_SYSTEM_UNKNOWN, PL_COLOR_PRIM_BT_709, k);
			Assert::IsTrue(SameBits(0.2126, k[0]) && SameBits(0.0722, k[2]));
		}

		TEST_METHOD(ReflectBlurMatchesTheFrameQualityCopy)
		{
			using namespace VpUnit::BandFromBase;
			for (double sigma : { 1.0, 2.0 })
				for (int T : { 33, 64 })
				{
					const std::vector<float> taps = Taps<float>(sigma, 3.0, false, 0.0);
					const int r = Radius(sigma, 3.0, false, 0.0);
					Assert::AreEqual(static_cast<size_t>(2 * r + 1), taps.size());
					for (int i = -r; i <= r; ++i)
						Assert::IsTrue(SameBits(static_cast<float>(Weight(i, sigma, r)), taps[static_cast<size_t>(i + r)]));
					const std::vector<double> sd = Series(static_cast<size_t>(T) * T, 7);
					std::vector<float> src(sd.size());
					for (size_t i = 0; i < sd.size(); ++i) src[i] = static_cast<float>(sd[i]);
					std::vector<float> a, b;
					OldBlurReflect(src, a, T, T, taps);
					BlurTaps<float>(src, b, T, T, taps, EdgeMode::Reflect);
					Assert::AreEqual(a.size(), b.size());
					for (size_t i = 0; i < a.size(); ++i) Assert::IsTrue(SameBits(a[i], b[i]));
					for (int i = -5; i < T + 5; ++i) Assert::AreEqual(OldReflect(i, T), ReflectIndex(i, T));
				}
		}

		// FIX (registered): LightLevel::Blocks::TemporalAlpha for tau < 0 or NaN. Old: 1 - exp(-1/(tau*60)) gave a negative
		// (tau < 0) or NaN weight; new: "no smoothing", 1 (every OnePole entry). tau = 0 gave 1 before and still does.
		TEST_METHOD(LightLevelTemporalAlphaFixAndIdentity)
		{
			LightLevel::Blocks::Params p;
			for (double rate : { 0.0, 24.0, 30.0, 50.0, 60.0, 100.0, 120.0, 240.0 })
			{
				p.frameRateHz = rate;
				for (double tau : { 0.0, -0.3, -1.0, std::nan("") })
				{
					p.tauSeconds = tau;
					Assert::AreEqual(1.0, LightLevel::Blocks::TemporalAlpha(p));
				}
				for (double tau : { 0.05, 0.1, 0.3, 1.0, 5.0 })
				{
					p.tauSeconds = tau;
					const double alpha60 = 1.0 - std::exp(-1.0 / (tau * GeneralRate::kFrameCountReferenceHz));
					Assert::IsTrue(SameBits(GeneralRate::PerFrameWeight(alpha60, rate), LightLevel::Blocks::TemporalAlpha(p)));
				}
			}
		}

		TEST_METHOD(NanAndSmallDomainRules)
		{
			const double nan = std::nan("");
			Assert::IsTrue(std::isnan(RampMath::Clamp01(nan)));              // passes NaN through (the template copies)
			Assert::AreEqual(0.0, RampMath::Clamp01NanToZero(nan));
			Assert::IsTrue(std::isnan(RampMath::Ramp(nan, 0.0, 1.0)));
			// StepWhenNotAscending: NaN edge -> a step; StepWhenEqual: NaN edges are "not equal" -> the ramp (NaN out).
			Assert::AreEqual(OldSmoothDither(nan, 1.0, 0.5), RampMath::Smoothstep(nan, 1.0, 0.5));
			Assert::IsTrue(SameBits(OldSmoothLightLevel(nan, 1.0, 0.5), RampMath::Smoothstep(nan, 1.0, 0.5, RampMath::SmoothRule::StepWhenEqual)) ||
				(std::isnan(OldSmoothLightLevel(nan, 1.0, 0.5)) && std::isnan(RampMath::Smoothstep(nan, 1.0, 0.5, RampMath::SmoothRule::StepWhenEqual))));
			Assert::IsTrue(std::isnan(RampMath::Smoothstep(0.0, 1.0, nan)) == std::isnan(OldSmoothDither(0.0, 1.0, nan)));
		}

		TEST_METHOD(MedianFloatAndMedianOfMatchTheOldCopies)
		{
			for (size_t n : { size_t(1), size_t(2), size_t(5), size_t(6), size_t(33) })
			{
				const std::vector<double> d = Series(n, 11);
				std::vector<float> f;
				for (double x : d) f.push_back(static_cast<float>(x));
				std::vector<double> fd(f.begin(), f.end());
				Assert::IsTrue(SameBits(OldMedianMean(fd), RobustStats::Median(f)));
				Assert::IsTrue(SameBits(OldMedianUpper(fd), RobustStats::Median(f, RobustStats::MedianRule::Upper, 1.0)));
				Assert::IsTrue(SameBits(OldMedianMean(d), RobustStats::MedianOf(d.data(), static_cast<int>(n))));
			}
			Assert::AreEqual(7.0, RobustStats::MedianOf(nullptr, 0, RobustStats::MedianRule::MeanOfMiddle, 7.0));
		}

		TEST_METHOD(LumaFromRgbPassTextIsUnchanged)
		{
			const std::string expected =
				"//!PARAM vp_kr\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 1.0\n0.2126\n\n"
				"//!PARAM vp_kb\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 1.0\n0.0722\n\n"
				"//!PARAM vp_range_scale\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 2.0\n1.0\n\n"
				"//!PARAM vp_range_offset\n//!TYPE float\n//!MINIMUM -1.0\n//!MAXIMUM 1.0\n0.0\n\n"
				"//!DESC VP luma from RGB (the net's trained range)\n"
				"//!HOOK RGB\n"
				"//!BIND HOOKED\n"
				"//!COMPONENTS 1\n"
				"\n"
				"vec4 hook() {\n"
				"    ivec2 p = ivec2(HOOKED_pos * HOOKED_size);\n"
				"    vec4 c = texelFetch(HOOKED_raw, p, 0) * HOOKED_mul;\n"
				"    float y = vp_kr * c.x + (1.0 - vp_kr - vp_kb) * c.y + vp_kb * c.z;\n"
				"    return vec4(y * vp_range_scale + vp_range_offset, 0.0, 0.0, 1.0);\n"
				"}\n";
			Assert::AreEqual(expected, std::string(VpNets::kLumaFromRgbPass));
			// The splice used by the source-resolution shader: the text of the old literal.
			Assert::AreEqual(std::string("vec3(0.2126, 0.7152, 0.0722)"), std::string(VP_LUMA_BT709_VEC3_TEXT));
		}

		TEST_METHOD(ReplicateBlurMatchesTheDoubleReference)
		{
			using namespace VpUnit::BandFromBase;
			const int W = 40, H = 31;
			const std::vector<double> in = Series(static_cast<size_t>(W) * H, 3);
			const std::vector<double> ref = Blur(in, W, H, 1.7);
			const int r = Radius(1.7, 3.0, true, 0.0);
			std::vector<double> got;
			BlurTaps<double>(in, got, W, H, Taps<double>(1.7, 3.0, true, 0.0), EdgeMode::Replicate);
			Assert::AreEqual(ref.size(), got.size());
			for (size_t i = 0; i < ref.size(); ++i) Assert::AreEqual(ref[i], got[i], 1e-12);
			(void)r;
		}
	};
}
