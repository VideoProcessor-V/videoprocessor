// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.output: the shared grain source's validity rule (X5-GRAIN-P10; gate cb-u1 r1 SHOULD 5), on the CPU tier. The three
// textures (P10 blocks, P11 mean luma, P11 coverage) must share ONE block grid, or the scaler's per-block reads would misalign
// silently. Textures are plain structs here: no device, no GPU object is touched.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <VP-V/modules/repair/grain_split/GrainLevelGpu.h>
#include <VP-V/modules/repair/grain_split/P10gCpu.h>

#include <cmath>
#include <memory>

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

		TEST_METHOD(GrainLevelFactsCodeToEightBitAndMetadataRoundTrip)
		{
			// POD2-H1: kP10gGrainLevel carries native bit depth (codeToEightBit = 255 / ((1 << depth) - 1)) and blockPx.
			Assert::IsTrue(std::fabs(GrainSplit::LevelGpu::CodeToEightBitOf(8) - 1.0) < 1e-12,
				L"8-bit codeToEightBit must be 255/255 = 1.0");
			Assert::IsTrue(std::fabs(GrainSplit::LevelGpu::CodeToEightBitOf(10) - (255.0 / 1023.0)) < 1e-12,
				L"10-bit codeToEightBit must be 255/1023");
			Assert::IsTrue(std::fabs(GrainSplit::LevelGpu::CodeToEightBitOf(12) - (255.0 / 4095.0)) < 1e-12,
				L"12-bit codeToEightBit must be 255/4095");
			Assert::IsTrue(std::fabs(GrainSplit::LevelGpu::CodeToEightBitOf(0) - (255.0 / 1023.0)) < 1e-12,
				L"out-of-range depth defaults to 10-bit");
			for (int d : { 8, 10, 12 })
			{
				GrainSplit::LevelCpu::Meta m;
				m.sourceBits = d;
				m.blockPx = 16;
				Assert::IsTrue(std::fabs(GrainSplit::LevelCpu::CodeToEightBitOf(d) - GrainSplit::LevelGpu::CodeToEightBitOf(d)) < 1e-12,
					L"LevelCpu::CodeToEightBitOf must match LevelGpu::CodeToEightBitOf");
				Assert::IsTrue(std::fabs(m.CodeToEightBit() - GrainSplit::LevelGpu::CodeToEightBitOf(d)) < 1e-12,
					L"LevelCpu::Meta::CodeToEightBit() must match CodeToEightBitOf");
			}

			// Negative control (POD2-H1 4x bug): a 1.0 8-bit-code sigma in a 12-bit source has sigma_12 = 4095/255.
			// With 12-bit facts, energy matches 1.0 * kEdgeEnergyPerSigma; the old hardcoded 10-bit scale is ~4x too large.
			const double sigma12 = 4095.0 / 255.0;
			const double energy12 = sigma12 * AdaptiveDownscale::GrainLevelEnergyScale(
				static_cast<float>(GrainSplit::LevelGpu::CodeToEightBitOf(12)));
			const double naive10BitEnergy = sigma12 * AdaptiveDownscale::GrainLevelEnergyScale(255.0f / 1023.0f);
			Assert::IsTrue(std::fabs(energy12 - static_cast<double>(AdaptiveDownscale::kEdgeEnergyPerSigma)) < 1e-4,
				L"12-bit facts must normalize 12-bit sigma back to 1.0 8-bit code energy");
			Assert::IsTrue(naive10BitEnergy > 3.9 * static_cast<double>(AdaptiveDownscale::kEdgeEnergyPerSigma),
				L"negative control: hardcoded 10-bit scale overstates 12-bit grain sigma by ~4x");

			// FactsOf(AcquireResult) round-trip + untagged negative control.
			auto facts = std::make_shared<GrainSplit::LevelGpu::GrainLevelFacts>();
			facts->sourceBits = 12;
			facts->blockPx = 32;
			facts->codeToEightBit = GrainSplit::LevelGpu::CodeToEightBitOf(12);
			VpSkeleton::AcquireResult r;
			r.status = VpSkeleton::AcquireStatus::Reused;
			r.gpu.textureSet = GrainSplit::LevelGpu::GrainLevelFactsTag();
			r.gpu.keepAlive = facts;
			const GrainSplit::LevelGpu::GrainLevelFacts* read = GrainSplit::LevelGpu::FactsOf(r);
			Assert::IsNotNull(read, L"tagged AcquireResult must yield GrainLevelFacts");
			Assert::AreEqual(12, read->sourceBits, L"sourceBits must round-trip");
			Assert::AreEqual(32, read->blockPx, L"blockPx must round-trip");
			Assert::IsTrue(std::fabs(read->codeToEightBit - (255.0 / 4095.0)) < 1e-12, L"codeToEightBit must round-trip");

			VpSkeleton::AcquireResult untagged = r;
			untagged.gpu.textureSet = nullptr;
			Assert::IsNull(GrainSplit::LevelGpu::FactsOf(untagged),
				L"negative control: untagged AcquireResult must return nullptr");
			VpSkeleton::AcquireResult failed = r;
			failed.status = VpSkeleton::AcquireStatus::Unavailable;
			Assert::IsNull(GrainSplit::LevelGpu::FactsOf(failed),
				L"negative control: unavailable AcquireResult must return nullptr");
		}
	};
}
