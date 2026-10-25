#include <pch.h>
#include "LedLookahead.h"

#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>

#include <algorithm>
#include <cmath>

namespace LedDrive
{
	RawTransfer IdentityRawTransfer()
	{
		RawTransfer identity(RAW_TRANSFER_SIZE);
		for (int i = 0; i < RAW_TRANSFER_SIZE; ++i)
			identity[i] = static_cast<float>(i) / (RAW_TRANSFER_SIZE - 1);
		return identity;
	}

	namespace
	{
		bool FullRangeRgbEncoding(VideoFrameEncoding encoding)
		{
			switch (encoding)
			{
			case VideoFrameEncoding::BGRA_8BIT:
			case VideoFrameEncoding::ARGB_8BIT:
			case VideoFrameEncoding::R12B:
			case VideoFrameEncoding::R12L:
				return true;
			default:
				return false;
			}
		}
	}

	bool SampleRemapsFullRange(const AnalysisLumaSource& luma)
	{
		// audit 2026-10-16: Sample() maps natively full-range sources (8-bit
		// and 12-bit RGB, the DV SDR-tunnel RGB, full-range YCbCr planes) to
		// the limited code domain: Y = 64 + 876/1023 * Y'.
		if (!kAnalysisLumaSampleLimitedDomain) return false;
		if (luma.format == AnalysisLumaFormat::SdrTunnelRgb) return true;
		if (luma.format == AnalysisLumaFormat::NativeRgb) return FullRangeRgbEncoding(luma.encoding);
		return luma.fullRangeYuv;
	}

	bool LumaIsFullRange(const AnalysisLumaSource& luma)
	{
		// audit 2026-10-16: with the limited-domain sampler, Sample() never
		// returns full-range codes (reading them as 0-1023 again was a double
		// conversion: full-range RGB black read ~6%).
		if (kAnalysisLumaSampleLimitedDomain) return false;
		return FullRangeRgbEncoding(luma.encoding);
	}

	LumaValueTable PeakValueTable(const AnalysisLumaSource& luma, const std::vector<float>& transfer, int range)
	{
		LumaValueTable table{};
		if (transfer.size() != RAW_TRANSFER_SIZE) return table;
		// audit 2026-10-16: source_range describes the captured codes. Where
		// Sample() remapped full-range codes to the limited domain, the source
		// code is recovered first, so auto and full read the remapped code as
		// limited (no second conversion) and a forced limited still reads the
		// source's own codes as 64-940.
		const bool remapped = SampleRemapsFullRange(luma);
		const bool full = range >= 0 ? range == 1 : (remapped || LumaIsFullRange(luma));
		for (int code = 0; code < LUMA_CODES; ++code)
		{
			const double sourceCode = remapped ? (code - 64.0) * 1023.0 / 876.0 : code;
			const double signal = full ? sourceCode / 1023.0 : (sourceCode - 64.0) / 876.0;
			const int index = static_cast<int>(std::lround(
				std::max(0.0, std::min(signal, 1.0)) * (RAW_TRANSFER_SIZE - 1)));
			table[code] = transfer[index];
		}
		return table;
	}

