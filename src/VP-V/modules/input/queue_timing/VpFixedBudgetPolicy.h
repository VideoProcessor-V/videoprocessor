#pragma once

// VP's queue/timing policy (VP-ONLY, 2026-10-24 draft): the ONE user-set total delay (ADR-0020),
// the 4/3/2 ladder, the FIFO capacity cap, the held-queue shed (gate 5/7, audit MED 6) and the age
// gate measured from the frame's START (arrivalQpc normalised, audit MED 7), behind upstream's
// QueueTimingPolicy::IQueueTimingPolicy.
//
// Commit (a) - byte-identical move: every decision below is the text of the renderer's
//   UpdateQueueDelayHold (pure part), PrefillTargetLocked, HoldTicksUntilDueLocked and the backlog
//   block of RenderLoop at d0212534, in the same order (including the || short-circuit that leaves
//   the excess run untouched when the backlog rule fires). The host keeps acting on the result
//   (EngageFallback/ReleaseFallback, logs, drops). QueueTimingPolicyTests compares it against a
//   frozen copy of that host code over a parameter sweep.
//
// Either/or (owner 2026-10-24, vp-either-or-overlaps): the host selects this policy or upstream's
// BillQueuePolicy. In commit (a) this policy also serves the Original pathway (Options::
// originalPathway: budget off, queue_timing_fixes off - exactly today's decisions); a later,
// separate commit hands the Original pathway to BillQueuePolicy (behaviour change, stated there).

