#pragma once

#include <VP-V/modules/drive/led_measure/FrameStats.h>
#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>

#include <algorithm>
#include <cmath>
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
		// The peak, in SUM_SCALE units; 0 = not measured: the largest area mean
		// over the whole picture (led_peak_full_coverage, AreaPeakGrid boxes) or
		// the brightest 3x3 block (off).
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

	// Hunt 2 (owner 2026-10-17, led_picture_only: measure the PICTURE ONLY):
	// a rectangle of source raster pixels (a detected subtitle) within the
	// displayed source crop, clipped to it; empty when it is invalid or
	// outside the crop. Unlike ActivePictureRect there is no sliver rule.
	MeasurementRect SourceRectInCrop(int left, int top, int right, int bottom,
		double cropX0, double cropY0, double cropX1, double cropY1);
	inline bool Empty(const MeasurementRect& r) { return !(r.x1 > r.x0 && r.y1 > r.y0); }

	// Hunt 2 (owner 2026-10-17, led_peak_full_coverage): the script's peak
	// was the maximum of its full-screen screenshot resized with cv2
	// INTER_AREA to this grid, so each of its values was the area mean of a
	// box (screen width / 854) x (screen height / 480). Only the reference
	// grid is fixed: the box is derived at run time from the real screen, so
	// it is the same share of the screen as the script's at any resolution.
	constexpr double SCRIPT_PEAK_GRID_W = 854.0;
	constexpr double SCRIPT_PEAK_GRID_H = 480.0;
	// Masks per frame (a subtitle's padded box and its card, where a moved
	// subtitle is shown, its layout cover and its cleanup fill).
	constexpr int MAX_MASK_RECTS = 6;

	// The boxes tiling a measured rectangle of rectW x rectH pixels: as many
	// as fit at the script's box size (screenW / 854 x screenH / 480 pixels,
	// at least one pixel), stretched to tile it exactly, so every pixel of
	// the rectangle falls in a box and no box is a sliver at its edge.
	struct PeakGrid
	{
		int boxesX = 0, boxesY = 0;
		double boxW = 1.0, boxH = 1.0;
	};
	PeakGrid AreaPeakGrid(double rectW, double rectH, double screenW, double screenH);

	// A rectangle in pixels (right/bottom exclusive). A pixel is masked when
	// its centre lies inside.
	struct PixelRect
	{
		double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
		bool Contains(double x, double y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
	};

	// The full-coverage peak, CPU side (the look-ahead's raw frames, and the
	// reference the GPU pass is tested against): the largest area mean of
	// the AreaPeakGrid boxes over [left,right) x [top,bottom); value(x, y, v)
	// gives a pixel's normalised signal v (0..1, already boosted) or false.
	// Masked pixels count neither value nor area; a box left with less than
	// half its area is skipped. Every pixel is read: this is the reference
	// (the look-ahead's fast path is AreaPeakAccumulator). Returns the code
	// (16 + 219 v), or -1 when no box was measured.
	template <typename Value>
	double AreaAveragedMaximum(Value&& value, int left, int top, int right, int bottom,
		double screenW, double screenH, const PixelRect* masks = nullptr, int maskCount = 0)
	{
		const int width = right - left, height = bottom - top;
		if (width <= 0 || height <= 0) return -1.0;
		const PeakGrid grid = AreaPeakGrid(width, height, screenW, screenH);
		struct Span { int at; int box; double w0, w1; };
		auto spans = [](int count, double boxSize, int boxes)
		{
			std::vector<Span> out;
			out.reserve(static_cast<size_t>(count));
			for (int i = 0; i < count; ++i)
			{
				const double a = i, b = i + 1.0;
				const int box = (std::min)(boxes - 1, static_cast<int>(std::floor(a / boxSize)));
				const double edge = box + 1 < boxes ? (box + 1) * boxSize : static_cast<double>(count);
				const double w0 = (std::min)(b, edge) - a;
				out.push_back({ i, box, w0, b - a - w0 });
			}
			return out;
		};
		const std::vector<Span> columns = spans(width, grid.boxW, grid.boxesX);
		const std::vector<Span> rows = spans(height, grid.boxH, grid.boxesY);
		std::vector<double> sum(static_cast<size_t>(grid.boxesX) * grid.boxesY, 0.0), area(sum.size(), 0.0);
		for (const Span& row : rows)
		{
			const int y = top + row.at;
			for (const Span& column : columns)
			{
				const int x = left + column.at;
				bool masked = false;
				for (int m = 0; m < maskCount && !masked; ++m)
					masked = masks[m].Contains(x + 0.5, y + 0.5);
				double v = 0.0;
				if (masked || !value(x, y, v)) continue;
				v = (std::max)(0.0, (std::min)(v, 1.0));
				const double wy[2] = { row.w0, row.w1 }, wx[2] = { column.w0, column.w1 };
				for (int j = 0; j < 2; ++j)
					for (int i = 0; i < 2; ++i)
					{
						const double w = wy[j] * wx[i];
						if (w <= 0.0) continue;
						const size_t index = static_cast<size_t>(row.box + j) * grid.boxesX + column.box + i;
						sum[index] += w * v;
						area[index] += w;
					}
			}
		}
		const double needed = 0.5 * grid.boxW * grid.boxH - 1e-9;
		double best = -1.0;
		for (size_t i = 0; i < sum.size(); ++i)
			if (area[i] > 0.0 && area[i] >= needed)
				best = (std::max)(best, sum[i] / area[i]);
		return best < 0.0 ? -1.0 : 16.0 + 219.0 * best;
	}

	// Hunt 2 (look-ahead cost, owner 2026-10-17): AreaAveragedMaximum over
	// every pixel (stride 1), fed one picture row at a time as 10-bit codes
	// and a code -> value table (values 0..1). The same boxes, fractional
	// edge weights, masks and half-area rule; equal to the template within
	// float rounding. Separable: each row is added (with its row weight)
	// into the column sums of the one or two box rows it falls in, and a box
	// row's boxes are summed across once it is complete, so a 4K frame costs
	// about a table read and an add per pixel.
	class AreaPeakAccumulator
	{
	public:
		AreaPeakAccumulator(int width, int height, double screenW, double screenH);
		// Interval of excluded pixels [x0, x1), relative to the row start.
		struct Span { int x0, x1; };
		// The next row (0..height-1, in order): codes[0..width) shifted right
		// by `shift` (a plane's upper bits, or 0) index `table`, whose values
		// must be 0..1. excluded: sorted, disjoint spans whose pixels count
		// neither value nor area (masks). Whole rows of one box row are
		// batched (a few table reads per column-sum update); codesStayValid:
		// `codes` (and `table`) stay readable until Result() (rows of a frame
		// read in place), else the codes are copied.
		void AddRow(const uint16_t* codes, int shift, const float* table,
			const Span* excluded = nullptr, int excludedCount = 0, bool codesStayValid = false);
		// The code (16 + 219 v) of the brightest box, or -1 when no box was
		// measured; completes the remaining box rows.
		double Result();
		const PeakGrid& Grid() const { return grid; }

	private:
		struct Slot { std::vector<float> values; std::vector<double> area; double weight = 0.0; bool masked = false; };
		static constexpr int MAX_PENDING = 4;
		void Flush();
		void Finish(int boxRow);
		int width, height;
		PeakGrid grid;
		int taps = 1;                     // pixels spanned by the widest box column
		int pad = 8;                      // zero floats after the column sums
		std::vector<int> boxStart;        // per box column: its first pixel
		std::vector<float> boxTaps;       // per box column: `taps` pixel weights from boxStart
		std::vector<float> boxTaps8;      // the same padded to eight (vector box pass)
		std::vector<double> boxAreaTaps;  // the same in double (the masked area)
		std::vector<double> boxArea;      // per box column: the sum of its weights
		std::vector<float> boxAreaF;      // the same in float (vector comparison)
		std::vector<float> boxSums;
		std::vector<int> rowBox;          // per pixel row: its (first) box row
		std::vector<double> rowWeight;    // per pixel row: its share of that box row
		Slot slots[2];                    // box rows r and r + 1 (by r & 1)
		std::vector<float> scratch;
		// Whole rows waiting to be added to box row pendingBoxRow2.
		const uint16_t* pending[MAX_PENDING] = {};
		std::vector<uint16_t> copies[MAX_PENDING];
		int pendingCount = 0, pendingBoxRow2 = 0, pendingShift = 0;
		const float* pendingTable = nullptr;
		int nextRow = 0, pendingBoxRow = 0;
		double best = -1.0;
	};

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
		// Hunt 3 (LED1, tests): the highlight persistence window's samples.
		size_t HighlightHistorySize() const { return highlightHistory.size(); }
		// Hunt 3 (LED1): the most samples a persistence window keeps (a time
		// base that stalls cannot grow it without limit).
		static constexpr size_t MAX_WINDOW_SAMPLES = 4096;

		// (time, value) over a persistence window.
		struct Sample { double time; double value; };

	private:
		LedDriveConfig config;
		std::vector<double> previousCells;
		std::deque<Sample> aplHistory;   // (time, APL) over the fade window
		std::deque<Sample> highlightHistory;
		std::deque<Sample> sustainHistory;
		double lastDifference = 0.0;
	};
}
