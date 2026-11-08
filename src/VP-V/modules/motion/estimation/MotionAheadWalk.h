// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Plan 58 T1 / B4 (call-site group B): the motion look-ahead hand-off, moved out of the renderer's
// render loop (LibplaceboVideoRenderer::Render*, "motionAheadFrames" pass). The try-body is the old
// code verbatim (one tab less, `m_impl->` -> `impl.`, queue vector -> `view.frames`); only the catch
// is new: besides the log line and the generic ReportWorker hook it now engages the visible
// FallbackCatalog::MotionAheadWorker() entry (released after 8 good batches).
//
// Threading: the render-loop thread, outside renderMutex (as before). The LookAheadFailures counter
// (Impl::motionAheadFailures) is touched by that thread only; no atomics.
// Existing hazard, unchanged by this move: impl.motionProducer (a unique_ptr) is null-checked and
// dereferenced here without a lock while the render thread may create / reset it (PlanProducer);
// see HOOKS.md section 2, row `motionProducer`.
//
// Templated on the host Impl / queued-frame / timing-clock types so the walk is testable with a fake
// host. Impl provides the fields the body names, SyncFallback() and motionAheadFailures; Queued
// provides the QueuedFrame fields the body names.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <vector>

#include <AnalysisFrame.h>
#include <DebugLog.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <vprenderer/RendererExtension.h>

namespace MotionAhead
{
	// The references the walk needs besides the Impl: the frames the render loop selected (the walk
	// does not release them), the timing clock (null when none) and the measured source rate.
	template <class Impl, class Queued, class Clock>
	struct AheadView
	{
		Impl& impl;
		std::vector<Queued>& frames;
		const Clock* timingClock;
		const std::atomic<double>& measuredFrameRate;
	};

