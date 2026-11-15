// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/RobustStats.h>
#include <VP-V/core/RobustStatsShaders.h>
#include <VP-V/modules/motion/estimation/global_model/PhotometricRefineGpu.h>
#include <VP-V/modules/motion/estimation/matcher/DejudderWarp.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// C6-2 U24 (plan lead18/coreplan-C6-2-U24.md (e)): the one robust-statistics owner, core/RobustStats.h, and its GPU
// twins in core/RobustStatsShaders.h. Known values per rule, empty and single inputs, input untouched, and the shader
// texts against the text they replace (golden). CPU only.
namespace RobustStatsTest
{
	using namespace RobustStats;

	bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

	// The matcher's GLSL median before C6-2 U24 (DejudderWarp.cpp MEDIAN_GLSL and the identical SMOOTH copy), verbatim.
	const char* const kPreChangeDejudderMedianGlsl = R"GLSL(
float median_(float a[9], int n)
{
    for (int i = 1; i < 9; i++) {
        if (i >= n) break;
        float x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
    return a[n / 2];
}
)GLSL";

	// DejudderWarp.cpp SMOOTH_HEADER (D3 smoothing) before C6-2 U24, verbatim.
	const char* const kPreChangeDejudderSmoothHeader = R"GLSL(
ivec2 half_size_;
float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
float sad_sub(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + v));
        }
    return s;
}
float median_(float a[9], int n)
{
    for (int i = 1; i < 9; i++) {
        if (i >= n) break;
        float x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
    return a[n / 2];
}
)GLSL";

	std::string Unix(const std::string& s)
	{
		std::string o;
		for (char c : s)
			if (c != '\r') o.push_back(c);
		return o;
	}

	// The documented C6-2 U24 rename, the only allowed difference: every `median_(` becomes `vpu_median_upper(`.
	std::string Renamed(const std::string& s)
	{
		std::string o = Unix(s);
		const std::string from = "median_(", to = "vpu_median_upper(";
		for (size_t at = o.find(from); at != std::string::npos; at = o.find(from, at + to.size()))
			o.replace(at, from.size(), to);
		return o;
	}

	size_t Count(const std::string& s, const std::string& what)
	{
		size_t n = 0;
		for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size()))
			++n;
		return n;
	}

	TEST_CLASS(RobustStatsUnit)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(MedianRulesEmptySingleEvenOdd)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MedianRulesEmptySingleEvenOdd)
		{
			const std::vector<double> none;
			Assert::AreEqual(0.0, Median(none));
			Assert::AreEqual(-3.0, Median(none, MedianRule::Upper, -3.0));
			Assert::AreEqual(5.0, MedianOf(nullptr, 0, MedianRule::Upper, 5.0));
			Assert::AreEqual(0, NthSmallest(std::vector<int>(), 2));   // empty -> T()
			const std::vector<double> one = { 4.5 };
			Assert::AreEqual(4.5, Median(one));
			Assert::AreEqual(4.5, Median(one, MedianRule::Upper));
			const std::vector<double> odd = { 9.0, 1.0, 5.0, 3.0, 7.0 };
			Assert::AreEqual(5.0, Median(odd));
			Assert::AreEqual(5.0, Median(odd, MedianRule::Upper));
			const std::vector<double> even = { 8.0, 2.0, 6.0, 4.0 };
			Assert::AreEqual(5.0, Median(even));                       // mean of the middle two
			Assert::AreEqual(6.0, Median(even, MedianRule::Upper));    // sorted[n / 2]
			Assert::AreEqual(5.0, MedianOf(even.data(), 4));
			Assert::AreEqual(6.0, MedianOf(even.data(), 4, MedianRule::Upper));
			// Input untouched (every helper copies).
			const std::vector<double> before = even;
			(void)Median(even, MedianRule::Upper);
			(void)Quantile(even, 0.5);
			(void)Mad(even);
			(void)MedianOf(even.data(), 4, MedianRule::Upper);
			Assert::IsTrue(even == before);
			// NthSmallest: the callers' k on their element types; k clamped to the last.
			const std::vector<float> f = { 3.f, 1.f, 2.f, 0.f };
			Assert::AreEqual(0.f, NthSmallest(f, 0));
			Assert::AreEqual(2.f, NthSmallest(f, 2));
			Assert::AreEqual(3.f, NthSmallest(f, 99));
			const std::vector<unsigned char> u8 = { 7, 200, 3, 50, 9 };
			Assert::AreEqual(9, static_cast<int>(NthSmallest(u8, u8.size() / 2)));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(MedianInPlaceSameValueNoAllocation)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MedianInPlaceSameValueNoAllocation)
		{
			// The PhotometricRefine |r| scratch form: same value as the old in-place nth_element (sorted[n / 2]) and as
			// Median(), the buffer keeps its storage (no allocation per pass) and still holds the same values.
			for (size_t n : { size_t(1), size_t(2), size_t(7), size_t(64), size_t(301) })
			{
				std::vector<double> buf;
				buf.reserve(512);
				uint32_t s = static_cast<uint32_t>(n) * 2654435761u + 7u;
				for (size_t i = 0; i < n; ++i)
				{
					s = s * 1664525u + 1013904223u;
					buf.push_back(std::fabs(static_cast<double>(s >> 8) / 16777216.0 * 20.0 - 10.0));
				}
				std::vector<double> old = buf;
				const size_t mid = old.size() / 2;
				std::nth_element(old.begin(), old.begin() + static_cast<std::ptrdiff_t>(mid), old.end());
				const double expect = 1.4826 * old[mid];
				const double byValue = Median(buf, MedianRule::Upper);
				std::vector<double> sortedBefore = buf;
				std::sort(sortedBefore.begin(), sortedBefore.end());
				const double* data = buf.data();
				const size_t cap = buf.capacity();
				const double inPlace = MedianInPlace(buf, MedianRule::Upper);
				Assert::IsTrue(SameBits(expect, MadSigma(inPlace)));
				Assert::IsTrue(SameBits(byValue, inPlace));
				Assert::IsTrue(data == buf.data() && cap == buf.capacity() && buf.size() == n);
				std::sort(buf.begin(), buf.end());
				Assert::IsTrue(buf == sortedBefore);
			}
			std::vector<double> empty;
			Assert::AreEqual(2.0, MedianInPlace(empty, MedianRule::Upper, 2.0));
			std::vector<double> even = { 8.0, 2.0, 6.0, 4.0 };
			Assert::AreEqual(5.0, MedianInPlace(even));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(MadKnownValues)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MadKnownValues)
		{
			// |v - 3| = {2, 1, 0, 1, 6} -> median 1.
			const std::vector<double> v = { 1.0, 2.0, 3.0, 4.0, 9.0 };
			Assert::AreEqual(1.0, Mad(v));
			Assert::AreEqual(1.0, MadAbout(v, 3.0));
			Assert::AreEqual(1.0, MadAboutOf(v.data(), 5, 3.0));
			// First four about 0: |v| = {1, 2, 3, 4}: MeanOfMiddle 2.5, Upper 3.
			Assert::AreEqual(2.5, MadAboutOf(v.data(), 4, 0.0));
			Assert::AreEqual(3.0, MadAboutOf(v.data(), 4, 0.0, MedianRule::Upper));
			Assert::AreEqual(7.0, MadAboutOf(nullptr, 0, 1.0, MedianRule::MeanOfMiddle, 7.0));
			Assert::AreEqual(0.0, MadAboutOf(nullptr, -1, 1.0));
			// MadAboutOf == the hand loops it replaces (ChainEvidence: std::abs on double, MeanOfMiddle median), bit for bit.
			const double w[] = { 0.25, -1.5, 3.75, 2.0, -0.125, 8.5 };
			const double med = MedianOf(w, 6);
			std::vector<double> dev(6);
			for (size_t i = 0; i < 6; ++i) dev[i] = std::abs(w[i] - med);
			Assert::IsTrue(SameBits(MedianOf(dev.data(), 6), MadAboutOf(w, 6, med)));
			Assert::AreEqual(1.4826, kMadToSigma);
			Assert::IsTrue(SameBits(1.4826 * 2.0, MadSigma(2.0)));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(QuantileEndsAndMiddlePerRankRule)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(QuantileEndsAndMiddlePerRankRule)
		{
			const std::vector<double> v = { 40.0, 10.0, 30.0, 20.0 };   // sorted 10 20 30 40
			// FloorIndex: sorted[floor(q * (n - 1))].
			Assert::AreEqual(10.0, Quantile(v, 0.0, RankRule::FloorIndex));
			Assert::AreEqual(40.0, Quantile(v, 1.0, RankRule::FloorIndex));
			Assert::AreEqual(20.0, Quantile(v, 0.5, RankRule::FloorIndex));
			// NearestRank: sorted[ceil(q * n) - 1]; q = 0 -> the first.
			Assert::AreEqual(10.0, Quantile(v, 0.0, RankRule::NearestRank));
			Assert::AreEqual(40.0, Quantile(v, 1.0, RankRule::NearestRank));
			Assert::AreEqual(20.0, Quantile(v, 0.5, RankRule::NearestRank));
			// Interpolated (type 7).
			Assert::AreEqual(10.0, Quantile(v, 0.0, RankRule::Interpolated));
			Assert::AreEqual(40.0, Quantile(v, 1.0, RankRule::Interpolated));
			Assert::AreEqual(25.0, Quantile(v, 0.5, RankRule::Interpolated));
			Assert::AreEqual(-1.0, Quantile(std::vector<double>(), 0.5, RankRule::FloorIndex, -1.0));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(TrimmedMeanOfRounding)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TrimmedMeanOfRounding)
		{
			const double v[] = { 100.0, 1.0, 2.0, 3.0, 4.0, -50.0, 5.0 };   // sorted -50 1 2 3 4 5 100
			Assert::AreEqual((-50.0 + 1.0 + 2.0 + 3.0 + 4.0 + 5.0 + 100.0) / 7.0, TrimmedMeanOf(v, 7, 0.0), 1e-12);
			// trim 0.2: k = floor(1.4) = 1 -> {1, 2, 3, 4, 5}.
			Assert::AreEqual(3.0, TrimmedMeanOf(v, 7, 0.2));
			// trim 0.29: k = floor(2.03) = 2 -> {2, 3, 4}.
			Assert::AreEqual(3.0, TrimmedMeanOf(v, 7, 0.29));
			// n - 2k <= 0: no trimming (trim 0.5 on n = 2 -> k = 1 -> nothing left -> k = 0).
			const double two[] = { 1.0, 3.0 };
			Assert::AreEqual(2.0, TrimmedMeanOf(two, 2, 0.5));
			Assert::AreEqual(0.0, TrimmedMeanOf(nullptr, 0, 0.1));
			Assert::AreEqual(4.0, TrimmedMeanOf(v + 4, 1, 0.4));   // n = 1: the single value v[4] = 4
			// Ascending summation order (the moved ChainCommon form): the sum runs over the sorted sample.
			const double big[] = { 1e16, 1.0, -1e16, 1.0 };
			double s = 0.0;
			for (double x : { -1e16, 1.0, 1.0, 1e16 }) s += x;
			Assert::IsTrue(SameBits(s / 4.0, TrimmedMeanOf(big, 4, 0.0)));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(NoSilentNaNHandling)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NoSilentNaNHandling)
		{
			// The helpers filter nothing (documented NaN policy: callers filter as before). Pinned: finite in -> finite out,
			// and a NaN input is NOT replaced by ifEmpty or any other value (no hidden fallback).
			const std::vector<double> v = { 1.0, 2.0, 3.0 };
			Assert::IsTrue(std::isfinite(Median(v)) && std::isfinite(Mad(v)) && std::isfinite(Quantile(v, 0.9)));
			const std::vector<double> oneNan = { std::nan("") };
			Assert::IsTrue(std::isnan(Median(oneNan, MedianRule::MeanOfMiddle, 7.0)));
			Assert::IsTrue(std::isnan(Median(oneNan, MedianRule::Upper, 7.0)));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ShaderTwinsMatchTheCpuOwnerAndThePreChangeText)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ShaderTwinsMatchTheCpuOwnerAndThePreChangeText)
		{
			// GLSL: the pre-change matcher text with only the function name changed to the owner prefix (golden).
			std::string golden = Unix(kPreChangeDejudderMedianGlsl);
			const std::string from = "float median_(", to = "float vpu_median_upper(";
			const size_t at = golden.find(from);
			Assert::IsTrue(at != std::string::npos);
			golden.replace(at, from.size(), to);
			Assert::AreEqual(golden, std::string(kMedianUpper9Glsl));
			Assert::IsTrue(std::string(kMedianUpper9Glsl).find("median_(") == std::string::npos);
			// HLSL: the text of VP_MAD_TO_SIGMA equals kMadToSigma and keeps the former inline literal's digits.
			const std::string h = kMadToSigmaHlsl;
			Assert::AreEqual(std::string("static const double VP_MAD_TO_SIGMA = 1.4826L;\n"), h);
			const size_t eq = h.find('=');
			Assert::IsTrue(eq != std::string::npos);
			Assert::AreEqual(kMadToSigma, std::stod(h.substr(eq + 1)));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(HostShaderTextsEqualThePreChangeTextApartFromTheRename)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(HostShaderTextsEqualThePreChangeTextApartFromTheRename)
		{
			// C5: the matcher's two headers are built from kMedianUpper9Glsl; the text handed to the shader compiler is the
			// pre-change text with only `median_` -> `vpu_median_upper` (golden, byte for byte).
			Assert::AreEqual(Renamed(kPreChangeDejudderMedianGlsl), Unix(Dejudder::SweepMedianGlslForTest()));
			Assert::AreEqual(Renamed(kPreChangeDejudderSmoothHeader), Unix(Dejudder::SmoothHeaderGlslForTest()));
			// PhotometricRefine HLSL: kMadToSigmaHlsl first, the factor used exactly once, no inline 1.4826 literal left.
			const std::string hlsl = CameraModel::RefineHlslSourceForTest();
			Assert::IsTrue(hlsl.compare(0, std::strlen(kMadToSigmaHlsl), kMadToSigmaHlsl) == 0);
			Assert::AreEqual(size_t(1), Count(hlsl, "1.4826"));                       // the declaration only
			Assert::AreEqual(size_t(1), Count(hlsl, "max(gMinScale, VP_MAD_TO_SIGMA * med)"));
			Assert::AreEqual(size_t(2), Count(hlsl, "VP_MAD_TO_SIGMA"));               // declaration + the one use
		}
	};
}
