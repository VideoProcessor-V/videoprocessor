/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// P15-DECL: the 64-byte frame-pattern record (identity/quality_pattern/QualityPatternRecord.h): exactly kQualityPatternBytes, stable
// field offsets, zero-initialised = Unknown / nothing valid / not provisional / not causal, spare words zero, and the P16 block-grid
// texture reserve constants (4 channels x 2 textures, cells derived from the dimensions, 240 x 135 at 4K). CPU only; nothing allocated.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Products.h>
#include <VP-V/modules/input/source_analysis/identity/quality_pattern/QualityPatternRecord.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(QualityPatternRecordTests)
	{
	public:
		TEST_METHOD(TheRecordIsExactly64BytesAndMatchesTheProductSize)
		{
			Assert::AreEqual(size_t(64), sizeof(QualityPattern::Record));
			Assert::AreEqual(size_t(VpSkeleton::kQualityPatternBytes), sizeof(QualityPattern::Record));
			Assert::AreEqual(uint32_t(VpSkeleton::kQualityPatternBytes), QualityPattern::kRecordBytes);
			Assert::AreEqual(1u, QualityPattern::kUnitVersion);
		}

		TEST_METHOD(ZeroInitialisedIsUnknownNotValidNotProvisionalNotCausal)
		{
			QualityPattern::Record r;
			std::memset(&r, 0, sizeof(r));
			Assert::AreEqual(0u, r.unit, L"never written");
			Assert::AreEqual(0u, r.validityBits);
			Assert::IsTrue(QualityPattern::FrameRole(r.role) == QualityPattern::FrameRole::Unknown);
			Assert::IsTrue(QualityPattern::PeakValley(r.peakValley) == QualityPattern::PeakValley::None);
			Assert::AreEqual(0, int(r.causal));
			Assert::IsTrue((r.validityBits & QualityPattern::kProvisional) == 0);
			const QualityPattern::Record v = QualityPattern::Record();   // value-initialisation is the same
			Assert::AreEqual(0u, v.unit);
			Assert::AreEqual(0, int(v.role));
		}

		TEST_METHOD(FieldOffsetsAreStable)
		{
			using R = QualityPattern::Record;
			Assert::AreEqual(size_t(0), offsetof(R, unit));
			Assert::AreEqual(size_t(4), offsetof(R, validityBits));
			Assert::AreEqual(size_t(8), offsetof(R, seq));
			Assert::AreEqual(size_t(16), offsetof(R, role));
			Assert::AreEqual(size_t(17), offsetof(R, leadUsed));
			Assert::AreEqual(size_t(18), offsetof(R, causal));
			Assert::AreEqual(size_t(19), offsetof(R, peakValley));
			Assert::AreEqual(size_t(20), offsetof(R, score));
			Assert::AreEqual(size_t(24), offsetof(R, rank));
			Assert::AreEqual(size_t(26), offsetof(R, windowFrames));
			Assert::AreEqual(size_t(28), offsetof(R, stepMean));
			Assert::AreEqual(size_t(30), offsetof(R, stepP90));
			Assert::AreEqual(size_t(32), offsetof(R, stepMax));
			Assert::AreEqual(size_t(34), offsetof(R, gridPeriodX));
			Assert::AreEqual(size_t(36), offsetof(R, gridPeriodY));
			Assert::AreEqual(size_t(38), offsetof(R, gridPhaseX));
			Assert::AreEqual(size_t(39), offsetof(R, gridPhaseY));
			Assert::AreEqual(size_t(40), offsetof(R, exactCopyFlags));
			Assert::AreEqual(size_t(41), offsetof(R, spareBytes));
			Assert::AreEqual(size_t(44), offsetof(R, spare));
		}

		TEST_METHOD(SparesStayZeroWhenTheNamedFieldsAreFilled)
		{
			QualityPattern::Record r;
			std::memset(&r, 0, sizeof(r));
			r.unit = QualityPattern::kUnitVersion;
			r.validityBits = QualityPattern::kValidRole | QualityPattern::kValidScore | QualityPattern::kProvisional;
			r.seq = 0x1122334455667788ull;
			r.role = uint8_t(QualityPattern::FrameRole::Peak);
			r.score = 0.75f;
			r.exactCopyFlags = QualityPattern::kCopyOfPrev1 | QualityPattern::kCopyOfPrev2;
			for (int i = 0; i < 3; ++i) Assert::AreEqual(0, int(r.spareBytes[i]));
			for (int i = 0; i < 5; ++i) Assert::AreEqual(0u, r.spare[i]);
			// the bytes after the last named field (offset 41..63) are all zero
			const uint8_t* b = reinterpret_cast<const uint8_t*>(&r);
			for (size_t i = 41; i < 64; ++i) Assert::AreEqual(0, int(b[i]));
			Assert::AreEqual(1, int(QualityPattern::kCopyOfPrev1));
			Assert::AreEqual(2, int(QualityPattern::kCopyOfPrev2));
		}

		TEST_METHOD(TheBlockGridReserveIsFourChannelsTwoTexturesAndDerivedFromTheDimensions)
		{
			using namespace QualityPattern;
			Assert::AreEqual(4u, kBlockGridChannels);
			Assert::AreEqual(2u, kBlockGridTextures);
			Assert::AreEqual(240u, BlockGridCellsX(3840));
			Assert::AreEqual(135u, BlockGridCellsY(2160));
			Assert::AreEqual(120u, BlockGridCellsX(1920));
			Assert::AreEqual(68u, BlockGridCellsY(1080), L"ceil(1080 / 16): derived, never a hard-coded resolution");
			Assert::AreEqual(1u, BlockGridCellsX(1));
			Assert::AreEqual(0u, BlockGridCellsX(0));
		}
	};
}
