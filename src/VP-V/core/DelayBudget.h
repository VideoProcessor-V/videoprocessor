#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

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
// frames is the RECOMMENDED setting (about 267 ms at 24p on 60 Hz, measured); the same total delay applies at EVERY
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
	// libplacebo's swapchain depth (pl_d3d11_params::max_frame_latency, set to this by the renderer).
	constexpr int kSwapchainFrames = 2;
	// The processing + present allowance inside the budget: VP's render cycle at 24p on the
	// laptop measures 5-8 ms; the allowance is the upper end (a deliberate, documented margin).
	constexpr double kProcessingAllowanceMs = 8.0;
	// Floating-point slack so an exact fit (e.g. 4 x 41.6667 ms) is not lost to rounding.
	constexpr double kExactFitTolerance = 1e-9;

	struct Inputs
	{
		double budgetMs = 250.0;          // the user's ONE total delay (AVR setting)
		double sourceFps = 0.0;            // measured source rate
		double refreshHz = 0.0;            // measured display refresh
		int swapchainFrames = kSwapchainFrames;   // libplacebo max_frame_latency
		double renderAheadMs = 0.0;        // counted inside the budget, never on top
		double processingMs = 0.0;         // processing + present allowance
		double captureTransferMs = 0.0;    // one input frame's transfer, if the budget covers it (O1)
		// The frame shown at its refresh: once due it waits up to one refresh for its vblank, and
		// the queue's steering oscillates about it (delay-gate measurement 2026-10-24: the worst
		// case is one refresh above the hold at every rate).
		int displayPhaseRefreshes = 1;
	};

	// Every fixed cost of the total delay, in one place (the planner, the recommended value and
	// the Config delay card all use it).
	inline double FixedCostsMs(const Inputs& in)
	{
		const double refreshPeriod = in.refreshHz > 0.0 ? 1000.0 / in.refreshHz : 0.0;
		return ((std::max)(0, in.swapchainFrames) + (std::max)(0, in.displayPhaseRefreshes)) * refreshPeriod +
			(std::max)(0.0, in.renderAheadMs) + (std::max)(0.0, in.processingMs) + (std::max)(0.0, in.captureTransferMs);
	}

	struct Plan
	{
		int queueFrames = 0;       // frames to queue (0 when infeasible)
		bool feasible = false;     // at least kMinimumDepth frames fit
		bool degraded = false;     // fewer than the design depth (a "Fallbacks engaged" entry)
		double fixedMs = 0.0;      // the fixed costs subtracted
		double queueMs = 0.0;      // queueFrames x source period
		double paddingMs = 0.0;    // budget - fixed - queue (>= 0 when feasible)
		// How the queue REALISES the plan (B2 wiring, 2026-10-24). Each frame waits holdMs in VP's
		// queue (from its arrival to its dequeue): the look-ahead frames plus the padding, so the
		// total is the same at every rate: fixed costs + holdMs = the budget. The queue keeps the
		// whole frames of the hold after a dequeue (at least the look-ahead) and the sub-frame rest
		// on top: on the plain path an AGE GATE (a head leaves once it has waited holdMs), on the
		// VP timeline its queue steering (target + holdFraction; a slow proportional trim that
		// settles in a minute or two - an age gate there leaves an extra frame: delay-gate finding
		// 2026-10-24).
		double holdMs = 0.0;
		size_t queueTargetFrames = 0;   // the whole frames of the hold (at least the look-ahead)
		double holdFraction = 0.0;      // the rest, 0 .. 1 frame
	};

	// The FIFO slots a hold needs (gate 4, 2026-10-24): every frame that arrives during the hold
	// waits in the queue, plus two slots of margin (the frame arriving while the head is taken, and
	// arrival jitter; one slot overflowed in the harness at 60p).
	inline size_t QueueCapacityFor(double holdMs, double periodMs)
	{
		if (!(holdMs > 0.0) || !(periodMs > 0.0))
			return 1;
		return static_cast<size_t>(std::ceil(holdMs / periodMs - kExactFitTolerance)) + 2;
	}

	// The longest hold a FIFO of `capacity` slots keeps without overflowing (QueueCapacityFor's inverse).
	inline double HoldForCapacity(size_t capacity, double periodMs)
	{
		return capacity > 2 ? static_cast<double>(capacity - 2) * periodMs : 0.0;
	}

	inline Plan Compute(const Inputs& in)
	{
		Plan plan;
		if (!(in.sourceFps > 0.0) || !(in.budgetMs > 0.0))
			return plan;
		const double period = 1000.0 / in.sourceFps;
		plan.fixedMs = FixedCostsMs(in);
		const double available = in.budgetMs - plan.fixedMs;
		// A tiny tolerance so an exact fit (e.g. 4 x 41.6667 ms) is not lost to rounding.
		const int fit = available > 0.0 ? static_cast<int>(std::floor(available / period + kExactFitTolerance)) : 0;
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
		plan.holdMs = in.budgetMs - plan.fixedMs;
		const double holdFrames = plan.holdMs / period;
		plan.queueTargetFrames = (std::max)(static_cast<size_t>(plan.queueFrames),
			static_cast<size_t>(std::floor(holdFrames + kExactFitTolerance)));
		plan.holdFraction = (std::max)(0.0, holdFrames - static_cast<double>(plan.queueTargetFrames));
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
			worst = (std::max)(worst, FixedCostsMs(in) + kDesignDepth * 1000.0 / fps);
		}
		return std::ceil(worst - kExactFitTolerance);
	}
}
