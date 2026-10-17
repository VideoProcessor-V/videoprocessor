#include "pch.h"
#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <led/LedDriveModule.h>
#include <led/LedLookahead.h>
#include <led/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	// A uniform picture at `code`, with `brightBlocks` of the blocks at `bright`.
	RawMeasurement Uniform(double code, uint32_t blocks = 1000, uint32_t brightBlocks = 0,
		int bright = 235)
	{
		RawMeasurement raw{};
		const uint32_t plain = blocks - brightBlocks;
		raw.histogram[static_cast<int>(code)] += plain;
		raw.histogram[bright] += brightBlocks;
		raw.blocks = blocks;
		raw.sum = static_cast<uint32_t>(code * SUM_SCALE * plain + bright * SUM_SCALE * brightBlocks);
		raw.maximum = static_cast<uint32_t>((brightBlocks > 0 ? bright : code) * SUM_SCALE);
		for (int i = 0; i < GRID_W * GRID_H; ++i)
		{
			raw.cellCount[i] = 2;
			raw.cellSum[i] = static_cast<uint32_t>(2 * code * SUM_SCALE);
		}
		return raw;
	}

	// Bug hunt 2: a limited-range 10-bit P010 frame (luma codes 64-940) for
	// the raw-frame measurement; code(x, y) gives the 8-bit-equivalent
	// limited code (16-235) of each pixel.
	struct P010Frame
	{
		std::vector<uint16_t> planes;
		AnalysisLumaSource luma;
		template <typename Code>
		P010Frame(int width, int height, Code code)
		{
			planes.assign(static_cast<size_t>(width) * height * 3 / 2, static_cast<uint16_t>(512 << 6));
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					const double y10 = 64.0 + (code(x, y) - 16.0) * 876.0 / 219.0;
					planes[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(std::lround(y10) << 6);
				}
			luma.data = reinterpret_cast<const uint8_t*>(planes.data());
			luma.dataBytes = planes.size() * 2;
			luma.width = width;
			luma.height = height;
			luma.rowBytes = luma.chromaRowBytes = static_cast<size_t>(width) * 2;
			luma.format = AnalysisLumaFormat::P010;
			luma.encoding = VideoFrameEncoding::V210;
			luma.colorspace = ColorSpace::REC_709;
		}
	};

	RawMeasurement MeasureP010(const P010Frame& frame, const RawMeasureOptions& options)
	{
		RawMeasurement raw;
		const QueuedFrameStatistics::PictureRect rect{ 0, 0, frame.luma.width, frame.luma.height };
		Assert::IsTrue(MeasureRawFrame(frame.luma, rect, IdentityRawTransfer(), raw, -1, &options));
		return raw;
	}

	double Peak(const RawMeasurement& raw) { return static_cast<double>(raw.maximum) / SUM_SCALE; }
	double Apl(const RawMeasurement& raw) { return static_cast<double>(raw.sum) / SUM_SCALE / raw.blocks; }

	bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 3000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	LedDriveConfig FastScript()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Script;
		config.tickMs = 50;
		// script_pacing (the default) with near-zero screenshot and echo
		// times: the paced loop runs, quiet loops in 10 ms.
		config.scriptMeasureMs = 10;
		config.scriptEchoMs = 0;
		return config;
	}

	ProjectorConfig FastProjector()
	{
		ProjectorConfig config;
		config.protocol = "barco_ascii";
		config.ackTimeoutMs = 60;
		config.minCommandGapMs = 0;
		config.readBack = true;
		return config;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveMeasurementTests)
	{
	public:
		TEST_METHOD(MeasurementFollowsTheTrustedActivePicture)
		{
			// Scope picture inside a 1920x1080 16:9 raster, all of it displayed.
			const auto scope = ActivePictureRect(true, 0, 138, 1920, 942, 0, 0, 1920, 1080);
			Assert::AreEqual(0.0, scope.x0, 1e-9);
			Assert::AreEqual(1.0, scope.x1, 1e-9);
			Assert::AreEqual(138.0 / 1080.0, scope.y0, 1e-9);
			Assert::AreEqual(942.0 / 1080.0, scope.y1, 1e-9);
			// Relative to a displayed crop.
			const auto cropped = ActivePictureRect(true, 240, 0, 1680, 1080, 240, 0, 1680, 1080);
			Assert::AreEqual(0.0, cropped.x0, 1e-9);
			Assert::AreEqual(1.0, cropped.x1, 1e-9);
			// No trusted picture, or a sliver: measure everything.
			for (const auto rect : { ActivePictureRect(false, 0, 138, 1920, 942, 0, 0, 1920, 1080),
				ActivePictureRect(true, 0, 500, 1920, 520, 0, 0, 1920, 1080),
				ActivePictureRect(true, 0, 0, 0, 0, 0, 0, 1920, 1080) })
			{
				Assert::AreEqual(0.0, rect.x0, 1e-9);
				Assert::AreEqual(1.0, rect.y1, 1e-9);
			}
		}

		TEST_METHOD(StatisticsAreInTheScriptsCodeUnits)
		{
			StatsBuilder builder{ LedDriveConfig() };
			const FrameStats grey = builder.Build(Uniform(60.0));
			Assert::AreEqual(60.0, grey.apl, 1e-9);
			Assert::AreEqual(60.0, grey.highlight, 1e-9);
			Assert::AreEqual(0.0, grey.highlightArea, 1e-9);
			Assert::AreEqual(1000u, grey.blocks);

			// 1% of blocks at white: above the 99.5th percentile, so the highlight
			// statistic sees them; 0.2% would not.
			const FrameStats lamp = builder.Build(Uniform(30.0, 1000, 10));
			Assert::AreEqual(235.0, lamp.highlight, 1e-9);
			Assert::AreEqual(0.01, lamp.highlightArea, 1e-9);
			Assert::AreEqual((30.0 * 990 + 235.0 * 10) / 1000.0, lamp.apl, 1e-6);
			const FrameStats speck = builder.Build(Uniform(30.0, 1000, 2));
			Assert::AreEqual(30.0, speck.highlight, 1e-9);
			Assert::AreEqual(235.0, speck.maximum, 1e-9,
				L"A 0.2% lamp is missed by the percentile but is the block maximum (script mode)");

			uint32_t histogram[256] = {};
			histogram[16] = 500;
			histogram[100] = 495;
			histogram[200] = 5;
			Assert::AreEqual(100.0, HistogramPercentile(histogram, 1000, 99.5), 1e-9);
			Assert::AreEqual(200.0, HistogramPercentile(histogram, 1000, 99.6), 1e-9);
			Assert::AreEqual(16.0, HistogramPercentile(histogram, 0, 99.5), 1e-9);
		}

		TEST_METHOD(CutsAreLargeBlockChangesAndFadesAreSteadyDeclines)
		{
			LedDriveConfig config;
			StatsBuilder builder(config);
			Assert::IsFalse(builder.Build(Uniform(100.0)).cut, L"No previous frame");
			Assert::IsFalse(builder.Build(Uniform(110.0)).cut, L"10 codes is a scene change, not a cut");
			Assert::IsTrue(builder.Build(Uniform(30.0)).cut, L"80 codes is a cut");
			Assert::AreEqual(80.0, builder.LastDifference(), 1e-9);

			// The fade is in time (general): the same at 24p and 60p.
			for (double fps : { 24.0, 60.0 })
			{
				const double step = 1000.0 / fps;
				const int frames = static_cast<int>(std::ceil(config.fadeMs / step - 1e-9));
				StatsBuilder fading(config);
				double apl = 120.0;
				FrameStats stats;
				for (int frame = 0; frame < frames; ++frame)
				{
					stats = fading.Build(Uniform(apl), frame * step);
					Assert::IsFalse(stats.fade, L"Needs fade_ms of declines");
					apl -= 2.0 * 24.0 / fps;   // 48 APL per second
				}
				stats = fading.Build(Uniform(apl), frames * step);
				Assert::IsTrue(stats.fade, L"A fade after fade_ms");
				stats = fading.Build(Uniform(apl), (frames + 1) * step);
				Assert::IsFalse(stats.fade, L"A held frame ends the fade");

				StatsBuilder slow(config);
				apl = 120.0;
				for (int frame = 0; frame <= frames + 2; ++frame)
				{
					stats = slow.Build(Uniform(apl), frame * step);
					apl -= 6.0 / fps;   // 6 APL per second: under fade_min_slope
				}
				Assert::IsFalse(stats.fade, L"Below fade_min_slope is drift, not a fade");
			}
		}

		// Step 7b: script mode is the script - real LPW1 and CNTR, VP's picture
		// untouched (script_smoothing off), CNTR 50 at start and exit.
		TEST_METHOD(ScriptModeSendsTheScriptsLightAndContrast)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			{
				LedDriveModule module(FastProjector(), FastScript(),
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
				Assert::IsTrue(WaitFor([&]
					{
						const auto writes = projector->Writes();
						return projector->light == 255 &&
							std::find(writes.begin(), writes.end(), std::string(":CNTR50\r")) != writes.end();
					}), L"LPW1 255 and CNTR 50 at start (even though the projector was at 50), as the script did");
				for (int frame = 0; frame < 5; ++frame)
				{
					module.SubmitMeasurement(Uniform(10.0));
					module.BeginFrame(static_cast<uint64_t>(frame + 1));
				}
				Assert::IsTrue(WaitFor([&] { return projector->light < 240; }), L"Dark scene dims the LED");
				Assert::IsTrue(WaitFor([&] { return projector->contrast > 51; }, 5000),
					L"Dark scene raises the projector's real CNTR");
				module.BeginFrame(6);
				Assert::IsFalse(module.CurrentGain().active, L"script_smoothing off: VP's picture untouched");
				Assert::AreEqual(1.0, module.CurrentGain().gain, 1e-12);
			}
			Assert::AreEqual(255, projector->light, L"Stopping restores full light");
			Assert::AreEqual(50, projector->contrast, L"and the contrast to 50");
		}

		// Audit stage7-step7ab, Should fix 6: a live switch from script to
		// compensate puts CNTR back to 50 before compensate does anything; the
		// rest of the script's tick is not sent.
		TEST_METHOD(SwitchFromScriptToCompensateResetsContrastFirst)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			const ProjectorConfig port = FastProjector();
			LedDriveModule module(port, FastScript(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			for (int frame = 0; frame < 5; ++frame)
				module.SubmitMeasurement(Uniform(10.0));
			Assert::IsTrue(WaitFor([&] { return projector->contrast >= 53; }, 5000), L"the script raised CNTR");

			LedDriveConfig compensate = FastScript();
			compensate.mode = Mode::Compensate;
			const size_t before = projector->Writes().size();
			Assert::IsTrue(module.Reconfigure(port, compensate));
			auto resetSeen = [&]
			{
				const auto writes = projector->Writes();
				return std::find(writes.begin() + static_cast<std::ptrdiff_t>(std::min(before, writes.size())),
					writes.end(), std::string(":CNTR50\r")) != writes.end();
			};
			bool compensated = false;
			for (uint64_t frame = 1; frame <= 200 && !compensated; ++frame)
			{
				module.SubmitMeasurement(Uniform(10.0), frame);
				const bool reset = resetSeen();
				const CompensationGain gain = module.BeginFrame(frame);
				if (!reset)
					Assert::IsFalse(gain.active, L"compensate touched the picture before CNTR was back at 50");
				compensated = reset && gain.active;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			Assert::IsTrue(compensated, L"compensate starts once CNTR is back at 50");
			const auto writes = projector->Writes();
			const auto reset = std::find(writes.begin() + static_cast<std::ptrdiff_t>(before), writes.end(),
				std::string(":CNTR50\r"));
			Assert::IsTrue(reset != writes.end());
			Assert::IsTrue(reset - (writes.begin() + static_cast<std::ptrdiff_t>(before)) <= 1,
				L"at most the command already in flight goes out before the reset");
			Assert::AreEqual(50, projector->contrast);
			module.Stop();
		}

		TEST_METHOD(ScriptSmoothingBridgesTheRealContrastSteps)
		{
			auto projector = std::make_shared<FakeProjector>();
			LedDriveConfig config = FastScript();
			config.scriptSmoothing = true;
			{
				LedDriveModule module(FastProjector(), config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(Uniform(10.0));
				bool bridged = false;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				for (uint64_t frame = 1; std::chrono::steady_clock::now() < deadline && projector->contrast < 54; ++frame)
				{
					module.SubmitMeasurement(Uniform(10.0));
					const CompensationGain gain = module.BeginFrame(frame);
					if (!gain.active) continue;
					Assert::IsTrue(gain.domain == GainDomain::Encoded);
					Assert::IsTrue(gain.gain <= 1.0 + 1e-12, L"Never brighter than the projector's own CNTR");
					bridged = bridged || gain.gain < 1.0 - 1e-6;
					std::this_thread::sleep_for(std::chrono::milliseconds(4));
				}
				Assert::IsTrue(projector->contrast >= 54, L"The script raised the real CNTR");
				Assert::IsTrue(bridged, L"VP's gain bridged the steps");
			}
			Assert::AreEqual(50, projector->contrast);
		}

		TEST_METHOD(LogOnlyNeverOpensAPortOrTouchesThePicture)
		{
			int ports = 0;
			LedDriveConfig config = FastScript();
			config.mode = Mode::LogOnly;
			LedDriveModule module(FastProjector(), config,
				[&ports] { ++ports; return std::unique_ptr<ISerialPort>(); });
			module.Start();
			for (int frame = 0; frame < 5; ++frame)
				module.SubmitMeasurement(Uniform(10.0));
			Assert::IsTrue(WaitFor([&] { return module.Ticks() >= 5; }));
			Assert::IsFalse(module.DrivingProjector());
			Assert::IsFalse(module.CurrentGain().active);
			Assert::AreEqual(1.0, module.CurrentGain().gain, 1e-12);
			Assert::IsTrue(module.ProjectorStatus().lastLight < 255, L"The dry run still logs the levels");
			module.Stop();
			Assert::AreEqual(0, ports);
		}

		// ---- Bug hunt 2 (owner 2026-10-17): the peak from every pixel, the picture only ----

		// The script's box: screen / 854 x screen / 480 at any resolution, tiling
		// the measured picture exactly, never under a pixel.
		TEST_METHOD(PeakBoxesAreTheScriptsShareOfTheScreen)
		{
			const PeakGrid uhd = AreaPeakGrid(3840, 2160, 3840, 2160);
			Assert::AreEqual(854, uhd.boxesX);
			Assert::AreEqual(480, uhd.boxesY);
			Assert::AreEqual(3840.0 / 854.0, uhd.boxW, 1e-9);
			Assert::AreEqual(4.5, uhd.boxH, 1e-9);
			const PeakGrid hd = AreaPeakGrid(1920, 1080, 1920, 1080);
			Assert::AreEqual(854, hd.boxesX);
			Assert::AreEqual(480, hd.boxesY);
			// A scope picture on a 16:9 screen: the same box, fewer rows.
			const PeakGrid scope = AreaPeakGrid(3840, 1608, 3840, 2160);
			Assert::AreEqual(854, scope.boxesX);
			Assert::AreEqual(357, scope.boxesY);
			Assert::AreEqual(1608.0 / 357.0, scope.boxH, 1e-9);
			// A picture half the screen wide has half the boxes.
			const PeakGrid half = AreaPeakGrid(1920, 1080, 3840, 2160);
			Assert::AreEqual(427, half.boxesX);
			Assert::AreEqual(240, half.boxesY);
			// Below the script's resolution a box is one pixel.
			const PeakGrid tiny = AreaPeakGrid(64, 64, 64, 64);
			Assert::AreEqual(64, tiny.boxesX);
			Assert::AreEqual(1.0, tiny.boxW, 1e-9);
			Assert::AreEqual(3, PeakSampleStride(3840, 2160, LOOKAHEAD_PEAK_SAMPLE_BUDGET));
			Assert::AreEqual(1, PeakSampleStride(960, 540, LOOKAHEAD_PEAK_SAMPLE_BUDGET));
		}

		// The area mean is cv2 INTER_AREA's: fractional pixels at a box's edge
		// count by their share.
		TEST_METHOD(PeakIsTheMaximumOfAnAreaAveragedDownscale)
		{
			// Five pixels, boxes of 2.5: box 0 = (0 + 1 + 0.5 x 0) / 2.5 = 0.4.
			const double row[5] = { 0.0, 1.0, 0.0, 0.0, 0.0 };
			auto value = [&](int x, int, double& v) { v = row[x]; return true; };
			const double peak = AreaAveragedMaximum(value, 0, 0, 5, 1, 2.5 * SCRIPT_PEAK_GRID_W, SCRIPT_PEAK_GRID_H);
			Assert::AreEqual(16.0 + 219.0 * 0.4, peak, 1e-9);
			// The middle pixel is shared: (0.5 x 1 + 2 x 0.25) / 2.5 for a 0.25 neighbour.
			const double shared[5] = { 0.0, 0.0, 1.0, 0.25, 0.25 };
			auto sharedValue = [&](int x, int, double& v) { v = shared[x]; return true; };
			Assert::AreEqual(16.0 + 219.0 * (0.5 + 0.5) / 2.5,
				AreaAveragedMaximum(sharedValue, 0, 0, 5, 1, 2.5 * SCRIPT_PEAK_GRID_W, SCRIPT_PEAK_GRID_H), 1e-9);
			// Nothing to measure.
			Assert::AreEqual(-1.0, AreaAveragedMaximum(value, 0, 0, 0, 1, 1.0, 1.0));
		}

		// A small specular between the old sample points: the sparse grid
		// (one 3x3 block every 20 pixels at 1920 wide) misses it; with every
		// pixel counting it is the peak, at its area mean.
		TEST_METHOD(FullCoveragePeakFindsASpecularBetweenTheSamplePoints)
		{
			// Dark picture (code 40), a 6x6 white specular at 8..13 - the first
			// block reads 0..2, the next starts at 20.
			const P010Frame frame(1920, 1080, [](int x, int y)
				{ return x >= 8 && x < 14 && y >= 8 && y < 14 ? 235.0 : 40.0; });
			RawMeasureOptions sparse;
			sparse.peakFullCoverage = false;
			const RawMeasurement before = MeasureP010(frame, sparse);
			Assert::AreEqual(40.0, Peak(before), 0.1, L"The sparse grid misses the specular");
			RawMeasureOptions full;   // the default: on
			full.peakSampleBudget = 1 << 30;   // every pixel
			const RawMeasurement after = MeasureP010(frame, full);
			Assert::IsTrue(Peak(after) >= 215.0, (L"The specular is the script's highlight: " +
				std::to_wstring(Peak(after))).c_str());
			// The script's own number: the 2.25-pixel boxes inside the specular are white.
			Assert::AreEqual(235.0, Peak(after), 0.5);
			// Only the peak changes: APL and the blocks are the sparse grid's.
			Assert::AreEqual(before.sum, after.sum);
			Assert::AreEqual(before.blocks, after.blocks);
			// Through the statistics, the script's highlight test sees it.
			LedDriveConfig config;
			config.mode = Mode::Script;
			StatsBuilder builder(config);
			Assert::IsTrue(builder.Build(after).maximum >= config.scriptHighlightCode);
			// The look-ahead's bounded read (every third pixel at 4K) still finds a
			// specular as large as its stride.
			const P010Frame uhd(3840, 2160, [](int x, int y)
				{ return x >= 30 && x < 36 && y >= 30 && y < 36 ? 235.0 : 40.0; });
			Assert::IsTrue(Peak(MeasureP010(uhd, RawMeasureOptions())) >= 215.0,
				L"The look-ahead's subsampled peak finds a 6-pixel specular at 4K");
		}

		// The scale is unchanged: the peak is in the same limited-range codes as
		// the APL (madVR's limited levels in Windows' full range: the script's
		// screenshot read the same numbers).
		TEST_METHOD(FullCoveragePeakKeepsTheBrightnessScale)
		{
			const P010Frame grey(960, 540, [](int, int) { return 125.5; });
			RawMeasureOptions full;
			RawMeasureOptions sparse;
			sparse.peakFullCoverage = false;
			const RawMeasurement a = MeasureP010(grey, full), b = MeasureP010(grey, sparse);
			Assert::AreEqual(125.5, Peak(a), 0.1);
			Assert::AreEqual(Peak(b), Peak(a), 0.1);
			Assert::AreEqual(Apl(a), Peak(a), 0.1);
			StatsBuilder builder{ LedDriveConfig() };
			Assert::AreEqual(125.5, builder.Build(a).maximum, 0.1);
		}

		// A detected subtitle is left out of every statistic; with no
		// detection nothing is masked.
		TEST_METHOD(SubtitleBoxIsMaskedAndNoDetectionMeasuresEverything)
		{
			// Dark picture, white subtitle text rows 900..960, columns 600..1320.
			const P010Frame frame(1920, 1080, [](int x, int y)
				{ return x >= 600 && x < 1320 && y >= 900 && y < 960 ? 235.0 : 40.0; });
			RawMeasureOptions none;
			none.peakSampleBudget = 1 << 30;
			const RawMeasurement open = MeasureP010(frame, none);
			Assert::AreEqual(235.0, Peak(open), 0.5, L"Unmasked (no detection), the subtitle is measured");
			Assert::IsTrue(Apl(open) > 41.0);
			RawMeasureOptions masked = none;
			masked.masks[0] = { 590.0, 890.0, 1330.0, 970.0 };
			masked.maskCount = 1;
			const RawMeasurement picture = MeasureP010(frame, masked);
			Assert::AreEqual(40.0, Peak(picture), 0.1, L"The masked subtitle is not the peak");
			Assert::AreEqual(40.0, Apl(picture), 0.1, L"nor in the APL");
			Assert::IsTrue(picture.blocks < open.blocks, L"Its blocks are left out");
			uint32_t bright = 0, cells = 0;
			for (int code = 200; code < HISTOGRAM_BINS; ++code) bright += picture.histogram[code];
			for (uint32_t count : picture.cellCount) cells += count;
			Assert::AreEqual(0u, bright, L"nor in the histogram (compensate's highlight)");
			Assert::AreEqual(picture.blocks, cells, L"The cut cells count the same blocks");
			// The sparse peak (led_peak_full_coverage off) is masked as well.
			RawMeasureOptions sparseMasked = masked;
			sparseMasked.peakFullCoverage = false;
			Assert::AreEqual(40.0, Peak(MeasureP010(frame, sparseMasked)), 0.1);
			// A box at least half masked is skipped; a mask in a source crop maps
			// by relative position, and one outside the crop is empty.
			const MeasurementRect inCrop = SourceRectInCrop(600, 900, 1320, 960, 0, 0, 1920, 1080);
			Assert::AreEqual(600.0 / 1920.0, inCrop.x0, 1e-9);
			Assert::AreEqual(960.0 / 1080.0, inCrop.y1, 1e-9);
			Assert::IsTrue(Empty(SourceRectInCrop(0, 0, 100, 100, 200, 200, 1920, 1080)));
			Assert::IsTrue(Empty(SourceRectInCrop(10, 10, 10, 20, 0, 0, 1920, 1080)));
		}

		TEST_METHOD(UnavailablePortDegradesToADryRun)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;
			LedDriveModule module(FastProjector(), FastScript(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			for (int frame = 0; frame < 5; ++frame)
				module.SubmitMeasurement(Uniform(10.0));
			Assert::IsTrue(WaitFor([&] { return module.Ticks() >= 5; }));
			Assert::IsFalse(module.DrivingProjector());
			Assert::IsFalse(module.CurrentGain().active, L"Without the LED dimming, no compensating gain");
			module.Stop();
			Assert::IsTrue(projector->Writes().empty());
		}
	};
}
