// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC W4 (B1, GEM-221 / MUST-8): the process-wide running-pathway latch (VP-V/core/RunningPathway.h) and the two
// consumers that read it: QueueLookahead::ResolveActivePictureLookahead (an unset look-ahead is Bill's 0 on Original)
// and CaptureDisplayFixes::Enabled (off on Original whatever the switch). B2 (the profile runtime latches and uses the
// latch) is tested through the real snapshot builder in VpvProfileQueueAdviceTests.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/RunningPathwayLatch.h>
#include <VP-V/modules/input/queue_timing/QueueLookahead.h>
#include <CaptureDisplayFixes.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// The latch is process-wide in the test DLL: every test leaves it unknown (and the capture switch on, its default).
		struct LatchReset
		{
			LatchReset() { Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown); }
			~LatchReset()
			{
				Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
				CaptureDisplayFixes::Set(true);
			}
		};
	}

	TEST_CLASS(RunningPathwayTests)
	{
	public:
		TEST_METHOD(TheFirstLatchWinsAndUnknownIsNotOriginal)
		{
			LatchReset reset;
			Assert::IsFalse(Vpv::RunningPathway::Original(), L"unknown reads as Original");
			Assert::IsTrue(Vpv::RunningPathway::Latch(true), L"the first latch (Original) is not reported");
			Assert::IsTrue(Vpv::RunningPathway::Original());
			Assert::IsTrue(Vpv::RunningPathway::Latch(false), L"a later VP-V latch replaced Original");
			Assert::IsTrue(Vpv::RunningPathway::Original(), L"a later latch changed the running pathway");

			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
			Assert::IsFalse(Vpv::RunningPathway::Latch(false), L"the first latch (VP-V) reads as Original");
			Assert::IsFalse(Vpv::RunningPathway::Latch(true), L"a later Original latch replaced VP-V");
			Assert::IsFalse(Vpv::RunningPathway::Original());
			Assert::AreEqual(int(Vpv::RunningPathway::kVpv), Vpv::RunningPathway::Value().load());
		}

		// B1: unset on Original = Bill's application value 0 (Bill has no automatic look-ahead); a set value is kept.
		TEST_METHOD(OriginalHasNoAutomaticLookahead)
		{
			LatchReset reset;
			using QueueLookahead::ResolveActivePictureLookahead;
			for (const int state : { int(Vpv::RunningPathway::kUnknown), int(Vpv::RunningPathway::kVpv) })
			{
				Vpv::RunningPathway::SetForTesting(state);
				Assert::AreEqual(size_t(4), ResolveActivePictureLookahead(false, 0, false, 5, 32), L"VP-V / unknown: target 5 -> 4 (auto)");
			}
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kOriginal);
			Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 5, 32), L"Original: unset is not Bill's 0");
			Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 20, 32), L"Original: unset is not Bill's 0 (long queue)");
			Assert::AreEqual(size_t(3), ResolveActivePictureLookahead(true, 3, false, 5, 32), L"Original: a set look-ahead was dropped");
			Assert::AreEqual(size_t(8), ResolveActivePictureLookahead(true, 12, false, 5, 32), L"Original: a set look-ahead lost the renderer's cap");
		}

		// GEM-221 / MUST-8: capture_display_fixes is off on the Original pathway; on VP-V (and before the latch) it is the switch.
		TEST_METHOD(CaptureDisplayFixesAreOffOnOriginal)
		{
			LatchReset reset;
			CaptureDisplayFixes::Set(true);
			Assert::IsTrue(CaptureDisplayFixes::Enabled(), L"unknown: the switch (on) does not apply");
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kVpv);
			Assert::IsTrue(CaptureDisplayFixes::Enabled(), L"VP-V: the switch (on) does not apply");
			CaptureDisplayFixes::Set(false);
			Assert::IsFalse(CaptureDisplayFixes::Enabled(), L"VP-V: the switch (off) does not apply");
			CaptureDisplayFixes::Set(true);
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kOriginal);
			Assert::IsFalse(CaptureDisplayFixes::Enabled(), L"Original: the capture / display fixes still run");
			Assert::IsTrue(CaptureDisplayFixes::Flag().load(), L"Original changed the saved switch itself");
		}
	};
}
