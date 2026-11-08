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
		};
	}
}
