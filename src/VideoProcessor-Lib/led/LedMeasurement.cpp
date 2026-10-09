#include <pch.h>
#include "LedMeasurement.h"

#include <algorithm>
#include <cmath>

namespace LedDrive
{
	MeasurementRect ActivePictureRect(bool available, int left, int top, int right, int bottom,
		double cropX0, double cropY0, double cropX1, double cropY1)
	{
		MeasurementRect rect;
		const double width = cropX1 - cropX0;
		const double height = cropY1 - cropY0;
		if (!available || width <= 0.0 || height <= 0.0 || right <= left || bottom <= top)
			return rect;
		auto clamp01 = [](double v) { return std::max(0.0, std::min(v, 1.0)); };
		MeasurementRect active;
		active.x0 = clamp01((left - cropX0) / width);
		active.x1 = clamp01((right - cropX0) / width);
		active.y0 = clamp01((top - cropY0) / height);
		active.y1 = clamp01((bottom - cropY0) / height);
		// Ignore slivers: an active area under 5% of an axis is not trusted.
		if (active.x1 - active.x0 < 0.05 || active.y1 - active.y0 < 0.05)
			return rect;
		return active;
	}

	double HistogramPercentile(const uint32_t* histogram, uint32_t total, double percentile)
	{
		if (total == 0) return 16.0;
		const double above = total * (100.0 - percentile) / 100.0;
		double counted = 0.0;
		for (int code = HISTOGRAM_BINS - 1; code >= 0; --code)
		{
			counted += histogram[code];
			if (counted > above) return code;
		}
		return 0.0;
	}

	StatsBuilder::StatsBuilder(const LedDriveConfig& configIn) : config(configIn)
	{
	}

	FrameStats StatsBuilder::Build(const RawMeasurement& raw)
	{
		FrameStats stats;
		if (raw.blocks == 0) return stats;
		stats.blocks = raw.blocks;
		std::copy(raw.histogram, raw.histogram + HISTOGRAM_BINS, stats.histogram.begin());
		stats.apl = static_cast<double>(raw.sum) / SUM_SCALE / raw.blocks;
		stats.highlight = HistogramPercentile(raw.histogram, raw.blocks, config.highlightPercentile);
		// Until mode: compensate defines the knee threshold (step 4), the area is
		// measured against the script's highlight level.
		uint32_t bright = 0;
		for (int code = std::max(0, std::min(config.scriptHighlightCode, 255)); code < HISTOGRAM_BINS; ++code)
			bright += raw.histogram[code];
		stats.highlightArea = static_cast<double>(bright) / raw.blocks;

		std::vector<double> cells(GRID_W * GRID_H, -1.0);
		for (int i = 0; i < GRID_W * GRID_H; ++i)
			if (raw.cellCount[i] > 0)
				cells[i] = static_cast<double>(raw.cellSum[i]) / SUM_SCALE / raw.cellCount[i];
		double difference = 0.0;
		int compared = 0;
		if (previousCells.size() == cells.size())
			for (size_t i = 0; i < cells.size(); ++i)
				if (cells[i] >= 0.0 && previousCells[i] >= 0.0)
				{
					difference += std::abs(cells[i] - previousCells[i]);
					++compared;
				}
		lastDifference = compared > 0 ? difference / compared : 0.0;
		stats.cut = compared > 0 && lastDifference > config.cutThreshold;
		previousCells = cells;

		// Fade: APL fell by at least fade_min_step on each of the last
		// fade_frames frames, with no cut among them.
		if (stats.cut) aplHistory.clear();
		aplHistory.push_back(stats.apl);
		while (static_cast<int>(aplHistory.size()) > config.fadeFrames + 1) aplHistory.pop_front();
		if (static_cast<int>(aplHistory.size()) == config.fadeFrames + 1)
		{
			bool falling = true;
			for (size_t i = 1; i < aplHistory.size(); ++i)
				falling = falling && aplHistory[i - 1] - aplHistory[i] >= config.fadeMinStep;
			stats.fade = falling;
		}
		return stats;
	}
}
