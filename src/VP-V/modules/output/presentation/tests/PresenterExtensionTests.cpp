// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Phase 3 (refs/108): the presentation module's three seam handlers, called as the hub calls them
// (no renderer, no hub): R11 PlanPresent answers the main present's interval and starts the frame,
// R12 AfterPresent runs the Presenter's step and records the present-end fact, R14 ResolveTime keeps the host's
// main-presented stamp after an extras frame (coordinator ruling D1: byte-identical to the base timing),
// and every one of them does nothing in the Original pathway or without a Presenter.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/modules/output/presentation/PresenterExtension.h>
#include <VP-V/modules/output/presentation/PresenterAfterPresent.h>
#include <vprenderer/RendererExtension.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using RenderAhead::Presenter;
using Vpv::Presentation::PresenterExtension;
namespace RE = RendererExtension;

namespace
{
	RE::FrameContext Frame(const void* host, RE::FramePhase phase, int64_t seq = 7)
	{
		RE::FrameContext c;
		c.host = host;
		c.phase = phase;
		c.seq = seq;
		c.generation = 1;
		return c;
	}

	// A committed extras frame: the plan queued with B's interval, A's target lead known.
	void CommitFrame(Presenter& p, int nextInterval, double targetLeadMs, double now)
	{
		RenderAhead::PresenterCommit c;
		c.nextFirstVsync = now + 0.05;
		c.nextInterval = nextInterval;
		c.expectedPresent = 9;
		c.cadenceRatio = 2.5;
		const std::vector<std::pair<uint32_t, double>> ids = { { 9u, now + 0.02 } };
		p.Commit(c, ids);
		p.targetLeadMs = targetLeadMs;
	}
}

