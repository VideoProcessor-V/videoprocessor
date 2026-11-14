// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// Slice A (lead10/design-queue-keyed-products.md s2.2, s4): the queue window is DERIVED from measured terms, never configured.
//   QW-T1 back = H + max(R, P), ahead = L + W (amendment 2 A1) over a sweep of every term; an undelivered term reads 0, "not fed";
//   QW-T1b H from a graph: a 223-frame record history leaves H unchanged (lag edges + maxSealLag + fill history only; audit M2);
//   QW-T2 the window peak (P): a dip shorter than the window keeps the value, a sustained fall shrinks it after the window;
//   QW-T2b a window that shrinks never makes the memo release a frame that is still queued (the presented edge);
//   QW-T6 EQUALITY with the shared terms: Frames() == FramesInFlight(0, W, H, max(R, P), L) and QueueFrames is THE R + L + W count
//         (audit S5 / M1: one derivation);
//   the board: a published window is read back whole from another thread;
//   S-C's law (audit MUST 2): DelayBudget::NextLowerQueueFrames steps down one feasible frame at a time to kMinimumDepth, the total
//         delay unchanged; a plan below aheadCompute binds the window as Budget, shown (audit SHOULD 7).
// CPU only, every test bounded.

#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/FramesInFlight.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/QueueFrames.h>
#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/SkeletonAcquire.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace QueueWindowTestsNs
{
	using namespace VpSkeleton;

	QueueWindowTerms Terms(uint16_t R, uint16_t P, uint16_t H, uint16_t L, uint16_t W, uint8_t wired = QueueWindowTerms::kAll)
	{
		QueueWindowTerms t;
		t.renderAhead = R;
		t.presentLag = P;
		t.history = H;
		t.laneLead = L;
		t.lookAheadWalk = W;
		t.wired = wired;
		return t;
	}

	TEST_CLASS(QueueWindowTests)
	{
	public:
		TEST_METHOD(DeriveSweepBackIsHistoryPlusMaxRenderAheadPresentLagAheadIsLeadPlusWalk)
		{
			for (uint16_t R = 0; R <= 5; ++R)
				for (uint16_t P = 0; P <= 4; ++P)
					for (uint16_t H = 0; H <= 2; ++H)
						for (uint16_t L = 0; L <= 13; ++L)
							for (uint16_t W = 0; W <= 13; W = uint16_t(W + 13)) {
								const QueueWindow w = DeriveWindow(Terms(R, P, H, L, W), 100, 7);
								const uint16_t back = uint16_t(H + (R > P ? R : P));
								const uint16_t ahead = uint16_t(L + W);
								Assert::IsTrue(w.back == back && w.ahead == ahead && w.history == H, L"back / ahead / history");
								Assert::IsTrue(w.Frames() == uint32_t(back) + 1u + uint32_t(ahead), L"frames = back + 1 + ahead");
								Assert::IsTrue(w.current == 100 && w.epoch == 7 && w.presentKnown && w.presented == 100u - P, L"facts");
								// QW-T6: EQUALITY on the shared terms (no queue capacity term; R the back term max(R, P)).
								const size_t fif = FramesInFlight(0, W, H, size_t(R > P ? R : P), L);
								Assert::IsTrue(size_t(w.Frames()) == fif, L"Frames() == FramesInFlight(shared terms)");
								Assert::IsTrue(uint32_t(w.ahead) == QueueFrames(0, L, W), L"ahead is THE queue-frames count of L and W");
							}
		}

		TEST_METHOD(UndeliveredTermsReadZeroAndAreNamedNotFed)
		{
			const QueueWindow none = DeriveWindow(Terms(4, 3, 2, 9, 6, 0), 50, 0);
			Assert::IsTrue(none.back == 0 && none.ahead == 0 && !none.presentKnown, L"nothing delivered: the floors");
			const std::string nf = NotFed(0);
			Assert::IsTrue(nf.find("render-ahead depth") != std::string::npos && nf.find("present facts") != std::string::npos
				&& nf.find("history") != std::string::npos && nf.find("lane lead") != std::string::npos
				&& nf.find("look-ahead walk") != std::string::npos, L"each named");
			Assert::IsTrue(NotFed(QueueWindowTerms::kAll).empty(), L"all fed: nothing named");
			const QueueWindowTerms partial = Terms(4, 3, 2, 9, 6, QueueWindowTerms::kRenderAhead | QueueWindowTerms::kLaneLead);
			const QueueWindow w = DeriveWindow(partial, 50, 0);
			Assert::IsTrue(w.back == 4 && w.ahead == 9 && !w.presentKnown && w.renderAhead == 4, L"only the delivered terms count");
			const std::string text = WindowText(partial, w);
			Assert::IsTrue(text.find("presented lag not fed") != std::string::npos && text.find("frames 14") != std::string::npos, L"status text");
		}

		// QW-T1b (audit M2): H is the largest input LAG edge + maxSealLag + the fills' declared history; a product's own record history
		// (Node::history / historyFull up to 223 frames) never counts.
		TEST_METHOD(RecordHistoryLeavesHistoryTermUnchanged)
		{
			Graph g;
			Node record;
			record.id = kQualityPattern;
			record.history = kRecordHistoryMax;       // 223 frames of record history
			record.historyFull = kRecordHistoryMax;
			record.inputCount = 1;
			record.inputs[0].product = kP4SourceStats;
			record.inputs[0].lag = 1;                // read at lag 1
			g.nodes.push_back(record);
			Assert::AreEqual<int>(1, HistoryTermOf(&g, 0), L"lag edge 1, not the 223-frame record history");
			g.maxSealLag = 2;
			Assert::AreEqual<int>(3, HistoryTermOf(&g, 0), L"+ maxSealLag");
			Assert::AreEqual<int>(4, HistoryTermOf(&g, 2), L"the fills' declared history: max(lag, fill) + sealLag");
			Assert::AreEqual<int>(0, HistoryTermOf(nullptr, 0), L"no graph, no fill history: 0");
		}

		TEST_METHOD(WindowPeakHoldsThroughADipAndShrinksAfterASustainedFall)
		{
			// The caller's window: the lane lead's (core kLeadWindowSeconds; P uses the same, A1); the window does not choose it.
			// Every time below is in tenths of that hold, so the test follows the constant.
			const uint64_t hold = SecondsToUs(kLeadWindowSeconds);
			const uint64_t u = hold / 10u;
			Assert::IsTrue(u > 1u, L"the hold is long enough to sample in tenths");
			WindowPeak p(hold);
			p.Sample(0, 6);
			p.Sample(1 * u, 2);   // a dip
			p.Sample(2 * u, 6);
			Assert::AreEqual<int>(6, p.Value(3 * u), L"a dip shorter than the hold keeps the peak");
			for (uint64_t t = 3 * u; t <= 14 * u; t += u)
				p.Sample(t, 3);   // a sustained fall
			Assert::AreEqual<int>(6, p.Value(11 * u), L"still within the hold of the last 6");
			Assert::AreEqual<int>(3, p.Value(12 * u + u / 2), L"after the hold the window follows the fall");
			p.Sample(13 * u, 9);
			Assert::AreEqual<int>(9, p.Value(13 * u + 1), L"a rise is taken at once");
		}

		// QW-T8 .. QW-T11 (amendment 3): aheadCompute = min(max declared need of the ACTIVE consumers, kAheadCeiling, queue ahead).
		TEST_METHOD(AheadComputeIsTheMaxDeclaredNeedCappedByCeilingAndQueue)
		{
			const uint16_t queueAhead = uint16_t(kAheadCeiling + 2);   // derived from the ceiling, no literal depth
			QueueWindowTerms t = Terms(1, 1, 0, uint16_t(queueAhead - 1), 1);
			// QW-T8: the max of the declared needs, named.
			const AheadDemand need = MaxAheadNeed({ AheadDemand{ uint16_t(kAheadCeiling - 3), "camera" },
				AheadDemand{ uint16_t(kAheadCeiling - 2), "temporal detail" }, AheadDemand{ uint16_t(kAheadCeiling - 4), "deband" } });
			t.aheadNeed = need.need;
			t.aheadNeedBy = need.consumer;
			QueueWindow w = DeriveWindow(t, 50, 0);
			Assert::IsTrue(w.aheadCompute == kAheadCeiling - 2 && w.aheadBinding == AheadBinding::Consumer, L"QW-T8 max of the needs");
			Assert::IsTrue(AheadComputeText(w).find("by temporal detail") != std::string::npos && !AheadComputeCapped(w), L"named, no cap line");
			Assert::IsTrue(w.ahead == queueAhead, L"the queue term is unchanged by the compute depth");
			// QW-T9: no active consumer -> 0.
			const AheadDemand none = MaxAheadNeed({});
			t.aheadNeed = none.need;
			w = DeriveWindow(t, 50, 0);
			Assert::IsTrue(w.aheadCompute == 0 && w.aheadBinding == AheadBinding::NoConsumer, L"QW-T9 no consumer: 0");
			// QW-T10: a request above the ceiling gives the ceiling, shown.
			t.aheadNeed = uint16_t(kAheadCeiling + 3);
			w = DeriveWindow(t, 50, 0);
			Assert::IsTrue(w.aheadCompute == kAheadCeiling && w.aheadBinding == AheadBinding::Ceiling && AheadComputeCapped(w), L"QW-T10 ceiling");
			Assert::IsTrue(AheadComputeText(w).find("ceiling " + std::to_string(kAheadCeiling)) != std::string::npos, L"ceiling shown");
			// QW-T11: a queue shorter than the need binds, shown.
			const uint16_t shortQueue = uint16_t(kAheadCeiling / 2);
			QueueWindowTerms q = Terms(1, 1, 0, shortQueue, 0);
			q.aheadNeed = uint16_t(shortQueue + 1);
			w = DeriveWindow(q, 50, 0);
			Assert::IsTrue(w.aheadCompute == shortQueue && w.aheadBinding == AheadBinding::Queue && AheadComputeText(w).find("queue") != std::string::npos,
				L"QW-T11 queue binds");
		}

		// QW-T12: a frame beyond aheadCompute (but inside the queue) is computed ONCE when it comes within aheadCompute, not at enqueue.
		// QW-T13: the keep window still spans the full queue back.
		TEST_METHOD(BeyondAheadComputeIsComputedOnApproachKeepSpansTheQueueBack)
		{
			SkeletonMemo m;
			const uint16_t compute = uint16_t(kAheadCeiling / 3);
			QueueWindowTerms t = Terms(3, 3, 1, kAheadCeiling, 0);   // queue ahead = the ceiling, compute asked lower
			t.aheadNeed = compute;
			const uint64_t far = 20u + compute + 2u;   // inside the queue, beyond the computed depth
			std::atomic<int> calls{ 0 };   // computes of frame `far` only
			OwnerFill f;
			f.fill = [&calls, far](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				if (r.seq == far)
					calls.fetch_add(1);
				out.bytes.assign(8, uint8_t(r.seq));
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "repair/grain_split", f);
			for (uint64_t s = 20; s <= far; ++s) {
				const QueueWindow w = DeriveWindow(t, s, 0);
				m.SetWindow(w);
				m.OnSeal(s);
				const AcquireResult a = m.Acquire(kP10GrainSplit, far, AcquireForm::Cpu);
				if (far > s + compute)
					Assert::IsTrue(a.cause == AcquireCause::NotPlannedAhead && calls.load() == 0, L"not at enqueue: refused, not computed");
				else
					Assert::IsTrue(a.Ok() && calls.load() == 1, L"computed once on approach");
				m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu);   // the render of s (frames up to far - 1 are not demanded otherwise)
			}
			// QW-T13: the keep window is the full queue back (H + max(R, P)), not the compute depth.
			const QueueWindow w = DeriveWindow(t, far, 0);
			const unsigned queueBack = unsigned(t.history) + (std::max)(unsigned(t.renderAhead), unsigned(t.presentLag));
			Assert::IsTrue(w.back == queueBack && w.back > w.aheadCompute && m.KeepFrames() == w.back, L"keep spans the full queue back");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, far - w.back, AcquireForm::Cpu).status == AcquireStatus::Reused, L"frame far - back still attached");
		}

		// QW-T2b: a window that SHRINKS (the render-ahead plan stopped) never releases a frame that is still waiting to be presented.
		TEST_METHOD(ShrinkingWindowNeverReleasesAQueuedFrame)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			OwnerFill f;
			f.fill = [&calls](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				calls.fetch_add(1);
				out.bytes.assign(8, uint8_t(r.seq));
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "repair/grain_split", f);
			m.SetWindow(DeriveWindow(Terms(3, 3, 0, 0, 0), 10, 0));
			m.OnSeal(10);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu).Ok(), L"frame 10");
			// The plan stops: R and P fall to 0 at the next frame, but frame 10 was presented only up to 9.
			QueueWindow shrunk = DeriveWindow(Terms(0, 0, 0, 0, 0, QueueWindowTerms::kAll & ~QueueWindowTerms::kPresentLag), 11, 0);
			m.SetWindow(shrunk);
			m.NotePresented(9);
			m.OnSeal(11);
			const AcquireResult r = m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu);
			Assert::IsTrue(r.status == AcquireStatus::Reused && calls.load() == 1, L"frame 10 not yet presented: still queued, kept");
			m.NotePresented(10);   // frame 10 is the one being presented: still in the queue (f < presented - H releases)
			m.OnSeal(12);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu).status == AcquireStatus::Reused, L"kept through its own present");
			m.NotePresented(11);
			m.OnSeal(13);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu).cause == AcquireCause::FrameReleased && calls.load() == 1,
				L"released once the queue moved past it, never recomputed");
		}

		TEST_METHOD(BoardPublishesAWholeWindowToOtherThreads)
		{
			QueueWindowBoard board;
			QueueWindow w;
			Assert::IsFalse(board.Read(&w), L"nothing before the first publish");
			std::atomic<bool> stop{ false }, torn{ false };
			std::thread reader([&]() {
				QueueWindow r;
				while (!stop.load())
					if (board.Read(&r) && !(r.back == uint16_t(r.current % 7u) && r.ahead == uint16_t(r.current % 5u) && r.epoch == r.current / 3u))
						torn = true;
			});
			for (uint64_t c = 1; c <= 20000; ++c) {
				QueueWindow x;
				x.current = c;
				x.back = uint16_t(c % 7u);
				x.ahead = uint16_t(c % 5u);
				x.epoch = c / 3u;
				board.Publish(x);
			}
			stop = true;
			reader.join();
			Assert::IsFalse(torn.load(), L"never a torn window");
			Assert::IsTrue(board.Read(&w) && w.current == 20000, L"the last one");
		}

		// S-C's law (audit MUST 2): the DELAY BUDGET gives each lower step for the same inputs. Design depth first, then one frame
		// down per step to kMinimumDepth, then no step; fixed + queue + padding stays budgetMs and the padding grows. Swept rates.
		TEST_METHOD(DelayBudgetNextLowerQueueFramesKeepsTheTotal)
		{
			for (double display : { 50.0, 59.94, 120.0 })
				for (double rate : { 23.976, 25.0, 29.97 }) {
					DelayBudget::Inputs in;
					in.sourceFps = rate;
					in.refreshHz = display;
					in.processingMs = DelayBudget::kProcessingAllowanceMs;
					in.budgetMs = DelayBudget::RecommendedBudgetMs(in, std::vector<double>{ rate });   // the design depth fits exactly
					DelayBudget::Plan plan = DelayBudget::Compute(in);
					Assert::IsTrue(plan.feasible && plan.queueFrames == DelayBudget::kDesignDepth, L"the recommended budget gives the design depth");
					int steps = 0;
					DelayBudget::Plan lower;
					while (DelayBudget::NextLowerQueueFrames(in, plan.queueFrames, &lower)) {
						Assert::AreEqual(plan.queueFrames - 1, lower.queueFrames, L"one frame down per step");
						Assert::IsTrue(lower.paddingMs > plan.paddingMs, L"the padding makes up the frame given up");
						Assert::IsTrue(std::abs(lower.fixedMs + lower.queueMs + lower.paddingMs - in.budgetMs) <= 1e-6, L"the total stays budgetMs");
						Assert::IsTrue(lower.degraded && lower.feasible, L"a lower step is a visible degradation, still feasible");
						plan = lower;
						++steps;
					}
					Assert::AreEqual(DelayBudget::kMinimumDepth, plan.queueFrames, L"stops at kMinimumDepth");
					Assert::AreEqual(DelayBudget::kDesignDepth - DelayBudget::kMinimumDepth, steps, L"every step taken once");
					Assert::IsFalse(DelayBudget::NextLowerQueueFrames(in, DelayBudget::kMinimumDepth, &lower), L"never below the floor");
					DelayBudget::Inputs unfed = in;
					unfed.sourceFps = 0.0;
					Assert::IsFalse(DelayBudget::NextLowerQueueFrames(unfed, DelayBudget::kDesignDepth, &lower), L"no rate measured: no step");
				}
		}

		// Audit SHOULD 7 (no silent cap): a plan that computes fewer frames ahead than the window's aheadCompute binds the window as
		// Budget, and the cap is shown; a plan at full depth leaves the window as it was.
		TEST_METHOD(APlanBelowAheadComputeBindsTheWindowAsBudget)
		{
			QueueWindowTerms t = Terms(1, 1, 0, kAheadCeiling, 0);
			t.aheadNeed = kAheadCeiling;
			t.aheadNeedBy = "camera";
			const QueueWindow w = DeriveWindow(t, 50, 0);
			Assert::IsTrue(w.aheadCompute == kAheadCeiling && w.aheadBinding != AheadBinding::Budget, L"the window before any plan");
			AheadPlan full;
			full.aheadCap = w.aheadCompute;
			full.depth.push_back(AheadPlan::Entry{ uint16_t(kP10GrainSplit), 0, w.aheadCompute });
			const QueueWindow same = WithAheadPlan(w, full);
			Assert::IsTrue(same.aheadCompute == w.aheadCompute && same.aheadBinding == w.aheadBinding, L"a full-depth plan binds nothing");
			AheadPlan shorter = full;
			shorter.depth[0].depth = uint16_t(w.aheadCompute - 2u);
			const QueueWindow bound = WithAheadPlan(w, shorter);
			Assert::IsTrue(bound.aheadCompute == w.aheadCompute - 2u && bound.aheadBinding == AheadBinding::Budget, L"the plan's depth, bound by the budget");
			Assert::IsTrue(AheadComputeCapped(bound) && AheadComputeText(bound).find("budget") != std::string::npos, L"the cap is shown");
			AheadPlan capped = full;
			capped.aheadCap = uint16_t(w.aheadCompute - 3u);   // S-C lowered the cap below every product's depth
			Assert::IsTrue(WithAheadPlan(w, capped).aheadCompute == w.aheadCompute - 3u, L"the S-C cap binds too");
		}

		// Slice C wiring (lead 11; OD 11-14, design s10): the feed the window hands S-3a's PlanAhead carries the ACTIVE consumers with
		// their declared needs, the queue frames ahead and THE ceiling, and the law applied to it gives the window's own aheadCompute
		// (one law, no drift) for every binding: consumer, ceiling, queue and none. Inactive (need 0) consumers are not handed over.
		TEST_METHOD(PlanAheadFeedGivesTheWindowsAheadCompute)
		{
			const uint16_t c = kAheadCeiling;
			struct Case { uint16_t laneLead; std::vector<AheadDemand> active; AheadBinding binding; };
			const Case cases[] = {
				{ uint16_t(c + 4u), { AheadDemand{ uint16_t(c - 2u), "camera" }, AheadDemand{ uint16_t(c - 1u), "temporal detail" } }, AheadBinding::Consumer },
				{ uint16_t(c + 4u), { AheadDemand{ uint16_t(c + 3u), "temporal detail" } }, AheadBinding::Ceiling },
				{ uint16_t(c - 3u), { AheadDemand{ uint16_t(c - 1u), "camera" } }, AheadBinding::Queue },
				{ uint16_t(c + 4u), { AheadDemand{ 0, "idle" } }, AheadBinding::NoConsumer },
				{ uint16_t(c + 4u), {}, AheadBinding::NoConsumer },
			};
			for (const Case& k : cases) {
				QueueWindowTerms t = Terms(1, 1, 0, k.laneLead, 0);
				SetAheadConsumers(t, k.active);
				Assert::IsTrue((t.wired & QueueWindowTerms::kAheadNeed) != 0, L"the consumers are fed (an empty list = none active)");
				const QueueWindow w = DeriveWindow(t, 100, 0);
				Assert::IsTrue(w.aheadBinding == k.binding, L"the binding");
				const PlanAheadFeed f = PlanAheadFeedOf(w, k.active);
				Assert::AreEqual<uint16_t>(kAheadCeiling, f.aheadCeiling, L"THE ceiling, never a call-site copy");
				Assert::AreEqual<uint16_t>(w.ahead, f.queueAheadFrames, L"the queue frames ahead are the window's");
				Assert::AreEqual<uint16_t>(w.aheadCompute, f.aheadCompute, L"the window's own aheadCompute rides along");
				Assert::AreEqual<uint16_t>(w.aheadCompute, AheadComputeOf(f), L"the law on the feed == the window's aheadCompute");
				for (const AheadDemand& d : f.activeConsumers)
					Assert::IsTrue(d.need > 0, L"an inactive consumer is not handed over");
				if (k.binding == AheadBinding::NoConsumer)
					Assert::AreEqual<uint16_t>(0, AheadComputeOf(f), L"no active consumer: 0");
				if (k.binding == AheadBinding::Ceiling)
					Assert::AreEqual<uint16_t>(kAheadCeiling, AheadComputeOf(f), L"a larger need is held to the ceiling");
			}
		}
	};
}
