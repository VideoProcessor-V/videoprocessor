// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 5 (TD-SOTA-5) Adversarial Red-Team Tests:
// Grain-Free 3x3 LDS Neighborhood 3-Sigma C^1 Soft Clamp on M/H Bands:
//   1. SoftClampToNeighborhood3SigmaSamuelsonIdentityC1ContinuityAnd3SigmaContainment (CPU):
//      - Exact Samuelson interior identity |v_i - mean| <= sqrt(8)*sigma across arbitrary 3x3 patches
//        (step edge, single-pixel impulse, ramp, checkerboard -> 0.0 bit-exact error!)
//      - C^1 value & slope == 1.0 continuity at |d| = sqrt(8)*sigma and strict [mean - 3*sigma, mean + 3*sigma] bound
//   2. CpuAndGpuNeighborhoodSoftClampSuppressesMicroMotionSpikeWhileLeavingRigidFineTextureBitIdentical (GpuWarp):
//      - CPU TdV2ReferenceEngine + real D3D11 WARP TdV2GpuEngine (Reference & RealTime compact LDS paths)
//      - Unmasked organic micro-motion outlier spike (thin moving eyelash/ripple on M/H below cell-wide Huber knee):
//        enableNeighborhoodSoftClamp=true suppresses the ghost spike vs enableNeighborhoodSoftClamp=false negative control!
//      - Static/rigid fine texture remains untouched (Samuelson interior identity proof on full engine)!
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
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2Sota5NeighborhoodClampNs
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

	TEST_CLASS(TdV2Sota5NeighborhoodClampTests)
	{
	public:
		TEST_METHOD(SoftClampToNeighborhood3SigmaSamuelsonIdentityC1ContinuityAnd3SigmaContainment)
		{
			// 1. Samuelson's inequality exact bit-identity across worst-case 3x3 patches:
			//    Even a single-pixel Dirac impulse [1000, 0, 0, 0, 0, 0, 0, 0, 0] achieves the exact Samuelson
			//    extremum |v_0 - mean| == sqrt(8) * sigma! Every sample in every 3x3 patch MUST stay bit-exact!
			const double patches[4][9] = {
				{ 1000.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },        // Exact Samuelson boundary r0 = sqrt(8)*sigma!
				{ 100.0, 100.0, 100.0, 900.0, 900.0, 900.0, 900.0, 900.0, 900.0 }, // Sharp step edge
				{ -25.0, 25.0, -25.0, 25.0, -25.0, 25.0, -25.0, 25.0, -25.0 },     // Nyquist checkerboard
				{ 12.0, 19.0, 27.0, 31.0, 44.0, 58.0, 63.0, 79.0, 95.0 }           // Arbitrary gradient
			};
			for (size_t p = 0; p < 4; ++p)
			{
				double mean = 0.0;
				for (int i = 0; i < 9; ++i)
					mean += patches[p][i];
				mean /= 9.0;
				double var = 0.0;
				for (int i = 0; i < 9; ++i)
				{
					const double d = patches[p][i] - mean;
					var += d * d;
				}
				const double sig = std::sqrt(var / 9.0);
				for (int i = 0; i < 9; ++i)
				{
					// Shrink by 1e-12 on the Dirac extremum only if FP rounding nudges |d| by 1 ULP; test exact sample too
					const double v = patches[p][i];
					const double clamped = SoftClampToNeighborhood3Sigma(v, mean, sig);
					Assert::AreEqual(v, clamped, 1e-12,
						L"Samuelson interior theorem: every actual 3x3 neighborhood sample must be preserved untouched");
				}
			}

			// 2. C^1 continuity (value and derivative == 1.0 at r0 = sqrt(8)*sigma) & strict 3-sigma containment
			const double mu = 250.0;
			const double sig = 10.0;
			const double r0 = std::sqrt(8.0) * sig;
			const double eps = 1e-5;
			const double fLeft = SoftClampToNeighborhood3Sigma(mu + r0 - eps, mu, sig);
			const double fAt   = SoftClampToNeighborhood3Sigma(mu + r0,       mu, sig);
			const double fRight = SoftClampToNeighborhood3Sigma(mu + r0 + eps, mu, sig);
			Assert::AreEqual(mu + r0, fAt, 1e-12, L"Value at Samuelson boundary r0 must equal mu + r0");
			const double slopeLeft = (fAt - fLeft) / eps;
			const double slopeRight = (fRight - fAt) / eps;
			Assert::AreEqual(1.0, slopeLeft, 1e-4, L"Left slope at r0 must be 1.0");
			Assert::AreEqual(1.0, slopeRight, 1e-4, L"Right C^1 shoulder slope at r0 must be 1.0");

			// Strict containment inside (mu - 3*sig, mu + 3*sig) for extreme +/- outliers
			const double posOut = SoftClampToNeighborhood3Sigma(mu + 1e6, mu, sig);
			const double negOut = SoftClampToNeighborhood3Sigma(mu - 1e6, mu, sig);
			Assert::IsTrue(posOut > mu + r0 && posOut <= mu + 3.0 * sig, L"Positive outlier must stay <= mu + 3*sigma");
			Assert::IsTrue(negOut < mu - r0 && negOut >= mu - 3.0 * sig, L"Negative outlier must stay >= mu - 3*sigma");
			Assert::AreEqual(mu, SoftClampToNeighborhood3Sigma(mu + 50.0, mu, 0.0), 1e-12, L"Zero sigma must clamp to mean");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CpuAndGpuNeighborhoodSoftClampSuppressesMicroMotionSpikeWhileLeavingRigidFineTextureBitIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CpuAndGpuNeighborhoodSoftClampSuppressesMicroMotionSpikeWhileLeavingRigidFineTextureBitIdentical)
		{
			WarpContext ctx;

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);
			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			// Part A: Unmasked organic micro-motion outlier spike (thin 1-px moving eyelash at y=24, x in [20..28)):
			//   - Surrounding region has rich texture & B-frame compression loss so cell C_kk ~ 64 (3-sigma Huber knee ~ 24 codes),
			//   - At y in [22..26], x in [18..30], the skin patch on current frame is locally smooth (3x3 M/H nbrSigma < 0.7 codes),
			//   - prevPeak & nextPeak both contain a thin +18-code eyelash at y=24, x in [20..28) (below cell Huber knee 24,
			//     but >> 3 * nbrSigma locally!).
			TdV2Plane masterSkin(w, h, 460.0);
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const bool inSmoothPatch = (x >= 17 && x < 31 && y >= 20 && y < 29);
					if (inSmoothPatch)
					{
						masterSkin.at(x, y) = 460.0 + 0.4 * std::sin(2.0 * kPi * x / 9.0) + 0.3 * std::cos(2.0 * kPi * y / 7.0);
					}
					else
					{
						masterSkin.at(x, y) = 460.0
							+ 20.0 * std::sin(2.0 * kPi * (x / 4.5 + y / 5.5) + 0.3)
							+ 18.0 * std::cos(2.0 * kPi * (x / 2.8 - y / 3.2) + 1.0);
					}
				}
			}

			TdV2Plane curFrame = masterSkin;
			TdV2Plane prevFrame = masterSkin;
			TdV2Plane nextFrame = masterSkin;
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const bool inSmoothPatch = (x >= 17 && x < 31 && y >= 20 && y < 29);
					if (!inSmoothPatch)
					{
						// Genuine compression loss on textured background so cell weights solve high (~0.85)
						curFrame.at(x, y) = 460.0 + 0.25 * (masterSkin.at(x, y) - 460.0);
					}
				}
			}
			// Stamp thin 1-pixel moving eyelash outlier in prevPeak & nextPeak at y = 24, x in [20..28)
			const ActiveRect eyelashRect{ 20, 24, 28, 25 };
			for (int x = eyelashRect.x0; x < eyelashRect.x1; ++x)
			{
				prevFrame.at(x, 24) += 18.0;
				nextFrame.at(x, 24) += 17.5;
			}

			FrameProcessInput spikeIn;
			spikeIn.seq = 501;
			spikeIn.receivedY = curFrame;
			spikeIn.hasCleanSplit = true;
			spikeIn.cleanY = curFrame;
			spikeIn.activeRect = full;
			spikeIn.prevPeak = { &prevFrame, &stillMf, false };
			spikeIn.nextPeak = { &nextFrame, &stillMf, false };
			spikeIn.hasStepPrior = true;
			spikeIn.estimatedStepDelta = 36.0;
			spikeIn.deadZonePhi = kDeadZonePhiInter;
			spikeIn.zeroCoeffFraction = 0.35;

			for (int presetIdx = 0; presetIdx < 2; ++presetIdx)
			{
				const Preset preset = (presetIdx == 0) ? Preset::Reference : Preset::RealTime;

				Config cfgOff = MakePreset(preset, Mode::On);
				cfgOff.enableLongAnchor = false;
				cfgOff.enableCarryForward = false;
				cfgOff.enableNeighborhoodSoftClamp = false;

				Config cfgOn = cfgOff;
				cfgOn.enableNeighborhoodSoftClamp = true;

				EngineGuard gpuOff(ctx.gpu, cfgOff), gpuOn(ctx.gpu, cfgOn);
				const GpuProcessResult gOff = gpuOff.engine.ProcessFrame(ctx.gpu, ctx.dispatch, spikeIn, nullptr, nullptr);
				const GpuProcessResult gOn = gpuOn.engine.ProcessFrame(ctx.gpu, ctx.dispatch, spikeIn, nullptr, nullptr);
				Assert::IsTrue(gOff.succeeded && gOn.succeeded, L"WARP ProcessFrame failed in SOTA-5");

				if (preset == Preset::Reference)
				{
					TdV2ReferenceEngine cpuOff(cfgOff), cpuOn(cfgOn);
					const FrameProcessResult cOff = cpuOff.ProcessFrame(spikeIn, nullptr);
					const FrameProcessResult cOn = cpuOn.ProcessFrame(spikeIn, nullptr);
					Assert::IsTrue(MaxAbsDiffRect(cOff.outputY, gOff.outputY, full) <= kGpuCpuTol,
						L"SOTA-5 off CPU vs GPU mismatch > 1e-3");
					Assert::IsTrue(MaxAbsDiffRect(cOn.outputY, gOn.outputY, full) <= kGpuCpuTol,
						L"SOTA-5 on CPU vs GPU mismatch > 1e-3");
				}

				// Mandatory Same-Input Negative Control: without neighborhood soft clamp, eyelash spike ghosts strongly (> 6 codes)!
				const double ghostPeakOff = MaxAbsDiffRect(gOff.outputY, masterSkin, eyelashRect);
				const double ghostPeakOn  = MaxAbsDiffRect(gOn.outputY,  masterSkin, eyelashRect);
				const double ghostRmsOff  = RmsDiffRect(gOff.outputY, masterSkin, eyelashRect);
				const double ghostRmsOn   = RmsDiffRect(gOn.outputY,  masterSkin, eyelashRect);
				Assert::IsTrue(ghostPeakOff > 6.0 && ghostRmsOff > 5.0,
					FmtMsg("Negative control failed: eyelash ghost spike with clamp=false too small (peak=%.4f, rms=%.4f)",
						ghostPeakOff, ghostRmsOff).c_str());
				Assert::IsTrue(ghostPeakOn < ghostPeakOff * 0.35 && ghostRmsOn < ghostRmsOff * 0.35,
					FmtMsg("SOTA-5 neighborhood clamp=true MUST suppress eyelash ghost spike by > 65%%: peakOn=%.4f vs peakOff=%.4f",
						ghostPeakOn, ghostPeakOff).c_str());
			}

			// Part B: Samuelson Static/Rigid Fine Texture Identity Proof on Full Engine (CPU + WARP GPU):
			//   Where fine/mid texture is static/rigid and cur == ref on M/H (while L-band has gentle quantization banding
			//   so cell weights solve > 0!), SoftClampToNeighborhood3Sigma MUST leave M/H 100% bit-identical!
			{
				TdV2Plane rigidMaster(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const double step = ((x / 5 + y / 7) % 2 == 0) ? 35.0 : -35.0;
						const double fine = 16.0 * std::sin(2.0 * kPi * (x / 2.6 - y / 3.1) + 0.5);
						rigidMaster.at(x, y) = 500.0 + step + fine;
					}
				}
				// Add low-frequency L-band banding offset on curRigid while keeping M/H rigid detail identical to refs!
				TdV2Plane curRigid = rigidMaster;
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						curRigid.at(x, y) += 4.5 * std::sin(2.0 * kPi * x / 32.0);

				FrameProcessInput rigidIn = spikeIn;
				rigidIn.seq = 502;
				rigidIn.receivedY = curRigid;
				rigidIn.cleanY = curRigid;
				rigidIn.prevPeak = { &rigidMaster, &stillMf, false };
				rigidIn.nextPeak = { &rigidMaster, &stillMf, false };

				Config cfgOff = MakePreset(Preset::Reference, Mode::On);
				cfgOff.enableLongAnchor = false;
				cfgOff.enableCarryForward = false;
				cfgOff.enableNeighborhoodSoftClamp = false;
				Config cfgOn = cfgOff;
				cfgOn.enableNeighborhoodSoftClamp = true;

				TdV2ReferenceEngine cpuOff(cfgOff), cpuOn(cfgOn);
				EngineGuard gpuOff(ctx.gpu, cfgOff), gpuOn(ctx.gpu, cfgOn);
				const FrameProcessResult cOff = cpuOff.ProcessFrame(rigidIn, nullptr);
				const FrameProcessResult cOn = cpuOn.ProcessFrame(rigidIn, nullptr);
				const GpuProcessResult gOff = gpuOff.engine.ProcessFrame(ctx.gpu, ctx.dispatch, rigidIn, nullptr, nullptr);
				const GpuProcessResult gOn = gpuOn.engine.ProcessFrame(ctx.gpu, ctx.dispatch, rigidIn, nullptr, nullptr);
				Assert::IsTrue(gOff.succeeded && gOn.succeeded);
				Assert::IsTrue(RmsDiffRect(gOn.outputY, curRigid, ActiveRect{ 8, 8, 56, 56 }) > 1.0,
					L"Rigid texture test must actively fuse references (non-vacuous)");
				Assert::AreEqual(0.0, MaxAbsDiffRect(cOff.outputY, cOn.outputY, full),
					L"Samuelson interior identity: CPU clamp=true must be bit-identical to clamp=false on static rigid fine texture");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gOff.outputY, gOn.outputY, full),
					L"Samuelson interior identity: GPU clamp=true must be bit-identical to clamp=false on static rigid fine texture");
			}
		}
	};
} // namespace TdV2Sota5NeighborhoodClampNs
} // namespace Tests
