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

	MeasurementRect SourceRectInCrop(int left, int top, int right, int bottom,
		double cropX0, double cropY0, double cropX1, double cropY1)
	{
		MeasurementRect empty{ 0.0, 0.0, 0.0, 0.0 };
		const double width = cropX1 - cropX0, height = cropY1 - cropY0;
		if (width <= 0.0 || height <= 0.0 || right <= left || bottom <= top) return empty;
		auto clamp01 = [](double v) { return std::max(0.0, std::min(v, 1.0)); };
		MeasurementRect r;
		r.x0 = clamp01((left - cropX0) / width);
		r.x1 = clamp01((right - cropX0) / width);
		r.y0 = clamp01((top - cropY0) / height);
		r.y1 = clamp01((bottom - cropY0) / height);
		return Empty(r) ? empty : r;
	}

	PeakGrid AreaPeakGrid(double rectW, double rectH, double screenW, double screenH)
	{
		PeakGrid grid;
		if (rectW <= 0.0 || rectH <= 0.0) return grid;
		if (screenW <= 0.0) screenW = rectW;
		if (screenH <= 0.0) screenH = rectH;
		// The script's box in pixels of this screen, never under one pixel.
		const double boxW = std::max(1.0, screenW / SCRIPT_PEAK_GRID_W);
		const double boxH = std::max(1.0, screenH / SCRIPT_PEAK_GRID_H);
		grid.boxesX = std::max(1, static_cast<int>(std::lround(rectW / boxW)));
		grid.boxesY = std::max(1, static_cast<int>(std::lround(rectH / boxH)));
		// Never a box under a pixel after the stretch (rounding up near 1 px).
		grid.boxesX = std::min(grid.boxesX, std::max(1, static_cast<int>(std::floor(rectW))));
		grid.boxesY = std::min(grid.boxesY, std::max(1, static_cast<int>(std::floor(rectH))));
		grid.boxW = rectW / grid.boxesX;
		grid.boxH = rectH / grid.boxesY;
		return grid;
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

	namespace
	{
		// The lowest value of a window that always includes the newest sample
		// and the newest one from before the window began.
		double WindowMinimum(std::deque<StatsBuilder::Sample>& history, double timeMs, double value, int windowMs)
		{
			history.push_back({ timeMs, value });
			while (history.size() > 1 && history[1].time <= timeMs - windowMs)
				history.pop_front();
			double minimum = value;
			for (const StatsBuilder::Sample& sample : history)
				minimum = std::min(minimum, sample.value);
			return minimum;
		}
	}

	FrameStats StatsBuilder::Build(const RawMeasurement& raw, double timeMs)
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

		// The script's highlight test used the maximum of its downscale.
		stats.maximum = raw.maximum > 0 ? static_cast<double>(raw.maximum) / SUM_SCALE : stats.highlight;

		// Sustained highlight: minimum over highlight_persist_ms, so a flash
		// shorter than that never raises it.
		stats.sustainedHighlight = WindowMinimum(highlightHistory, timeMs, stats.highlight,
			config.highlightPersistMs);

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

		// Sustained APL: minimum over apl_persist_ms, restarted at a cut so
		// the new scene's APL counts at once (a dark-to-bright cut must not
		// wait out the window; a flash without a cut still does).
		if (stats.cut) sustainHistory.clear();
		stats.sustainedApl = WindowMinimum(sustainHistory, timeMs, stats.apl, config.aplPersistMs);

		// Fade (in time, any frame rate): APL fell between every pair of
		// frames over at least the last fade_ms, each step at no less than
		// fade_min_slope per second, with no cut among them.
		if (stats.cut) aplHistory.clear();
		aplHistory.push_back({ timeMs, stats.apl });
		// Keep the newest sample at or before the window's start, so the
		// samples kept span the whole window.
		while (aplHistory.size() > 2 && aplHistory[1].time <= timeMs - config.fadeMs + 1e-6)
			aplHistory.pop_front();
		if (aplHistory.size() >= 2 && timeMs - aplHistory.front().time >= config.fadeMs - 1e-6)
		{
			bool falling = true;
			for (size_t i = 1; i < aplHistory.size(); ++i)
			{
				const double seconds = std::max(0.0, aplHistory[i].time - aplHistory[i - 1].time) / 1000.0;
				const double drop = aplHistory[i - 1].value - aplHistory[i].value;
				falling = falling && drop > 0.0 && drop >= config.fadeMinSlope * seconds - 1e-9;
			}
			stats.fade = falling;
		}
		return stats;
	}
}
