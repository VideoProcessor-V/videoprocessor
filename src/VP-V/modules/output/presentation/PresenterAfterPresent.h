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

#include <atomic>
#include <cstdint>

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
		bool originalPathway = false;    // Bill's pathway: the handler does nothing
		bool rendered = false, submitted = false;
		bool targetTimingKnown = false;  // the host's presentationTargetTimingKnown
		double hostTargetLeadMs = 0.0;   // the host's presentationTargetLeadMs before this step
		bool openLoop = false;           // the extras' display rate (dejudderDisplayRate.openLoop)
		double now = 0.0;                // the steady clock (s): RenderAheadNow
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
		double targetLeadMs = 0.0;       // > 0: raise the host's presentation target lead to this
		double estimatedLeadMs = 0.0;    // as published in Presenter::estimatedLeadMs
		PresentStatistics statistics;    // the read made for the bookkeeping
		bool notedMainRefresh = false;   // the bookkeeping kept a main present's refresh
	};

	// The present block's render-ahead step (LVR present block, end of RenderLocked):
	// - extras path: a frame whose plan was not queued ends the chain; a queued one
	//   raises A's presentation target to its planned refresh (when the target is
	//   measured), and in open loop the planned lead is published as an estimate;
	// - plain path: a rendered and submitted frame ends the chain (retro gate F1: the
	//   matched-rate plan at maxExtras 0 replaces this branch in S4; not built).
	// Then the present bookkeeping: the statistics' latest present is kept as a main
	// present's refresh unless it is an extra or already the newest kept (idempotent:
	// a second note of the same present changes nothing).
	inline AfterPresentResult PresenterAfterPresent(Presenter& p, const AfterPresentFacts& f, IDXGISwapChain* native)
	{
		AfterPresentResult r;
		if (f.originalPathway)
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
				r.targetLeadMs = p.targetLeadMs;
			p.estimatedLeadMs.store(p.committed && !f.targetTimingKnown && f.openLoop ? p.targetLeadMs : 0.0,
				std::memory_order_relaxed);
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
