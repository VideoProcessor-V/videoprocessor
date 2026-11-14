// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// Slice F (lead10/design-queue-keyed-products.md s4): the cross-slice timeline on the CPU. A simulated host renders frame t, keeps
// D frames rendered ahead of the present (D = 2, 3, 5), queues L frames ahead (a look-ahead walk W inside it), MEASURES the terms
// (R from the plan, P from what it actually presented, L as a decaying peak of the queue size) and derives the window from them
// every frame, exactly as the skeleton will (slice B). Readers earlier in the queue demand frames ahead; the render reads its frame;
// the present reads its frame D renders later; a flush lands mid-run.
//   timeline: every present-time read is the spot made ahead (same pointer), 0 FrameReleased at present time, computed once per
//             frame (the post-flush refill counted apart), the window never shrinks below what is queued;
//   F-T1 AheadBudgetShortfallStepsDownInOrder (OWNER Q2, amendment 2 S10): S-A (Fill, Derived, Slice), then S-B, then S-C only
//             when both cannot fit; S-C = the DelayBudget's next lower ladder step (fake), total delay unchanged, never below the mode's
//             minimum queue, a no-op in Games / Competitive; the 200 ms AVR-safe tier is a CEILING (never a floor); five rates;
//             every step has its line; no step releases a spot; a larger grant climbs back;
//   F-T2 StalledPresentsStopAheadNoEviction (audit S3).
// Slice C (lane claim) and D/E cases are added when those slices land. CPU only, every test bounded.

