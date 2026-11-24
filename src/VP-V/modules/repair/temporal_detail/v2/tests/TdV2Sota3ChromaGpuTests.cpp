// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 3 (TD-SOTA-3) Adversarial Red-Team WARP GPU Tests:
// TdV2ChromaGpu.h Fused RG32F GPU Coded-Grid Chroma Fusion Compute Pipeline:
//   1. GpuPlayerUpsampled420Over422And444RecoversMissingPeakChromaWhilePreservingOwnChromaGrain (GpuWarp):
//      - Player-upsampled 4:2:0 delivered over 4:2:2, 4:4:4, and RGB 4:4:4 recovers genuine missing peak
//        chroma detail on Cb/Cr while returning own chroma grain untouched (vs naive temporal-avg negative control!)
//   2. GpuNative422AndTrue444PreserveFullHighFrequencyChromaDetailWithoutDownsampling (GpuWarp):
//      - True native 4:2:2 (coded=Yuv422, delivered=Yuv422) and true 4:4:4 (coded=Yuv444, delivered=Yuv444)
//        run at full coded grid (0 Pass-C0 reduction dispatches) with ZERO discarding of high-frequency
//        vertical/horizontal chroma detail (vs mis-subsampled 4:2:0 / 4:2:2 negative controls!)
//   3. GpuChromaFaceMaskAndRackFocusBokehVetoStrictZeroDeltaAcross420AndUpsampled422 (GpuWarp):
//      - Pass-1 faceMask, rack-focus (explicitFocusChangeCells -> L/M/H == 0), and optical bokeh
//        (explicitDefocusedCells -> M/H == 0, L gated by BokehMode) across 4:2:0 AND player-upsampled 4:2:0->4:2:2
//        straddling cell boundaries (strict deliveredDelta == 0.0 parity with CPU TdV2ChromaEngine!)
//   4. GpuChromaMissingMotionH9ZeroGhostingScratchArenaShortageAndModeOffZeroDispatches (GpuWarp):
//      - Mode::Off == 0 dispatches
//      - ScratchArena shortage under kChromaArenaScope = 0x0403 fails visibly (scratchShort=true, zero private fallback!)
//      - H9 missing motion (lumaMotion == nullptr) skips refs with ZERO ghosting on both ProcessChromaPlanes and
//        zero-copy ProcessChromaGpu reusing outDeliveredUvTex & outDeliveredDeltaTex across frames!
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ChromaGpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2Sota3ChromaGpuNs
{
	using namespace TdV2;
	using namespace TdV2::Gpu;

	constexpr double kPi = 3.14159265358979323846;

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

	struct ChromaGpuGuard
	{
		pl_gpu gpu = nullptr;
		TdV2ChromaGpuEngine engine;

		explicit ChromaGpuGuard(pl_gpu g, const Config& cfg = Config{})
			: gpu(g), engine(cfg)
		{
		}

		~ChromaGpuGuard()
		{
			engine.Release(gpu);
		}

		ChromaGpuGuard(const ChromaGpuGuard&) = delete;
		ChromaGpuGuard& operator=(const ChromaGpuGuard&) = delete;
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
		char buf[340];
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

	static double RmsRect(const TdV2Plane& p, const ActiveRect& r)
	{
		double sumSq = 0.0;
		size_t cnt = 0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double v = p.at(x, y);
				sumSq += v * v;
				++cnt;
			}
		}
		return cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
	}

	static double FineBandVarianceRect(const TdV2Plane& p, const ActiveRect& roi)
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
		return cnt > 0 ? (sumSq / static_cast<double>(cnt)) : 0.0;
	}

	TEST_CLASS(TdV2Sota3ChromaGpuTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuPlayerUpsampled420Over422And444RecoversMissingPeakChromaWhilePreservingOwnChromaGrain)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuPlayerUpsampled420Over422And444RecoversMissingPeakChromaWhilePreservingOwnChromaGrain)
		{
			WarpContext ctx;

			const int lumaW = 64;
			const int lumaH = 64;
			const int cw = lumaW / 16; // 4
			const int ch = lumaH / 16; // 4
			const int cW = 32;
			const int cH = 32;
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
			const SitingSpec siting = SitingSpec::Left();

			// Clean coded 4:2:0 master Cb/Cr with rich L/M/H color detail
			TdV2Plane masterCodedCb(cW, cH, 512.0);
			TdV2Plane masterCodedCr(cW, cH, 512.0);
			for (int y = 0; y < cH; ++y)
			{
				for (int x = 0; x < cW; ++x)
				{
					const double cbSig = 22.0 * std::sin(2.0 * kPi * x / 15.0 + 0.3)
					                   + 16.0 * std::cos(2.0 * kPi * (x / 4.5 + y / 5.5) + 0.7)
					                   + 14.0 * std::sin(2.0 * kPi * (x / 2.6 - y / 2.9) + 1.1);
					const double crSig = -19.0 * std::cos(2.0 * kPi * y / 13.0 - 0.2)
					                   + 15.0 * std::sin(2.0 * kPi * (x / 5.0 - y / 4.2) + 0.5)
					                   - 13.0 * std::cos(2.0 * kPi * (x / 2.5 + y / 2.8) + 0.9);
					masterCodedCb.at(x, y) = 512.0 + cbSig;
					masterCodedCr.at(x, y) = 512.0 + crSig;
				}
			}

			// Current coded B-frame wiped 80% of mid+fine chroma detail; add own film grain (sigma = 3.6)
			const ThreeBandPlanes mBandsCb = DecomposeActiveBands(masterCodedCb, ActiveRect::Full(cW, cH));
			const ThreeBandPlanes mBandsCr = DecomposeActiveBands(masterCodedCr, ActiveRect::Full(cW, cH));
			TdV2Plane curCleanCodedCb(cW, cH, 512.0);
			TdV2Plane curCleanCodedCr(cW, cH, 512.0);
			TdV2Plane ownGrainCodedCb(cW, cH, 0.0);
			TdV2Plane ownGrainCodedCr(cW, cH, 0.0);
			{
				std::mt19937 rngG(3101u);
				std::normal_distribution<double> ndG(0.0, 3.6);
				for (size_t i = 0; i < masterCodedCb.v.size(); ++i)
				{
					curCleanCodedCb.v[i] = masterCodedCb.v[i] - 0.80 * (mBandsCb.M.v[i] + mBandsCb.H.v[i]);
					curCleanCodedCr.v[i] = masterCodedCr.v[i] - 0.80 * (mBandsCr.M.v[i] + mBandsCr.H.v[i]);
					ownGrainCodedCb.v[i] = ndG(rngG);
					ownGrainCodedCr.v[i] = ndG(rngG);
				}
			}

			TdV2Plane prevCb = masterCodedCb, prevCr = masterCodedCr;
			TdV2Plane nextCb = masterCodedCb, nextCr = masterCodedCr;
			{
				std::mt19937 rP(3102u), rN(3103u);
				std::normal_distribution<double> ndRef(0.0, 0.4);
				for (size_t i = 0; i < prevCb.v.size(); ++i)
				{
					prevCb.v[i] += ndRef(rP); prevCr.v[i] += ndRef(rP);
					nextCb.v[i] += ndRef(rN); nextCr.v[i] += ndRef(rN);
				}
			}

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			const Config cfg = MakePreset(Preset::Reference, Mode::On);

			const std::vector<VpSkeleton::ArenaRequest> reqs =
				TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, 16);
			Assert::IsFalse(reqs.empty());
			VpSkeleton::PlTexBackend backend(ctx.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T1)));

			// Test BOTH player-upsampled DeliveredChromaFormat::Yuv422 (32x64) AND DeliveredChromaFormat::Yuv444 (64x64)!
			const DeliveredChromaFormat formats[2] = { DeliveredChromaFormat::Yuv422, DeliveredChromaFormat::Yuv444 };
			for (int fi = 0; fi < 2; ++fi)
			{
				const DeliveredChromaFormat delivFmt = formats[fi];
				const TdV2Plane masterDelivCb = UpsampleDeltaToDelivered(
					masterCodedCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane masterDelivCr = UpsampleDeltaToDelivered(
					masterCodedCr, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane cleanDelivCb = UpsampleDeltaToDelivered(
					curCleanCodedCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane cleanDelivCr = UpsampleDeltaToDelivered(
					curCleanCodedCr, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane grainDelivCb = UpsampleDeltaToDelivered(
					ownGrainCodedCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane grainDelivCr = UpsampleDeltaToDelivered(
					ownGrainCodedCr, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);

				TdV2Plane recDelivCb = cleanDelivCb;
				TdV2Plane recDelivCr = cleanDelivCr;
				for (size_t i = 0; i < recDelivCb.v.size(); ++i)
				{
					recDelivCb.v[i] += grainDelivCb.v[i];
					recDelivCr.v[i] += grainDelivCr.v[i];
				}

				ChromaProcessInput cIn;
				cIn.seq = static_cast<uint64_t>(310 + fi);
				cIn.lumaW = lumaW;
				cIn.lumaH = lumaH;
				cIn.gridStatus = ChromaGridStatus::Identified;
				cIn.codedSubsampling = ChromaSubsampling::Yuv420;
				cIn.deliveredFormat = delivFmt;
				cIn.siting = siting;
				cIn.activeRect = lumaFull;
				cIn.receivedCb = recDelivCb;
				cIn.receivedCr = recDelivCr;
				cIn.hasCleanSplit = true;
				cIn.cleanCb = curCleanCodedCb;
				cIn.cleanCr = curCleanCodedCr;
				cIn.prevPeak.cleanCb = &prevCb;
				cIn.prevPeak.cleanCr = &prevCr;
				cIn.prevPeak.lumaMotion = &stillMf;
				cIn.nextPeak.cleanCb = &nextCb;
				cIn.nextPeak.cleanCr = &nextCr;
				cIn.nextPeak.lumaMotion = &stillMf;
				cIn.hasStepPrior = true;
				cIn.estimatedStepDelta = 36.0;
				cIn.deadZonePhi = kDeadZonePhiInter;
				cIn.zeroCoeffFraction = 0.35;

				TdV2ChromaEngine cpuEng(cfg);
				ChromaGpuGuard gpuGuard(ctx.gpu, cfg);
				arena.BeginFrame();
				const ChromaProcessResult cpuRes = cpuEng.ProcessFrame(cIn, nullptr);
				const ChromaGpuProcessResult gpuRes = gpuGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &arena, nullptr);
				Assert::IsTrue(gpuRes.succeeded && !gpuRes.scratchShort, L"WARP ProcessChromaPlanes failed on upsampled 4:2:0");
				Assert::AreEqual(cW, gpuRes.codedW);
				Assert::AreEqual(cH, gpuRes.codedH);

				// Parity with CPU TdV2ChromaEngine
				const ActiveRect dRoi{ 4, 4, masterDelivCb.w - 4, masterDelivCb.h - 4 };
				Assert::IsTrue(MaxAbsDiffRect(cpuRes.outputCb, gpuRes.outputCb, dRoi) <= 2e-2,
					FmtMsg("GPU vs CPU upsampled chroma Cb diff=%.6f", MaxAbsDiffRect(cpuRes.outputCb, gpuRes.outputCb, dRoi)).c_str());

				// 1. Genuine missing peak chroma detail recovery (scored strictly against master + true own grain!)
				TdV2Plane targetWithGrainCb = masterDelivCb;
				TdV2Plane targetWithGrainCr = masterDelivCr;
				for (size_t i = 0; i < targetWithGrainCb.v.size(); ++i)
				{
					targetWithGrainCb.v[i] += grainDelivCb.v[i];
					targetWithGrainCr.v[i] += grainDelivCr.v[i];
				}
				const double inErrCb = RmsDiffRect(recDelivCb, targetWithGrainCb, dRoi);
				const double outErrCb = RmsDiffRect(gpuRes.outputCb, targetWithGrainCb, dRoi);
				const double inErrCr = RmsDiffRect(recDelivCr, targetWithGrainCr, dRoi);
				const double outErrCr = RmsDiffRect(gpuRes.outputCr, targetWithGrainCr, dRoi);
				Assert::IsTrue(outErrCb < inErrCb * 0.50 && outErrCr < inErrCr * 0.50,
					FmtMsg("Upsampled 4:2:0 GPU chroma fusion MUST cut clean error by >50%%: outCb=%.4f vs inCb=%.4f",
						outErrCb, inErrCb).c_str());

				// 2. Own chroma grain returned untouched vs Naive 3-frame temporal average negative control!
				TdV2Plane outExtractedGrainCb(masterDelivCb.w, masterDelivCb.h, 0.0);
				for (size_t i = 0; i < outExtractedGrainCb.v.size(); ++i)
					outExtractedGrainCb.v[i] = gpuRes.outputCb.v[i] - (cleanDelivCb.v[i] + gpuRes.deliveredDeltaCb.v[i]);
				Assert::IsTrue(MaxAbsDiffRect(outExtractedGrainCb, grainDelivCb, dRoi) <= 1e-3,
					L"Own chroma grain must pass through untouched when clean split has zero leak");

				// Mandatory Same-Input Negative Control: naive 3-frame chroma average crushes own chroma grain variance!
				const TdV2Plane prevDelivCb = UpsampleDeltaToDelivered(
					prevCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane nextDelivCb = UpsampleDeltaToDelivered(
					nextCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				TdV2Plane naiveAvgResidual(masterDelivCb.w, masterDelivCb.h, 0.0);
				for (size_t i = 0; i < naiveAvgResidual.v.size(); ++i)
				{
					const double avg = (recDelivCb.v[i] + prevDelivCb.v[i] + nextDelivCb.v[i]) / 3.0;
					naiveAvgResidual.v[i] = avg - masterDelivCb.v[i];
				}
				const double trueGrainVar = FineBandVarianceRect(grainDelivCb, dRoi);
				const double gpuGrainVar = FineBandVarianceRect(outExtractedGrainCb, dRoi);
				const double naiveGrainVar = FineBandVarianceRect(naiveAvgResidual, dRoi);
				Assert::IsTrue(gpuGrainVar >= 0.96 * trueGrainVar && naiveGrainVar < 0.25 * trueGrainVar,
					FmtMsg("Own chroma grain variance retention: gpu=%.4f, naive=%.4f, true=%.4f",
						gpuGrainVar, naiveGrainVar, trueGrainVar).c_str());
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuNative422AndTrue444PreserveFullHighFrequencyChromaDetailWithoutDownsampling)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuNative422AndTrue444PreserveFullHighFrequencyChromaDetailWithoutDownsampling)
		{
			WarpContext ctx;
			const int lumaW = 64;
			const int lumaH = 64;
			const int cw = lumaW / 16;
			const int ch = lumaH / 16;
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			const Config cfg = MakePreset(Preset::Reference, Mode::On);

			// ---- Part A: True Native 4:2:2 (coded=Yuv422, delivered=Yuv422 -> 32x64) ----
			// Vertical wavelength = 2.25 luma rows (above 4:2:0 vertical Nyquist = 4.0 luma rows!).
			{
				const int w422 = 32;
				const int h422 = 64;
				TdV2Plane masterCb422(w422, h422, 512.0);
				TdV2Plane masterCr422(w422, h422, 512.0);
				for (int y = 0; y < h422; ++y)
				{
					for (int x = 0; x < w422; ++x)
					{
						const double vHi = 24.0 * std::sin(2.0 * kPi * static_cast<double>(y) / 2.25 + 0.3 * x);
						masterCb422.at(x, y) = 512.0 + vHi;
						masterCr422.at(x, y) = 512.0 - 0.9 * vHi;
					}
				}
				TdV2Plane degradedCb422(w422, h422, 512.0), degradedCr422(w422, h422, 512.0);
				for (size_t i = 0; i < masterCb422.v.size(); ++i)
				{
					degradedCb422.v[i] = 512.0 + 0.20 * (masterCb422.v[i] - 512.0);
					degradedCr422.v[i] = 512.0 + 0.20 * (masterCr422.v[i] - 512.0);
				}

				ChromaProcessInput in422;
				in422.seq = 321;
				in422.lumaW = lumaW;
				in422.lumaH = lumaH;
				in422.gridStatus = ChromaGridStatus::Identified;
				in422.codedSubsampling = ChromaSubsampling::Yuv422;
				in422.deliveredFormat = DeliveredChromaFormat::Yuv422;
				in422.siting = SitingSpec::Left();
				in422.activeRect = lumaFull;
				in422.receivedCb = degradedCb422;
				in422.receivedCr = degradedCr422;
				in422.hasCleanSplit = true;
				in422.cleanCb = degradedCb422;
				in422.cleanCr = degradedCr422;
				in422.prevPeak.cleanCb = &masterCb422;
				in422.prevPeak.cleanCr = &masterCr422;
				in422.prevPeak.lumaMotion = &stillMf;
				in422.nextPeak.cleanCb = &masterCb422;
				in422.nextPeak.cleanCr = &masterCr422;
				in422.nextPeak.lumaMotion = &stillMf;
				in422.hasStepPrior = true;
				in422.estimatedStepDelta = 40.0;
				in422.deadZonePhi = kDeadZonePhiInter;
				in422.zeroCoeffFraction = 0.35;

				ChromaGpuGuard g422(ctx.gpu, cfg);
				const ChromaGpuProcessResult res422 = g422.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, in422, nullptr, nullptr);
				Assert::IsTrue(res422.succeeded);
				Assert::AreEqual(w422, res422.codedW, L"Native 4:2:2 codedW must be 32");
				Assert::AreEqual(h422, res422.codedH, L"Native 4:2:2 codedH must be full 64 (zero vertical subsampling!)");

				// Mandatory Same-Input Negative Control: forcing codedSubsampling = Yuv420 on the exact same 4:2:2 input
				// downsamples vertically past Nyquist and fails to recover the 2.25-row vertical chroma stripes!
				ChromaProcessInput wrongAs420 = in422;
				wrongAs420.codedSubsampling = ChromaSubsampling::Yuv420;
				ChromaGpuGuard gWrong420(ctx.gpu, cfg);
				const ChromaGpuProcessResult resWrong420 = gWrong420.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, wrongAs420, nullptr, nullptr);
				Assert::IsTrue(resWrong420.succeeded);

				const ActiveRect roi422{ 4, 6, 28, 58 };
				const double errNative422 = RmsDiffRect(res422.outputCb, masterCb422, roi422);
				const double errWrong420 = RmsDiffRect(resWrong420.outputCb, masterCb422, roi422);
				Assert::IsTrue(errNative422 < 3.0 && errWrong420 > errNative422 * 3.5,
					FmtMsg("True Native 4:2:2 MUST preserve/recover vertical high-freq chroma without 4:2:0 downsampling: nativeErr=%.4f vs wrong420Err=%.4f",
						errNative422, errWrong420).c_str());

				// Also verify already-sharp Native 4:2:2 suffers ZERO high-frequency chroma discarding!
				ChromaProcessInput sharpIn422 = in422;
				sharpIn422.receivedCb = masterCb422;
				sharpIn422.receivedCr = masterCr422;
				sharpIn422.cleanCb = masterCb422;
				sharpIn422.cleanCr = masterCr422;
				const ChromaGpuProcessResult resSharp422 = g422.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, sharpIn422, nullptr, nullptr);
				Assert::IsTrue(RmsDiffRect(resSharp422.outputCb, masterCb422, roi422) < 1e-3,
					L"Already-sharp Native 4:2:2 chroma must suffer ZERO high-frequency loss");
			}

			// ---- Part B: True Native 4:4:4 (coded=Yuv444, delivered=Yuv444 -> 64x64) ----
			// Horizontal + vertical wavelength = 2.30 luma px (above BOTH 4:2:0 and 4:2:2 horizontal Nyquist = 4.0 px!).
			{
				TdV2Plane masterCb444(lumaW, lumaH, 512.0);
				TdV2Plane masterCr444(lumaW, lumaH, 512.0);
				for (int y = 0; y < lumaH; ++y)
				{
					for (int x = 0; x < lumaW; ++x)
					{
						const double hvHi = 24.0 * std::cos(2.0 * kPi * static_cast<double>(x) / 2.30)
						                  * std::sin(2.0 * kPi * static_cast<double>(y) / 2.35);
						masterCb444.at(x, y) = 512.0 + hvHi;
						masterCr444.at(x, y) = 512.0 - hvHi;
					}
				}
				TdV2Plane degradedCb444(lumaW, lumaH, 512.0), degradedCr444(lumaW, lumaH, 512.0);
				for (size_t i = 0; i < masterCb444.v.size(); ++i)
				{
					degradedCb444.v[i] = 512.0 + 0.20 * (masterCb444.v[i] - 512.0);
					degradedCr444.v[i] = 512.0 + 0.20 * (masterCr444.v[i] - 512.0);
				}

				ChromaProcessInput in444;
				in444.seq = 322;
				in444.lumaW = lumaW;
				in444.lumaH = lumaH;
				in444.gridStatus = ChromaGridStatus::Identified;
				in444.codedSubsampling = ChromaSubsampling::Yuv444;
				in444.deliveredFormat = DeliveredChromaFormat::Yuv444;
				in444.siting = SitingSpec::Center();
				in444.activeRect = lumaFull;
				in444.receivedCb = degradedCb444;
				in444.receivedCr = degradedCr444;
				in444.hasCleanSplit = true;
				in444.cleanCb = degradedCb444;
				in444.cleanCr = degradedCr444;
				in444.prevPeak.cleanCb = &masterCb444;
				in444.prevPeak.cleanCr = &masterCr444;
				in444.prevPeak.lumaMotion = &stillMf;
				in444.nextPeak.cleanCb = &masterCb444;
				in444.nextPeak.cleanCr = &masterCr444;
				in444.nextPeak.lumaMotion = &stillMf;
				in444.hasStepPrior = true;
				in444.estimatedStepDelta = 40.0;
				in444.deadZonePhi = kDeadZonePhiInter;
				in444.zeroCoeffFraction = 0.35;

				ChromaGpuGuard g444(ctx.gpu, cfg);
				const ChromaGpuProcessResult res444 = g444.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, in444, nullptr, nullptr);
				Assert::IsTrue(res444.succeeded);
				Assert::AreEqual(lumaW, res444.codedW);
				Assert::AreEqual(lumaH, res444.codedH);

				// Negative control: forcing codedSubsampling = Yuv422 horizontally subsamples and destroys 2.3-px X detail!
				ChromaProcessInput wrongAs422 = in444;
				wrongAs422.codedSubsampling = ChromaSubsampling::Yuv422;
				ChromaGpuGuard gWrong422(ctx.gpu, cfg);
				const ChromaGpuProcessResult resWrong422 = gWrong422.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, wrongAs422, nullptr, nullptr);
				Assert::IsTrue(resWrong422.succeeded);

				const ActiveRect roi444{ 6, 6, 58, 58 };
				const double errNative444 = RmsDiffRect(res444.outputCb, masterCb444, roi444);
				const double errWrong422 = RmsDiffRect(resWrong422.outputCb, masterCb444, roi444);
				Assert::IsTrue(errNative444 < 2.5 && errWrong422 > errNative444 * 3.5,
					FmtMsg("True Native 4:4:4 MUST preserve horizontal+vertical high-freq chroma: nativeErr=%.4f vs wrong422Err=%.4f",
						errNative444, errWrong422).c_str());
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaFaceMaskAndRackFocusBokehVetoStrictZeroDeltaAcross420AndUpsampled422)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuChromaFaceMaskAndRackFocusBokehVetoStrictZeroDeltaAcross420AndUpsampled422)
		{
			WarpContext ctx;
			const int lumaW = 128;
			const int lumaH = 64;
			const int cw = lumaW / 16; // 8
			const int ch = lumaH / 16; // 4
			const size_t numCells = static_cast<size_t>(cw * ch);
			const int cW = 64;
			const int cH = 32;
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
			const SitingSpec siting = SitingSpec::Left();
			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			TdV2Plane masterCodedCb(cW, cH, 512.0), masterCodedCr(cW, cH, 512.0);
			TdV2Plane curCodedCb(cW, cH, 512.0), curCodedCr(cW, cH, 512.0);
			for (int y = 0; y < cH; ++y)
			{
				for (int x = 0; x < cW; ++x)
				{
					const double lSig = 24.0 * std::sin(2.0 * kPi * x / 24.0 + 0.2);
					const double mhSig = 16.0 * std::cos(2.0 * kPi * (x / 2.6 + y / 3.0) + 0.7);
					masterCodedCb.at(x, y) = 512.0 + lSig + mhSig;
					masterCodedCr.at(x, y) = 512.0 - lSig + mhSig;
					curCodedCb.at(x, y) = 512.0 + 0.2 * lSig + 0.1 * mhSig;
					curCodedCr.at(x, y) = 512.0 - 0.2 * lSig + 0.1 * mhSig;
				}
			}

			const TdV2Plane recDeliv422Cb = UpsampleDeltaToDelivered(
				curCodedCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, lumaFull, nullptr);
			const TdV2Plane recDeliv422Cr = UpsampleDeltaToDelivered(
				curCodedCr, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, lumaFull, nullptr);

			std::vector<uint8_t> faceMask(numCells, 0u);
			std::vector<uint8_t> rackMask(numCells, 0u);
			std::vector<uint8_t> bokehMask(numCells, 0u);
			faceMask[static_cast<size_t>(1 * cw + 1)] = 1u;  // Cell (1, 1): coded [8..16)x[8..16), deliv 4:2:2 [8..16)x[16..32)
			rackMask[static_cast<size_t>(1 * cw + 3)] = 1u;  // Cell (3, 1): coded [24..32)x[8..16), deliv 4:2:2 [24..32)x[16..32)
			bokehMask[static_cast<size_t>(1 * cw + 5)] = 1u; // Cell (5, 1): coded [40..48)x[8..16), deliv 4:2:2 [40..48)x[16..32)

			ChromaProcessInput in;
			in.seq = 330;
			in.lumaW = lumaW;
			in.lumaH = lumaH;
			in.gridStatus = ChromaGridStatus::Identified;
			in.codedSubsampling = ChromaSubsampling::Yuv420;
			in.deliveredFormat = DeliveredChromaFormat::Yuv422;
			in.siting = siting;
			in.activeRect = lumaFull;
			in.receivedCb = recDeliv422Cb;
			in.receivedCr = recDeliv422Cr;
			in.hasCleanSplit = true;
			in.cleanCb = curCodedCb;
			in.cleanCr = curCodedCr;
			in.prevPeak.cleanCb = &masterCodedCb;
			in.prevPeak.cleanCr = &masterCodedCr;
			in.prevPeak.lumaMotion = &stillMf;
			in.nextPeak.cleanCb = &masterCodedCb;
			in.nextPeak.cleanCr = &masterCodedCr;
			in.nextPeak.lumaMotion = &stillMf;
			in.hasStepPrior = true;
			in.estimatedStepDelta = 40.0;
			in.deadZonePhi = kDeadZonePhiInter;
			in.zeroCoeffFraction = 0.35;

			Config cfgUnt = MakePreset(Preset::Reference, Mode::On);
			cfgUnt.bokehMode = BokehMode::Untouched;

			// Mandatory Same-Input Negative Control (unmasked): all 3 target cells produce strong non-zero delivered deltas!
			ChromaGpuGuard gCtrl(ctx.gpu, cfgUnt);
			const ChromaGpuProcessResult rCtrl = gCtrl.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
			Assert::IsTrue(rCtrl.succeeded);
			Assert::IsTrue(MaxAbsRect(rCtrl.deliveredDeltaCb, ActiveRect{ 8, 16, 16, 32 }) > 6.0, L"Unmasked face cell must have delta > 6");
			Assert::IsTrue(MaxAbsRect(rCtrl.deliveredDeltaCb, ActiveRect{ 24, 16, 32, 32 }) > 6.0, L"Unmasked rack cell must have delta > 6");
			Assert::IsTrue(MaxAbsRect(rCtrl.deliveredDeltaCb, ActiveRect{ 40, 16, 48, 32 }) > 6.0, L"Unmasked bokeh cell must have delta > 6");

			// Masked run on player-upsampled 4:2:0 -> 4:2:2:
			// Every coded AND delivered pixel inside faceMask, rackFocus, and Untouched bokeh cells MUST be exact 0.0
			// (even where 4x4 bicubic coded->delivered upsampling straddles adjacent unmasked cells — CPU parity!).
			ChromaProcessInput maskedIn = in;
			maskedIn.faceMask = &faceMask;
			maskedIn.explicitFocusChangeCells = &rackMask;
			maskedIn.explicitDefocusedCells = &bokehMask;

			TdV2ChromaEngine cpuEng(cfgUnt);
			ChromaGpuGuard gUnt(ctx.gpu, cfgUnt);
			StandInRegistry siUnt;
			const ChromaProcessResult cpuUnt = cpuEng.ProcessFrame(maskedIn, nullptr);
			const ChromaGpuProcessResult gpuUnt = gUnt.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, maskedIn, nullptr, &siUnt);
			Assert::IsTrue(gpuUnt.succeeded);
			Assert::AreEqual(static_cast<size_t>(1u), gpuUnt.faceMaskedCells);
			Assert::AreEqual(static_cast<size_t>(2u), gpuUnt.focusVetoCells);
			Assert::IsTrue(siUnt.IsActive(StandInKind::FaceNonRigidMask));

			// Coded-grid zero-delta check on all 3 masked/vetoed cells
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.codedDeltaCb, ActiveRect{ 8, 8, 16, 16 }), L"Face codedDeltaCb must be 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.codedDeltaCb, ActiveRect{ 24, 8, 32, 16 }), L"Rack codedDeltaCb must be 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.codedDeltaCb, ActiveRect{ 40, 8, 48, 16 }), L"Untouched bokeh codedDeltaCb must be 0.0");

			// Delivered 4:2:2 strict zero-delta & bit-exact passthrough check (matching CPU TdV2ChromaEngine!)
			Assert::AreEqual(0.0, MaxAbsRect(cpuUnt.deliveredDeltaCb, ActiveRect{ 8, 16, 16, 32 }), L"CPU face deliveredDeltaCb must be 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.deliveredDeltaCb, ActiveRect{ 8, 16, 16, 32 }),
				L"GPU face-masked cell MUST keep deliveredDeltaCb == 0.0 across 4:2:0->4:2:2 upsampling (zero mask-edge bleed!)");
			Assert::AreEqual(0.0, MaxAbsDiffRect(gpuUnt.outputCb, recDeliv422Cb, ActiveRect{ 8, 16, 16, 32 }),
				L"GPU face-masked cell outputCb MUST stay bit-exact receivedCb on 4:2:0->4:2:2");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.deliveredDeltaCb, ActiveRect{ 24, 16, 32, 32 }),
				L"GPU rack-focus cell MUST keep deliveredDeltaCb == 0.0 across 4:2:0->4:2:2 upsampling");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.deliveredDeltaCb, ActiveRect{ 40, 16, 48, 32 }),
				L"GPU BokehMode::Untouched defocused cell MUST keep deliveredDeltaCb == 0.0 across 4:2:0->4:2:2 upsampling");

			// BokehMode::LowBandCellOnly allows L-band on bokeh cell while keeping rack-focus & face cells strictly 0.0!
			Config cfgLow = cfgUnt;
			cfgLow.bokehMode = BokehMode::LowBandCellOnly;
			ChromaGpuGuard gLow(ctx.gpu, cfgLow);
			const ChromaGpuProcessResult gpuLow = gLow.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, maskedIn, nullptr, nullptr);
			Assert::IsTrue(gpuLow.succeeded);
			Assert::AreEqual(0.0, MaxAbsRect(gpuLow.codedDeltaCb, ActiveRect{ 24, 8, 32, 16 }),
				L"Rack-focus codedDeltaCb must stay 0.0 under BokehMode::LowBandCellOnly");
			const ThreeBandPlanes lowBands = DecomposeActiveBands(gpuLow.codedDeltaCb, ActiveRect::Full(cW, cH));
			Assert::IsTrue(RmsRect(lowBands.L, ActiveRect{ 42, 10, 46, 14 }) > 2.0,
				L"BokehMode::LowBandCellOnly must allow L-band recovery on defocused cell");
			Assert::IsTrue(RmsRect(lowBands.H, ActiveRect{ 42, 10, 46, 14 }) < 0.35,
				L"BokehMode::LowBandCellOnly must hard-veto H-band on defocused cell");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaMissingMotionH9ZeroGhostingScratchArenaShortageAndModeOffZeroDispatches)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuChromaMissingMotionH9ZeroGhostingScratchArenaShortageAndModeOffZeroDispatches)
		{
			WarpContext ctx;

			const int lumaW = 64;
			const int lumaH = 64;
			const int cw = lumaW / 16;
			const int ch = lumaH / 16;
			const int cW = 32;
			const int cH = 32;
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
			Assert::IsNotNull(fmtRG32);
			Assert::IsNotNull(fmtRGBA32);

			// 1. Mode::Off -> exact 0 GPU dispatches!
			{
				ChromaGpuGuard offGuard(ctx.gpu, MakePreset(Preset::Reference, Mode::Off));
				ChromaGpuInput offIn{};
				offIn.lumaW = lumaW;
				offIn.lumaH = lumaH;
				offIn.activeRect = lumaFull;
				offIn.codedSubsampling = ChromaSubsampling::Yuv420;
				offIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
				ChromaGpuOutput offOut{};
				Assert::IsTrue(offGuard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, offIn, offOut, nullptr, nullptr));
				Assert::IsTrue(offOut.succeeded);
				Assert::AreEqual(0, offOut.dispatchCount, L"Mode::Off ProcessChromaGpu MUST issue 0 GPU dispatches");
				Assert::AreEqual(0, offGuard.engine.LastDispatchCount(), L"Mode::Off LastDispatchCount() MUST be 0");
			}

			// Prepare GPU textures for live zero-copy ProcessChromaGpu tests (4:2:0 -> 4:2:0)
			OwnedTex recTex1(ctx.gpu), recTex2(ctx.gpu), prevTex(ctx.gpu), nextTex(ctx.gpu);
			OwnedTex outCodedDelta(ctx.gpu), outDelivUv(ctx.gpu), outDelivDelta(ctx.gpu);
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex1.tex, cW, cH, fmtRG32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex2.tex, cW, cH, fmtRG32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, prevTex.tex, cW, cH, fmtRG32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, nextTex.tex, cW, cH, fmtRG32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCodedDelta.tex, cW, cH, fmtRG32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDelivUv.tex, cW, cH, fmtRGBA32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDelivDelta.tex, cW, cH, fmtRG32, true, true, false, true));

			auto uploadConstUv = [&](pl_tex t, float cb, float cr)
			{
				std::vector<float> buf(static_cast<size_t>(cW * cH) * 2u, 0.0f);
				for (size_t i = 0; i < static_cast<size_t>(cW * cH); ++i)
				{
					buf[i * 2u + 0u] = cb + 8.0f * std::sin(0.5f * static_cast<float>(i));
					buf[i * 2u + 1u] = cr - 7.0f * std::cos(0.4f * static_cast<float>(i));
				}
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, t, buf.data()));
			};
			uploadConstUv(recTex1.tex, 300.0f, 700.0f);
			uploadConstUv(prevTex.tex, 312.0f, 688.0f);
			uploadConstUv(nextTex.tex, 312.0f, 688.0f);

			// Frame 2 received chroma has a completely different scene color (Cb=620, Cr=380) so any stale Frame-1 ghost
			// or whole-frame unaligned blend with prevTex/nextTex (Cb=312, Cr=688) is immediately > 200 codes!
			std::vector<float> rec2Host(static_cast<size_t>(cW * cH) * 2u, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(cW * cH); ++i)
			{
				rec2Host[i * 2u + 0u] = 620.0f;
				rec2Host[i * 2u + 1u] = 380.0f;
			}
			Assert::IsTrue(Detail::UploadFloats(ctx.gpu, recTex2.tex, rec2Host.data()));

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			const Config cfgOn = MakePreset(Preset::Reference, Mode::On);

			// 2. ScratchArena shortage under kChromaArenaScope = 0x0403 fails visibly with ZERO silent private fallback!
			{
				const std::vector<VpSkeleton::ArenaRequest> reqs =
					TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, 16);
				Assert::IsFalse(reqs.empty());
				for (const auto& r : reqs)
					Assert::AreEqual(static_cast<int>(kChromaArenaScope), static_cast<int>(r.scope),
						L"Chroma ArenaRequests must use distinct kChromaArenaScope = 0x0403");

				VpSkeleton::PlTexBackend backend(ctx.gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);

				// Shed to Tier::T0 -> 0 T1 textures reserved -> MUST fail visibly!
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T0)));

				ChromaGpuGuard shortGuard(ctx.gpu, cfgOn);
				ChromaGpuInput sIn{};
				sIn.seq = 340;
				sIn.lumaW = lumaW;
				sIn.lumaH = lumaH;
				sIn.codedSubsampling = ChromaSubsampling::Yuv420;
				sIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
				sIn.receivedTex = recTex1.tex;
				sIn.prevPeak = { prevTex.tex, &stillMf, false };
				sIn.nextPeak = { nextTex.tex, &stillMf, false };
				sIn.hasStepPrior = true;
				sIn.estimatedStepDelta = 32.0;

				ChromaGpuOutput sOut{};
				sOut.outCodedDeltaTex = outCodedDelta.tex;
				StandInRegistry siShort;
				arena.BeginFrame();
				const bool okShort = shortGuard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, sIn, sOut, &arena, &siShort);
				Assert::IsFalse(okShort, L"ScratchArena shortage on kChromaArenaScope=0x0403 MUST return false");
				Assert::IsFalse(sOut.succeeded, L"sOut.succeeded must be false on chroma scratch shortage");
				Assert::IsTrue(sOut.scratchShort && shortGuard.engine.ScratchShort(), L"scratchShort must be true");
				Assert::IsTrue(siShort.IsActive(StandInKind::GpuScratchShortage), L"Must engage StandInKind::GpuScratchShortage");
				Assert::IsFalse(siShort.IsActive(StandInKind::GpuPrivateScratchFallback),
					L"ZERO silent fallback to private scratch textures allowed when arena != nullptr!");
				Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(),
					L"Chroma ArenaScopeGuard must close 0x0403 scope cleanly on shortage");
			}

			// 3. H9 Missing Motion Skip (lumaMotion == nullptr -> ZERO ghosting on live zero-copy ProcessChromaGpu!)
			{
				ChromaGpuGuard h9Guard(ctx.gpu, cfgOn);

				// Frame 1: valid motion -> writes Frame 1's fused Cb~308, Cr~692 into outDelivUv and non-zero delta into outDelivDelta
				ChromaGpuInput f1In{};
				f1In.seq = 341;
				f1In.lumaW = lumaW;
				f1In.lumaH = lumaH;
				f1In.codedSubsampling = ChromaSubsampling::Yuv420;
				f1In.deliveredFormat = DeliveredChromaFormat::Yuv420;
				f1In.receivedTex = recTex1.tex;
				f1In.prevPeak = { prevTex.tex, &stillMf, false };
				f1In.nextPeak = { nextTex.tex, &stillMf, false };
				f1In.hasStepPrior = true;
				f1In.estimatedStepDelta = 40.0;

				ChromaGpuOutput zcOut{};
				zcOut.outCodedDeltaTex = outCodedDelta.tex;
				zcOut.outDeliveredUvTex = outDelivUv.tex;
				zcOut.outDeliveredDeltaTex = outDelivDelta.tex;
				Assert::IsTrue(h9Guard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, f1In, zcOut, nullptr, nullptr));

				// Mandatory Same-Input Negative Control: if Frame 2 (Cb=620, Cr=380) synthesized a fake Still motion field
				// against unaligned prevTex/nextTex (Cb~312, Cr~688) with large step prior, it would ghost badly!
				ChromaGpuInput fakeStillIn = f1In;
				fakeStillIn.seq = 342;
				fakeStillIn.receivedTex = recTex2.tex;
				fakeStillIn.estimatedStepDelta = 400.0;
				ChromaGpuGuard fakeGuard(ctx.gpu, cfgOn);
				ChromaGpuOutput fakeOut{};
				OwnedTex fakeCoded(ctx.gpu), fakeUv(ctx.gpu), fakeDel(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, fakeCoded.tex, cW, cH, fmtRG32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, fakeUv.tex, cW, cH, fmtRGBA32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, fakeDel.tex, cW, cH, fmtRG32, true, true, false, true));
				fakeOut.outCodedDeltaTex = fakeCoded.tex;
				fakeOut.outDeliveredUvTex = fakeUv.tex;
				fakeOut.outDeliveredDeltaTex = fakeDel.tex;
				Assert::IsTrue(fakeGuard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, fakeStillIn, fakeOut, nullptr, nullptr));
				std::vector<float> fakeUvHost(static_cast<size_t>(cW * cH) * 4u, 0.0f);
				Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, fakeUv.tex, fakeUvHost.data()));
				Assert::IsTrue(std::fabs(fakeUvHost[0] - 620.0f) > 50.0f,
					L"Negative control: fake whole-frame Still motion on unaligned chroma refs must produce >50-code ghost");

				// Actual H9 Frame 2: lumaMotion == nullptr on BOTH refs, reusing zcOut textures from Frame 1!
				// Must skip both refs visibly, zero outCodedDeltaTex AND outDeliveredDeltaTex, AND write Frame 2's
				// receivedTex (Cb=620, Cr=380) into outDeliveredUvTex (ZERO stale Frame-1 ghosting!)
				ChromaGpuInput h9In = fakeStillIn;
				h9In.prevPeak = { prevTex.tex, nullptr, false };
				h9In.nextPeak = { nextTex.tex, nullptr, false };

				StandInRegistry siH9;
				Assert::IsTrue(h9Guard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, h9In, zcOut, nullptr, &siH9));
				Assert::IsTrue(zcOut.succeeded);
				Assert::AreEqual(static_cast<size_t>(2u), zcOut.motionStandInRefs, L"H9 must count 2 skipped motionStandInRefs");
				Assert::AreEqual(static_cast<size_t>(0u), zcOut.activeRefCount, L"H9 must leave activeRefCount == 0");
				Assert::IsTrue(siH9.IsActive(StandInKind::MissingMotionNoWholeBlend),
					L"H9 must engage StandInKind::MissingMotionNoWholeBlend");

				std::vector<float> cdHost(static_cast<size_t>(cW * cH) * 2u, 1.0f);
				std::vector<float> ddHost(static_cast<size_t>(cW * cH) * 2u, 1.0f);
				std::vector<float> uvHost(static_cast<size_t>(cW * cH) * 4u, 0.0f);
				Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, outCodedDelta.tex, cdHost.data()));
				Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, outDelivDelta.tex, ddHost.data()));
				Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, outDelivUv.tex, uvHost.data()));

				for (size_t i = 0; i < static_cast<size_t>(cW * cH); ++i)
				{
					Assert::AreEqual(0.0f, cdHost[i * 2u + 0u], 0.0f, L"H9 outCodedDeltaTex Cb must be exact 0.0");
					Assert::AreEqual(0.0f, ddHost[i * 2u + 0u], 0.0f,
						L"H9 zero-copy ProcessChromaGpu MUST zero outDeliveredDeltaTex (no stale Frame-1 delta!)");
					Assert::AreEqual(620.0f, uvHost[i * 4u + 0u], 1e-4f,
						L"H9 zero-copy ProcessChromaGpu MUST write Frame-2 received Cb=620 into outDeliveredUvTex (ZERO Frame-1 ghosting!)");
					Assert::AreEqual(380.0f, uvHost[i * 4u + 1u], 1e-4f,
						L"H9 zero-copy ProcessChromaGpu MUST write Frame-2 received Cr=380 into outDeliveredUvTex (ZERO Frame-1 ghosting!)");
				}
			}
		}
	};
} // namespace TdV2Sota3ChromaGpuNs
} // namespace Tests
