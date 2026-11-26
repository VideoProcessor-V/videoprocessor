#pragma once

// VP's queue/timing policy (VP-ONLY, 2026-10-24 draft): the ONE user-set total delay (ADR-0020),
// the 4/3/2 ladder, the FIFO capacity cap, the held-queue shed (gate 5/7, audit MED 6) and the age
// gate measured from the frame's START (arrivalQpc normalised, audit MED 7), behind upstream's
// QueueTimingPolicy::IQueueTimingPolicy.
//
// Commit (a) - byte-identical move: every decision below is the text of the renderer's
//   UpdateQueueDelayHold (pure part), PrefillTargetLocked, HoldTicksUntilDueLocked and the backlog
//   block of RenderLoop at d0212534 (with the re-audit MED 1 frame-start rule of integ/batch1
//   e9f27487), in the same order (including the || short-circuit that leaves
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
		bool lowLatency = false;          // R30: low-latency queue active (delay hold off; enables sustained excess shed)
		double cadenceLeadMs = 0.0;       // W23-DV-M1: locked pulldown FilmClock +cadenceLatenessMax shift (ms)
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
			m_lowLatency.store(options.lowLatency, std::memory_order_relaxed);
			m_cadenceLeadMs.store(options.cadenceLeadMs > 0.0 ? options.cadenceLeadMs : 0.0, std::memory_order_relaxed);
		}
		Options GetOptions() const
		{
			return Options{ m_originalPathway.load(std::memory_order_relaxed),
				m_queueTimingFixes.load(std::memory_order_relaxed),
				m_lowLatency.load(std::memory_order_relaxed),
				m_cadenceLeadMs.load(std::memory_order_relaxed) };
		}

		// S1 amendment (TD design (a)4/(b)6; RenderAhead::FloorPolicy::RaiseQueue): the look-ahead
		// floor may raise the queue target above the desired one (up to capacity). Default off:
		// QueueTarget is CombineQueueTarget exactly. Any thread. Set together with RenderAhead::Presenter::floorPolicy =
		// RaiseQueue (+ queueCapacity): the coupling test TheTwoRaiseQueueSwitchesAgreeOnTheRaisedQueue pins that both
		// give the same raised queue (P6-rest audit S1).
		void SetRaiseQueueForAnalysis(bool raise) { m_raiseQueueForAnalysis.store(raise, std::memory_order_relaxed); }
		bool RaiseQueueForAnalysis() const { return m_raiseQueueForAnalysis.load(std::memory_order_relaxed); }

		Kind GetKind() const override { return Kind::External; }
		const char* Name() const override { return m_originalPathway.load(std::memory_order_relaxed) ? "vp (original pathway)" : "vp fixed delay"; }

		// Bodies in VpFixedBudgetPolicy.cpp (VideoProcessor-Lib).
		HoldPlan PlanHold(const HoldContext& c) const override;
		size_t QueueTarget(const TargetContext& c) const override;
		int64_t TicksUntilDue(const DueContext& c) const override;
		BacklogDecision EvaluateBacklog(const BacklogContext& c, BacklogRunState& run) const override;

	private:
		std::atomic<bool> m_originalPathway{ false };
		std::atomic<bool> m_queueTimingFixes{ true };
		std::atomic<bool> m_lowLatency{ false };
		std::atomic<bool> m_raiseQueueForAnalysis{ false };
		std::atomic<double> m_cadenceLeadMs{ 0.0 };
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
