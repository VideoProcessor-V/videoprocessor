// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Q0-A: the pulldown's resync policy (PulldownResync.h). Pure helpers; no Indicator method beyond
// Advance is needed (the cause is passed as an argument).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/queue_timing/PulldownResync.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;
using VpIndicator::Bit;

namespace Tests
{
	namespace PulldownResyncQ0
	{
		constexpr VpIndicator::CounterMask F = Bit(VpIndicator::kFull);
		constexpr VpIndicator::CounterMask M = Bit(VpIndicator::kContinuity);
		constexpr VpIndicator::CounterMask T = Bit(VpIndicator::kTiming);

		// A test-only cadence class: below 40 Hz, below 80 Hz, above (the policy has no rates of its own).
		int ClassOf(double hz) { return hz < 40.0 ? 0 : (hz < 80.0 ? 1 : 2); }

		PulldownResyncPolicy::Input In(VpIndicator::CounterMask changed, Cause full = Cause::None,
			double oldHz = 59.94, double newHz = 59.94, bool edge = false)
		{
			PulldownResyncPolicy::Input i;
			i.changed = changed;
			i.fullCause = full;
			i.oldHz = oldHz;
			i.newHz = newHz;
			i.modeWasOnNowOff = edge;
			return i;
		}

		PulldownResyncAction Decide(const PulldownResyncPolicy::Input& i)
		{
			return PulldownResyncPolicy::Decide(i, PulldownResyncPolicy::ClassFn(ClassOf));
		}
	}

