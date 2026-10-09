#include <pch.h>
#include "LedLookahead.h"

#include "LedDriveModule.h"

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

	bool LumaIsFullRange(const AnalysisLumaSource& luma)
	{
		switch (luma.encoding)
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

	bool MeasureRawFrame(const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& rect,
		const std::vector<float>& transfer, RawMeasurement& out)
	{
		out = RawMeasurement{};
		if (!luma.IsValid() || transfer.size() != RAW_TRANSFER_SIZE) return false;
		const int left = std::max(0, rect.left), top = std::max(0, rect.top);
		const int right = std::min(luma.width, rect.right), bottom = std::min(luma.height, rect.bottom);
		const int width = right - left, height = bottom - top;
		if (width < 3 || height < 3) return false;
		const int blocksX = std::min(96, width / 3), blocksY = std::min(54, height / 3);
		const double stepX = static_cast<double>(width) / blocksX;
		const double stepY = static_cast<double>(height) / blocksY;
		const bool full = LumaIsFullRange(luma);
		for (int by = 0; by < blocksY; ++by)
			for (int bx = 0; bx < blocksX; ++bx)
			{
				const int ox = left + static_cast<int>(bx * stepX);
				const int oy = top + static_cast<int>(by * stepY);
				double acc = 0.0;
				int samples = 0;
				for (int dy = 0; dy < 3; ++dy)
					for (int dx = 0; dx < 3; ++dx)
					{
						AnalysisLumaSample sample;
						if (!luma.Sample(std::min(ox + dx, right - 1), std::min(oy + dy, bottom - 1), sample))
							continue;
						const double signal = full ? sample.luma / 1023.0 : (sample.luma - 64.0) / 876.0;
						const int index = static_cast<int>(std::lround(
							std::max(0.0, std::min(signal, 1.0)) * (RAW_TRANSFER_SIZE - 1)));
						acc += transfer[index];
						++samples;
					}
				if (samples == 0) continue;
				const double code = 16.0 + 219.0 * std::max(0.0, std::min(acc / samples, 1.0));
				const uint32_t scaled = static_cast<uint32_t>(code * SUM_SCALE + 0.5);
				out.histogram[std::min(static_cast<int>(code), HISTOGRAM_BINS - 1)] += 1;
				out.sum += scaled;
				out.blocks += 1;
				const int cell = std::min(by * GRID_H / blocksY, GRID_H - 1) * GRID_W +
					std::min(bx * GRID_W / blocksX, GRID_W - 1);
				out.cellSum[cell] += scaled;
				out.cellCount[cell] += 1;
			}
		return out.blocks > 0;
	}

	LookaheadObserver::LookaheadObserver(LedDriveModule& moduleIn)
		: module(moduleIn), builder(moduleIn.Config())
	{
	}

	void LookaheadObserver::BeginWalk(size_t futureFrames)
	{
		module.SetLookaheadDepth(futureFrames);
	}

	void LookaheadObserver::ObserveFrame(uint64_t sourceSequence, size_t framesAhead,
		const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& activePicture)
	{
		(void)framesAhead;
		if (!module.WantsPredictions()) return;
		// Each frame once, in source order; a jump backwards is a new stream.
		if (sourceSequence != 0 && sourceSequence <= lastSequence &&
			lastSequence - sourceSequence < 1000)
			return;
		const std::shared_ptr<const RawTransfer> current = module.CurrentRawTransfer();
		if (current != transfer)
		{
			transfer = current;
			const double boost = BoostExponent(module.Config());
			boosted.resize(transfer->size());
			for (size_t i = 0; i < transfer->size(); ++i)
				boosted[i] = static_cast<float>(std::pow(std::max(0.0f, (*transfer)[i]), boost));
		}
		RawMeasurement raw;
		if (!MeasureRawFrame(luma, activePicture, boosted, raw)) return;
		lastSequence = sourceSequence;
		const FrameStats stats = builder.Build(raw, sourceSequence * module.FramePeriodMs());
		module.SubmitPrediction(sourceSequence, stats);
	}
}
