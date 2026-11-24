// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P6 rest seam rework: the Presenter's after-present step as a module-side handler body
// for the approved S5 seam (R12 AfterPresent, HA-R12-AfterPresent). It is NOT wired here:
// Phase 3 (refs/108) adds the one dispatch at the end of the renderer's present block and
// calls this body from the presentation module's handler, so this package adds no host
// line (rule 32). Until then the renderer's own block runs, and this body is checked
// against a frozen copy of it (PresenterAfterPresentTests).
//
// It also reads the DXGI frame statistics itself (refs/108 addendum 2026-11-11 23:08:
// HA-R13 PresentSample stays unwired; refs/112:24), with the same mapping as the
// renderer's ObservePresent, for the Presenter's present bookkeeping.

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "RenderAheadPolicy.h"

namespace RenderAhead
{
	// Which branch of the present block ran (Phase 3's PresentContext kind).
	enum class AfterPresentKind
	{
		AfterLastExtra,   // the de-judder extras path ran (extras presented after the main one)
		NoExtras,         // the extras path ran and presented no extra
		Plain             // the extras path did not run (no extras, or render_ahead off)
	};

	// The facts the host's present block reads that are not in the Presenter (Phase 3
	// fills them from PresentContext and the host's members, read at the seam).
	struct AfterPresentFacts
	{
		AfterPresentKind kind = AfterPresentKind::Plain;
		bool originalPathway = false;    // Bill's pathway: the handler does nothing (as with Presenter::originalPathway)
		bool rendered = false, submitted = false;
		bool targetTimingKnown = false;  // the host's presentationTargetTimingKnown
		double hostTargetLeadMs = 0.0;   // the host's presentationTargetLeadMs before this step
		bool openLoop = false;           // the extras' display rate (dejudderDisplayRate.openLoop)
		double now = 0.0;                // the steady clock (s): RenderAheadNow
		bool matchedRatePlan = false;    // P6-S4-MATCHED: a maxExtras == 0 matched-rate plan was committed for this frame
	};

	// One GetFrameStatistics read, mapped as the renderer's ObservePresent maps it
	// (PresentCount, PresentRefreshCount, SyncRefreshCount, SyncQPCTime).
	struct PresentStatistics
	{
		bool read = false;               // a swapchain was there to ask
		bool available = false;          // SUCCEEDED
		bool disjoint = false;           // DXGI_ERROR_FRAME_STATISTICS_DISJOINT
		HRESULT result = S_OK;
		uint32_t presentCount = 0, presentRefreshCount = 0, syncRefreshCount = 0;
		int64_t syncQpc = 0;
	};

