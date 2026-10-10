#pragma once

#include "LedController.h"
#include "LedDriveConfig.h"

#include <cstdint>
#include <deque>
#include <vector>

// Layer 1 of the LED drive module, CPU side: turns the GPU pass's raw counts
// into FrameStats. The GPU pass (LedMeasurementHook) runs at PRE_OUTPUT on the
// picture before the compensation gain and measures 3x3 block means in
// limited-range code units (code = 16 + 219 x value).
namespace LedDrive
{
	constexpr int HISTOGRAM_BINS = 256;
	constexpr int GRID_W = 32;
	constexpr int GRID_H = 18;
	// Sums are stored in 1/16 code units so they stay integers on the GPU.
	constexpr uint32_t SUM_SCALE = 16;

	// Exactly the layout of the GPU pass's storage buffer.
	struct RawMeasurement
	{
		uint32_t histogram[HISTOGRAM_BINS];
		uint32_t cellSum[GRID_W * GRID_H];
		uint32_t cellCount[GRID_W * GRID_H];
		uint32_t sum;
		uint32_t blocks;
		// Brightest block, in SUM_SCALE units; 0 = not measured.
		uint32_t maximum;
	};

	// Normalised rectangle (0..1) of the hooked picture to measure.
	struct MeasurementRect
	{
		double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;
	};

	// The fork's trusted active picture (source raster pixels) expressed
	// within the displayed source crop; the whole picture when there is no
	// trusted rectangle or it does not overlap the crop.
	MeasurementRect ActivePictureRect(bool available, int left, int top, int right, int bottom,
		double cropX0, double cropY0, double cropX1, double cropY1);

	// Code at or above which `percentile` percent of blocks lie below.
	double HistogramPercentile(const uint32_t* histogram, uint32_t total, double percentile);

	class StatsBuilder
	{
	public:
		explicit StatsBuilder(const LedDriveConfig& config);
		// timeMs: the frame's time, for highlight persistence.
		FrameStats Build(const RawMeasurement& raw, double timeMs = 0.0);
		// Mean absolute difference of block luma against the previous frame,
		// in codes (the cut metric); 0 before there is a previous frame.
		double LastDifference() const { return lastDifference; }

		// (time, value) over a persistence window.
		struct Sample { double time; double value; };

	private:
		LedDriveConfig config;
		std::vector<double> previousCells;
		std::deque<double> aplHistory;
		std::deque<Sample> highlightHistory;
		std::deque<Sample> sustainHistory;
		double lastDifference = 0.0;
	};
}
