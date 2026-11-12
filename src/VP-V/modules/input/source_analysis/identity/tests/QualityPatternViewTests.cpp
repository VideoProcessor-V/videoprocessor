/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// P15-DECL: QualityPatternView (identity/quality_pattern/QualityPatternView.h): an absent / never-written / other-version record shows
// "frame pattern: not produced" with the engaged FallbackCatalog decision product.quality_pattern; the causal (P15c) record shows
// "frame pattern: provisional"; both consumers (temporal-detail engine status, dither need) are STATUS-ONLY. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/source_analysis/identity/quality_pattern/QualityPatternView.h>

#include <cstring>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(QualityPatternViewTests)
	{
	public:
		static QualityPattern::Record Fresh()
		{
			QualityPattern::Record r;
			std::memset(&r, 0, sizeof(r));
			r.unit = QualityPattern::kUnitVersion;
			r.validityBits = QualityPattern::kValidRole;
			r.role = uint8_t(QualityPattern::FrameRole::Reference);
			return r;
		}

		TEST_METHOD(AnAbsentRecordShowsNotProducedAndEngagesTheCatalogEntry)
		{
			const QualityPattern::View v = QualityPattern::Read(nullptr, false);
			Assert::IsFalse(v.available);
			Assert::IsTrue(v.role == QualityPattern::FrameRole::Unknown);
			Assert::IsFalse(v.reason.empty());
			Assert::IsTrue(v.fallback.engaged && v.fallback.id == "product.quality_pattern");
			// ProductFallback format = catalog text + ": " + cause (FallbackCatalog.h ProductFallback)
			Assert::AreEqual(std::string("frame pattern record unavailable, stale or of another unit version: temporal detail and dither need show it as not produced and use their classical forms: no producer or no record"), v.fallback.reason);
			Assert::AreEqual(std::string("frame pattern: not produced"), QualityPattern::StatusText(v));
			Assert::IsNotNull(FallbackCatalog::FindProductEntry("product.quality_pattern"));
			// the causal reader sees the same absent state
			Assert::AreEqual(std::string("frame pattern: not produced"), QualityPattern::StatusText(QualityPattern::Read(nullptr, true)));
		}

		TEST_METHOD(ANeverWrittenOrOtherVersionRecordIsAbsentWithItsCause)
		{
			QualityPattern::Record z;
			std::memset(&z, 0, sizeof(z));
			const QualityPattern::View a = QualityPattern::Read(&z, false);
			Assert::IsFalse(a.available);
			Assert::AreEqual(std::string("record never written"), a.reason);
			QualityPattern::Record o = Fresh();
			o.unit = 7;
			const QualityPattern::View b = QualityPattern::Read(&o, false);
			Assert::IsFalse(b.available);
			Assert::AreEqual(std::string("record of another unit version"), b.reason);
			Assert::IsTrue(b.fallback.engaged);
		}

		TEST_METHOD(AFreshP15RecordIsAvailableAndNotProvisional)
		{
			const QualityPattern::Record r = Fresh();
			const QualityPattern::View v = QualityPattern::Read(&r, false);
			Assert::IsTrue(v.available && !v.provisional && v.reason.empty());
			Assert::IsTrue(v.role == QualityPattern::FrameRole::Reference);
			Assert::IsFalse(v.fallback.engaged);
			Assert::AreEqual(std::string("product.quality_pattern"), v.fallback.id, L"released, same id");
			Assert::AreEqual(std::string("frame pattern: available"), QualityPattern::StatusText(v));
		}

		TEST_METHOD(TheCausalRecordShowsProvisional)
		{
			const QualityPattern::Record r = Fresh();
			const QualityPattern::View viaReader = QualityPattern::Read(&r, true);
			Assert::IsTrue(viaReader.available && viaReader.provisional);
			Assert::AreEqual(std::string("frame pattern: provisional"), QualityPattern::StatusText(viaReader));
			QualityPattern::Record c = Fresh();
			c.causal = 1;
			Assert::IsTrue(QualityPattern::Read(&c, false).provisional, L"the record's own causal flag");
			QualityPattern::Record p = Fresh();
			p.validityBits |= QualityPattern::kProvisional;
			Assert::IsTrue(QualityPattern::Read(&p, false).provisional, L"the record's own provisional bit");
		}

		TEST_METHOD(AnOutOfRangeRoleReadsAsUnknown)
		{
			QualityPattern::Record r = Fresh();
			r.role = 200;
			Assert::IsTrue(QualityPattern::Read(&r, false).role == QualityPattern::FrameRole::Unknown);
		}

		TEST_METHOD(BothConsumersAreStatusOnly)
		{
			Assert::IsTrue(QualityPattern::StatusOnly(QualityPattern::Consumer::TemporalDetailEngine));
			Assert::IsTrue(QualityPattern::StatusOnly(QualityPattern::Consumer::DitherNeed), L"dither need shows the status; no behaviour change");
		}

		TEST_METHOD(TheCatalogEntryTextIsTheVisibleStatusNotASilentFallback)
		{
			const FallbackCatalog::ProductEntry* e = FallbackCatalog::FindProductEntry("product.quality_pattern");
			Assert::IsNotNull(e);
			Assert::IsTrue(std::string(e->text).find("not produced") != std::string::npos);
		}
	};
}
