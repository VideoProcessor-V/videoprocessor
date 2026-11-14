// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S0: IC-2 scheduler contract (core/SchedulerContract.h) and the slot budget feed (modules/control/skeleton/SlotBudgetFeed.h). Pure CPU.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/SchedulerContract.h>
#include <VP-V/modules/control/skeleton/SlotBudgetFeed.h>
#include <VP-V/modules/control/skeleton/gpu/GpuProductCost.h>

#include <cmath>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpSkeleton;
using Vpv::Skeleton::SlotBudgetFeed;
using Vpv::Skeleton::SlotPresentFacts;

namespace SchedulerContractTest
{
	namespace
	{
		// The frozen body of OnDemand::RenderBudget as it stood before S0 moved it into core (the reference for T1).
		class OldRenderBudget
		{
		public:
			void Note(double renderMs)
			{
				if (!(renderMs >= 0.0))
					return;
				ewmaMs_ = measured_ ? ewmaMs_ + (renderMs - ewmaMs_) / 8.0 : renderMs;
				measured_ = true;
			}
			bool Measured() const { return measured_; }
			double RenderMs() const { return ewmaMs_; }
			double BudgetMs(double sourceHz) const
			{
				if (!(sourceHz > 0.0))
					return 0.0;
				const double period = 1000.0 / sourceHz;
				if (!measured_)
					return period * 0.5;
				const double b = period - ewmaMs_ - period * 0.1;
				return b > 0.0 ? b : 0.0;
			}
			void Reset() { ewmaMs_ = 0.0; measured_ = false; }

		private:
			double ewmaMs_ = 0.0;
			bool measured_ = false;
		};

		// One Core work that costs a fixed time and always has something to do.
		struct CoreWork
		{
			FunctionSpareWork work;
			int runs = 0;
			double ms = 2.0;
			CoreWork() : work(1, SpareClass::Core, 0)
			{
				work.hasWork = []() { return true; };
				work.run = [this](uint32_t) -> double { ++runs; return ms; };
			}
		};

		SlotPresentFacts Present(uint64_t seq, bool repeat, double renderMs, double rateHz)
		{
			SlotPresentFacts f;
			f.sourceSequence = seq;
			f.trigger = SpareTrigger::PostPresentNoExtras;
			f.rendered = true;
			f.cadenceRepeat = repeat;
			f.renderMs = renderMs;
			f.captureRateHz = rateHz;
			return f;
		}
	}

