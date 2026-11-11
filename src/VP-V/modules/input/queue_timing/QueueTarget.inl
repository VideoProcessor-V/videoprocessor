// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-6 (doc 59 section 5.1 item q, R19): the VP-V queue target. QueueTargetVp is the body PrefillTargetLocked had
// before R19 (moved verbatim): the delay budget's hold, the film-lock scaling, render_ahead_queue_credit and the
// look-ahead floor through the queue policy (Bill's policy under Original / input.queue_timing off). LockedFilmQueueTarget
// is moved verbatim. R19: PrefillTargetLocked now returns Bill's own target through RendererExtension::ResolveQueueTarget;
// the hub module input.queue_timing replaces it with QueueTargetVp through the renderer's binding (QueueTargetThunk).
// The helper's capacity clamp never changes this value (it is <= the clamped configured depth or the clamped hold).
// LibplaceboVideoRenderer.cpp includes this file ONCE, right after PrefillTargetLocked (where LockedFilmQueueTarget
// stood); no other translation unit may. Any thread holding m_queueMutex, as before.
#pragma once

size_t LibplaceboVideoRenderer::QueueTargetVp() const
{
	if (!m_useFrameQueue)
		return 1;
	// VP-V delay budget (ADR-0020): with a budget the queue target is the hold's whole frames at
	// this rate (time-based already, so no film-lock scaling); otherwise the frame-count target.
	QueueTimingPolicy::TargetContext context;
	context.useFrameQueue = m_useFrameQueue;
	context.configuredDepth = m_frameQueueDesiredDepth;
	context.capacity = m_frameQueueMaxSize;
	context.hold = CurrentQueueHold();
	// Host-side cadence scaling (hunt 2 06-interp M3); evaluated only when the hold does not decide.
	context.cadenceScaledDepth = context.hold.active ? 0 :
		LockedFilmQueueTarget(AlphaQueuePolicy::ClampDesiredDepthToCapacity(m_frameQueueDesiredDepth, m_frameQueueMaxSize));
	// render_ahead_queue_credit (design step 7) and the look-ahead floor (hunt 2 13-timing M3), in
	// the one rule the stress harness tests too (AlphaQueuePolicy::CombineQueueTarget): the floor
	// never exceeds `desired`, so a delay budget's hold target wins (audit B2).
	context.renderAheadCreditFrames = m_impl ? m_impl->renderAheadCreditFrames.load(std::memory_order_acquire) : 0;
	context.lookaheadFloor = m_queueTargetFloor.load(std::memory_order_relaxed);
	// P6 rest S1 through the S2 seam (refs/107 R19): the Presenter's analysis floor and queue capacity
	// (RenderAhead::Presenter::FitBudget), published in the VP queue target's own body (0 renderer lines).
	// No analysis product declares a lead yet, so the floor is the look-ahead floor above.
	if (m_impl)
	{
		m_impl->renderAheadPresenter.analysisFloor.store(VpQueueRules::AnalysisFloorFrames(context.lookaheadFloor, 0),
			std::memory_order_release);
		m_impl->renderAheadPresenter.queueCapacity.store(m_frameQueueMaxSize, std::memory_order_release);
	}
	return QueuePolicy().QueueTarget(context);
}

// Hunt 2 (cadence_class_fixes, 06-interp M3: pulldown D2 wired). While a pulldown lock
// holds, the queue holds film frames: target_frames (set for the signal) is scaled by
// new / cycle so the queue's latency does not step by cycle / new at each lock change;
// never below the frames the queue's look-ahead users need (A shown, B queued and converted
// a frame before A leaves, for the interpolator / de-judder: kLockedQueuePairFrames; the optical-flow look-ahead's depth + 1, MotionAhead::QueueFramesFor;
// the active-picture look-ahead + 1) or the target itself. Re-resolved on every call from
// the current cadence.
size_t LibplaceboVideoRenderer::LockedFilmQueueTarget(size_t desired) const
{
	if (!m_cadenceClassFixes.load(std::memory_order_relaxed))
		return desired;
	const int cadence = m_pulldownCadence.load(std::memory_order_relaxed);
	if (cadence == 0)
		return desired;
	size_t minimum = Pulldown::kLockedQueuePairFrames;   // gate r3: A, B ready and the arrival (was 2)
	if (m_impl)
	{
		const int motionDepth = m_impl->motionAheadDepth.load(std::memory_order_acquire);
		if (motionDepth > 0)
			minimum = (std::max)(minimum, MotionAhead::QueueFramesFor(motionDepth));
	}
	// Bug hunt 3 F2: an explicit look-ahead's frames; the automatic one (target - 1 for the
	// signal) follows the scaled queue and holds no floor, else it cancelled the scaling.
	minimum = (std::max)(minimum, QueueLookahead::LockedFilmLookaheadFloor(
		m_activePictureLookaheadFrames.load(std::memory_order_acquire),
		m_activePictureLookaheadAutomatic.load(std::memory_order_acquire)));
	return Pulldown::LockedQueueTarget(desired, Pulldown::FilmToSignalRatio(cadence >> 16, cadence & 0xffff), minimum);
}

void LibplaceboVideoRenderer::QueueTargetThunk(const void* self, RendererExtension::QueueTargetContext& qc)
{
	qc.target = static_cast<const LibplaceboVideoRenderer*>(self)->QueueTargetVp();
}
