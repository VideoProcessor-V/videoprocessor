// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// P13-DECL: the pin bookkeeping of the keyed source ring and its S7 invalidation contract (SourceRingPin.h). CPU only, no frames held.

#include <VP-V/modules/input/source_analysis/SourceRingPin.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SourceRingPinTestsNs
{
	using namespace VpSourceRing;

	TEST_CLASS(SourceRingPinTests)
	{
	public:
		TEST_METHOD(TheNoopHostRefusesVisiblyAndHoldsNothing)
		{
			NoopFramePin host;
			Assert::IsTrue(host.Pin(5) == PinResult::NotWired);
			Assert::IsFalse(host.Pinned(5));
			Assert::IsFalse(host.Release(5));
			Assert::IsTrue(std::string(Token(PinResult::NotWired)).find("not bound") != std::string::npos);
		}

		TEST_METHOD(AnUnboundBookRefusesAndCounts)
		{
			FramePinBook b;
			Assert::IsTrue(b.Pin(1) == PinResult::NotWired);
			Assert::AreEqual(uint64_t(1), b.Counters().refusedNotWired);
			Assert::AreEqual(size_t(0), b.PinCount());
		}

		TEST_METHOD(OnlyFramesInsideTheWindowCanBePinned)
		{
			FramePinBook b;
			b.SetWindow(100, 3, 2);   // addressable 97..102
			Assert::IsTrue(b.Pin(97) == PinResult::Pinned);
			Assert::IsTrue(b.Pin(102) == PinResult::Pinned);
			Assert::IsTrue(b.Pin(96) == PinResult::OutsideWindow);
			Assert::IsTrue(b.Pin(103) == PinResult::OutsideWindow);
			Assert::AreEqual(uint64_t(2), b.Counters().refusedOutsideWindow);
			Assert::AreEqual(size_t(2), b.PinCount());
		}

		TEST_METHOD(AtMostThreePinsAndRepinTakesNoSecond)
		{
			FramePinBook b;
			b.SetWindow(10, 8, 0);
			Assert::IsTrue(b.Pin(5) == PinResult::Pinned);
			Assert::IsTrue(b.Pin(5) == PinResult::AlreadyPinned);
			Assert::IsTrue(b.Pin(6) == PinResult::Pinned);
			Assert::IsTrue(b.Pin(7) == PinResult::Pinned);
			Assert::AreEqual(size_t(VpSkeleton::kMaxRingPins), b.PinCount());
			Assert::IsTrue(b.Pin(8) == PinResult::TooManyPins);
			Assert::AreEqual(uint64_t(1), b.Counters().refusedTooMany);
			Assert::IsTrue(b.Release(6));
			Assert::IsFalse(b.Release(6));
			Assert::IsTrue(b.Pin(8) == PinResult::Pinned);
		}

		TEST_METHOD(APinOutlivesTheWindowMovingPastIt)
		{
			FramePinBook b;
			b.SetWindow(10, 2, 0);
			Assert::IsTrue(b.Pin(8) == PinResult::Pinned);
			b.SetWindow(20, 2, 0);
			Assert::IsTrue(b.Pinned(8));
			Assert::IsTrue(b.Pin(8) == PinResult::AlreadyPinned);
		}

		TEST_METHOD(EveryQueueFactInvalidatesEveryPin)
		{
			const Invalidation facts[] = { Invalidation::Flush, Invalidation::Discontinuity, Invalidation::FormatChange, Invalidation::Reset };
			uint32_t epoch = 0;
			for (Invalidation f : facts) {
				FramePinBook b;
				b.SetWindow(50, 4, 4);
				b.Pin(48); b.Pin(50); b.Pin(54);
				Assert::AreEqual(size_t(3), b.PinCount());
				b.OnQueueFact(f);
				Assert::AreEqual(size_t(0), b.PinCount());
				Assert::IsFalse(b.Pinned(48) || b.Pinned(50) || b.Pinned(54));
				Assert::AreEqual(uint64_t(3), b.Counters().pinsDropped);
				Assert::AreEqual(uint64_t(1), b.Counters().invalidations);
				Assert::AreEqual(uint32_t(1), b.Epoch());
				Assert::IsTrue(b.Pin(48) == PinResult::Pinned, L"the window is still bound: a fresh pin after the fact works");
				Assert::IsTrue(std::string(Token(f)).size() > 0);
				epoch = b.Epoch();
			}
			Assert::AreEqual(uint32_t(1), epoch);
		}

		TEST_METHOD(UnbindingRefusesNewPinsKeepsOldUntilAFact)
		{
			FramePinBook b;
			b.SetWindow(10, 2, 2);
			b.Pin(10);
			b.Unbind();
			Assert::IsTrue(b.Pin(11) == PinResult::NotWired);
			Assert::IsTrue(b.Pinned(10));
			b.OnQueueFact(Invalidation::Flush);
			Assert::IsFalse(b.Pinned(10));
		}

		TEST_METHOD(AWindowNearZeroDoesNotWrap)
		{
			FramePinBook b;
			b.SetWindow(1, 5, 1);   // 0..2
			Assert::IsTrue(b.Pin(0) == PinResult::Pinned);
			Assert::IsTrue(b.Pin(3) == PinResult::OutsideWindow);
		}
	};
}
