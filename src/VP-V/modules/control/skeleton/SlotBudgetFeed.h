// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S0 (lead8/brief-S0-scheduler.md s3; refs/122 N1, N2; owner 2026-11-12 15:13, refs/owner-decisions-2026-11-07.md:612): the budget feed of the ONE
// post-present spare slot. SpareHost::SetBudget was never called, so every slot trigger reported NoBudget. This feed turns the facts the host side
// hands over (option B, lead 8: the render ms from the module clock, the measured source rate cached from the Entrance fan-out, the nominal rate from
// the lane's period - zero Bill lines; the feed itself is source-agnostic) into the slot's budget through the ONE render budget (core/SchedulerContract.h RenderBudget: period - render EWMA - period/10), and writes what the slot used back
// to the Presenter's spareSlotMs (IC-2 R2), so render-ahead and the slot never promise the same milliseconds twice.
//
// Rules (IC-2 R1, R2, R9): the budget is fed once per SOURCE frame, from the MEASURED source rate (the nominal rate only while unmeasured); a
// cadence repeat and a Plain call (nothing presented) never note a render sample; no render sample yet or an unknown rate = budget 0, which the slot
// reports as NoBudget and the status line names (rule 5: never silent). In the Original pathway nothing is fed and nothing is written. Never blocks,
// starts no thread. Render thread only. Pure C++14: the host types are copied into SlotPresentFacts by the caller.