	double FullCoveragePeak(const AnalysisLumaSource& luma, int left, int top, int right, int bottom,
		const LumaValueTable& table, double screenW, double screenH, const PixelRect* masks, int maskCount)
	{
		left = std::max(0, left);
		top = std::max(0, top);
		right = std::min(luma.width, right);
		bottom = std::min(luma.height, bottom);
		const int width = right - left, height = bottom - top;
		if (width <= 0 || height <= 0 || !luma.IsValid()) return -1.0;
		maskCount = std::max(0, std::min(maskCount, MAX_MASK_RECTS));
		AreaPeakAccumulator accumulator(width, height, screenW, screenH);
		// The accumulator's values are the template's, clamped to 0..1.
		LumaValueTable clamped;
		for (int code = 0; code < LUMA_CODES; ++code)
			clamped[code] = std::max(0.0f, std::min(table[code], 1.0f));
		// The 16-bit plane formats are read in place, the table indexed by the
		// lane's upper ten bits; other formats read the frame's shared luma
		// decode (WP-23, Sample().luma per pixel) or are decoded a row at a time.
		const bool plane = luma.LumaPlaneRow(top) != nullptr;
		const bool shared = !plane && luma.SharedLumaRow(top) != nullptr;
		LumaValueTable planeTable{};
		if (plane)
			for (int code = 0; code < LUMA_CODES; ++code)
				planeTable[code] = clamped[luma.PlaneLuma(static_cast<uint16_t>(code))];
		std::vector<uint16_t> codes(plane || shared ? 0 : static_cast<size_t>(width));
		AreaPeakAccumulator::Span spans[MAX_MASK_RECTS];
		for (int y = top; y < bottom; ++y)
		{
			const uint16_t* planeRow = plane ? luma.LumaPlaneRow(y) : nullptr;
			const uint16_t* sharedRow = shared ? luma.SharedLumaRow(y) : nullptr;
			if (plane ? !planeRow : shared ? !sharedRow : !luma.SampleLumaRow(left, y, width, codes.data()))
			{
				// A format the row reader cannot decode: the per-pixel reference.
				auto value = [&](int x, int yy, double& v)
				{
					AnalysisLumaSample sample;
					if (!luma.Sample(x, yy, sample)) return false;
					v = table[std::min<int>(sample.luma, LUMA_CODES - 1)];
					return true;
				};
				return AreaAveragedMaximum(value, left, top, right, bottom, screenW, screenH, masks, maskCount);
			}
			// The masks crossing this row as pixel spans: a pixel is masked
			// when its centre lies inside (PixelRect::Contains).
			int count = 0;
			for (int m = 0; m < maskCount; ++m)
			{
				const PixelRect& r = masks[m];
				if (!(y + 0.5 >= r.y0 && y + 0.5 < r.y1)) continue;
				int x0 = static_cast<int>(std::max<double>(left, std::min<double>(right, std::ceil(r.x0 - 0.5))));
				int x1 = static_cast<int>(std::max<double>(left, std::min<double>(right, std::ceil(r.x1 - 0.5))));
				while (x0 > left && r.Contains(x0 - 1 + 0.5, y + 0.5)) --x0;
				while (x0 < x1 && !r.Contains(x0 + 0.5, y + 0.5)) ++x0;
				while (x1 < right && r.Contains(x1 + 0.5, y + 0.5)) ++x1;
				while (x1 > x0 && !r.Contains(x1 - 1 + 0.5, y + 0.5)) --x1;
				if (x1 > x0) spans[count++] = { x0 - left, x1 - left };
			}
			// Sorted and merged.
			std::sort(spans, spans + count, [](const auto& a, const auto& b) { return a.x0 < b.x0; });
			int merged = 0;
			for (int i = 0; i < count; ++i)
				if (merged > 0 && spans[i].x0 <= spans[merged - 1].x1)
					spans[merged - 1].x1 = std::max(spans[merged - 1].x1, spans[i].x1);
				else
					spans[merged++] = spans[i];
			if (plane)
				accumulator.AddRow(planeRow + left, 6, planeTable.data(), spans, merged, true);
			else if (shared)
				accumulator.AddRow(sharedRow + left, 0, clamped.data(), spans, merged);
			else
				accumulator.AddRow(codes.data(), 0, clamped.data(), spans, merged);
		}
		return accumulator.Result();
	}

