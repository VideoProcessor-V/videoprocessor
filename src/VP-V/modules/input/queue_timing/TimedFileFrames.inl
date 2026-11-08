// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-7a (doc 59 section 5.1 item l): the renderer's timed file frames (ABI 37, the VP Renderer DirectShow
// filter), moved VERBATIM out of LibplaceboVideoRenderer.cpp: OnTimedVideoFrame, FileFrameSlotFreeLocked,
// WaitForFileFrameSlot, CancelFileFrameWait, FileHeadTicksUntilDueLocked. These are member definitions of
// LibplaceboVideoRenderer; LibplaceboVideoRenderer.cpp includes this file ONCE where the text stood (right after EnqueueFrame, before the
// QueueDelayHold.inl include and Bill's UpdateFrameRateAndPPM), so the translation
// unit is textually unchanged. No other translation unit may include it. The plugin boundary is P2's
// vpv.timed_file_frames.v1 (src/VP-V/abi: RendererAbiBridge forwards to these members; VPRendererFilter calls through
// VpvHostCalls). Threads as before: the file thread (OnTimedVideoFrame, WaitForFileFrameSlot), any thread
// (CancelFileFrameWait), the render loop under m_queueMutex (FileFrameSlotFreeLocked, FileHeadTicksUntilDueLocked).
// The state (m_fileSlotGate, the file fields of QueuedFrame) stays in the renderer's header until the LVRH step
// (R20: IQueueAdmission).
// ABI 37 (VP Renderer DirectShow filter): a file frame. Capture timing is not
// used (no UpdateFrameRateAndPPM, no pulldown detection or cadence): the film
// clock is filled from the frame's own timing - the source rate (exact
// rational, else measured) times the playback rate, on the frame's due time.
void LibplaceboVideoRenderer::OnTimedVideoFrame(VideoFrame& videoFrame,
	const FileFrameTiming& timing)
{
	// Any timed frame re-arms the slot wait (CancelFileFrameWait is sticky
	// until here).
	m_fileSlotGate.Arm();
	const int64_t callbackQpc = PerformanceCounterNow();
	const Vpv::Ingress::CallbackGate::Use ingressUse(m_ingressGate); if (!ingressUse) return;   // P10-8 (fix C F2): the Impl outlives this callback
	if (m_state.load(std::memory_order_acquire) != RendererState::RENDERSTATE_RENDERING)
		return;
	if (!FileFrameClock::TimingUsable(timing))
	{
		const uint64_t total = m_fileTimingRejected.fetch_add(1, std::memory_order_relaxed) + 1;
		m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
		if ((total & (total - 1)) == 0)
			DebugLog::Log("File frame refused: timing block of %u bytes, this renderer needs %u (total %llu)",
				static_cast<unsigned>(timing.structSize), static_cast<unsigned>(sizeof(FileFrameTiming)),
				static_cast<unsigned long long>(total));
		return;
	}

	uint64_t enqueueGeneration = 0;
	{
		std::lock_guard<std::mutex> guard(m_queueMutex);
		if (m_stopRequested)
			return;
		enqueueGeneration = m_queueGeneration;
	}
	m_frameCounter.fetch_add(1, std::memory_order_relaxed);

	VideoStateComPtr frameState;
	{
		std::lock_guard<std::mutex> guard(m_stateMutex);
		frameState = m_videoState;
	}
	if (!frameState)
	{
		const uint64_t droppedTotal =
			m_droppedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
		const uint64_t reasonTotal =
			m_missingFrameStateDrops.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((reasonTotal & (reasonTotal - 1)) == 0)
		{
			DebugLog::Log(
				"Alpha dropped file frame: reason=video_state_unavailable reason_total=%llu dropped_total=%llu",
				static_cast<unsigned long long>(reasonTotal),
				static_cast<unsigned long long>(droppedTotal));
		}
		return;
	}

	const double filmHz = FileFrameClock::FilmHz(timing);
	const bool motionOff = !FileFrameClock::MotionAllowed(timing);
	if (filmHz != m_fileLoggedFilmHz || motionOff != m_fileLoggedMotionOff)
	{
		// On a change only (a seek, a rate change, pause / run).
		m_fileLoggedFilmHz = filmHz;
		m_fileLoggedMotionOff = motionOff;
		DebugLog::Log("File frames: film clock %.6f Hz (source %u/%u = %.6f Hz, measured %.6f Hz, playback rate %.3f)%s, motion %s%s",
			filmHz, timing.sourceRateNumerator, timing.sourceRateDenominator,
			FileFrameClock::SourceRateHz(timing), timing.sourceRateHz, timing.playbackRate,
			filmHz > 0.0 ? "" : " (rate unknown: the signal's nominal rate)",
			motionOff ? "off" : "allowed",
			timing.paused ? " (paused: shown and held)" : "");
	}

	FileQueueFields file;
	file.presentationQpc = timing.presentationQpc;
	file.paused = timing.paused;
	file.motionOff = motionOff;
	file.discontinuity = timing.discontinuity;
	// A discontinuity is a source discontinuity at this frame (continuity of the
	// active-picture timeline, smooth motion and the timeline break here; the
	// render thread invalidates the vectors-valid state when it dequeues it).
	if (timing.discontinuity)
		videoFrame.SetSourceDiscontinuity(true);
	EnqueueFrame(videoFrame, frameState, enqueueGeneration, callbackQpc,
		FileFrameClock::ArrivalQpc(timing, callbackQpc),
		timing.discontinuity, timing.discontinuity ? "file discontinuity (seek, step or rate change)" : nullptr,
		0, filmHz, false, &file);
}


bool LibplaceboVideoRenderer::FileFrameSlotFreeLocked() const
{
	if (m_stopRequested ||
		m_state.load(std::memory_order_acquire) != RendererState::RENDERSTATE_RENDERING)
		return false;
	const size_t queueLimit = m_useFrameQueue ? m_frameQueueMaxSize : 1;
	return m_frameQueue.size() < queueLimit;
}


bool LibplaceboVideoRenderer::WaitForFileFrameSlot(uint32_t timeoutMs)
{
	// The gate sleeps on its own mutex; the queue mutex is taken only to read
	// the queue's depth, so the render thread is never blocked by a waiter.
	return m_fileSlotGate.Wait(timeoutMs, [this]()
		{
			std::lock_guard<std::mutex> guard(m_queueMutex);
			return FileFrameSlotFreeLocked();
		});
}


void LibplaceboVideoRenderer::CancelFileFrameWait()
{
	m_fileSlotGate.Cancel();
}


int64_t LibplaceboVideoRenderer::FileHeadTicksUntilDueLocked() const
{
	if (m_frameQueue.empty() || !m_frameQueue.front().fileFrame)
		return 0;
	const QueuedFrame& head = m_frameQueue.front();
	LARGE_INTEGER frequency{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return 0;
	// The measured lead from submission to the display's target refresh: the
	// frame is submitted that much before it is due on screen.
	int64_t leadQpc = 0;
	if (m_presentationTargetTimingKnown.load(std::memory_order_acquire))
	{
		const double leadMs = m_presentationTargetLeadMs.load(std::memory_order_relaxed);
		if (std::isfinite(leadMs) && leadMs > 0.0 && leadMs < 1000.0)
			leadQpc = static_cast<int64_t>(leadMs * static_cast<double>(frequency.QuadPart) / 1000.0);
	}
	return FileFrameClock::TicksUntilDue(PerformanceCounterNow(), head.filePresentationQpc,
		leadQpc, head.filePaused);
}
