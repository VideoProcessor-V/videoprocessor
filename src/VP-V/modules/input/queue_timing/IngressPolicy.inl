// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-5 (doc 59 section 5.1 items j, confirmed split): OnVideoFrame's VP ingress, moved VERBATIM into renderer member
// definitions. LibplaceboVideoRenderer.cpp includes this file ONCE (after EnqueueFrame); no other translation unit may.
// Changes inside the moved text: the locals shared between the blocks are the per-frame ingress members of
// Vpv::Ingress::IngressState (m_ingress*; capture callbacks are serial, as m_enqueueArrivalIsFrameStart already
// assumed) or, in block 3, references to the R15 IngressContext fields; their declarations became assignments; the
// repeat drop's `return;` sets ic.drop. Bill's lines in OnVideoFrame keep their order (block 1 before the queue
// generation read, block 2 around UpdateFrameRateAndPPM, block 3 = R15 DecideIngress just before EnqueueFrame).
// Capture thread; no lock taken here (m_queueChanged.notify_all only), as before.
#pragma once

// Block 1 (before the queue generation read): the frame's hardware arrival. Was OnVideoFrame's locals arrivalQpc,
// hardwareQpc and hardwareAgeKnown; now the per-frame ingress members (capture callbacks are serial).
void LibplaceboVideoRenderer::IngressHardwareArrival(VideoFrame& videoFrame)
{
	// PHASE 1 (IngressState.h FixSwitches): the capture thread's switches follow the active settings from THIS arrival on
	// (before the render loop's first refresh and before any reader below or in EnqueueFrame), and the ingress binding
	// learns the pathway for R15 / R19 when it changes. Inside the callback gate: the Impl outlives this callback.
	// T-AR3-N2: the switches come from the hub fact the settings seam published (no activeSettings read on this thread).
	uint32_t fixBits = 0;
	if (m_impl && Vpv::Hub::ExtensionHub::Instance().FixSwitchBits(m_impl.get(), fixBits) &&
		SyncIngressSwitches(FixSwitchesFromBits(fixBits), m_queueTimingFixes, m_motionHistoryFixes))
		Vpv::Ingress::NoteOriginalPathway(this, m_originalPathway.load(std::memory_order_relaxed));
	int64_t& arrivalQpc = m_ingressArrivalQpc;
	int64_t& hardwareQpc = m_ingressHardwareQpc;
	bool& hardwareAgeKnown = m_ingressHardwareAgeKnown;
	arrivalQpc = 0;
	hardwareQpc = 0;
	hardwareAgeKnown = false;
	{
		const timingclocktime_t frameTicks = videoFrame.GetTimingTimestamp();
		if (m_timingClock && frameTicks > 0)
		{
			try
			{
				const timingclocktime_t nowTicks = m_timingClock->TimingClockNow();
				const int64_t nowQpc = PerformanceCounterNow();
				const timingclocktime_t ticksPerSecond = m_timingClock->TimingClockTicksPerSecond();
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				const timingclocktime_t age = nowTicks - frameTicks;
				if (ticksPerSecond > 0 && frequency.QuadPart > 0 && age >= 0)
				{
					hardwareQpc = nowQpc - static_cast<int64_t>(static_cast<double>(age) *
						static_cast<double>(frequency.QuadPart) / static_cast<double>(ticksPerSecond));
					hardwareAgeKnown = true;
				}
			}
			catch (const std::exception&)
			{
				hardwareAgeKnown = false;   // the callback time is used
			}
		}
	}
}

// Block 2a (right before Bill's UpdateFrameRateAndPPM): the counters it moves.
void LibplaceboVideoRenderer::IngressCountersBefore()
{
	m_ingressMissedBefore = m_captureMissedFrames.load(std::memory_order_relaxed);   // was: const uint64_t missedBefore = ...
	m_ingressBreaksBefore = m_captureTimestampBreaks.load(std::memory_order_relaxed);   // was: const uint64_t breaksBefore = ...
}