	bool MeasureRawFrame(const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& rect,
		const std::vector<float>& transfer, RawMeasurement& out, int range, const RawMeasureOptions* optionsIn)
	{
		static const RawMeasureOptions defaults{};
		const RawMeasureOptions& options = optionsIn ? *optionsIn : defaults;
		out = RawMeasurement{};
		if (!luma.IsValid() || transfer.size() != RAW_TRANSFER_SIZE) return false;
		const int left = std::max(0, rect.left), top = std::max(0, rect.top);
		const int right = std::min(luma.width, rect.right), bottom = std::min(luma.height, rect.bottom);
		const int width = right - left, height = bottom - top;
		if (width < 3 || height < 3) return false;
		const int blocksX = std::min(96, width / 3), blocksY = std::min(54, height / 3);
		const double stepX = static_cast<double>(width) / blocksX;
		const double stepY = static_cast<double>(height) / blocksY;
		// One pixel's mapped (boosted) signal, 0..1, through the code table.
		const LumaValueTable table = PeakValueTable(luma, transfer, range);
		auto mapped = [&](int x, int y, double& value)
		{
			uint16_t code = 0;
			if (!luma.SampleLuma(x, y, code)) return false;   // WP-23: the shared decode when ready
			value = table[std::min<int>(code, LUMA_CODES - 1)];
			return true;
		};
		const int maskCount = std::max(0, std::min(options.maskCount, MAX_MASK_RECTS));
		for (int by = 0; by < blocksY; ++by)
			for (int bx = 0; bx < blocksX; ++bx)
			{
				const int ox = left + static_cast<int>(bx * stepX);
				const int oy = top + static_cast<int>(by * stepY);
				// Hunt 2 (led_picture_only): a block centred in a mask is left out.
				bool masked = false;
				for (int m = 0; m < maskCount && !masked; ++m)
					masked = options.masks[m].Contains(ox + 1.5, oy + 1.5);
				if (masked) continue;
				double acc = 0.0;
				int samples = 0;
				for (int dy = 0; dy < 3; ++dy)
					for (int dx = 0; dx < 3; ++dx)
					{
						double value = 0.0;
						if (!mapped(std::min(ox + dx, right - 1), std::min(oy + dy, bottom - 1), value))
							continue;
						acc += value;
						++samples;
					}
				if (samples == 0) continue;
				const double code = 16.0 + 219.0 * std::max(0.0, std::min(acc / samples, 1.0));
				const uint32_t scaled = static_cast<uint32_t>(code * SUM_SCALE + 0.5);
				out.histogram[std::min(static_cast<int>(code), HISTOGRAM_BINS - 1)] += 1;
				out.sum += scaled;
				out.blocks += 1;
				out.maximum = std::max(out.maximum, scaled);
				const int cell = std::min(by * GRID_H / blocksY, GRID_H - 1) * GRID_W +
					std::min(bx * GRID_W / blocksX, GRID_W - 1);
				out.cellSum[cell] += scaled;
				out.cellCount[cell] += 1;
			}
		if (out.blocks > 0 && options.peakFullCoverage)
		{
			// Hunt 2 (led_peak_full_coverage): the peak over the whole picture,
			// every pixel counted.
			const double peak = FullCoveragePeak(luma, left, top, right, bottom, table,
				options.screenWidth > 0.0 ? options.screenWidth : luma.width,
				options.screenHeight > 0.0 ? options.screenHeight : luma.height,
				options.masks, maskCount);
			out.maximum = peak >= 0.0 ? static_cast<uint32_t>(peak * SUM_SCALE + 0.5) : 0u;
		}
		return out.blocks > 0;
	}

	LookaheadObserver::LookaheadObserver(LedDriveModule& moduleIn)
		: module(moduleIn), builder(LedDriveConfig())
	{
	}

	void LookaheadObserver::SetPictureGeometry(double screenWidth, double screenHeight,
		const PixelRect* masks, int count)
	{
		std::lock_guard<std::mutex> lock(geometryMutex);
		geometry.screenWidth = screenWidth;
		geometry.screenHeight = screenHeight;
		geometry.maskCount = 0;
		for (int i = 0; masks && i < count && geometry.maskCount < MAX_MASK_RECTS; ++i)
			if (masks[i].x1 > masks[i].x0 && masks[i].y1 > masks[i].y0)
				geometry.masks[geometry.maskCount++] = masks[i];
	}

