// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "PresenterExtension.h"

// RenderAheadPolicy.h before PresenterAfterPresent.h: the latter includes <windows.h> (min / max macros) and
// the Presenter's inline bodies use the plain std::min / std::max forms.
#include "RenderAheadPolicy.h"
#include "PresenterAfterPresent.h"

#include <utility>

namespace Vpv
{
	namespace Presentation
	{
		namespace RE = RendererExtension;

		std::shared_ptr<PresenterExtension::PresentEnd> PresenterExtension::EndFor(const void* host, bool create)
		{
			std::shared_ptr<PresentEnd> end = ends_.Find(host);
			if (!end && create && host)
			{
				end = std::make_shared<PresentEnd>();
				end->host = host;
				if (!ends_.Put(host, end))
					end = ends_.Find(host);   // claimed meanwhile (same renderer) or the table is full (no fact: R14 = the host's value)
			}
			return end;
		}

		// R11: the SyncInterval of the main present about to be made = the holds the last plan left after its last
		// extra (Presenter::mainInterval, set by the extras' commit), consumed here so that every exit of the render
		// consumes it (bug hunt 05 L2); clamped by TakeMainInterval. The Presenter's frame starts (no plan queued yet).
		bool PresenterExtension::OnPlanPresent(RE::FrameContext& c, RE::PresentPlan& plan)
		{
			if (const std::shared_ptr<PresentEnd> end = ends_.Find(c.host))
				end->valid.store(false, std::memory_order_release);   // a new frame: its present end is not known yet (also on the way into Original: no stale end outlives a switch)
			RenderAhead::Presenter* p = plan.presenter;
			if (!p || p->originalPathway)
				return false;   // nothing in the Original pathway: the host presents at interval 1 (Bill)
			p->BeginFrame();
			const int pending = std::exchange(p->mainInterval, 1);
			plan.interval = static_cast<uint32_t>(p->TakeMainInterval(pending));
			return true;
		}

		// R12: the render-ahead step of the host's present block (PresenterAfterPresent: the frozen copy of the host's
		// block is its test) and the present-end fact for R14. The outcome carries the raise-only lead; the host applies it.
		void PresenterExtension::OnAfterPresent(RE::FrameContext& c, RE::PresentContext& pc)
		{
			RenderAhead::Presenter* p = pc.presenter;
			if (!p || p->originalPathway)
				return;
			RenderAhead::AfterPresentFacts f;
			// The host's extras branch ran only for AfterLastExtra (rendered && submitted && de-judder active && extras
			// allowed, the condition the host and the B3-Z call share); NoExtras is the B3-Z spare's name for the host's
			// plain branch after a present, so for the Presenter it is Plain.
			f.kind = pc.kind == RE::AfterPresentKind::AfterLastExtra ?
				(pc.extrasSpentTime ? RenderAhead::AfterPresentKind::AfterLastExtra : RenderAhead::AfterPresentKind::NoExtras) :
				RenderAhead::AfterPresentKind::Plain;
			f.originalPathway = false;   // the host did not call in Original; the Presenter's own fact is checked above
			f.rendered = pc.rendered;
			f.submitted = pc.submitted;
			f.targetTimingKnown = pc.targetTimingKnown;
			f.hostTargetLeadMs = pc.hostTargetLeadMs;
			f.openLoop = pc.openLoop;
			f.now = pc.now;
			const RenderAhead::AfterPresentResult r = RenderAhead::PresenterAfterPresent(*p, f, pc.native);
			pc.outcome.committed = r.committed;
			pc.outcome.targetLeadMs = r.targetLeadMs;
			pc.outcome.raiseTargetLead = r.raised;
			pc.outcome.estimatedLeadMs = r.estimatedLeadMs;
			if (const std::shared_ptr<PresentEnd> end = EndFor(c.host, true))
			{
				end->qpc.store(pc.swapEndQpc, std::memory_order_relaxed);
				end->extrasSpentTime.store(pc.extrasSpentTime, std::memory_order_relaxed);
				end->valid.store(true, std::memory_order_release);
			}
		}

		// R14: the frame's present end. After an extras frame the queue timing measures from the MAIN present's end
		// (the extras spent time after it), as the host's `dejudderExtrasSpentTime ? mainDone : now` read did; the
		// main present's end is the swap end the host handed R12. Otherwise the host's value stands.
		void PresenterExtension::OnResolveTime(RE::FrameContext& c, RE::TimeQuery& q)
		{
			if (q.kind != RE::TimeKind::FramePresentEnd)
				return;
			const std::shared_ptr<PresentEnd> end = ends_.Find(c.host);
			if (!end || !end->valid.load(std::memory_order_acquire) || !end->extrasSpentTime.load(std::memory_order_relaxed))
				return;
			const int64_t qpc = end->qpc.load(std::memory_order_relaxed);
			if (qpc > 0)
				q.qpc = qpc;
		}

		void PresenterExtension::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown)
				ends_.Remove(f.host, true);
		}

		void RegisterPresenterExtension()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new PresenterExtension()));
		}
	}
}
