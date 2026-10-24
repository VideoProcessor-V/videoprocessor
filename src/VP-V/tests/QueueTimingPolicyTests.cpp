// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Queue/timing policy (2026-10-24 draft, branch feat/motion-timing-extract). CPU only.
//
//  - VpFixedBudgetPolicy == a FROZEN copy of the renderer host's d0212534 code (UpdateQueueDelayHold,
//    PrefillTargetLocked, HoldTicksUntilDueLocked, the RenderLoop backlog block) over a sweep:
//    commit (a) is a byte-identical move of the decisions.
//  - the 4/3/2 ladder, the capacity cap, the held shed (exact late frames, run of late heads), the
//    age gate from the frame's START;
//  - either/or: BillQueuePolicy is upstream's behaviour; the Original pathway served by the VP
//    policy decides like upstream on a frame-count queue; selection rules;
//  - the one look-ahead walk == the old SelectLookaheadWalk, and its preparation view == upstream's
//    SelectActivePicturePreparation (v1.4.02, reference copy below).

#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>
#include <VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h>
#include <vprenderer/QueueLookaheadWalk.h>
#include <vprenderer/QueueTimingPolicy.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace QueueTimingPolicy;

namespace
{
	// ---- FROZEN host code (d0212534), reduced to its inputs; do not edit -----------------------
	struct FrozenHold { bool active = false; double holdMs = 0, transferMs = 0, fraction = 0; size_t targetFrames = 0; std::string note; int queueFrames = 0; bool degraded = false; };

	FrozenHold FrozenUpdateQueueDelayHold(int budgetMs, bool originalPathway, bool lowLatency, double sourceRateHz,
		double signalRateHz, double displayHz, bool renderAheadActive, double lead, size_t credit, size_t capacity)
	{
		if (originalPathway)
			budgetMs = 0;
		DelayBudget::Plan plan;
		double transferMs = 0.0;
		std::string budgetNote;
		if (budgetMs > 0 && sourceRateHz > 0.0 && !lowLatency)
		{
			DelayBudget::Model model;
			if (displayHz > 0.0)
			{
				model.refreshHz = displayHz;
				if (renderAheadActive)
				{
					const double ps = 1.0 / sourceRateHz;
					model.renderAheadMs = (std::max)(0.0, lead - static_cast<double>(credit) * ps) * 1000.0;
					budgetNote = "render-ahead on: the total across frame rates is not verified yet (PENDING-RA)";
				}
				const DelayBudget::Inputs budgetInputs = DelayBudget::InputsFor(budgetMs, sourceRateHz, model, signalRateHz);
				transferMs = budgetInputs.captureTransferMs;
				plan = DelayBudget::Compute(budgetInputs);
				if (!plan.feasible)
					budgetNote = "too short for 2 queued frames at this rate; frame-count queue";
				else
				{
					const double periodMs = 1000.0 / sourceRateHz;
					const size_t needed = DelayBudget::QueueCapacityFor(plan.holdMs, periodMs);
					if (needed > capacity)
					{
						const double reachableMs = plan.fixedMs + DelayBudget::HoldForCapacity(capacity, periodMs);
						const DelayBudget::Plan cut = DelayBudget::Compute(DelayBudget::InputsFor(reachableMs, sourceRateHz, model, signalRateHz));
						char text[192] = {};
						sprintf_s(text, "queue_size %zu holds %.0f of %d ms at %.3f Hz (needs %zu); raise queue_size", capacity,
							cut.feasible ? cut.fixedMs + cut.holdMs : 0.0, budgetMs, sourceRateHz, needed);
						budgetNote = text;
						plan = cut;
					}
				}
			}
			else
				budgetNote = "display refresh not known yet; frame-count queue";
		}
		FrozenHold next;
		next.active = plan.feasible;
		next.holdMs = next.active ? plan.holdMs : 0.0;
		next.transferMs = next.active ? transferMs : 0.0;
		next.fraction = next.active ? plan.holdFraction : 0.0;
		next.targetFrames = next.active ? plan.queueTargetFrames : 0;
		next.note = budgetNote;
		next.queueFrames = plan.queueFrames;
		next.degraded = plan.degraded;
		return next;
	}

