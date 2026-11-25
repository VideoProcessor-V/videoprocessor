// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// B3-Z Z2 scaffold: the generic post-present spare slot (core/PostPresentSpare.h). Pure CPU.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/PostPresentSpare.h>

#include <initializer_list>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpSkeleton;

namespace PostPresentSpareTest
{
	namespace
	{
		// A scripted work: fixed measured cost per row (or whole), with a log of runs.
		struct Scripted
		{
			FunctionSpareWork work;
			uint32_t rows = 0;          // 0 = unsplittable
			uint32_t left = 0;
			double msPerRow = 0.0;
			double msWhole = 0.0;
			int pending = 0;            // whole runs pending (unsplittable)
			std::vector<uint32_t> runs; // rows per run (0 = whole)
			std::vector<uint16_t>* order = nullptr;

			Scripted(uint16_t id, SpareClass c, uint32_t pos) : work(id, c, pos)
			{
				work.hasWork = [this]() { return rows > 0 ? left > 0 : pending > 0; };
				work.rowsLeft = [this]() { return rows > 0 ? left : 0u; };
				work.run = [this](uint32_t r) -> double
				{
					runs.push_back(r);
					if (order) order->push_back(work.Id());
					if (rows == 0) { --pending; return msWhole; }
					const uint32_t n = (r == 0 || r > left) ? left : r;
					left -= n;
					return msPerRow * n;
				};
			}
		};

		SpareSlotInput Slot(uint64_t key, double budget, double slice = 0.0, SpareTrigger t = SpareTrigger::PostPresentNoExtras)
		{
			SpareSlotInput in;
			in.enabled = true;
			in.trigger = t;
			in.frameKey = key;
			in.budgetMs = budget;
			in.sliceMs = slice;
			return in;
		}
	}

