// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/drive/led_measure/NativeReadback.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include <cstdint>

// fx/dv-frame-quiet: no native D3D11 work inside libplacebo's frame, and the source-resolution
// monitor in frame-bound mode. The renderer host (Bill's file) keeps NO call sites; the calls are:
//   Arm()    once per render, before the frame's hooks run (PlanEntrance);
//   Begin()  once per NEW source frame, before the render (PlanEntrance; the monitor's CPU part, frame-bound on);
//   Seconds() the source time handed to the monitor's decision calls (AutoFactors, AutoTransition);
//   EndThisFrame() after the frame's submit, from the R12 AfterPresent seam (SkeletonModule::OnAfterPresent): the
//            monitor's tile pass and copy, then the copies the hooks (adaptive downscale statistics, LED
//            measurement) recorded, then one flush only when that work ran. Re-applied onto the P4 e planner and
//            the AfterPresent seam (refs/121): no line in a Bill-owned file.
//
// Why: the odd renders of the DV capture test (DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer,
// NVIDIA only) followed the monitor's tile compute pass and its native CopyResource issued before
// pl_render_image; issuing them after the frame's submit removed ~97% of the odd frames (record:
// D:\codex\videoprocessor\fxdv-disc-trx). The tile pass reads the NATIVE source plane, so it does not
// need to run before the render; the hooks' statistics are written by their passes inside the frame
// (those stay) but their native copy + fence signal are recorded and issued here (NativeReadback.h,
// DeferredCopies). The monitor's results then depend on the frame sequence, not on readback timing:
// frame-bound mode releases the analysis of frames <= s - D at frame s (SourceResolutionMonitor.h).
//
// Header-only, C++14. Render thread only.
namespace FrameQuiet
{
	// Frames the monitor's decisions lag the analysis by (frame-bound D). A tile copy is issued at the end
	// of frame s, collected at the end of frame s+1 or a few frames later (the fence), analysed on the
	// worker in milliseconds, and applied at frame s + D. Frames, not seconds: general over frame rates.
	// A result that still arrives after its release point is counted as a missed wait (Reproducible()
	// turns false, Describe says so): the lead is a margin, and a miss is visible.
	inline uint32_t LeadFrames() { return 8; }

	// Source seconds for the monitor's frame-bound mode: the frames elapsed times the source period (the
	// measured rate), never the wall clock. A rate that is unknown (0) holds the time still, a sequence
	// that steps backwards (a seek) holds it and goes on from there. Pure, CPU-tested.
	class SourceTime
	{
	public:
		double Advance(uint64_t sequence, double rateHz)
		{
			if (!have)
			{
				have = true;
				last = sequence;
				return seconds;
			}
			if (sequence > last)
			{
				if (rateHz > 0.0)
					seconds += static_cast<double>(sequence - last) / rateHz;
				last = sequence;
			}
			else if (sequence < last)
				last = sequence;
			return seconds;
		}
		double Seconds() const { return seconds; }
		void Reset() { have = false; last = 0; seconds = 0.0; }

	private:
		bool have = false;
		uint64_t last = 0;
		double seconds = 0.0;
	};

	// One per render thread (the renderer host owns nothing of it: PlanEntrance feeds it, the R12 AfterPresent
	// seam ends it, so the host needs no new line). Not thread safe by design: render thread only.
	class Deferred
	{
	public:
		static Deferred& ForThisThread()
		{
			static thread_local Deferred instance;
			return instance;
		}

		// Every render, before pl_render_image (PlanEntrance, not run in the Original pathway): native copies a hook
		// records are issued by End(). An End() that never came (the seam was not reached) is made up now, outside
		// the new frame's libplacebo work, and counted (MissedEnds, visible through the decision).
		void Arm(pl_gpu gpu)
		{
			if (armed)
			{
				++missedEnds;
				EndFrame();
			}
			armed = true;
			armedGpu = gpu;
			LedDrive::DeferredCopies::ForThisThread().Arm();
		}

