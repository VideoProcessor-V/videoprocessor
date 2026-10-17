#pragma once

#include <FileFrameTiming.h>

#include <cmath>
#include <cstdint>

// ABI 35 (VP Renderer DirectShow filter): the film clock of a file frame,
// computed from its FileFrameTiming alone (no capture rate measurement, no
// pulldown detection). Pure, C++14, unit tested. No rate is fixed here: every
// value comes from the stream's timing.
namespace FileFrameClock
{
	// A timing block the renderer may read: the sender wrote at least the
	// fields this build knows.
	inline bool TimingUsable(const FileFrameTiming& timing)
	{
		return timing.structSize >= sizeof(FileFrameTiming);
	}

	// The source frame rate: the exact rational when both terms are given,
	// else the measured rate; 0 when unknown or invalid.
	inline double SourceRateHz(const FileFrameTiming& timing)
	{
		if (timing.sourceRateNumerator != 0 && timing.sourceRateDenominator != 0)
			return static_cast<double>(timing.sourceRateNumerator) /
				static_cast<double>(timing.sourceRateDenominator);
		const double hz = timing.sourceRateHz;
		return std::isfinite(hz) && hz > 0.0 ? hz : 0.0;
	}

	// The graph playback rate when valid (finite, positive); 0 otherwise.
	inline double PlaybackRate(const FileFrameTiming& timing)
	{
		const double rate = timing.playbackRate;
		return std::isfinite(rate) && rate > 0.0 ? rate : 0.0;
	}

	// The rate at which this stream's frames reach the screen: source rate
	// times playback rate. 0 when either is unknown or invalid (the renderer
	// then falls back to the signal's nominal rate, as for capture).
	inline double FilmHz(const FileFrameTiming& timing)
	{
		return SourceRateHz(timing) * PlaybackRate(timing);
	}

	// True only at normal speed (rate exactly 1, as DirectShow passes it), not
	// paused, with a known film rate: any other frame shows without motion
	// (de-judder / interpolation off).
	inline bool MotionAllowed(const FileFrameTiming& timing)
	{
		return !timing.paused && timing.playbackRate == 1.0 && FilmHz(timing) > 0.0;
	}

	// The frame's place on the QPC clock for the timeline: its due time when
	// known, else the moment it was handed over.
	inline int64_t ArrivalQpc(const FileFrameTiming& timing, int64_t handedOverQpc)
	{
		return timing.presentationQpc > 0 ? timing.presentationQpc : handedOverQpc;
	}

	// The dequeue gate. A frame may leave the queue when it is paused (shown
	// and held at once), when its due time is unknown, or once now has
	// reached its due time less the measured presentation lead.
	inline bool Due(int64_t nowQpc, int64_t presentationQpc, int64_t leadQpc, bool paused)
	{
		if (paused || presentationQpc <= 0)
			return true;
		return nowQpc >= presentationQpc - (leadQpc > 0 ? leadQpc : 0);
	}

	// How long to wait (QPC ticks) before the frame is due; 0 when due.
	inline int64_t TicksUntilDue(int64_t nowQpc, int64_t presentationQpc, int64_t leadQpc, bool paused)
	{
		if (Due(nowQpc, presentationQpc, leadQpc, paused))
			return 0;
		return presentationQpc - (leadQpc > 0 ? leadQpc : 0) - nowQpc;
	}
}
