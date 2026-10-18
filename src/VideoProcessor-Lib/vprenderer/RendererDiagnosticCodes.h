#pragma once

// Report-a-problem (2026-10-17): the codes the renderer DLL writes into
// DiagnosticHistory::FrameRecord::holdReason, so the report README can explain
// a "hold" value in frame rows. 0 = none. Stable: append, never renumber.
//
// StageDropped rows (a frame that never reached the screen):
//    1 no video state when the frame arrived
//    2 queue full of authorised repeats: the incoming frame dropped
//    3 queue hard overflow: the oldest frame dropped
//    4 active-picture identity rejected
//    5 enqueue failed (exception)
//    6 render or present failed
//    7 backlog flush (queued latency shed)
//    8 conversion failed
//    9 drift-correction drop (cadence correction)
//   10 queue resized below its depth
// StageRepeated rows (de-judder / render-ahead: the frame shown for its
// refreshes without extra pictures; the decision ring carries the text):
//   20 other / unclassified hold
//   21 no motion estimate for this frame
//   22 motion vectors invalid (a discontinuity since the estimate)
//   23 cadence repeat
//   24 composed source (subtitle moving)
//   25 composed or RGB source without the rendered picture
//   26 no flip presentation
//   27 timing not known (source rate, display rate or arrival)
//   28 frame work too long for extras
//   29 no swapchain, output or present id
//   30 render-ahead: no plan (timing)
//   31 render-ahead gate at hold (extras dropped)
//   32 render-ahead: extras dropped (gate or late)
//   33 render-ahead: no refresh after the frame's own (cadence)
//   34 render-ahead: every following refresh is a hold (phase)
//   35 an extra failed or was late
//   36 de-judder planner stopped (next frame due, extra limit, ...)
//   37 the frame after extras showed a refresh late
//   38 cadence-correction repeat queued (StageRepeated from the render loop)
//
// Other renderer frame-row fields: "presented" rows carry render_ms (render
// call), upload_ms (CPU source upload), wait_ms (Present blocking),
// interval_ms (since the previous submit), lead_ms (target lead), latency_ms
// (capture callback -> Present returned), phase (render-ahead only: the main
// present's SyncInterval), repeat (1 = cadence repeat), w/h (source size),
// repeated_total (de-judder holds so far). Hold rows (20-37) carry the holds so
// far in repeated_total; code 38 rows the drift-correction repeats so far.
// "renderer_ingest" rows: queue = depth after the frame was queued; flag
// 128 = a gap before it. Dropped rows: dropped_total where known.

#include <cstdint>
#include <cstring>

namespace RendererDiagnostics
{
	enum HoldReason : uint32_t
	{
		HoldNone = 0,
		DropNoVideoState = 1,
		DropQueueFullOfRepeats = 2,
		DropQueueOverflow = 3,
		DropIdentityRejected = 4,
		DropEnqueueFailed = 5,
		DropRenderFailed = 6,
		DropBacklogFlush = 7,
		DropConversionFailed = 8,
		DropDriftCorrection = 9,
		DropQueueResize = 10,
		HoldOther = 20,
		HoldNoMotionEstimate = 21,
		HoldVectorsInvalid = 22,
		HoldCadenceRepeat = 23,
		HoldComposedSubtitle = 24,
		HoldComposedWithoutPicture = 25,
		HoldNoFlip = 26,
		HoldTimingUnknown = 27,
		HoldWorkTooLong = 28,
		HoldNoSwapchain = 29,
		HoldRenderAheadNoPlan = 30,
		HoldRenderAheadGate = 31,
		HoldRenderAheadDropped = 32,
		HoldRenderAheadCadence = 33,
		HoldRenderAheadPhase = 34,
		HoldExtraFailed = 35,
		HoldPlannerStop = 36,
		HoldNextFrameLate = 37,
		RepeatCadenceCorrection = 38,
	};

	// Maps a de-judder hold text (DejudderHold's constant strings, MotionValidity's
	// causes, DejudderPolicy's stops) to its code. Prefix compares only: no
	// allocation; called once per held frame.
	inline uint32_t ClassifyDejudderHold(const char* why) noexcept
	{
		if (!why)
			return HoldOther;
		struct Entry { const char* prefix; uint32_t code; };
		static const Entry table[] = {
			{ "no motion estimate", HoldNoMotionEstimate },
			{ "cadence repeat", HoldCadenceRepeat },
			{ "composed source without", HoldComposedWithoutPicture },
			{ "RGB source", HoldComposedWithoutPicture },
			{ "composed source", HoldComposedSubtitle },
			{ "no flip", HoldNoFlip },
			{ "timing not known", HoldTimingUnknown },
			{ "frame work too long", HoldWorkTooLong },
			{ "no DXGI swapchain", HoldNoSwapchain },
			{ "no output for vsync", HoldNoSwapchain },
			{ "no present id", HoldNoSwapchain },
			{ "render-ahead: no plan", HoldRenderAheadNoPlan },
			{ "render-ahead gate: hold", HoldRenderAheadGate },
			{ "render-ahead: extras dropped", HoldRenderAheadDropped },
			{ "render-ahead: no refresh", HoldRenderAheadCadence },
			{ "render-ahead: every refresh", HoldRenderAheadPhase },
			{ "the frame after extras", HoldNextFrameLate },
			// MotionValidity::Describe causes (the estimate's epoch is over).
			{ "resync or source change", HoldVectorsInvalid },
			{ "live queue reset", HoldVectorsInvalid },
			{ "frame gap", HoldVectorsInvalid },
			{ "drift correction drop", HoldVectorsInvalid },
			{ "smooth motion mixing changed", HoldVectorsInvalid },
			{ "re-base factor changed", HoldVectorsInvalid },
			{ "source format changed", HoldVectorsInvalid },
			{ "seek or skip", HoldVectorsInvalid },
			{ "motion source changed", HoldVectorsInvalid },
			{ "motion settings changed", HoldVectorsInvalid },
			// Extra failures (PresentDejudderExtras' `failure`).
			{ "picture not seen", HoldExtraFailed },
			{ "too late for the refresh", HoldExtraFailed },
			{ "rendered warp failed", HoldExtraFailed },
			{ "interpolation failed", HoldExtraFailed },
			{ "warp failed", HoldExtraFailed },
			{ "no backbuffer", HoldExtraFailed },
			{ "backbuffer not renderable", HoldExtraFailed },
			{ "no swapchain frame", HoldExtraFailed },
			{ "extra finished after", HoldExtraFailed },
			// DejudderPolicy::Name(Stop).
			{ "next frame", HoldPlannerStop },
			{ "past the next frame", HoldPlannerStop },
			{ "extra limit", HoldPlannerStop },
			{ "none", HoldPlannerStop },
		};
		for (const Entry& entry : table)
			if (std::strncmp(why, entry.prefix, std::strlen(entry.prefix)) == 0)
				return entry.code;
		return HoldOther;
	}
}