	TEST_CLASS(SchedulerContractTests)
	{
	public:
		// T1: the moved RenderBudget gives bit-identical numbers to the frozen old body, over a sample trace and every edge.
		TEST_METHOD(RenderBudgetIsBitIdenticalToTheFrozenOldBody)
		{
			RenderBudget now;
			OldRenderBudget old;
			Assert::IsTrue(now.BudgetMs(24.0) == old.BudgetMs(24.0) && now.BudgetMs(24.0) == 1000.0 / 24.0 * 0.5, L"half a period until measured");
			Assert::IsTrue(now.BudgetMs(0.0) == 0.0 && old.BudgetMs(0.0) == 0.0, L"unknown rate = 0");
			const double trace[] = { 9.5, 10.25, 7.0, -1.0, 12.5, 40.0, 41.7, 3.0, 11.0, 10.0, 0.0, 9.9 };
			const double rates[] = { 0.0, 23.976, 24.0, 30.0, 50.0, 59.94, 60.0 };
			for (double s : trace)
			{
				now.Note(s);
				old.Note(s);
				Assert::IsTrue(now.Measured() == old.Measured(), L"measured");
				Assert::IsTrue(now.RenderMs() == old.RenderMs(), L"ewma bit-identical");
				for (double hz : rates)
					Assert::IsTrue(now.BudgetMs(hz) == old.BudgetMs(hz), L"budget bit-identical");
			}
			Assert::IsTrue(now.RenderMs() > 0.0, L"the trace measured something");
			now.Note(std::nan(""));
			Assert::IsTrue(now.RenderMs() == old.RenderMs(), L"NaN ignored");
			now.Reset();
			old.Reset();
			Assert::IsTrue(!now.Measured() && now.BudgetMs(24.0) == old.BudgetMs(24.0), L"reset");
		}

		// T1b: the class helper maps Derived onto Core and leaves the others.
		TEST_METHOD(DerivedRunsWithCoreInTheSlot)
		{
			Assert::IsTrue(SpareClassOf(JobClass::Unpack) == SpareClass::Unpack);
			Assert::IsTrue(SpareClassOf(JobClass::Core) == SpareClass::Core);
			Assert::IsTrue(SpareClassOf(JobClass::Derived) == SpareClass::Core);
			Assert::IsTrue(SpareClassOf(JobClass::Slice) == SpareClass::Slice);
			Assert::IsTrue(SpareClassOf(JobClass::Fill) == SpareClass::Fill);
		}

		// T2: 24-in-60 cadence on a fake host: the budget is fed once per SOURCE frame, the slot runs once per source frame, and after the first
		// measured frame it never reports NoBudget again.
		TEST_METHOD(TheBudgetIsFedOncePerSourceFrame)
		{
			SpareHost host;
			CoreWork w;
			host.Slot().Register(&w.work);
			SlotBudgetFeed feed;
			Assert::IsTrue(feed.StatusLine().find("no present seen yet") != std::string::npos, L"visible before any present");

			// First present: no render sample yet: budget 0, NoBudget, and the status names the reason (rule 5).
			SpareReport r = feed.OnAfterPresent(host, Present(1, false, 0.0, 24.0), nullptr);
			Assert::IsTrue(r.outcome == SpareOutcome::NoBudget, L"no render sample = NoBudget");
			Assert::IsTrue(feed.StatusLine().find("render (module clock) not measured") != std::string::npos, L"status says why");
			Assert::AreEqual(0, w.runs);

			// 60 Hz presents of a 24 fps source: the sequence advances on 2 of every 5 presents.
			const uint64_t seqs[] = { 2, 2, 3, 3, 3, 4, 4, 5, 5, 5 };
			const bool repeats[] = { false, true, false, true, true, false, true, false, true, true };
			unsigned noBudget = 0, ran = 0;
			for (int i = 0; i < 10; ++i)
			{
				r = feed.OnAfterPresent(host, Present(seqs[i], repeats[i], 10.0, 24.0), nullptr);
				noBudget += r.outcome == SpareOutcome::NoBudget ? 1u : 0u;
				ran += r.outcome == SpareOutcome::Ran ? 1u : 0u;
			}
			Assert::AreEqual(0u, noBudget, L"no NoBudget after the first measured frame");
			Assert::AreEqual(4u, ran, L"one run per source frame (2,3,4,5)");
			Assert::AreEqual(4, w.runs, L"four source frames ran");
			Assert::IsTrue(feed.BudgetFeeds() == 5u, L"fed once per source frame (1..5), not per present");
			const SlotBudget b = feed.Budget();
			Assert::IsTrue(b.measured, L"measured");
			const double want = 1000.0 / 24.0 - b.renderMs - 1000.0 / 24.0 * 0.1;
			Assert::IsTrue(std::fabs(b.budgetMs - want) < 1e-9, L"the ONE render budget law");
			Assert::IsTrue(feed.StatusLine().find("render (module clock)") != std::string::npos, L"status labels the source of the render ms");
		}

		// T2b: an unknown source rate = budget 0, visible; nominal only while the measured one is missing; the Entrance-cached rate is the measured one.
		TEST_METHOD(RateSourcesAndTheUnknownRate)
		{
			SpareHost host;
			CoreWork w;
			host.Slot().Register(&w.work);
			SlotBudgetFeed feed;
			SpareReport r = feed.OnAfterPresent(host, Present(1, false, 10.0, 0.0), nullptr);
			Assert::IsTrue(r.outcome == SpareOutcome::NoBudget, L"no rate at all = no budget");
			Assert::IsTrue(feed.StatusLine().find("source rate unknown") != std::string::npos, L"visible");
			SlotPresentFacts f = Present(2, false, 10.0, 0.0);
			f.nominalRateHz = 25.0;
			feed.OnAfterPresent(host, f, nullptr);
			Assert::IsTrue(std::fabs(feed.Budget().periodMs - 40.0) < 1e-9, L"nominal while unmeasured");
			feed.NoteCaptureRate(24.0);
			f.sourceSequence = 3;
			feed.OnAfterPresent(host, f, nullptr);
			Assert::IsTrue(std::fabs(feed.Budget().periodMs - 1000.0 / 24.0) < 1e-9, L"the Entrance-cached measured rate wins over nominal");
			f.captureRateHz = 30.0;
			f.sourceSequence = 4;
			feed.OnAfterPresent(host, f, nullptr);
			Assert::IsTrue(std::fabs(feed.Budget().periodMs - 1000.0 / 30.0) < 1e-9, L"an explicit measured rate wins over the cache");
		}

		// T2c: a Plain call, a cadence repeat or a frame that did not render never notes a render sample (R1).
		TEST_METHOD(OnlyANewRenderedFrameNotesARenderSample)
		{
			SpareHost host;
			SlotBudgetFeed feed;
			SlotPresentFacts f = Present(1, true, 10.0, 24.0);
			feed.OnAfterPresent(host, f, nullptr);
			Assert::IsFalse(feed.RenderBudgetUnit().Measured(), L"cadence repeat");
			f = Present(2, false, 10.0, 24.0);
			f.plain = true;
			feed.OnAfterPresent(host, f, nullptr);
			Assert::IsFalse(feed.RenderBudgetUnit().Measured(), L"plain");
			f = Present(3, false, 10.0, 24.0);
			f.rendered = false;
			feed.OnAfterPresent(host, f, nullptr);
			Assert::IsFalse(feed.RenderBudgetUnit().Measured(), L"not rendered");
			feed.OnAfterPresent(host, Present(4, false, 10.0, 24.0), nullptr);
			Assert::IsTrue(feed.RenderBudgetUnit().Measured(), L"a new rendered frame");
		}

		// T2d: the Original pathway never feeds a budget and never writes the Presenter (R9).
		TEST_METHOD(TheOriginalPathwayIsNotBudgeted)
		{
			SpareHost host;
			CoreWork w;
			host.Slot().Register(&w.work);
			SlotBudgetFeed feed;
			RenderAhead::Presenter p;
			p.originalPathway = true;
			for (int i = 1; i <= 5; ++i)
			{
				const SpareReport r = feed.OnAfterPresent(host, Present(uint64_t(i), false, 10.0, 24.0), &p);
				Assert::IsTrue(r.outcome == SpareOutcome::NoBudget, L"never budgeted");
			}
			Assert::AreEqual(0, w.runs);
			Assert::IsTrue(feed.BudgetFeeds() == 0u, L"SetBudget never called");
			Assert::AreEqual(0.0, p.spareSlotMs.load(), L"the Presenter is not written");
			Assert::IsTrue(feed.StatusLine().find("Original pathway") != std::string::npos, L"visible");
		}

		// T3: the slot's use per source frame (EWMA) plus the declared TD slice goes to Presenter::spareSlotMs, and the gate sees it.
		TEST_METHOD(SpareSlotMsReachesTheGate)
		{
			SpareHost host;
			CoreWork w;
			w.ms = 2.0;
			host.Slot().Register(&w.work);
			SlotBudgetFeed feed;
			feed.SetDeclaredSliceMs(0.5);
			RenderAhead::Presenter p;
			for (int i = 1; i <= 4; ++i)
				feed.OnAfterPresent(host, Present(uint64_t(i), false, 10.0, 24.0), &p);
			// every frame had a measured render (the sample is noted before the budget) and used 2.0 ms: EWMA 2.0 + the 0.5 slice.
			Assert::IsTrue(std::fabs(p.spareSlotMs.load() - 2.5) < 1e-9, L"used EWMA + declared slice");
			Assert::IsTrue(std::fabs(feed.SharedMs() - 2.5) < 1e-9);
			RenderAhead::PresenterGateFrame gf;
			gf.sourcePeriod = 1.0 / 24.0;
			gf.displayPeriod = 1.0 / 60.0;
			Assert::IsTrue(std::fabs(p.UpdateGate(1.0, gf).input.costs.spareSlotMs - 2.5) < 1e-9, L"the Presenter's input reaches the gate");
			RenderAhead::GateInput in;
			in.sourcePeriod = 1.0 / 24.0;
			in.displayPeriod = 1.0 / 60.0;
			in.costs.newFrameMs = 10.0;
			const double base = RenderAhead::FrameCostMs(in, 0);
			in.costs.spareSlotMs = p.spareSlotMs.load();
			Assert::IsTrue(std::fabs(RenderAhead::FrameCostMs(in, 0) - (base + 2.5)) < 1e-9, L"FrameCostMs includes it");
		}

		// T4 (FIX-S0-COST): a GPU product's cost is the SUM of all passes of a frame, not the last pass: five passes of 1.0/0.5/0.5/0.5/0.5 ms.
		TEST_METHOD(TheMeasuredJobCostIsThePassSum)
		{
			Vpv::Skeleton::GpuProductCost cost;
			uint64_t lastPassNs = 0;
			for (int f = 0; f < 6; ++f)
			{
				cost.NoteFrameDone();
				cost.NoteResult(1000000u);
				for (int i = 0; i < 4; ++i)
				{
					lastPassNs = 500000u;
					cost.NoteResult(lastPassNs);
				}
				cost.NotePoll();
			}
			Assert::IsTrue(std::fabs(cost.MeanFrameMs() - 3.0) < 1e-9, L"the per-frame mean is the pass sum");
			Assert::IsTrue(cost.MeanFrameMs() > 5.9 * double(lastPassNs) / 1.0e6, L"about six times the last single pass");
		}

		// T5: no work runs without a presented source frame; nothing blocks (a pure call, no thread, no clock).
		TEST_METHOD(NothingRunsBeforeAFrameWasPresented)
		{
			SpareHost host;
			CoreWork w;
			host.Slot().Register(&w.work);
			SlotBudgetFeed feed;
			feed.OnAfterPresent(host, Present(0, false, 10.0, 24.0), nullptr);
			Assert::AreEqual(0, w.runs, L"no key, no run");
			Assert::IsTrue(host.NoKey() == 1ull, L"counted");
		}
	};
}