#include <vprenderer/QueueTimingPolicy.h>
#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/DelayBudgetLadder.h>
#include "VpQueueRules.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace VpQueueTiming
{
	using namespace QueueTimingPolicy;

	struct Options
	{
		bool originalPathway = false;     // pipeline_original_pathway: budget off (ADR-0019)
		bool queueTimingFixes = true;     // queue_timing_fixes (H1 stale beyond target, M3 floor)
	};

	class VpFixedBudgetPolicy final : public IQueueTimingPolicy
	{
	public:
		explicit VpFixedBudgetPolicy(const Options& options = Options()) { SetOptions(options); }
		// Any thread (the settings apply, or the host right before a call from the same sources it
		// read before the move); each option is read once per decision.
		void SetOptions(const Options& options)
		{
			m_originalPathway.store(options.originalPathway, std::memory_order_relaxed);
			m_queueTimingFixes.store(options.queueTimingFixes, std::memory_order_relaxed);
		}
		Options GetOptions() const
		{
			return Options{ m_originalPathway.load(std::memory_order_relaxed), m_queueTimingFixes.load(std::memory_order_relaxed) };
		}

		Kind GetKind() const override { return Kind::External; }
		const char* Name() const override { return m_originalPathway.load(std::memory_order_relaxed) ? "vp (original pathway)" : "vp fixed delay"; }

		// UpdateQueueDelayHold, d0212534 26866-26927 (the decision; the host keeps status/log).
		HoldPlan PlanHold(const HoldContext& c) const override
		{
			int budgetMs = c.budgetMs;
			// The Original pathway keeps upstream's frame-count queue (ADR-0019; audit B2).
			if (m_originalPathway.load(std::memory_order_relaxed))
				budgetMs = 0;
			DelayBudget::Plan plan;
			double transferMs = 0.0;   // the plan's capture transfer (timestamps mark a frame's start)
			std::string budgetNote;
			if (budgetMs > 0 && c.sourceRateHz > 0.0 && !c.lowLatency)
			{
				DelayBudget::Model model;
				// The MEASURED display refresh (or the monitor's), never an assumed one (audit B2).
				const double displayHz = c.displayHz;
				if (displayHz > 0.0)
				{
					model.refreshHz = displayHz;
					if (c.renderAheadActive)
					{
						const double ps = 1.0 / c.sourceRateHz;
						const double lead = c.renderAheadLeadSeconds;
						const size_t credit = c.renderAheadCreditFrames;
						model.renderAheadMs = (std::max)(0.0, lead - static_cast<double>(credit) * ps) * 1000.0;
						budgetNote = "render-ahead on: the total across frame rates is not verified yet (PENDING-RA)";
					}
					const DelayBudget::Inputs budgetInputs = DelayBudget::InputsFor(budgetMs, c.sourceRateHz, model, c.signalRateHz);
					transferMs = budgetInputs.captureTransferMs;
					plan = DelayBudget::Compute(budgetInputs);
					if (!plan.feasible)
						budgetNote = "too short for 2 queued frames at this rate; frame-count queue";
					else
					{
						const size_t capacity = c.queueCapacity;
						const double periodMs = 1000.0 / c.sourceRateHz;
						const size_t needed = DelayBudget::QueueCapacityFor(plan.holdMs, periodMs);
						if (needed > capacity)
						{
							const double reachableMs = plan.fixedMs + DelayBudget::HoldForCapacity(capacity, periodMs);
							const DelayBudget::Plan cut = DelayBudget::Compute(DelayBudget::InputsFor(reachableMs, c.sourceRateHz, model, c.signalRateHz));
							char text[192] = {};
							std::snprintf(text, sizeof(text), "queue_size %zu holds %.0f of %d ms at %.3f Hz (needs %zu); raise queue_size", capacity,
								cut.feasible ? cut.fixedMs + cut.holdMs : 0.0, budgetMs, c.sourceRateHz, needed);
							budgetNote = text;
							plan = cut;
						}
					}
				}
				else
					budgetNote = "display refresh not known yet; frame-count queue";
			}
			HoldPlan next;
			next.active = plan.feasible;
			next.holdMs = next.active ? plan.holdMs : 0.0;
			next.transferMs = next.active ? transferMs : 0.0;
			next.fraction = next.active ? plan.holdFraction : 0.0;
			next.targetFrames = next.active ? plan.queueTargetFrames : 0;
			next.queueFrames = plan.queueFrames;
			next.degraded = plan.degraded;
			next.fixedMs = plan.fixedMs;
			next.paddingMs = plan.paddingMs;
			next.note = budgetNote;
			return next;
		}

		// PrefillTargetLocked, d0212534 32535-32551.
		size_t QueueTarget(const TargetContext& c) const override
		{
			if (!c.useFrameQueue)
				return 1;
			const size_t desired = c.hold.active ?
				AlphaQueuePolicy::ClampDesiredDepthToCapacity(c.hold.targetFrames, c.capacity) :
				c.cadenceScaledDepth;
			return VpQueueRules::CombineQueueTarget(desired, c.renderAheadCreditFrames, c.lookaheadFloor,
				m_queueTimingFixes.load(std::memory_order_relaxed));
		}

		// HoldTicksUntilDueLocked, d0212534 26978-26997 (the host checks the empty queue / no Impl).
		int64_t TicksUntilDue(const DueContext& c) const override
		{
			if (!c.hold.active || c.timelineActive)
				return 0;
			const int64_t since = c.arrivalQpc != 0 ? c.arrivalQpc : c.enqueueQpc;
			if (c.fileFrame || since == 0)
				return 0;
			if (c.qpcFrequency <= 0)
				return 0;
			// The timestamp marks the frame's START (audit MED 7): the transfer counted once.
			const int64_t due = since + static_cast<int64_t>((c.hold.transferMs + c.hold.holdMs) * static_cast<double>(c.qpcFrequency) / 1000.0);
			return due > c.nowQpc ? due - c.nowQpc : 0;
		}

		// RenderLoop backlog block, d0212534 31877-31914. measuredSignalRateHz: the host's resolved
		// signal rate (measured, else the display mode's, else the nominal capture rate).
		BacklogDecision EvaluateBacklog(const BacklogContext& c, BacklogRunState& run) const override
		{
			BacklogDecision d;
			const bool queueFixes = m_queueTimingFixes.load(std::memory_order_relaxed);
			const double queueRateHz = queueFixes ? VpQueueRules::QueueFrameRateHz(c.frontFilmHz, c.measuredSignalRateHz) :
				c.captureRateHz;
			d.queueRateHz = queueRateHz;
			bool recovered = !c.fileSource &&
				VpQueueRules::ShouldRecoverBacklog(c.queueBefore, c.target, c.oldestQueuedAgeMs, c.renderCycleMs,
					queueRateHz, queueFixes);
			bool heldBacklog = false;
			size_t heldDrop = 0;
			const bool heldEvaluated = !recovered && c.hold.active && c.qpcAvailable && !c.fileSource && c.queueBefore > 0;
			if (!heldEvaluated)
				run.heldExcessRun = 0;   // a skipped evaluation breaks the run
			if (heldEvaluated)
			{
				const double headAgeMs = c.headArrivalAgeMs > 0.0 ? c.headArrivalAgeMs : 0.0;
				const double heldMs = c.hold.transferMs + c.hold.holdMs;
				heldBacklog = VpQueueRules::ShouldShedHeldBacklog(c.queueBefore, c.target, headAgeMs, heldMs, queueRateHz) ||
					VpQueueRules::ShouldShedHeldExcess(c.queueBefore, c.target, headAgeMs, heldMs, queueRateHz,
						run.heldExcessRun, c.hold.holdMs);
				if (heldBacklog)
					heldDrop = VpQueueRules::HeldShedFrames(c.queueBefore, c.target, headAgeMs, heldMs, queueRateHz);
				recovered = heldBacklog && heldDrop > 0;
			}
			d.recover = recovered;
			if (recovered)
			{
				d.trigger = heldBacklog ? Trigger::HeldBacklog :
					c.renderCycleMs >= VpQueueRules::RenderStallThresholdMs(c.captureRateHz) ? Trigger::RenderStall : Trigger::StaleAge;
				d.maxDrop = heldBacklog ? heldDrop : std::numeric_limits<size_t>::max();
				d.restartsTiming = !heldBacklog;
			}
			return d;
		}

	private:
		std::atomic<bool> m_originalPathway{ false };
		std::atomic<bool> m_queueTimingFixes{ true };
	};

	// The either/or selection (owner 2026-10-24): queue_policy = vp (default) | upstream. The
	// Original pathway selects upstream once the separate hand-over commit lands; until then it is
	// served by VpFixedBudgetPolicy with Options::originalPathway (today's exact decisions).
	enum class Selection { Vp, Upstream };

	inline Selection Select(bool upstreamRequested, bool originalPathway, bool originalUsesUpstream)
	{
		if (upstreamRequested)
			return Selection::Upstream;
		return originalPathway && originalUsesUpstream ? Selection::Upstream : Selection::Vp;
	}

	inline const char* SelectionName(Selection s) { return s == Selection::Upstream ? "upstream" : "vp"; }
}