	size_t FrozenPrefill(bool useFrameQueue, const FrozenHold& hold, size_t lockedFilmTarget, size_t capacity, size_t credit,
		size_t floor, bool fixes)
	{
		if (!useFrameQueue)
			return 1;
		const size_t desired = hold.active ? AlphaQueuePolicy::ClampDesiredDepthToCapacity(hold.targetFrames, capacity) : lockedFilmTarget;
		return AlphaQueuePolicy::CombineQueueTarget(desired, credit, floor, fixes);
	}

	int64_t FrozenTicks(const FrozenHold& hold, bool timeline, bool fileFrame, int64_t arrival, int64_t enqueue, int64_t now, int64_t freq)
	{
		if (!hold.active || timeline)
			return 0;
		const int64_t since = arrival != 0 ? arrival : enqueue;
		if (fileFrame || since == 0)
			return 0;
		if (freq <= 0)
			return 0;
		const int64_t due = since + static_cast<int64_t>((hold.transferMs + hold.holdMs) * static_cast<double>(freq) / 1000.0);
		return due > now ? due - now : 0;
	}

	struct FrozenBacklog { bool recovered = false, held = false; size_t heldDrop = 0; };

	FrozenBacklog FrozenBacklogBlock(size_t queueBefore, size_t target, double oldestMs, double cycleMs, double captureRateHz,
		double signalRateHz, double frontFilmHz, bool fixes, bool file, bool qpc, const FrozenHold& hold, double headAgeMs, unsigned& run)
	{
		FrozenBacklog r;
		const double queueRateHz = fixes ? AlphaQueuePolicy::QueueFrameRateHz(frontFilmHz, signalRateHz) : captureRateHz;
		r.recovered = !file && AlphaQueuePolicy::ShouldRecoverBacklog(queueBefore, target, oldestMs, cycleMs, queueRateHz, fixes);
		const bool heldEvaluated = !r.recovered && hold.active && qpc && !file && queueBefore > 0;
		if (!heldEvaluated)
			run = 0;
		if (heldEvaluated)
		{
			const double heldMs = hold.transferMs + hold.holdMs;
			r.held = AlphaQueuePolicy::ShouldShedHeldBacklog(queueBefore, target, headAgeMs, heldMs, queueRateHz) ||
				AlphaQueuePolicy::ShouldShedHeldExcess(queueBefore, target, headAgeMs, heldMs, queueRateHz, run, hold.holdMs);
			if (r.held)
				r.heldDrop = AlphaQueuePolicy::HeldShedFrames(queueBefore, target, headAgeMs, heldMs, queueRateHz);
			r.recovered = r.held && r.heldDrop > 0;
		}
		return r;
	}

	// ---- upstream v1.4.02 SelectActivePicturePreparation (reference copy, billslack2 b1415da4) ---
	template<typename Queue>
	std::vector<size_t> UpstreamPreparation(const Queue& queue, size_t configured, size_t maxLookahead, size_t requiredOutward)
	{
		auto indices = AlphaQueuePolicy::SelectActivePicturePreview(queue, configured, maxLookahead).indices;
		size_t outwardFrames = 0;
		for (size_t index = 1; index < queue.size() && outwardFrames < requiredOutward; ++index)
		{
			if (queue[index].cadenceRepeat) continue;
			indices.push_back(index);
			++outwardFrames;
		}
		std::sort(indices.begin(), indices.end());
		indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
		return indices;
	}

	struct Q { bool cadenceRepeat = false; };

	HoldPlan ToPlan(const FrozenHold& f)
	{
		HoldPlan p;
		p.active = f.active; p.holdMs = f.holdMs; p.transferMs = f.transferMs; p.fraction = f.fraction; p.targetFrames = f.targetFrames;
		return p;
	}

	const double kRates[] = { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0, 48.0, 50.0, 60000.0 / 1001.0, 60.0, 120.0 };
}

