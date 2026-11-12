// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10-5 preparation (doc 59 section 5.2, the confirmed split): the capture thread's ingress state, the members the
// pulldown / film clock / stale-arrival code reads, copied VERBATIM (names, types, initialisers, comments) from
// LibplaceboVideoRenderer.h so that, at the LVRH step, the renderer takes them as ONE base (the members leave Bill's
// header and every reader keeps its text, as P5a's settings base did). R15 DecideIngress then runs the pulldown /
// stale-gap block before EnqueueFrame from IngressPolicy; the hardware-age block and the missed / break counters
// around Bill's UpdateFrameRateAndPPM become one-line helpers into it; Bill's lines keep their order.
// The renderer takes it as a private base (LibplaceboVideoRenderer.h, M2 P10-5). Threads: as each comment says; the atomics are
// read by the render loop (cadence, skipped count) and written by its settings refresh. C++14.

#include <vprenderer/CaptureTimestampGap.h>   // CaptureArrival::Tracker
#include <VP-V/modules/input/queue_timing/CadenceFeed.h>
#include <VP-V/modules/input/queue_timing/CallbackGate.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>

#include <atomic>
#include <cstdint>

namespace Vpv
{
	namespace Ingress
	{
		struct IngressState
		{
	// Pulldown detection (owner 2026-10-14; dejudder_pulldown, off by
	// default). The detector runs on the capture thread only; the switch is
	// refreshed from the active settings by the render loop; Reset asks for a
	// fresh start.
	std::atomic<bool> m_pulldownDetection{false};
	std::atomic<bool> m_pulldownFixes{true};   // pulldown_fixes (audit 2026-10-16)
	bool m_pulldownFixesApplied = true;        // capture thread
	// timing_continuity_fixes (Bug hunt 2026-10-17): refreshed by the render loop.
	std::atomic<bool> m_timingContinuityFixes{true};
	// cadence_class_fixes (Hunt 2, 2026-10-17): refreshed by the render loop.
	std::atomic<bool> m_cadenceClassFixes{true};
	bool m_cadenceClassFixesApplied = true;   // capture thread
	// PHASE 1 of the host seams plan (refs/107 s2.4 item 3; owner 2026-11-11: FIX): the "fix" switches above (and the
	// renderer's m_queueTimingFixes / m_motionHistoryFixes, declared in Bill's header) start TRUE and were refreshed only
	// by the render loop, so the frames that arrived before its first pass ran the VP fix paths in the Original pathway.
	// Now every arrival syncs them from the ACTIVE settings first (IngressHardwareArrival / OnTimedVideoFrame, inside the
	// callback gate, before any reader), so the first frame already runs with the pathway's switches: Original = all
	// five off (OriginalPathway.h rows queue_timing_fixes, pulldown_fixes, timing_continuity_fixes, cadence_class_fixes,
	// motion_history_fixes), and the ingress binding learns the pathway for R15 / R19 (IngressPolicy.h). The render
	// loop's own refresh stays (same values, same atomics). Capture / file thread. T-AR3-N2: the values are NOT read
	// from `m_impl->activeSettings` (a cross-thread read of a struct the render thread rewrites): UpdatePipelineSlots
	// publishes them (FixSwitchBitsOf) into the hub's per-host fact, and the arrival reads that word.
	struct FixSwitches
	{
		bool pulldownFixes = true, timingContinuityFixes = true, cadenceClassFixes = true;
		bool queueTimingFixes = true, motionHistoryFixes = true;
		bool originalPathway = false;
	};
	template<class Settings>
	static FixSwitches FixSwitchesOf(const Settings& s)
	{
		FixSwitches f;
		f.pulldownFixes = s.pulldownFixes;
		f.timingContinuityFixes = s.timingContinuityFixes;
		f.cadenceClassFixes = s.cadenceClassFixes;
		f.queueTimingFixes = s.queueTimingFixes;
		f.motionHistoryFixes = s.motionHistoryFixes;
		f.originalPathway = s.pipelineOriginalPathway;
		return f;
	}
	// T-AR3-N2: the six switches as one word, published by the settings seam (UpdatePipelineSlots) into the hub's per-host
	// fact and read lock-free by the capture / file threads (they never read the Impl's activeSettings).
	static uint32_t FixSwitchBitsOf(const FixSwitches& f)
	{
		return (f.pulldownFixes ? 1u : 0u) | (f.timingContinuityFixes ? 2u : 0u) | (f.cadenceClassFixes ? 4u : 0u) |
			(f.queueTimingFixes ? 8u : 0u) | (f.motionHistoryFixes ? 16u : 0u) | (f.originalPathway ? 32u : 0u);
	}
	static FixSwitches FixSwitchesFromBits(uint32_t b)
	{
		FixSwitches f;
		f.pulldownFixes = (b & 1u) != 0;
		f.timingContinuityFixes = (b & 2u) != 0;
		f.cadenceClassFixes = (b & 4u) != 0;
		f.queueTimingFixes = (b & 8u) != 0;
		f.motionHistoryFixes = (b & 16u) != 0;
		f.originalPathway = (b & 32u) != 0;
		return f;
	}
	std::atomic<bool> m_originalPathway{false};   // the pathway the active settings selected at the last arrival
	// Stores the switches (the renderer's two are passed in). Returns true when the pathway fact CHANGED (the caller
	// tells the ingress binding: Vpv::Ingress::NoteOriginalPathway).
	bool SyncIngressSwitches(const FixSwitches& f, std::atomic<bool>& queueTimingFixes, std::atomic<bool>& motionHistoryFixes)
	{
		m_pulldownFixes.store(f.pulldownFixes, std::memory_order_relaxed);
		m_timingContinuityFixes.store(f.timingContinuityFixes, std::memory_order_relaxed);
		m_cadenceClassFixes.store(f.cadenceClassFixes, std::memory_order_relaxed);
		queueTimingFixes.store(f.queueTimingFixes, std::memory_order_relaxed);
		motionHistoryFixes.store(f.motionHistoryFixes, std::memory_order_relaxed);
		return m_originalPathway.exchange(f.originalPathway, std::memory_order_acq_rel) != f.originalPathway;
	}
	bool m_pulldownForceGap = false;          // capture thread: a toggle restarted the film clock (06-interp L1)
	// Q1 (queue stress 2026-10-18, queue_timing_fixes): stale-frame arrivals and their
	// delivery gap (capture thread).
	CaptureArrival::Tracker m_captureArrival;
	bool m_captureStaleGapPending = false;
	const char* m_captureStaleGapCause = nullptr;
	uint64_t m_captureStaleRuns = 0;
	bool m_enqueueArrivalIsFrameStart = false;   // OnVideoFrame -> EnqueueFrame (capture callbacks are serial; re-audit MED 1)
	// Capture timestamp breaks (UpdateFrameRateAndPPM's discontinuity branch).
	std::atomic<uint64_t> m_captureTimestampBreaks{0};
	std::atomic<bool> m_pulldownResetRequested{false};
	std::atomic<int> m_pulldownCadence{0};   // (cycle << 16) | new frames per cycle; 0 = not locked
	std::atomic<bool> m_pulldownAcquiring{false};   // SK-MV: a film cadence is being acquired (Detector::Acquiring), not yet locked
	std::atomic<uint64_t> m_pulldownSkipped{0};
	// C7b: this frame's pulldown facts (invalid when detection is off); set by IngressPulldown and taken by EnqueueFrame onto the
	// queued frame (capture thread, callbacks are serial); published to the store's cadence group at arrival (CadenceFeed.h).
	CadenceFacts m_pulldownFrameFacts;
	Pulldown::Detector m_pulldown;
	Pulldown::FilmClock m_pulldownClock;
	uint64_t m_pulldownSignature = 0;
	int64_t m_pulldownFilmQpc = 0;   // the last film frame's even time (QPC)
	// M2 P10-5: OnVideoFrame's locals shared by its three ingress blocks (IngressPolicy.inl), per frame on the capture
	// thread (callbacks are serial). Written by blocks 1-2, read by the R15 call site and block 3.
	int64_t m_ingressArrivalQpc = 0, m_ingressHardwareQpc = 0, m_ingressCallbackQpc = 0;
	bool m_ingressHardwareAgeKnown = false;
	uint64_t m_ingressMissedBefore = 0, m_ingressBreaksBefore = 0;
	bool m_ingressGapBefore = false;
	const char* m_ingressGapCause = nullptr;
	uint32_t m_ingressGapFrames = 0;
	bool m_ingressTimingFixes = true;
	// M2 P10-8 (fix C F2): the capture / file callbacks run inside a Use of this gate; the renderer closes and drains
	// it before it resets its Impl (Retire, FinalizeRetirementForShutdown, Build, the destructor).
	CallbackGate m_ingressGate;