#include <VP-V/core/SchedulerContract.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Vpv
{
	namespace Skeleton
	{
		// The few scalars of one R12 AfterPresent call (RE::FrameContext / RE::PresentContext).
		struct SlotPresentFacts
		{
			uint64_t sourceSequence = 0;                    // FrameContext::seq
			VpSkeleton::SpareTrigger trigger = VpSkeleton::SpareTrigger::BeforeDequeueWait;   // the AfterPresentKind mapped as OnAfterPresent does
			bool plain = false;                             // kind == Plain: nothing presented this frame
			bool rendered = false;                          // PresentContext::rendered
			bool cadenceRepeat = false;                     // PresentContext::cadenceRepeat
			double renderMs = 0.0;                          // the frame render ms (0 = not measured); option B: module clock, biased high
			double captureRateHz = 0.0;                     // measured source rate (0 = use the Entrance-cached value, then nominal)
			double nominalRateHz = 0.0;                     // the lane configured rate
		};

		class SlotBudgetFeed
		{
		public:
			// One call per frame, in place of NotePresented + Fire. Returns the slot's report.
			VpSkeleton::SpareReport OnAfterPresent(VpSkeleton::SpareHost& host, const SlotPresentFacts& f, RenderAhead::Presenter* presenter)
			{
				const bool original = presenter != nullptr && presenter->originalPathway;
				++calls_;
				hostFedPresenter_ = presenter != nullptr && !original;
				if (original)
				{
					// Bill's pathway: the slot is never budgeted and the Presenter is not written (nothing registered either).
					originalSeen_ = true;
					host.NotePresented(f.sourceSequence, f.cadenceRepeat);
					return host.Fire(f.trigger);
				}
				originalSeen_ = false;
				// R1: a render sample only from a frame that rendered a new source frame.
				if (f.rendered && !f.plain && !f.cadenceRepeat && f.renderMs > 0.0)
					budget_.Note(f.renderMs);
				const double hz = f.captureRateHz > 0.0 ? f.captureRateHz : cachedRateHz_ > 0.0 ? cachedRateHz_ : f.nominalRateHz;   // measured first, nominal only when unmeasured (rule 6)
				rateHz_ = hz;
				periodMs_ = hz > 0.0 ? 1000.0 / hz : 0.0;
				const bool newSource = !f.cadenceRepeat && (f.sourceSequence != lastSource_ || !sourceSeen_);
				host.NotePresented(f.sourceSequence, f.cadenceRepeat);
				if (newSource || !budgetFed_)
				{
					// Until a render sample exists the budget is 0 (the slot reports NoBudget; the status says why) - never half a period of a guess.
					budgetMs_ = budget_.Measured() ? budget_.BudgetMs(hz) : 0.0;
					// Until a render sample exists the host keeps the budget it has (0 when nothing set one: the slot reports NoBudget as
					// before); the feed writes only a measured budget, so it never zeroes one another owner of the host set.
					// Condition (gate ruling 4): if RenderBudget::Reset is ever called on this feed it must also zero the host budget (SetBudget(0, 0)).
					if (budget_.Measured())
						host.SetBudget(budgetMs_, declaredSliceMs_);
					++budgetFeeds_;
					budgetFed_ = true;
					lastSource_ = f.sourceSequence;
					sourceSeen_ = true;
				}
				const VpSkeleton::SpareReport r = host.Fire(f.trigger);
				last_ = r;
				// R2: the slot's use per source frame (EWMA 1/8, as the render budget) + the declared TD slice, shared with the gate. Only a decision of
				// the slot for a frame counts (Ran / NothingToDo); NoBudget, Off and AlreadyRan carry no use.
				if (r.outcome == VpSkeleton::SpareOutcome::Ran || r.outcome == VpSkeleton::SpareOutcome::NothingToDo)
				{
					usedEwmaMs_ = usedSeen_ ? usedEwmaMs_ + (r.usedMs - usedEwmaMs_) / 8.0 : r.usedMs;
					usedSeen_ = true;
				}
				sharedMs_ = usedEwmaMs_ + declaredSliceMs_;
				if (presenter != nullptr)
					presenter->spareSlotMs.store(sharedMs_, std::memory_order_release);
				return r;
			}

			// The measured source rate of the Entrance fan-out (FrameContext::captureRateHz; 0 = unknown keeps the last value off: a 0 clears it).
			void NoteCaptureRate(double hz) { cachedRateHz_ = hz > 0.0 ? hz : 0.0; }
			// The slice share the slot declares for the TD (R5: a Configure-time value; 0 until the admission table exists).
			void SetDeclaredSliceMs(double ms) { declaredSliceMs_ = ms > 0.0 ? ms : 0.0; }
			// A new source / resync: the slot's own use starts over; the render EWMA stays (the renderer did not change).
			void ResetSource() { sourceSeen_ = false; budgetFed_ = false; lastSource_ = 0; usedSeen_ = false; usedEwmaMs_ = 0.0; sharedMs_ = 0.0; }

			// The per-source-frame numbers, as IC-2 SlotBudget.
			VpSkeleton::SlotBudget Budget() const
			{
				VpSkeleton::SlotBudget b = {};
				b.periodMs = periodMs_;
				b.renderMs = budget_.RenderMs();
				b.budgetMs = budgetMs_;
				b.sliceMs = declaredSliceMs_;
				b.usedMs = usedEwmaMs_;
				b.measured = budget_.Measured() && periodMs_ > 0.0;
				return b;
			}
			const VpSkeleton::RenderBudget& RenderBudgetUnit() const { return budget_; }
			uint64_t BudgetFeeds() const { return budgetFeeds_; }
			uint64_t Calls() const { return calls_; }
			double SharedMs() const { return sharedMs_; }

			// Ctrl+Alt+D / Ctrl+I status text (rule 5: the missing feed says so).
			std::string StatusLine() const
			{
				if (calls_ == 0)
					return "spare slot: no budget (no present seen yet)";
				if (originalSeen_)
					return "spare slot: off (Original pathway: not budgeted)";
				if (!(periodMs_ > 0.0))
					return "spare slot: no budget (source rate unknown)";
				if (!budget_.Measured())
					return "spare slot: no budget (render (module clock) not measured)";
				char buf[200] = {};
				std::snprintf(buf, sizeof(buf), "spare slot: budget %.2f ms (render (module clock) %.2f ms, period %.2f ms), used %.2f ms, outcome %s%s", budgetMs_,
					budget_.RenderMs(), periodMs_, usedEwmaMs_, VpSkeleton::SpareOutcomeName(last_.outcome),
					hostFedPresenter_ ? "" : "; not shared with render-ahead (no presenter)");
				return std::string(buf);
			}

		private:
			VpSkeleton::RenderBudget budget_;
			VpSkeleton::SpareReport last_;
			uint64_t lastSource_ = 0;
			bool sourceSeen_ = false;
			bool budgetFed_ = false;
			bool hostFedPresenter_ = false;
			bool originalSeen_ = false;
			double rateHz_ = 0.0;
			double cachedRateHz_ = 0.0;
			double periodMs_ = 0.0;
			double budgetMs_ = 0.0;
			double usedEwmaMs_ = 0.0;
			bool usedSeen_ = false;
			double declaredSliceMs_ = 0.0;
			double sharedMs_ = 0.0;
			uint64_t calls_ = 0;
			uint64_t budgetFeeds_ = 0;
		};
	}
}
