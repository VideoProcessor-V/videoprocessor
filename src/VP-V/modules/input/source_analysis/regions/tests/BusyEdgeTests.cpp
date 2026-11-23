// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: the CPU references of the S5 busy-share variant and the edge map (lead8/iface-L-B5.md B, C).
#include "pch.h"
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>

#include <cmath>
#include <vector>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(BusyEdgeTests)
	{
	public:
		TEST_METHOD(BusyShareOfAQuantisedRampIsZero)
		{
			const int w = 64, h = 32;
			std::vector<float> p(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) p[static_cast<size_t>(y) * w + x] = float(x / 4);
			RegionEvidence::BusyEdge::PlaneGroup g; g.w = w; g.h = h; g.n = 1; g.comp[0] = p.data();
			const std::vector<float> s = RegionEvidence::BusyEdge::BusyShare(g, 16, 1.5f, 1.5f);
			Assert::AreEqual(size_t(4 * 2), s.size());
			for (float v : s) Assert::AreEqual(0.0f, v);
		}
		TEST_METHOD(BusyShareOfCheckerNoiseIsOne)
		{
			const int w = 32, h = 32;
			std::vector<float> p(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) p[static_cast<size_t>(y) * w + x] = ((x + y) & 1) ? 10.0f : 0.0f;
			RegionEvidence::BusyEdge::PlaneGroup g; g.w = w; g.h = h; g.n = 1; g.comp[0] = p.data();
			const std::vector<float> s = RegionEvidence::BusyEdge::BusyShare(g, 16, 2.0f, 2.0f);
			Assert::AreEqual(size_t(4), s.size(), L"32x32 with 16x16 blocks yields 4 blocks");
			for (float v : s) Assert::AreEqual(1.0f, v);
		}
		TEST_METHOD(BusyShareTakesTheLargestComponent)
		{
			const int w = 16, h = 16;
			std::vector<float> flat(static_cast<size_t>(w) * h, 5.0f), noisy(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) noisy[static_cast<size_t>(y) * w + x] = ((x + y) & 1) ? 8.0f : 0.0f;
			RegionEvidence::BusyEdge::PlaneGroup g; g.w = w; g.h = h; g.n = 2; g.comp[0] = flat.data(); g.comp[1] = noisy.data();
			Assert::AreEqual(1.0f, RegionEvidence::BusyEdge::BusyShare(g, 16, 2.0f, 2.0f)[0]);
			g.n = 1;
			Assert::AreEqual(0.0f, RegionEvidence::BusyEdge::BusyShare(g, 16, 2.0f, 2.0f)[0]);
		}
		TEST_METHOD(PartialEdgeBlockCountsInPlanePixelsOnly)
		{
			const int w = 20, h = 20;
			std::vector<float> p(static_cast<size_t>(w) * h, 3.0f);
			RegionEvidence::BusyEdge::PlaneGroup g; g.w = w; g.h = h; g.n = 1; g.comp[0] = p.data();
			const std::vector<float> s = RegionEvidence::BusyEdge::BusyShare(g, 16, 1.0f, 1.0f);
			Assert::AreEqual(size_t(4), s.size());
			for (float v : s) Assert::AreEqual(0.0f, v);
		}
		TEST_METHOD(SobelOfAVerticalStepIsFourTimesTheStep)
		{
			const int w = 8, h = 8;
			std::vector<float> p(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) p[static_cast<size_t>(y) * w + x] = x < 4 ? 100.0f : 140.0f;
			const std::vector<float> m = RegionEvidence::BusyEdge::SobelMagnitude(p.data(), w, h);
			Assert::AreEqual(160.0f, m[static_cast<size_t>(4) * w + 3], 1e-3f);
			Assert::AreEqual(160.0f, m[static_cast<size_t>(4) * w + 4], 1e-3f);
			Assert::AreEqual(0.0f, m[static_cast<size_t>(4) * w + 1]);
		}
	};
}