	TEST_CLASS(PostPresentSpareTests)
	{
	public:
		TEST_METHOD(OffNoBudgetAndOncePerFrame)
		{
			PostPresentSpare spare;
			Scripted a(1, SpareClass::Core, 1);
			a.pending = 5; a.msWhole = 1.0;
			spare.Register(&a.work);
			SpareSlotInput off = Slot(1, 10.0);
			off.enabled = false;
			Assert::IsTrue(spare.Run(off).outcome == SpareOutcome::Off);
			Assert::IsTrue(spare.Run(Slot(2, 0.0)).outcome == SpareOutcome::NoBudget);
			Assert::IsTrue(spare.Run(Slot(3, -1.0)).outcome == SpareOutcome::NoBudget);
			const SpareReport first = spare.Run(Slot(4, 10.0, 0.0, SpareTrigger::BeforeDequeueWait));
			Assert::IsTrue(first.outcome == SpareOutcome::Ran);
			Assert::AreEqual(1u, first.ran);
			// A second trigger point of the same rendered frame does nothing.
			Assert::IsTrue(spare.Run(Slot(4, 10.0, 0.0, SpareTrigger::AfterLastExtra)).outcome == SpareOutcome::AlreadyRan);
			Assert::AreEqual(1ull, spare.Fired(SpareTrigger::BeforeDequeueWait));
			Assert::AreEqual(0ull, spare.Fired(SpareTrigger::AfterLastExtra));
			spare.Reset();
			Assert::IsTrue(spare.Run(Slot(4, 10.0)).outcome == SpareOutcome::Ran, L"Reset: the key runs again");
			a.pending = 0;
			Assert::IsTrue(spare.Run(Slot(5, 10.0)).outcome == SpareOutcome::NothingToDo);
		}

		TEST_METHOD(FixedClassOrderThenPosition)
		{
			PostPresentSpare spare;
			std::vector<uint16_t> order;
			Scripted fill(9, SpareClass::Fill, 0), slice(7, SpareClass::Slice, 4), p3b(3, SpareClass::Core, 2),
				p3(2, SpareClass::Core, 1), unpack(1, SpareClass::Unpack, 0);
			for (Scripted* s : { &fill, &slice, &p3b, &p3, &unpack })
			{
				s->pending = 1; s->msWhole = 0.5; s->order = &order;
				s->work.Run(0);   // measure once outside the slot
				s->pending = 1;
				spare.Register(&s->work);
			}
			order.clear();
			const SpareReport r = spare.Run(Slot(1, 100.0, 50.0));
			Assert::AreEqual(5u, r.ran);
			const std::vector<uint16_t> expected = { 1, 2, 3, 7, 9 };
			Assert::IsTrue(order == expected, L"unpack, P3, P3b, slice, fill");
			Assert::AreEqual(2.5, r.usedMs, 1e-9);
		}

		TEST_METHOD(CoreSplitsOrDefersNeverDecays)
		{
			PostPresentSpare spare;
			Scripted core(2, SpareClass::Core, 1);
			core.rows = core.left = 100; core.msPerRow = 0.1;   // 10 ms whole
			core.work.Run(10);                                   // measured: 1 ms for 10 rows -> 10 ms whole
			core.left = 100;
			spare.Register(&core.work);
			const SpareReport r = spare.Run(Slot(1, 4.0));
			Assert::AreEqual(1u, r.split, L"a core work above the budget is row-split, not skipped");
			Assert::AreEqual(0u, r.coreDeferred);
			Assert::IsTrue(r.usedMs <= 4.0 + 1e-9, L"the split fits the budget");
			Assert::IsTrue(core.left < 100u && core.left > 0u);
			// It continues first in the next slots until done.
			for (uint64_t k = 2; k < 10 && core.left > 0; ++k) spare.Run(Slot(k, 4.0));
			Assert::AreEqual(0u, core.left);
			// Unsplittable core above the budget: deferred (counted), its prediction untouched.
			PostPresentSpare spare2;
			Scripted whole(3, SpareClass::Core, 1);
			whole.pending = 1; whole.msWhole = 8.0;
			whole.work.Run(0);
			whole.pending = 1;
			spare2.Register(&whole.work);
			const double before = whole.work.PredictMs();
			for (uint64_t k = 1; k <= 5; ++k)
				Assert::AreEqual(1u, spare2.Run(Slot(k, 4.0)).coreDeferred);
			Assert::AreEqual(before, whole.work.PredictMs(), 1e-12, L"no capNoFit-style decay");
			Assert::AreEqual(5ull, spare2.CoreDeferred());
			Assert::AreEqual(1u, spare2.Run(Slot(6, 9.0)).ran, L"runs as soon as the budget fits");
		}

		TEST_METHOD(SliceInsideItsShareFillInTheRest)
		{
			PostPresentSpare spare;
			Scripted core(2, SpareClass::Core, 1), slice(5, SpareClass::Slice, 4), fill(9, SpareClass::Fill, 0);
			core.pending = 1; core.msWhole = 3.0;
			slice.pending = 1; slice.msWhole = 2.0;
			fill.pending = 1; fill.msWhole = 2.0;
			for (Scripted* s : { &core, &slice, &fill }) { s->work.Run(0); s->pending = 1; spare.Register(&s->work); }
			SpareReport r = spare.Run(Slot(1, 6.0, 1.5));
			Assert::AreEqual(1u, r.sliceSkipped, L"slice above its share");
			Assert::AreEqual(0u, r.fillSkipped, L"fill runs in what is left (3 ms)");
			Assert::AreEqual(2u, r.ran);
			Assert::AreEqual(5.0, r.usedMs, 1e-9);
			Assert::AreEqual(1, slice.pending, L"the slice work waits for a slot with room in its share");
		}

		TEST_METHOD(UnmeasuredRunsOnlyFirst)
		{
			PostPresentSpare spare;
			Scripted a(1, SpareClass::Core, 1), b(2, SpareClass::Core, 2);
			a.pending = 1; a.msWhole = 1.0;
			b.pending = 1; b.msWhole = 1.0;
			spare.Register(&a.work);
			spare.Register(&b.work);
			SpareReport r = spare.Run(Slot(1, 10.0));
			Assert::AreEqual(1u, r.ran, L"only the first unmeasured work runs");
			Assert::AreEqual(1u, r.unmeasured);
			Assert::AreEqual(1u, r.coreDeferred);
			Assert::AreEqual(1.0, a.work.PredictMs(), 1e-12, L"measured from its run");
			r = spare.Run(Slot(2, 10.0));
			Assert::AreEqual(1u, r.ran);
			Assert::AreEqual(1.0, b.work.PredictMs(), 1e-12);
		}

		// Doc 101 row 11: the slot fires exactly once per SOURCE frame at 24/24, 60/60 and 24-in-60 (2:3, cadence repeats
		// keep their film frame's key), whichever trigger points fire for a rendered frame.
		TEST_METHOD(FiresExactlyOncePerSourceFrame)
		{
			struct Case { const wchar_t* name; int sourceFrames; bool pulldown; bool extras; };
			const Case cases[] = { { L"24/24", 48, false, false }, { L"60/60", 120, false, false },
				{ L"24-in-60", 48, true, false }, { L"24/24 extras", 48, false, true } };
			for (const Case& c : cases) {
				SpareHost host;
				std::vector<uint64_t> ranKeys;
				FunctionSpareWork work(1, SpareClass::Core, 1);
				work.hasWork = []() { return true; };
				work.run = [&](uint32_t) -> double { ranKeys.push_back(host.Key()); return 0.5; };
				host.Slot().Register(&work);
				host.SetBudget(8.0);
				uint64_t sequence = 0;
				int rendered = 0;
				std::vector<uint64_t> filmKeys;
				for (int f = 0; f < c.sourceFrames; ++f) {
					const int copies = c.pulldown ? (f % 2 == 0 ? 2 : 3) : 1;   // 2:3: 24 film frames -> 60 captured
					for (int k = 0; k < copies; ++k) {
						++sequence;   // every captured frame has its own source sequence
						if (k == 0) filmKeys.push_back(sequence);
						host.NotePresented(sequence, k > 0);
						host.Fire(c.extras ? SpareTrigger::AfterLastExtra : SpareTrigger::PostPresentNoExtras);
						host.Fire(SpareTrigger::BeforeDequeueWait);
						++rendered;
					}
				}
				Assert::AreEqual(size_t(c.sourceFrames), ranKeys.size(), c.name);
				Assert::IsTrue(ranKeys == filmKeys, c.name);
				Assert::AreEqual(static_cast<unsigned long long>(c.sourceFrames), host.Slot().Count(SpareOutcome::Ran), c.name);
				Assert::AreEqual(static_cast<unsigned long long>(2 * rendered - c.sourceFrames), host.Slot().Count(SpareOutcome::AlreadyRan), c.name);
				Assert::AreEqual(static_cast<unsigned long long>(c.sourceFrames),
					host.Slot().Fired(c.extras ? SpareTrigger::AfterLastExtra : SpareTrigger::PostPresentNoExtras), c.name);
				Assert::AreEqual(0ull, host.Slot().Fired(SpareTrigger::BeforeDequeueWait), c.name);
				if (c.pulldown) Assert::AreEqual(c.sourceFrames * 5 / 2, rendered, L"2:3 cadence: 60 rendered per 24 film frames");
			}
		}

		// Audit C5: the key is the SOURCE frame's. Two extras of one source frame (each re-presents the same source
		// sequence) share the key: the slot runs once, after the last extra; the second extra counts AlreadyRan.
		TEST_METHOD(TwoExtrasOfOneSourceFrameShareTheKey)
		{
			SpareHost host;
			int runs = 0;
			FunctionSpareWork work(1, SpareClass::Core, 1);
			work.hasWork = []() { return true; };
			work.run = [&](uint32_t) -> double { ++runs; return 0.5; };
			host.Slot().Register(&work);
			host.SetBudget(8.0);
			host.NotePresented(41, false);   // extra 1 of source frame 41
			const uint64_t first = host.Key();
			Assert::IsTrue(host.Fire(SpareTrigger::AfterLastExtra).outcome == SpareOutcome::Ran);
			host.NotePresented(41, false);   // extra 2 of the same source frame
			Assert::AreEqual(first, host.Key(), L"same source frame: same key");
			Assert::IsTrue(host.Fire(SpareTrigger::AfterLastExtra).outcome == SpareOutcome::AlreadyRan);
			Assert::AreEqual(1, runs);
			host.NotePresented(42, false);
			Assert::AreNotEqual(first, host.Key());
			Assert::IsTrue(host.Fire(SpareTrigger::AfterLastExtra).outcome == SpareOutcome::Ran);
			Assert::AreEqual(2, runs);
		}

		// Audit S4: an unmeasured core work behind a measured one that always has work is not starved: it is deferred
		// (counted) once and goes first in the next slot, where it is measured; after that the class order holds.
		TEST_METHOD(UnmeasuredCoreWorkIsNotStarvedByAnEarlierWork)
		{
			PostPresentSpare slot;
			std::vector<int> order;
			FunctionSpareWork first(1, SpareClass::Core, 1);
			first.hasWork = []() { return true; };
			first.run = [&](uint32_t) -> double { order.push_back(1); return 1.0; };
			FunctionSpareWork late(2, SpareClass::Core, 2);
			late.hasWork = []() { return true; };
			late.run = [&](uint32_t) -> double { order.push_back(2); return 1.0; };
			slot.Register(&first);
			slot.Register(&late);
			SpareSlotInput in;
			in.enabled = true;
			in.budgetMs = 8.0;
			in.frameKey = 1;
			SpareReport r = slot.Run(in);   // first is unmeasured and first: runs; late is unmeasured behind it
			Assert::AreEqual(1u, r.ran);
			Assert::AreEqual(1u, r.coreDeferred);
			in.frameKey = 2;
			r = slot.Run(in);              // late goes first (unmeasured), then first (measured, fits)
			Assert::AreEqual(2u, r.ran);
			Assert::AreEqual(1ull, slot.Promoted());
			Assert::IsTrue(late.Cost().Samples() == 1u, L"measured once");
			in.frameKey = 3;
			r = slot.Run(in);              // both measured: the fixed order again
			Assert::AreEqual(2u, r.ran);
			Assert::AreEqual(1ull, slot.Promoted());
			const std::vector<int> want = { 1, 2, 1, 1, 2 };
			Assert::IsTrue(order == want);
		}

		TEST_METHOD(HostIsVisibleWithoutBudgetWorkOrKey)
		{
			SpareHost host;
			Assert::IsTrue(host.Fire(SpareTrigger::BeforeDequeueWait).outcome == SpareOutcome::Off, L"no work registered: off");
			int runs = 0;
			FunctionSpareWork work(1, SpareClass::Core, 1);
			work.hasWork = []() { return true; };
			work.run = [&](uint32_t) -> double { ++runs; return 0.5; };
			host.Slot().Register(&work);
			Assert::IsTrue(host.Fire(SpareTrigger::BeforeDequeueWait).outcome == SpareOutcome::Off, L"no presented frame: no key");
			Assert::AreEqual(1ull, host.NoKey());
			host.NotePresented(7, false);
			Assert::IsTrue(host.Fire(SpareTrigger::PostPresentNoExtras).outcome == SpareOutcome::NoBudget, L"budget 0 until C5b: counted");
			Assert::IsTrue(host.Fire(SpareTrigger::BeforeDequeueWait).outcome == SpareOutcome::AlreadyRan, L"NoBudget consumes the key");
			Assert::AreEqual(0, runs);
			host.SetBudget(4.0);
			host.NotePresented(8, true);   // a repeat of 7: same key
			Assert::IsTrue(host.Fire(SpareTrigger::PostPresentNoExtras).outcome == SpareOutcome::AlreadyRan);
			host.NotePresented(9, false);
			Assert::IsTrue(host.Fire(SpareTrigger::PostPresentNoExtras).outcome == SpareOutcome::Ran);
			Assert::AreEqual(1, runs);
			host.Reset();
			Assert::AreEqual(0ull, host.Key());
			host.NotePresented(12, true);   // first frame after a reset: its own key even when flagged a repeat
			Assert::AreEqual(12ull, host.Key());
		}

		TEST_METHOD(RowsThatFitFollowPlanChunks)
		{
			Assert::AreEqual(100u, SpareRowsThatFit(100, 2.0, 4.0));
			Assert::AreEqual(34u, SpareRowsThatFit(100, 10.0, 4.0));   // 3 chunks
			Assert::AreEqual(1u, SpareRowsThatFit(5, 1000.0, 0.001));
			Assert::AreEqual(0u, SpareRowsThatFit(0, 1.0, 1.0));
		}

		// K#7: an idle Unpack work returning 0.0 ms leaves usedMs == 0.0, so an unmeasured Core work behind it
		// MUST run on the very first slot (r.ran == 2), while a non-idle Unpack (> 0 ms) still defers Core.
		TEST_METHOD(IdleUnpackAllowsUnmeasuredCoreToRunOnFirstSlot)
		{
			// 1. Protected path: idle 0-ms Unpack followed by unmeasured 1.5-ms Core.
			{
				PostPresentSpare spare;
				std::vector<uint16_t> order;
				Scripted unpack(1, SpareClass::Unpack, 0);
				unpack.pending = 1;
				unpack.msWhole = 0.0;
				unpack.order = &order;
				Scripted core(2, SpareClass::Core, 1);
				core.pending = 1;
				core.msWhole = 1.5;
				core.order = &order;
				spare.Register(&unpack.work);
				spare.Register(&core.work);

				const SpareReport r = spare.Run(Slot(1, 10.0));
				Assert::IsTrue(r.outcome == SpareOutcome::Ran);
				Assert::AreEqual(2u, r.ran, L"K#7: idle 0-ms Unpack + unmeasured Core both run on slot 1");
				Assert::AreEqual(2u, r.unmeasured, L"both unmeasured works ran while usedMs remained 0.0");
				Assert::AreEqual(0u, r.coreDeferred, L"Core must not be deferred behind an idle 0-ms Unpack");
				Assert::AreEqual(size_t(1), core.runs.size(), L"Core executed on slot 1");
				Assert::AreEqual(1.5, r.usedMs, 1e-9, L"slot usedMs equals Core's 1.5 ms");
				const std::vector<uint16_t> want = { 1, 2 };
				Assert::IsTrue(order == want, L"Unpack ran before Core");
			}

			// 2. In-test negative control: non-idle Unpack (0.5 ms > 0.0) defers unmeasured Core on slot 1,
			//    then Core is promoted and runs on slot 2.
			{
				PostPresentSpare negSpare;
				Scripted busyUnpack(1, SpareClass::Unpack, 0);
				busyUnpack.pending = 2;
				busyUnpack.msWhole = 0.5;
				Scripted negCore(2, SpareClass::Core, 1);
				negCore.pending = 1;
				negCore.msWhole = 1.5;
				negSpare.Register(&busyUnpack.work);
				negSpare.Register(&negCore.work);

				const SpareReport r1 = negSpare.Run(Slot(1, 10.0));
				Assert::AreEqual(1u, r1.ran, L"negative control: non-idle 0.5-ms Unpack blocks unmeasured Core on slot 1");
				Assert::AreEqual(1u, r1.coreDeferred, L"negative control: unmeasured Core deferred behind non-idle Unpack");
				Assert::IsTrue(negCore.runs.empty(), L"negative control: Core did not run on slot 1");
				Assert::AreEqual(0.5, r1.usedMs, 1e-9);

				const SpareReport r2 = negSpare.Run(Slot(2, 10.0));
				Assert::AreEqual(1ull, negSpare.Promoted(), L"negative control: deferred Core promoted first on slot 2");
				Assert::AreEqual(2u, r2.ran, L"Core runs first (promoted) followed by measured Unpack");
				Assert::AreEqual(size_t(1), negCore.runs.size());
			}
		}

		// K#8: PostPresentSpare maintains m_works sorted on Register/Unregister so execution order and every report
		// field are bit-for-bit identical regardless of registration order or mid-run Unregister/re-Register.
		TEST_METHOD(RegistrationOrderDoesNotAffectSpareExecutionOrder)
		{
			struct RigSet
			{
				PostPresentSpare spare;
				std::vector<uint16_t> order;
				Scripted unpack{ 10, SpareClass::Unpack, 0 };
				Scripted coreSplit{ 20, SpareClass::Core, 1 };
				Scripted coreTieFirst{ 22, SpareClass::Core, 2 };
				Scripted coreUnmeasured{ 25, SpareClass::Core, 2 };
				Scripted sliceWork{ 30, SpareClass::Slice, 1 };
				Scripted fillWork{ 40, SpareClass::Fill, 0 };

				RigSet()
				{
					unpack.pending = 1; unpack.msWhole = 0.4; unpack.work.Run(0); unpack.pending = 5; unpack.order = &order;
					coreSplit.rows = coreSplit.left = 20; coreSplit.msPerRow = 0.1; coreSplit.work.Run(5); coreSplit.left = 20; coreSplit.order = &order;
					coreTieFirst.pending = 1; coreTieFirst.msWhole = 0.5; coreTieFirst.work.Run(0); coreTieFirst.pending = 5; coreTieFirst.order = &order;
					coreUnmeasured.pending = 5; coreUnmeasured.msWhole = 0.8; coreUnmeasured.order = &order;   // deliberately unmeasured
					sliceWork.pending = 1; sliceWork.msWhole = 1.2; sliceWork.work.Run(0); sliceWork.pending = 5; sliceWork.order = &order;
					fillWork.pending = 1; fillWork.msWhole = 0.7; fillWork.work.Run(0); fillWork.pending = 5; fillWork.order = &order;
				}
			};

			RigSet a, b;
			// Instance A: canonical sorted registration order.
			a.spare.Register(&a.unpack.work);
			a.spare.Register(&a.coreSplit.work);
			a.spare.Register(&a.coreTieFirst.work);
			a.spare.Register(&a.coreUnmeasured.work);
			a.spare.Register(&a.sliceWork.work);
			a.spare.Register(&a.fillWork.work);

			// Instance B: scrambled / reverse registration order.
			b.spare.Register(&b.fillWork.work);
			b.spare.Register(&b.coreUnmeasured.work);
			b.spare.Register(&b.sliceWork.work);
			b.spare.Register(&b.unpack.work);
			b.spare.Register(&b.coreTieFirst.work);
			b.spare.Register(&b.coreSplit.work);

			uint32_t totalSplit = 0;
			uint32_t totalSliceSkipped = 0;
			auto runAndCompare = [&](uint64_t key, double budget, double slice) {
				const SpareReport ra = a.spare.Run(Slot(key, budget, slice));
				const SpareReport rb = b.spare.Run(Slot(key, budget, slice));
				Assert::IsTrue(ra.outcome == rb.outcome);
				Assert::AreEqual(ra.ran, rb.ran);
				Assert::AreEqual(ra.usedMs, rb.usedMs, 1e-12);
				Assert::AreEqual(ra.split, rb.split);
				Assert::AreEqual(ra.unmeasured, rb.unmeasured);
				Assert::AreEqual(ra.coreDeferred, rb.coreDeferred);
				Assert::AreEqual(ra.sliceSkipped, rb.sliceSkipped);
				Assert::AreEqual(ra.fillSkipped, rb.fillSkipped);
				Assert::IsTrue(a.order == b.order, L"execution order identical regardless of registration order");
				totalSplit += ra.split;
				totalSliceSkipped += ra.sliceSkipped;
			};

			// Slot 1: Unpack (0.4) + CoreSplit (2.0 > 1.6 remaining -> split) + CoreTieFirst (0.5) + CoreUnmeasured deferred -> promoted next slot.
			runAndCompare(1, 2.0, 1.0);
			// Slot 2: promoted CoreUnmeasured runs first, then remaining works in canonical order.
			runAndCompare(2, 6.0, 2.0);

			// Mid-run Unregister + re-Register in different orders on A vs B.
			a.spare.Unregister(&a.coreTieFirst.work);
			a.spare.Unregister(&a.sliceWork.work);
			a.spare.Register(&a.coreTieFirst.work);
			a.spare.Register(&a.sliceWork.work);

			b.spare.Unregister(&b.sliceWork.work);
			b.spare.Unregister(&b.coreTieFirst.work);
			b.spare.Register(&b.sliceWork.work);
			b.spare.Register(&b.coreTieFirst.work);

			runAndCompare(3, 6.0, 0.8);   // slice (1.2 > 0.8) skipped; fill runs
			runAndCompare(4, 10.0, 5.0);

			Assert::AreEqual(a.spare.Promoted(), b.spare.Promoted());
			Assert::AreEqual(a.spare.CoreDeferred(), b.spare.CoreDeferred());
			Assert::AreEqual(a.spare.UsedMsTotal(), b.spare.UsedMsTotal(), 1e-12);
			Assert::IsTrue(totalSplit > 0u, L"row-splitting branch exercised");
			Assert::IsTrue(a.spare.Promoted() > 0ull, L"unmeasured promotion branch exercised");
			Assert::IsTrue(totalSliceSkipped > 0u, L"slice-skip branch exercised");
			Assert::IsTrue(a.order.size() >= 10u, L"multiple works executed across all 4 slots");
		}
	};
}
