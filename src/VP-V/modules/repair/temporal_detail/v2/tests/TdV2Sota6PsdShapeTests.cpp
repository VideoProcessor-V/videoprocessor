// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 6 (TD-SOTA-6) Adversarial Red-Team Tests:
// P10g PsdShape (bandShareLow / bandShareMid / bandShareFine) Wired into EstimateC_A1 psdScale:
//   - Synthesizes spatially correlated clumpy AR(2) / AV1-AFGS1-like colored film grain whose mid-band
//     PSD share is > 2.2x white noise (bandShareMid != 1.0, bandShareFine != 1.0), derived directly
//     from empirical band power shares (Rule 1: zero invented constants!).
//   - Exercises ALL 4 engines on identical clumpy-grain inputs:
//       1) CPU luma (TdV2ReferenceEngine)
//       2) Real D3D11 WARP GPU luma (TdV2GpuEngine)
//       3) CPU chroma (TdV2ChromaEngine)
//       4) Real D3D11 WARP GPU chroma (TdV2ChromaGpuEngine)
//   - Proves wired PsdShape prevents upward-biased EstimateC_A1 false M-band grain stripping vs naive
//     white-noise PsdShape::White() (1.0, 1.0, 1.0) negative control on the exact same input!
//   - Proves invalid/negative/non-finite PsdShape fails closed visibly with StandInKind::NonFiniteClosure!
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ChromaGpu.h>
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
namespace TdV2Sota6PsdShapeNs
{
	using namespace TdV2;
	using namespace TdV2::Gpu;

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

	struct LumaGpuGuard
	{
		pl_gpu gpu = nullptr;
		TdV2GpuEngine engine;
		explicit LumaGpuGuard(pl_gpu g, const Config& cfg) : gpu(g), engine(cfg) {}
		~LumaGpuGuard() { engine.Release(gpu); }
		LumaGpuGuard(const LumaGpuGuard&) = delete;
		LumaGpuGuard& operator=(const LumaGpuGuard&) = delete;
	};

	struct ChromaGpuGuard
	{
		pl_gpu gpu = nullptr;
		TdV2ChromaGpuEngine engine;
		explicit ChromaGpuGuard(pl_gpu g, const Config& cfg) : gpu(g), engine(cfg) {}
		~ChromaGpuGuard() { engine.Release(gpu); }
		ChromaGpuGuard(const ChromaGpuGuard&) = delete;
		ChromaGpuGuard& operator=(const ChromaGpuGuard&) = delete;
	};