// Block 2b (right after Bill's UpdateFrameRateAndPPM): missed frames and timestamp breaks as gaps, and the arrival
// (Q1 CaptureArrival: a stale delivery keeps its hardware time when plausible).
void LibplaceboVideoRenderer::IngressAfterRate(int64_t callbackQpc)
{
	m_ingressCallbackQpc = callbackQpc;
	const uint64_t missedBefore = m_ingressMissedBefore;
	const uint64_t breaksBefore = m_ingressBreaksBefore;
	int64_t& arrivalQpc = m_ingressArrivalQpc;
	const int64_t hardwareQpc = m_ingressHardwareQpc;
	const bool hardwareAgeKnown = m_ingressHardwareAgeKnown;
	bool& gapBefore = m_ingressGapBefore;
	const char*& gapCause = m_ingressGapCause;
	uint32_t& gapFrames = m_ingressGapFrames;
	bool& timingFixes = m_ingressTimingFixes;
	const uint64_t missedNow = m_captureMissedFrames.load(std::memory_order_relaxed);
	gapBefore = missedNow != missedBefore;
	gapCause = gapBefore ? "missed frames" : nullptr;
	gapFrames = gapBefore ? static_cast<uint32_t>((std::min)(missedNow - missedBefore, uint64_t{ 1000 })) : 0;
	// Bug hunt 2026-10-17 (timing_continuity_fixes): a timestamp break (a jump of
	// 9+ periods, an odd jump, a backward step) is a gap too - not whole missed
	// frames, so never filled - else motion pairs and history carry across it.
	timingFixes = m_timingContinuityFixes.load(std::memory_order_relaxed);
	const bool timestampBreak = m_captureTimestampBreaks.load(std::memory_order_relaxed) != breaksBefore;
	if (timingFixes && timestampBreak)
	{
		gapBefore = true;
		gapFrames = 0;
		gapCause = "capture timestamp discontinuity";
	}
	// Q1 (queue stress 2026-10-18, queue_timing_fixes): the arrival. A stale frame (a stalled
	// capture delivering its held frames at once) keeps its hardware time when plausible,
	// else the callback time (as before). The delivery discontinuity is a no-fill gap, marked
	// on the kept frame after the pulldown detector (the cadence lock is kept, no gap-fill
	// pair spans the stall).
	{
		LARGE_INTEGER arrivalFrequency{};
		QueryPerformanceFrequency(&arrivalFrequency);
		const double qpcPerSecond = static_cast<double>(arrivalFrequency.QuadPart);
		const bool queueFixes = m_queueTimingFixes.load(std::memory_order_relaxed);
		if (qpcPerSecond > 0.0)
		{
			const CaptureArrival::Result arrival = m_captureArrival.Next(
				static_cast<double>(callbackQpc) / qpcPerSecond, hardwareAgeKnown,
				static_cast<double>(hardwareQpc) / qpcPerSecond, !timestampBreak, queueFixes);
			arrivalQpc = arrival.Hardware() ? hardwareQpc : 0;
			m_enqueueArrivalIsFrameStart = arrivalQpc != 0;   // re-audit MED 1
			if (arrival.staleStart || arrival.staleEnd)
			{
				m_captureStaleGapPending = true;
				m_captureStaleGapCause = arrival.staleStart ? "stale capture delivery" : "stale capture delivery ended";
				const uint64_t stale = ++m_captureStaleRuns;
				if ((stale & (stale - 1)) == 0)
					DebugLog::Log("Capture: %s (age %.0f ms, %s time; runs %llu, stale frames on hardware time %llu, on callback time %llu)",
						m_captureStaleGapCause, static_cast<double>(callbackQpc - hardwareQpc) * 1000.0 / qpcPerSecond,
						arrival.Hardware() ? "hardware" : "callback", static_cast<unsigned long long>(stale),
						m_captureArrival.StaleHardware(), m_captureArrival.StaleCallback());
			}
		}
		else
		{
			arrivalQpc = 0;
			m_enqueueArrivalIsFrameStart = false;
		}
		if (!queueFixes)
			m_captureStaleGapPending = false;
	}
}

