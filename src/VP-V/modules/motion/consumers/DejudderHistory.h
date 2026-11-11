// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-7 (doc 50 Q4 / R2, doc 59 section 2.2): the de-judder's `motion-history` reader action at its apply
// point (the renderer's ApplyMotionInvalidate: release, or forget the matcher history / estimate-ahead / mask
// slots / estimated pair and the history holders).
//
// The R2 switch is resync_event: off / shadow (and on while no motion-history reader is registered on the
// FrameSeal) = TODAY: the action is the one vectors-valid state's epoch move (MotionValidity::Pending), the
// pending is left exactly as consumed. On with the reader registered: the reader's pending (taken here, once
// per apply point) also acts - its release adds to the pending's, and a rebase-axis change alone (no epoch
// move) forgets as an epoch move would (conservative: the de-judder holds no re-based-plane-only state that
// could be kept). W1-C2a MUST 2: the readers taken here are VectorEngineState::motionResyncReaders, the ones the
// resync "motion" provider registers on each era's Indicator (VectorEngine::RegisterResyncFor); with resync_event on the
// reader drives (an on-mode-only behaviour change: its release and rebase-only forget now act); off / shadow = today.
// Render thread. Header-only, C++14.

#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

namespace VpMotion
{
	// Which state drives the de-judder's history action.
	inline bool HistoryReaderDrives(bool resyncOn, const VpMotionResync::Readers& readers)
	{
		return resyncOn && readers.Rearms() > 0;   // RegisterOn re-arms the readers: registered on a seal
	}

	// True: the apply point acts (release when pending.release, else forget). epochMoved = what the vector
	// engine's TakeInvalidation returned (pending is its consumed state). Off / shadow / unregistered: exactly
	// epochMoved, pending unchanged.
	inline bool TakeDejudderHistoryAction(bool resyncOn, bool epochMoved, MotionValidity::Pending& pending,
		VpMotionResync::Readers& readers)
	{
		if (!HistoryReaderDrives(resyncOn, readers))
			return epochMoved;
		const VpMotionResync::HistoryPending h = readers.TakeHistory();
		if (h.release)
			pending.release = true;
		return epochMoved || h.changed || h.forgetRebaseHolders;
	}
}