	// W1-A4 (88 resync/flush audit, PARTIAL: pulldownAcquiring sticks true): a fresh start of the pulldown detector, the film
	// clock and the signature (the resync reset), and the warm-engine flag with them: the flag is the detector's own
	// verdict, so a detector with no history has nothing to be acquiring. Capture thread.
	void ResetPulldown()
	{
		m_pulldown.Reset();
		m_pulldownClock.Reset();
		m_pulldownSignature = 0;
		m_pulldownFilmQpc = 0;
		m_pulldownAcquiring.store(false, std::memory_order_relaxed);
	}

	// W2-C1 (open item from W1-A4): a contained throw in IngressPulldown (IngressFaultThunk). The detector and the film clock start
	// over on the next frame through the renderer's reset request, as before; the acquiring flag is cleared now, with the request, as
	// ResetPulldown does on the next frame: a faulted frame's verdict is not a demand, and no further frame may arrive to rewrite it.
	// (The renderer and live-queue reset paths in LibplaceboVideoRenderer.cpp clear it only on that next frame: hook freeze, rule 32.)
	// Capture thread.
	void RequestPulldownResetAfterFault()
	{
		m_pulldownResetRequested.store(true, std::memory_order_release);
		m_pulldownAcquiring.store(false, std::memory_order_relaxed);
	}

	// W1-A4: a frame arrives while pulldown detection is off, or without a display mode (the signal's mode lost). Nothing runs the
	// detector then, so its last verdict (the cadence lock, and the acquiring flag the motion engine's standing demand reads)
	// would otherwise outlive the cause. Clears both, and the detector's stale history with them. Returns true when a LOCK /
	// cadence was in use (the caller marks the "pulldown off" gap, as before); an acquiring-only state held no film timing, so
	// it needs none. Capture thread.
	bool ClearPulldownWhenOff()
	{
		const bool wasAcquiring = m_pulldownAcquiring.exchange(false, std::memory_order_relaxed);
		const bool wasLocked = m_pulldownCadence.load(std::memory_order_relaxed) != 0 || m_pulldown.Locked();
		if (wasLocked || wasAcquiring)
		{
			ResetPulldown();
			m_pulldownCadence.store(0, std::memory_order_relaxed);
		}
		return wasLocked;
	}
		};
	}
}