	inline PresentStatistics ReadPresentStatistics(IDXGISwapChain* native)
	{
		PresentStatistics s;
		if (!native)
			return s;
		s.read = true;
		DXGI_FRAME_STATISTICS statistics{};
		s.result = native->GetFrameStatistics(&statistics);
		if (s.result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
			s.disjoint = true;
		else if (SUCCEEDED(s.result))
		{
			s.available = true;
			s.presentCount = statistics.PresentCount;
			s.presentRefreshCount = statistics.PresentRefreshCount;
			s.syncRefreshCount = statistics.SyncRefreshCount;
			s.syncQpc = statistics.SyncQPCTime.QuadPart;
		}
		return s;
	}

	// What the step did; the handler copies targetLeadMs / estimatedLeadMs / committed
	// into PresentOutcome (raise-only lead: the hub and the host take the larger).
	struct AfterPresentResult
	{
		bool ran = false;                // false in the Original pathway
		bool broke = false;              // the chain ended (Presenter::Break)
		bool committed = false;          // this frame's plan was queued
		bool raised = false;             // the host's presentation target lead is raised to targetLeadMs (Phase 3: PresentOutcome::raiseTargetLead)
		double targetLeadMs = 0.0;       // > 0: raise the host's presentation target lead to this
		double estimatedLeadMs = 0.0;    // as published in Presenter::estimatedLeadMs
		PresentStatistics statistics;    // the read made for the bookkeeping
		bool notedMainRefresh = false;   // the bookkeeping kept a main present's refresh
		bool matchedRateContinued = false; // P6-S4-MATCHED: Plain path kept the matched-rate chain alive
	};

	// P6-S4-MATCHED (retro gate F1 / presentation README): release margin for a matched-rate
	// (maxExtras == 0) plan, strictly derived from RenderAhead::kTolerance (0.05 refreshes,
	// RenderAheadPolicy.h:39 - the refresh-boundary cadence/phase tolerance shared with
	// DejudderPolicy and PhaseTolerance) times the measured displayPeriod.
	// Since 0 < kTolerance < 1, releaseAt = plannedFirstVsync - displayPeriod + margin stays
	// strictly past frame N's own vsync (clearing VBlank boundary jitter) and strictly before
	// frame N+1's planned vsync (leaving (1 - kTolerance) * displayPeriod = 0.95 * displayPeriod
	// for frame N+1), scaling cleanly with any display rate (never a fixed millisecond value).
	inline double MatchedRateReleaseMargin(double displayPeriod)
	{
		if (!(displayPeriod > 0.0) || !std::isfinite(displayPeriod))
			return 0.0;
		return kTolerance * displayPeriod;
	}

	// P6-S4-MATCHED: inputs for planning frame N+1 at matched rates (maxExtras == 0, e.g.,
	// 24/24, 50/50, 60/60) when the Presenter is active and not in the Original pathway.
	struct MatchedRateInput
	{
		bool renderAheadOn = false;      // render_ahead setting (checked via Presenter::Active)
		double now = 0.0;                // steady/QPC clock (s) after frame N's present returned
		double sourcePeriod = 0.0;       // measured source period P_s (s)
		double displayPeriod = 0.0;      // measured display period P_d (s)
		double gridVsync = 0.0;          // any observed display vsync on the same clock (s; 0 = unknown)
		double maxAhead = 0.0;           // optional FirstVsync upper bound (s; 0 = derived from chainDepth)
		uint32_t mainPresentId = 0;      // DXGI present id of frame N's main present (0 = unknown)
		double swapEndSeconds = 0.0;     // QPC seconds when frame N's Present returned (0 = use `now`)
	};

	// P6-S4-MATCHED: the matched-rate (maxExtras == 0) plan for frame N+1.
	// Plans frame N+1 on the next vsync (currentFirstVsync + displayPeriod, SyncInterval = 1,
	// extras = 0), computes releaseAt = plannedFirstVsync - displayPeriod + margin, and defines
	// the B3-Z post-present spare-work slot size postPresentSlotSeconds = max(0.0, releaseAt - now).
	struct MatchedRatePlan
	{
		bool valid = false;
		const char* reason = "";         // diagnostic reason when invalid, or "matched rate" when valid
		bool restart = false;            // chain anchored freshly (cold start, behind, too far ahead, or present gap)
		bool behind = false;             // active chain fell behind `now` (frame N was late)
		int syncInterval = 1;            // frame N+1's SyncInterval (always 1 at matched rate)
		int extras = 0;                  // extra presents planned (always 0 at matched rate)
		double currentFirstVsync = 0.0;  // frame N's planned vsync (Schedule anchor)
		double plannedFirstVsync = 0.0;  // frame N+1's planned first vsync (currentFirstVsync + displayPeriod)
		double marginSeconds = 0.0;      // MatchedRateReleaseMargin(displayPeriod)
		double releaseAt = 0.0;          // plannedFirstVsync - displayPeriod + marginSeconds
		double postPresentSlotSeconds = 0.0; // max(0.0, releaseAt - now): B3-Z post-present slot size
		double targetLeadMs = 0.0;       // max(0.0, (currentFirstVsync - swapEnd) * 1000.0)
	};

	// Pure matched-rate planner (no mutation of Presenter). Returns valid = false with an explicit
	// diagnostic reason whenever Original pathway is active, Presenter is inactive (render_ahead
	// off or budget vetoed), periods are invalid, or cadence has extras (maxExtras > 0).
	inline MatchedRatePlan PlanMatchedRate(const Presenter& p, const MatchedRateInput& in)
	{
		MatchedRatePlan plan;
		if (p.originalPathway)
		{
			plan.reason = "original pathway";
			return plan;
		}
		if (!p.Active(in.renderAheadOn))
		{
			plan.reason = p.On(in.renderAheadOn) ? "budget veto" : "render_ahead off";
			return plan;
		}
		if (!(in.sourcePeriod > 0.0) || !std::isfinite(in.sourcePeriod))
		{
			plan.reason = "invalid source period";
			return plan;
		}
		if (!(in.displayPeriod > 0.0) || !std::isfinite(in.displayPeriod))
		{
			plan.reason = "invalid display period";
			return plan;
		}
		if (!std::isfinite(in.now))
		{
			plan.reason = "invalid clock";
			return plan;
		}
		if (!std::isfinite(in.gridVsync))
		{
			plan.reason = "invalid grid vsync";
			return plan;
		}
		if (!std::isfinite(in.swapEndSeconds))
		{
			plan.reason = "invalid swap end";
			return plan;
		}
		if (!std::isfinite(in.maxAhead))
		{
			plan.reason = "invalid max ahead";
			return plan;
		}
		const double ratio = in.sourcePeriod / in.displayPeriod;
		if (!std::isfinite(ratio))
		{
			plan.reason = "invalid cadence ratio";
			return plan;
		}
		if (MaxExtras(ratio) != 0)
		{
			plan.reason = "extras cadence (maxExtras > 0)";
			return plan;
		}

		const double gridVsync = in.gridVsync > 0.0 ? in.gridVsync : 0.0;
		// Upper bound on how far ahead a real flip-chain present can land (matches
		// DejudderExtrasPresent: PlannedLead (0 at maxExtras 0) + (queueDepth + 1) * 1 * P_d).
		const int queueDepth = p.chainDepth.latency > 0 ? static_cast<int>(p.chainDepth.latency) : 2;
		const double maxAhead = in.maxAhead > 0.0 ? in.maxAhead :
			static_cast<double>(queueDepth + 1) * in.displayPeriod;

		// A present outside the plan since the last commit forces a fresh anchor.
		const bool presentGap = in.mainPresentId != 0 && p.expectedPresent != 0 &&
			in.mainPresentId != p.expectedPresent;
		bool restart = false;
		bool behind = false;
		double currentVsync = 0.0;
		if (presentGap)
		{
			restart = true;
			behind = false;
			if (gridVsync > 0.0)
			{
				const double k = std::ceil((in.now - gridVsync) / in.displayPeriod - 1e-6);
				currentVsync = gridVsync + k * in.displayPeriod;
			}
			else
				currentVsync = in.now;
		}
		else
		{
			currentVsync = p.schedule.FirstVsync(in.now, in.displayPeriod, gridVsync, restart, behind, maxAhead);
		}

		plan.valid = true;
		plan.reason = "matched rate";
		plan.restart = restart;
		plan.behind = behind;
		plan.syncInterval = 1;
		plan.extras = 0;
		plan.currentFirstVsync = currentVsync;
		plan.plannedFirstVsync = currentVsync + in.displayPeriod;
		plan.marginSeconds = MatchedRateReleaseMargin(in.displayPeriod);
		plan.releaseAt = plan.plannedFirstVsync - in.displayPeriod + plan.marginSeconds;
		plan.postPresentSlotSeconds = (std::max)(0.0, plan.releaseAt - in.now);
		const double swapEnd = in.swapEndSeconds > 0.0 ? in.swapEndSeconds : in.now;
		plan.targetLeadMs = (std::max)(0.0, (plan.currentFirstVsync - swapEnd) * 1000.0);
		return plan;
	}

	// P6-S4-MATCHED: commits a valid matched-rate (maxExtras == 0) plan to the Presenter so
	// Schedule continuity, expectedPresent, SyncInterval (1), and targetLeadMs stay alive across
	// consecutive matched-rate frames. Pair with AfterPresentFacts::matchedRatePlan = true on
	// AfterPresentKind::Plain. Returns false (leaving Presenter untouched) if invalid, Original,
	// inactive, or non-matched-rate.
	inline bool CommitMatchedRate(Presenter& p, const MatchedRatePlan& plan, const MatchedRateInput& in)
	{
		if (p.originalPathway || !plan.valid || !p.Active(in.renderAheadOn))
			return false;
		if (!(in.sourcePeriod > 0.0) || !std::isfinite(in.sourcePeriod) ||
			!(in.displayPeriod > 0.0) || !std::isfinite(in.displayPeriod))
			return false;
		const double ratio = in.sourcePeriod / in.displayPeriod;
		if (!std::isfinite(ratio) || MaxExtras(ratio) != 0)
			return false;
		if (plan.syncInterval != 1 || plan.extras != 0)
			return false;
		if (!std::isfinite(plan.currentFirstVsync) || !std::isfinite(plan.plannedFirstVsync) ||
			!std::isfinite(plan.targetLeadMs) || !(plan.plannedFirstVsync > plan.currentFirstVsync))
			return false;

		if (plan.restart)
		{
			p.RestartChain();
			++p.restarts;
		}
		if (plan.behind)
			++p.late;

		PresenterCommit c;
		c.nextFirstVsync = plan.plannedFirstVsync;
		c.nextInterval = plan.syncInterval;
		c.expectedPresent = in.mainPresentId != 0 ? in.mainPresentId + 1 : 0;
		c.lateRefreshes = 0;
		c.displayPeriod = in.displayPeriod;
		c.storeCredit = false;
		c.queueCredit = false;
		c.cadenceRatio = ratio;

		std::vector<std::pair<uint32_t, double>> plannedIds;
		if (in.mainPresentId != 0)
		{
			plannedIds.emplace_back(in.mainPresentId, plan.currentFirstVsync);
			// Cap matches Schedule::Commit's 64-entry pending bound (RenderAheadPolicy.h:633;
			// 1 main present per frame, 0 extras at matched rate).
			p.NotePresentInterval(in.mainPresentId, p.lastMainInterval, 64);
			p.lastMainInterval = 1;
		}
		p.lead = 0.0;
		p.targetLeadMs = (std::max)(0.0, plan.targetLeadMs);
		p.Commit(c, plannedIds);
		++p.frames;
		return true;
	}

	// The present block's render-ahead step (LVR present block, end of RenderLocked):
	// - extras path: a frame whose plan was not queued ends the chain; a queued one
	//   raises A's presentation target to its planned refresh (when the target is
	//   measured), and in open loop the planned lead is published as an estimate;
	// - plain path: when a matched-rate (maxExtras == 0) plan is active (f.matchedRatePlan),
	//   a rendered, submitted, committed frame keeps the Schedule chain alive, raises A's
	//   presentation target (when measured), and publishes the open-loop estimate; otherwise
	//   (uncommitted / render_ahead off / not rendered or submitted) it ends the chain.
	//   Without f.matchedRatePlan, a rendered and submitted frame ends the chain as before.
	// Then the present bookkeeping: the statistics' latest present is kept as a main
	// present's refresh unless it is an extra or already the newest kept (idempotent:
	// a second note of the same present changes nothing).
	inline AfterPresentResult PresenterAfterPresent(Presenter& p, const AfterPresentFacts& f, IDXGISwapChain* native)
	{
		AfterPresentResult r;
		if (f.originalPathway || p.originalPathway)   // the host's fact or the Presenter's (PHASE1 SetOriginalPathway)
			return r;
		r.ran = true;
		if (f.kind != AfterPresentKind::Plain)
		{
			if (!p.committed)
			{
				p.Break(f.now);
				r.broke = true;
			}
			else if (f.targetTimingKnown && p.targetLeadMs > f.hostTargetLeadMs)
			{
				r.targetLeadMs = p.targetLeadMs;
				r.raised = true;
			}
			p.estimatedLeadMs.store(p.committed && !f.targetTimingKnown && f.openLoop ? p.targetLeadMs : 0.0,
				std::memory_order_relaxed);
		}
		else if (f.matchedRatePlan)
		{
			if (!f.rendered || !f.submitted || !p.committed || !p.schedule.Active())
			{
				p.Break(f.now);
				p.committed = false;
				r.broke = true;
			}
			else
			{
				r.matchedRateContinued = true;
				if (f.targetTimingKnown && p.targetLeadMs > f.hostTargetLeadMs)
				{
					r.targetLeadMs = p.targetLeadMs;
					r.raised = true;
				}
				p.estimatedLeadMs.store(!f.targetTimingKnown && f.openLoop ? p.targetLeadMs : 0.0,
					std::memory_order_relaxed);
			}
		}
		else if (f.rendered && f.submitted)
		{
			p.Break(f.now);
			r.broke = true;
		}
		r.committed = p.committed;
		r.estimatedLeadMs = p.estimatedLeadMs.load(std::memory_order_relaxed);

		r.statistics = ReadPresentStatistics(native);
		if (r.statistics.available)
		{
			p.NoteIdsSwapchain(native);
			const size_t before = p.mainRefresh.size();
			const bool hadBack = !p.mainRefresh.empty();
			const uint32_t backId = hadBack ? p.mainRefresh.back().first : 0;
			p.NoteStatisticsPresent(r.statistics.presentCount, r.statistics.presentRefreshCount);
			r.notedMainRefresh = p.mainRefresh.size() != before ||
				(!p.mainRefresh.empty() && (!hadBack || p.mainRefresh.back().first != backId));
		}
		return r;
	}
}