namespace VpvTests
{
	TEST_CLASS(QueueTimingPolicy)
	{
	public:
		TEST_METHOD(PlanHoldMatchesFrozenHost)
		{
			VpQueueTiming::VpFixedBudgetPolicy policy;
			size_t cases = 0;
			for (int budget : { 0, 120, 184, 200, 226, 250, 268, 400 })
				for (double rate : kRates)
					for (double display : { 0.0, 60.0, 59.94, 120.0 })
						for (int flags = 0; flags < 16; ++flags)
							for (size_t capacity : { size_t{ 1 }, size_t{ 5 }, size_t{ 8 }, size_t{ 16 } })
							{
								const bool original = flags & 1, low = (flags & 2) != 0, ra = (flags & 4) != 0;
								const double signal = (flags & 8) ? 60.0 : 0.0;
								policy.SetOptions({ original, true });
								HoldContext c;
								c.budgetMs = budget; c.sourceRateHz = rate; c.signalRateHz = signal; c.displayHz = display;
								c.lowLatency = low; c.renderAheadActive = ra; c.renderAheadLeadSeconds = ra ? 0.05 : 0.0;
								c.renderAheadCreditFrames = ra ? 1 : 0; c.queueCapacity = capacity;
								const HoldPlan p = policy.PlanHold(c);
								const FrozenHold f = FrozenUpdateQueueDelayHold(budget, original, low, rate, signal, display, ra,
									c.renderAheadLeadSeconds, c.renderAheadCreditFrames, capacity);
								Assert::AreEqual(f.active, p.active);
								Assert::AreEqual(f.holdMs, p.holdMs);
								Assert::AreEqual(f.transferMs, p.transferMs);
								Assert::AreEqual(f.fraction, p.fraction);
								Assert::AreEqual(f.targetFrames, p.targetFrames);
								Assert::IsTrue(f.note == p.note);
								Assert::AreEqual(f.queueFrames, p.queueFrames);
								Assert::AreEqual(f.degraded, p.degraded);
								++cases;
							}
			Assert::IsTrue(cases > 10000);
		}

		TEST_METHOD(TargetTicksAndBacklogMatchFrozenHost)
		{
			std::mt19937 rng(20261024);
			auto u = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
			auto n = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
			VpQueueTiming::VpFixedBudgetPolicy policy;
			BacklogRunState run;
			unsigned frozenRun = 0;
			for (int i = 0; i < 200000; ++i)
			{
				const bool fixes = n(0, 3) != 0;
				policy.SetOptions({ false, fixes });
				const double rate = kRates[n(0, 9)];
				const FrozenHold fh = n(0, 2) ? FrozenUpdateQueueDelayHold(n(150, 400), false, false, rate, 0.0, 60.0, false, 0, 0, 16) : FrozenHold{};
				const HoldPlan hold = ToPlan(fh);
				TargetContext t;
				t.useFrameQueue = n(0, 9) != 0; t.capacity = n(1, 16); t.configuredDepth = n(1, 8);
				t.cadenceScaledDepth = n(1, 8); t.renderAheadCreditFrames = n(0, 2); t.lookaheadFloor = n(0, 6); t.hold = hold;
				const size_t target = policy.QueueTarget(t);
				Assert::AreEqual(FrozenPrefill(t.useFrameQueue, fh, t.cadenceScaledDepth, t.capacity, t.renderAheadCreditFrames,
					t.lookaheadFloor, fixes), target);

				DueContext d;
				d.hold = hold; d.timelineActive = n(0, 4) == 0; d.fileFrame = n(0, 9) == 0;
				d.qpcFrequency = n(0, 9) ? 10000000 : 0; d.nowQpc = 1000000000LL + n(0, 5000000);
				d.arrivalQpc = n(0, 2) ? d.nowQpc - n(0, 4000000) : 0; d.enqueueQpc = n(0, 4) ? d.nowQpc - n(0, 3000000) : 0;
				Assert::AreEqual(FrozenTicks(fh, d.timelineActive, d.fileFrame, d.arrivalQpc, d.enqueueQpc, d.nowQpc, d.qpcFrequency),
					policy.TicksUntilDue(d));

				BacklogContext b;
				b.queueBefore = n(0, 20); b.target = n(1, 12); b.oldestQueuedAgeMs = u(0, 600); b.renderCycleMs = u(0, 120);
				b.captureRateHz = rate; b.measuredSignalRateHz = n(0, 1) ? rate : 0.0; b.frontFilmHz = n(0, 3) ? 0.0 : 23.976;
				b.fileSource = n(0, 19) == 0; b.qpcAvailable = n(0, 19) != 0; b.hold = hold; b.headArrivalAgeMs = u(0, 600);
				const BacklogDecision got = policy.EvaluateBacklog(b, run);
				const FrozenBacklog want = FrozenBacklogBlock(b.queueBefore, b.target, b.oldestQueuedAgeMs, b.renderCycleMs, b.captureRateHz,
					b.measuredSignalRateHz, b.frontFilmHz, fixes, b.fileSource, b.qpcAvailable, fh, b.headArrivalAgeMs, frozenRun);
				Assert::AreEqual(want.recovered, got.recover);
				Assert::AreEqual(frozenRun, run.heldExcessRun);
				if (want.recovered)
				{
					Assert::AreEqual(want.held, got.trigger == Trigger::HeldBacklog);
					Assert::AreEqual(want.held ? want.heldDrop : std::numeric_limits<size_t>::max(), got.maxDrop);
					Assert::AreEqual(!want.held, got.restartsTiming);
				}
			}
		}

		TEST_METHOD(LadderFourThreeTwoAndCapacityCap)
		{
			VpQueueTiming::VpFixedBudgetPolicy policy;
			const std::vector<double> film = { 24000.0 / 1001.0 };
			for (int frames : { 4, 3, 2 })
			{
				HoldContext c;
				c.budgetMs = static_cast<int>(DelayBudget::BudgetForFrames(frames, film));
				c.sourceRateHz = film[0]; c.displayHz = 60.0; c.queueCapacity = 16;
				const HoldPlan p = policy.PlanHold(c);
				Assert::IsTrue(p.active);
				Assert::AreEqual(frames, p.queueFrames);
				Assert::AreEqual(frames < 4, p.degraded);
				Assert::IsTrue(p.note.empty());
			}
			HoldContext tooShort;
			tooShort.budgetMs = 60; tooShort.sourceRateHz = film[0]; tooShort.displayHz = 60.0; tooShort.queueCapacity = 16;
			Assert::IsFalse(policy.PlanHold(tooShort).active);
			Assert::IsFalse(policy.PlanHold(tooShort).note.empty());   // visible, never log-only
			// Capacity cap: 268 ms at 60p wants ~13 slots; a queue_size of 6 cuts the hold, visibly.
			HoldContext cap;
			cap.budgetMs = 268; cap.sourceRateHz = 60.0; cap.displayHz = 60.0; cap.queueCapacity = 6;
			const HoldPlan cut = policy.PlanHold(cap);
			Assert::IsTrue(cut.note.find("raise queue_size") != std::string::npos);
			Assert::IsTrue(DelayBudget::QueueCapacityFor(cut.holdMs, 1000.0 / 60.0) <= 6);
		}

		TEST_METHOD(HeldShedDropsExactlyTheLateFramesAndKeepsTiming)
		{
			VpQueueTiming::VpFixedBudgetPolicy policy;
			BacklogRunState run;
			BacklogContext b;
			b.hold.active = true; b.hold.holdMs = 100.0; b.hold.transferMs = 1000.0 / 24.0;
			b.captureRateHz = 24.0; b.measuredSignalRateHz = 24.0; b.queueBefore = 9; b.target = 4;
			b.oldestQueuedAgeMs = 0.0; b.renderCycleMs = 1.0;
			b.headArrivalAgeMs = b.hold.transferMs + b.hold.holdMs + 3.4 * (1000.0 / 24.0);   // 3.4 frames late
			const BacklogDecision d = policy.EvaluateBacklog(b, run);
			Assert::IsTrue(d.recover);
			Assert::IsTrue(d.trigger == Trigger::HeldBacklog);
			Assert::AreEqual(size_t{ 3 }, d.maxDrop);
			Assert::IsFalse(d.restartsTiming);
		}

		TEST_METHOD(HeldExcessNeedsASustainedRun)
		{
			VpQueueTiming::VpFixedBudgetPolicy policy;
			BacklogRunState run;
			BacklogContext b;
			b.hold.active = true; b.hold.holdMs = 120.0; b.captureRateHz = 60.0; b.measuredSignalRateHz = 60.0;
			b.queueBefore = 9; b.target = 7; b.renderCycleMs = 1.0;
			b.headArrivalAgeMs = 120.0 + 1.5 * (1000.0 / 60.0);   // 1.5 frames late: excess, not backlog
			const unsigned needed = 30;   // max(8, round(0.5 x 60))
			for (unsigned i = 1; i < needed; ++i)
				Assert::IsFalse(policy.EvaluateBacklog(b, run).recover);
			Assert::IsTrue(policy.EvaluateBacklog(b, run).recover);
			Assert::AreEqual(0u, run.heldExcessRun);
			// A skipped evaluation (file frame) breaks a run.
			policy.EvaluateBacklog(b, run);
			b.fileSource = true;
			policy.EvaluateBacklog(b, run);
			Assert::AreEqual(0u, run.heldExcessRun);
		}

		TEST_METHOD(AgeGateCountsFromTheFrameStartTransferOnce)
		{
			VpQueueTiming::VpFixedBudgetPolicy policy;
			DueContext d;
			d.hold.active = true; d.hold.holdMs = 150.0; d.hold.transferMs = 1000.0 / 24.0;
			d.qpcFrequency = 10000000; d.nowQpc = 50000000;
			d.arrivalQpc = d.nowQpc - 1000000;   // started 100 ms ago
			d.enqueueQpc = d.nowQpc;             // enqueued now: must NOT be used when the arrival is known
			const int64_t ticks = policy.TicksUntilDue(d);
			const double expectMs = 150.0 + 1000.0 / 24.0 - 100.0;
			Assert::AreEqual(expectMs, static_cast<double>(ticks) / 10000.0, 0.001);
			d.timelineActive = true;   // the timeline realises the hold by steering, never an age gate
			Assert::AreEqual(int64_t{ 0 }, policy.TicksUntilDue(d));
		}

		TEST_METHOD(UpstreamPolicyIsUpstreamBehaviour)
		{
			BillQueuePolicy bill;
			BacklogRunState run;
			run.heldExcessRun = 5;
			HoldContext h; h.budgetMs = 268; h.sourceRateHz = 24.0; h.displayHz = 60.0; h.queueCapacity = 16;
			Assert::IsFalse(bill.PlanHold(h).active);   // no timed hold upstream
			TargetContext t; t.configuredDepth = 9; t.capacity = 6; t.renderAheadCreditFrames = 2; t.lookaheadFloor = 5;
			Assert::AreEqual(size_t{ 6 }, bill.QueueTarget(t));   // ClampDesiredDepthToCapacity only
			DueContext d; d.hold.active = true; d.hold.holdMs = 100; d.qpcFrequency = 1; d.arrivalQpc = 1;
			Assert::AreEqual(int64_t{ 0 }, bill.TicksUntilDue(d));
			std::mt19937 rng(7);
			for (int i = 0; i < 20000; ++i)
			{
				BacklogContext b;
				b.queueBefore = rng() % 16; b.target = 1 + rng() % 8; b.oldestQueuedAgeMs = (rng() % 4000) / 10.0;
				b.renderCycleMs = (rng() % 1000) / 10.0; b.captureRateHz = kRates[rng() % 10];
				const BacklogDecision got = bill.EvaluateBacklog(b, run);
				Assert::AreEqual(AlphaQueuePolicy::ShouldRecoverBacklog(b.queueBefore, b.target, b.oldestQueuedAgeMs, b.renderCycleMs,
					b.captureRateHz), got.recover);
				Assert::IsTrue(got.restartsTiming);
			}
			Assert::AreEqual(0u, run.heldExcessRun);
		}

		TEST_METHOD(OriginalPathwayDecidesLikeUpstreamOnAFrameCountQueue)
		{
			// Today's Original pathway (VP policy, original options: budget off, queue_timing_fixes off)
			// equals upstream when the host applies no cadence scaling and no render-ahead credit -
			// the precondition for handing the Original pathway to BillQueuePolicy (separate commit).
			VpQueueTiming::VpFixedBudgetPolicy original({ true, false });
			BillQueuePolicy bill;
			BacklogRunState r1, r2;
			std::mt19937 rng(11);
			for (int i = 0; i < 20000; ++i)
			{
				HoldContext h; h.budgetMs = 100 + rng() % 300; h.sourceRateHz = kRates[rng() % 10]; h.displayHz = 60.0; h.queueCapacity = 16;
				const HoldPlan ph = original.PlanHold(h);
				Assert::IsFalse(ph.active);
				Assert::IsTrue(ph.note.empty());
				TargetContext t; t.configuredDepth = 1 + rng() % 8; t.capacity = 1 + rng() % 16;
				t.cadenceScaledDepth = AlphaQueuePolicy::ClampDesiredDepthToCapacity(t.configuredDepth, t.capacity);
				t.lookaheadFloor = rng() % 6;
				Assert::AreEqual(bill.QueueTarget(t), original.QueueTarget(t));
				BacklogContext b;
				b.queueBefore = rng() % 16; b.target = 1 + rng() % 8; b.oldestQueuedAgeMs = (rng() % 4000) / 10.0;
				b.renderCycleMs = (rng() % 1000) / 10.0; b.captureRateHz = kRates[rng() % 10]; b.measuredSignalRateHz = 59.94;
				const BacklogDecision a = original.EvaluateBacklog(b, r1), c = bill.EvaluateBacklog(b, r2);
				Assert::AreEqual(c.recover, a.recover);
				if (a.recover)
					Assert::IsTrue(a.trigger == c.trigger);
			}
		}

		TEST_METHOD(EitherOrSelection)
		{
			using VpQueueTiming::Selection;
			Assert::IsTrue(VpQueueTiming::Select(false, false, false) == Selection::Vp);           // default: ours
			Assert::IsTrue(VpQueueTiming::Select(true, false, false) == Selection::Upstream);      // user picks upstream
			Assert::IsTrue(VpQueueTiming::Select(false, true, false) == Selection::Vp);            // commit (a): Original via VP options
			Assert::IsTrue(VpQueueTiming::Select(false, true, true) == Selection::Upstream);       // after the hand-over: Original = Bill
			VpQueueTiming::VpFixedBudgetPolicy vp;
			BillQueuePolicy bill;
			const IQueueTimingPolicy* policies[] = { &vp, &bill };
			Assert::IsTrue(policies[0]->GetKind() == Kind::External);
			Assert::IsTrue(policies[1]->GetKind() == Kind::Upstream);
			Assert::IsTrue(std::string("upstream") == policies[1]->Name());
		}

		TEST_METHOD(OneWalkEqualsOldWalkAndUpstreamPreparation)
		{
			std::mt19937 rng(3);
			for (int i = 0; i < 50000; ++i)
			{
				std::vector<Q> queue(rng() % 12);
				for (Q& q : queue) q.cadenceRepeat = rng() % 4 == 0;
				QueueLookaheadWalk::WalkRequest r;
				r.activePictureConfigured = rng() % 6; r.maxActivePicture = 1 + rng() % 4; r.statisticsDepth = rng() % 8;
				r.outwardFutureFrames = rng() % 4;
				const auto walk = QueueLookaheadWalk::SelectWalk(queue, r);
				const auto old = AlphaQueuePolicy::SelectLookaheadWalk(queue, r.activePictureConfigured, r.statisticsDepth, r.maxActivePicture);
				Assert::IsTrue(old.window.indices == walk.window.indices);
				Assert::AreEqual(old.window.availableFutureFrames, walk.window.availableFutureFrames);
				Assert::AreEqual(old.window.effectiveFutureFrames, walk.window.effectiveFutureFrames);
				Assert::AreEqual(old.activePictureFrames, walk.activePictureFrames);
				Assert::IsTrue(UpstreamPreparation(queue, r.activePictureConfigured, r.maxActivePicture, r.outwardFutureFrames) == walk.preparation);
				// With no statistics observer, the active-picture view is upstream's preview exactly.
				QueueLookaheadWalk::WalkRequest plain = r;
				plain.statisticsDepth = 0;
				const auto pw = QueueLookaheadWalk::SelectWalk(queue, plain);
				Assert::IsTrue(AlphaQueuePolicy::SelectActivePicturePreview(queue, r.activePictureConfigured, r.maxActivePicture).indices ==
					QueueLookaheadWalk::ActivePictureView(pw));
			}
		}
	};
}
