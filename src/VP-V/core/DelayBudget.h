#pragma once

#include <algorithm>
#include <cmath>

// The queue as a TOTAL-DELAY BUDGET (design freeze 2026-10-24; ADR-0003; barrier scan B2).
//
// Lip sync needs VP's end-to-end video delay to be a known constant, because the AVR delays the
// audio by a fixed amount. The user sets that one total delay (ADR-0020). The old rule turned
// the target into queued frames by rounding UP (QueueLatency.h: 200 ms x 23.976 = 4.8 -> 5 frames
// = 208.5 ms of queue alone, before the swapchain, render-ahead and processing). The budget rule
// rounds DOWN after subtracting every fixed cost, so the total stays inside the budget:
//
//   queue frames = floor((budget - fixed costs) / source frame period), capped at the design
//   depth (4) and walking the 4 / 3 / 2 ladder when less fits; below 2 the budget is infeasible.
//
// OWNER DECISION 2026-10-24 (ADR-0020): the user sets ONE total delay to match the AVR; 4 queued
// frames is the RECOMMENDED setting (about 250 ms at 24p); the same total delay applies at EVERY
// frame rate (padded), so the AVR delay never changes between titles; every feature keeps working
// at a lower margin (the 4 -> 3 -> 2 ladder).
//
// Fixed costs: the swapchain depth (libplacebo max_frame_latency x refresh period), render-ahead,
// the processing/present allowance and, when the owner's definition includes it (decision O1),
// the capture transfer of one input frame. The remainder is the PADDING that makes the delay
// exactly the budget at every frame rate (owner 2026-10-24: the same total at every rate).
// Config shows queueFrames, degraded and paddingMs (the margin) for the chosen total.
//
// Pure and header-only; rates come from measurement (no fixed rates, ADR-0018).
namespace DelayBudget
{
	constexpr int kDesignDepth = 4;     // owner 2026-10-20: build for 4 queued frames
	constexpr int kMinimumDepth = 2;    // the graceful-fallback floor of the ladder

	struct Inputs
	{
		double budgetMs = 250.0;          // the user's ONE total delay (AVR setting)
		double sourceFps = 0.0;            // measured source rate
		double refreshHz = 0.0;            // measured display refresh
		int swapchainFrames = 2;           // libplacebo max_frame_latency
		double renderAheadMs = 0.0;        // counted inside the budget, never on top
		double processingMs = 0.0;         // processing + present allowance
		double captureTransferMs = 0.0;    // one input frame's transfer, if the budget covers it (O1)
	};

	struct Plan
	{
		int queueFrames = 0;       // frames to queue (0 when infeasible)
		bool feasible = false;     // at least kMinimumDepth frames fit
		bool degraded = false;     // fewer than the design depth (a "Fallbacks engaged" entry)
		double fixedMs = 0.0;      // the fixed costs subtracted
		double queueMs = 0.0;      // queueFrames x source period
		double paddingMs = 0.0;    // budget - fixed - queue (>= 0 when feasible)
	};

	inline Plan Compute(const Inputs& in)
	{
		Plan plan;
		if (!(in.sourceFps > 0.0) || !(in.budgetMs > 0.0))
			return plan;
		const double period = 1000.0 / in.sourceFps;
		const double refreshPeriod = in.refreshHz > 0.0 ? 1000.0 / in.refreshHz : 0.0;
		plan.fixedMs = (std::max)(0, in.swapchainFrames) * refreshPeriod + (std::max)(0.0, in.renderAheadMs) +
			(std::max)(0.0, in.processingMs) + (std::max)(0.0, in.captureTransferMs);
		const double available = in.budgetMs - plan.fixedMs;
		// A tiny tolerance so an exact fit (e.g. 4 x 41.6667 ms) is not lost to rounding.
		const int fit = available > 0.0 ? static_cast<int>(std::floor(available / period + 1e-9)) : 0;
		plan.queueFrames = (std::min)(fit, kDesignDepth);
		plan.feasible = plan.queueFrames >= kMinimumDepth;
		if (!plan.feasible)
		{
			plan.queueFrames = 0;
			return plan;
		}
		plan.degraded = plan.queueFrames < kDesignDepth;
		plan.queueMs = plan.queueFrames * period;
		plan.paddingMs = in.budgetMs - plan.fixedMs - plan.queueMs;
		return plan;
	}

	// The total delay that gives the design depth (4 frames) at the slowest of the rates the
	// user's sources use, rounded UP to a whole millisecond: the RECOMMENDED setting shown in
	// Config. Using it at every rate means the faster rates are padded to the same total.
	template <class Rates>
	double RecommendedBudgetMs(Inputs in, const Rates& sourceRates)
	{
		double worst = 0.0;
		for (double fps : sourceRates)
		{
			if (!(fps > 0.0))
				continue;
			in.sourceFps = fps;
			in.budgetMs = 0.0;
			const double refreshPeriod = in.refreshHz > 0.0 ? 1000.0 / in.refreshHz : 0.0;
			const double fixed = (std::max)(0, in.swapchainFrames) * refreshPeriod + (std::max)(0.0, in.renderAheadMs) +
				(std::max)(0.0, in.processingMs) + (std::max)(0.0, in.captureTransferMs);
			worst = (std::max)(worst, fixed + kDesignDepth * 1000.0 / fps);
		}
		return std::ceil(worst - 1e-9);
	}
}