	TEST_CLASS(PulldownResyncTests)
	{
	public:
		TEST_METHOD(NothingChangedDoesNothing)
		{
			Assert::IsTrue(PulldownResyncQ0::Decide(PulldownResyncQ0::In(0)).None());
			// A continuity-only change is not the pulldown's reader row.
			Assert::IsTrue(PulldownResyncQ0::Decide(PulldownResyncQ0::In(PulldownResyncQ0::M)).None());
		}

		TEST_METHOD(FullLiveQueueResetIsAbsorbed)
		{
			const PulldownResyncAction a = PulldownResyncQ0::Decide(
				PulldownResyncQ0::In(PulldownResyncQ0::F, Cause::LiveQueueReset));
			Assert::IsTrue(a.absorb);
			Assert::IsFalse(a.reset);
			Assert::IsFalse(a.rederiveHz);
		}

		TEST_METHOD(AnyOtherFullResets)
		{
			const Cause causes[] = { Cause::Start, Cause::StartAfterSourceChange, Cause::InPlaceSourceChange,
				Cause::RendererReset, Cause::CalibrationStart, Cause::QueueGenerationOther, Cause::FileDiscontinuity,
				Cause::CounterBackwards, Cause::TimestampBreakLarge, Cause::SourceFormatChanged };
			for (Cause c : causes)
			{
				const PulldownResyncAction a = PulldownResyncQ0::Decide(PulldownResyncQ0::In(PulldownResyncQ0::F, c));
				Assert::IsTrue(a.reset);
				Assert::IsFalse(a.absorb);
			}
		}

		TEST_METHOD(UnknownFullCauseResets)
		{
			const PulldownResyncAction a = PulldownResyncQ0::Decide(PulldownResyncQ0::In(PulldownResyncQ0::F, Cause::None));
			Assert::IsTrue(a.reset);
			Assert::IsFalse(a.absorb);
		}

		TEST_METHOD(FullWinsOverTiming)
		{
			// FULL + timing together: a seek with a rate step is absorbed (no rederive, no class reset)...
			const PulldownResyncAction seek = PulldownResyncQ0::Decide(PulldownResyncQ0::In(
				PulldownResyncQ0::F | PulldownResyncQ0::T, Cause::LiveQueueReset, 59.94, 24.0));
			Assert::IsTrue(seek.absorb);
			Assert::IsFalse(seek.reset || seek.rederiveHz || seek.resetOnClassChange);
			// ... and any other FULL with timing is one reset.
			const PulldownResyncAction other = PulldownResyncQ0::Decide(PulldownResyncQ0::In(
				PulldownResyncQ0::F | PulldownResyncQ0::T, Cause::InPlaceSourceChange, 59.94, 59.94));
			Assert::IsTrue(other.reset);
			Assert::IsFalse(other.absorb || other.rederiveHz);
		}

		TEST_METHOD(TimingOnlyRederivesHzAndKeepsTheLock)
		{
			// 59.94 -> 59.96: the same class.
			const PulldownResyncAction a = PulldownResyncQ0::Decide(
				PulldownResyncQ0::In(PulldownResyncQ0::T, Cause::None, 59.94, 59.96));
			Assert::IsTrue(a.rederiveHz);
			Assert::IsFalse(a.reset || a.absorb || a.resetOnClassChange);
		}

		TEST_METHOD(TimingClassChangeResets)
		{
			const PulldownResyncAction a = PulldownResyncQ0::Decide(
				PulldownResyncQ0::In(PulldownResyncQ0::T, Cause::None, 59.94, 29.97));
			Assert::IsTrue(a.rederiveHz);
			Assert::IsTrue(a.resetOnClassChange);
			Assert::IsTrue(a.reset);
		}

		TEST_METHOD(TimingWithUnknownRateOnlyRederives)
		{
			const PulldownResyncAction zeroOld = PulldownResyncQ0::Decide(
				PulldownResyncQ0::In(PulldownResyncQ0::T, Cause::None, 0.0, 29.97));
			Assert::IsTrue(zeroOld.rederiveHz);
			Assert::IsFalse(zeroOld.reset);
			const PulldownResyncAction noFn = PulldownResyncPolicy::Decide(
				PulldownResyncQ0::In(PulldownResyncQ0::T, Cause::None, 59.94, 29.97), PulldownResyncPolicy::ClassFn());
			Assert::IsTrue(noFn.rederiveHz);
			Assert::IsFalse(noFn.reset);
		}

		TEST_METHOD(ModeOnToOffResetsOnceViaTheLatch)
		{
			ModeEdgeLatch latch;
			Assert::IsFalse(latch.Consume(false), L"starts off: no edge");
			Assert::IsFalse(latch.Consume(true), L"off -> on is not the edge");
			Assert::IsFalse(latch.Consume(true));
			const bool edge = latch.Consume(false);
			Assert::IsTrue(edge, L"on -> off");
			Assert::IsFalse(latch.Consume(false), L"consumed once");
			const PulldownResyncAction a = PulldownResyncQ0::Decide(PulldownResyncQ0::In(0, Cause::None, 59.94, 59.94, edge));
			Assert::IsTrue(a.reset);
			Assert::IsFalse(a.absorb);
		}

		TEST_METHOD(ModeEdgeWinsOverAbsorb)
		{
			const PulldownResyncAction a = PulldownResyncQ0::Decide(PulldownResyncQ0::In(
				PulldownResyncQ0::F, Cause::LiveQueueReset, 59.94, 59.94, true));
			Assert::IsTrue(a.reset);
			Assert::IsFalse(a.absorb);
		}

		TEST_METHOD(SignalHzUsesTheWindowedRateOnlyWhenDriving)
		{
			Shadow shadow;
			Assert::AreEqual(23.976, PulldownSignalHz(shadow, 23.976), 1e-12, L"off: Bill's value");
			shadow.Configure(Shadow::Mode::Shadow, 32);
			Assert::AreEqual(50.0, PulldownSignalHz(shadow, 50.0), 1e-12, L"shadow does not drive");
			shadow.Configure(Shadow::Mode::On, 32);
			Assert::AreEqual(50.0, PulldownSignalHz(shadow, 50.0), 1e-12, L"driving but the window has not filled (0)");
		}
	};
}
