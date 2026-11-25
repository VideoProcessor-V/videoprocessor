// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// O#3 (L2a): Display-domain P4 variant CPU law for LED measurement.
// Computes limited-range 8-bit code-unit (ITU-R BT.709: black 16, span 219) display statistics
// at the PRE_OUTPUT pipeline stage immediately preceding drive.led_measure, sharing the U6
// percentile bin-search law (VpUnit::SourceStats::FirstBinReaching) and matching the GPU
// MeasurementHook 3x3 block pass + AreaAveragedMaximum peak pass.
//
// Header-only, pure C++14, no Windows or GPU headers.

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/VariantKey.h>
#include <VP-V/modules/drive/led_measure/LedMeasurement.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LedDrive
{
	namespace SourceStatsDisplay
	{
		constexpr uint32_t kUnitVersion = 1;
		// ITU-R BT.709 limited-range 8-bit display code units: code = 16 + 219 * v.
		constexpr uint8_t kDisplayCodeUnit = 8;
		constexpr double kLimitedBlack = 16.0;
		constexpr double kLimitedSpan = 219.0;
		// 3x3 block mean matching GPU MeasurementHook.
		constexpr int kBlockSize = 3;
		constexpr int kDefaultMaxBlocksX = 320;
		constexpr int kDefaultMaxBlocksY = 180;

		// The PRE_OUTPUT stage immediately preceding drive.led_measure (after colour.appearance).
		inline uint8_t PreOutputStage()
		{
			return VpSkeleton::VariantKey::StageAfter(static_cast<uint8_t>(PipelineSlots::Id::ColourAppearance));
		}

		// Canonical P4 variant key for the display-domain measurement at `stage` and `precision`.
		inline VpSkeleton::VariantKey VariantKeyFor(uint8_t stage = PreOutputStage(),
			uint8_t precision = VpSkeleton::kPrecisionFloat32)
		{
			VpSkeleton::VariantKey key;
			key.codeUnit = kDisplayCodeUnit;
			key.stage = stage;
			key.precision = precision;
			return key.Canonical();
		}

		// Code at or above which `percentile` percent of blocks lie below, reusing U6 SourceStats::FirstBinReaching.
		inline double DisplayPercentile(const uint32_t* histogram, uint32_t total, double percentile)
		{
			if (histogram == nullptr || total == 0)
				return kLimitedBlack;
			const double totalD = static_cast<double>(total);
			const double above = totalD * (100.0 - percentile) / 100.0;
			if (above >= totalD)
				return 0.0;
			if (above < 0.0)
				return static_cast<double>(HISTOGRAM_BINS - 1);
			const double target = totalD - above;
			const int bin = VpUnit::SourceStats::FirstBinReaching<double>(histogram, HISTOGRAM_BINS, target);
			return bin >= 0 ? static_cast<double>(bin) : 0.0;
		}

		struct DisplayRecord
		{
			uint32_t version = kUnitVersion;
			double apl = kLimitedBlack;
			double highlight = kLimitedBlack;
			double highlightArea = 0.0;
			double maximum = kLimitedBlack;
			double p01 = kLimitedBlack;
			double p50 = kLimitedBlack;
			double p99 = kLimitedBlack;
			uint32_t blocks = 0;
			RawMeasurement raw{};
		};

		// Single CPU law converting RawMeasurement counts into DisplayRecord summary statistics.
		inline DisplayRecord SummaryFromRaw(const RawMeasurement& raw, double highlightPercentile = 99.5,
			int scriptHighlightCode = 215)
		{
			DisplayRecord rec;
			rec.raw = raw;
			if (raw.blocks == 0)
				return rec;
			rec.blocks = raw.blocks;
			rec.apl = static_cast<double>(raw.sum) / static_cast<double>(SUM_SCALE) / static_cast<double>(raw.blocks);
			rec.highlight = DisplayPercentile(raw.histogram, raw.blocks, highlightPercentile);
			uint32_t bright = 0;
			const int minCode = (std::max)(0, (std::min)(scriptHighlightCode, HISTOGRAM_BINS - 1));
			for (int code = minCode; code < HISTOGRAM_BINS; ++code)
				bright += raw.histogram[code];
			rec.highlightArea = static_cast<double>(bright) / static_cast<double>(raw.blocks);
			rec.maximum = raw.maximum > 0
				? static_cast<double>(raw.maximum) / static_cast<double>(SUM_SCALE)
				: rec.highlight;
			rec.p01 = DisplayPercentile(raw.histogram, raw.blocks, 1.0);
			rec.p50 = DisplayPercentile(raw.histogram, raw.blocks, 50.0);
			rec.p99 = DisplayPercentile(raw.histogram, raw.blocks, 99.0);
			return rec;
		}

		struct MeasureParams
		{
			MeasurementRect rect{};
			double weights[3] = { 0.299, 0.587, 0.114 };
			double boost = 1.0;
			bool peakFullCoverage = true;
			double screenWidth = 0.0;
			double screenHeight = 0.0;
			const PixelRect* masks = nullptr;
			int maskCount = 0;
			int maxBlocksX = kDefaultMaxBlocksX;
			int maxBlocksY = kDefaultMaxBlocksY;
			double highlightPercentile = 99.5;
			int scriptHighlightCode = 215;
		};

		// Exact CPU twin of the PRE_OUTPUT 3x3 block pass (+ AreaAveragedMaximum when peakFullCoverage).
		// `sampleRgb(int x, int y, double& r, double& g, double& b)` returns true when pixel (x, y) is readable.
		template <typename SampleRgbFn>
		inline DisplayRecord MeasureDisplayFrame(int width, int height, SampleRgbFn&& sampleRgb,
			const MeasureParams& params = MeasureParams())
		{
			if (width <= 0 || height <= 0)
				return DisplayRecord{};
			const int left = (std::max)(0, (std::min)(width, static_cast<int>(std::floor(params.rect.x0 * width))));
			const int top = (std::max)(0, (std::min)(height, static_cast<int>(std::floor(params.rect.y0 * height))));
			const int right = (std::max)(left, (std::min)(width, static_cast<int>(std::ceil(params.rect.x1 * width))));
			const int bottom = (std::max)(top, (std::min)(height, static_cast<int>(std::ceil(params.rect.y1 * height))));
			const double rectW = static_cast<double>(right - left);
			const double rectH = static_cast<double>(bottom - top);
			if (rectW < static_cast<double>(kBlockSize) || rectH < static_cast<double>(kBlockSize))
				return DisplayRecord{};

			const int maxBX = params.maxBlocksX > 0 ? params.maxBlocksX : kDefaultMaxBlocksX;
			const int maxBY = params.maxBlocksY > 0 ? params.maxBlocksY : kDefaultMaxBlocksY;
			const int blocksX = (std::min)(maxBX, static_cast<int>(std::floor(rectW / static_cast<double>(kBlockSize))));
			const int blocksY = (std::min)(maxBY, static_cast<int>(std::floor(rectH / static_cast<double>(kBlockSize))));
			if (blocksX <= 0 || blocksY <= 0)
				return DisplayRecord{};

			const double stepX = rectW / static_cast<double>(blocksX);
			const double stepY = rectH / static_cast<double>(blocksY);
			const int loX = left;
			const int loY = top;
			const int hiX = right - 1;
			const int hiY = bottom - 1;

			RawMeasurement raw{};
			for (int by = 0; by < blocksY; ++by)
			{
				for (int bx = 0; bx < blocksX; ++bx)
				{
					const int originX = static_cast<int>(static_cast<double>(left) + static_cast<double>(bx) * stepX);
					const int originY = static_cast<int>(static_cast<double>(top) + static_cast<double>(by) * stepY);
					const double cx = static_cast<double>(originX) + 1.5;
					const double cy = static_cast<double>(originY) + 1.5;
					bool masked = false;
					for (int m = 0; m < params.maskCount && params.masks != nullptr && !masked; ++m)
						masked = params.masks[m].Contains(cx, cy);
					if (masked)
						continue;

					double acc = 0.0;
					int sampled = 0;
					for (int dy = 0; dy < kBlockSize; ++dy)
					{
						for (int dx = 0; dx < kBlockSize; ++dx)
						{
							const int px = (std::max)(loX, (std::min)(originX + dx, hiX));
							const int py = (std::max)(loY, (std::min)(originY + dy, hiY));
							double r = 0.0, g = 0.0, b = 0.0;
							if (!sampleRgb(px, py, r, g, b))
								continue;
							r = (std::max)(0.0, (std::min)(r, 1.0));
							g = (std::max)(0.0, (std::min)(g, 1.0));
							b = (std::max)(0.0, (std::min)(b, 1.0));
							if (params.boost != 1.0)
							{
								r = std::pow(r, params.boost);
								g = std::pow(g, params.boost);
								b = std::pow(b, params.boost);
							}
							acc += r * params.weights[0] + g * params.weights[1] + b * params.weights[2];
							++sampled;
						}
					}
					if (sampled == 0)
						continue;

					const double mean = (std::max)(0.0, (std::min)(acc / static_cast<double>(sampled), 1.0));
					const double code = kLimitedBlack + kLimitedSpan * mean;
					const uint32_t scaled = static_cast<uint32_t>(code * static_cast<double>(SUM_SCALE) + 0.5);
					const int bin = (std::max)(0, (std::min)(static_cast<int>(code), HISTOGRAM_BINS - 1));
					++raw.histogram[bin];
					raw.sum += scaled;
					++raw.blocks;
					if (!params.peakFullCoverage && scaled > raw.maximum)
						raw.maximum = scaled;
				}
			}

			if (params.peakFullCoverage && raw.blocks > 0)
			{
				const double screenW = params.screenWidth > 0.0 ? params.screenWidth : static_cast<double>(width);
				const double screenH = params.screenHeight > 0.0 ? params.screenHeight : static_cast<double>(height);
				const double peakCode = AreaAveragedMaximum(
					[&](int x, int y, double& v) -> bool {
						double r = 0.0, g = 0.0, b = 0.0;
						if (!sampleRgb(x, y, r, g, b))
							return false;
						r = (std::max)(0.0, (std::min)(r, 1.0));
						g = (std::max)(0.0, (std::min)(g, 1.0));
						b = (std::max)(0.0, (std::min)(b, 1.0));
						if (params.boost != 1.0)
						{
							r = std::pow(r, params.boost);
							g = std::pow(g, params.boost);
							b = std::pow(b, params.boost);
						}
						v = r * params.weights[0] + g * params.weights[1] + b * params.weights[2];
						return true;
					},
					left, top, right, bottom, screenW, screenH, params.masks,
					(params.masks != nullptr && params.maskCount > 0) ? params.maskCount : 0);
				if (peakCode >= 0.0)
					raw.maximum = static_cast<uint32_t>(peakCode * static_cast<double>(SUM_SCALE) + 0.5);
			}

			return SummaryFromRaw(raw, params.highlightPercentile, params.scriptHighlightCode);
		}
	}
}
