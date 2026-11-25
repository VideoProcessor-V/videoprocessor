// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 1 (TD-SOTA-1) Adversarial Red-Team Tests:
// Per-Cell AQ & ST.2084 PQ Step Scaling in cellPriorTex.z / FrameProcessInput::cellStepScale:
//   1. ComputeCellStepScaleExactCodec6QpLawAndSt2084PqPhysics (CPU):
//      - Exact 2^(dQP/6) codec step doubling law and +/-24 QP [1/16, 16] derived bounds
//      - Independent numerical finite-difference verification of SMPTE ST 2084 PQ EOTF logarithmic
//        Weber slope relative to ITU-R BT.2408 diffuse white (203 nits), dark-toe tightening [0.25],
//        highlight relaxation [2.0], and non-finite / invalid-bit-depth fail-safe identity (1.0)
//   2. CpuAndGpuPerCellStepScaleUnlocksHighAqDarkCellTightensLowCellAndPreservesDefaultIdentity (GpuWarp):
//      - CPU TdV2ReferenceEngine + real D3D11 WARP TdV2GpuEngine (ProcessFrame & zero-copy ProcessFrameGpu)
//      - High-AQ dark PQ cell (scale > 1.0) unlocks strictly larger lawful recovery vs scale == 1.0 negative control
//      - Tight negative-AQ / shadow cell (scale < 1.0) tightens recovery strictly vs scale == 1.0 negative control
//      - Default unprovided cellStepScale / legacy RG32F cellPriorTex stays bit-identical to baseline 1.0
//      - Non-finite / non-positive cellStepScale closes affected cell cleanly with visible NonFiniteClosure
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

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
namespace TdV2Sota1StepScaleNs
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
			Assert::IsNotNull(log, L"pl_log_create failed on WARP");

			pl_d3d11_params dp = pl_d3d11_default_params;
			dp.force_software = true;
			dp.allow_software = true;
			d3d11 = pl_d3d11_create(log, &dp);
			Assert::IsNotNull(d3d11, L"pl_d3d11_create failed on WARP");

			gpu = d3d11->gpu;
			Assert::IsNotNull(gpu, L"pl_d3d11->gpu is null");

			dispatch = pl_dispatch_create(log, gpu);
			Assert::IsNotNull(dispatch, L"pl_dispatch_create failed on WARP");
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

	struct OwnedTex
	{
		pl_gpu gpu = nullptr;
		pl_tex tex = nullptr;

		explicit OwnedTex(pl_gpu g) : gpu(g) {}
		~OwnedTex()
		{
			if (gpu != nullptr && tex != nullptr)
				pl_tex_destroy(gpu, &tex);
		}

		OwnedTex(const OwnedTex&) = delete;
		OwnedTex& operator=(const OwnedTex&) = delete;
	};

	static std::wstring FmtMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0)
	{
		char buf[320];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	// Independent SMPTE ST 2084 EOTF & inverse EOTF (nits <-> normalized PQ signal e in [0, 1]) for physics verification.
	static double IndependentPqNitsToSignal(double nits)
	{
		constexpr double m1 = 2610.0 / 16384.0;
		constexpr double m2 = (2523.0 / 4096.0) * 128.0;
		constexpr double c1 = 3424.0 / 4096.0;
		constexpr double c2 = (2413.0 / 4096.0) * 32.0;
		constexpr double c3 = (2392.0 / 4096.0) * 32.0;
		const double y = (std::max)(0.0, (std::min)(1.0, nits / 10000.0));
		const double ym1 = std::pow(y, m1);
		return std::pow((c1 + c2 * ym1) / (1.0 + c3 * ym1), m2);
	}

	static double IndependentPqSignalToNits(double e)
	{
		constexpr double m1 = 2610.0 / 16384.0;
		constexpr double m2 = (2523.0 / 4096.0) * 128.0;
		constexpr double c1 = 3424.0 / 4096.0;
		constexpr double c2 = (2413.0 / 4096.0) * 32.0;
		constexpr double c3 = (2392.0 / 4096.0) * 32.0;
		const double ec = (std::max)(0.0, (std::min)(1.0, e));
		const double p = std::pow(ec, 1.0 / m2);
		const double num = (std::max)(0.0, p - c1);
		const double den = c2 - c3 * p;
		if (!(den > 1e-15))
			return 10000.0;
		return 10000.0 * std::pow(num / den, 1.0 / m1);
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

	static double MaxAbsRect(const TdV2Plane& p, const ActiveRect& r)
	{
		double mx = 0.0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double v = std::fabs(p.at(x, y));
				if (v > mx)
					mx = v;
			}
		}
		return mx;
	}

	TEST_CLASS(TdV2Sota1StepScaleTests)
	{
	public:
		TEST_METHOD(ComputeCellStepScaleExactCodec6QpLawAndSt2084PqPhysics)
		{
			// 1. Exact HEVC / AVC / AV1 6-QP doubling law (isPqHdr == false): 2^(dQP / 6)
			Assert::AreEqual(1.0, ComputeCellStepScale(0.0, 0.0, false, 10.0), 1e-15, L"dQP=0 SDR must be exact 1.0");
			Assert::AreEqual(2.0, ComputeCellStepScale(6.0, 500.0, false, 10.0), 1e-14, L"dQP=+6 must double step scale");
			Assert::AreEqual(4.0, ComputeCellStepScale(12.0, 500.0, false, 10.0), 1e-14, L"dQP=+12 must quadruple step scale");
			Assert::AreEqual(16.0, ComputeCellStepScale(24.0, 500.0, false, 10.0), 1e-13, L"dQP=+24 must hit 16x");
			Assert::AreEqual(0.5, ComputeCellStepScale(-6.0, 500.0, false, 10.0), 1e-15, L"dQP=-6 must halve step scale");
			Assert::AreEqual(0.25, ComputeCellStepScale(-12.0, 500.0, false, 10.0), 1e-15, L"dQP=-12 must quarter step scale");
			Assert::AreEqual(0.0625, ComputeCellStepScale(-24.0, 500.0, false, 10.0), 1e-15, L"dQP=-24 must hit 1/16");

			// Derived +/-24 QP [1/16, 16] saturation bounds
			Assert::AreEqual(kCellStepScaleMax, ComputeCellStepScale(36.0, 500.0, false, 10.0), 1e-14, L"dQP=+36 must clamp to kCellStepScaleMax=16");
			Assert::AreEqual(kCellStepScaleMin, ComputeCellStepScale(-42.0, 500.0, false, 10.0), 1e-15, L"dQP=-42 must clamp to kCellStepScaleMin=1/16");

			// Non-finite / malformed inputs fall back safely to 1.0
			const double kNaN = std::numeric_limits<double>::quiet_NaN();
			const double kInf = std::numeric_limits<double>::infinity();
			Assert::AreEqual(1.0, ComputeCellStepScale(kNaN, 512.0, false, 10.0), 0.0, L"NaN dQP must return 1.0");
			Assert::AreEqual(1.0, ComputeCellStepScale(kInf, 512.0, true, 10.0), 0.0, L"+Inf dQP must return 1.0");
			Assert::AreEqual(1.0, ComputeCellStepScale(6.0, kNaN, true, 10.0), 0.0, L"NaN PQ code must return 1.0");
			Assert::AreEqual(1.0, ComputeCellStepScale(6.0, 512.0, true, 0.0), 0.0, L"sourceBits=0 must return 1.0");
			Assert::AreEqual(1.0, ComputeCellStepScale(6.0, 512.0, true, 30.0), 0.0, L"sourceBits>24 must return 1.0");

			// 2. Independent finite-difference EOTF verification of ST.2084 PQ Weber slope S(e_203) / S(e)
			auto numericLogSlope = [](double e) -> double
			{
				const double h = 1e-6;
				const double lPlus = IndependentPqSignalToNits(e + h);
				const double lMinus = IndependentPqSignalToNits(e - h);
				return (std::log(lPlus) - std::log(lMinus)) / (2.0 * h);
			};

			const double e203 = IndependentPqNitsToSignal(203.0);
			const double s203 = numericLogSlope(e203);
			Assert::AreEqual(1.0, ComputeCellStepScale(0.0, e203 * 1023.0, true, 10.0), 1e-12,
				L"ITU-R BT.2408 diffuse white (203 nits) at 10-bit must give exact 1.0 PQ step weight");
			Assert::AreEqual(1.0, ComputeCellStepScale(0.0, e203 * 4095.0, true, 12.0), 1e-12,
				L"ITU-R BT.2408 diffuse white (203 nits) at 12-bit must give exact 1.0 PQ step weight");

			// Check interior un-clamped nits levels against independent central-difference Weber slope ratio
			const double testNits[] = { 8.0, 35.0, 100.0, 500.0, 1000.0 };
			double prevScale = 0.0;
			for (size_t i = 0; i < sizeof(testNits) / sizeof(testNits[0]); ++i)
			{
				const double e = IndependentPqNitsToSignal(testNits[i]);
				const double expectedRatio = (std::max)(kPqStepWeightMin, (std::min)(kPqStepWeightMax, s203 / numericLogSlope(e)));
				const double got = ComputeCellStepScale(0.0, e * 1023.0, true, 10.0);
				Assert::AreEqual(expectedRatio, got, 1e-5,
					FmtMsg("PQ Weber slope mismatch at %.1f nits: expected=%.6f got=%.6f", testNits[i], expectedRatio, got).c_str());
				Assert::IsTrue(got > prevScale, L"PQ step weight must increase monotonically from shadow toe to highlights");
				prevScale = got;
			}

			// Deep PQ toe / pure black clamps to kPqStepWeightMin = 0.25; 10000-nit peak matches exact ST.2084 Weber flatness (~1.00138)
			Assert::AreEqual(kPqStepWeightMin, ComputeCellStepScale(0.0, 0.0, true, 10.0), 1e-15,
				L"Pure black PQ code 0 must clamp to shadow floor 0.25");
			const double eDeepToe = IndependentPqNitsToSignal(0.01);
			Assert::AreEqual(kPqStepWeightMin, ComputeCellStepScale(0.0, eDeepToe * 1023.0, true, 10.0), 1e-12,
				L"0.01-nit PQ deep toe must clamp to shadow floor 0.25");
			const double expectedPeakWeber = s203 / numericLogSlope(1.0 - 2e-6);
			Assert::AreEqual(expectedPeakWeber, ComputeCellStepScale(0.0, 1023.0, true, 10.0), 1e-4,
				L"10000-nit PQ peak code 1023 must match exact SMPTE ST.2084 Weber slope ratio (~1.00138)");

			// High-AQ dark cell (+15 QP in 5-nit PQ shadow): AQ doubling (+15 QP -> 5.657x) overcomes PQ shadow
			// tightening (~0.35x) to yield scale > 1.8 (> 1.0), whereas naive AQ-ignorant scale stays 1.0!
			const double e5Nits = IndependentPqNitsToSignal(5.0);
			const double darkHighAqScale = ComputeCellStepScale(15.0, e5Nits * 1023.0, true, 10.0);
			Assert::IsTrue(darkHighAqScale > 1.8 && darkHighAqScale < 4.0,
				FmtMsg("High-AQ (+15 QP) 5-nit PQ dark cell scale=%.4f expected in (1.8, 4.0)", darkHighAqScale).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CpuAndGpuPerCellStepScaleUnlocksHighAqDarkCellTightensLowCellAndPreservesDefaultIdentity)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CpuAndGpuPerCellStepScaleUnlocksHighAqDarkCellTightensLowCellAndPreservesDefaultIdentity)
		{
			WarpContext ctx;

			const int w = 64;
			const int h = 64;
			const int cellPx = 16;
			const int cw = w / cellPx; // 4
			const int ch = h / cellPx; // 4
			const size_t numCells = static_cast<size_t>(cw * ch);
			const ActiveRect full = ActiveRect::Full(w, h);
			const ActiveRect highAqRect{ 0, 0, 32, 32 };   // Top-left 2x2 cells: high-AQ dark PQ region
			const ActiveRect tightRect{ 32, 32, 64, 64 };  // Bottom-right 2x2 cells: tight low-QP region

			// Construct synthetic clean master with dark PQ shadow top-left (~230 code) and brighter midtone bottom-right (~580 code)
			TdV2Plane master(w, h, 0.0);
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const bool inDark = (x < 32 && y < 32);
					const double dc = inDark ? 230.0 : 580.0;
					const double mid = 14.0 * std::sin(2.0 * kPi * (x / 9.0 + y / 11.0) + 0.4)
					                 + 11.0 * std::cos(2.0 * kPi * (x / 10.0 - y / 13.0) + 1.1);
					const double fine = 12.0 * std::sin(2.0 * kPi * (x / 4.2 + y / 5.6) + 0.3)
					                  + 9.0 * std::cos(2.0 * kPi * (x / 5.1 - y / 4.4) + 1.7);
					master.at(x, y) = dc + mid + fine;
				}
			}

			// Current B-frame suffered heavy AQ quantization loss in the dark region (85% fine+mid wiped)
			// and moderate quantization offset in the tight region, while frame-average stepDelta = 5.0 is tight!
			const ThreeBandPlanes masterBands = DecomposeActiveBands(master, full);
			TdV2Plane curClean = master;
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					if (x < 32 && y < 32)
					{
						// Heavy dark-AQ texture loss (~18-code peak error vs nominal stepDelta = 5.0)
						curClean.at(x, y) = master.at(x, y) - 0.85 * (masterBands.M.at(x, y) + masterBands.H.at(x, y));
					}
					else if (x >= 32 && y >= 32)
					{
						// Moderate distortion in tight negative-AQ region where stepScale < 1.0 must clamp strictly
						curClean.at(x, y) = master.at(x, y) - 0.65 * (masterBands.M.at(x, y) + masterBands.H.at(x, y));
					}
				}
			}

			// Sharp prevPeak & nextPeak with small independent peak rounding noise (sigma = 0.35)
			TdV2Plane prevClean = master;
			TdV2Plane nextClean = master;
			{
				std::mt19937 rngPrev(9101u);
				std::mt19937 rngNext(9102u);
				std::normal_distribution<double> nd(0.0, 0.35);
				for (size_t i = 0; i < master.v.size(); ++i)
				{
					prevClean.v[i] += nd(rngPrev);
					nextClean.v[i] += nd(rngNext);
				}
			}

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			const double darkScale = ComputeCellStepScale(18.0, 230.0, true, 10.0);  // > 2.5
			const double tightScale = ComputeCellStepScale(-9.0, 580.0, true, 10.0); // < 0.40
			Assert::IsTrue(darkScale > 2.5, FmtMsg("Expected darkScale > 2.5, got %.4f", darkScale).c_str());
			Assert::IsTrue(tightScale < 0.40, FmtMsg("Expected tightScale < 0.40, got %.4f", tightScale).c_str());

			std::vector<double> stepScales(numCells, 1.0);
			std::vector<double> unitScales(numCells, 1.0);
			for (int cy = 0; cy < ch; ++cy)
			{
				for (int cx = 0; cx < cw; ++cx)
				{
					const size_t idx = static_cast<size_t>(cy * cw + cx);
					if (cx < 2 && cy < 2)
						stepScales[idx] = darkScale;
					else if (cx >= 2 && cy >= 2)
						stepScales[idx] = tightScale;
				}
			}

			// Test BOTH transform-unknown (Pass 2B cA2 + Pass 3A pixel bound) AND 8x8 DCT CELL POCS (Pass 3B blockStepDelta)!
			for (int passMode = 0; passMode < 2; ++passMode)
			{
				const bool xform8x8 = (passMode == 1);
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;
				cfg.estimator = VarianceEstimator::A3_ConservativeMin;
				cfg.residualMode = ResidualMode::Cell;
				cfg.cellProjIterations = 2;

				FrameProcessInput baseIn;
				baseIn.seq = static_cast<uint64_t>(100 + passMode);
				baseIn.receivedY = curClean;
				baseIn.hasCleanSplit = true;
				baseIn.cleanY = curClean;
				baseIn.activeRect = full;
				baseIn.prevPeak = { &prevClean, &stillMf, false };
				baseIn.nextPeak = { &nextClean, &stillMf, false };
				baseIn.hasStepPrior = true;
				baseIn.estimatedStepDelta = 9.0; // Nominal frame step: scale==1.0 clears >1.5 RMS in tightRect while darkScale>2.5 unlocks dark AQ!
				baseIn.deadZonePhi = kDeadZonePhiInter;
				baseIn.zeroCoeffFraction = 0.35;
				baseIn.transformIdentified8x8 = xform8x8;

				FrameProcessInput unitIn = baseIn;
				unitIn.cellStepScale = unitScales;

				FrameProcessInput scaledIn = baseIn;
				scaledIn.cellStepScale = stepScales;

				TdV2ReferenceEngine cpuDef(cfg), cpuUnit(cfg), cpuScaled(cfg);
				EngineGuard gpuDef(ctx.gpu, cfg), gpuUnit(ctx.gpu, cfg), gpuScaled(ctx.gpu, cfg);

				const FrameProcessResult cResDef = cpuDef.ProcessFrame(baseIn, nullptr);
				const FrameProcessResult cResUnit = cpuUnit.ProcessFrame(unitIn, nullptr);
				const FrameProcessResult cResScaled = cpuScaled.ProcessFrame(scaledIn, nullptr);

				const GpuProcessResult gResDef = gpuDef.engine.ProcessFrame(ctx.gpu, ctx.dispatch, baseIn, nullptr, nullptr);
				const GpuProcessResult gResUnit = gpuUnit.engine.ProcessFrame(ctx.gpu, ctx.dispatch, unitIn, nullptr, nullptr);
				const GpuProcessResult gResScaled = gpuScaled.engine.ProcessFrame(ctx.gpu, ctx.dispatch, scaledIn, nullptr, nullptr);
				Assert::IsTrue(gResDef.succeeded && gResUnit.succeeded && gResScaled.succeeded, L"WARP ProcessFrame failed in SOTA-1");

				// (a) Default empty cellStepScale MUST be bit-identical to explicit all-1.0 cellStepScale on both CPU & GPU!
				Assert::AreEqual(0.0, MaxAbsDiffRect(cResDef.outputY, cResUnit.outputY, full),
					L"CPU empty cellStepScale must be bit-identical to all-1.0 cellStepScale");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gResDef.outputY, gResUnit.outputY, full),
					L"GPU empty cellStepScale must be bit-identical to all-1.0 cellStepScale");

				// (b) Strict CPU vs WARP GPU parity (<= 1e-3 of a 10-bit code) on scaled run + TdMaps::kCodingNoise scaling!
				const double cpuGpuErr = MaxAbsDiffRect(cResScaled.outputY, gResScaled.outputY, full);
				Assert::IsTrue(cpuGpuErr <= kGpuCpuTol,
					FmtMsg("SOTA-1 CPU vs GPU outputY diff=%.6f > 1e-3 (xform8x8=%.0f)", cpuGpuErr, xform8x8 ? 1.0 : 0.0).c_str());
				const float cnDef = gResDef.tdMaps[0].f[TdMapsView::kCodingNoise];
				const float cnDark = gResScaled.tdMaps[0].f[TdMapsView::kCodingNoise];
				const float cnTight = gResScaled.tdMaps[numCells - 1u].f[TdMapsView::kCodingNoise];
				Assert::AreEqual(static_cast<double>(cnDef) * darkScale, static_cast<double>(cnDark), 1e-4,
					L"TdMaps::kCodingNoise on dark cell must scale linearly by darkScale");
				Assert::AreEqual(static_cast<double>(cnDef) * tightScale, static_cast<double>(cnTight), 1e-4,
					L"TdMaps::kCodingNoise on tight cell must scale linearly by tightScale");

				// (c) High-AQ dark cell (scale > 1.0) unlocks STRICTLY larger lawful recovery than scale == 1.0 negative control!
				const double rawDarkErr = RmsDiffRect(curClean, master, highAqRect);
				const double unitDarkErr = RmsDiffRect(gResUnit.outputY, master, highAqRect);
				const double scaledDarkErr = RmsDiffRect(gResScaled.outputY, master, highAqRect);
				Assert::IsTrue(rawDarkErr > 6.0, FmtMsg("Input dark-cell error too small: %.4f", rawDarkErr).c_str());
				Assert::IsTrue(scaledDarkErr < unitDarkErr - 1.5,
					FmtMsg("High-AQ dark cell (scale=%.2f) MUST beat scale==1.0 negative control by > 1.5 RMS codes: scaledErr=%.4f vs unitErr=%.4f",
						darkScale, scaledDarkErr, unitDarkErr).c_str());

				// (d) Tight cell (scale < 1.0) tightens recovery STRICTLY vs scale == 1.0 negative control!
				const double unitTightDeltaRms = RmsDiffRect(gResUnit.outputY, curClean, tightRect);
				const double scaledTightDeltaRms = RmsDiffRect(gResScaled.outputY, curClean, tightRect);
				const double unitTightPeak = MaxAbsRect(gResUnit.tdDelta, tightRect);
				const double scaledTightPeak = MaxAbsRect(gResScaled.tdDelta, tightRect);
				Assert::IsTrue(unitTightDeltaRms > 1.5,
					FmtMsg("Negative control scale==1.0 must produce non-trivial delta in tightRect: %.4f", unitTightDeltaRms).c_str());
				Assert::IsTrue(scaledTightDeltaRms < unitTightDeltaRms * 0.65 && scaledTightPeak < unitTightPeak * 0.65,
					FmtMsg("Tight cell (scale=%.2f) MUST strictly tighten recovery vs scale==1.0: scaledRms=%.4f vs unitRms=%.4f",
						tightScale, scaledTightDeltaRms, unitTightDeltaRms).c_str());
			}

			// 3. Zero-copy ProcessFrameGpu: legacy 2-channel RG32F cellPriorTex vs explicit 4-channel RGBA32F cellPriorTex
			//    + adversarial non-positive/NaN cellPriorTex.z fail-closed NonFiniteClosure check!
			{
				const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
				const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
				const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
				Assert::IsNotNull(fmtR32);
				Assert::IsNotNull(fmtRG32);
				Assert::IsNotNull(fmtRGBA32);

				OwnedTex recTex(ctx.gpu), prevTex(ctx.gpu), nextTex(ctx.gpu);
				OwnedTex priorRg32(ctx.gpu), priorRgba32(ctx.gpu), priorBadZ(ctx.gpu);
				OwnedTex outY1(ctx.gpu), outY2(ctx.gpu), outYBad(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, prevTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, nextTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, priorRg32.tex, cw, ch, fmtRG32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, priorRgba32.tex, cw, ch, fmtRGBA32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, priorBadZ.tex, cw, ch, fmtRGBA32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outY1.tex, w, h, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outY2.tex, w, h, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYBad.tex, w, h, fmtR32, true, true, false, true));

				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, curClean));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, prevTex.tex, prevClean));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, nextTex.tex, nextClean));

				std::vector<float> rgBuf(numCells * 2u, 0.0f);
				std::vector<float> rgbaUnitBuf(numCells * 4u, 0.0f);
				std::vector<float> rgbaBadBuf(numCells * 4u, 0.0f);
				for (size_t i = 0; i < numCells; ++i)
				{
					rgbaUnitBuf[i * 4u + 2u] = 1.0f;
					rgbaBadBuf[i * 4u + 2u] = (i == 0u) ? 0.0f : 1.0f; // Cell 0 has invalid non-positive scale == 0.0!
				}
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, priorRg32.tex, rgBuf.data()));
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, priorRgba32.tex, rgbaUnitBuf.data()));
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, priorBadZ.tex, rgbaBadBuf.data()));

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;

				GpuFrameInput gIn;
				gIn.seq = 200;
				gIn.w = w;
				gIn.h = h;
				gIn.receivedYTex = recTex.tex;
				gIn.activeRect = full;
				gIn.prevPeak = { prevTex.tex, &stillMf, false };
				gIn.nextPeak = { nextTex.tex, &stillMf, false };
				gIn.hasStepPrior = true;
				gIn.estimatedStepDelta = 5.0;
				gIn.deadZonePhi = kDeadZonePhiInter;
				gIn.zeroCoeffFraction = 0.35;

				EngineGuard g1(ctx.gpu, cfg), g2(ctx.gpu, cfg), gBad(ctx.gpu, cfg);
				GpuFrameInput inRg = gIn;
				inRg.cellPriorTex = priorRg32.tex;
				GpuFrameOutput outRg;
				outRg.outputYTex = outY1.tex;

				GpuFrameInput inRgba = gIn;
				inRgba.cellPriorTex = priorRgba32.tex;
				GpuFrameOutput outRgba;
				outRgba.outputYTex = outY2.tex;

				Assert::IsTrue(g1.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, inRg, outRg, nullptr, nullptr));
				Assert::IsTrue(g2.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, inRgba, outRgba, nullptr, nullptr));

				TdV2Plane pRg(w, h, 0.0), pRgba(w, h, 0.0);
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outY1.tex, w, h, pRg));
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outY2.tex, w, h, pRgba));
				Assert::AreEqual(0.0, MaxAbsDiffRect(pRg, pRgba, full),
					L"Legacy 2-channel RG32F cellPriorTex must be bit-identical to RGBA32F cellPriorTex with .z == 1.0");

				// Adversarial invalid .z == 0.0 on Cell 0: closes Cell 0 to exact passthrough & reports nonFiniteCells == 1
				GpuFrameInput inBad = gIn;
				inBad.cellPriorTex = priorBadZ.tex;
				GpuFrameOutput outBad;
				outBad.outputYTex = outYBad.tex;
				outBad.readbackCellDiagnostics = true;
				StandInRegistry siBad;
				Assert::IsTrue(gBad.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, inBad, outBad, nullptr, &siBad));
				Assert::AreEqual(static_cast<size_t>(1u), outBad.nonFiniteCells,
					L"Non-positive cellPriorTex.z == 0.0 on Cell 0 must count nonFiniteCells == 1");
				Assert::IsTrue(siBad.IsActive(StandInKind::NonFiniteClosure),
					L"Non-positive cellPriorTex.z == 0.0 must engage StandInKind::NonFiniteClosure visibly");
				TdV2Plane pBad(w, h, 0.0);
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYBad.tex, w, h, pBad));
				Assert::AreEqual(0.0, MaxAbsDiffRect(pBad, curClean, ActiveRect{ 0, 0, 16, 16 }),
					L"Invalid cellPriorTex.z == 0.0 cell must close cleanly to bit-exact passthrough");
			}
		}
	};
} // namespace TdV2Sota1StepScaleNs
} // namespace Tests
