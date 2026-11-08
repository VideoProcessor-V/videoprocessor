// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-6a (doc 59 section 5.1 item m): the VP-V delay budget's queue hold (ADR-0020), moved VERBATIM out of
// LibplaceboVideoRenderer.cpp: SetQueueDelayBudget, CurrentQueueHold, QueuePolicy (the either/or with Bill's queue
// policy), UpdateQueueDelayHold, HoldTicksUntilDueLocked, kDueWaitRecheckMicros and WaitForDequeueLocked. These are
// member definitions of LibplaceboVideoRenderer; LibplaceboVideoRenderer.cpp includes this file ONCE where the text stood
// (right after the TimedFileFrames.inl include, before Bill's UpdateFrameRateAndPPM), so the translation unit is textually unchanged. No other translation unit may include it. Threads as before:
// SetQueueDelayBudget from the GUI (vpv.presentation_ctl.v1), UpdateQueueDelayHold from the render loop,
// CurrentQueueHold under m_queueHoldMutex from any thread, HoldTicksUntilDueLocked / WaitForDequeueLocked from the
// render loop holding m_queueMutex. The state (m_queueHold, m_queueDelayBudgetMs, the policies) stays in the
// renderer's header until the LVRH step (then R19 ResolveQueueTarget).
// VP-V delay budget (ADR-0020, barrier B2). The user sets ONE total delay, the same at every
// frame rate. DelayBudget turns it into the time each frame waits in this queue (the hold):
// fixed costs (swapchain, processing, capture transfer, render-ahead's residual lead) + hold =
// the budget. The whole frames of the hold are the queue target (PrefillTargetLocked); the
// sub-frame rest by the age gate below on the plain path, by the steering (target + holdFraction)
// on the VP timeline. Not in low latency (no budget there).
void LibplaceboVideoRenderer::SetQueueDelayBudget(int totalMs)
{
	const int previous = m_queueDelayBudgetMs.exchange(totalMs > 0 ? totalMs : 0, std::memory_order_acq_rel);
	if (previous != totalMs)
		DebugLog::Log("Delay budget: %d ms total (0 = frame-count queue)", totalMs > 0 ? totalMs : 0);
	m_queueChanged.notify_all();
}

LibplaceboVideoRenderer::QueueHold LibplaceboVideoRenderer::CurrentQueueHold() const
{
	std::lock_guard<std::mutex> guard(m_queueHoldMutex);
	return m_queueHold;
}

const QueueTimingPolicy::IQueueTimingPolicy& LibplaceboVideoRenderer::QueuePolicy() const
{
	// Either/or (owner 2026-10-25): the Original pathway and input.queue_timing switched off run Bill's
	// queue policy (SlotSwitchSettings::UsesUpstreamQueuePolicy); otherwise VP's fixed total delay.
	const bool original = m_impl && m_impl->pipelineSlots.OriginalPathway();
	const bool slotOff = m_impl && !m_impl->pipelineSlots.Runs(PipelineSlots::Id::InputQueueTiming);
	if (VpQueueTiming::Select(slotOff, original, true) == VpQueueTiming::Selection::Upstream)
		return m_upstreamQueuePolicy;
	const_cast<VpQueueTiming::VpFixedBudgetPolicy&>(m_vpQueuePolicy).SetOptions({
		original, m_queueTimingFixes.load(std::memory_order_relaxed) });
	return m_vpQueuePolicy;
}

