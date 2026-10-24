#pragma once

// Helpers around DelayBudget.h (the skeleton's total-delay rule, ADR-0020) for the places that
// SHOW or GATE the delay: the VP-V Config delay card, the delay gate and the combination runner
// (settings agent, 2026-10-24).
//
// The owner's figure "4 frames is about 250 ms at 24p": with these fixed costs - 2 swapchain
// refreshes at 60 Hz, one source frame of capture transfer (frame-rate matched HDMI), 8 ms of
// processing and the display phase (one refresh, measured by the delay gate 2026-10-24) - the
// recommended 4-frame value at 23.976 on 60 Hz is 267 ms. Whether the capture transfer belongs inside the budget is owner decision O1, so it
// is a parameter here, not a constant in the rule.

#include "DelayBudget.h"

#include <cmath>
#include <vector>

namespace DelayBudget
{
	// The display refresh used where VP has not measured one (the Config card before VP has run,
	// the CPU tests): the owner's FL35 mode. The renderer always passes its measured refresh.
	constexpr double kReferenceRefreshHz = 60.0;

	struct Model
	{
		double refreshHz = kReferenceRefreshHz;   // the renderer: the measured display refresh
		int swapchainFrames = kSwapchainFrames;   // libplacebo max_frame_latency
		double processingMs = kProcessingAllowanceMs;
		bool captureTransfer = true;        // one SIGNAL frame of HDMI transfer (O1)
		double renderAheadMs = 0.0;
	};

	// signalFps: the HDMI signal's rate when it differs from the content rate (a pulldown lock: the
	// queue holds film frames, but one signal frame is transferred; audit B2 2026-10-24). 0 = the
	// same as sourceFps.
	inline Inputs InputsFor(double budgetMs, double sourceFps, const Model& m = Model(), double signalFps = 0.0)
	{
		Inputs in;
		in.budgetMs = budgetMs;
		in.sourceFps = sourceFps;
		in.refreshHz = m.refreshHz;
		in.swapchainFrames = m.swapchainFrames;
		in.processingMs = m.processingMs;
		in.renderAheadMs = m.renderAheadMs;
		const double transferFps = signalFps > 0.0 ? signalFps : sourceFps;
		in.captureTransferMs = m.captureTransfer && transferFps > 0.0 ? 1000.0 / transferFps : 0.0;
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
			need = (std::max)(need, FixedCostsMs(InputsFor(0.0, fps, m)) + frames * 1000.0 / fps);
		}
		return std::ceil(need - kExactFitTolerance);
	}

	inline const char* LevelName(const Plan& p)
	{
		if (!p.feasible) return "too short";
		if (!p.degraded) return "recommended";
		return p.queueFrames >= kMinimumDepth + 1 ? "reduced look-ahead" : "minimum look-ahead";
	}

	inline double TotalMs(const Plan& p) { return p.fixedMs + p.queueMs + p.paddingMs; }
}
