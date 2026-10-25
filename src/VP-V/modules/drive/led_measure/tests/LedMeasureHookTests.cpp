// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(LedMeasureHookTests)
	{
	public:


		TEST_METHOD(LedHighlightStatisticIgnoresGrain)
		{
			// Delta 8 R2: with the grain map (g = 1 everywhere), grain on a flat
			// field no longer raises the highlight percentile, a real highlight
			// patch in the same grain still does, and the APL and the block
			// maximum (script mode, SF-4) are exactly as without the map.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int size = 192;
			auto grainOf = [](int x, int y)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
				return static_cast<int>((h * 2654435761u >> 24) % 61u) - 30;
			};
			auto flat = [&](int, int x, int y) { return static_cast<double>(100 + grainOf(x, y)); };
			auto patch = [&](int, int x, int y)
			{
				const bool in = x >= 60 && x < 132 && y >= 60 && y < 132;
				return static_cast<double>(std::min(255, (in ? 225 : 100) + grainOf(x, y)));
			};
			const float texel[4] = { 0.4f, 1.0f, 0.0f, 1.0f };   // r = the floor: g = 1
			pl_tex_params mp{};
			mp.w = mp.h = 1;
			mp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			mp.sampleable = true;
			mp.initial_data = texel;
			pl_tex map = mp.format ? pl_tex_create(gpu, &mp) : nullptr;
			Assert::IsNotNull(map);
			const double weights[3] = { 0.299, 0.587, 0.114 };
			const pl_rect2df crop{ 0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size) };
			auto measure = [&](auto code, bool withGrain)
			{
				LedDrive::MeasurementHook hook;
				hook.SetFrame({}, weights);
				hook.SetGrain(withGrain ? map : nullptr, 0.4f, crop);
				fixture.RenderSequence(std::vector<const pl_hook*>{ hook.Hook() }, size, size, 1, code);
				std::vector<LedDrive::RawMeasurement> measured;
				Assert::AreEqual(1, hook.Collect(measured, 5000), L"No measurement came back");
				hook.Release();
				return measured.front();
			};
			auto p99 = [](const LedDrive::RawMeasurement& r)
			{
				return LedDrive::HistogramPercentile(r.histogram, r.blocks, 99.0);
			};
			const LedDrive::RawMeasurement flatOff = measure(flat, false), flatOn = measure(flat, true);
			const LedDrive::RawMeasurement patchOff = measure(patch, false), patchOn = measure(patch, true);
			pl_tex_destroy(gpu, &map);
			const std::wstring message = L"flat grain p99 " + std::to_wstring(p99(flatOff)) + L" -> " +
				std::to_wstring(p99(flatOn)) + L"; highlight patch p99 " + std::to_wstring(p99(patchOff)) + L" -> " +
				std::to_wstring(p99(patchOn)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(p99(flatOn) < p99(flatOff) - 5.0, message.c_str());
			// The highlight survives at its own level (8-bit 225 = limited code
			// 209.2); the plain statistic's extra was the grain riding on it.
			Assert::IsTrue(std::fabs(p99(patchOn) - (16.0 + 219.0 * 225.0 / 255.0)) <= 3.0 &&
				p99(patchOn) > p99(flatOn) + 50.0, message.c_str());
			for (const auto& pair : { std::make_pair(&flatOff, &flatOn), std::make_pair(&patchOff, &patchOn) })
			{
				Assert::AreEqual(pair.first->sum, pair.second->sum, (L"the APL must not change; " + message).c_str());
				Assert::AreEqual(pair.first->maximum, pair.second->maximum, (L"the block maximum must not change; " + message).c_str());
				Assert::AreEqual(pair.first->blocks, pair.second->blocks);
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		// Bug hunt 2 (owner 2026-10-17), on the WARP device: the full-coverage peak
		// pass against its CPU reference (box = the script's share of the screen),
		// and a subtitle mask leaving its pixels out of every statistic.
		TEST_METHOD(LedMeasurementHookFullCoveragePeakAndPictureOnlyMask)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			const int size = 192;
			// Dark (40), a 2x2 specular (250) and a bright subtitle bar (230).
			auto code = [](int, int x, int y)
			{
				if (x >= 61 && x < 63 && y >= 61 && y < 63) return 250;
				if (x >= 40 && x < 150 && y >= 160 && y < 176) return 230;
				return 40;
			};
			const double weights[3] = { 0.299, 0.587, 0.114 };
			// A screen three times the script's grid: 3x3-pixel boxes.
			const double screenW = 3.0 * LedDrive::SCRIPT_PEAK_GRID_W, screenH = 3.0 * LedDrive::SCRIPT_PEAK_GRID_H;
			std::vector<int> rendered;   // the picture as rendered (red = grey), for the reference
			auto measure = [&](const LedDrive::MeasurementRect* masks, int count)
			{
				LedDrive::MeasurementHook hook;
				hook.SetFrame({}, weights);
				hook.SetPeak(true, screenW, screenH);
				hook.SetMask(masks, count);
				rendered = fixture.RenderSequence(std::vector<const pl_hook*>{ hook.Hook() }, size, size, 1, code).front();
				std::vector<LedDrive::RawMeasurement> measured;
				Assert::AreEqual(1, hook.Collect(measured, 5000), L"No measurement came back");
				hook.Release();
				return measured.front();
			};
			auto reference = [&](const LedDrive::PixelRect* masks, int count)
			{
				auto value = [&](int x, int y, double& v) { v = rendered[static_cast<size_t>(y) * size + x] / 255.0; return true; };
				return LedDrive::AreaAveragedMaximum(value, 0, 0, size, size, screenW, screenH, masks, count);
			};
			const LedDrive::RawMeasurement open = measure(nullptr, 0);
			const std::wstring message = L"peak " + std::to_wstring(open.maximum / 16.0) + L" reference " +
				std::to_wstring(reference(nullptr, 0)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::AreEqual(reference(nullptr, 0), open.maximum / 16.0, 0.75, message.c_str());
			// The subtitle bar (16 rows of 230) is the brightest 3x3 area; masked, the
			// specular's box is: (4 x 250 + 5 x 40) / 9 in the box at 60..62.
			const LedDrive::MeasurementRect subtitle{ 36.0 / size, 156.0 / size, 154.0 / size, 180.0 / size };
			const LedDrive::PixelRect subtitlePx{ 36.0, 156.0, 154.0, 180.0 };
			const LedDrive::RawMeasurement picture = measure(&subtitle, 1);
			Assert::AreEqual(reference(&subtitlePx, 1), picture.maximum / 16.0, 0.75, message.c_str());
			Assert::IsTrue(picture.maximum < open.maximum, L"The masked subtitle is not the peak");
			Assert::IsTrue(picture.blocks < open.blocks && picture.sum / static_cast<double>(picture.blocks) <
				open.sum / static_cast<double>(open.blocks), L"nor in the blocks and the APL");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		TEST_METHOD(LedMeasurementHookMeasuresWithoutTouchingThePicture)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			LedDrive::MeasurementHook hook;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			hook.SetFrame({}, weights);
			std::string libplaceboLog;
			fixture.CaptureLog(&libplaceboLog);
			const std::vector<RgbaPixel> pixels = fixture.RenderCoordinateField(hook.Hook());
			fixture.CaptureLog(nullptr);
			cleanLog += libplaceboLog;
			fixture.CaptureLog(&cleanLog);
			for (size_t i = 0; i < pixels.size(); ++i)
				Assert::IsTrue(pixels[i].r == baseline[i].r && pixels[i].g == baseline[i].g &&
					pixels[i].b == baseline[i].b, L"The measurement must never change the picture");
			std::vector<LedDrive::RawMeasurement> measured;
			const std::wstring failure = L"No measurement came back: " +
				std::wstring(libplaceboLog.begin(), libplaceboLog.end());
			Assert::AreEqual(1, hook.Collect(measured, 5000), failure.c_str());
			const LedDrive::RawMeasurement& raw = measured.front();

			// The same blocks measured on the CPU from the rendered pixels.
			const int blocks = 64 / 3;
			const double step = 64.0 / blocks;
			double expected = 0.0, brightest = 0.0;
			for (int by = 0; by < blocks; ++by)
				for (int bx = 0; bx < blocks; ++bx)
				{
					double acc = 0.0;
					for (int dy = 0; dy < 3; ++dy)
						for (int dx = 0; dx < 3; ++dx)
						{
							const int x = std::min(63, static_cast<int>(bx * step) + dx);
							const int y = std::min(63, static_cast<int>(by * step) + dy);
							const RgbaPixel& p = baseline[y * 64 + x];
							acc += (weights[0] * p.r + weights[1] * p.g + weights[2] * p.b) / 255.0;
						}
					expected += 16.0 + 219.0 * acc / 9.0;
					brightest = std::max(brightest, 16.0 + 219.0 * acc / 9.0);
				}
			expected /= blocks * blocks;
			Assert::AreEqual(static_cast<uint32_t>(blocks * blocks), raw.blocks);
			uint32_t histogramTotal = 0, cellTotal = 0;
			for (uint32_t count : raw.histogram) histogramTotal += count;
			for (uint32_t count : raw.cellCount) cellTotal += count;
			Assert::AreEqual(raw.blocks, histogramTotal);
			Assert::AreEqual(raw.blocks, cellTotal);
			const double apl = static_cast<double>(raw.sum) / LedDrive::SUM_SCALE / raw.blocks;
			Assert::AreEqual(expected, apl, 0.75);
			// Bug hunt 2 (led_peak_full_coverage, the default): the peak over every
			// pixel; at 64x64 (under the script's 854x480) a box is one pixel.
			auto pixelValue = [&](int x, int y, double& v)
			{
				const RgbaPixel& p = baseline[y * 64 + x];
				v = (weights[0] * p.r + weights[1] * p.g + weights[2] * p.b) / 255.0;
				return true;
			};
			const double fullPeak = LedDrive::AreaAveragedMaximum(pixelValue, 0, 0, 64, 64, 64.0, 64.0);
			Assert::IsTrue(fullPeak >= brightest - 0.75);
			Assert::AreEqual(fullPeak, static_cast<double>(raw.maximum) / LedDrive::SUM_SCALE, 0.75,
				L"The full-coverage peak comes back in the same readback buffer");
			// Off: the brightest sample block, as before.
			hook.SetPeak(false);
			fixture.RenderCoordinateField(hook.Hook());
			measured.clear();
			Assert::AreEqual(1, hook.Collect(measured, 5000));
			Assert::AreEqual(brightest, static_cast<double>(measured.front().maximum) / LedDrive::SUM_SCALE, 0.75,
				L"The block maximum comes back in the same readback buffer");
			Assert::AreEqual(raw.sum, measured.front().sum, L"The peak mode never changes the APL");
			hook.SetPeak(true);

			// Measuring only the left half sees less red.
			hook.SetFrame({ 0.0, 0.0, 0.5, 1.0 }, weights);
			fixture.RenderCoordinateField(hook.Hook());
			measured.clear();
			Assert::AreEqual(1, hook.Collect(measured, 5000));
			const double left = static_cast<double>(measured.front().sum) / LedDrive::SUM_SCALE /
				measured.front().blocks;
			Assert::IsTrue(left < apl - 5.0);

			// The statistics describe the gamma-boosted picture.
			hook.SetFrame({}, weights, 2.2 / 2.4);
			fixture.RenderCoordinateField(hook.Hook());
			measured.clear();
			Assert::AreEqual(1, hook.Collect(measured, 5000));
			const double boostedApl = static_cast<double>(measured.front().sum) / LedDrive::SUM_SCALE /
				measured.front().blocks;
			Assert::IsTrue(boostedApl > apl + 2.0, L"The boost lifts the measured APL");
			hook.Release();
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}
	};
}
