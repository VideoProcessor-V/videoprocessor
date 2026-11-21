// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC series C: bodies moved verbatim (apart from member -> argument) out of LibplaceboVideoRenderer.cpp's Impl
// (UpdateRenderAheadGate's log part, UpdateRenderAheadBudget's
// decision and log, CheckRenderAheadChainDepthLocked's settle and log). See RenderAheadHostSteps.h.

// RenderAheadPolicy.h first: the headers below include <windows.h> (min / max macros) and the policy's inline bodies use
// the plain std::min / std::max forms (as PresenterExtension.cpp).
#include "RenderAheadHostSteps.h"

#include <DebugLog.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>

#include <cstdio>

namespace RenderAhead
{
	namespace HostSteps
	{
		int UpdateGateLogged(Presenter& presenter, double now, const PresenterGateFrame& frame)
		{
			// M2 P6-4: the decision is Presenter::UpdateGate. (A hold reached on start-up lateness before the first extra
			// probes one step up with the extra's cost unknown - render-ahead follow-up 2026-10-17: before, it was never left.)
			const double sourcePeriod = frame.sourcePeriod;
			const int maxExtras = frame.maxExtras;
			const PresenterGateResult gate = presenter.UpdateGate(now, frame);
			const GateInput& g = gate.input;
			const bool probing = gate.probing;
			if (gate.evaluated)
			{
				const GateChange& change = gate.change;
				if (change.changed)
					DebugLog::Log("Render-ahead gate: %s -> %s (%s%s; p95 new frame %.2f ms, extra %.2f ms (L1 %.2f), estimate ahead %.2f ms; "
						"per source frame %.2f ms of %.2f ms at u=%.2f; queue depth %d)",
						LevelName(change.from, maxExtras).c_str(), LevelName(change.to, maxExtras).c_str(),
						change.why, probing && change.to < change.from ? ", probing: the extra's cost not measured yet" : "",
						g.costs.newFrameMs, g.costs.extraMs, g.costs.extraCheaperMs, g.costs.estimateMs,
						FrameCostMs(g, change.to), 1000.0 * sourcePeriod, g.utilisation, g.queueDepth);
				// a = from level, b = to level, x = p95 new frame ms, y = extra ms.
				if (change.changed)
					DiagnosticHistory::Global().RecordDecision("ra-gate", change.why, change.from, change.to,
						g.costs.newFrameMs, g.costs.extraMs);
			}
			return gate.level;
		}

		BudgetStep StepDelayBudget(bool vetoNow, double captureRateHz, double displayHz, size_t queueFrames, bool queueCredit)
		{
			BudgetStep step;
			if (!(captureRateHz > 0.0) || !(displayHz > 0.0))
				return step;   // rates not known yet: the last decision stands
			step.decided = true;
			const BudgetDecision d = FitDelayBudget(1.0 / captureRateHz, 1.0 / displayHz, queueFrames, queueCredit);
			step.credit = d.fits ? d.creditFrames : 0;
			step.veto = !d.fits;
			step.changed = step.veto != vetoNow;
			if (step.changed)
				DebugLog::Log("Render-ahead: %s - lead %.1f ms, queue %zu frames (%.1f ms budget), with render-ahead %.1f ms, "
					"credit %zu frames (%s)", step.veto ? "OFF, outside the delay budget" : "on, inside the delay budget",
					d.leadMs, queueFrames, d.budgetMs, d.totalMs, d.creditFrames, d.reason);
			if (step.veto)
				snprintf(step.why, sizeof(step.why), "off: its %.0f ms lead does not fit the %.0f ms delay budget; %s",
					d.leadMs, d.budgetMs, d.reason);
			return step;
		}

		bool ChainDepthSettled(double& wantedSince, const ChainDepth& current, const ChainDepth& wanted, double now, double ratio,
			bool lowLatency)
		{
			if (wanted == current)
			{
				wantedSince = -1.0;
				return false;
			}
			if (wantedSince < 0.0)
			{
				wantedSince = now;
				return false;
			}
			if (now - wantedSince < kChainSettleSeconds)
				return false;
			DebugLog::Log("Render-ahead swapchain: depth %d buffers / latency %u -> %d / %u (ratio %.3f, low latency %d): making the chain again",
				current.buffers, current.latency, wanted.buffers, wanted.latency, ratio, lowLatency ? 1 : 0);
			return true;
		}
	}
}
