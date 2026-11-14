// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// S-12BIT (refs/119 #29 / #30 / #48 / #49 / #51): the intermediate-picture precision follows the setting alone and is
// visible; libplacebo's latched error flags are visible.

#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/modules/control/diagnostics/LibplaceboErrors.h>
#include <VP-V/modules/scaling/output/PicturePrecision.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace PicturePrecisionTest
{
	TEST_CLASS(PicturePrecisionTests)
	{
	public:
		TEST_METHOD(FloatIntermediatesFollowTheSettingAlone)
		{
			Assert::IsTrue(PicturePrecision::FboFloat32(-1), L"auto is float32");
			Assert::IsTrue(PicturePrecision::FboFloat32(1));
			Assert::IsFalse(PicturePrecision::FboFloat32(0));
		}

		TEST_METHOD(StableLaneIsLabelledBelowTwelveBit)
		{
			const FallbackCatalog::Decision d = PicturePrecision::Precision(false, false, -1);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("picture precision"), d.id);
			Assert::IsTrue(d.reason.find("Stable 7.360") != std::string::npos);
			Assert::IsTrue(d.reason.find("below 12-bit") != std::string::npos);
		}

		TEST_METHOD(BetaLaneIsQuietUnlessSetOff)
		{
			Assert::IsFalse(PicturePrecision::Precision(false, true, -1).engaged);
			Assert::IsFalse(PicturePrecision::Precision(false, true, 1).engaged);
			const FallbackCatalog::Decision off = PicturePrecision::Precision(false, true, 0);
			Assert::IsTrue(off.engaged);
			Assert::IsTrue(off.reason.find("fbo_float32 is off") != std::string::npos);
		}

		TEST_METHOD(OriginalPathwayIsNeverLabelled)
		{
			Assert::IsFalse(PicturePrecision::Precision(true, false, 0).engaged);
			Assert::IsFalse(PicturePrecision::Precision(true, true, 0).engaged);
		}

		TEST_METHOD(NoticeSentencesExist)
		{
			Assert::IsTrue(FallbackNoticeText("picture precision", std::string()).find("16-bit") != std::string::npos);
			Assert::IsTrue(FallbackNoticeText("libplacebo errors", std::string()).find("internal error") != std::string::npos);
		}

		TEST_METHOD(LatchedErrorsAreVisibleAndNamed)
		{
			Assert::IsFalse(LibplaceboErrors::Latched(0).engaged);
			const FallbackCatalog::Decision d = LibplaceboErrors::Latched(LibplaceboErrors::kFbo | LibplaceboErrors::kDebanding);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("libplacebo errors"), d.id);
			Assert::IsTrue(d.reason.find("FBO") != std::string::npos);
			Assert::IsTrue(d.reason.find("debanding") != std::string::npos);
			Assert::IsTrue(d.reason.find("sampling") == std::string::npos);
		}

		TEST_METHOD(OriginalPathwayReleasesTheEntries)
		{
			const int flags = LibplaceboErrors::kFbo | LibplaceboErrors::kSampling;
			Assert::IsTrue(LibplaceboErrors::Latched(flags, false).engaged);
			const FallbackCatalog::Decision off = LibplaceboErrors::Latched(flags, true);
			Assert::IsFalse(off.engaged, L"a stale entry must release in Original");
			Assert::AreEqual(std::string("libplacebo errors"), off.id);
			FallbackStatus::Registry registry;
			FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(flags, false));
			FallbackCatalog::Sync(registry, PicturePrecision::Precision(false, false, -1));
			Assert::IsTrue(registry.Engaged("libplacebo errors") && registry.Engaged("picture precision"));
			FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(flags, true));
			FallbackCatalog::Sync(registry, PicturePrecision::Precision(true, false, -1));
			Assert::IsFalse(registry.Engaged("libplacebo errors"));
			Assert::IsFalse(registry.Engaged("picture precision"));
		}

		TEST_METHOD(DisabledHooksAreNotDoubleReported)
		{
			Assert::IsFalse(LibplaceboErrors::Latched(LibplaceboErrors::kHooks).engaged, L"hooks have their own entry");
			Assert::IsTrue(LibplaceboErrors::Latched(LibplaceboErrors::kHooks | LibplaceboErrors::kBlur).engaged);
		}

		TEST_METHOD(EveryOtherBitHasAName)
		{
			for (int bit = 1; bit <= (1 << 12); bit <<= 1)
			{
				if (bit == LibplaceboErrors::kHooks) continue;
				Assert::IsFalse(LibplaceboErrors::Names(bit).empty(), L"a latched flag with no name");
			}
		}
	};
}
