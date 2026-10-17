#pragma once

#include <cstdint>

// ABI 35 (VP Renderer DirectShow filter): timing that travels with a frame from
// a file source. Plain data with fixed-width fields; the layout is part of the
// plugin ABI (VP_LIBPLACEBO_PLUGIN_API_VERSION), so append fields only with an
// API bump. No rates are fixed here: every rate is measured or derived from the
// stream (AvgTimePerFrame snapped to a rational and checked against PTS deltas).
struct FileFrameTiming
{
	// sizeof(FileFrameTiming) as written by the sender; receivers reject a
	// smaller value.
	uint32_t structSize = sizeof(FileFrameTiming);

	// QueryPerformanceCounter tick on which the frame is due on screen, from the
	// graph reference clock (tStart + rtStart / rate) mapped to QPC, including
	// file_av_offset_ms. 0 = unknown (present as soon as possible).
	int64_t presentationQpc = 0;

	// Exact source frame rate as a rational (e.g. 24000/1001). Both 0 when
	// unknown; sourceRateHz is then the measured rate or 0.
	uint32_t sourceRateNumerator = 0;
	uint32_t sourceRateDenominator = 0;
	double sourceRateHz = 0.0;

	// Graph playback rate (NewSegment rate). 1.0 = normal. At any other rate the
	// renderer scales the film clock and turns motion off.
	double playbackRate = 1.0;

	// Source sample time (100 ns units, segment relative) for diagnostics.
	int64_t sampleStart100ns = 0;
	int64_t sampleStop100ns = 0;

	// First frame after a flush, seek, frame step or rate change: the renderer
	// invalidates vector/temporal history (one vectors-valid state).
	bool discontinuity = false;
	// True for file sources (always true from the filter); reserved so a future
	// timed capture source can reuse the path.
	bool isFileSource = true;
	// The graph is paused and this frame is the held picture (pre-roll or
	// frame step); it is presented and held.
	bool paused = false;
	bool reserved0 = false;
};