	static std::wstring FmtMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0)
	{
		char buf[340];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	// Generate clumpy colored AR(2)/AFGS1-like grain concentrated in the M band by taking the M-band of
	// independent Gaussian noise + small H residual, and measure its exact empirical PsdShape vs white noise!
	static TdV2Plane MakeClumpyColoredGrain(int w, int h, uint32_t seed, double targetStd, PsdShape& outMeasuredPsd, double& outTotalVar)
	{
		TdV2Plane raw(w, h, 0.0);
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		for (double& v : raw.v)
			v = nd(rng);

		const ActiveRect full = ActiveRect::Full(w, h);
		const ThreeBandPlanes rb = DecomposeActiveBands(raw, full);
		TdV2Plane clumpy(w, h, 0.0);
		double mean = 0.0;
		for (size_t i = 0; i < clumpy.v.size(); ++i)
		{
			// Dominantly clumpy mid-scale grain (AR(2) clump peak around sigma=1.5 px) + minor fine tail
			clumpy.v[i] = 2.8 * rb.M.v[i] + 0.35 * rb.H.v[i];
			mean += clumpy.v[i];
		}
		mean /= static_cast<double>(clumpy.v.size());
		double m2 = 0.0;
		for (double& v : clumpy.v)
		{
			v -= mean;
			m2 += v * v;
		}
		const double curStd = std::sqrt(m2 / static_cast<double>(clumpy.v.size()));
		const double scale = (curStd > 1e-9) ? (targetStd / curStd) : 0.0;
		for (double& v : clumpy.v)
			v *= scale;

		const ActiveRect interior{ 6, 6, w - 6, h - 6 };
		const ThreeBandPlanes cb = DecomposeActiveBands(clumpy, full);
		double totE = 0.0, lE = 0.0, mE = 0.0, hE = 0.0;
		size_t cnt = 0;
		for (int y = interior.y0; y < interior.y1; ++y)
		{
			for (int x = interior.x0; x < interior.x1; ++x)
			{
				const double g = clumpy.at(x, y);
				totE += g * g;
				lE += cb.L.at(x, y) * cb.L.at(x, y);
				mE += cb.M.at(x, y) * cb.M.at(x, y);
				hE += cb.H.at(x, y) * cb.H.at(x, y);
				++cnt;
			}
		}
		outTotalVar = totE / static_cast<double>(cnt);
		const BandWhiteNoiseGains wg = ComputeBandWhiteNoiseGains();
		outMeasuredPsd.bandShareLow  = (lE / static_cast<double>(cnt)) / (outTotalVar * wg.low);
		outMeasuredPsd.bandShareMid  = (mE / static_cast<double>(cnt)) / (outTotalVar * wg.mid);
		outMeasuredPsd.bandShareFine = (hE / static_cast<double>(cnt)) / (outTotalVar * wg.fine);
		return clumpy;
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

	TEST_CLASS(TdV2Sota6PsdShapeTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(CpuAndGpuPsdShapePreventsBiasedEstimateCA1GrainTransferOnClumpyAr2ColoredGrain)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CpuAndGpuPsdShapePreventsBiasedEstimateCA1GrainTransferOnClumpyAr2ColoredGrain)
		{
			WarpContext ctx;

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const size_t numCells = static_cast<size_t>(cw * ch);
			const ActiveRect full = ActiveRect::Full(w, h);
			const ActiveRect interior{ 8, 8, 56, 56 };

			PsdShape clumpyPsd{};
			double leakedGrainVar = 0.0;
			const TdV2Plane leakedClumpyGrain = MakeClumpyColoredGrain(w, h, 6601u, 6.0, clumpyPsd, leakedGrainVar);
			const TdV2Plane sepOwnGrain = [&]()
			{
				PsdShape dummy{};
				double dummyVar = 0.0;
				return MakeClumpyColoredGrain(w, h, 6602u, 3.0, dummy, dummyVar);
			}();

			// Verify clumpy grain genuinely departs strongly from white noise (bandShareMid > 2.2, bandShareFine < 0.55)!
			Assert::IsTrue(clumpyPsd.IsValid());
			Assert::IsTrue(clumpyPsd.bandShareMid > 2.2 && clumpyPsd.bandShareFine < 0.55,
				FmtMsg("Clumpy AR(2) grain PSD shares not strongly colored: mid=%.4f fine=%.4f",
					clumpyPsd.bandShareMid, clumpyPsd.bandShareFine).c_str());

			// Scene has ZERO true compression loss (masterClean == 500.0 everywhere), ONLY clumpy film grain
			// partially leaked into curClean (hasCleanSplit=true, cellOwnGrainVar < retainedGrainVar so Layer-3
			// alphaLeak == 0 and EstimateC_A1's grain subtraction governs M-band grain preservation directly!).
			const TdV2Plane masterClean(w, h, 500.0);
			TdV2Plane curClean(w, h, 500.0);
			TdV2Plane received(w, h, 500.0);
			for (size_t i = 0; i < masterClean.v.size(); ++i)
			{
				curClean.v[i] = masterClean.v[i] + leakedClumpyGrain.v[i];
				received.v[i] = curClean.v[i] + sepOwnGrain.v[i]; // Total true grain = leaked + sepOwn
			}
			// Grain-free peak references with tiny independent rounding noise (sigma = 0.25)
			TdV2Plane prevClean = masterClean, nextClean = masterClean;
			{
				std::mt19937 r1(6603u), r2(6604u);
				std::normal_distribution<double> nd(0.0, 0.25);
				for (size_t i = 0; i < masterClean.v.size(); ++i)
				{
					prevClean.v[i] += nd(r1);
					nextClean.v[i] += nd(r2);
				}
			}

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			const std::vector<double> retGVarVec(numCells, leakedGrainVar);

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;
			cfg.enableCarryForward = false;
			cfg.estimator = VarianceEstimator::A3_ConservativeMin;

			// =========================================================================
			// Part 1: Luma CPU (TdV2ReferenceEngine) + Real D3D11 WARP GPU (TdV2GpuEngine)
			// =========================================================================
			{
				FrameProcessInput whiteIn;
				whiteIn.seq = 601;
				whiteIn.receivedY = received;
				whiteIn.hasCleanSplit = true;
				whiteIn.cleanY = curClean;
				whiteIn.activeRect = full;
				whiteIn.prevPeak = { &prevClean, &stillMf, false };
				whiteIn.nextPeak = { &nextClean, &stillMf, false };
				whiteIn.retainedGrainVar = retGVarVec;
				whiteIn.hasStepPrior = true;
				whiteIn.estimatedStepDelta = 36.0;
				whiteIn.deadZonePhi = kDeadZonePhiInter;
				whiteIn.zeroCoeffFraction = 0.35;
				whiteIn.psdShape = PsdShape::White(); // Naive (1.0, 1.0, 1.0) negative control!

				FrameProcessInput psdIn = whiteIn;
				psdIn.seq = 602;
				psdIn.psdShape = clumpyPsd;

				TdV2ReferenceEngine cpuWhite(cfg), cpuPsd(cfg);
				LumaGpuGuard gpuWhite(ctx.gpu, cfg), gpuPsd(ctx.gpu, cfg);

				const FrameProcessResult cWhite = cpuWhite.ProcessFrame(whiteIn, nullptr);
				const FrameProcessResult cPsd   = cpuPsd.ProcessFrame(psdIn, nullptr);
				const GpuProcessResult gWhite   = gpuWhite.engine.ProcessFrame(ctx.gpu, ctx.dispatch, whiteIn, nullptr, nullptr);
				const GpuProcessResult gPsd     = gpuPsd.engine.ProcessFrame(ctx.gpu, ctx.dispatch, psdIn, nullptr, nullptr);
				Assert::IsTrue(gWhite.succeeded && gPsd.succeeded, L"WARP luma ProcessFrame failed in SOTA-6");

				Assert::IsTrue(MaxAbsDiffRect(cWhite.outputY, gWhite.outputY, full) <= kGpuCpuTol, L"Luma white PsdShape CPU/GPU mismatch");
				Assert::IsTrue(MaxAbsDiffRect(cPsd.outputY, gPsd.outputY, full) <= kGpuCpuTol, L"Luma wired PsdShape CPU/GPU mismatch");

				// Mandatory Same-Input Negative Control:
				// Naive white-noise PsdShape (1.0, 1.0, 1.0) under-subtracts M-band clumpy grain variance by >2.2x,
				// causing upward-biased EstimateC_A1 > 0 that falsely strips > 2.2 RMS codes of clumpy film grain!
				const double whiteStrippedGrainRms = RmsDiffRect(gWhite.outputY, received, interior);
				const double psdStrippedGrainRms   = RmsDiffRect(gPsd.outputY,   received, interior);
				Assert::IsTrue(whiteStrippedGrainRms > 2.2,
					FmtMsg("Negative control failed: white-noise PsdShape did not strip clumpy M-band grain (rms=%.4f)",
						whiteStrippedGrainRms).c_str());
				Assert::IsTrue(psdStrippedGrainRms < whiteStrippedGrainRms * 0.30,
					FmtMsg("SOTA-6 wired PsdShape on luma MUST prevent false clumpy grain stripping: psdRms=%.4f vs whiteRms=%.4f",
						psdStrippedGrainRms, whiteStrippedGrainRms).c_str());

				// Adversarial invalid PsdShape (negative & NaN) -> fails closed visibly with NonFiniteClosure!
				FrameProcessInput badPsdIn = psdIn;
				badPsdIn.psdShape.bandShareMid = -0.25;
				StandInRegistry siCpuBad, siGpuBad;
				const FrameProcessResult cBad = cpuPsd.ProcessFrame(badPsdIn, &siCpuBad);
				const GpuProcessResult gBad = gpuPsd.engine.ProcessFrame(ctx.gpu, ctx.dispatch, badPsdIn, nullptr, &siGpuBad);
				Assert::IsFalse(gBad.succeeded);
				Assert::AreEqual(numCells, cBad.nonFiniteCells);
				Assert::AreEqual(numCells, gBad.nonFiniteCells);
				Assert::IsTrue(siCpuBad.IsActive(StandInKind::NonFiniteClosure) && siGpuBad.IsActive(StandInKind::NonFiniteClosure),
					L"Negative PsdShape must engage StandInKind::NonFiniteClosure on both CPU & GPU luma");
			}

			// =========================================================================
			// Part 2: Chroma CPU (TdV2ChromaEngine) + Real D3D11 WARP GPU (TdV2ChromaGpuEngine)
			// =========================================================================
			{
				// Use 4:4:4 coded=delivered (64x64) so exact same clumpy grain planes & cell grid apply directly
				ChromaProcessInput cWhiteIn;
				cWhiteIn.seq = 611;
				cWhiteIn.lumaW = w;
				cWhiteIn.lumaH = h;
				cWhiteIn.gridStatus = ChromaGridStatus::Identified;
				cWhiteIn.codedSubsampling = ChromaSubsampling::Yuv444;
				cWhiteIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
				cWhiteIn.siting = SitingSpec::Center();
				cWhiteIn.activeRect = full;
				cWhiteIn.receivedCb = received;
				cWhiteIn.receivedCr = received;
				cWhiteIn.hasCleanSplit = true;
				cWhiteIn.cleanCb = curClean;
				cWhiteIn.cleanCr = curClean;
				cWhiteIn.prevPeak.cleanCb = &prevClean;
				cWhiteIn.prevPeak.cleanCr = &prevClean;
				cWhiteIn.prevPeak.lumaMotion = &stillMf;
				cWhiteIn.nextPeak.cleanCb = &nextClean;
				cWhiteIn.nextPeak.cleanCr = &nextClean;
				cWhiteIn.nextPeak.lumaMotion = &stillMf;
				cWhiteIn.retainedGrainVarCb = retGVarVec;
				cWhiteIn.retainedGrainVarCr = retGVarVec;
				cWhiteIn.hasStepPrior = true;
				cWhiteIn.estimatedStepDelta = 36.0;
				cWhiteIn.deadZonePhi = kDeadZonePhiInter;
				cWhiteIn.zeroCoeffFraction = 0.35;
				cWhiteIn.psdShape = PsdShape::White();

				ChromaProcessInput cPsdIn = cWhiteIn;
				cPsdIn.seq = 612;
				cPsdIn.psdShape = clumpyPsd;

				TdV2ChromaEngine chCpuWhite(cfg), chCpuPsd(cfg);
				ChromaGpuGuard chGpuWhite(ctx.gpu, cfg), chGpuPsd(ctx.gpu, cfg);

				const ChromaProcessResult crCpuWhite = chCpuWhite.ProcessFrame(cWhiteIn, nullptr);
				const ChromaProcessResult crCpuPsd   = chCpuPsd.ProcessFrame(cPsdIn, nullptr);
				const ChromaGpuProcessResult crGpuWhite = chGpuWhite.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cWhiteIn, nullptr, nullptr);
				const ChromaGpuProcessResult crGpuPsd   = chGpuPsd.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cPsdIn, nullptr, nullptr);
				Assert::IsTrue(crGpuWhite.succeeded && crGpuPsd.succeeded, L"WARP chroma ProcessChromaPlanes failed in SOTA-6");

				Assert::IsTrue(MaxAbsDiffRect(crCpuWhite.outputCb, crGpuWhite.outputCb, full) <= 1e-2, L"Chroma white PsdShape CPU/GPU mismatch");
				Assert::IsTrue(MaxAbsDiffRect(crCpuPsd.outputCb, crGpuPsd.outputCb, full) <= 1e-2, L"Chroma wired PsdShape CPU/GPU mismatch");

				const double chWhiteStripCb = RmsDiffRect(crGpuWhite.outputCb, received, interior);
				const double chPsdStripCb   = RmsDiffRect(crGpuPsd.outputCb,   received, interior);
				const double chWhiteStripCr = RmsDiffRect(crGpuWhite.outputCr, received, interior);
				const double chPsdStripCr   = RmsDiffRect(crGpuPsd.outputCr,   received, interior);
				Assert::IsTrue(chWhiteStripCb > 2.2 && chWhiteStripCr > 2.2,
					FmtMsg("Chroma negative control failed: white PsdShape did not strip clumpy chroma grain (cb=%.4f)",
						chWhiteStripCb).c_str());
				Assert::IsTrue(chPsdStripCb < chWhiteStripCb * 0.30 && chPsdStripCr < chWhiteStripCr * 0.30,
					FmtMsg("SOTA-6 wired PsdShape on chroma MUST prevent false clumpy grain stripping: psdCb=%.4f vs whiteCb=%.4f",
						chPsdStripCb, chWhiteStripCb).c_str());

				// Adversarial NaN PsdShape on chroma CPU & GPU -> NonFiniteClosure!
				ChromaProcessInput badChPsd = cPsdIn;
				badChPsd.psdShape.bandShareFine = std::numeric_limits<double>::quiet_NaN();
				StandInRegistry siChCpuBad, siChGpuBad;
				const ChromaProcessResult crBadCpu = chCpuPsd.ProcessFrame(badChPsd, &siChCpuBad);
				const ChromaGpuProcessResult crBadGpu = chGpuPsd.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, badChPsd, nullptr, &siChGpuBad);
				Assert::IsFalse(crBadGpu.succeeded);
				Assert::AreEqual(numCells, crBadCpu.nonFiniteCells);
				Assert::IsTrue(siChCpuBad.IsActive(StandInKind::NonFiniteClosure) && siChGpuBad.IsActive(StandInKind::NonFiniteClosure),
					L"NaN PsdShape must engage StandInKind::NonFiniteClosure on both CPU & GPU chroma");
			}
		}
	};
} // namespace TdV2Sota6PsdShapeNs
} // namespace Tests
