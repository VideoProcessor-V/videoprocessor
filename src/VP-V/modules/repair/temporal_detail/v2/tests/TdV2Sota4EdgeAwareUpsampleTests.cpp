// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 4 (TD-SOTA-4) Adversarial Red-Team Tests:
// Clean-Luma-Guided Edge-Aware Weight Upsampling in Pass 3A (SC-04):
//   1. SpatialGuidedBilinearWeightSnapsToCleanLumaSilhouetteAndMatchesBilinearOnFlatLuma (CPU):
//      - Uniform luminance produces exact SpatialBilinearHuberWeight / SpatialBilinearWeight parity
//      - Sharp high-contrast luminance step snaps 16x16 cell interpolation weights cleanly to the silhouette
//   2. CpuAndGpuEdgeAwareWeightUpsamplePreventsBelowHuberKneeSilhouetteHaloBleed (GpuWarp):
//      - Sharp high-contrast silhouette bisecting a 16x16 cell where one side has valid reference and
//        the other side is occluded/moved with a residual below the 3-sigma Huber knee
//      - CPU TdV2ReferenceEngine + real D3D11 WARP TdV2GpuEngine (Preset::Reference & Preset::RealTime):
//        enableEdgeAwareWeightUpsample=true snaps weights cleanly to the silhouette, suppressing edge
//        halo bleed AND strengthening valid-side near-edge recovery vs enableEdgeAwareWeightUpsample=false
//        spatial bilinear negative control!
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2Sota4EdgeAwareUpsampleNs
{
	using namespace TdV2;
	using namespace TdV2::Gpu;

	constexpr double kPi = 3.14159265358979323846;
	constexpr double kGpuCpuTol = 1e-3;

	struct WarpContext
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;

		WarpContext()
		{
			pl_log_params lp = pl_log_default_params;
			lp.log_level = PL_LOG_ERR;
			log = pl_log_create(PL_API_VER, &lp);
			Assert::IsNotNull(log);

			pl_d3d11_params dp = pl_d3d11_default_params;
			dp.force_software = true;
			dp.allow_software = true;
			d3d11 = pl_d3d11_create(log, &dp);
			Assert::IsNotNull(d3d11);

			gpu = d3d11->gpu;
			Assert::IsNotNull(gpu);

			dispatch = pl_dispatch_create(log, gpu);
			Assert::IsNotNull(dispatch);
		}

		~WarpContext()
		{
			if (dispatch != nullptr)
				pl_dispatch_destroy(&dispatch);
			if (d3d11 != nullptr)
				pl_d3d11_destroy(&d3d11);
			if (log != nullptr)
				pl_log_destroy(&log);
		}

		WarpContext(const WarpContext&) = delete;
		WarpContext& operator=(const WarpContext&) = delete;
	};

	struct EngineGuard
	{
		pl_gpu gpu = nullptr;
		TdV2GpuEngine engine;

		explicit EngineGuard(pl_gpu g, const Config& cfg)
			: gpu(g), engine(cfg)
		{
		}

		~EngineGuard()
		{
			engine.Release(gpu);
		}

		EngineGuard(const EngineGuard&) = delete;
		EngineGuard& operator=(const EngineGuard&) = delete;
	};

	static std::wstring FmtMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0)
	{
		char buf[320];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	static double RmsDiffRect(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& r)
	{
		double sumSq = 0.0;
		size_t cnt = 0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double d = a.at(x, y) - b.at(x, y);
				sumSq += d * d;
				++cnt;
			}
		}
		return cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
	}

	static double MaxAbsDiffRect(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& r)
	{
		double mx = 0.0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double d = std::fabs(a.at(x, y) - b.at(x, y));
				if (d > mx)
					mx = d;
			}
		}
		return mx;
	}

	TEST_CLASS(TdV2Sota4EdgeAwareUpsampleTests)
	{
	public:
		TEST_METHOD(SpatialGuidedBilinearWeightSnapsToCleanLumaSilhouetteAndMatchesBilinearOnFlatLuma)
		{
			// 2x2 cell grid (cellsW=2, cellsH=2, cellPx=16)
			const double weights[4] = { 0.0, 0.84, 0.0, 0.84 }; // Left col occluded (0.0), right col valid (0.84)
			const double diagC[4]   = { 36.0, 36.0, 36.0, 36.0 }; // sigma=6 -> 3-sigma Huber knee = 18 codes

			// 1. Uniform luminance -> exact match with SpatialBilinearHuberWeight
			const double flatMean[4] = { 500.0, 500.0, 500.0, 500.0 };
			const double flatStd[4]  = { 12.0,  12.0,  12.0,  12.0  };
			const double wGuidedFlat = SpatialGuidedBilinearWeight(
				weights, diagC, flatMean, flatStd, 2, 2, 18.0, 14.0, 16, 8.0, 500.0, 1.0, true);
			const double wBilinFlat = SpatialBilinearHuberWeight(
				weights, diagC, 2, 2, 18.0, 14.0, 16, 8.0);
			Assert::AreEqual(wBilinFlat, wGuidedFlat, 1e-12,
				L"Uniform cellMeanY must match SpatialBilinearHuberWeight identically");

			// 2. Sharp silhouette step: left col dark occluded (mean=150, std=2), right col mostly bright valid (mean=720, std=180)
			//    At dark silhouette pixel x=17 (cleanY=150, diff=9.0 < 18.0 Huber knee!):
			const double edgeMean[4] = { 150.0, 720.0, 150.0, 720.0 };
			const double edgeStd[4]  = { 2.0,   180.0, 2.0,   180.0 };
			const double wBilinDark = SpatialBilinearHuberWeight(weights, diagC, 2, 2, 17.0, 14.0, 16, 9.0);
			const double wGuidedDark = SpatialGuidedBilinearWeight(
				weights, diagC, edgeMean, edgeStd, 2, 2, 17.0, 14.0, 16, 9.0, 150.0, 1.0, true);
			Assert::IsTrue(wBilinDark > 0.45 && wGuidedDark < 0.06,
				FmtMsg("Dark silhouette pixel must suppress bleeding right-cell weight: guided=%.4f vs bilin=%.4f",
					wGuidedDark, wBilinDark).c_str());

			//    At bright valid pixel x=20 (cleanY=820, diff=9.0): guided snaps to ~full 0.84 instead of bilinear ~0.65!
			const double wBilinBright = SpatialBilinearHuberWeight(weights, diagC, 2, 2, 20.0, 14.0, 16, 9.0);
			const double wGuidedBright = SpatialGuidedBilinearWeight(
				weights, diagC, edgeMean, edgeStd, 2, 2, 20.0, 14.0, 16, 9.0, 820.0, 1.0, true);
			Assert::IsTrue(wGuidedBright > 0.83 && wBilinBright < 0.68,
				FmtMsg("Bright valid near-edge pixel must snap to full right-cell weight: guided=%.4f vs bilin=%.4f",
					wGuidedBright, wBilinBright).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CpuAndGpuEdgeAwareWeightUpsamplePreventsBelowHuberKneeSilhouetteHaloBleed)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CpuAndGpuEdgeAwareWeightUpsamplePreventsBelowHuberKneeSilhouetteHaloBleed)
		{
			WarpContext ctx;

			const int w = 64;
			const int h = 64;
			const int cw = w / 16; // 4
			const int ch = h / 16; // 4
			const ActiveRect full = ActiveRect::Full(w, h);

			// Sharp vertical silhouette at x = 19 bisecting cell column cx = 1 (x in [16, 32)):
			//   - Left side (x < 19, covering all of cx = 0 [0..15] plus [16..18] of cx = 1):
			//     dark occluded foreground silhouette (master ~ 150.0). Cell cx = 0 is marked rack-focus / occluded
			//     (w[cx=0] == 0.0), while pixels x in [16..18] inside cx = 1 carry a +11.0-code occlusion ghost in
			//     prevPeak & nextPeak that sits BELOW cx = 1's 3-sigma Huber knee!
			//   - Right side (x >= 19, covering [19..31] of cx = 1 and all of cx = 2, 3):
			//     bright textured background (master ~ 810.0 +/- 24.0) where current B-frame lost 80% of mid+fine detail
			//     (large C_kk ~ 120 -> 3-sigma Huber knee ~ 33 codes > 11-code occlusion mismatch!).
			TdV2Plane master(w, h, 0.0);
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					if (x < 19)
					{
						master.at(x, y) = 150.0 + 0.8 * std::sin(2.0 * kPi * y / 13.0);
					}
					else
					{
						const double tex = 18.0 * std::sin(2.0 * kPi * (x / 5.0 + y / 6.0) + 0.4)
						                 + 15.0 * std::cos(2.0 * kPi * (x / 3.1 - y / 3.7) + 1.1);
						master.at(x, y) = 810.0 + tex;
					}
				}
			}

			TdV2Plane curClean = master;
			TdV2Plane prevClean = master;
			TdV2Plane nextClean = master;
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					if (x < 19)
					{
						// Uncorrupted dark silhouette on current frame, but prevPeak & nextPeak have +11-code occlusion mismatch
						// modulated at mid/fine spatial frequency so M/H bands carry the below-Huber-knee halo hazard!
						const double ghost = 11.0 * std::sin(2.0 * kPi * y / 4.0 + 0.3);
						prevClean.at(x, y) = master.at(x, y) + ghost;
						nextClean.at(x, y) = master.at(x, y) + 0.9 * ghost;
					}
					else
					{
						// Bright background lost 80% of its texture on the current B-frame, while prevPeak & nextPeak keep it!
						const double tex = master.at(x, y) - 810.0;
						curClean.at(x, y) = 810.0 + 0.20 * tex;
					}
				}
			}

			// Cell column cx = 0 is occluded (explicitFocusChangeCells = 1 -> cellW == 0 on cx = 0)
			std::vector<uint8_t> focusChangeMask(static_cast<size_t>(cw * ch), 0u);
			for (int cy = 0; cy < ch; ++cy)
				focusChangeMask[static_cast<size_t>(cy * cw + 0)] = 1u;

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			FrameProcessInput in;
			in.seq = 401;
			in.receivedY = curClean;
			in.hasCleanSplit = true;
			in.cleanY = curClean;
			in.activeRect = full;
			in.prevPeak = { &prevClean, &stillMf, false };
			in.nextPeak = { &nextClean, &stillMf, false };
			in.explicitFocusChangeCells = &focusChangeMask;
			in.hasStepPrior = true;
			in.estimatedStepDelta = 40.0;
			in.deadZonePhi = kDeadZonePhiInter;
			in.zeroCoeffFraction = 0.35;

			const ActiveRect darkEdgeStrip{ 16, 8, 19, 56 };   // Dark silhouette pixels inside cx=1 just right of x=16
			const ActiveRect brightEdgeStrip{ 19, 8, 24, 56 }; // Bright valid pixels inside cx=1 just right of silhouette

			// Verify on BOTH Reference and RealTime presets!
			for (int presetIdx = 0; presetIdx < 2; ++presetIdx)
			{
				const Preset preset = (presetIdx == 0) ? Preset::Reference : Preset::RealTime;

				Config cfgBilin = MakePreset(preset, Mode::On);
				cfgBilin.enableLongAnchor = false;
				cfgBilin.enableCarryForward = false;
				cfgBilin.momentPoolRadiusCells = 0; // Unpooled per-cell solve isolates cx=1 cleanly
				cfgBilin.enablePerCornerHuber = true;
				cfgBilin.enableEdgeAwareWeightUpsample = false;

				Config cfgGuided = cfgBilin;
				cfgGuided.enableEdgeAwareWeightUpsample = true;

				EngineGuard gpuBilin(ctx.gpu, cfgBilin), gpuGuided(ctx.gpu, cfgGuided);
				const GpuProcessResult gBilin = gpuBilin.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
				const GpuProcessResult gGuided = gpuGuided.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
				Assert::IsTrue(gBilin.succeeded && gGuided.succeeded, L"WARP ProcessFrame failed in SOTA-4");

				if (preset == Preset::Reference)
				{
					TdV2ReferenceEngine cpuBilin(cfgBilin), cpuGuided(cfgGuided);
					const FrameProcessResult cBilin = cpuBilin.ProcessFrame(in, nullptr);
					const FrameProcessResult cGuided = cpuGuided.ProcessFrame(in, nullptr);
					Assert::IsTrue(MaxAbsDiffRect(cBilin.outputY, gBilin.outputY, full) <= kGpuCpuTol,
						L"SOTA-4 bilinear CPU vs GPU parity > 1e-3");
					Assert::IsTrue(MaxAbsDiffRect(cGuided.outputY, gGuided.outputY, full) <= kGpuCpuTol,
						L"SOTA-4 edge-aware guided CPU vs GPU parity > 1e-3");
				}

				// Mandatory Same-Input Negative Control (Rule 3):
				// Spatial bilinear (enableEdgeAwareWeightUpsample=false) bleeds > 2.0 RMS codes of ghost halo onto darkEdgeStrip!
				const double bilinHaloBleed = RmsDiffRect(gBilin.outputY, master, darkEdgeStrip);
				const double guidedHaloBleed = RmsDiffRect(gGuided.outputY, master, darkEdgeStrip);
				Assert::IsTrue(bilinHaloBleed > 2.0,
					FmtMsg("Negative control failed: spatial bilinear halo bleed=%.4f <= 2.0 on dark silhouette strip", bilinHaloBleed).c_str());
				Assert::IsTrue(guidedHaloBleed < bilinHaloBleed * 0.45,
					FmtMsg("SOTA-4 guided upsample MUST suppress silhouette halo bleed by > 55%%: guided=%.4f vs bilin=%.4f",
						guidedHaloBleed, bilinHaloBleed).c_str());

				// Simultaneously, on the bright valid side right across the silhouette (x in [19..24)),
				// guided upsample snaps away from occluded cx=0 (w=0) and recovers MORE true detail than spatial bilinear!
				const double rawBrightErr = RmsDiffRect(curClean, master, brightEdgeStrip);
				const double bilinBrightErr = RmsDiffRect(gBilin.outputY, master, brightEdgeStrip);
				const double guidedBrightErr = RmsDiffRect(gGuided.outputY, master, brightEdgeStrip);
				Assert::IsTrue(guidedBrightErr < bilinBrightErr - 0.5 && bilinBrightErr < rawBrightErr,
					FmtMsg("SOTA-4 guided upsample must also recover sharper detail on valid side of silhouette: guided=%.4f < bilin=%.4f < raw=%.4f",
						guidedBrightErr, bilinBrightErr, rawBrightErr).c_str());
			}
		}
	};
} // namespace TdV2Sota4EdgeAwareUpsampleNs
} // namespace Tests
