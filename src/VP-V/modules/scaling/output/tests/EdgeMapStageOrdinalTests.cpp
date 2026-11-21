// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// CB slice 18 S3 (coreplan-CB-18-edgemap.md S3, EDGEMAP-EXTRAS-KEY; lead ruling 5): X5 asks kEdgeMap for each picture it measures
// under (seq, stage, size, ordinal), with the ordinal from EdgeMapStageOrdinal. Two pictures of the SAME seq (the main render and a
// de-judder extras re-render, or a cadence repeat) must get different ordinals, so their P13 stage keys and kEdgeMap stage keys
// differ and the second is never served the first one's edge map. CPU tier, no GPU object.

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/VariantKey.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>
#include <VP-V/modules/scaling/output/EdgeMapStageOrdinal.h>

#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace EdgeMapStageOrdinalTestsNs
{
	uint8_t X5Stage() { return VpSkeleton::VariantKey::StageAfter(uint8_t(PipelineSlots::Id::ScalingGeometry)); }

	VpSkeleton::VariantKey EdgeKey(uint16_t ordinal) { return Vpv::Regions::EdgeMapStageKey(X5Stage(), 1920u, 1080u, ordinal); }
	VpSkeleton::VariantKey PlaneKey(uint16_t ordinal) { return Vpv::Capture::SourcePlanes::StageKey(X5Stage(), 1920u, 1080u, ordinal); }

	TEST_CLASS(EdgeMapStageOrdinalTests)
	{
	public:
		TEST_METHOD(FirstMeasureOfASeqIsOrdinalZero)
		{
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			Assert::AreEqual(0, int(o.Next(42u)), L"the main render of a seq takes ordinal 0 (the key the first measure asks)");
		}

		TEST_METHOD(TwoRendersOfOneSeqGetDifferentKeys)
		{
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			const uint16_t mainOrdinal = o.Next(7u);
			const uint16_t extrasOrdinal = o.Next(7u);
			Assert::AreNotEqual(int(mainOrdinal), int(extrasOrdinal), L"main render and extras re-render of one seq: different ordinals");
			Assert::IsTrue(EdgeKey(mainOrdinal) != EdgeKey(extrasOrdinal), L"so their kEdgeMap stage keys differ");
			Assert::IsTrue(PlaneKey(mainOrdinal) != PlaneKey(extrasOrdinal), L"and their P13 stage-picture keys differ");
			const uint16_t third = o.Next(7u);
			Assert::IsTrue(third != mainOrdinal && third != extrasOrdinal, L"a third picture of the seq (cadence repeat) is distinct too");
			Assert::AreEqual(2, int(third), L"ordinals count 0, 1, 2 within one seq");
		}

		TEST_METHOD(ANewSeqStartsAtZeroAgain)
		{
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			o.Next(1u);
			o.Next(1u);
			Assert::AreEqual(0, int(o.Next(2u)), L"a new seq is a first measure");
			Assert::AreEqual(1, int(o.Next(2u)), L"and its re-render is 1");
		}

		TEST_METHOD(ResetMakesTheNextMeasureAFirstOne)
		{
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			o.Next(5u);
			o.Next(5u);
			o.Reset();
			Assert::AreEqual(0, int(o.Next(5u)), L"after Reset (release / discontinuity) the same seq starts at 0");
		}

		TEST_METHOD(SeqZeroIsAFirstMeasureOnAFreshCounter)
		{
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			Assert::AreEqual(0, int(o.Next(0u)), L"seq 0 on a fresh counter is not mistaken for a repeat of the initial state");
			Assert::AreEqual(1, int(o.Next(0u)), L"its second measure is 1");
		}

		TEST_METHOD(KeysOfDifferentSeqsMayShareOrdinalZero)
		{
			// The seq is the Acquire argument, not a key axis: two frames' first measures share the key and are told apart by seq.
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			const uint16_t a = o.Next(10u);
			const uint16_t b = o.Next(11u);
			Assert::IsTrue(EdgeKey(a) == EdgeKey(b), L"first measures of two seqs: same key (seq separates them in Acquire)");
			Assert::AreEqual(8, int(EdgeKey(a).codeUnit), L"the kEdgeMap stage key carries code unit 8");
			Assert::AreEqual(int(X5Stage()), int(EdgeKey(a).stage), L"X5 asks the stage after scaling.geometry");
		}

		TEST_METHOD(OrdinalWrapsAt65536MeasuresOfOneSeq)
		{
			AdaptiveDownscale::EdgeMapStageOrdinal o;
			uint16_t last = 0;
			for (uint32_t i = 0; i < 65536u; ++i) last = o.Next(3u);
			Assert::AreEqual(65535, int(last), L"the 65536th measure of one seq is ordinal 65535");
			Assert::AreEqual(0, int(o.Next(3u)), L"then the ordinal wraps (harmless: the stage entries are frame-scoped)");
		}
	};
}
