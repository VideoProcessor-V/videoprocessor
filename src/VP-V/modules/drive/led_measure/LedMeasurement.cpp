#include <pch.h>
#include "LedMeasurement.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#if defined(__AVX__)
#include <immintrin.h>
#endif

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

	AreaPeakAccumulator::AreaPeakAccumulator(int widthIn, int heightIn, double screenW, double screenH)
		: width(std::max(0, widthIn)), height(std::max(0, heightIn)),
		grid(AreaPeakGrid(width, height, screenW, screenH))
	{
		if (width <= 0 || height <= 0) return;
		// One pixel's split between its box and the next: AreaAveragedMaximum's
		// spans at stride 1, the same arithmetic.
		auto split = [](int i, int count, double boxSize, int boxCount, int& box, double& w0)
		{
			const double a = i, b = i + 1.0;
			box = std::min(boxCount - 1, static_cast<int>(std::floor(a / boxSize)));
			const double edge = box + 1 < boxCount ? (box + 1) * boxSize : static_cast<double>(count);
			w0 = std::min(b, edge) - a;
		};
		// Each pixel's (box, weight) pairs across the row: a whole pixel gives 1
		// to its box, a pixel on a box edge its two shares.
		std::vector<int> first(grid.boxesX, width), last(grid.boxesX, -1);
		std::vector<std::pair<int, double>> pairs[2];
		pairs[0].resize(width);
		pairs[1].assign(width, { -1, 0.0 });
		for (int x = 0; x < width; ++x)
		{
			int box = 0;
			double w0 = 0.0;
			split(x, width, grid.boxW, grid.boxesX, box, w0);
			if (w0 >= 1.0) pairs[0][x] = { box, 1.0 };
			else if (w0 <= 0.0) pairs[0][x] = { std::min(box + 1, grid.boxesX - 1), 1.0 };
			else
			{
				pairs[0][x] = { box, w0 };
				if (box + 1 < grid.boxesX) pairs[1][x] = { box + 1, 1.0 - w0 };
			}
			for (const auto& list : pairs)
			{
				const int b = list[x].first;
				if (b < 0) continue;
				first[b] = std::min(first[b], x);
				last[b] = std::max(last[b], x);
			}
		}
		// Per box column: a run of `taps` weights from its first pixel (zero
		// past its last), so the box pass is branch-free.
		taps = 1;
		for (int b = 0; b < grid.boxesX; ++b)
		{
			if (last[b] < first[b]) first[b] = last[b] = 0;
			taps = std::max(taps, last[b] - first[b] + 1);
		}
		boxStart.assign(first.begin(), first.end());
		boxTaps.assign(static_cast<size_t>(grid.boxesX) * taps, 0.0f);
		boxAreaTaps.assign(boxTaps.size(), 0.0);
		boxArea.assign(grid.boxesX, 0.0);
		boxAreaF.assign(static_cast<size_t>(grid.boxesX) + 8, 0.0f);
		for (int x = 0; x < width; ++x)
			for (const auto& list : pairs)
			{
				const int b = list[x].first;
				if (b < 0) continue;
				const size_t at = static_cast<size_t>(b) * taps + (x - first[b]);
				boxTaps[at] = static_cast<float>(list[x].second);
				boxAreaTaps[at] = list[x].second;
				boxArea[b] += list[x].second;
			}
		for (int b = 0; b < grid.boxesX; ++b)
			boxAreaF[b] = static_cast<float>(boxArea[b]);
		rowBox.resize(height);
		rowWeight.resize(height);
		for (int y = 0; y < height; ++y)
			split(y, height, grid.boxH, grid.boxesY, rowBox[y], rowWeight[y]);
		// Column sums padded with zeros so every box's run (at least eight
		// floats for the vector box pass) stays in range.
		pad = std::max(taps, 8);
		for (Slot& slot : slots)
			slot.values.assign(static_cast<size_t>(width) + pad, 0.0f);
		scratch.assign(static_cast<size_t>(width) + pad, 0.0f);
		for (auto& copy : copies)
			copy.resize(width);
		boxSums.assign(static_cast<size_t>(grid.boxesX) + 8, 0.0f);
	}

	namespace
	{
		// The row pass: sums += the K rows' table values (whole rows of one box
		// row, weight 1), K table reads per load/store of the column sum. Two
		// 16-bit codes per 32-bit read; Shift < 0 = `shift` at run time.
		template <int K, int Shift>
		void AddRows(float* sums, const uint16_t* const* rows, int shift, const float* table, int width)
		{
			const int s = Shift >= 0 ? Shift : shift;
			auto pair = [&](int k, int x, float& lo, float& hi)
			{
				uint32_t v;
				std::memcpy(&v, rows[k] + x, sizeof(v));
				lo = table[(v & 0xffffu) >> s];
				hi = table[v >> (16 + s)];
			};
			int x = 0;
			for (; x + 2 <= width; x += 2)
			{
				float l0, h0, l1 = 0.0f, h1 = 0.0f, l2 = 0.0f, h2 = 0.0f, l3 = 0.0f, h3 = 0.0f;
				pair(0, x, l0, h0);
				if (K > 1) pair(1, x, l1, h1);
				if (K > 2) pair(2, x, l2, h2);
				if (K > 3) pair(3, x, l3, h3);
				sums[x] += (l0 + l1) + (l2 + l3);
				sums[x + 1] += (h0 + h1) + (h2 + h3);
			}
			for (; x < width; ++x)
				for (int k = 0; k < K; ++k)
					sums[x] += table[rows[k][x] >> s];
		}

		template <int Shift>
		void AddRowsBatch(int count, float* sums, const uint16_t* const* rows, int shift, const float* table, int width)
		{
			switch (count)
			{
			case 1: AddRows<1, Shift>(sums, rows, shift, table, width); break;
			case 2: AddRows<2, Shift>(sums, rows, shift, table, width); break;
			case 3: AddRows<3, Shift>(sums, rows, shift, table, width); break;
			default: AddRows<4, Shift>(sums, rows, shift, table, width); break;
			}
		}

		// A row with a weight into one or two box rows.
		template <int Shift>
		void AddWeightedRow(float* a, float* b, const uint16_t* codes, int shift, const float* table,
			float wa, float wb, int width)
		{
			const int s = Shift >= 0 ? Shift : shift;
			if (b)
				for (int x = 0; x < width; ++x)
				{
					const float v = table[codes[x] >> s];
					a[x] += wa * v;
					b[x] += wb * v;
				}
			else
				for (int x = 0; x < width; ++x)
					a[x] += wa * table[codes[x] >> s];
		}
	}

	void AreaPeakAccumulator::Flush()
	{
		if (pendingCount == 0) return;
		float* sums = slots[pendingBoxRow2 & 1].values.data();
		if (pendingShift == 6)        // a 16-bit plane's ten-bit codes
			AddRowsBatch<6>(pendingCount, sums, pending, 6, pendingTable, width);
		else if (pendingShift == 0)   // decoded codes
			AddRowsBatch<0>(pendingCount, sums, pending, 0, pendingTable, width);
		else
			AddRowsBatch<-1>(pendingCount, sums, pending, pendingShift, pendingTable, width);
		pendingCount = 0;
	}

	void AreaPeakAccumulator::Finish(int boxRow)
	{
		Slot& slot = slots[boxRow & 1];
		const double needed = 0.5 * grid.boxW * grid.boxH - 1e-9;
		const float* v = slot.values.data();
		double brightest = -1.0;
		if (!slot.masked)
		{
			// Each box column's weighted sum, then the brightest qualifying mean
			// (every box column's area is the same share, slot.weight, of its own).
			float* sums = boxSums.data();
			int b = 0;
#if defined(__AVX__)
			if (taps <= 8)
			{
				// Eight boxes at a time: eight 8-wide products, reduced across.
				if (boxTaps8.empty())
				{
					boxTaps8.assign(static_cast<size_t>(grid.boxesX) * 8, 0.0f);
					for (int c = 0; c < grid.boxesX; ++c)
						for (int t = 0; t < taps; ++t)
							boxTaps8[static_cast<size_t>(c) * 8 + t] = boxTaps[static_cast<size_t>(c) * taps + t];
				}
				const float* w8 = boxTaps8.data();
				for (; b + 8 <= grid.boxesX; b += 8)
				{
					__m256 p[8];
					for (int j = 0; j < 8; ++j)
						p[j] = _mm256_mul_ps(_mm256_loadu_ps(v + boxStart[b + j]), _mm256_loadu_ps(w8 + (b + j) * 8));
					const __m256 s0123 = _mm256_hadd_ps(_mm256_hadd_ps(p[0], p[1]), _mm256_hadd_ps(p[2], p[3]));
					const __m256 s4567 = _mm256_hadd_ps(_mm256_hadd_ps(p[4], p[5]), _mm256_hadd_ps(p[6], p[7]));
					_mm256_storeu_ps(sums + b, _mm256_add_ps(_mm256_permute2f128_ps(s0123, s4567, 0x20),
						_mm256_permute2f128_ps(s0123, s4567, 0x31)));
				}
			}
#endif
			for (; b < grid.boxesX; ++b)
			{
				const float* run = v + boxStart[b];
				const float* w = boxTaps.data() + static_cast<size_t>(b) * taps;
				float s = 0.0f;
				for (int t = 0; t < taps; ++t) s += w[t] * run[t];
				sums[b] = s;
			}
			b = 0;
#if defined(__AVX__)
			// Eight means at a time (float; candidates are confirmed in double).
			{
				const __m256 weight = _mm256_set1_ps(static_cast<float>(slot.weight));
				const __m256 need = _mm256_set1_ps(static_cast<float>(needed) * (1.0f - 1e-5f));   // a filter only
				__m256 top = _mm256_set1_ps(-1.0f);
				for (; b + 8 <= grid.boxesX; b += 8)
				{
					const __m256 area = _mm256_mul_ps(weight, _mm256_loadu_ps(boxAreaF.data() + b));
					const __m256 ok = _mm256_and_ps(_mm256_cmp_ps(area, _mm256_setzero_ps(), _CMP_GT_OQ),
						_mm256_cmp_ps(area, need, _CMP_GE_OQ));
					const __m256 mean = _mm256_div_ps(_mm256_loadu_ps(sums + b), area);
					top = _mm256_max_ps(top, _mm256_blendv_ps(_mm256_set1_ps(-1.0f), mean, ok));
				}
				alignas(32) float lanes[8];
				_mm256_store_ps(lanes, top);
				float candidate = -1.0f;
				for (float lane : lanes) candidate = std::max(candidate, lane);
				if (candidate >= 0.0f && candidate >= static_cast<float>(best) * (1.0f - 1e-5f))
				{
					// The exact (double) mean of the boxes that reached the float maximum.
					// A float maximum the double rules do not confirm (a box at the
					// half-area limit) rescans the row without the filter.
					const int vectorEnd = b;
					double rowBest = -1.0;
					for (int pass = 0; pass < 2 && rowBest < candidate * (1.0 - 1e-5); ++pass)
						for (int c = 0; c < vectorEnd; ++c)
						{
							const double area = slot.weight * boxArea[c];
							if (!(area > 0.0 && area >= needed)) continue;
							const float meanF = sums[c] / static_cast<float>(area);
							if (pass == 1 || meanF >= candidate * (1.0f - 1e-5f))
								rowBest = std::max(rowBest, sums[c] / area);
						}
					brightest = std::max(brightest, rowBest);
				}
			}
#endif
			for (; b < grid.boxesX; ++b)
			{
				const double area = slot.weight * boxArea[b];
				const double sum = sums[b];
				if (area > 0.0 && area >= needed && sum > brightest * area)
					brightest = sum / area;
			}
		}
		else
		{
			// Masked pixels in this box row: each box's own area.
			const double* a = slot.area.data();
			for (int b = 0; b < grid.boxesX; ++b)
			{
				const float* run = v + boxStart[b];
				const float* w = boxTaps.data() + static_cast<size_t>(b) * taps;
				const double* areaRun = a + boxStart[b];
				const double* areaTaps = boxAreaTaps.data() + static_cast<size_t>(b) * taps;
				double sum = 0.0, area = 0.0;
				for (int t = 0; t < taps; ++t)
				{
					sum += w[t] * run[t];
					area += areaTaps[t] * areaRun[t];
				}
				if (area > 0.0 && area >= needed && sum > brightest * area)
					brightest = sum / area;
			}
		}
		best = std::max(best, brightest);
		std::fill(slot.values.begin(), slot.values.begin() + width, 0.0f);
		slot.weight = 0.0;
		slot.masked = false;
	}

	void AreaPeakAccumulator::AddRow(const uint16_t* codes, int shift, const float* table,
		const Span* excluded, int excludedCount, bool codesStayValid)
	{
		if (nextRow >= height || boxStart.empty()) return;
		const int y = nextRow++;
		int boxRow = rowBox[y];
		double w0 = rowWeight[y], w1 = 1.0 - w0;
		if (w0 <= 0.0) { boxRow = std::min(boxRow + 1, grid.boxesY - 1); w0 = 1.0; w1 = 0.0; }
		if (boxRow + 1 >= grid.boxesY || w0 >= 1.0) w1 = 0.0;
		const bool whole = w0 == 1.0 && w1 <= 0.0 && excludedCount <= 0;
		// Rows batched for one box row are added before anything else.
		if (pendingCount > 0 && (!whole || boxRow != pendingBoxRow2 || table != pendingTable ||
			shift != pendingShift || pendingCount == MAX_PENDING))
			Flush();
		while (pendingBoxRow < boxRow) Finish(pendingBoxRow++);
		Slot* targets[2] = { &slots[boxRow & 1], &slots[(boxRow + 1) & 1] };
		const double weights[2] = { w0, w1 };
		if (excludedCount <= 0)
		{
			if (whole)
			{
				// Batched: the codes are kept (copied unless they stay valid).
				const uint16_t* row = codes;
				if (!codesStayValid)
				{
					std::copy(codes, codes + width, copies[pendingCount].begin());
					row = copies[pendingCount].data();
				}
				pending[pendingCount++] = row;
				pendingBoxRow2 = boxRow;
				pendingTable = table;
				pendingShift = shift;
			}
			else
			{
				float* second = w1 > 0.0 ? targets[1]->values.data() : nullptr;
				const float wa = static_cast<float>(w0), wb = static_cast<float>(w1);
				if (shift == 6)
					AddWeightedRow<6>(targets[0]->values.data(), second, codes, 6, table, wa, wb, width);
				else if (shift == 0)
					AddWeightedRow<0>(targets[0]->values.data(), second, codes, 0, table, wa, wb, width);
				else
					AddWeightedRow<-1>(targets[0]->values.data(), second, codes, shift, table, wa, wb, width);
			}
			for (int j = 0; j < 2; ++j)
			{
				if (weights[j] <= 0.0) continue;
				targets[j]->weight += weights[j];
				if (targets[j]->masked)
					for (int x = 0; x < width; ++x) targets[j]->area[x] += weights[j];
			}
			return;
		}
		// A masked row: excluded pixels add neither value nor area.
		float* t = scratch.data();
		for (int x = 0; x < width; ++x) t[x] = table[codes[x] >> shift];
		for (int i = 0; i < excludedCount; ++i)
			for (int x = std::max(0, excluded[i].x0); x < std::min(width, excluded[i].x1); ++x) t[x] = 0.0f;
		for (int j = 0; j < 2; ++j)
		{
			if (weights[j] <= 0.0) continue;
			Slot& slot = *targets[j];
			if (!slot.masked)
			{
				// From here this box row tracks its area per column.
				slot.area.assign(static_cast<size_t>(width) + pad, 0.0);
				std::fill(slot.area.begin(), slot.area.begin() + width, slot.weight);
				slot.masked = true;
			}
			const float w = static_cast<float>(weights[j]);
			float* a = slot.values.data();
			for (int x = 0; x < width; ++x) a[x] += w * t[x];
			int at = 0;
			for (int i = 0; i <= excludedCount; ++i)
			{
				const int stop = i < excludedCount ? std::max(at, std::min(width, excluded[i].x0)) : width;
				for (int x = at; x < stop; ++x) slot.area[x] += weights[j];
				if (i < excludedCount) at = std::max(at, std::min(width, excluded[i].x1));
			}
			slot.weight += weights[j];
		}
	}

	double AreaPeakAccumulator::Result()
	{
		if (boxStart.empty()) return -1.0;
		Flush();
		while (pendingBoxRow < grid.boxesY) Finish(pendingBoxRow++);
		return best < 0.0 ? -1.0 : 16.0 + 219.0 * std::max(0.0, std::min(best, 1.0));
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
			// Hunt 3 (LED1): time going backwards is a new time base (the old
			// samples would never expire): the window restarts. Bounded too.
			if (!history.empty() && timeMs < history.back().time)
				history.clear();
			history.push_back({ timeMs, value });
			while (history.size() > 1 && history[1].time <= timeMs - windowMs)
				history.pop_front();
			while (history.size() > StatsBuilder::MAX_WINDOW_SAMPLES)
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
		if (stats.cut || (!aplHistory.empty() && timeMs < aplHistory.back().time)) aplHistory.clear();
		aplHistory.push_back({ timeMs, stats.apl });
		while (aplHistory.size() > MAX_WINDOW_SAMPLES)
			aplHistory.pop_front();
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
