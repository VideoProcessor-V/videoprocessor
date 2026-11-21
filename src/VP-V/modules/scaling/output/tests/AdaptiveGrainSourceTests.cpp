// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.output: the shared grain source's validity rule (X5-GRAIN-P10; gate cb-u1 r1 SHOULD 5), on the CPU tier. The three
// textures (P10 blocks, P11 mean luma, P11 coverage) must share ONE block grid, or the scaler's per-block reads would misalign
// silently. Textures are plain structs here: no device, no GPU object is touched.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(AdaptiveGrainSourceTests)
	{
	public:
		TEST_METHOD(GrainSourceIsValidOnlyForOneSharedBlockGrid)
		{
			pl_tex_t p10{}, s0{}, s4{};
			for (pl_tex_t* t : { &p10, &s0, &s4 })
			{
				t->params.w = 24;
				t->params.h = 15;
			}
			AdaptiveDownscale::GrainSource g;
			Assert::IsFalse(g.Valid(), L"a default source (no textures) is not valid");
			g.p10Blocks = &p10;
			g.p11Stats0 = &s0;
			g.p11Stats4 = &s4;
			Assert::IsTrue(g.SameGrid(), L"three textures of one grid share it");
			Assert::IsTrue(g.Valid(), L"three textures of one grid are a valid source");

			// Any one of the three on another grid (width or height) is a mismatch, whichever it is.
			for (pl_tex_t* odd : { &p10, &s0, &s4 })
			{
				const int w = odd->params.w, h = odd->params.h;
				odd->params.w = w / 2;
				Assert::IsFalse(g.SameGrid(), L"another width is a grid mismatch");
				Assert::IsFalse(g.Valid(), L"another width is not a valid source");
				odd->params.w = w;
				odd->params.h = h / 2;
				Assert::IsFalse(g.SameGrid(), L"another height is a grid mismatch");
				Assert::IsFalse(g.Valid(), L"another height is not a valid source");
				odd->params.h = h;
				Assert::IsTrue(g.Valid(), L"restored: valid again");
			}
		}

		TEST_METHOD(GrainSourceNeedsAllThreeTexturesAndPositiveScalars)
		{
			pl_tex_t p10{}, s0{}, s4{};
			for (pl_tex_t* t : { &p10, &s0, &s4 })
			{
				t->params.w = 24;
				t->params.h = 15;
			}
			auto make = [&]()
			{
				AdaptiveDownscale::GrainSource g;
				g.p10Blocks = &p10;
				g.p11Stats0 = &s0;
				g.p11Stats4 = &s4;
				return g;
			};
			Assert::IsTrue(make().Valid(), L"the complete source is valid");
			{
				AdaptiveDownscale::GrainSource g = make();
				g.p10Blocks = nullptr;
				Assert::IsFalse(g.Valid(), L"no P10 blocks");
			}
			{
				AdaptiveDownscale::GrainSource g = make();
				g.p11Stats0 = nullptr;
				Assert::IsFalse(g.Valid(), L"no P11 mean luma");
			}
			{
				AdaptiveDownscale::GrainSource g = make();
				g.p11Stats4 = nullptr;
				Assert::IsFalse(g.Valid(), L"no P11 coverage");
			}
			{
				AdaptiveDownscale::GrainSource g = make();
				g.blockPx = 0;
				Assert::IsFalse(g.Valid(), L"a zero block size");
			}
			{
				AdaptiveDownscale::GrainSource g = make();
				g.codeToEightBit = 0.0f;
				Assert::IsFalse(g.Valid(), L"a zero code scale");
			}
		}

		TEST_METHOD(GrainEnergyPerSigmaIsTheWhiteNoiseLaplacianConstant)
		{
			// The energy per sigma constant is sqrt(20) x sqrt(2 / pi) to float precision (the derivation in AdaptiveDownscale.h);
			// it fails on the old mistyped literal 3.5682208 (2.7e-5 off). The mapping of each reported case to its status line
			// and fallback reason is tested on the stage (AdaptiveScalingHookTests, WARP).
			const double exact = std::sqrt(20.0) * std::sqrt(2.0 / 3.14159265358979323846);
			Assert::IsTrue(std::fabs(static_cast<double>(AdaptiveDownscale::kEdgeEnergyPerSigma) - exact) < 1e-6,
				L"kEdgeEnergyPerSigma = sqrt(20) sqrt(2/pi) = 3.5682482");
		}

		TEST_METHOD(GrainSourceSameGridIsNullSafe)
		{
			// Gate cb-u1 r2 NIT: SameGrid() is public; with any texture missing it answers "no shared grid" instead of
			// dereferencing a null texture (before f41a2c1c5 each of these calls read through a null pointer).
			pl_tex_t p10{}, s0{}, s4{};
			for (pl_tex_t* t : { &p10, &s0, &s4 })
			{
				t->params.w = 24;
				t->params.h = 15;
			}
			Assert::IsFalse(AdaptiveDownscale::GrainSource().SameGrid(), L"no textures: no shared grid");
			for (int missing = 0; missing < 3; ++missing)
			{
				AdaptiveDownscale::GrainSource g;
				g.p10Blocks = missing == 0 ? nullptr : &p10;
				g.p11Stats0 = missing == 1 ? nullptr : &s0;
				g.p11Stats4 = missing == 2 ? nullptr : &s4;
				Assert::IsFalse(g.SameGrid(), L"one texture missing: no shared grid");
			}
			AdaptiveDownscale::GrainSource all;
			all.p10Blocks = &p10; all.p11Stats0 = &s0; all.p11Stats4 = &s4;
			Assert::IsTrue(all.SameGrid(), L"control: all three present on one grid share it");
		}
	};
}