	template <class Impl, class Queued, class Clock>
	void RunQueueWalk(AheadView<Impl, Queued, Clock>& view)
	{
		Impl& impl = view.impl;
		try
		{
			// The stream generation (resync / queue reset / seek / format change), as before
			// step 1: a gap moves only the epoch, so the ahead pairs after it are kept.
			const uint64_t generation = impl.motionValidity.StreamGeneration();
			// Owner 2026-10-16: the look-ahead engages once 4 frames are queued ahead (the startup
			// fill, a refilled queue), disengages with none queued or a broken sequence (a new
			// generation: skip, seek, reset) and waits for 4 again; disengaged, the producer gets
			// no work (no pair could be used before its frames are shown).
			const size_t queuedAhead = impl.queuedFutureFrames.load(std::memory_order_acquire);
			const size_t engageAhead = impl.activeSettings.vectorDemand ?
				static_cast<size_t>(VectorDemand::EngageAhead(impl.motionAheadConfigured.load(std::memory_order_acquire),
					impl.queueAheadCapacity.load(std::memory_order_acquire))) :
				static_cast<size_t>((std::max)(1, impl.motionAheadConfigured.load(std::memory_order_acquire)));
			const bool backwardPairs = impl.motionAheadBackward.load(std::memory_order_acquire);   // vector engine: engaged while it runs
			// A break always disengages and the next pass must see 4 queued ahead again (a re-engage
			// in the same pass would skip the refill wait: the reset test, 2026-10-16).
			if (impl.motionAheadEngaged &&
				(queuedAhead == 0 || generation != impl.motionAheadEngagedGeneration))
			{
				impl.motionAheadEngaged = false;
				DebugLog::Log("Motion ahead: waiting for %zu frames queued ahead (%s)", engageAhead,
					queuedAhead == 0 ? "the queue holds none" : "the sequence broke: skip, seek or reset");
			}
			else if (!impl.motionAheadEngaged && queuedAhead >= engageAhead && !backwardPairs)
			{
				impl.motionAheadEngaged = true;
				impl.motionAheadEngagedGeneration = generation;
				DebugLog::Log("Motion ahead: engaged (%zu frames queued ahead%s)", queuedAhead,
					impl.vectorDemand.lowLatencyAllowed ? "; vector engine: shallow queue" : "");
			}
			const bool framesAhead = impl.motionAheadEngaged || backwardPairs;
			// A frame's time as the dequeue reads it (the film clock when pulldown is locked,
			// else the capture timestamp; 0 = not known).
			LARGE_INTEGER aheadQpcFrequency{};
			const bool aheadQpcKnown = QueryPerformanceFrequency(&aheadQpcFrequency) && aheadQpcFrequency.QuadPart > 0;
			auto aheadTime = [&](const Queued& q) -> double {
				if (q.filmHz > 0.0 && q.arrivalQpc > 0 && aheadQpcKnown)
					return static_cast<double>(q.arrivalQpc) / static_cast<double>(aheadQpcFrequency.QuadPart);
				if (view.timingClock && view.timingClock->TimingClockTicksPerSecond() > 0)
					return static_cast<double>(q.frame.GetTimingTimestamp()) / static_cast<double>(view.timingClock->TimingClockTicksPerSecond());
				return 0.0;
			};
			for (Queued& ahead : view.frames)
			{
				const auto& aheadState = *ahead.state;
				if (!framesAhead || !aheadState.displayMode || !impl.motionProducer) continue;
				// The luma source keeps the frame's own buffer alive for the producer's worker.
				const auto prepared = Impl::PrepareFrameAnalysis(ahead.frame, aheadState, ahead.converted, ahead.analysis,
					AnalysisSourceProfile::QueuedCapture, ahead.activePictureIdentity.sourceFormatGeneration, false,
					aheadState.dolbyVisionTunnel);
				// The first frame not ready yet (a conversion pending) stops this pass: the producer's
				// high-water mark would otherwise pass it and its pairs would never exist (audit 2026-10-16).
				if (!prepared.source.IsValid())
					break;
				// Integration 2026-10-17: also a break when the pair's times are not adjacent
				// (the dequeue's AdjacentPairTimes rule, with timing_continuity_fixes) or, on the
				// DV tunnel, when the pair is not trusted (DolbyVisionPairTrusted).
				DejudderPolicy::MotionAheadFrame aheadFrame;
				aheadFrame.time = aheadTime(ahead);
				aheadFrame.filmClock = ahead.filmHz > 0.0;
				// Hunt 2 02-dv L1: the kind by the payload's capture encoding, as the dequeue
				// classifies it (ClassifyDolbyVisionPayload), not the walk's state encoding.
				if (ahead.converted)
				{
					aheadFrame.kind = DejudderPolicy::ClassifyDolbyVisionPayload(*ahead.converted);
					aheadFrame.matrices = DejudderPolicy::DolbyVisionMatricesOf(ahead.converted->dolby);
				}
				const double aheadRateHz = ahead.filmHz > 0.0 ? ahead.filmHz :
					view.measuredFrameRate.load(std::memory_order_relaxed) > 0.0 ? view.measuredFrameRate.load(std::memory_order_relaxed) :
					aheadState.displayMode->RefreshRateHz();
				const bool havePrev = impl.motionAheadPrevKnown && impl.motionAheadPrevGeneration == generation;
				const bool breakBefore = DejudderPolicy::MotionAheadBreakBefore(ahead.gapBefore, ahead.filmUpdate,
					impl.activeSettings.timingContinuityFixes, aheadRateHz, aheadState.dolbyVisionTunnel,
					havePrev, impl.motionAheadPrev, aheadFrame, impl.activeSettings.motionHistoryFixes);
				impl.motionProducer->Submit(ahead.sourceSequence, generation, prepared.source, breakBefore);
				impl.motionAheadPrevKnown = true;
				impl.motionAheadPrevGeneration = generation;
				impl.motionAheadPrev = aheadFrame;
			}
		}
		catch (const std::exception& e)
		{
			DebugLog::Log("Motion ahead: frames skipped: %s", e.what());
			RendererExtension::ReportWorker(&impl, "motion-ahead", e.what());
			impl.SyncFallback(impl.motionAheadFailures.Note(true, e.what()));
			return;
		}
		impl.SyncFallback(impl.motionAheadFailures.Note(false));
	}
}