namespace VpvTests
{
	TEST_CLASS(PresenterExtensionTests)
	{
	public:
		TEST_METHOD(PlanPresentTakesTheHeldIntervalOnceAndStartsTheFrame)
		{
			PresenterExtension ext;
			Presenter p;
			int host = 0;
			p.mainInterval = 3;   // the holds the last plan left after its last extra
			p.committed = true;   // stale from the last frame: a new frame starts with no plan queued
			RE::PresentPlan plan;
			plan.presenter = &p;
			RE::FrameContext c = Frame(&host, RE::FramePhase::PlanPresent);
			Assert::IsTrue(ext.OnPlanPresent(c, plan), L"a handler decided");
			Assert::AreEqual(3u, plan.interval, L"the held interval is the main present's SyncInterval");
			Assert::AreEqual(1, p.mainInterval, L"consumed: a later present does not inherit it");
			Assert::AreEqual(3, p.lastMainInterval, L"TakeMainInterval's bookkeeping");
			Assert::AreEqual(1ull, p.intervalPresents, L"one present after holds");
			Assert::IsFalse(p.committed, L"BeginFrame: no plan queued yet");
			RE::PresentPlan again;
			again.presenter = &p;
			Assert::IsTrue(ext.OnPlanPresent(c, again));
			Assert::AreEqual(1u, again.interval, L"nothing held: interval 1");
			Assert::AreEqual(1ull, p.intervalPresents, L"an interval-1 present is not one after holds");
		}

		TEST_METHOD(PlanPresentClampsLikeTheHostDid)
		{
			PresenterExtension ext;
			Presenter p;
			int host = 0;
			p.mainInterval = RenderAhead::kMaxSyncInterval + 5;
			RE::PresentPlan plan;
			plan.presenter = &p;
			RE::FrameContext c = Frame(&host, RE::FramePhase::PlanPresent);
			ext.OnPlanPresent(c, plan);
			Assert::AreEqual(static_cast<uint32_t>(RenderAhead::kMaxSyncInterval), plan.interval, L"at most kMaxSyncInterval (TakeMainInterval)");
			p.mainInterval = 0;
			ext.OnPlanPresent(c, plan);
			Assert::AreEqual(1u, plan.interval, L"at least 1");
		}

		TEST_METHOD(EverySeamDoesNothingInTheOriginalPathwayOrWithoutAPresenter)
		{
			PresenterExtension ext;
			Presenter p;
			int host = 0;
			p.SetOriginalPathway(true, 100.0);
			p.mainInterval = 3;
			p.committed = true;
			RE::PresentPlan plan;
			plan.presenter = &p;
			RE::FrameContext c = Frame(&host, RE::FramePhase::PlanPresent);
			Assert::IsFalse(ext.OnPlanPresent(c, plan), L"Original: no decision");
			Assert::AreEqual(1u, plan.interval, L"Bill presents at SyncInterval 1");
			Assert::AreEqual(3, p.mainInterval, L"untouched");
			Assert::IsTrue(p.committed, L"untouched");
			RE::PresentContext pc;
			pc.presenter = &p;
			pc.kind = RE::AfterPresentKind::AfterLastExtra;
			pc.extrasSpentTime = true;
			pc.swapEndQpc = 123456;
			pc.rendered = pc.submitted = true;
			pc.targetTimingKnown = true;
			pc.hostTargetLeadMs = 1.0;
			pc.now = 101.0;
			RE::FrameContext after = Frame(&host, RE::FramePhase::AfterPresent);
			ext.OnAfterPresent(after, pc);
			Assert::IsFalse(pc.outcome.raiseTargetLead, L"Original: no raise");
			Assert::IsFalse(pc.outcome.committed, L"Original: nothing ran");
			Assert::IsTrue(ext.PresentEndOf(&host) == nullptr, L"Original: no present end recorded");
			RE::TimeQuery q;
			q.billQpc = q.qpc = 555;
			RE::FrameContext resolve = Frame(&host, RE::FramePhase::ResolveTime);
			ext.OnResolveTime(resolve, q);
			Assert::AreEqual(555ll, q.qpc, L"Original: the host's value");
			// No Presenter in the payload (a host that does not anchor one): nothing either.
			RE::PresentPlan none;
			Assert::IsFalse(ext.OnPlanPresent(c, none));
			Assert::AreEqual(1u, none.interval);
			RE::PresentContext noneCtx;
			noneCtx.kind = RE::AfterPresentKind::AfterLastExtra;
			ext.OnAfterPresent(after, noneCtx);
			Assert::IsFalse(noneCtx.outcome.committed);
		}

		TEST_METHOD(AfterPresentRunsThePresentersStepAndReportsTheRaiseOnlyLead)
		{
			PresenterExtension ext;
			int host = 0;
			RE::FrameContext c = Frame(&host, RE::FramePhase::AfterPresent);
			// 1. The extras ran and the plan was queued with a lead above the host's measured one: raised.
			{
				Presenter p;
				p.BeginFrame();
				CommitFrame(p, 2, 30.0, 100.0);
				RE::PresentContext pc;
				pc.presenter = &p;
				pc.kind = RE::AfterPresentKind::AfterLastExtra;
				pc.extrasSpentTime = true;
				pc.rendered = pc.submitted = true;
				pc.targetTimingKnown = true;
				pc.hostTargetLeadMs = 12.0;
				pc.openLoop = false;
				pc.now = 100.1;
				ext.OnAfterPresent(c, pc);
				Assert::IsTrue(pc.outcome.committed);
				Assert::IsTrue(pc.outcome.raiseTargetLead, L"known target, planned lead above the host's: raised");
				Assert::AreEqual(30.0, pc.outcome.targetLeadMs);
				Assert::IsTrue(p.schedule.Active(), L"the chain goes on");
				Assert::AreEqual(0.0, pc.outcome.estimatedLeadMs, L"closed loop: no estimate");
			}
			// 2. Queued, target not measured, open loop: no raise, the planned lead published as an estimate.
			{
				Presenter p;
				p.BeginFrame();
				CommitFrame(p, 1, 20.0, 100.0);
				RE::PresentContext pc;
				pc.presenter = &p;
				pc.kind = RE::AfterPresentKind::AfterLastExtra;
				pc.extrasSpentTime = true;
				pc.rendered = pc.submitted = true;
				pc.targetTimingKnown = false;
				pc.hostTargetLeadMs = -3.0;
				pc.openLoop = true;
				pc.now = 100.1;
				ext.OnAfterPresent(c, pc);
				Assert::IsFalse(pc.outcome.raiseTargetLead, L"target not known: the host's lead stands, even a negative one");
				Assert::AreEqual(20.0, pc.outcome.estimatedLeadMs);
				Assert::AreEqual(20.0, p.estimatedLeadMs.load());
			}
			// 3. The extras ran but no plan was queued: the chain ends.
			{
				Presenter p;
				CommitFrame(p, 2, 30.0, 99.0);
				p.BeginFrame();
				RE::PresentContext pc;
				pc.presenter = &p;
				pc.kind = RE::AfterPresentKind::AfterLastExtra;
				pc.rendered = pc.submitted = true;
				pc.targetTimingKnown = true;
				pc.hostTargetLeadMs = 1.0;
				pc.now = 100.1;
				ext.OnAfterPresent(c, pc);
				Assert::IsFalse(pc.outcome.committed);
				Assert::IsFalse(pc.outcome.raiseTargetLead);
				Assert::IsFalse(p.schedule.Active(), L"Break");
			}
			// 4. The host's plain branch (NoExtras for the B3-Z spare), rendered and submitted: the chain ends,
			//    the estimate is zeroed. Rule 28: CORRECTED, evidence-backed expectation (not a weakening): the plain
			//    path calls p.Break (PresenterAfterPresent.h:126), which stores 0.0 into estimatedLeadMs
			//    (RenderAheadPolicy.h:1281), the same as the pre-Phase-3 host's RenderAheadBreak(). The earlier 17.0
			//    expectation never compiled in its lane and was wrong.
			{
				Presenter p;
				CommitFrame(p, 2, 30.0, 99.0);
				p.estimatedLeadMs.store(17.0);
				RE::PresentContext pc;
				pc.presenter = &p;
				pc.kind = RE::AfterPresentKind::NoExtras;
				pc.rendered = pc.submitted = true;
				pc.now = 100.1;
				ext.OnAfterPresent(c, pc);
				Assert::IsFalse(p.schedule.Active(), L"a rendered, submitted frame without extras ends the chain");
				Assert::AreEqual(0.0, p.estimatedLeadMs.load(), L"zeroed by Break on the plain branch");
				Assert::IsFalse(pc.outcome.raiseTargetLead);
			}
			// 5. Nothing presented (Plain): nothing changes.
			{
				Presenter p;
				CommitFrame(p, 2, 30.0, 99.0);
				RE::PresentContext pc;
				pc.presenter = &p;
				pc.kind = RE::AfterPresentKind::Plain;
				pc.rendered = false;
				pc.submitted = false;
				pc.now = 100.1;
				ext.OnAfterPresent(c, pc);
				Assert::IsTrue(p.schedule.Active(), L"not presented: the chain is left alone");
			}
		}

		TEST_METHOD(ResolveTimeAnswersTheMainPresentEndOnlyAfterAnExtrasFrame)
		{
			PresenterExtension ext;
			Presenter p;
			int host = 0, other = 0;
			RE::FrameContext after = Frame(&host, RE::FramePhase::AfterPresent);
			RE::FrameContext resolve = Frame(&host, RE::FramePhase::ResolveTime);
			auto ask = [&](RE::FrameContext& c, int64_t billQpc)
			{
				RE::TimeQuery q;
				q.billQpc = q.qpc = billQpc;
				ext.OnResolveTime(c, q);
				return q.qpc;
			};
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"no frame yet: the host's value");
			// An extras frame that spent time: the present end is the host's main-presented stamp (billQpc, D1), never the
			// R12 swap end (right after the submit, earlier than Bill's read).
			RE::PresentContext pc;
			pc.presenter = &p;
			pc.kind = RE::AfterPresentKind::AfterLastExtra;
			pc.extrasSpentTime = true;
			pc.swapEndQpc = 777;
			pc.rendered = pc.submitted = true;
			pc.now = 100.0;
			ext.OnAfterPresent(after, pc);
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"after the extras: the host's main-presented stamp, not the swap end 777");
			Assert::AreEqual(1234ll, ask(resolve, 1234), L"after the extras: whatever stamp the host hands in");
			Assert::IsTrue(ext.PresentEndOf(&host) && ext.PresentEndOf(&host)->extrasSpentTime.load(), L"the extras fact is recorded at R12");
			RE::FrameContext otherResolve = Frame(&other, RE::FramePhase::ResolveTime);
			Assert::AreEqual(2000ll, ask(otherResolve, 2000), L"another renderer: its own facts only");
			RE::TimeQuery otherKind;
			otherKind.kind = static_cast<RE::TimeKind>(99);
			otherKind.billQpc = otherKind.qpc = 3000;
			ext.OnResolveTime(resolve, otherKind);
			Assert::AreEqual(3000ll, otherKind.qpc, L"only FramePresentEnd is answered");
			// The next frame starts (R11): until its R12 the present end is not known.
			RE::PresentPlan plan;
			plan.presenter = &p;
			RE::FrameContext planCtx = Frame(&host, RE::FramePhase::PlanPresent);
			ext.OnPlanPresent(planCtx, plan);
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"a new frame: the host's value again");
			// An extras frame that did not spend time, and a plain frame: the host's value.
			pc.extrasSpentTime = false;
			pc.swapEndQpc = 888;
			ext.OnAfterPresent(after, pc);
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"no extras time spent: the host's value");
			pc.kind = RE::AfterPresentKind::Plain;
			pc.extrasSpentTime = true;   // cannot happen (no extras ran), and the Presenter step ignores it
			pc.swapEndQpc = 999;
			ext.OnAfterPresent(after, pc);
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"the host's stamp, not the swap end 999 handed R12");
			// GpuReleased drops the fact.
			RE::LifecycleFacts gone;
			gone.host = &host;
			gone.kind = RE::Lifecycle::GpuReleased;
			ext.OnLifecycle(gone);
			Assert::IsTrue(ext.PresentEndOf(&host) == nullptr, L"released");
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"after release: the host's value");
		}

		// P6-S4-MATCHED (Red Team): matched-rate (maxExtras == 0) release timing across 24/24, 50/50, 60/60
		// (plus 23.976/23.976), multi-frame CommitMatchedRate -> PresenterAfterPresent chain continuity,
		// raise-only closed-loop lead / open-loop estimatedLeadMs, present-ID gap restart, and same-input
		// negative control proving unprotected Plain breaks the chain on the exact same matched-rate frame.
		TEST_METHOD(MatchedRateReleaseTimingAndChainContinuity)
		{
			const double matchedRatesHz[] = { 24.0, 50.0, 60.0, 24000.0 / 1001.0 };
			for (double hz : matchedRatesHz)
			{
				const double pd = 1.0 / hz;
				const double ps = 1.0 / hz;
				const double expectedMargin = RenderAhead::kTolerance * pd;
				Assert::AreEqual(expectedMargin, RenderAhead::MatchedRateReleaseMargin(pd), 1e-15,
					L"release margin is strictly kTolerance * displayPeriod");
				Assert::IsTrue(expectedMargin > 0.0 && expectedMargin < pd,
					L"margin stays strictly inside one refresh");

				Presenter p;
				Presenter unprotected;   // Same-input negative control (Rule 3), kept in lockstep for frames 0..1
				const double grid0 = 100.0;
				const uint32_t basePresentId = 100u;
				double expectedCurrentVsync = grid0 + pd;
				double prevPlannedFirstVsync = 0.0;

				for (int frame = 0; frame < 6; ++frame)
				{
					p.BeginFrame();
					if (frame <= 1)
						unprotected.BeginFrame();
					Assert::IsFalse(p.committed, L"BeginFrame clears committed before matched-rate commit");

					RenderAhead::MatchedRateInput in;
					in.renderAheadOn = true;
					in.sourcePeriod = ps;
					in.displayPeriod = pd;
					in.gridVsync = grid0;
					in.mainPresentId = basePresentId + static_cast<uint32_t>(frame);
					// Frame lands 0.40 refreshes before its vsync; swap returned 0.45 refreshes before.
					in.now = expectedCurrentVsync - 0.40 * pd;
					in.swapEndSeconds = frame % 2 == 0 ? (expectedCurrentVsync - 0.45 * pd) : 0.0;

					const RenderAhead::MatchedRatePlan plan = RenderAhead::PlanMatchedRate(p, in);
					Assert::IsTrue(plan.valid, L"matched-rate plan must succeed");
					Assert::AreEqual(std::string("matched rate"), std::string(plan.reason), L"diagnostic reason");
					Assert::AreEqual(1, plan.syncInterval, L"matched rate always uses SyncInterval 1");
					Assert::AreEqual(0, plan.extras, L"matched rate always plans 0 extras");
					Assert::AreEqual(frame == 0, plan.restart, L"cold start restarts once; consecutive frames stay chained");
					Assert::IsFalse(plan.behind, L"on-time frames are never behind");
					Assert::AreEqual(expectedCurrentVsync, plan.currentFirstVsync, 1e-12, L"current vsync");
					Assert::AreEqual(expectedCurrentVsync + pd, plan.plannedFirstVsync, 1e-12,
						L"next frame planned on currentFirstVsync + displayPeriod");
					if (frame > 0)
						Assert::AreEqual(prevPlannedFirstVsync, plan.currentFirstVsync, 1e-12,
							L"chained frame inherits previous plannedFirstVsync");
					Assert::AreEqual(expectedMargin, plan.marginSeconds, 1e-15, L"marginSeconds");

					const double expectedReleaseAt = plan.plannedFirstVsync - pd + expectedMargin;
					Assert::AreEqual(expectedReleaseAt, plan.releaseAt, 1e-15,
						L"releaseAt == plannedFirstVsync - displayPeriod + margin");
					Assert::IsTrue(plan.releaseAt > plan.currentFirstVsync && plan.releaseAt < plan.plannedFirstVsync,
						L"releaseAt sits strictly between frame N and frame N+1 vsyncs");

					const double expectedSlot = (std::max)(0.0, plan.releaseAt - in.now);
					Assert::AreEqual(expectedSlot, plan.postPresentSlotSeconds, 1e-12,
						L"postPresentSlotSeconds == max(0.0, releaseAt - now)");
					Assert::IsTrue(plan.postPresentSlotSeconds > 0.0, L"early frame leaves positive post-present slot");

					const double swapEnd = in.swapEndSeconds > 0.0 ? in.swapEndSeconds : in.now;
					const double expectedLeadMs = (std::max)(0.0, (plan.currentFirstVsync - swapEnd) * 1000.0);
					Assert::AreEqual(expectedLeadMs, plan.targetLeadMs, 1e-9,
						L"targetLeadMs honors explicit swapEndSeconds or falls back to now");

					// Verify PlanMatchedRate is pure (did not mutate Presenter before CommitMatchedRate).
					Assert::IsFalse(p.committed, L"PlanMatchedRate must not mutate committed");
					Assert::AreEqual(static_cast<unsigned long long>(frame), p.frames,
						L"PlanMatchedRate must not increment frame count");

					Assert::IsTrue(RenderAhead::CommitMatchedRate(p, plan, in), L"CommitMatchedRate succeeds");
					Assert::IsTrue(p.committed, L"committed set");
					Assert::AreEqual(1, p.mainInterval, L"next mainInterval is 1");
					Assert::AreEqual(0.0, p.lead, 1e-15, L"matched rate has zero extra lead");
					Assert::AreEqual(in.mainPresentId + 1u, p.expectedPresent, L"expectedPresent advances by 1");
					Assert::IsTrue(p.schedule.Active(), L"schedule stays active");
					Assert::AreEqual(plan.plannedFirstVsync, p.schedule.NextVsync(), 1e-12, L"schedule NextVsync");
					Assert::AreEqual(static_cast<unsigned long long>(frame + 1), p.frames, L"frames incremented");
					Assert::AreEqual(1ull, p.restarts, L"no spurious restarts across consecutive matched-rate frames");

					// Same-input negative control (Rule 3) on frames 0..1: drive an identical lockstep Presenter
					// through frame 0 (establishing an active chain), then on frame 1 run the pre-fix Plain path
					// (matchedRatePlan == false) on the exact same timing and prove it destroys the chain.
					if (frame == 0)
					{
						const RenderAhead::MatchedRatePlan ctrlPlan0 = RenderAhead::PlanMatchedRate(unprotected, in);
						Assert::IsTrue(ctrlPlan0.valid);
						Assert::IsTrue(RenderAhead::CommitMatchedRate(unprotected, ctrlPlan0, in));
						RenderAhead::AfterPresentFacts ctrlF0;
						ctrlF0.kind = RenderAhead::AfterPresentKind::Plain;
						ctrlF0.rendered = true;
						ctrlF0.submitted = true;
						ctrlF0.now = in.now;
						ctrlF0.matchedRatePlan = true;
						Assert::IsTrue(RenderAhead::PresenterAfterPresent(unprotected, ctrlF0, nullptr).matchedRateContinued);
						Assert::IsTrue(unprotected.schedule.Active());
					}
					else if (frame == 1)
					{
						const RenderAhead::MatchedRatePlan ctrlPlan1 = RenderAhead::PlanMatchedRate(unprotected, in);
						Assert::IsTrue(ctrlPlan1.valid);
						Assert::IsTrue(RenderAhead::CommitMatchedRate(unprotected, ctrlPlan1, in));
						RenderAhead::AfterPresentFacts unprotectedFacts;
						unprotectedFacts.kind = RenderAhead::AfterPresentKind::Plain;
						unprotectedFacts.rendered = true;
						unprotectedFacts.submitted = true;
						unprotectedFacts.targetTimingKnown = true;
						unprotectedFacts.hostTargetLeadMs = 0.5;
						unprotectedFacts.now = in.now;
						unprotectedFacts.matchedRatePlan = false;
						const RenderAhead::AfterPresentResult unprotectedRes =
							RenderAhead::PresenterAfterPresent(unprotected, unprotectedFacts, nullptr);
						Assert::IsTrue(unprotectedRes.ran, L"negative control ran");
						Assert::IsTrue(unprotectedRes.broke, L"negative control: Plain without matchedRatePlan breaks chain");
						Assert::IsFalse(unprotectedRes.matchedRateContinued, L"negative control: did not continue");
						Assert::IsFalse(unprotected.schedule.Active(), L"negative control: chain destroyed");
					}

					// Protected path: PresenterAfterPresent on Plain with matchedRatePlan == true continues chain.
					RenderAhead::AfterPresentFacts f;
					f.kind = RenderAhead::AfterPresentKind::Plain;
					f.rendered = true;
					f.submitted = true;
					f.now = in.now;
					f.matchedRatePlan = true;
					if (frame == 0)
					{
						// Closed loop, host lead below planned lead -> raise.
						f.targetTimingKnown = true;
						f.hostTargetLeadMs = plan.targetLeadMs * 0.5;
						f.openLoop = false;
					}
					else if (frame == 1)
					{
						// Closed loop, host lead already above planned lead -> raise-only (no raise).
						f.targetTimingKnown = true;
						f.hostTargetLeadMs = plan.targetLeadMs + 5.0;
						f.openLoop = false;
					}
					else if (frame == 2)
					{
						// Open loop, target timing unknown -> publish estimatedLeadMs, no raise.
						f.targetTimingKnown = false;
						f.hostTargetLeadMs = -2.0;
						f.openLoop = true;
					}
					else
					{
						f.targetTimingKnown = true;
						f.hostTargetLeadMs = 0.0;
						f.openLoop = false;
					}

					const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
					Assert::IsTrue(r.ran, L"after-present ran");
					Assert::IsFalse(r.broke, L"matched-rate Plain frame must not break the chain");
					Assert::IsTrue(r.matchedRateContinued, L"matched-rate chain continued");
					Assert::IsTrue(r.committed, L"frame remains committed");
					Assert::IsTrue(p.schedule.Active(), L"schedule stays active after Plain after-present");
					if (frame == 0)
					{
						Assert::IsTrue(r.raised, L"closed loop: raises host target lead above smaller host lead");
						Assert::AreEqual(plan.targetLeadMs, r.targetLeadMs, 1e-9);
						Assert::AreEqual(0.0, r.estimatedLeadMs, 1e-12, L"closed loop: zero estimate");
						Assert::AreEqual(0.0, p.estimatedLeadMs.load(), 1e-12);
					}
					else if (frame == 1)
					{
						Assert::IsFalse(r.raised, L"closed loop: raise-only does not lower higher host lead");
						Assert::AreEqual(0.0, r.targetLeadMs, 1e-12);
						Assert::AreEqual(0.0, r.estimatedLeadMs, 1e-12);
					}
					else if (frame == 2)
					{
						Assert::IsFalse(r.raised, L"open loop: no raise when target timing unknown");
						Assert::AreEqual(plan.targetLeadMs, r.estimatedLeadMs, 1e-9, L"open loop publishes estimate");
						Assert::AreEqual(plan.targetLeadMs, p.estimatedLeadMs.load(), 1e-9);
					}

					prevPlannedFirstVsync = plan.plannedFirstVsync;
					expectedCurrentVsync += pd;
				}

				// Present-ID gap (external present outside the plan) forces restart and re-anchors cleanly.
				p.BeginFrame();
				RenderAhead::MatchedRateInput gapIn;
				gapIn.renderAheadOn = true;
				gapIn.sourcePeriod = ps;
				gapIn.displayPeriod = pd;
				gapIn.gridVsync = grid0;
				gapIn.mainPresentId = p.expectedPresent + 3u;
				gapIn.now = expectedCurrentVsync - 0.30 * pd;
				const RenderAhead::MatchedRatePlan gapPlan = RenderAhead::PlanMatchedRate(p, gapIn);
				Assert::IsTrue(gapPlan.valid, L"present-ID gap still plans cleanly");
				Assert::IsTrue(gapPlan.restart, L"present-ID gap must trigger restart");
				Assert::IsFalse(gapPlan.behind, L"present-ID gap is a re-anchor, not lateness");
				Assert::AreEqual(expectedCurrentVsync, gapPlan.currentFirstVsync, 1e-12,
					L"present-ID gap snaps to the display vsync grid");
				Assert::IsTrue(RenderAhead::CommitMatchedRate(p, gapPlan, gapIn), L"gap commit succeeds");
				Assert::AreEqual(2ull, p.restarts, L"gap commit records the restart");
				Assert::AreEqual(gapIn.mainPresentId + 1u, p.expectedPresent, L"expectedPresent tracks post-gap id");
			}
		}

		// P6-S4-MATCHED (Red Team, Rule 3): same-input negative controls proving render_ahead OFF and
		// Original pathway never engage the matched-rate plan and preserve pre-fix / Bill behavior.
		TEST_METHOD(MatchedRateNegativeControlRenderAheadOffAndOriginalPathway)
		{
			const double pd = 1.0 / 60.0;
			const double ps = 1.0 / 60.0;
			const double grid0 = 200.0;

			// Establish a valid matched-rate plan on an active Presenter as the exact same-input reference.
			RenderAhead::MatchedRateInput baseIn;
			baseIn.renderAheadOn = true;
			baseIn.sourcePeriod = ps;
			baseIn.displayPeriod = pd;
			baseIn.gridVsync = grid0;
			baseIn.now = grid0 + 0.60 * pd;
			baseIn.mainPresentId = 50u;

			Presenter activeRef;
			const RenderAhead::MatchedRatePlan validPlan = RenderAhead::PlanMatchedRate(activeRef, baseIn);
			Assert::IsTrue(validPlan.valid, L"positive control: matched-rate plan engages when render_ahead is ON");
			Assert::IsTrue(RenderAhead::CommitMatchedRate(activeRef, validPlan, baseIn),
				L"positive control: commit succeeds");

			// Negative control 1: renderAheadOn == false on the exact same timing.
			{
				Presenter p;
				// Seed non-trivial state to prove CommitMatchedRate leaves Presenter untouched when declined.
				CommitFrame(p, 2, 18.0, baseIn.now - 0.1);
				p.BeginFrame();
				p.expectedPresent = 49u;
				const bool activeBefore = p.schedule.Active();
				const double nextBefore = p.schedule.NextVsync();
				const int intervalBefore = p.mainInterval;
				const double leadBefore = p.targetLeadMs;
				const unsigned long long framesBefore = p.frames;
				const unsigned long long restartsBefore = p.restarts;

				RenderAhead::MatchedRateInput offIn = baseIn;
				offIn.renderAheadOn = false;
				const RenderAhead::MatchedRatePlan offPlan = RenderAhead::PlanMatchedRate(p, offIn);
				Assert::IsFalse(offPlan.valid, L"render_ahead OFF: PlanMatchedRate must decline");
				Assert::IsTrue(offPlan.reason != nullptr && offPlan.reason[0] != '\0', L"non-empty decline reason");
				Assert::AreEqual(std::string("render_ahead off"), std::string(offPlan.reason));

				// Even if handed the valid plan from the active control, CommitMatchedRate must refuse.
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, validPlan, offIn),
					L"render_ahead OFF: CommitMatchedRate must decline and leave Presenter untouched");
				Assert::AreEqual(activeBefore, p.schedule.Active());
				Assert::AreEqual(nextBefore, p.schedule.NextVsync(), 1e-12);
				Assert::AreEqual(intervalBefore, p.mainInterval);
				Assert::AreEqual(49u, p.expectedPresent);
				Assert::AreEqual(leadBefore, p.targetLeadMs, 1e-12);
				Assert::AreEqual(framesBefore, p.frames);
				Assert::AreEqual(restartsBefore, p.restarts);
				Assert::IsFalse(p.committed);

				// PresenterAfterPresent with matchedRatePlan == false breaks the chain (pre-fix behavior).
				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = true;
				f.submitted = true;
				f.now = baseIn.now;
				f.matchedRatePlan = false;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsTrue(r.ran);
				Assert::IsTrue(r.broke, L"uncommitted/off Plain frame breaks chain as pre-fix");
				Assert::IsFalse(r.matchedRateContinued);
				Assert::IsFalse(r.committed);
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(0.0, p.estimatedLeadMs.load(), 1e-12);
			}

			// Negative control 2: originalPathway == true on the exact same timing.
			{
				Presenter p;
				p.SetOriginalPathway(true, baseIn.now - 0.1);
				p.mainInterval = 3;
				p.targetLeadMs = 15.0;
				p.expectedPresent = 49u;

				const RenderAhead::MatchedRatePlan origPlan = RenderAhead::PlanMatchedRate(p, baseIn);
				Assert::IsFalse(origPlan.valid, L"Original pathway: PlanMatchedRate must decline");
				Assert::AreEqual(std::string("original pathway"), std::string(origPlan.reason));
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, validPlan, baseIn),
					L"Original pathway: CommitMatchedRate must decline");
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(3, p.mainInterval, L"Original pathway: Presenter untouched");
				Assert::AreEqual(15.0, p.targetLeadMs, 1e-12);
				Assert::AreEqual(49u, p.expectedPresent);
				Assert::AreEqual(0ull, p.frames);

				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = true;
				f.submitted = true;
				f.now = baseIn.now;
				f.matchedRatePlan = true;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsFalse(r.ran, L"Original pathway (Presenter flag): PresenterAfterPresent does nothing");
				Assert::IsFalse(r.broke);
				Assert::IsFalse(r.matchedRateContinued);
				Assert::IsFalse(r.committed);

				// Also verify host-side AfterPresentFacts::originalPathway == true gates an active Presenter.
				Presenter activeHostOrig;
				Assert::IsTrue(RenderAhead::CommitMatchedRate(activeHostOrig, validPlan, baseIn));
				Assert::IsTrue(activeHostOrig.schedule.Active());
				RenderAhead::AfterPresentFacts hostOrig = f;
				hostOrig.originalPathway = true;
				const RenderAhead::AfterPresentResult rHost =
					RenderAhead::PresenterAfterPresent(activeHostOrig, hostOrig, nullptr);
				Assert::IsFalse(rHost.ran, L"Original pathway (facts flag): PresenterAfterPresent does nothing");
				Assert::IsFalse(rHost.broke);
				Assert::IsFalse(rHost.matchedRateContinued);
				Assert::IsTrue(activeHostOrig.schedule.Active(), L"chain untouched when gated by Original");
			}

			// Retro gate F2 check: TD on with renderAheadOn == false activates matched-rate planning,
			// and budgetVeto while TD is on reports "budget veto" rather than "render_ahead off".
			{
				Presenter tdOnly;
				tdOnly.SetTemporalDetailOn(true, false);
				RenderAhead::MatchedRateInput tdIn = baseIn;
				tdIn.renderAheadOn = false;
				const RenderAhead::MatchedRatePlan tdPlan = RenderAhead::PlanMatchedRate(tdOnly, tdIn);
				Assert::IsTrue(tdPlan.valid, L"TD on activates Presenter even with renderAheadOn == false");
				tdOnly.budgetVeto = true;
				const RenderAhead::MatchedRatePlan tdVeto = RenderAhead::PlanMatchedRate(tdOnly, tdIn);
				Assert::IsFalse(tdVeto.valid, L"budget veto still blocks matched-rate plan with TD on");
				Assert::AreEqual(std::string("budget veto"), std::string(tdVeto.reason));
			}
		}

		// P6-S4-MATCHED (Red Team, Rule 3): extras cadences (maxExtras > 0) must never be hijacked by
		// PlanMatchedRate / CommitMatchedRate, contrasted against a matched-rate cadence on the same Presenter.
		TEST_METHOD(MatchedRateNegativeControlDeclinesExtrasCadences)
		{
			struct CadenceCase { double sourceHz; double displayHz; };
			const CadenceCase extrasCases[] = {
				{ 24.0, 120.0 },            // ratio = 5.0, maxExtras = 4
				{ 24.0, 60.0 },             // ratio = 2.5, maxExtras = 2
				{ 30.0, 60.0 },             // ratio = 2.0, maxExtras = 1
				{ 24.0, 30.0 },             // ratio = 1.25, maxExtras = 1
				{ 24000.0 / 1001.0, 60.0 }  // ratio ~= 2.5025, maxExtras = 2
			};

			Presenter p;
			// Build a valid-looking forged MatchedRatePlan to prove CommitMatchedRate re-validates cadence.
			RenderAhead::MatchedRatePlan forgedPlan;
			forgedPlan.valid = true;
			forgedPlan.reason = "matched rate";
			forgedPlan.syncInterval = 1;
			forgedPlan.extras = 0;
			forgedPlan.currentFirstVsync = 100.05;
			forgedPlan.plannedFirstVsync = 100.05 + 1.0 / 60.0;
			forgedPlan.targetLeadMs = 5.0;

			for (const CadenceCase& tc : extrasCases)
			{
				const double ps = 1.0 / tc.sourceHz;
				const double pd = 1.0 / tc.displayHz;
				Assert::IsTrue(RenderAhead::MaxExtras(ps / pd) > 0, L"test case must be an extras cadence");

				RenderAhead::MatchedRateInput in;
				in.renderAheadOn = true;
				in.sourcePeriod = ps;
				in.displayPeriod = pd;
				in.gridVsync = 100.0;
				in.now = 100.0 + 0.5 * pd;
				in.mainPresentId = 10u;

				const RenderAhead::MatchedRatePlan plan = RenderAhead::PlanMatchedRate(p, in);
				Assert::IsFalse(plan.valid, L"extras cadence must be declined by PlanMatchedRate");
				Assert::AreEqual(std::string("extras cadence (maxExtras > 0)"), std::string(plan.reason));
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, forgedPlan, in),
					L"CommitMatchedRate must refuse extras cadences even with a forged valid plan");
				Assert::IsFalse(p.schedule.Active(), L"Presenter schedule untouched");
				Assert::IsFalse(p.committed, L"Presenter committed untouched");
				Assert::AreEqual(0ull, p.frames, L"Presenter frames untouched");
			}

			// Same-Presenter positive control: switching to matched 60/60 (maxExtras == 0) immediately succeeds.
			RenderAhead::MatchedRateInput matchedIn;
			matchedIn.renderAheadOn = true;
			matchedIn.sourcePeriod = 1.0 / 60.0;
			matchedIn.displayPeriod = 1.0 / 60.0;
			matchedIn.gridVsync = 100.0;
			matchedIn.now = 100.0 + 0.5 / 60.0;
			matchedIn.mainPresentId = 10u;
			const RenderAhead::MatchedRatePlan matchedPlan = RenderAhead::PlanMatchedRate(p, matchedIn);
			Assert::IsTrue(matchedPlan.valid, L"same Presenter succeeds once cadence is matched-rate");
			Assert::IsTrue(RenderAhead::CommitMatchedRate(p, matchedPlan, matchedIn));
			Assert::IsTrue(p.schedule.Active());
		}

		// P6-S4-MATCHED (Red Team): adversarial late-frame clamping (now >= releaseAt -> postPresentSlotSeconds == 0.0,
		// never negative), invalid/non-finite periods & clocks, budgetVeto == true, and tampered plan rejection.
		TEST_METHOD(MatchedRateAdversarialLateFrameInvalidInputsAndBudgetVeto)
		{
			const double pd = 1.0 / 60.0;
			const double ps = 1.0 / 60.0;
			const double grid0 = 300.0;
			const double margin = RenderAhead::MatchedRateReleaseMargin(pd);

			// 1. Late-frame clamping on an active 60/60 chain.
			{
				Presenter p;
				RenderAhead::MatchedRateInput frame0;
				frame0.renderAheadOn = true;
				frame0.sourcePeriod = ps;
				frame0.displayPeriod = pd;
				frame0.gridVsync = grid0;
				frame0.now = grid0 + 0.50 * pd;
				frame0.mainPresentId = 1u;
				const RenderAhead::MatchedRatePlan plan0 = RenderAhead::PlanMatchedRate(p, frame0);
				Assert::IsTrue(plan0.valid);
				Assert::IsTrue(RenderAhead::CommitMatchedRate(p, plan0, frame0));
				const double v1 = plan0.plannedFirstVsync;

				// Positive control: early frame (now < releaseAt) yields strictly positive slot.
				RenderAhead::MatchedRateInput earlyIn = frame0;
				earlyIn.mainPresentId = 2u;
				earlyIn.now = v1 - 0.30 * pd;
				const RenderAhead::MatchedRatePlan earlyPlan = RenderAhead::PlanMatchedRate(p, earlyIn);
				Assert::IsTrue(earlyPlan.valid && !earlyPlan.restart && !earlyPlan.behind);
				Assert::AreEqual(0.35 * pd, earlyPlan.postPresentSlotSeconds, 1e-12,
					L"control: early frame has positive post-present slot");
				Assert::IsTrue(earlyPlan.targetLeadMs > 0.0);

				// Exact boundary: now == releaseAt (within FirstVsync's 0.25-refresh grace window).
				RenderAhead::MatchedRateInput atBoundaryIn = earlyIn;
				atBoundaryIn.now = earlyPlan.releaseAt;
				const RenderAhead::MatchedRatePlan boundaryPlan = RenderAhead::PlanMatchedRate(p, atBoundaryIn);
				Assert::IsTrue(boundaryPlan.valid && !boundaryPlan.restart && !boundaryPlan.behind);
				Assert::IsTrue(boundaryPlan.postPresentSlotSeconds >= 0.0, L"never negative at boundary");
				Assert::AreEqual(0.0, boundaryPlan.postPresentSlotSeconds, 1e-12,
					L"now == releaseAt gives zero post-present slot");
				Assert::AreEqual(0.0, boundaryPlan.targetLeadMs, 1e-12, L"swap after vsync clamps lead to 0");

				// Strictly past releaseAt (raw releaseAt - now == -0.10 * pd < 0, still inside 0.25-refresh window).
				RenderAhead::MatchedRateInput pastReleaseIn = earlyIn;
				pastReleaseIn.now = v1 + margin + 0.10 * pd;
				const RenderAhead::MatchedRatePlan pastPlan = RenderAhead::PlanMatchedRate(p, pastReleaseIn);
				Assert::IsTrue(pastPlan.valid && !pastPlan.restart && !pastPlan.behind);
				Assert::IsTrue(pastPlan.releaseAt < pastReleaseIn.now, L"precondition: now strictly past releaseAt");
				Assert::AreEqual(0.0, pastPlan.postPresentSlotSeconds,
					L"now > releaseAt must clamp postPresentSlotSeconds to exact 0.0 (never negative)");
				Assert::AreEqual(0.0, pastPlan.targetLeadMs,
					L"now > currentFirstVsync must clamp targetLeadMs to exact 0.0 (never negative)");

				// Very late frame (beyond FirstVsync's 0.25-refresh grace window): behind && restart.
				RenderAhead::MatchedRateInput veryLateIn = earlyIn;
				veryLateIn.now = v1 + 0.40 * pd;
				const RenderAhead::MatchedRatePlan latePlan = RenderAhead::PlanMatchedRate(p, veryLateIn);
				Assert::IsTrue(latePlan.valid);
				Assert::IsTrue(latePlan.restart && latePlan.behind, L"chain fell behind now: restart + behind");
				Assert::IsTrue(latePlan.postPresentSlotSeconds >= 0.0, L"slot never negative on behind restart");
				Assert::IsTrue(RenderAhead::CommitMatchedRate(p, latePlan, veryLateIn));
				Assert::AreEqual(2ull, p.restarts, L"behind restart counted");
				Assert::AreEqual(1ull, p.late, L"behind frame counted in Presenter::late");
			}

			// 2. Invalid/non-finite periods and clocks cleanly decline with non-empty diagnostic reasons.
			{
				const double nan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();
				const double badPeriods[] = { 0.0, -pd, nan, inf, -inf };

				Presenter p;
				RenderAhead::MatchedRateInput good;
				good.renderAheadOn = true;
				good.sourcePeriod = ps;
				good.displayPeriod = pd;
				good.gridVsync = grid0;
				good.now = grid0 + 0.5 * pd;
				good.mainPresentId = 1u;
				const RenderAhead::MatchedRatePlan goodPlan = RenderAhead::PlanMatchedRate(p, good);
				Assert::IsTrue(goodPlan.valid, L"control: good input succeeds");

				for (double bad : badPeriods)
				{
					Assert::AreEqual(0.0, RenderAhead::MatchedRateReleaseMargin(bad),
						L"invalid displayPeriod gives 0.0 margin");

					RenderAhead::MatchedRateInput badDp = good;
					badDp.displayPeriod = bad;
					const RenderAhead::MatchedRatePlan rDp = RenderAhead::PlanMatchedRate(p, badDp);
					Assert::IsFalse(rDp.valid, L"invalid displayPeriod must decline");
					Assert::AreEqual(std::string("invalid display period"), std::string(rDp.reason));
					Assert::IsFalse(RenderAhead::CommitMatchedRate(p, goodPlan, badDp),
						L"CommitMatchedRate rejects invalid displayPeriod");

					RenderAhead::MatchedRateInput badSp = good;
					badSp.sourcePeriod = bad;
					const RenderAhead::MatchedRatePlan rSp = RenderAhead::PlanMatchedRate(p, badSp);
					Assert::IsFalse(rSp.valid, L"invalid sourcePeriod must decline");
					Assert::AreEqual(std::string("invalid source period"), std::string(rSp.reason));
					Assert::IsFalse(RenderAhead::CommitMatchedRate(p, goodPlan, badSp),
						L"CommitMatchedRate rejects invalid sourcePeriod");
				}

				const double nonFinite[] = { nan, inf, -inf };
				for (double nf : nonFinite)
				{
					RenderAhead::MatchedRateInput badNow = good;
					badNow.now = nf;
					const RenderAhead::MatchedRatePlan rNow = RenderAhead::PlanMatchedRate(p, badNow);
					Assert::IsFalse(rNow.valid, L"non-finite now must decline");
					Assert::IsTrue(rNow.reason != nullptr && rNow.reason[0] != '\0');

					RenderAhead::MatchedRateInput badGrid = good;
					badGrid.gridVsync = nf;
					const RenderAhead::MatchedRatePlan rGrid = RenderAhead::PlanMatchedRate(p, badGrid);
					Assert::IsFalse(rGrid.valid, L"non-finite gridVsync must decline");
					Assert::IsTrue(rGrid.reason != nullptr && rGrid.reason[0] != '\0');

					RenderAhead::MatchedRateInput badSwap = good;
					badSwap.swapEndSeconds = nf;
					const RenderAhead::MatchedRatePlan rSwap = RenderAhead::PlanMatchedRate(p, badSwap);
					Assert::IsFalse(rSwap.valid, L"non-finite swapEndSeconds must decline");
					Assert::IsTrue(rSwap.reason != nullptr && rSwap.reason[0] != '\0');

					RenderAhead::MatchedRateInput badMax = good;
					badMax.maxAhead = nf;
					const RenderAhead::MatchedRatePlan rMax = RenderAhead::PlanMatchedRate(p, badMax);
					Assert::IsFalse(rMax.valid, L"non-finite maxAhead must decline");
					Assert::IsTrue(rMax.reason != nullptr && rMax.reason[0] != '\0');
				}

				// 3. budgetVeto == true declines cleanly and leaves Presenter untouched; clearing veto restores it.
				p.budgetVeto = true;
				const RenderAhead::MatchedRatePlan vetoPlan = RenderAhead::PlanMatchedRate(p, good);
				Assert::IsFalse(vetoPlan.valid, L"budgetVeto must decline");
				Assert::AreEqual(std::string("budget veto"), std::string(vetoPlan.reason));
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, goodPlan, good),
					L"CommitMatchedRate must refuse when budgetVeto is active");
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(0ull, p.frames);

				p.budgetVeto = false;
				Assert::IsTrue(RenderAhead::PlanMatchedRate(p, good).valid,
					L"clearing budgetVeto restores matched-rate planning on the exact same input");

				// 4. Tampered/inconsistent plans rejected by CommitMatchedRate.
				RenderAhead::MatchedRatePlan badInterval = goodPlan;
				badInterval.syncInterval = 2;
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, badInterval, good), L"syncInterval != 1 rejected");

				RenderAhead::MatchedRatePlan badExtras = goodPlan;
				badExtras.extras = 1;
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, badExtras, good), L"extras != 0 rejected");

				RenderAhead::MatchedRatePlan badOrder = goodPlan;
				badOrder.plannedFirstVsync = badOrder.currentFirstVsync;
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, badOrder, good),
					L"plannedFirstVsync <= currentFirstVsync rejected");

				RenderAhead::MatchedRatePlan badLead = goodPlan;
				badLead.targetLeadMs = nan;
				Assert::IsFalse(RenderAhead::CommitMatchedRate(p, badLead, good), L"non-finite targetLeadMs rejected");
				Assert::IsFalse(p.schedule.Active(), L"no tampered plan mutated Presenter");
				Assert::AreEqual(0ull, p.frames);
			}
		}

		// P6-S4-MATCHED (Red Team): AfterPresentKind::Plain with matchedRatePlan == true must cleanly break
		// the chain whenever the frame was not rendered, not submitted, not committed, or schedule is inactive.
		TEST_METHOD(MatchedRatePlainAfterPresentBreaksWhenUnrenderedUnsubmittedOrUncommitted)
		{
			const double pd = 1.0 / 60.0;
			const double ps = 1.0 / 60.0;
			const double grid0 = 400.0;

			auto initCommittedPresenter = [&](Presenter& p)
			{
				RenderAhead::MatchedRateInput in;
				in.renderAheadOn = true;
				in.sourcePeriod = ps;
				in.displayPeriod = pd;
				in.gridVsync = grid0;
				in.now = grid0 + 0.50 * pd;
				in.mainPresentId = 7u;
				const RenderAhead::MatchedRatePlan plan = RenderAhead::PlanMatchedRate(p, in);
				Assert::IsTrue(plan.valid);
				Assert::IsTrue(RenderAhead::CommitMatchedRate(p, plan, in));
				p.estimatedLeadMs.store(9.0);
			};

			// Positive control: rendered && submitted && committed && schedule.Active() continues the chain.
			{
				Presenter p;
				initCommittedPresenter(p);
				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = true;
				f.submitted = true;
				f.now = grid0 + 0.55 * pd;
				f.matchedRatePlan = true;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsTrue(r.ran);
				Assert::IsFalse(r.broke);
				Assert::IsTrue(r.matchedRateContinued);
				Assert::IsTrue(r.committed);
				Assert::IsTrue(p.schedule.Active());
			}

			// Abort case 1: !f.rendered
			{
				Presenter p;
				initCommittedPresenter(p);
				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = false;
				f.submitted = true;
				f.now = grid0 + 0.55 * pd;
				f.matchedRatePlan = true;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsTrue(r.ran);
				Assert::IsTrue(r.broke, L"!rendered must break matched-rate chain");
				Assert::IsFalse(r.matchedRateContinued);
				Assert::IsFalse(r.committed);
				Assert::IsFalse(p.committed);
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(0.0, p.estimatedLeadMs.load(), 1e-12, L"Break clears stale estimate");
			}

			// Abort case 2: !f.submitted
			{
				Presenter p;
				initCommittedPresenter(p);
				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = true;
				f.submitted = false;
				f.now = grid0 + 0.55 * pd;
				f.matchedRatePlan = true;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsTrue(r.ran);
				Assert::IsTrue(r.broke, L"!submitted must break matched-rate chain");
				Assert::IsFalse(r.matchedRateContinued);
				Assert::IsFalse(r.committed);
				Assert::IsFalse(p.committed);
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(0.0, p.estimatedLeadMs.load(), 1e-12);
			}

			// Abort case 3: !p.committed (BeginFrame ran without CommitMatchedRate)
			{
				Presenter p;
				initCommittedPresenter(p);
				p.BeginFrame();
				Assert::IsFalse(p.committed);
				Assert::IsTrue(p.schedule.Active());
				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = true;
				f.submitted = true;
				f.now = grid0 + 0.55 * pd;
				f.matchedRatePlan = true;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsTrue(r.ran);
				Assert::IsTrue(r.broke, L"!committed must break matched-rate chain");
				Assert::IsFalse(r.matchedRateContinued);
				Assert::IsFalse(r.committed);
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(0.0, p.estimatedLeadMs.load(), 1e-12);
			}

			// Abort case 4: !p.schedule.Active() (chain restarted/cleared even though p.committed was set)
			{
				Presenter p;
				initCommittedPresenter(p);
				p.RestartChain();
				Assert::IsTrue(p.committed);
				Assert::IsFalse(p.schedule.Active());
				RenderAhead::AfterPresentFacts f;
				f.kind = RenderAhead::AfterPresentKind::Plain;
				f.rendered = true;
				f.submitted = true;
				f.now = grid0 + 0.55 * pd;
				f.matchedRatePlan = true;
				const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(p, f, nullptr);
				Assert::IsTrue(r.ran);
				Assert::IsTrue(r.broke, L"inactive schedule must break matched-rate chain");
				Assert::IsFalse(r.matchedRateContinued);
				Assert::IsFalse(r.committed);
				Assert::IsFalse(p.committed);
				Assert::IsFalse(p.schedule.Active());
				Assert::AreEqual(0.0, p.estimatedLeadMs.load(), 1e-12);
			}
		}
	};
}