		// Every NEW source frame, before the render. `image` is the native, unwarped, not re-based frame.
		// frameBound true (the switch source_resolution_frame_bound): the monitor's CPU part runs now, its GPU part in
		// End(); false: the monitor runs as before (everything inside the frame, wall clock), Seconds() = -1.
		void Begin(SourceResolutionMonitor::Monitor& monitor, pl_dispatch dispatch, const pl_frame& image, uint64_t sequence,
			double sourceRateHz, bool frameBound, double wallSeconds)
		{
			if (!frameBound)
			{
				monitor.SetFrameBound(false, LeadFrames());   // idempotent
				time.Reset();
				boundNow = false;
				monitor.Frame(armedGpu, dispatch, image, sequence, wallSeconds);
				return;
			}
			monitor.SetFrameBound(true, LeadFrames());   // idempotent
			boundNow = true;
			time.Advance(sequence, sourceRateHz);
			frame = image;
			frameSequence = sequence;
			frameMonitor = &monitor;
			frameDispatch = dispatch;
			pending = true;
			monitor.FrameBegin(frame, sequence, time.Seconds());
		}

		// The source time of the frame Begin() saw last (what AutoFactors / AutoTransition take); -1 = not frame-bound.
		double Seconds() const { return boundNow ? time.Seconds() : -1.0; }

		// The R12 AfterPresent seam (a module on the hub): ends the frame this thread armed. No-op when not armed.
		static void EndThisFrame() { ForThisThread().EndFrame(); }

		// The decision of the last End, taken once by the host's next PlanEntrance (SyncFallback).
		FallbackCatalog::Decision TakeDecision() { const FallbackCatalog::Decision d = decision; decision = FallbackCatalog::Decision(); return d; }

		unsigned long long MissedEnds() const { return missedEnds; }
		// Counters for the tests: how many ends ran VP GPU work and flushed.
		unsigned long long Flushes() const { return flushes; }

		// After the frame's submit. Never throws. pl_gpu_flush only when VP's own native work ran in this frame (the
		// deferred hook copies, or the monitor's tile pass); a frame with none flushes nothing.
		void EndFrame()
		{
			if (!armed)
				return;
			armed = false;
			LedDrive::DeferredCopies& deferred = LedDrive::DeferredCopies::ForThisThread();
			bool ranWork = deferred.Flush() > 0;
			if (pending)
			{
				pending = false;
				if (frameMonitor)
					ranWork = frameMonitor->FrameSubmit(armedGpu, frameDispatch, frame, frameSequence) || ranWork;
				frameMonitor = nullptr;
				frameDispatch = nullptr;
			}
			// Submit what was just issued (the copies and their fence signals): outside the frame, the batch the
			// next frame starts is then its own.
			if (ranWork && armedGpu)
			{
				pl_gpu_flush(armedGpu);
				++flushes;
			}
			armedGpu = nullptr;
			// Audit fix 8: a late copy / missed End / refused signal keeps the status engaged for a window of frames, not for ever.
			++endCount;
			const unsigned long long lateTotal = deferred.LateIssued() + missedEnds;
			const unsigned long long refusedTotal = LedDrive::SignalFailures().load(std::memory_order_relaxed);
			if (lateTotal != lateSeen || refusedTotal != refusedSeen) lastEventEnd = endCount;
			lateSeen = lateTotal;
			refusedSeen = refusedTotal;
			const bool recent = lastEventEnd != 0 && endCount - lastEventEnd < kRecentFrames;
			decision = FallbackCatalog::NativeReadback(LedDrive::FenceUnavailableReadbacks().load(std::memory_order_relaxed),
				recent ? lateTotal : 0, recent ? refusedTotal : 0);
		}

	private:
		SourceTime time;
		pl_frame frame{};
		uint64_t frameSequence = 0;
		SourceResolutionMonitor::Monitor* frameMonitor = nullptr;
		pl_dispatch frameDispatch = nullptr;
		pl_gpu armedGpu = nullptr;
		FallbackCatalog::Decision decision;
		unsigned long long missedEnds = 0, flushes = 0;
		static constexpr unsigned long long kRecentFrames = 600;   // about 25 s at 24p: a transient shows, then clears
		unsigned long long endCount = 0, lastEventEnd = 0, lateSeen = 0, refusedSeen = 0;
		bool pending = false, armed = false, boundNow = false;
	};
}
