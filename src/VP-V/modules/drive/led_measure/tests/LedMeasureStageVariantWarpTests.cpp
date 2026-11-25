// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// O#4 (L2b): Red Team adversarial tests for GPU MeasurementHook PRE_OUTPUT stage variant.
// Verifies:
//   1. MeasurementHook(PreOutput) runs on WARP at PL_HOOK_PRE_OUTPUT, exposes canonical
//      SourceStatsDisplay::VariantKeyFor(), and CollectDisplay agrees with CPU
//      SourceStatsDisplay::MeasureDisplayFrame on the exact rendered pixels (with
//      same-input negative controls for both subtitle masking and gamma boost).
//   2. Invoking the hook at any wrong pipeline stage (PL_HOOK_OUTPUT / PL_HOOK_MAIN) is
//      visibly rejected via StageMismatches() with zero silent measurements.

#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/drive/led_measure/SourceStatsDisplay.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(LedMeasureStageVariantWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(PreOutputStageVariantMatchesCpuDisplayLawOnWarp)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PreOutputStageVariantMatchesCpuDisplayLawOnWarp)
		{
			std::string log;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			fixture.CaptureLog(&log);
			struct CaptureLogGuard
			{
				TargetLutGpuFixture& f;
				bool active = true;
				void Release()
				{
					if (active)
					{
						f.CaptureLog(nullptr);
						active = false;
					}
				}
				~CaptureLogGuard() { Release(); }
			} captureLogGuard{ fixture };

			constexpr int kSize = 192;
			constexpr double kBoost = 2.2 / 2.4;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			const double screenW = 3.0 * LedDrive::SCRIPT_PEAK_GRID_W;
			const double screenH = 3.0 * LedDrive::SCRIPT_PEAK_GRID_H;

			// Mid-grey background (90), 2x2 specular (250) inside 3x3 box [60,63)^2, bright subtitle bar (230).
			auto sceneCode = [](int, int x, int y)
			{
				if (x >= 61 && x < 63 && y >= 61 && y < 63) return 250;
				if (x >= 40 && x < 150 && y >= 160 && y < 176) return 230;
				return 90;
			};

			const LedDrive::MeasurementRect subtitleNorm{ 36.0 / kSize, 156.0 / kSize, 154.0 / kSize, 180.0 / kSize };
			const LedDrive::PixelRect subtitlePx{ 36.0, 156.0, 154.0, 180.0 };

			LedDrive::MeasurementHook hook(LedDrive::MeasurementStageVariant::PreOutput);
			struct HookGuard
			{
				LedDrive::MeasurementHook& h;
				~HookGuard() { h.Release(); }
			} hookGuard{ hook };
			Assert::IsTrue(hook.StageVariant() == LedDrive::MeasurementStageVariant::PreOutput);
			Assert::AreEqual(static_cast<uint64_t>(PL_HOOK_PRE_OUTPUT), static_cast<uint64_t>(hook.Hook()->stages));
			Assert::AreEqual(static_cast<uint64_t>(PL_HOOK_PRE_OUTPUT), hook.HookStagesMask());
			Assert::IsTrue(hook.MatchesStage(PL_HOOK_PRE_OUTPUT));
			Assert::IsTrue(hook.VariantKey() == LedDrive::SourceStatsDisplay::VariantKeyFor(),
				L"MeasurementHook VariantKey must match SourceStatsDisplay::VariantKeyFor()");
			Assert::AreEqual(0, hook.StageMismatches());

			hook.SetFrame({}, weights, kBoost, 42ull);
			hook.SetPeak(true, screenW, screenH);
			hook.SetMask(&subtitleNorm, 1);

			const std::vector<int> rendered =
				fixture.RenderSequence(std::vector<const pl_hook*>{ hook.Hook() }, kSize, kSize, 1, sceneCode).front();

			std::vector<LedDrive::SourceStatsDisplay::DisplayRecord> gpuRecords;
			std::vector<uint64_t> seqs;
			Assert::AreEqual(1, hook.CollectDisplay(gpuRecords, 5000, &seqs), L"CollectDisplay must return 1 record");
			Assert::AreEqual<size_t>(1, gpuRecords.size());
			Assert::AreEqual<size_t>(1, seqs.size());
			Assert::AreEqual<uint64_t>(42ull, seqs.front());
			Assert::AreEqual(0, hook.StageMismatches());
			Assert::AreEqual(1, hook.Measured());
			hook.Release();

			const LedDrive::SourceStatsDisplay::DisplayRecord& gpuRec = gpuRecords.front();

			// CPU twin on the EXACT rendered pixels.
			auto sampleRendered = [&](int x, int y, double& r, double& g, double& b) -> bool
			{
				const double v = static_cast<double>(rendered[static_cast<size_t>(y) * kSize + x]) / 255.0;
				r = g = b = v;
				return true;
			};

			LedDrive::SourceStatsDisplay::MeasureParams matchedParams;
			matchedParams.weights[0] = weights[0];
			matchedParams.weights[1] = weights[1];
			matchedParams.weights[2] = weights[2];
			matchedParams.boost = kBoost;
			matchedParams.peakFullCoverage = true;
			matchedParams.screenWidth = screenW;
			matchedParams.screenHeight = screenH;
			matchedParams.masks = &subtitlePx;
			matchedParams.maskCount = 1;

			const LedDrive::SourceStatsDisplay::DisplayRecord cpuMatched =
				LedDrive::SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleRendered, matchedParams);

			const std::wstring msg = L"GPU apl=" + std::to_wstring(gpuRec.apl) + L" max=" + std::to_wstring(gpuRec.maximum) +
				L" blocks=" + std::to_wstring(gpuRec.blocks) +
				L" vs CPU apl=" + std::to_wstring(cpuMatched.apl) + L" max=" + std::to_wstring(cpuMatched.maximum) +
				L" blocks=" + std::to_wstring(cpuMatched.blocks);

			Assert::AreEqual(cpuMatched.blocks, gpuRec.blocks, msg.c_str());
			Assert::AreEqual(cpuMatched.apl, gpuRec.apl, 0.75, msg.c_str());
			Assert::AreEqual(cpuMatched.maximum, gpuRec.maximum, 0.75, msg.c_str());
			Assert::AreEqual(cpuMatched.highlight, gpuRec.highlight, 1.0, msg.c_str());
			Assert::AreEqual(cpuMatched.p50, gpuRec.p50, 1.0, msg.c_str());

			// Negative control 1 (unmasked hazard on the EXACT same rendered pixels):
			// naive unmasked CPU measurement must violate agreement with masked GPU CollectDisplay.
			LedDrive::SourceStatsDisplay::MeasureParams unmaskedParams = matchedParams;
			unmaskedParams.masks = nullptr;
			unmaskedParams.maskCount = 0;
			const LedDrive::SourceStatsDisplay::DisplayRecord cpuUnmasked =
				LedDrive::SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleRendered, unmaskedParams);
			Assert::IsTrue(cpuUnmasked.blocks > gpuRec.blocks,
				L"Negative control: unmasked CPU baseline must include subtitle blocks excluded by GPU mask");
			Assert::IsTrue(cpuUnmasked.maximum > gpuRec.maximum + 40.0,
				L"Negative control: unmasked subtitle peak must sharply diverge from masked GPU CollectDisplay");
			Assert::IsTrue(cpuUnmasked.apl > gpuRec.apl + 3.5,
				L"Negative control: unmasked subtitle bar must inflate APL above masked GPU CollectDisplay");

			// Negative control 2 (unboosted hazard on the EXACT same rendered pixels):
			// naive boost=1.0 CPU measurement must violate agreement with boosted GPU CollectDisplay.
			LedDrive::SourceStatsDisplay::MeasureParams unboostedParams = matchedParams;
			unboostedParams.boost = 1.0;
			const LedDrive::SourceStatsDisplay::DisplayRecord cpuUnboosted =
				LedDrive::SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleRendered, unboostedParams);
			Assert::IsTrue(std::fabs(cpuUnboosted.apl - gpuRec.apl) > 5.0,
				L"Negative control: unboosted CPU baseline must diverge from boosted GPU CollectDisplay by > 5 codes");

			captureLogGuard.Release();
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(WrongStageInvocationIsRejectedVisibly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(WrongStageInvocationIsRejectedVisibly)
		{
			LedDrive::MeasurementHook hook(LedDrive::MeasurementStageVariant::PreOutput);
			Assert::IsTrue(hook.MatchesStage(PL_HOOK_PRE_OUTPUT));
			Assert::IsFalse(hook.MatchesStage(PL_HOOK_MAIN));
			Assert::IsFalse(hook.MatchesStage(PL_HOOK_OUTPUT));
			Assert::IsFalse(hook.MatchesStage(0));
			Assert::AreEqual(0, hook.StageMismatches());
			Assert::AreEqual(0, hook.Measured());

			pl_hook_params wrongOutput{};
			wrongOutput.stage = PL_HOOK_OUTPUT;
			const pl_hook_res resOutput = hook.Hook()->hook(hook.Hook()->priv, &wrongOutput);
			Assert::AreEqual(static_cast<int>(PL_HOOK_SIG_NONE), static_cast<int>(resOutput.output));
			Assert::IsFalse(resOutput.failed);
			Assert::AreEqual(1, hook.StageMismatches(), L"PL_HOOK_OUTPUT invocation must increment StageMismatches");
			Assert::AreEqual(0, hook.Measured(), L"Wrong-stage invocation must never silently measure");

			pl_hook_params wrongMain{};
			wrongMain.stage = PL_HOOK_MAIN;
			const pl_hook_res resMain = hook.Hook()->hook(hook.Hook()->priv, &wrongMain);
			Assert::AreEqual(static_cast<int>(PL_HOOK_SIG_NONE), static_cast<int>(resMain.output));
			Assert::IsFalse(resMain.failed);
			Assert::AreEqual(2, hook.StageMismatches(), L"PL_HOOK_MAIN invocation must increment StageMismatches");
			Assert::AreEqual(0, hook.Measured());
		}
	};
}
