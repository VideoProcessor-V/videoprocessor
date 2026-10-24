#pragma once

// Helpers around DelayBudget.h (the skeleton's total-delay rule, ADR-0020) for the places that
// SHOW or GATE the delay: the VP-V Config delay card, the delay gate and the combination runner
// (settings agent, 2026-10-24).
//
// The owner's figure "4 frames is about 250 ms at 24p" holds with these fixed costs: 2 swapchain
// refreshes at 60 Hz + one source frame of capture transfer (frame-rate matched HDMI) + 8 ms of
// processing. Whether the capture transfer belongs inside the budget is owner decision O1, so it
// is a parameter here, not a constant in the rule.

#include "DelayBudget.h"

#include <cmath>
#include <vector>

namespace DelayBudget
{
	struct Model
	{
		double refreshHz = 60.0;            // the FL35 mode
		int swapchainFrames = 2;            // libplacebo max_frame_latency
		double processingMs = 8.0;
		bool captureTransfer = true;        // one source frame (signal = source with rate matching)
		double renderAheadMs = 0.0;
	};

	inline Inputs InputsFor(double budgetMs, double sourceFps, const Model& m = Model())
	{
		Inputs in;
		in.budgetMs = budgetMs;
		in.sourceFps = sourceFps;
		in.refreshHz = m.refreshHz;
		in.swapchainFrames = m.swapchainFrames;
		in.processingMs = m.processingMs;
		in.renderAheadMs = m.renderAheadMs;
		in.captureTransferMs = m.captureTransfer && sourceFps > 0.0 ? 1000.0 / sourceFps : 0.0;
		return in;
	}

	// 23.976 / 24 / 25 / 29.97 / 30 / 50 / 59.94 / 60.
	inline std::vector<double> StandardRates()
	{
		return { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0, 50.0, 60000.0 / 1001.0, 60.0 };
	}

	// The smallest whole-millisecond total that gives `frames` queued frames at every rate in
	// `rates` (the slowest rate decides): the 4/3/2 ladder budgets. frames = kDesignDepth is the
	// RECOMMENDED setting (the same as RecommendedBudgetMs when the capture transfer is constant).
	inline double BudgetForFrames(int frames, const std::vector<double>& rates, const Model& m = Model())
	{
		double need = 0.0;
		for (double fps : rates)
		{
			if (!(fps > 0.0)) continue;
			const Inputs in = InputsFor(0.0, fps, m);
			const double fixed = (std::max)(0, in.swapchainFrames) * 1000.0 / in.refreshHz + in.renderAheadMs +
				in.processingMs + in.captureTransferMs;
			need = (std::max)(need, fixed + frames * 1000.0 / fps);
		}
		return std::ceil(need - 1e-9);
	}

	inline const char* LevelName(const Plan& p)
	{
		if (!p.feasible) return "too short";
		if (!p.degraded) return "recommended";
		return p.queueFrames >= kMinimumDepth + 1 ? "reduced look-ahead" : "minimum look-ahead";
	}

	inline double TotalMs(const Plan& p) { return p.fixedMs + p.queueMs + p.paddingMs; }
}