#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/SkeletonAcquire.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace QueueKeyedTimelineTestsNs
{
	using namespace VpSkeleton;

	const char* const kGrainUnit = "repair/grain_split";

	struct Host {
		uint16_t depth;      // render-ahead depth the plan keeps (R)
		uint16_t queued;     // frames queued ahead of the render (the lane lead L)
		uint16_t walk;       // look-ahead walk W (inside the queue)
		uint64_t holdUs;     // the decaying-peak hold (the lane lead's rule; the test's clock)
	};

	TEST_CLASS(QueueKeyedTimelineTests)
	{
	public:
		TEST_METHOD(RenderAheadLookAheadAndFlushTimeline)
		{
			for (uint16_t D : { uint16_t(2), uint16_t(3), uint16_t(5) }) {
				const Host h{ D, uint16_t(D + 3), uint16_t(D + 1), 2000000 };
				SkeletonMemo m;
				std::atomic<int> calls{ 0 };
				OwnerFill f;
				f.fill = [&calls](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
					calls.fetch_add(1);
					out.bytes.assign(16, uint8_t(r.seq));
					return true;
				};
				m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
				WindowPeak leadPeak(h.holdUs), lagPeak(h.holdUs);
				std::map<uint64_t, const uint8_t*> made;
				std::map<uint64_t, bool> presentedOnce;
				uint64_t epoch = 0, presented = 0;
				bool havePresented = false;
				const uint64_t first = 100, last = 160, flushAt = 130;
				int refills = 0;
				for (uint64_t t = first; t <= last; ++t) {
					const uint64_t nowUs = t * 16683;   // ~60 Hz render clock
					if (t == flushAt) {
						m.Invalidate(SkeletonMemo::InvalidateCause::Flush);   // the queue flushed: everything attached goes
						++epoch;
						made.clear();
						havePresented = false;
					}
					// Measure the terms the way the skeleton will (slice B).
					uint16_t sizeNow = h.queued;
					if (t % 7 == 0)
						sizeNow = uint16_t(h.queued - 1);   // a one-frame wobble of the queue size: the peak holds
					leadPeak.Sample(nowUs, sizeNow);
					if (havePresented)
						lagPeak.Sample(nowUs, uint16_t(t - presented));
					QueueWindowTerms terms;
					terms.renderAhead = h.depth;
					terms.presentLag = lagPeak.Value(nowUs);
					terms.history = 0;
					terms.laneLead = leadPeak.Value(nowUs);
					terms.lookAheadWalk = h.walk;
					terms.aheadNeed = h.queued;   // the active consumer asks for the queued frames (capped by kAheadCeiling, amendment 3)
					terms.aheadNeedBy = "timeline consumer";
					terms.wired = uint8_t(QueueWindowTerms::kAll & ~(havePresented ? 0 : QueueWindowTerms::kPresentLag));
					const QueueWindow w = DeriveWindow(terms, t, epoch);
					Assert::IsTrue(w.ahead >= h.queued - 1u && w.back >= h.depth, L"the window covers the queue");
					m.SetWindow(w);
					if (havePresented)
						m.NotePresented(presented);
					m.OnSeal(t);
					// A reader earlier in the queue demands the furthest frame within the computed depth (amendment 3).
					Assert::IsTrue(w.aheadCompute == (h.queued < kAheadCeiling ? h.queued : kAheadCeiling), L"ahead compute = min(need, ceiling, queue)");
					const uint64_t ahead = t + w.aheadCompute;
					const AcquireResult a = m.Acquire(kP10GrainSplit, ahead, AcquireForm::Cpu);
					Assert::IsTrue(a.Ok() && a.cpu[0] == uint8_t(ahead), L"ahead demand admitted");
					if (!made.count(ahead))
						made[ahead] = a.cpu;
					// The render of t.
					const AcquireResult r = m.Acquire(kP10GrainSplit, t, AcquireForm::Cpu);
					Assert::IsTrue(r.Ok() && r.cpu[0] == uint8_t(t), L"render read");
					if (!made.count(t)) {
						made[t] = r.cpu;
						++refills;   // only right after the flush (nothing was demanded ahead for those frames since)
					}
					Assert::IsTrue(r.cpu == made[t], L"the render reads the spot made ahead");
					// The present of t - D (frames rendered after the flush only).
					if (t >= first + D && (t < flushAt || t >= flushAt + D)) {
						const uint64_t p = t - D;
						const AcquireResult pr = m.Acquire(kP10GrainSplit, p, AcquireForm::Cpu);
						Assert::IsTrue(pr.status == AcquireStatus::Reused && pr.cpu == made[p],
							(L"present of n while n + D renders (D " + std::to_wstring(D) + L")").c_str());
						presented = p;
						havePresented = true;
						presentedOnce[p] = true;
					}
				}
				const AcquireCounters c = m.CountersOf(kP10GrainSplit);
				Assert::IsTrue(c.unavailable[size_t(AcquireCause::FrameReleased)] == 0, L"0 FrameReleased at present time");
				Assert::IsTrue(c.unavailable[size_t(AcquireCause::NotQueued)] == 0, L"every demand inside the window");
				Assert::IsTrue(refills <= int(kAheadCeiling) + 1, L"a refill only for the frames the flush took");
				Assert::IsTrue(m.Invalidations(SkeletonMemo::InvalidateCause::Flush) == 1 && m.Epoch() == 1, L"one flush, one epoch");
				const int framesTouched = int((last + kAheadCeiling) - first + 1u);
				Assert::IsTrue(calls.load() - int(c.invalidatedRecompute) <= framesTouched, L"computed once per frame (recompute counted apart)");
			}
		}

		// F-T1: the shortfall is PLANNED (OWNER Q2; amendment 2 S10): S-A (Fill, Derived, Slice), then S-B, then S-C only when both
		// cannot fit. S-C is the DelayBudget's next LOWER ladder step (a fake of core/DelayBudget.h here: total delay unchanged by
		// padding, never below kMinimumDepth, no rounding of a time); its floor is the MODE's own minimum queue (standard: the ladder
		// floor; Games / Competitive: S-C is a no-op, never a raise). The 200 ms AVR-safe tier is a CEILING: with it selected the
		// total latency never exceeds 200 ms. Swept at 23.976, 25, 29.97, 50 and 59.94 Hz.
		TEST_METHOD(AheadBudgetShortfallStepsDownInOrder)
		{
			const double hz[] = { 23.976, 25.0, 29.97, 50.0, 59.94 };
			for (double rate : hz)
				for (double budgetMs : { 250.0, 200.0 /* the AVR-safe tier: a ceiling */ }) {
					// The delay budget the scheduler holds (the real DelayBudget law): its queue frames are the S-C starting point.
					DelayBudget::Inputs in;
					in.budgetMs = budgetMs;
					in.sourceFps = rate;
					in.refreshHz = 60.0;
					const DelayBudget::Plan budget = DelayBudget::Compute(in);
					if (!budget.feasible)
						continue;   // nothing to plan at this rate and tier (shown by the delay budget itself)
					const double periodMs = 1000.0 / rate;
					const double totalBefore = DelayBudget::FixedCostsMs(in) + budget.queueFrames * periodMs + budget.paddingMs;
					// The fake: one step down the ladder; the padding grows by the frame it removes, so the TOTAL stays.
					int fakeSteps = 0;
					double fakeTotal = totalBefore;
					LowerQueueStep fake = [&](uint16_t cur, uint16_t* lower) {
						if (int(cur) - 1 < DelayBudget::kMinimumDepth)
							return false;
						*lower = uint16_t(cur - 1u);
						++fakeSteps;
						fakeTotal = DelayBudget::FixedCostsMs(in) + double(*lower) * periodMs
							+ (budget.paddingMs + double(budget.queueFrames - *lower) * periodMs);
						return true;
					};
					for (QueueMode mode : { QueueMode::Normal, QueueMode::Games, QueueMode::Competitive })
						for (int coreAtFullWindow = 0; coreAtFullWindow < 2; ++coreAtFullWindow) {
							AheadNeed n;
							QueueWindowTerms t;
							t.renderAhead = 1;
							t.presentLag = 1;
							t.laneLead = uint16_t(budget.queueFrames);
							t.lookAheadWalk = 0;
							t.aheadNeed = uint16_t(budget.queueFrames);
							t.aheadNeedBy = "fake consumer";
							t.wired = QueueWindowTerms::kAll;
							n.window = DeriveWindow(t, 100, 0);
							n.mode = mode;
							n.lowerQueueStep = fake;
							const uint16_t A = n.window.aheadCompute;
							const JobClass classes[] = { JobClass::Fill, JobClass::Derived, JobClass::Slice, JobClass::Core };
							for (size_t i = 0; i < 4; ++i) {
								AheadCost c;
								c.id = uint16_t(10 + i);
								c.form = 1;
								c.bytesPerFrame = 1000;
								c.cls = classes[i];
								c.declaredDepth = classes[i] == JobClass::Core ? (coreAtFullWindow ? A : uint16_t(1)) : uint16_t(0);
								n.costs.push_back(c);
							}
							const uint64_t full = 1000ull * 4u * (1u + n.window.back + A);
							std::vector<AheadStep> seen;
							uint16_t lastTarget = A;
							fakeSteps = 0;
							fakeTotal = totalBefore;
							for (uint64_t grant = full; ; grant -= 250) {
								n.grantBytes = grant;
								const AheadPlan p = PlanAheadLadder(n);
								Assert::IsFalse(p.line.empty(), L"every step has its line");
								Assert::IsTrue(p.line.find(Token(p.step)) != std::string::npos, L"the line names the step");
								uint16_t dFill = 0, dDer = 0, dSlice = 0, dCore = 0;
								p.Has(10, &dFill);
								p.Has(11, &dDer);
								p.Has(12, &dSlice);
								p.Has(13, &dCore);
								if (p.step == AheadStep::ReduceEarlyProducts)
									Assert::IsTrue(dCore == A && (dFill == 0 || dDer == A) && (dDer == 0 || dSlice == A), L"S-A: Fill, then Derived, then Slice");
								if (p.step == AheadStep::FewerFramesAhead)
									Assert::IsTrue(dFill == 0 && dDer == 0 && dSlice == 0 && dCore >= 1 && dCore < A, L"S-B: core shrinks, not below its declared depth");
								if (p.step == AheadStep::ShorterQueue || p.step == AheadStep::AtLatencyFloor)
									Assert::IsTrue(dFill == 0 && dDer == 0 && dSlice == 0, L"S-C only after S-A and S-B");
								Assert::IsTrue(p.queueTargetFrames <= lastTarget && p.queueTargetFrames <= A, L"never a raise");
								if (mode != QueueMode::Normal)
									Assert::IsTrue(p.queueTargetFrames == A && p.step != AheadStep::ShorterQueue, L"Games / Competitive: S-C is a no-op");
								else
									Assert::IsTrue(p.queueTargetFrames >= DelayBudget::kMinimumDepth || p.queueTargetFrames == A, L"never below the mode's minimum queue");
								Assert::IsTrue(fakeTotal <= totalBefore + 1e-6 && fakeTotal >= totalBefore - 1e-6, L"the total delay unchanged by S-C");
								if (budgetMs == 200.0)
									Assert::IsTrue(totalBefore <= 200.0 + 1e-6, L"the AVR-safe tier is a ceiling: total <= 200 ms");
								if (seen.empty() || seen.back() != p.step)
									seen.push_back(p.step);
								lastTarget = p.queueTargetFrames;
								if (p.step == AheadStep::AtLatencyFloor || grant <= 250)
									break;
							}
							for (size_t i = 1; i < seen.size(); ++i)
								Assert::IsTrue(uint8_t(seen[i]) > uint8_t(seen[i - 1]), L"S-A -> S-B -> S-C -> floor, in order");
							Assert::IsTrue(seen.front() == AheadStep::None && seen.back() == AheadStep::AtLatencyFloor, L"from full depth down to the floor");
							if (mode == QueueMode::Normal && coreAtFullWindow && A > DelayBudget::kMinimumDepth)
								Assert::IsTrue(std::find(seen.begin(), seen.end(), AheadStep::ShorterQueue) != seen.end() && fakeSteps > 0,
									L"S-C reached through the delay budget when S-A and S-B cannot fit");
							n.grantBytes = full;
							Assert::IsTrue(PlanAheadLadder(n).step == AheadStep::None, L"step-up climbs back");
						}
				}
		}

		// F-T2 (audit S3): presents STALL while renders go on: ahead demand stops visibly (PresentsStalled), nothing is evicted, and
		// once presents move the ahead work resumes.
		TEST_METHOD(StalledPresentsStopAheadNoEviction)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			OwnerFill f;
			f.fill = [&calls](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				calls.fetch_add(1);
				out.bytes.assign(8, uint8_t(r.seq));
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
			QueueWindowTerms t;
			t.renderAhead = 2;
			t.presentLag = 2;
			t.laneLead = 2;
			t.aheadNeed = 2;
			t.aheadNeedBy = "stall consumer";
			t.wired = QueueWindowTerms::kAll;
			const uint64_t stalledAt = 20;
			size_t attached = 0;
			bool stalledSeen = false;
			for (uint64_t s = 10; s <= 40; ++s) {
				const uint64_t presentedSeq = s <= stalledAt ? s - 2 : stalledAt - 2;   // presents stop at frame 18
				t.presentLag = uint16_t(s - presentedSeq);                             // P as measured: it grows in the stall
				QueueWindow w = DeriveWindow(t, s, 0);
				m.SetWindow(w);
				m.NotePresented(presentedSeq);
				m.OnSeal(s);
				m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu);
				const AcquireResult a = m.Acquire(kP10GrainSplit, s + 2, AcquireForm::Cpu);
				if (a.cause == AcquireCause::PresentsStalled)
					stalledSeen = true;
				if (s > stalledAt)
					Assert::IsTrue(m.MemoCount() >= attached, L"nothing evicted while presents stall");
				attached = m.MemoCount();
			}
			Assert::IsTrue(stalledSeen, L"ahead demand stopped, visibly");
			bool shown = false;
			for (const FallbackCatalog::Decision& d : m.Decisions())
				shown = shown || d.reason.find("presents stalled") != std::string::npos;
			Assert::IsTrue(shown, L"on the Fallbacks list");
			for (uint64_t f2 = stalledAt - 2; f2 <= 40; ++f2)
				Assert::IsTrue(m.Acquire(kP10GrainSplit, f2, AcquireForm::Cpu).Ok(), L"every unpresented frame still attached or refillable");
			t.presentLag = 1;
			QueueWindow w = DeriveWindow(t, 42, 0);
			m.SetWindow(w);
			m.NotePresented(41);
			m.OnSeal(42);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 44, AcquireForm::Cpu).Ok(), L"presents moved: ahead work resumes");
		}

		// No step of the plan releases a spot of a queued frame: the memo keeps every attached frame when a smaller plan arrives.
		TEST_METHOD(APlanStepReleasesNothingAttached)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			OwnerFill f;
			f.fill = [&calls](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				calls.fetch_add(1);
				out.bytes.assign(8, uint8_t(r.seq));
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
			QueueWindowTerms t;
			t.laneLead = kAheadCeiling;
			t.aheadNeed = kAheadCeiling;
			t.wired = QueueWindowTerms::kAll;
			m.SetWindow(DeriveWindow(t, 50, 0));
			m.OnSeal(50);
			for (uint64_t s = 50; s <= 56; ++s)
				m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu);
			const uint64_t releasedBefore = m.CountersOf(kP10GrainSplit).released;
			AheadPlan p;
			p.depth.push_back(std::make_pair(uint16_t(kP10GrainSplit), uint16_t(0)));
			p.step = AheadStep::ShorterQueue;
			p.line = "memo ahead plan: shorter queue";
			m.SetAheadPlan(p);
			Assert::IsTrue(m.MemoCount() == 7 && m.CountersOf(kP10GrainSplit).released == releasedBefore, L"nothing released by the plan");
			for (uint64_t s = 50; s <= 56; ++s)
				Assert::IsTrue(m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu).status == AcquireStatus::Reused, L"every attached frame served");
			Assert::AreEqual(7, calls.load(), L"computed once");
		}
	};
}
