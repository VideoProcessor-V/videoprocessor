// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC (A20-CC order 3, series C): the render-ahead steps the renderer's Impl ran inline, as free functions in VP-V's own
// compiled file (RenderAheadHostSteps.cpp). The renderer passes its members as arguments and keeps only the calls into
// Bill's own code (fallback catalogue, swapchain recreation, RenderAheadBreak) - each step returns what to do, the host
// does it in the same order as before. Render-loop thread, as the callers. No state of its own.
#pragma once

#include "RenderAheadPolicy.h"

#include <cstddef>
#include <cstdint>

namespace RenderAhead
{
	namespace HostSteps
	{
		// The gate per source period: Presenter::UpdateGate plus its log line and the diagnostic-history decision when
		// the level changes. Returns the gate level.
		int UpdateGateLogged(Presenter& presenter, double now, const PresenterGateFrame& frame);

		// The delay-budget step (r6 fix 4) with render_ahead on: `decided` false = rates not known, the last decision
		// stands. Otherwise `credit` is the new budget credit; `changed` = the veto flips to `veto` (logged here); `why`
		// is the fallback text while vetoed.
		struct BudgetStep
		{
			bool decided = false;
			size_t credit = 0;
			bool changed = false;
			bool veto = false;
			char why[200] = {};
		};
		BudgetStep StepDelayBudget(bool vetoNow, double captureRateHz, double displayHz, size_t queueFrames, bool queueCredit);

		// The VP-owned chain's depth settle (bug hunt 05 M6): true once the depth wanted has differed from the current
		// one for kChainSettleSeconds (logged here; the caller then makes the chain again). `wantedSince` < 0 = no
		// other depth wanted.
		constexpr double kChainSettleSeconds = 1.0;
		bool ChainDepthSettled(double& wantedSince, const ChainDepth& current, const ChainDepth& wanted, double now, double ratio,
			bool lowLatency);
	}
}
