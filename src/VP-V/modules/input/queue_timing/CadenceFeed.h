// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// C7b cadence feed: the ingress policy's per-frame pulldown facts, as the frame-analysis store's cadence group
// (FrameAnalysis::Cadence, FrameAnalysisStore.h) - ONE derivation shared by every reader of the record (rule 21),
// so no consumer re-derives the cadence from the detector.
//
// The facts are taken from the detector's decision for the frame (IngressPolicy.inl, after Detector::Next):
//   locked          Decision::locked   - a film cadence is held (the lock needs three cycles plus two seconds);
//   filmHz          Decision::filmHz   - the film rate while locked (0 = the signal's rate: unlocked, or acquiring);
//   cycle / unique  Decision::cycle / ::unique - frames per cycle and new pictures per cycle (3:2 = 5 / 2);
//   filmIndex       Decision::filmIndex - kept frames since the lock (the even film clock's counter);
//   phaseUncertain  Decision::phaseUncertain - a re-phase is being tracked: the slot is unknown.
// "Acquiring" (Detector::Acquiring, a cadence seen but not yet locked) has no field in the record: it is the
// standing demand the renderer already publishes through m_pulldownAcquiring, so an acquiring frame is recorded as
// unknown (phase -1, filmHz 0), the same as an unlocked one.
//
// Record: filmHz = Decision::filmHz while locked, else 0; phase = filmIndex % unique while locked and the phase is
// certain (the kept frame's position in the cycle's new pictures, counted from the lock), else -1; repeatOfPrevious
// false (the ingress drops a cadence repeat instead of queueing it, so a queued frame is never one; a captured repeat
// the store marks later keeps these fields beside its repeatOf).
//
// Published when the record exists (VpFrameAnalysis::Host::OnArrival, on the conversion thread), not at the ingress
// decision: the store opens a frame's record at arrival, after the frame was queued. Nothing is built while the store is
// off (the host returns first). Pure and header-only, no Windows; C++14.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>

#include <cstdint>

namespace Vpv
{
	namespace Ingress
	{
		struct CadenceFacts
		{
			bool valid = false;            // a cadence group is published for this frame: the detector ran (detection on) or a capture signature was taken (C8-PROD)
			bool locked = false;
			bool phaseUncertain = false;
			double filmHz = 0.0;
			int cycle = 0, unique = 0;
			uint64_t filmIndex = 0;
			// C8-PROD (R14-59): the always-on capture signature compare (one hash per captured frame, shared with pulldown when on).
			// captureValid: a signature was taken for this frame. `valid` above stays "the pulldown detector decided this frame".
			bool captureValid = false;
			bool captureRepeat = false;
		};

		// The previous captured frame's signature; Step() is the one compare. Pulldown keeps its own copy (its toggle edge and
		// resync reset are unchanged); this one runs for every captured frame.
		struct CaptureSignature
		{
			uint64_t last = 0;
			bool Step(uint64_t signature) { const bool repeat = signature != 0 && signature == last; last = signature; return repeat; }
			void Reset() { last = 0; }
		};

		inline CadenceFacts FactsOf(const Pulldown::Decision& decision)
		{
			CadenceFacts facts;
			facts.valid = true;
			facts.locked = decision.locked;
			facts.phaseUncertain = decision.phaseUncertain;
			facts.filmHz = decision.locked ? decision.filmHz : 0.0;
			facts.cycle = decision.locked ? decision.cycle : 0;
			facts.unique = decision.locked ? decision.unique : 0;
			facts.filmIndex = decision.locked ? decision.filmIndex : 0;
			return facts;
		}

		// The detector's facts with the frame's capture bit carried over (the bit is decided before the detector runs).
		inline CadenceFacts FactsOf(const Pulldown::Decision& decision, const CadenceFacts& capture)
		{
			CadenceFacts facts = FactsOf(decision);
			facts.captureValid = capture.captureValid;
			facts.captureRepeat = capture.captureRepeat;
			return facts;
		}

		// Take-once: hands the slot's facts to the caller and clears the slot, so a frame never publishes the previous
		// frame's facts (EnqueueFrame calls this once per captured frame).
		inline CadenceFacts Take(CadenceFacts& slot)
		{
			const CadenceFacts taken = slot;
			slot = CadenceFacts{};
			return taken;
		}

		inline FrameAnalysis::Cadence ToRecord(const CadenceFacts& facts)
		{
			FrameAnalysis::Cadence cadence;
			cadence.captureValid = facts.captureValid;
			cadence.captureRepeat = facts.captureValid && facts.captureRepeat;
			cadence.filmHz = facts.locked ? facts.filmHz : 0.0;
			if (facts.locked && !facts.phaseUncertain && facts.unique > 0)
				cadence.phase = static_cast<int>(facts.filmIndex % static_cast<uint64_t>(facts.unique));
			return cadence;
		}
	}
}
