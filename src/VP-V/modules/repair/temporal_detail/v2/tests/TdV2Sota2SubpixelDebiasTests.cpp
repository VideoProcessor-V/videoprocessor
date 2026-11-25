// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 2 (TD-SOTA-2) Adversarial Red-Team Tests:
// Sub-Pixel Bicubic H-Band Attenuation Debiasing in EstimateC_A1:
//   1. FineBandBicubicWarpGainAndSubpixelBiasMatchSpatialConvolutionPhysics (CPU):
//      - Independent 2-D spatial Catmull-Rom bicubic warp + U2 BandFromBase high-pass convolution
//        verification of FineBandBicubicWarpVarianceGain (zero formula tautology!)
//      - Exact integer-shift identity (gain == 1.0, bias == 0.0 when either partner is integer-shifted)
//   2. CpuAndGpuHalfPelPanSubpixelHDebiasPreventsFalseSharpnessLossWhileIntegerPanStaysIdentical (GpuWarp):
//      - CPU TdV2ReferenceEngine + real D3D11 WARP TdV2GpuEngine
//      - Sharp high-frequency texture where BOTH prevPeak (-0.5, -0.5) and nextPeak (+0.5, +0.5) sit at
//        half-pel camera pan: enableSubpixelHAttenuationDebias=true removes false upward c_A1 bias and
//        preserves sharper H-band energy vs enableSubpixelHAttenuationDebias=false negative control!
//      - Integer-pel (0.0, 0.0) and (-1.0, +1.0) pan stays bit-identical!
//      - Genuine wiped H-band positive control at half-pel (-0.5, -0.5) / (+0.5, +0.5) still recovers!
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/modules/motion/estimation/composed/WarpSampler.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2Sota2SubpixelDebiasNs
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

	static std::wstring FmtMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0)
	{
		char buf[320];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	// Continuous high-frequency analytical texture with rich H-band detail near 2.6-3.4 px wavelength.
	static double HighFreqMasterAt(double x, double y)
	{
		const double base = 500.0 + 40.0 * std::sin(2.0 * kPi * x / 53.0 + 0.2) * std::cos(2.0 * kPi * y / 47.0);
		const double fine1 = 26.0 * std::sin(2.0 * kPi * (x / 2.7 + y / 3.1) + 0.35);
		const double fine2 = 22.0 * std::cos(2.0 * kPi * (x / 2.9 - y / 2.6) + 1.15);
		const double fine3 = 16.0 * std::sin(2.0 * kPi * (x / 2.35) + 0.8) * std::cos(2.0 * kPi * (y / 2.45) - 0.5);
		return base + fine1 + fine2 + fine3;
	}

	static TdV2Plane RenderShiftedHighFreq(int w, int h, double dx, double dy)
	{
		TdV2Plane p(w, h, 0.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				p.at(x, y) = HighFreqMasterAt(static_cast<double>(x) - dx, static_cast<double>(y) - dy);
		return p;
	}

	static double BandEnergyRms(const TdV2Plane& p, const ActiveRect& roi)
	{
		const ThreeBandPlanes b = DecomposeActiveBands(p, ActiveRect::Full(p.w, p.h));
		double sumSq = 0.0;
		size_t cnt = 0;
		for (int y = roi.y0; y < roi.y1; ++y)
		{
			for (int x = roi.x0; x < roi.x1; ++x)
			{
				const double v = b.H.at(x, y);
				sumSq += v * v;
				++cnt;
			}
		}
		return cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
	}

	static double RmsDiffRect(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& roi)
	{
		double sumSq = 0.0;
		size_t cnt = 0;
		for (int y = roi.y0; y < roi.y1; ++y)
		{
			for (int x = roi.x0; x < roi.x1; ++x)
			{
				const double d = a.at(x, y) - b.at(x, y);
				sumSq += d * d;
				++cnt;
			}
		}
		return cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
	}

	static double MaxAbsDiffRect(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& roi)
	{
		double mx = 0.0;
		for (int y = roi.y0; y < roi.y1; ++y)
		{
			for (int x = roi.x0; x < roi.x1; ++x)
			{
				const double d = std::fabs(a.at(x, y) - b.at(x, y));
				if (d > mx)
					mx = d;
			}
		}
		return mx;
	}

	TEST_CLASS(TdV2Sota2SubpixelDebiasTests)
	{
	public:
		TEST_METHOD(FineBandBicubicWarpGainAndSubpixelBiasMatchSpatialConvolutionPhysics)
		{
			// 1. Exact integer-pel shift identity: gain == 1.0, bias == 0.0 whenever EITHER partner is integer-shifted
			Assert::AreEqual(1.0, FineBandBicubicWarpVarianceGain(0.0, 0.0), 1e-14, L"(0,0) gain must be exact 1.0");
			Assert::AreEqual(1.0, FineBandBicubicWarpVarianceGain(2.0, -3.0), 1e-14, L"(2,-3) integer gain must be exact 1.0");
			Assert::AreEqual(0.0, ComputeSubpixelHAttenuationBias(0.0, 0.0, 0.5, 0.5, 400.0, 0.0), 0.0,
				L"Integer partner P + half-pel partner Q must have 0.0 shared subpixel bias");
			Assert::AreEqual(0.0, ComputeSubpixelHAttenuationBias(0.5, 0.5, -1.0, 2.0, 400.0, 0.0), 0.0,
				L"Half-pel partner P + integer partner Q must have 0.0 shared subpixel bias");

			// Half-pel (0.5, 0.5) has minimum gain in (0, 1) and strictly positive bias on clean H-band texture
			const double gHalf2D = FineBandBicubicWarpVarianceGain(0.5, 0.5);
			const double gHalf1D = FineBandBicubicWarpVarianceGain(0.5, 0.0);
			Assert::IsTrue(gHalf2D > 0.15 && gHalf2D < gHalf1D && gHalf1D < 0.85,
				FmtMsg("Expected 0.15 < g(0.5,0.5)=%.4f < g(0.5,0)=%.4f < 0.85", gHalf2D, gHalf1D).c_str());

			// 2. Independent 2-D spatial impulse convolution proof (zero self-reference!):
			//    Place unit impulse at center of 33x33 grid, apply Catmull-Rom shift (fx, fy) then (I - G_fine),
			//    and verify ||(I - G_fine) * W(fx, fy) delta||_2^2 / ||(I - G_fine) delta||_2^2 == FineBandBicubicWarpVarianceGain!
			const int N = 33;
			const int c = N / 2;
			TdV2Plane imp(N, N, 0.0);
			imp.at(c, c) = 1.0;
			const ActiveRect full = ActiveRect::Full(N, N);
			const ThreeBandPlanes impBands = DecomposeActiveBands(imp, full);
			double baseEnergy = 0.0;
			for (double v : impBands.H.v)
				baseEnergy += v * v;

			const double shifts[][2] = { { 0.5, 0.5 }, { 0.5, 0.0 }, { 0.25, 0.75 }, { 0.0, 0.0 } };
			for (size_t s = 0; s < sizeof(shifts) / sizeof(shifts[0]); ++s)
			{
				const double fx = shifts[s][0];
				const double fy = shifts[s][1];
				double wx[4] = {}, wy[4] = {};
				VpComposed::CubicWeights(fx, wx);
				VpComposed::CubicWeights(fy, wy);

				// Warping white noise with weights w[i] has impulse response at (-1+i, -1+j)
				TdV2Plane wImp(N, N, 0.0);
				for (int j = 0; j < 4; ++j)
					for (int i = 0; i < 4; ++i)
						wImp.at(c - 1 + i, c - 1 + j) = wx[i] * wy[j];

				const ThreeBandPlanes wBands = DecomposeActiveBands(wImp, full);
				double warpedEnergy = 0.0;
				for (double v : wBands.H.v)
					warpedEnergy += v * v;

				const double empiricalGain = warpedEnergy / baseEnergy;
				const double analyticalGain = FineBandBicubicWarpVarianceGain(fx, fy);
				Assert::AreEqual(empiricalGain, analyticalGain, 1e-12,
					FmtMsg("2-D spatial impulse Parseval mismatch at (%.2f, %.2f): emp=%.8f ana=%.8f", fx, fy, empiricalGain, analyticalGain).c_str());
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CpuAndGpuHalfPelPanSubpixelHDebiasPreventsFalseSharpnessLossWhileIntegerPanStaysIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CpuAndGpuHalfPelPanSubpixelHDebiasPreventsFalseSharpnessLossWhileIntegerPanStaysIdentical)
		{
			WarpContext ctx;

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);
			const ActiveRect interior{ 8, 8, 56, 56 }; // Avoid active-border bilinear edge strip

			// Case A: Crisp high-frequency current frame with constant-velocity 0.5 px/frame camera pan:
			//   prevPeak sits at (-0.5, -0.5) (warped with mv = (-0.5, -0.5) -> fractional phase 0.5, 0.5),
			//   nextPeak sits at (+0.5, +0.5) (warped with mv = (+0.5, +0.5) -> fractional phase 0.5, 0.5).
			// Current frame has ZERO coding loss! Any H-band attenuation comes purely from false c_A1 upward bias.
			const TdV2Plane curSharp = RenderShiftedHighFreq(w, h, 0.0, 0.0);
			const TdV2Plane prevHalf = RenderShiftedHighFreq(w, h, -0.5, -0.5); // sample at (x - 0.5, y - 0.5) -> HighFreqMasterAt(x, y)!
			const TdV2Plane nextHalf = RenderShiftedHighFreq(w, h, +0.5, +0.5); // sample at (x + 0.5, y + 0.5) -> HighFreqMasterAt(x, y)!

			const CellMotionField mfPrevHalf = CellMotionField::Uniform(cw, ch, -0.5, -0.5, -0.5, -0.5, RegionMotionClass::Still);
			const CellMotionField mfNextHalf = CellMotionField::Uniform(cw, ch, +0.5, +0.5, +0.5, +0.5, RegionMotionClass::Still);

			FrameProcessInput halfIn;
			halfIn.seq = 301;
			halfIn.receivedY = curSharp;
			halfIn.hasCleanSplit = true;
			halfIn.cleanY = curSharp;
			halfIn.activeRect = full;
			halfIn.prevPeak = { &prevHalf, &mfPrevHalf, false };
			halfIn.nextPeak = { &nextHalf, &mfNextHalf, false };
			halfIn.hasStepPrior = true;
			halfIn.estimatedStepDelta = 44.0; // Realistic B-frame QP step so cA2 does not hide false cA1 bias or Case-C wipe recovery
			halfIn.deadZonePhi = kDeadZonePhiInter;
			halfIn.zeroCoeffFraction = 0.35;

			Config cfgOff = MakePreset(Preset::Reference, Mode::On);
			cfgOff.enableLongAnchor = false;
			cfgOff.enableCarryForward = false;
			cfgOff.estimator = VarianceEstimator::A3_ConservativeMin;
			cfgOff.enableSubpixelHAttenuationDebias = false;

			Config cfgOn = cfgOff;
			cfgOn.enableSubpixelHAttenuationDebias = true;

			TdV2ReferenceEngine cpuOff(cfgOff), cpuOn(cfgOn);
			EngineGuard gpuOff(ctx.gpu, cfgOff), gpuOn(ctx.gpu, cfgOn);

			const FrameProcessResult cOff = cpuOff.ProcessFrame(halfIn, nullptr);
			const FrameProcessResult cOn = cpuOn.ProcessFrame(halfIn, nullptr);
			const GpuProcessResult gOff = gpuOff.engine.ProcessFrame(ctx.gpu, ctx.dispatch, halfIn, nullptr, nullptr);
			const GpuProcessResult gOn = gpuOn.engine.ProcessFrame(ctx.gpu, ctx.dispatch, halfIn, nullptr, nullptr);
			Assert::IsTrue(gOff.succeeded && gOn.succeeded, L"WARP ProcessFrame failed in SOTA-2 half-pel test");

			// Strict CPU vs GPU parity on both configurations
			Assert::IsTrue(MaxAbsDiffRect(cOff.outputY, gOff.outputY, full) <= kGpuCpuTol, L"SOTA-2 off CPU/GPU mismatch");
			Assert::IsTrue(MaxAbsDiffRect(cOn.outputY, gOn.outputY, full) <= kGpuCpuTol, L"SOTA-2 on CPU/GPU mismatch");

			const double inHRms = BandEnergyRms(curSharp, interior);
			const double offHRms = BandEnergyRms(gOff.outputY, interior);
			const double onHRms = BandEnergyRms(gOn.outputY, interior);
			const double offFalseDamageRms = RmsDiffRect(gOff.outputY, curSharp, interior);
			const double onFalseDamageRms = RmsDiffRect(gOn.outputY, curSharp, interior);

			// Mandatory Same-Input Negative Control (Rule 3):
			// Without debias, shared half-pel bicubic attenuation creates false c_A1 > 0 and smears sharp H-band detail!
			Assert::IsTrue(offFalseDamageRms > 2.5 && offHRms < inHRms - 2.0,
				FmtMsg("Negative control failed: debias=false did not exhibit false half-pel H-band loss (damageRms=%.4f, inH=%.4f, offH=%.4f)",
					offFalseDamageRms, inHRms, offHRms).c_str());

			// With debias=true: white-noise ComputeSubpixelHAttenuationBias suppresses false c_A1 bias without narrowband overfitting!
			Assert::IsTrue(onHRms > offHRms + 1.5 && onFalseDamageRms < offFalseDamageRms * 0.58,
				FmtMsg("SOTA-2 debias=true MUST preserve sharper H-band energy vs debias=false: onH=%.4f vs offH=%.4f, onErr=%.4f vs offErr=%.4f",
					onHRms, offHRms, onFalseDamageRms, offFalseDamageRms).c_str());

			// Case B: Integer-pel (-1.0, +1.0) pan control -> debias=true vs debias=false MUST be bit-identical!
			{
				const TdV2Plane prevInt = RenderShiftedHighFreq(w, h, -1.0, +1.0);
				const TdV2Plane nextInt = RenderShiftedHighFreq(w, h, +1.0, -1.0);
				const CellMotionField mfPrevInt = CellMotionField::Uniform(cw, ch, -1.0, +1.0, -1.0, +1.0, RegionMotionClass::Still);
				const CellMotionField mfNextInt = CellMotionField::Uniform(cw, ch, +1.0, -1.0, +1.0, -1.0, RegionMotionClass::Still);

				// Add genuine coding noise so weights are actively solved (> 0), proving non-trivial bit-identity!
				TdV2Plane curDamaged = curSharp;
				std::mt19937 rng(9205u);
				std::normal_distribution<double> nd(0.0, 5.0);
				for (double& v : curDamaged.v)
					v += nd(rng);

				FrameProcessInput intIn = halfIn;
				intIn.seq = 302;
				intIn.receivedY = curDamaged;
				intIn.cleanY = curDamaged;
				intIn.prevPeak = { &prevInt, &mfPrevInt, false };
				intIn.nextPeak = { &nextInt, &mfNextInt, false };

				TdV2ReferenceEngine cpuIntOff(cfgOff), cpuIntOn(cfgOn);
				EngineGuard gpuIntOff(ctx.gpu, cfgOff), gpuIntOn(ctx.gpu, cfgOn);
				const FrameProcessResult cIntOff = cpuIntOff.ProcessFrame(intIn, nullptr);
				const FrameProcessResult cIntOn = cpuIntOn.ProcessFrame(intIn, nullptr);
				const GpuProcessResult gIntOff = gpuIntOff.engine.ProcessFrame(ctx.gpu, ctx.dispatch, intIn, nullptr, nullptr);
				const GpuProcessResult gIntOn = gpuIntOn.engine.ProcessFrame(ctx.gpu, ctx.dispatch, intIn, nullptr, nullptr);
				Assert::IsTrue(gIntOff.succeeded && gIntOn.succeeded);
				Assert::IsTrue(RmsDiffRect(gIntOn.outputY, curDamaged, interior) > 1.5, L"Integer-pan control must be non-trivial");
				Assert::AreEqual(0.0, MaxAbsDiffRect(cIntOff.outputY, cIntOn.outputY, full),
					L"Integer-pel pan CPU output must be bit-identical between debias=false and debias=true");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gIntOff.outputY, gIntOn.outputY, full),
					L"Integer-pel pan GPU output must be bit-identical between debias=false and debias=true");
			}

			// Case C: Positive recovery check at half-pel (-0.5, -0.5) / (+0.5, +0.5) when current B-frame genuinely lost H-band detail!
			{
				const ThreeBandPlanes sharpB = DecomposeActiveBands(curSharp, full);
				TdV2Plane curWiped = curSharp;
				for (size_t i = 0; i < curWiped.v.size(); ++i)
					curWiped.v[i] -= 0.85 * sharpB.H.v[i];

				FrameProcessInput wipedIn = halfIn;
				wipedIn.seq = 303;
				wipedIn.receivedY = curWiped;
				wipedIn.cleanY = curWiped;

				EngineGuard gpuWipedOn(ctx.gpu, cfgOn);
				const GpuProcessResult gWiped = gpuWipedOn.engine.ProcessFrame(ctx.gpu, ctx.dispatch, wipedIn, nullptr, nullptr);
				Assert::IsTrue(gWiped.succeeded);
				const double beforeRms = RmsDiffRect(curWiped, curSharp, interior);
				const double afterRms = RmsDiffRect(gWiped.outputY, curSharp, interior);
				// Catmull-Rom half-pel (-0.5, -0.5) attenuates 2.3-2.9 px near-Nyquist sinusoids to ~40% ref amplitude (~0.80x floor):
				Assert::IsTrue(afterRms < beforeRms * 0.85,
					FmtMsg("SOTA-2 debias=true MUST still recover genuinely wiped H-band detail on half-pel pan: after=%.4f vs before=%.4f",
						afterRms, beforeRms).c_str());
			}
		}
	};
} // namespace TdV2Sota2SubpixelDebiasNs
} // namespace Tests