// Block 3 = R15 DecideIngress (the hub module IngressPolicy calls it through the renderer's binding): pulldown
// detection, the film clock and the stale-delivery gap. In: the frame and its state, the gap facts and the arrival
// the caller pre-filled from blocks 1-2. Out: drop (a cadence repeat), the gap facts, arrivalQpc, filmHz, filmUpdate.
// Returns true (decided).
bool LibplaceboVideoRenderer::IngressPulldown(RendererExtension::IngressContext& ic)
{
	VideoFrame& videoFrame = *static_cast<VideoFrame*>(const_cast<void*>(ic.frame));
	const VideoStateComPtr& frameState = *static_cast<const VideoStateComPtr*>(ic.state);
	const int64_t callbackQpc = m_ingressCallbackQpc;
	const uint64_t missedBefore = m_ingressMissedBefore;
	const bool timingFixes = m_ingressTimingFixes;
	bool& gapBefore = ic.gapBefore;
	const char*& gapCause = ic.gapCause;
	uint32_t& gapFrames = ic.gapFrames;
	int64_t& arrivalQpc = ic.arrivalQpc;
	double& filmHz = ic.filmHz;
	bool& filmUpdate = ic.filmUpdate;
	// Pulldown detection (owner 2026-10-14; off by default): a frame that
	// repeats the previous one where the locked cadence has a repeat is not
	// queued; the unique frames get the film rate and an even film clock.
	filmHz = 0.0;
	filmUpdate = false;
	m_pulldownFrameFacts = Vpv::Ingress::CadenceFacts{};   // C7b: invalid unless the detector decides this frame
	if (m_pulldownDetection.load(std::memory_order_relaxed) && frameState->displayMode)
	{
		const Vpv::Resync::PulldownStep pulldownResync = Vpv::Resync::PulldownResyncStep(m_impl.get(), m_pulldownResetRequested.exchange(false, std::memory_order_acq_rel)); // VPV-HOOK: resync cursor (on: FULL resets, timing re-latches)
		m_pulldown.SetTimingRelatch(pulldownResync.driving); // VPV-HOOK
		if (pulldownResync.reset) // VPV-HOOK
			ResetPulldown();   // W1-A4: also clears the acquiring flag (Next below sets it again from the fresh history)
		const uint64_t signature = Pulldown::Signature(videoFrame.GetData(),
			static_cast<size_t>(frameState->BytesPerRow()),
			static_cast<size_t>(frameState->displayMode->FrameHeight()));
		const bool repeat = signature != 0 && signature == m_pulldownSignature;
		m_pulldownSignature = signature;
		const double measuredRateHz = m_measuredFrameRate.load(std::memory_order_relaxed);
		const double signalHz = RendererExtension::ResolveFor(RendererExtension::Resolve::SourceSignalRate, m_impl.get(), measuredRateHz > 0.0 ? measuredRateHz : frameState->displayMode->RefreshRateHz()); // VPV-HOOK
		const bool fixes = m_pulldownFixes.load(std::memory_order_relaxed);
		const bool classFixes = m_cadenceClassFixes.load(std::memory_order_relaxed);
		if (fixes != m_pulldownFixesApplied || classFixes != m_cadenceClassFixesApplied)
		{
			const bool clockForm = fixes != m_pulldownFixesApplied;
			m_pulldownFixesApplied = fixes;
			m_cadenceClassFixesApplied = classFixes;
			Pulldown::Settings settings;
			settings.keepLockAcrossMisses = settings.rephaseInPlace = settings.cadenceAwareClock = fixes;
			// Hunt 2 (cadence_class_fixes): the exclusive film floor (H1), lateness while the
			// phase is uncertain (M1) and a retry that needs the locked phase to fail (L3).
			settings.exclusiveFilmFloor = settings.trackingLatenessMax = settings.retryNeedsFailure = classFixes;
			// Real-footage suite 2026-10-18: a lock needs a maximally even pattern (no mixed-cadence lock).
			settings.maximallyEvenPattern = classFixes;
			m_pulldown.SetSettings(settings);
			// Bug hunt 2026-10-17: the clock's form changes with the switch (cadence-aware
			// or not): start it over, and no pair across the change.
			if (clockForm)
			{
				// Hunt 2 (L1): a clock in use restarting is a break whatever pulldown_fixes now says.
				m_pulldownForceGap = m_pulldownForceGap || m_pulldownClock.InUse();
				m_pulldownClock.Reset();
				m_pulldown.MarkDiscontinuity();
			}
		}
		// Hunt 2 (cadence_class_fixes, 06-interp L6): the second-largest anchor; toggled mid-lock the
		// offsets start over and no pair crosses the change.
		if (m_pulldownClock.SetRobustAnchor(classFixes))
		{
			m_pulldown.MarkDiscontinuity();
			m_pulldownForceGap = true;
		}
		// Bug hunt 2026-10-17 (timing_continuity_fixes): the film clock re-bases when
		// the film rate (from signalHz: measured or nominal) changes mid-lock.
		// Post-merge audit 2026-10-17: toggled mid-lock, the clock starts over and no
		// pair crosses the change.
		if (m_pulldownClock.SetRebaseOnRateChange(timingFixes))
		{
			m_pulldown.MarkDiscontinuity();
			m_pulldownForceGap = true;   // Hunt 2 (L1)
			DebugLog::Log("Pulldown: film clock re-basing %s (timing_continuity_fixes); clock restarted",
				timingFixes ? "on" : "off");
		}
		// Q1 (pulldown_fixes): the film clock's late-arrival guard (never backwards inside a lock).
		m_pulldownClock.SetLateGuard(fixes);
		const bool wasLocked = m_pulldown.Locked();
		const int previousCycle = m_pulldown.Cycle(), previousUnique = m_pulldown.Unique();
		// R3 (resync_event on): an unannounced rate move (timing) re-latches the signal rate; the lock
		// is dropped only when the cadence class changes (Pulldown::Detector::RelatchSignalHz).
		if (pulldownResync.relatchTiming && m_pulldown.RelatchSignalHz(signalHz)) // VPV-HOOK
			DebugLog::Log("Pulldown: signal rate moved to %.3f Hz beyond the lock's resolution (resync timing); lock dropped",
				signalHz);
		// D4: the counted misses keep the lock (verified over the next cycle).
		const uint64_t missed = m_captureMissedFrames.load(std::memory_order_relaxed) - missedBefore;
		static GeneralRate::FallbackNotice pulldownRateFallback;   // fixed-constant batch B7 2026-10-18: logged (PulldownDetector assumes 60)
		GeneralRate::NoteFallback(pulldownRateFallback, signalHz > 0.0, "pulldown contradiction window", 60.0);
		const Pulldown::Decision decision = m_pulldown.Next(repeat, gapBefore, signalHz,
			gapBefore && fixes ? static_cast<int>((std::min)(missed, uint64_t{ 1000 })) : -1);
		if (decision.locked != wasLocked || decision.relocked)
		{
			// The frame timing changes here: no pair across it.
			if (!gapBefore)
				gapCause = "pulldown lock change";
			gapBefore = true;
			gapFrames = 0;   // not a loss of whole frames: never filled
			m_pulldownClock.Reset();
			// Not rate-limited: a lock needs two seconds, so changes are rare.
			const std::string lockText = decision.locked ?
				Pulldown::Describe(decision.filmHz, signalHz, decision.unique, decision.cycle) : std::string("lost");
			const std::string before = wasLocked && previousUnique > 0 ?
				Pulldown::Describe(signalHz * previousUnique / previousCycle, signalHz, previousUnique, previousCycle) :
				std::string("none");
			DebugLog::Log("Pulldown: %s (was %s)", lockText.c_str(), before.c_str());
		}
		const int cadence = decision.locked ? (decision.cycle << 16) | decision.unique : 0;
		m_pulldownFrameFacts = Vpv::Ingress::FactsOf(decision);   // C7b: the cadence group of this frame's store record (CadenceFeed.h)
		m_pulldownAcquiring.store(!decision.locked && m_pulldown.Acquiring(), std::memory_order_relaxed);   // SK-MV warm engine
		// Hunt 2 (06-interp M3): the queue target follows the lock (PrefillTargetLocked): wake
		// the waiters when it changes.
		if (m_pulldownCadence.exchange(cadence, std::memory_order_relaxed) != cadence && classFixes)
			m_queueChanged.notify_all();
		if (decision.drop)
		{
			// D1: a lock change (or a gap) on a dropped repeat marks the next kept frame.
			if (gapBefore && fixes)
				m_pulldown.MarkDiscontinuity();
			m_pulldownSkipped.fetch_add(1, std::memory_order_relaxed);
			ic.drop = true;   // P10-5: was `return;` (the repeat is not queued)
			return true;
		}
		// Hunt 2 (cadence_class_fixes, 06-interp L1): a clock restarted by a switch toggle is a
		// break even when pulldown_fixes is now off (the consume below ignores the mark then);
		// kept across dropped repeats until this kept frame.
		const bool forcedGap = m_pulldownForceGap;
		m_pulldownForceGap = false;
		if (forcedGap && classFixes)
		{
			if (!gapBefore)
			{
				gapCause = "pulldown film clock restarted (switch toggled)";
				gapBefore = true;
			}
			gapFrames = 0;
		}
		// Consumed always (a mark left while the fixes were off must not fire later); labelled by cause.
		if (m_pulldown.ConsumeDiscontinuity() && fixes)
		{
			if (!gapBefore)
			{
				gapCause = decision.rephased ? "pulldown re-phase" : "pulldown lock change (on a skipped repeat)";
				gapBefore = true;
			}
			gapFrames = 0;   // the film timing changed: never a gap-fill pair, missed frames or not
		}
		if (decision.rephased)
		{
			gapFrames = 0;
			// Hunt 2 (cadence_class_fixes, 06-interp M1): the offsets from before the shift would
			// hold the anchor for the clock's window and then step it mid-shot; this frame
			// carries the re-phase discontinuity, so the clock re-anchors here.
			if (classFixes)
				m_pulldownClock.Reanchor();
			DebugLog::Log("Pulldown: re-phased in place (lock kept)");
		}
		if (decision.locked)
		{
			filmHz = decision.filmHz;
			if (decision.update && m_pulldownFilmQpc > 0)
			{
				// Option b: the same film frame, updated - at its time.
				filmUpdate = true;
				arrivalQpc = m_pulldownFilmQpc;
			}
			else
			{
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				const double qpcPerSecond = static_cast<double>(frequency.QuadPart);
				const int64_t arrived = arrivalQpc > 0 ? arrivalQpc : callbackQpc;
				m_enqueueArrivalIsFrameStart = arrivalQpc > 0;   // the film clock keeps its anchor's meaning
				arrivalQpc = static_cast<int64_t>((fixes ? m_pulldownClock.Time(decision.filmIndex,
					static_cast<double>(arrived) / qpcPerSecond, filmHz, decision.cadenceLateness, decision.cadenceLatenessMax) :
					m_pulldownClock.Time(decision.filmIndex, static_cast<double>(arrived) / qpcPerSecond, filmHz)) * qpcPerSecond);
				m_pulldownFilmQpc = arrivalQpc;
			}
		}
		else
			m_pulldownFilmQpc = 0;
	}
	else if (ClearPulldownWhenOff())   // W1-A4: also clears a stuck acquiring flag (no lock held, detector idle); the gap only for a lock
	{
		if (!gapBefore)
			gapCause = "pulldown off";
		gapBefore = true;
		gapFrames = 0;
	}
	// Q1 (queue_timing_fixes): a stale delivery's gap, kept across dropped pulldown repeats
	// until this kept frame; never filled.
	if (m_captureStaleGapPending)
	{
		m_captureStaleGapPending = false;
		if (!gapBefore)
			gapCause = m_captureStaleGapCause;
		gapBefore = true;
		gapFrames = 0;
	}
	return true;
}

bool LibplaceboVideoRenderer::IngressDecideThunk(void* self, RendererExtension::IngressContext& ic)
{
	return static_cast<LibplaceboVideoRenderer*>(self)->IngressPulldown(ic);
}

// Audit slice 3 F1: after a contained throw in IngressPulldown, the detector and the film clock start over on the next
// frame (the renderer's own reset request, as a resync FULL would). W2-C1: the acquiring flag clears with the request.
void LibplaceboVideoRenderer::IngressFaultThunk(void* self)
{
	static_cast<LibplaceboVideoRenderer*>(self)->RequestPulldownResetAfterFault();
}