void LibplaceboVideoRenderer::UpdateQueueDelayHold(double sourceRateHz, double signalRateHz)
{
	const int budgetMs = m_queueDelayBudgetMs.load(std::memory_order_acquire);
	const bool lowLatency = m_impl && m_impl->lowLatencyQueueActive.load(std::memory_order_acquire);
	// The decision is the policy's (VP fixed total delay, or upstream's frame-count queue); the host
	// keeps the status, the log and the wake-up.
	QueueTimingPolicy::HoldContext context;
	context.budgetMs = m_impl ? budgetMs : 0;
	context.sourceRateHz = sourceRateHz;
	context.signalRateHz = signalRateHz;
	context.lowLatency = lowLatency;
	if (m_impl)
	{
		context.displayHz = m_impl->TimelineDisplayHz();
		context.renderAheadActive = m_impl->RenderAheadActive();
		if (context.renderAheadActive && context.displayHz > 0.0 && sourceRateHz > 0.0)
			context.renderAheadLeadSeconds = RenderAhead::PlannedLead(1.0 / sourceRateHz, 1.0 / context.displayHz);
		context.renderAheadCreditFrames = m_impl->renderAheadCreditFrames.load(std::memory_order_acquire);
		// Owner decision 2: the swapchain frames of whichever owner presents (Bill's configured
		// depth unless render-ahead owns presentation), never a fixed 2.
		context.presentationFrames = m_impl->delaySwapchainFrames.load(std::memory_order_acquire);
	}
	{
		std::lock_guard<std::mutex> guard(m_queueMutex);
		context.queueCapacity = m_useFrameQueue ? m_frameQueueMaxSize : 1;
	}
	const QueueTimingPolicy::HoldPlan plan = QueuePolicy().PlanHold(context);
	const std::string& budgetNote = plan.note;
	QueueHold next = plan;
	bool wasActive = false;
	{
		std::lock_guard<std::mutex> guard(m_queueHoldMutex);
		wasActive = m_queueHold.active;
		m_queueHold = next;
	}
	if (m_impl)
	{
		if (budgetNote.empty())
			m_impl->ReleaseFallback("delay budget");
		else
			m_impl->EngageFallback("delay budget", budgetNote);
		// WP-23: the store's ring follows the budget (queue + look-ahead + 2), never a fixed count.
		const int analysisFrames = next.active ? plan.queueFrames : static_cast<int>(AlphaQueuePolicy::DEFAULT_DESIRED_DEPTH);
		m_impl->frameAnalysis.SetRingDepth(analysisFrames, analysisFrames);
		// The frame fallback ladder (owner 2026-10-20/24): below the design depth every look-ahead
		// consumer uses what is queued (shorter look-ahead, never waiting, never adding delay);
		// visible, not silent.
		if (next.active && plan.degraded)
			m_impl->EngageFallback("look-ahead", std::to_string(plan.queueFrames) + " of " + std::to_string(DelayBudget::kDesignDepth) +
				" queued frames (delay budget " + std::to_string(budgetMs) + " ms)");
		else
			m_impl->ReleaseFallback("look-ahead");
	}
	std::string state;
	if (next.active)
	{
		char text[192] = {};
		sprintf_s(text, "%d ms at %.3f Hz (signal %.3f Hz): %d look-ahead frames, hold %.1f ms = target %zu + %.2f frames, padding %.1f ms",
			budgetMs, sourceRateHz, signalRateHz > 0.0 ? signalRateHz : sourceRateHz, plan.queueFrames, plan.holdMs,
			plan.targetFrames, plan.fraction, plan.paddingMs);
		state = text;
	}
	if (state != m_queueHoldLogged)
	{
		m_queueHoldLogged = state;
		if (next.active)
			DebugLog::Log("Delay budget: %s", state.c_str());
		else if (wasActive)
			DebugLog::Log("Delay budget: hold off (%s)", budgetMs <= 0 || (m_impl && m_impl->pipelineSlots.OriginalPathway()) ? "no budget" : lowLatency ? "low latency" : "not feasible");
	}
	if (next.active != wasActive)
		m_queueChanged.notify_all();
}

int64_t LibplaceboVideoRenderer::HoldTicksUntilDueLocked() const
{
	// The plain per-refresh path only. The VP timeline realises the rest by its steering (target +
	// holdFraction); an age gate there adds a frame its slow steering does not drain (delay-gate
	// finding 2026-10-24).
	if (m_frameQueue.empty() || !m_impl)
		return 0;
	const QueuedFrame& head = m_frameQueue.front();
	QueueTimingPolicy::DueContext context;
	context.hold = CurrentQueueHold();
	if (!context.hold.active)
		return 0;   // as before: no clock reads without a hold
	context.timelineActive = m_impl->vpTimelineActive;
	context.fileFrame = head.fileFrame;
	context.arrivalQpc = head.arrivalQpc;   // the frame's START (DeckLink normalised, audit MED 7)
	context.enqueueQpc = head.enqueueQpc;
	LARGE_INTEGER frequency{};
	context.arrivalIsFrameStart = head.arrivalIsFrameStart;   // re-audit MED 1: the transfer only for a hardware frame-start time
	context.qpcFrequency = QueryPerformanceFrequency(&frequency) ? frequency.QuadPart : 0;
	context.nowQpc = PerformanceCounterNow();
	return QueuePolicy().TicksUntilDue(context);
}

// A timed dequeue wait (a file head's due time, the delay budget's hold) re-checks at least this
// often, so a changed presentation lead or budget is followed promptly.
static constexpr int64_t kDueWaitRecheckMicros = 100000;

void LibplaceboVideoRenderer::WaitForDequeueLocked(std::unique_lock<std::mutex>& lock,
	bool wakeOnCalibrationPattern)
{
	// wakeOnCalibrationPattern (integration round 4): the render loop's preview
	// wait also returns when a calibration patch starts, as on its branch; the
	// caller checks m_calibrationPatternActive and presents the patch instead.
	const auto calibrationActive = [this, wakeOnCalibrationPattern]() {
		return wakeOnCalibrationPattern &&
			CalibrationLutHost::IsTakingOver(m_calibrationPatternActive);
	};
	for (;;)
	{
		m_queueChanged.wait(lock, [this, &calibrationActive]() {
			return m_stopRequested || CanDequeueLocked() || calibrationActive(); });
		if (m_stopRequested || calibrationActive())
			return;
		// Capture heads: 0 at once (the wait above), unless the delay budget's hold applies
		// (HoldTicksUntilDueLocked: the plain path's age gate).
		const int64_t fileTicks = FileHeadTicksUntilDueLocked();
		const int64_t ticks = fileTicks > 0 ? fileTicks : HoldTicksUntilDueLocked();
		if (ticks <= 0)
			return;
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		const int64_t micros = frequency.QuadPart > 0 ?
			static_cast<int64_t>(static_cast<double>(ticks) * 1000000.0 / static_cast<double>(frequency.QuadPart)) : 1000;
		// Woken early by any queue change (a reset, a stop, a new head); re-checked
		// at least every 100 ms so a changed presentation lead is followed.
		m_queueChanged.wait_for(lock, std::chrono::microseconds(
			(std::max)(int64_t{ 1 }, (std::min)(micros, kDueWaitRecheckMicros))));
	}
}
