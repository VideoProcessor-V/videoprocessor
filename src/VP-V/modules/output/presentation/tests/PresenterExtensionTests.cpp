// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Phase 3 (refs/108): the presentation module's three seam handlers, called as the hub calls them
// (no renderer, no hub): R11 PlanPresent answers the main present's interval and starts the frame,
// R12 AfterPresent runs the Presenter's step and records the present end, R14 ResolveTime serves it,
// and every one of them does nothing in the Original pathway or without a Presenter.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/modules/output/presentation/PresenterExtension.h>
#include <VP-V/modules/output/presentation/PresenterAfterPresent.h>
#include <vprenderer/RendererExtension.h>

#include <cstdint>
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
			//    the estimate is left as it was (the host's else branch never touched it).
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
				Assert::AreEqual(17.0, p.estimatedLeadMs.load(), L"untouched on the plain branch");
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
			// An extras frame that spent time: the present end is the main present's swap end.
			RE::PresentContext pc;
			pc.presenter = &p;
			pc.kind = RE::AfterPresentKind::AfterLastExtra;
			pc.extrasSpentTime = true;
			pc.swapEndQpc = 777;
			pc.rendered = pc.submitted = true;
			pc.now = 100.0;
			ext.OnAfterPresent(after, pc);
			Assert::AreEqual(777ll, ask(resolve, 1000), L"after the extras: the main present's end");
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
			Assert::AreEqual(999ll, ask(resolve, 1000), L"the fact is what the host handed R12");
			// GpuReleased drops the fact.
			RE::LifecycleFacts gone;
			gone.host = &host;
			gone.kind = RE::Lifecycle::GpuReleased;
			ext.OnLifecycle(gone);
			Assert::IsTrue(ext.PresentEndOf(&host) == nullptr, L"released");
			Assert::AreEqual(1000ll, ask(resolve, 1000), L"after release: the host's value");
		}
	};
}