	void LookaheadObserver::BeginWalk(size_t futureFrames)
	{
		module.SetLookaheadDepth(futureFrames);
	}

	// The same tests, in the same order, as ObserveFrame's returns before it measures. The
	// builder's reset on a settings change is deferred to the next frame measured, which is
	// the first one to use it.
	bool LookaheadObserver::WantsFrame(uint64_t sourceSequence) const
	{
		if (!module.WantsPredictions()) return false;
		return !(sourceSequence != 0 && sourceSequence <= lastSequence &&
			lastSequence - sourceSequence < 1000);
	}

	void LookaheadObserver::ObserveFrame(uint64_t sourceSequence, size_t framesAhead,
		const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& activePicture)
	{
		(void)framesAhead;
		if (!module.WantsPredictions()) return;
		// Settings changed (or first use): this thread owns the builder.
		const uint64_t generation = module.ConfigGeneration() + 1;
		if (generation != builderGeneration)
		{
			builder = StatsBuilder(module.Config());
			builderGeneration = generation;
			transfer.reset();
		}
		// Hunt 3 (LED2): a new stream (a hold-over drive adopted by the next
		// file, a sequence restart): the last stream's sequence, windows and
		// clock are dropped, so the new one is measured from its first frame.
		const uint64_t epoch = module.LookaheadEpoch();
		if (epoch != streamEpoch)
		{
			streamEpoch = epoch;
			builder = StatsBuilder(module.Config());
			lastSequence = 0;
			haveStreamTime = false;
		}
		// Each frame once, in source order; a jump backwards is a new stream.
		if (sourceSequence != 0 && sourceSequence <= lastSequence &&
			lastSequence - sourceSequence < 1000)
			return;
		const std::shared_ptr<const RawTransfer> current = module.CurrentRawTransfer();
		const double boost = BoostExponent(module.Config(), module.DisplayGamma());
		if (current != transfer || boost != boostedExponent)
		{
			transfer = current;
			boostedExponent = boost;
			boosted.resize(transfer->size());
			for (size_t i = 0; i < transfer->size(); ++i)
				boosted[i] = static_cast<float>(std::pow(std::max(0.0f, (*transfer)[i]), boost));
		}
		RawMeasureOptions options;
		{
			std::lock_guard<std::mutex> lock(geometryMutex);
			options = geometry;
		}
		const LedDriveConfig& config = module.Config();
		options.peakFullCoverage = config.peakFullCoverage;
		if (!config.pictureOnly) options.maskCount = 0;
		RawMeasurement raw;
		if (!MeasureRawFrame(luma, activePicture, boosted, raw, module.SourceRange(), &options)) return;
		// The builder's windows are in real time: one source frame apart.
		// Hunt 3 (LED1): a monotonic clock - the frames since the last measured
		// one at the current period (bounded, so a gap cannot run away); a jump
		// backwards (a new stream the epoch did not announce) restarts the
		// windows and advances one frame.
		const double periodMs = module.SourceFramePeriodMs();
		if (!haveStreamTime)
		{
			streamTimeMs = 0.0;
			haveStreamTime = true;
		}
		else if (sourceSequence > lastSequence && lastSequence != 0)
			streamTimeMs += static_cast<double>(std::min<uint64_t>(sourceSequence - lastSequence, 1000)) * periodMs;
		else
		{
			if (sourceSequence != 0 && sourceSequence < lastSequence)
				builder = StatsBuilder(module.Config());
			streamTimeMs += periodMs;
		}
		lastSequence = sourceSequence;
		const FrameStats stats = builder.Build(raw, streamTimeMs);
		module.SubmitPrediction(sourceSequence, stats);
	}
}
