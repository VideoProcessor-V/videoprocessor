// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Out-of-line bodies of VpQueueTiming::VpFixedBudgetPolicy (see VpFixedBudgetPolicy.h).
#include <pch.h>
#include <VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>

namespace VpQueueTiming
{
	using namespace QueueTimingPolicy;

	// UpdateQueueDelayHold, d0212534 26866-26927 (the decision; the host keeps status/log).
	HoldPlan VpFixedBudgetPolicy::PlanHold(const HoldContext& c) const
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
				// Owner decision 2 (Bill merge): the configured presentation depth, never a fixed 2.
				if (c.presentationFrames > 0)
					model.swapchainFrames = c.presentationFrames;
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
	size_t VpFixedBudgetPolicy::QueueTarget(const TargetContext& c) const
	{
		if (!c.useFrameQueue)
			return 1;
		const size_t desired = c.hold.active ?
			AlphaQueuePolicy::ClampDesiredDepthToCapacity(c.hold.targetFrames, c.capacity) :
			c.cadenceScaledDepth;
		return VpQueueRules::CombineQueueTarget(desired, c.renderAheadCreditFrames, c.lookaheadFloor,
			m_queueTimingFixes.load(std::memory_order_relaxed), m_raiseQueueForAnalysis.load(std::memory_order_relaxed),
			c.capacity);
	}

	// HoldTicksUntilDueLocked, d0212534 26978-26997 + e9f27487 (the host checks the empty queue / no Impl).
	int64_t VpFixedBudgetPolicy::TicksUntilDue(const DueContext& c) const
	{
		if (!c.hold.active || c.timelineActive)
			return 0;
		const int64_t since = c.arrivalQpc != 0 ? c.arrivalQpc : c.enqueueQpc;
		if (c.fileFrame || since == 0)
			return 0;
		if (c.qpcFrequency <= 0)
			return 0;
		// A hardware FRAME-START time (audit MED 7): the frame arrives after the transfer, then waits the
		// hold - the transfer counted once. A callback or enqueue time (re-audit MED 1): the hold alone.
		const double waitMs = DelayBudget::WaitAfterTimestampMs(c.hold.holdMs, c.hold.transferMs, c.arrivalQpc != 0 && c.arrivalIsFrameStart);
		const int64_t due = since + static_cast<int64_t>(waitMs * static_cast<double>(c.qpcFrequency) / 1000.0);
		return due > c.nowQpc ? due - c.nowQpc : 0;
	}

	// RenderLoop backlog block, d0212534 31877-31914 + e9f27487 + R30 low-latency sustained excess shed.
	BacklogDecision VpFixedBudgetPolicy::EvaluateBacklog(const BacklogContext& c, BacklogRunState& run) const
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
		const bool lowLatencyEvaluated = !recovered && !c.hold.active
			&& m_lowLatency.load(std::memory_order_relaxed)
			&& !m_originalPathway.load(std::memory_order_relaxed)
			&& queueFixes && !c.fileSource && c.queueBefore > 0;
		if (!heldEvaluated && !lowLatencyEvaluated)
			run.heldExcessRun = 0;   // a skipped evaluation breaks the run
		if (heldEvaluated)
		{
			const double headAgeMs = c.headArrivalAgeMs > 0.0 ? c.headArrivalAgeMs : 0.0;
			// The transfer only for a hardware frame-start time (re-audit MED 1).
			const double heldMs = DelayBudget::WaitAfterTimestampMs(c.hold.holdMs, c.hold.transferMs, c.headArrivalIsFrameStart);
			heldBacklog = VpQueueRules::ShouldShedHeldBacklog(c.queueBefore, c.target, headAgeMs, heldMs, queueRateHz) ||
				VpQueueRules::ShouldShedHeldExcess(c.queueBefore, c.target, headAgeMs, heldMs, queueRateHz,
					run.heldExcessRun, c.hold.holdMs);
			if (heldBacklog)
				heldDrop = VpQueueRules::HeldShedFrames(c.queueBefore, c.target, headAgeMs, heldMs, queueRateHz);
			recovered = heldBacklog && heldDrop > 0;
		}
		else if (lowLatencyEvaluated)
		{
			const size_t desired = VpQueueRules::NormalizeDesiredDepth(c.target);
			if (VpQueueRules::ShouldShedLowLatencyExcess(c.queueBefore, desired, queueRateHz, run.heldExcessRun)
				&& c.queueBefore > desired)
			{
				heldBacklog = true;
				heldDrop = c.queueBefore - desired;
				recovered = true;
			}
		}
		d.recover = recovered;
		if (recovered)
		{
			d.trigger = heldBacklog ? Trigger::HeldBacklog :
				c.renderCycleMs >= VpQueueRules::RenderStallThresholdMs(c.captureRateHz) ? Trigger::RenderStall : Trigger::StaleAge;
			d.maxDrop = heldBacklog ? heldDrop : (std::numeric_limits<size_t>::max)();
			d.restartsTiming = !heldBacklog;
		}
		return d;
	}
}
