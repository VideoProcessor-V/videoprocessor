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

	static std::wstring FmtMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0)
	{
		char buf[340];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
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

				// Mandatory Same-Input Negative Control: naive 3-frame chroma average crushes own chroma grain variance to ~1/9!
				const TdV2Plane prevDelivCb = UpsampleDeltaToDelivered(
					prevCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				const TdV2Plane nextDelivCb = UpsampleDeltaToDelivered(
					nextCb, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, siting, lumaFull, nullptr);
				TdV2Plane naiveExtractedGrainCb(masterDelivCb.w, masterDelivCb.h, 0.0);
				for (size_t i = 0; i < naiveExtractedGrainCb.v.size(); ++i)
				{
					const double avg = (recDelivCb.v[i] + prevDelivCb.v[i] + nextDelivCb.v[i]) / 3.0;
					const double cleanExpectedAvg = (cleanDelivCb.v[i] + 2.0 * masterDelivCb.v[i]) / 3.0;
					naiveExtractedGrainCb.v[i] = avg - cleanExpectedAvg;
				}
				const double trueGrainVar = FineBandVarianceRect(grainDelivCb, dRoi);
				const double gpuGrainVar = FineBandVarianceRect(outExtractedGrainCb, dRoi);
				const double naiveGrainVar = FineBandVarianceRect(naiveExtractedGrainCb, dRoi);
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
			// 2x2 bokeh block (5..6, 1..2): coded [40..56)x[8..24), deliv 4:2:2 [40..56)x[16..48) so interior stays outside G_fine rFine=3 halo!
			bokehMask[static_cast<size_t>(1 * cw + 5)] = 1u;
			bokehMask[static_cast<size_t>(1 * cw + 6)] = 1u;
			bokehMask[static_cast<size_t>(2 * cw + 5)] = 1u;
			bokehMask[static_cast<size_t>(2 * cw + 6)] = 1u;

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
			Assert::AreEqual(static_cast<size_t>(5u), gpuUnt.focusVetoCells);
			Assert::IsTrue(siUnt.IsActive(StandInKind::FaceNonRigidMask));

			// Coded-grid zero-delta check on all masked/vetoed cells
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.codedDeltaCb, ActiveRect{ 8, 8, 16, 16 }), L"Face codedDeltaCb must be 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.codedDeltaCb, ActiveRect{ 24, 8, 32, 16 }), L"Rack codedDeltaCb must be 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.codedDeltaCb, ActiveRect{ 40, 8, 56, 24 }), L"Untouched bokeh codedDeltaCb must be 0.0");

			// Delivered 4:2:2 strict zero-delta & bit-exact passthrough check (matching CPU TdV2ChromaEngine!)
			Assert::AreEqual(0.0, MaxAbsRect(cpuUnt.deliveredDeltaCb, ActiveRect{ 8, 16, 16, 32 }), L"CPU face deliveredDeltaCb must be 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.deliveredDeltaCb, ActiveRect{ 8, 16, 16, 32 }),
				L"GPU face-masked cell MUST keep deliveredDeltaCb == 0.0 across 4:2:0->4:2:2 upsampling (zero mask-edge bleed!)");
			Assert::AreEqual(0.0, MaxAbsDiffRect(gpuUnt.outputCb, recDeliv422Cb, ActiveRect{ 8, 16, 16, 32 }),
				L"GPU face-masked cell outputCb MUST stay bit-exact receivedCb on 4:2:0->4:2:2");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.deliveredDeltaCb, ActiveRect{ 24, 16, 32, 32 }),
				L"GPU rack-focus cell MUST keep deliveredDeltaCb == 0.0 across 4:2:0->4:2:2 upsampling");
			Assert::AreEqual(0.0, MaxAbsRect(gpuUnt.deliveredDeltaCb, ActiveRect{ 40, 16, 56, 48 }),
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
			Assert::IsTrue(RmsRect(lowBands.L, ActiveRect{ 44, 12, 52, 20 }) > 2.0,
				L"BokehMode::LowBandCellOnly must allow L-band recovery on defocused cell");
			Assert::IsTrue(RmsRect(lowBands.H, ActiveRect{ 44, 12, 52, 20 }) < 0.35,
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

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaUnidentifiedCoarseOnlyPreservesFullGridRejectsFinerCodedAndTrimsNativeArena)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuChromaUnidentifiedCoarseOnlyPreservesFullGridRejectsFinerCodedAndTrimsNativeArena)
		{
			WarpContext ctx;
			const Config cfg = MakePreset(Preset::Reference, Mode::On);
			const YCbCrMatrixSpec mat = YCbCrMatrixSpec::Bt709();
			const double midCode = 512.0;

			// ---- Part A (TD-AUDIT-H2a): ChromaGridStatus::Unidentified across Yuv422, Yuv444, and Rgb444 ----
			// Adversarially pass codedSubsampling = Yuv420 alongside gridStatus = Unidentified:
			// GPU MUST visibly engage StandInKind::ChromaGridDetector, set coarseBandOnly = true, keep full delivered
			// resolution (codedW == dW, codedH == dH, ZERO 4:2:0 downsampling of real chroma!), preserve high-frequency
			// real chroma detail untouched, and match CPU TdV2ChromaEngine Branch-A coarse-only within < 1e-2 codes!
			{
				const int lumaW = 96;
				const int lumaH = 96;
				const int cw = lumaW / 16;
				const int ch = lumaH / 16;
				const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
				const SitingSpec siting = SitingSpec::Left();
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				const DeliveredChromaFormat delivFormats[3] = {
					DeliveredChromaFormat::Yuv422,
					DeliveredChromaFormat::Yuv444,
					DeliveredChromaFormat::Rgb444
				};

				for (int fi = 0; fi < 3; ++fi)
				{
					const DeliveredChromaFormat delivFmt = delivFormats[fi];
					const int dSubX = DeliveredSubsampleX(delivFmt);
					const int dSubY = DeliveredSubsampleY(delivFmt);
					const int dW = ChromaDetail::SubsampledDim(lumaW, dSubX);
					const int dH = ChromaDetail::SubsampledDim(lumaH, dSubY);
					const ActiveRect dFull = ActiveRect::Full(dW, dH);
					const ActiveRect dInterior{ dW / 4, 20, (3 * dW) / 4, dH - 20 };

					TdV2Plane masterLowCb(dW, dH, 512.0), masterLowCr(dW, dH, 512.0);
					TdV2Plane curCb(dW, dH, 512.0), curCr(dW, dH, 512.0);
					TdV2Plane lumaY(lumaW, lumaH, 500.0);

					for (int y = 0; y < dH; ++y)
					{
						const double wy = (y >= 8 && y < dH - 8)
							? (0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(y - 8) / static_cast<double>(dH - 17))))
							: 0.0;
						for (int x = 0; x < dW; ++x)
						{
							const double wx = (x >= 8 / dSubX && x < dW - 8 / dSubX)
								? (0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(x - 8 / dSubX) / static_cast<double>(dW - 2 * (8 / dSubX) - 1))))
								: 0.0;
							const double win = wx * wy;
							const double lowCb = 478.0 + 0.65 * static_cast<double>(x * dSubX) + 0.22 * static_cast<double>(y * dSubY);
							const double lowCr = 546.0 - 0.60 * static_cast<double>(x * dSubX) - 0.20 * static_cast<double>(y * dSubY);
							masterLowCb.at(x, y) = lowCb;
							masterLowCr.at(x, y) = lowCr;

							const double bandCb = std::nearbyint(lowCb / 20.0) * 20.0;
							const double bandCr = std::nearbyint(lowCr / 20.0) * 20.0;

							// Real delivered-grid high-frequency chroma above 4:2:0 Nyquist (present ONLY in cur, absent in peaks!)
							const double hfWave = (dSubX > 1)
								? std::sin(2.0 * kPi * static_cast<double>(y) / 2.4 + 0.25 * static_cast<double>(x))
								: (std::cos(2.0 * kPi * static_cast<double>(x) / 2.4) * std::sin(2.0 * kPi * static_cast<double>(y) / 2.4));
							const double hfCb = 28.0 * win * hfWave;
							const double hfCr = -16.0 * win * hfWave;

							curCb.at(x, y) = bandCb + hfCb;
							curCr.at(x, y) = bandCr + hfCr;
						}
					}

					TdV2Plane prevCb = masterLowCb, prevCr = masterLowCr;
					TdV2Plane nextCb = masterLowCb, nextCr = masterLowCr;
					{
						std::mt19937 rP(9301u + static_cast<uint32_t>(fi)), rN(9311u + static_cast<uint32_t>(fi));
						std::normal_distribution<double> nd(0.0, 0.35);
						for (size_t i = 0; i < prevCb.v.size(); ++i)
						{
							prevCb.v[i] += nd(rP); prevCr.v[i] += nd(rP);
							nextCb.v[i] += nd(rN); nextCr.v[i] += nd(rN);
						}
					}

					ChromaProcessInput cIn;
					cIn.seq = static_cast<uint64_t>(500 + fi);
					cIn.lumaW = lumaW;
					cIn.lumaH = lumaH;
					cIn.deliveredFormat = delivFmt;
					cIn.codedSubsampling = ChromaSubsampling::Yuv420; // Adversarial: Unidentified MUST ignore 4:2:0 and keep dW x dH!
					cIn.gridStatus = ChromaGridStatus::Unidentified;
					cIn.siting = siting;
					cIn.matrix = mat;
					cIn.activeRect = lumaFull;
					cIn.hasCleanSplit = true;
					cIn.cleanCb = curCb;
					cIn.cleanCr = curCr;
					cIn.prevPeak = { &prevCb, &prevCr, &stillMf, false };
					cIn.nextPeak = { &nextCb, &nextCr, &stillMf, false };
					cIn.hasStepPrior = true;
					cIn.estimatedStepDelta = 24.0;
					cIn.deadZonePhi = kDeadZonePhiInter;
					cIn.zeroCoeffFraction = 0.35;

					if (delivFmt == DeliveredChromaFormat::Rgb444)
					{
						cIn.receivedR = TdV2Plane(dW, dH, 0.0);
						cIn.receivedG = TdV2Plane(dW, dH, 0.0);
						cIn.receivedB = TdV2Plane(dW, dH, 0.0);
						const double kR = 2.0 * (1.0 - mat.kr);
						const double kB = 2.0 * (1.0 - mat.kb);
						for (size_t i = 0; i < curCb.v.size(); ++i)
						{
							const double yv = lumaY.v[i];
							const double cb0 = curCb.v[i] - midCode;
							const double cr0 = curCr.v[i] - midCode;
							cIn.receivedR.v[i] = yv + kR * cr0;
							cIn.receivedB.v[i] = yv + kB * cb0;
							cIn.receivedG.v[i] = yv - (mat.kr * kR / mat.kg) * cr0 - (mat.kb * kB / mat.kg) * cb0;
						}
					}
					else
					{
						cIn.receivedCb = curCb;
						cIn.receivedCr = curCr;
					}

					TdV2ChromaEngine cpuEng(cfg);
					ChromaGpuGuard gpuGuard(ctx.gpu, cfg);
					StandInRegistry siCpu, siGpu;
					const ChromaProcessResult cpuRes = cpuEng.ProcessFrame(cIn, &siCpu);
					const ChromaGpuProcessResult gpuRes = gpuGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, nullptr, &siGpu);

					Assert::IsTrue(gpuRes.succeeded, L"Unidentified grid GPU chroma must succeed");
					Assert::IsTrue(gpuRes.coarseBandOnly && cpuRes.coarseBandOnly,
						L"Unidentified grid must set coarseBandOnly == true on both CPU and GPU");
					Assert::IsTrue(siGpu.IsActive(StandInKind::ChromaGridDetector) && siCpu.IsActive(StandInKind::ChromaGridDetector),
						L"Unidentified grid must visibly engage StandInKind::ChromaGridDetector");
					Assert::AreEqual(dW, gpuRes.codedW, L"Unidentified grid must keep full delivered width (zero 4:2:0 downsampling!)");
					Assert::AreEqual(dH, gpuRes.codedH, L"Unidentified grid must keep full delivered height (zero 4:2:0 downsampling!)");

					// Strict CPU Branch-A coarse-only parity (< 1e-2 codes) across deltas and outputs!
					Assert::IsTrue(MaxAbsDiffRect(cpuRes.deliveredDeltaCb, gpuRes.deliveredDeltaCb, dFull) < 1e-2,
						FmtMsg("Unidentified fmt=%.0f deliveredDeltaCb CPU/GPU diff=%.6f",
							static_cast<double>(fi), MaxAbsDiffRect(cpuRes.deliveredDeltaCb, gpuRes.deliveredDeltaCb, dFull)).c_str());
					Assert::IsTrue(MaxAbsDiffRect(cpuRes.deliveredDeltaCr, gpuRes.deliveredDeltaCr, dFull) < 1e-2,
						FmtMsg("Unidentified fmt=%.0f deliveredDeltaCr CPU/GPU diff=%.6f",
							static_cast<double>(fi), MaxAbsDiffRect(cpuRes.deliveredDeltaCr, gpuRes.deliveredDeltaCr, dFull)).c_str());

					TdV2Plane gpuOutCb(dW, dH, 0.0);
					if (delivFmt == DeliveredChromaFormat::Rgb444)
					{
						Assert::IsTrue(MaxAbsDiffRect(cpuRes.outputR, gpuRes.outputR, dFull) < 1e-2 &&
						               MaxAbsDiffRect(cpuRes.outputG, gpuRes.outputG, dFull) < 1e-2 &&
						               MaxAbsDiffRect(cpuRes.outputB, gpuRes.outputB, dFull) < 1e-2,
							L"Unidentified Rgb444 output RGB must match CPU Branch-A within < 1e-2 codes");
						for (size_t i = 0; i < gpuOutCb.v.size(); ++i)
						{
							double yv = 0.0, cbv = 0.0, crv = 0.0;
							mat.RgbToYCbCr(gpuRes.outputR.v[i], gpuRes.outputG.v[i], gpuRes.outputB.v[i], midCode, yv, cbv, crv);
							gpuOutCb.v[i] = cbv;
						}
					}
					else
					{
						Assert::IsTrue(MaxAbsDiffRect(cpuRes.outputCb, gpuRes.outputCb, dFull) < 1e-2 &&
						               MaxAbsDiffRect(cpuRes.outputCr, gpuRes.outputCr, dFull) < 1e-2,
							L"Unidentified YUV output Cb/Cr must match CPU Branch-A within < 1e-2 codes");
						gpuOutCb = gpuRes.outputCb;
					}

					// Real delivered-grid high-frequency chroma is preserved untouched (< 5e-2 RMS), whereas
					// a naive full-band temporal average negative control destroys the real HF chroma (> 5.0 RMS)!
					const ThreeBandPlanes curBandsCb = DecomposeActiveBands(curCb, dFull);
					const ThreeBandPlanes outBandsCb = DecomposeActiveBands(gpuOutCb, dFull);
					TdV2Plane naiveFullCb(dW, dH, 0.0);
					for (size_t i = 0; i < curCb.v.size(); ++i)
						naiveFullCb.v[i] = (curCb.v[i] + prevCb.v[i] + nextCb.v[i]) / 3.0;
					const ThreeBandPlanes naiveBandsCb = DecomposeActiveBands(naiveFullCb, dFull);
					const double gpuHfLoss = RmsDiffRect(outBandsCb.H, curBandsCb.H, dInterior);
					const double naiveHfLoss = RmsDiffRect(naiveBandsCb.H, curBandsCb.H, dInterior);
					Assert::IsTrue(gpuHfLoss < 5e-2 && naiveHfLoss > 5.0,
						FmtMsg("Unidentified fmt=%.0f must preserve real HF chroma (gpuLoss=%.5f vs naiveLoss=%.3f)",
							static_cast<double>(fi), gpuHfLoss, naiveHfLoss).c_str());

					// Coarse banding error IS genuinely reduced!
					double dummyGain = 1.0;
					const double sigX = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubX);
					const double sigY = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubY);
					const TdV2Plane refCoarse = ChromaDetail::BlurActiveAnisotropic(masterLowCb, dFull, sigX, sigY, dummyGain);
					const TdV2Plane inCoarse  = ChromaDetail::BlurActiveAnisotropic(curCb, dFull, sigX, sigY, dummyGain);
					const TdV2Plane outCoarse = ChromaDetail::BlurActiveAnisotropic(gpuOutCb, dFull, sigX, sigY, dummyGain);
					Assert::IsTrue(RmsDiffRect(outCoarse, refCoarse, dInterior) < RmsDiffRect(inCoarse, refCoarse, dInterior) * 0.70,
						L"Unidentified coarse-only mode must genuinely reduce coarse chroma banding error");
				}
			}

			// ---- Part B (TD-AUDIT-H2b): Invalid codedSubsampling finer than deliveredFormat rejected visibly ----
			{
				const int lumaW = 64, lumaH = 64;
				const CellMotionField stillMf = CellMotionField::Uniform(4, 4, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				auto assertFinerCodedRejected = [&](ChromaSubsampling badCoded, DeliveredChromaFormat delivFmt, const wchar_t* label)
				{
					const int dW = ChromaDetail::SubsampledDim(lumaW, DeliveredSubsampleX(delivFmt));
					const int dH = ChromaDetail::SubsampledDim(lumaH, DeliveredSubsampleY(delivFmt));
					const TdV2Plane flatPlane(dW, dH, 512.0);

					ChromaProcessInput badIn;
					badIn.seq = 520;
					badIn.lumaW = lumaW;
					badIn.lumaH = lumaH;
					badIn.gridStatus = ChromaGridStatus::Identified;
					badIn.codedSubsampling = badCoded;
					badIn.deliveredFormat = delivFmt;
					badIn.receivedCb = flatPlane;
					badIn.receivedCr = flatPlane;
					badIn.hasCleanSplit = true;
					badIn.cleanCb = flatPlane;
					badIn.cleanCr = flatPlane;
					badIn.prevPeak = { &flatPlane, &flatPlane, &stillMf, false };
					badIn.nextPeak = { &flatPlane, &flatPlane, &stillMf, false };
					badIn.hasStepPrior = true;
					badIn.estimatedStepDelta = 16.0;

					ChromaGpuGuard gGuard(ctx.gpu, cfg);
					StandInRegistry st;
					const ChromaGpuProcessResult r = gGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, badIn, nullptr, &st);
					Assert::IsFalse(r.succeeded, label);
					Assert::AreEqual(0, r.dispatchCount, label);
					Assert::IsFalse(r.lastError.empty(), label);
					Assert::IsTrue(st.IsActive(StandInKind::GpuDispatchFailed), label);
				};

				assertFinerCodedRejected(ChromaSubsampling::Yuv444, DeliveredChromaFormat::Yuv420, L"coded=444 on delivered=420 must fail visibly");
				assertFinerCodedRejected(ChromaSubsampling::Yuv422, DeliveredChromaFormat::Yuv420, L"coded=422 on delivered=420 must fail visibly");
				assertFinerCodedRejected(ChromaSubsampling::Yuv444, DeliveredChromaFormat::Yuv422, L"coded=444 on delivered=422 must fail visibly");
			}

			// ---- Part C (TD-AUDIT-L2): Format-aware EstimatedScratchVramBytesFor & ArenaRequests trim 3 reduced* textures ----
			{
				const int w4k = 3840;
				const int h4k = 2160;
				const uint64_t full420 = TdV2ChromaGpuEngine::EstimatedScratchVramBytes(w4k, h4k, ChromaSubsampling::Yuv420);
				const uint64_t nat420  = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(w4k, h4k, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv420);
				const uint64_t full422 = TdV2ChromaGpuEngine::EstimatedScratchVramBytes(w4k, h4k, ChromaSubsampling::Yuv422);
				const uint64_t nat422  = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(w4k, h4k, ChromaSubsampling::Yuv422, DeliveredChromaFormat::Yuv422);
				const uint64_t full444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytes(w4k, h4k, ChromaSubsampling::Yuv444);
				const uint64_t nat444  = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(w4k, h4k, ChromaSubsampling::Yuv444, DeliveredChromaFormat::Yuv444);
				const uint64_t rgb444  = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(w4k, h4k, ChromaSubsampling::Yuv444, DeliveredChromaFormat::Rgb444);
				const uint64_t up420To444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(w4k, h4k, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444);

				// Exact 3 * cW * cH * 8-byte RG32F reduction scratch savings (-47.5 MiB on 4K 4:2:0, -189.8 MiB on 4K 4:4:4)!
				Assert::AreEqual(static_cast<uint64_t>(3ULL * 1920ULL * 1080ULL * 8ULL), full420 - nat420,
					L"4K native 4:2:0 must trim exact 3 x 1920x1080 RG32F textures (47.46 MiB)");
				Assert::AreEqual(static_cast<uint64_t>(3ULL * 1920ULL * 2160ULL * 8ULL), full422 - nat422,
					L"4K native 4:2:2 must trim exact 3 x 1920x2160 RG32F textures (94.92 MiB)");
				Assert::AreEqual(static_cast<uint64_t>(3ULL * 3840ULL * 2160ULL * 8ULL), full444 - nat444,
					L"4K native 4:4:4 must trim exact 3 x 3840x2160 RG32F textures (189.84 MiB)");
				Assert::AreEqual(full444, rgb444, L"Delivered Rgb444 requires Pass C0 RGB->YUV reduction scratch");
				Assert::AreEqual(full420, up420To444, L"Player-upsampled 4:2:0->4:4:4 requires Pass C0 reduction scratch");

				const auto reqs4k420 = TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, w4k, h4k, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv420);
				const auto reqs4k444 = TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, w4k, h4k, ChromaSubsampling::Yuv444, DeliveredChromaFormat::Yuv444);
				Assert::AreEqual(nat420, VpSkeleton::PlanArena(reqs4k420, VpSkeleton::Tier::T1).bytes);
				Assert::AreEqual(nat444, VpSkeleton::PlanArena(reqs4k444, VpSkeleton::Tier::T1).bytes);

				// Execute live on WARP using the trimmed format-aware ScratchArena on native 4:2:0 AND native 4:4:4!
				const int lumaW = 64, lumaH = 64;
				const CellMotionField stillMf = CellMotionField::Uniform(4, 4, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				VpSkeleton::PlTexBackend backend(ctx.gpu);

				const ChromaSubsampling subs[2] = { ChromaSubsampling::Yuv420, ChromaSubsampling::Yuv444 };
				const DeliveredChromaFormat fmts[2] = { DeliveredChromaFormat::Yuv420, DeliveredChromaFormat::Yuv444 };
				for (int mi = 0; mi < 2; ++mi)
				{
					const int cW = ChromaDetail::SubsampledDim(lumaW, SubsampleX(subs[mi]));
					const int cH = ChromaDetail::SubsampledDim(lumaH, SubsampleY(subs[mi]));
					TdV2Plane masterCb(cW, cH, 512.0), masterCr(cW, cH, 512.0);
					TdV2Plane degCb(cW, cH, 512.0), degCr(cW, cH, 512.0);
					for (int y = 0; y < cH; ++y)
					{
						for (int x = 0; x < cW; ++x)
						{
							const double s = 20.0 * std::sin(2.0 * kPi * (x / 5.0 + y / 6.0) + 0.4);
							masterCb.at(x, y) = 512.0 + s;
							masterCr.at(x, y) = 512.0 - s;
							degCb.at(x, y) = 512.0 + 0.25 * s;
							degCr.at(x, y) = 512.0 - 0.25 * s;
						}
					}
					TdV2Plane prevMasterCb = masterCb, prevMasterCr = masterCr;
					TdV2Plane nextMasterCb = masterCb, nextMasterCr = masterCr;
					{
						std::mt19937 rP(9401u + static_cast<uint32_t>(mi)), rN(9411u + static_cast<uint32_t>(mi));
						std::normal_distribution<double> nd(0.0, 0.35);
						for (size_t i = 0; i < masterCb.v.size(); ++i)
						{
							prevMasterCb.v[i] += nd(rP); prevMasterCr.v[i] += nd(rP);
							nextMasterCb.v[i] += nd(rN); nextMasterCr.v[i] += nd(rN);
						}
					}

					const auto trimmedReqs = TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, lumaW, lumaH, subs[mi], fmts[mi], 16);
					VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> trimmedArena(backend);
					Assert::IsTrue(trimmedArena.Reserve(VpSkeleton::PlanArena(trimmedReqs, VpSkeleton::Tier::T1)));

					ChromaProcessInput nIn;
					nIn.seq = static_cast<uint64_t>(530 + mi);
					nIn.lumaW = lumaW;
					nIn.lumaH = lumaH;
					nIn.gridStatus = ChromaGridStatus::Identified;
					nIn.codedSubsampling = subs[mi];
					nIn.deliveredFormat = fmts[mi];
					nIn.receivedCb = degCb;
					nIn.receivedCr = degCr;
					nIn.hasCleanSplit = true;
					nIn.cleanCb = degCb;
					nIn.cleanCr = degCr;
					nIn.prevPeak = { &prevMasterCb, &prevMasterCr, &stillMf, false };
					nIn.nextPeak = { &nextMasterCb, &nextMasterCr, &stillMf, false };
					nIn.hasStepPrior = true;
					nIn.estimatedStepDelta = 36.0;
					nIn.deadZonePhi = kDeadZonePhiInter;
					nIn.zeroCoeffFraction = 0.35;

					ChromaGpuGuard gGuard(ctx.gpu, cfg);
					StandInRegistry stNat;
					trimmedArena.BeginFrame();
					const ChromaGpuProcessResult rNat = gGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, nIn, &trimmedArena, &stNat);
					Assert::IsTrue(rNat.succeeded && !rNat.scratchShort, L"Native YUV must execute cleanly on trimmed format-aware arena");
					Assert::IsFalse(stNat.IsActive(StandInKind::GpuScratchShortage));
					Assert::IsFalse(stNat.IsActive(StandInKind::GpuPrivateScratchFallback));
					const ActiveRect cRoi{ 4, 4, cW - 4, cH - 4 };
					Assert::IsTrue(RmsDiffRect(rNat.outputCb, masterCb, cRoi) < RmsDiffRect(degCb, masterCb, cRoi) * 0.45);

					// Mandatory Negative Control on 4:2:0 trimmed arena: player-upsampled 4:2:0->4:4:4 needs reducedRec (7th RG32F)
					// and MUST visibly report scratchShort on the 6-RG32F native arena (proving the 3 textures were genuinely omitted!).
					if (mi == 0)
					{
						ChromaProcessInput upIn = nIn;
						upIn.seq = 539;
						upIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
						upIn.receivedCb = TdV2Plane(lumaW, lumaH, 512.0);
						upIn.receivedCr = TdV2Plane(lumaW, lumaH, 512.0);
						StandInRegistry stShort;
						trimmedArena.BeginFrame();
						const ChromaGpuProcessResult rUp = gGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, upIn, &trimmedArena, &stShort);
						Assert::IsFalse(rUp.succeeded, L"Upsampled 4:2:0->4:4:4 on native-trimmed 6-RG32F arena must fail visibly");
						Assert::IsTrue(rUp.scratchShort && stShort.IsActive(StandInKind::GpuScratchShortage),
							L"Upsampled 4:2:0->4:4:4 on native-trimmed arena must engage GpuScratchShortage");
					}
				}
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaSmallCellPx8CornerZeroMvWithLargeInteriorForegroundMvZeroOobAndCpuParity)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuChromaSmallCellPx8CornerZeroMvWithLargeInteriorForegroundMvZeroOobAndCpuParity)
		{
			WarpContext ctx;

			// 128x128 luma with cellPx = 8 -> 16x16 cells!
			// On Yuv420 (64x64 coded = 4x4 workgroups of 16x16 coded px), each 16x16 coded workgroup spans 4x4 luma cells:
			// e.g. workgroup (1, 1) [coded 16..31 x 16..31] has 4 corner cells (4,4), (7,4), (4,7), (7,7).
			// Setting those 4 corner cells to mv = (0, 0) while interior cells carry large mv = (+10, -10) luma px
			// (+5, -5 coded px -> ldsRy = -5..-1 < 0 and ldsRx = 20 >= 20 if LDS were wrongly enabled!) directly tests TD-AUDIT-H3!
			const int lumaW = 128;
			const int lumaH = 128;
			const int cellPx = 8;
			const int cw = lumaW / cellPx; // 16
			const int ch = lumaH / cellPx; // 16
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
			const SitingSpec siting = SitingSpec::Left();

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.cellPx = cellPx;
			cfg.grainCellPx = cellPx;

			const ChromaSubsampling codedSubs[2] = { ChromaSubsampling::Yuv420, ChromaSubsampling::Yuv422 };
			const DeliveredChromaFormat delivFmts[2] = { DeliveredChromaFormat::Yuv420, DeliveredChromaFormat::Yuv422 };

			for (int si = 0; si < 2; ++si)
			{
				const ChromaSubsampling cSub = codedSubs[si];
				const DeliveredChromaFormat dFmt = delivFmts[si];
				const int subX = SubsampleX(cSub);
				const int subY = SubsampleY(cSub);
				const int cW = ChromaDetail::SubsampledDim(lumaW, subX);
				const int cH = ChromaDetail::SubsampledDim(lumaH, subY);
				const ActiveRect cFull = ActiveRect::Full(cW, cH);

				auto sampleBaseScene = [&](double lx, double ly, bool isCb) -> double
				{
					const double s = 28.0 * std::sin(2.0 * kPi * (lx / 12.0 + ly / 14.0) + 0.35)
					               + 22.0 * std::cos(2.0 * kPi * (lx / 6.0 - ly / 6.5) + 0.9);
					return isCb ? (512.0 + s) : (512.0 - 0.9 * s);
				};

				auto isWgCornerCell = [](int cx, int cy) -> bool
				{
					const int rx = cx % 4;
					const int ry = cy % 4;
					return (rx == 0 || rx == 3) && (ry == 0 || ry == 3);
				};

				auto isForegroundBlockCell = [&](int cx, int cy) -> bool
				{
					// Interior cells of workgroup (1, 1) [cells 4..7 x 4..7] and workgroup (2, 1) [cells 8..11 x 4..7]
					// excluding the 4 corner cells of each 4x4 workgroup block!
					const bool inWg11 = (cx >= 4 && cx <= 7 && cy >= 4 && cy <= 7) && !isWgCornerCell(cx, cy);
					const bool inWg21 = (cx >= 8 && cx <= 11 && cy >= 4 && cy <= 7) && !isWgCornerCell(cx, cy);
					return inWg11 || inWg21;
				};

				// Regime 1: Fast foreground Object (mv = (+10, -10), camMv = (0, 0), cls = Object) surrounded by
				// 4 workgroup corner cells with mv = (0, 0) (Still), proving zero OOB LDS corruption & <1e-2 parity.
				// Regime 2: Tracked motion (mv = (+10, -10), camMv = (+10, -10), cls = Camera -> gate = 1.0) so
				// warped interior pixels from p + (+5, -5) are actively fused with full weight into outputCb/Cr!
				for (int regime = 0; regime < 2; ++regime)
				{
					const bool trackedActiveFuse = (regime == 1);
					CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					CellMotionField mfNext = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					for (int cy = 0; cy < ch; ++cy)
					{
						for (int cx = 0; cx < cw; ++cx)
						{
							if (!isForegroundBlockCell(cx, cy))
								continue;
							const size_t idx = static_cast<size_t>(cy * cw + cx);
							mfPrev.vx[idx] = +10.0;
							mfPrev.vy[idx] = -10.0;
							mfNext.vx[idx] = +10.0;
							mfNext.vy[idx] = -10.0;
							if (trackedActiveFuse)
							{
								mfPrev.camVx[idx] = +10.0; mfPrev.camVy[idx] = -10.0; mfPrev.cls[idx] = RegionMotionClass::Camera;
								mfNext.camVx[idx] = +10.0; mfNext.camVy[idx] = -10.0; mfNext.cls[idx] = RegionMotionClass::Camera;
							}
							else
							{
								mfPrev.cls[idx] = RegionMotionClass::Object;
								mfNext.cls[idx] = RegionMotionClass::Object;
							}
						}
					}

					// Smooth continuous reference scene everywhere:
					//   prevRef(lx, ly) ~= sampleBaseScene(lx, ly), nextRef(lx, ly) ~= sampleBaseScene(lx, ly)
					//   masterCur(lx, ly) == sampleBaseScene(lx + 10, ly - 10) on interior (+10, -10) cells,
					//                     == sampleBaseScene(lx, ly) on (0, 0) cells!
					// Hence ref(p + mvCoded) == masterCur(p) pixel-for-pixel across the entire plane (zero stamp seams!).
					TdV2Plane masterCurCb(cW, cH, 512.0), masterCurCr(cW, cH, 512.0);
					TdV2Plane prevRefCb(cW, cH, 512.0), prevRefCr(cW, cH, 512.0);
					TdV2Plane nextRefCb(cW, cH, 512.0), nextRefCr(cW, cH, 512.0);
					for (int y = 0; y < cH; ++y)
					{
						const double ly = siting.CodedToLumaY(static_cast<double>(y), subY);
						const int cy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ly)) / cellPx));
						for (int x = 0; x < cW; ++x)
						{
							const double lx = siting.CodedToLumaX(static_cast<double>(x), subX);
							const int cx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(lx)) / cellPx));
							const bool fg = isForegroundBlockCell(cx, cy);
							const double sx = fg ? (lx + 10.0) : lx;
							const double sy = fg ? (ly - 10.0) : ly;
							masterCurCb.at(x, y) = sampleBaseScene(sx, sy, true);
							masterCurCr.at(x, y) = sampleBaseScene(sx, sy, false);
							prevRefCb.at(x, y) = sampleBaseScene(lx, ly, true);
							prevRefCr.at(x, y) = sampleBaseScene(lx, ly, false);
							nextRefCb.at(x, y) = sampleBaseScene(lx, ly, true);
							nextRefCr.at(x, y) = sampleBaseScene(lx, ly, false);
						}
					}
					{
						std::mt19937 rP(9501u + static_cast<uint32_t>(si * 2 + regime));
						std::mt19937 rN(9511u + static_cast<uint32_t>(si * 2 + regime));
						std::normal_distribution<double> nd(0.0, 0.35);
						for (size_t i = 0; i < prevRefCb.v.size(); ++i)
						{
							prevRefCb.v[i] += nd(rP); prevRefCr.v[i] += nd(rP);
							nextRefCb.v[i] += nd(rN); nextRefCr.v[i] += nd(rN);
						}
					}

					const ThreeBandPlanes mBandsCb = DecomposeActiveBands(masterCurCb, cFull);
					const ThreeBandPlanes mBandsCr = DecomposeActiveBands(masterCurCr, cFull);
					TdV2Plane degCurCb(cW, cH, 512.0), degCurCr(cW, cH, 512.0);
					for (size_t i = 0; i < masterCurCb.v.size(); ++i)
					{
						degCurCb.v[i] = masterCurCb.v[i] - 0.80 * (mBandsCb.M.v[i] + mBandsCb.H.v[i]);
						degCurCr.v[i] = masterCurCr.v[i] - 0.80 * (mBandsCr.M.v[i] + mBandsCr.H.v[i]);
					}

					const auto arenaReqs = TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, lumaW, lumaH, cSub, dFmt, cellPx);
					VpSkeleton::PlTexBackend backend(ctx.gpu);
					VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
					Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(arenaReqs, VpSkeleton::Tier::T1)));

					ChromaProcessInput in;
					in.seq = static_cast<uint64_t>(600 + si * 10 + regime);
					in.lumaW = lumaW;
					in.lumaH = lumaH;
					in.gridStatus = ChromaGridStatus::Identified;
					in.codedSubsampling = cSub;
					in.deliveredFormat = dFmt;
					in.siting = siting;
					in.activeRect = lumaFull;
					in.receivedCb = degCurCb;
					in.receivedCr = degCurCr;
					in.hasCleanSplit = true;
					in.cleanCb = degCurCb;
					in.cleanCr = degCurCr;
					in.prevPeak = { &prevRefCb, &prevRefCr, &mfPrev, false };
					in.nextPeak = { &nextRefCb, &nextRefCr, &mfNext, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 48.0;
					in.deadZonePhi = kDeadZonePhiInter;
					in.zeroCoeffFraction = 0.35;

					TdV2ChromaEngine cpuEng(cfg);
					ChromaGpuGuard gpuGuard(ctx.gpu, cfg);
					arena.BeginFrame();
					const ChromaProcessResult cpuRes = cpuEng.ProcessFrame(in, nullptr);
					const ChromaGpuProcessResult gpuRes = gpuGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, in, &arena, nullptr);

					Assert::IsTrue(gpuRes.succeeded && !gpuRes.scratchShort, L"cellPx=8 chroma GPU run must succeed");
					Assert::AreEqual(static_cast<size_t>(0u), gpuRes.nonFiniteCells, L"Zero non-finite / OOB LDS corruption cells allowed");
					for (double v : gpuRes.outputCb.v)
						Assert::IsTrue(std::isfinite(v), L"All outputCb pixels must be finite (zero OOB LDS NaN)");
					for (double v : gpuRes.outputCr.v)
						Assert::IsTrue(std::isfinite(v), L"All outputCr pixels must be finite (zero OOB LDS NaN)");

					// Strict CPU vs WARP GPU parity (< 1e-2 codes) across the entire plane including workgroup (1, 1)!
					const double maxDiffCb = MaxAbsDiffRect(cpuRes.outputCb, gpuRes.outputCb, cFull);
					const double maxDiffCr = MaxAbsDiffRect(cpuRes.outputCr, gpuRes.outputCr, cFull);
					Assert::IsTrue(maxDiffCb < 1e-2 && maxDiffCr < 1e-2,
						FmtMsg("TD-AUDIT-H3 cellPx=8 CPU/GPU parity failed (sub=%.0f regime=%.0f): maxCb=%.6f maxCr=%.6f",
							static_cast<double>(si), static_cast<double>(regime), maxDiffCb, maxDiffCr).c_str());

					if (trackedActiveFuse)
					{
						// Interior core of workgroup (1, 1) cells (5..6, 5..6) -> luma [40..56) x [40..56)
						const ActiveRect wgInteriorCoded{ 40 / subX, 40 / subY, 56 / subX, 56 / subY };
						const double inFgErr = RmsDiffRect(degCurCb, masterCurCb, wgInteriorCoded);
						const double outFgErr = RmsDiffRect(gpuRes.outputCb, masterCurCb, wgInteriorCoded);

						// Mandatory Same-Input Negative Control: using the workgroup corner's mv=(0,0) on those interior
						// cells samples unaligned ref(p + 0) instead of ref(p + (+5, -5)), failing recovery (> 14 codes)!
						const CellMotionField wrongZeroMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
						ChromaProcessInput wrongIn = in;
						wrongIn.seq = static_cast<uint64_t>(650 + si);
						wrongIn.prevPeak.lumaMotion = &wrongZeroMf;
						wrongIn.nextPeak.lumaMotion = &wrongZeroMf;
						ChromaGpuGuard wrongGuard(ctx.gpu, cfg);
						const ChromaGpuProcessResult wrongRes = wrongGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, wrongIn, nullptr, nullptr);
						const double wrongFgErr = RmsDiffRect(wrongRes.outputCb, masterCurCb, wgInteriorCoded);

						Assert::IsTrue(outFgErr < inFgErr * 0.50 && wrongFgErr > outFgErr * 2.5 && wrongFgErr > 14.0,
							FmtMsg("cellPx=8 interior (+10,-10) cells must fuse cleanly without LDS OOB: out=%.4f in=%.4f wrongZeroMv=%.2f",
								outFgErr, inFgErr, wrongFgErr).c_str());
					}
				}
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaUnidentifiedArenaBackedWithMismatched420HintSucceedsOnYuv444AndRgb444)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuChromaUnidentifiedArenaBackedWithMismatched420HintSucceedsOnYuv444AndRgb444)
		{
			WarpContext ctx;
			const Config cfg = MakePreset(Preset::Reference, Mode::On);
			const YCbCrMatrixSpec mat = YCbCrMatrixSpec::Bt709();
			const double midCode = 512.0;

			const int lumaW = 96;
			const int lumaH = 96;
			const int cw = lumaW / kCanonicalBlockPx;
			const int ch = lumaH / kCanonicalBlockPx;
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);
			const SitingSpec siting = SitingSpec::Left();
			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
			Assert::IsNotNull(fmtRG32);
			Assert::IsNotNull(fmtRGBA32);

			// Verify OPUS-M1 VRAM estimation & ArenaRequests sizing invariants when codedSubsampling = Yuv420 hint + Unidentified:
			const uint64_t estUnidYuv444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
				lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444,
				kCanonicalBlockPx, ChromaGridStatus::Unidentified);
			const uint64_t estUnidRgb444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
				lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444,
				kCanonicalBlockPx, ChromaGridStatus::Unidentified);
			const uint64_t estUnidAgnostic = TdV2ChromaGpuEngine::EstimatedScratchVramBytes(
				lumaW, lumaH, ChromaSubsampling::Yuv420, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
			const uint64_t estNat444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
				lumaW, lumaH, ChromaSubsampling::Yuv444, DeliveredChromaFormat::Yuv444,
				kCanonicalBlockPx, ChromaGridStatus::Identified);
			const uint64_t estIdRgb444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
				lumaW, lumaH, ChromaSubsampling::Yuv444, DeliveredChromaFormat::Rgb444,
				kCanonicalBlockPx, ChromaGridStatus::Identified);
			const uint64_t estId420HintOn444 = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
				lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444,
				kCanonicalBlockPx, ChromaGridStatus::Identified);

			Assert::AreEqual(estNat444, estUnidYuv444,
				L"OPUS-M1: Unidentified Yuv420-hint + Yuv444 delivery must size full 4:4:4 native scratch (0 reduction textures)");
			Assert::AreEqual(estIdRgb444, estUnidRgb444,
				L"OPUS-M1: Unidentified Yuv420-hint + Rgb444 delivery must size full 4:4:4 RGB scratch (+3 reduction textures)");
			Assert::AreEqual(static_cast<uint64_t>(3ULL * static_cast<uint64_t>(lumaW) * static_cast<uint64_t>(lumaH) * 8ULL),
				estUnidRgb444 - estUnidYuv444,
				L"OPUS-M1: Rgb444 vs Yuv444 Unidentified scratch must differ by exact 3 full-res RG32F reduction textures");
			const uint64_t codedPoolPerPx6 =
				6ULL * static_cast<uint64_t>(VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RG32F)) +
				6ULL * static_cast<uint64_t>(VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RGBA32F)) +
				2ULL * static_cast<uint64_t>(VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::R16));
			const uint64_t coded420And422Bytes = codedPoolPerPx6 * (
				static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaW, 2)) * static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaH, 2)) +
				static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaW, 2)) * static_cast<uint64_t>(lumaH));
			Assert::AreEqual(estUnidRgb444 + coded420And422Bytes, estUnidAgnostic,
				L"OPUS-M1: format-agnostic Unidentified EstimatedScratchVramBytes must equal deduplicated {4:2:0, 4:2:2, 4:4:4+reduce} union + 1 cell-grid set");
			Assert::IsTrue(estUnidYuv444 > estId420HintOn444,
				L"OPUS-M1: Unidentified full-grid 4:4:4 scratch must strictly exceed Identified 4:2:0-hint scratch");

			const auto agnosticUnidReqs = TdV2ChromaGpuEngine::ArenaRequests(
				ctx.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
			const VpSkeleton::ArenaPlan agnosticUnidPlan = VpSkeleton::PlanArena(agnosticUnidReqs, VpSkeleton::Tier::T1);
			Assert::AreEqual(estUnidAgnostic, agnosticUnidPlan.bytes,
				L"OPUS-M1: format-agnostic Unidentified ArenaRequests must match EstimatedScratchVramBytes");
			Assert::AreEqual(static_cast<size_t>(13u), agnosticUnidReqs.size(),
				L"OPUS-M1: 96x96 Unidentified agnostic ArenaRequests must emit 3*3 coded + 4 cell = 13 unique shape requests");
			Assert::AreEqual(agnosticUnidReqs.size(), agnosticUnidPlan.pools.size(),
				L"OPUS-M1: 96x96 Unidentified agnostic ArenaRequests must have zero duplicate TexShape entries");
			for (size_t a = 0; a < agnosticUnidReqs.size(); ++a)
			{
				for (size_t b = a + 1; b < agnosticUnidReqs.size(); ++b)
					Assert::IsTrue(agnosticUnidReqs[a].shape != agnosticUnidReqs[b].shape);
			}

			// Verify lumaW == 1 / lumaH == 1 coded-grid collision deduplication (zero duplicate TexShape entries when dims collapse):
			const int collapseCases[2][2] = { { 1, lumaH }, { lumaW, 1 } };
			for (int ci = 0; ci < 2; ++ci)
			{
				const int cw1 = collapseCases[ci][0];
				const int ch1 = collapseCases[ci][1];
				const auto colReqs = TdV2ChromaGpuEngine::ArenaRequests(
					ctx.gpu, cw1, ch1, ChromaSubsampling::Yuv420, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				const VpSkeleton::ArenaPlan colPlan = VpSkeleton::PlanArena(colReqs, VpSkeleton::Tier::T1);
				const uint64_t colEst = TdV2ChromaGpuEngine::EstimatedScratchVramBytes(
					cw1, ch1, ChromaSubsampling::Yuv420, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				Assert::AreEqual(colEst, colPlan.bytes,
					L"OPUS-M1: collapsed-dim EstimatedScratchVramBytes must match PlanArena bytes");
				Assert::AreEqual(static_cast<size_t>(10u), colReqs.size(),
					L"OPUS-M1: lumaW==1 or lumaH==1 must deduplicate 3 coded grids into 2 (6 coded + 4 cell = 10 requests)");
				Assert::AreEqual(colReqs.size(), colPlan.pools.size(),
					L"OPUS-M1: collapsed-dim ArenaRequests must contain zero duplicate TexShape entries");
				for (size_t a = 0; a < colReqs.size(); ++a)
				{
					for (size_t b = a + 1; b < colReqs.size(); ++b)
					{
						Assert::IsTrue(colReqs[a].shape != colReqs[b].shape,
							L"OPUS-M1: no duplicate TexShape entries allowed when lumaW==1 or lumaH==1 collapses coded dims");
					}
				}
				if (ci == 0)
				{
					const VpSkeleton::ArenaPool* pMerged = colPlan.FindPool(
						VpSkeleton::TexShape{ 1u, static_cast<uint32_t>(lumaH), VpSkeleton::TexFormat::RG32F });
					Assert::IsNotNull(pMerged);
					Assert::AreEqual(static_cast<uint32_t>(9u), pMerged->capacity,
						L"OPUS-M1: lumaW==1 must merge Yuv422(6) + Yuv444(9) into max(6,9)=9 RG32F (never 15)");
				}
				else
				{
					const VpSkeleton::ArenaPool* pMerged = colPlan.FindPool(
						VpSkeleton::TexShape{ static_cast<uint32_t>(ChromaDetail::SubsampledDim(lumaW, 2)), 1u, VpSkeleton::TexFormat::RG32F });
					Assert::IsNotNull(pMerged);
					Assert::AreEqual(static_cast<uint32_t>(6u), pMerged->capacity,
						L"OPUS-M1: lumaH==1 must merge Yuv420(6) + Yuv422(6) into max(6,6)=6 RG32F (never 12)");
				}
			}
			{
				const auto reqs1x1 = TdV2ChromaGpuEngine::ArenaRequests(
					ctx.gpu, 1, 1, ChromaSubsampling::Yuv420, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				const VpSkeleton::ArenaPlan plan1x1 = VpSkeleton::PlanArena(reqs1x1, VpSkeleton::Tier::T1);
				const uint64_t est1x1 = TdV2ChromaGpuEngine::EstimatedScratchVramBytes(
					1, 1, ChromaSubsampling::Yuv420, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				const uint64_t est1x1Rgb = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
					1, 1, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				Assert::AreEqual(est1x1, plan1x1.bytes,
					L"OPUS-M1: 1x1 EstimatedScratchVramBytes must match PlanArena bytes");
				Assert::AreEqual(est1x1Rgb, est1x1,
					L"OPUS-M1: 1x1 Unidentified agnostic scratch must deduplicate all 3 coded grids into 1 coded-grid set");
				Assert::AreEqual(static_cast<size_t>(7u), reqs1x1.size(),
					L"OPUS-M1: 1x1 Unidentified agnostic ArenaRequests must emit 3 coded + 4 cell = 7 requests");
				const VpSkeleton::ArenaPool* p1x1Rg = plan1x1.FindPool(
					VpSkeleton::TexShape{ 1u, 1u, VpSkeleton::TexFormat::RG32F });
				Assert::IsNotNull(p1x1Rg);
				Assert::AreEqual(static_cast<uint32_t>(10u), p1x1Rg->capacity,
					L"OPUS-M1: 1x1 RG32F pool must hold 9 coded + 1 cell warpDiag = 10 textures (never 6+6+9+1=22)");
			}

			VpSkeleton::PlTexBackend backend(ctx.gpu);
			const DeliveredChromaFormat delivFormats[2] = {
				DeliveredChromaFormat::Yuv444,
				DeliveredChromaFormat::Rgb444
			};

			for (int fi = 0; fi < 2; ++fi)
			{
				const DeliveredChromaFormat delivFmt = delivFormats[fi];
				const bool isRgb = (delivFmt == DeliveredChromaFormat::Rgb444);
				const int dSubX = DeliveredSubsampleX(delivFmt);
				const int dSubY = DeliveredSubsampleY(delivFmt);
				const int dW = ChromaDetail::SubsampledDim(lumaW, dSubX);
				const int dH = ChromaDetail::SubsampledDim(lumaH, dSubY);
				const ActiveRect dFull = ActiveRect::Full(dW, dH);
				const ActiveRect dInterior{ dW / 4, 20, (3 * dW) / 4, dH - 20 };

				TdV2Plane masterLowCb(dW, dH, 512.0), masterLowCr(dW, dH, 512.0);
				TdV2Plane curCb(dW, dH, 512.0), curCr(dW, dH, 512.0);
				TdV2Plane lumaY(lumaW, lumaH, 500.0);

				for (int y = 0; y < dH; ++y)
				{
					const double wy = (y >= 8 && y < dH - 8)
						? (0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(y - 8) / static_cast<double>(dH - 17))))
						: 0.0;
					for (int x = 0; x < dW; ++x)
					{
						const double wx = (x >= 8 && x < dW - 8)
							? (0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(x - 8) / static_cast<double>(dW - 17))))
							: 0.0;
						const double win = wx * wy;
						const double lowCb = 478.0 + 0.65 * static_cast<double>(x) + 0.22 * static_cast<double>(y);
						const double lowCr = 546.0 - 0.60 * static_cast<double>(x) - 0.20 * static_cast<double>(y);
						masterLowCb.at(x, y) = lowCb;
						masterLowCr.at(x, y) = lowCr;

						const double bandCb = std::nearbyint(lowCb / 20.0) * 20.0;
						const double bandCr = std::nearbyint(lowCr / 20.0) * 20.0;

						// Real 4:4:4 high-frequency chroma above 4:2:0 Nyquist (present ONLY in cur, absent in peaks!)
						const double hfWave =
							std::cos(2.0 * kPi * static_cast<double>(x) / 2.4) *
							std::sin(2.0 * kPi * static_cast<double>(y) / 2.4);
						curCb.at(x, y) = bandCb + 28.0 * win * hfWave;
						curCr.at(x, y) = bandCr - 16.0 * win * hfWave;
					}
				}

				TdV2Plane prevCb = masterLowCb, prevCr = masterLowCr;
				TdV2Plane nextCb = masterLowCb, nextCr = masterLowCr;
				{
					std::mt19937 rP(9701u + static_cast<uint32_t>(fi)), rN(9711u + static_cast<uint32_t>(fi));
					std::normal_distribution<double> nd(0.0, 0.35);
					for (size_t i = 0; i < prevCb.v.size(); ++i)
					{
						prevCb.v[i] += nd(rP); prevCr.v[i] += nd(rP);
						nextCb.v[i] += nd(rN); nextCr.v[i] += nd(rN);
					}
				}

				ChromaProcessInput cIn;
				cIn.seq = static_cast<uint64_t>(700 + fi);
				cIn.lumaW = lumaW;
				cIn.lumaH = lumaH;
				cIn.deliveredFormat = delivFmt;
				cIn.codedSubsampling = ChromaSubsampling::Yuv420; // Mismatched 4:2:0 hint with Unidentified 4:4:4 / RGB!
				cIn.gridStatus = ChromaGridStatus::Unidentified;
				cIn.siting = siting;
				cIn.matrix = mat;
				cIn.activeRect = lumaFull;
				cIn.hasCleanSplit = true;
				cIn.cleanCb = curCb;
				cIn.cleanCr = curCr;
				cIn.prevPeak = { &prevCb, &prevCr, &stillMf, false };
				cIn.nextPeak = { &nextCb, &nextCr, &stillMf, false };
				cIn.hasStepPrior = true;
				cIn.estimatedStepDelta = 24.0;
				cIn.deadZonePhi = kDeadZonePhiInter;
				cIn.zeroCoeffFraction = 0.35;

				if (isRgb)
				{
					cIn.receivedR = TdV2Plane(dW, dH, 0.0);
					cIn.receivedG = TdV2Plane(dW, dH, 0.0);
					cIn.receivedB = TdV2Plane(dW, dH, 0.0);
					const double kR = 2.0 * (1.0 - mat.kr);
					const double kB = 2.0 * (1.0 - mat.kb);
					for (size_t i = 0; i < curCb.v.size(); ++i)
					{
						const double yv = lumaY.v[i];
						const double cb0 = curCb.v[i] - midCode;
						const double cr0 = curCr.v[i] - midCode;
						cIn.receivedR.v[i] = yv + kR * cr0;
						cIn.receivedB.v[i] = yv + kB * cb0;
						cIn.receivedG.v[i] = yv - (mat.kr * kR / mat.kg) * cr0 - (mat.kb * kB / mat.kg) * cb0;
					}
				}
				else
				{
					cIn.receivedCb = curCb;
					cIn.receivedCr = curCr;
				}

				// 1a. Plan real ScratchArena<PlTexBackend> via ArenaRequests(..., Yuv420, delivFmt, cellPx, Unidentified):
				const uint64_t expectedBytes = TdV2ChromaGpuEngine::EstimatedScratchVramBytesFor(
					lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				const std::vector<VpSkeleton::ArenaRequest> unidReqs = TdV2ChromaGpuEngine::ArenaRequests(
					ctx.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, kCanonicalBlockPx, ChromaGridStatus::Unidentified);
				const auto unidPlan = VpSkeleton::PlanArena(unidReqs, VpSkeleton::Tier::T1);
				Assert::AreEqual(expectedBytes, unidPlan.bytes,
					L"OPUS-M1: PlanArena bytes must match EstimatedScratchVramBytesFor for Unidentified format");

				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> unidArena(backend);
				Assert::IsTrue(unidArena.Reserve(unidPlan),
					L"OPUS-M1: ScratchArena Reserve must succeed for Unidentified full-grid plan");

				TdV2ChromaEngine cpuEng(cfg);
				ChromaGpuGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry siCpu, siGpu;
				unidArena.BeginFrame();
				const ChromaProcessResult cpuRes = cpuEng.ProcessFrame(cIn, &siCpu);
				const ChromaGpuProcessResult gpuRes =
					gpuGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &unidArena, &siGpu);

				Assert::IsTrue(gpuRes.succeeded && !gpuRes.scratchShort && !gpuGuard.engine.ScratchShort(),
					L"OPUS-M1: Unidentified arena-backed ProcessChromaPlanes with Yuv420 hint must succeed cleanly");
				Assert::IsTrue(gpuRes.coarseBandOnly && cpuRes.coarseBandOnly,
					L"OPUS-M1: Unidentified arena-backed run must set coarseBandOnly == true on CPU and GPU");
				Assert::IsTrue(siGpu.IsActive(StandInKind::ChromaGridDetector) && siCpu.IsActive(StandInKind::ChromaGridDetector),
					L"OPUS-M1: Unidentified arena-backed run must visibly engage StandInKind::ChromaGridDetector");
				Assert::IsFalse(siGpu.IsActive(StandInKind::GpuScratchShortage),
					L"OPUS-M1: Unidentified arena-backed run must NOT engage GpuScratchShortage");
				Assert::IsFalse(siGpu.IsActive(StandInKind::GpuPrivateScratchFallback),
					L"OPUS-M1: Unidentified arena-backed run must NOT fall back to private scratch");
				Assert::AreEqual(static_cast<uint64_t>(0u), unidArena.NestedScopes(),
					L"OPUS-M1: Unidentified arena scope must close cleanly");
				Assert::AreEqual(dW, gpuRes.codedW, L"OPUS-M1: Unidentified codedW must match full delivered width");
				Assert::AreEqual(dH, gpuRes.codedH, L"OPUS-M1: Unidentified codedH must match full delivered height");

				// Exact < 1e-2 CPU Branch-A coarse-only parity across deltas and outputs:
				Assert::IsTrue(MaxAbsDiffRect(cpuRes.deliveredDeltaCb, gpuRes.deliveredDeltaCb, dFull) < 1e-2 &&
				               MaxAbsDiffRect(cpuRes.deliveredDeltaCr, gpuRes.deliveredDeltaCr, dFull) < 1e-2 &&
				               MaxAbsDiffRect(cpuRes.codedDeltaCb, gpuRes.codedDeltaCb, dFull) < 1e-2 &&
				               MaxAbsDiffRect(cpuRes.codedDeltaCr, gpuRes.codedDeltaCr, dFull) < 1e-2,
					FmtMsg("OPUS-M1 fmt=%.0f CPU/GPU coarse-only delta parity failed: dCb=%.6f dCr=%.6f",
						static_cast<double>(fi),
						MaxAbsDiffRect(cpuRes.deliveredDeltaCb, gpuRes.deliveredDeltaCb, dFull),
						MaxAbsDiffRect(cpuRes.deliveredDeltaCr, gpuRes.deliveredDeltaCr, dFull)).c_str());

				TdV2Plane gpuOutCb(dW, dH, 0.0);
				if (isRgb)
				{
					Assert::IsTrue(MaxAbsDiffRect(cpuRes.outputR, gpuRes.outputR, dFull) < 1e-2 &&
					               MaxAbsDiffRect(cpuRes.outputG, gpuRes.outputG, dFull) < 1e-2 &&
					               MaxAbsDiffRect(cpuRes.outputB, gpuRes.outputB, dFull) < 1e-2,
						L"OPUS-M1: Unidentified arena-backed Rgb444 output RGB must match CPU Branch-A within < 1e-2 codes");
					for (size_t i = 0; i < gpuOutCb.v.size(); ++i)
					{
						double yv = 0.0, cbv = 0.0, crv = 0.0;
						mat.RgbToYCbCr(gpuRes.outputR.v[i], gpuRes.outputG.v[i], gpuRes.outputB.v[i], midCode, yv, cbv, crv);
						gpuOutCb.v[i] = cbv;
					}
				}
				else
				{
					Assert::IsTrue(MaxAbsDiffRect(cpuRes.outputCb, gpuRes.outputCb, dFull) < 1e-2 &&
					               MaxAbsDiffRect(cpuRes.outputCr, gpuRes.outputCr, dFull) < 1e-2,
						L"OPUS-M1: Unidentified arena-backed Yuv444 output Cb/Cr must match CPU Branch-A within < 1e-2 codes");
					gpuOutCb = gpuRes.outputCb;
				}

				// Zero HF destruction vs naive full-band temporal average negative control, plus genuine coarse error reduction:
				const ThreeBandPlanes curBandsCb = DecomposeActiveBands(curCb, dFull);
				const ThreeBandPlanes outBandsCb = DecomposeActiveBands(gpuOutCb, dFull);
				TdV2Plane naiveFullCb(dW, dH, 0.0);
				for (size_t i = 0; i < curCb.v.size(); ++i)
					naiveFullCb.v[i] = (curCb.v[i] + prevCb.v[i] + nextCb.v[i]) / 3.0;
				const ThreeBandPlanes naiveBandsCb = DecomposeActiveBands(naiveFullCb, dFull);
				const double gpuHfLoss = RmsDiffRect(outBandsCb.H, curBandsCb.H, dInterior);
				const double naiveHfLoss = RmsDiffRect(naiveBandsCb.H, curBandsCb.H, dInterior);
				Assert::IsTrue(gpuHfLoss < 5e-2 && naiveHfLoss > 5.0,
					FmtMsg("OPUS-M1 fmt=%.0f must preserve real HF chroma (gpuLoss=%.5f vs naiveLoss=%.3f)",
						static_cast<double>(fi), gpuHfLoss, naiveHfLoss).c_str());

				double dummyGain = 1.0;
				const double sigX = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubX);
				const double sigY = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubY);
				const TdV2Plane refCoarse = ChromaDetail::BlurActiveAnisotropic(masterLowCb, dFull, sigX, sigY, dummyGain);
				const TdV2Plane inCoarse  = ChromaDetail::BlurActiveAnisotropic(curCb, dFull, sigX, sigY, dummyGain);
				const TdV2Plane outCoarse = ChromaDetail::BlurActiveAnisotropic(gpuOutCb, dFull, sigX, sigY, dummyGain);
				Assert::IsTrue(RmsDiffRect(outCoarse, refCoarse, dInterior) < RmsDiffRect(inCoarse, refCoarse, dInterior) * 0.70,
					L"OPUS-M1: Unidentified arena-backed coarse-only mode must genuinely reduce coarse banding error");

				// 1b. Also verify direct zero-copy ProcessChromaGpu on the exact same arena-backed inputs:
				OwnedTex recUvTex(ctx.gpu), recRgbTex(ctx.gpu), cleanUvTex(ctx.gpu), prevUvTex(ctx.gpu), nextUvTex(ctx.gpu);
				OwnedTex outCodedDelta(ctx.gpu), outDelivDelta(ctx.gpu), outDelivUv(ctx.gpu), outDelivRgb(ctx.gpu);
				auto uploadUv = [&](OwnedTex& dst, const TdV2Plane& cb, const TdV2Plane& cr)
				{
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, dst.tex, dW, dH, fmtRG32, true, false, true, false));
					std::vector<float> buf(cb.v.size() * 2u, 0.0f);
					for (size_t i = 0; i < cb.v.size(); ++i)
					{
						buf[i * 2u + 0u] = static_cast<float>(cb.v[i]);
						buf[i * 2u + 1u] = static_cast<float>(cr.v[i]);
					}
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, dst.tex, buf.data()));
				};
				uploadUv(cleanUvTex, curCb, curCr);
				uploadUv(prevUvTex, prevCb, prevCr);
				uploadUv(nextUvTex, nextCb, nextCr);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCodedDelta.tex, dW, dH, fmtRG32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDelivDelta.tex, dW, dH, fmtRG32, true, true, false, true));
				if (isRgb)
				{
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recRgbTex.tex, dW, dH, fmtRGBA32, true, false, true, false));
					std::vector<float> rgbBuf(cIn.receivedR.v.size() * 4u, 0.0f);
					for (size_t i = 0; i < cIn.receivedR.v.size(); ++i)
					{
						rgbBuf[i * 4u + 0u] = static_cast<float>(cIn.receivedR.v[i]);
						rgbBuf[i * 4u + 1u] = static_cast<float>(cIn.receivedG.v[i]);
						rgbBuf[i * 4u + 2u] = static_cast<float>(cIn.receivedB.v[i]);
					}
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, recRgbTex.tex, rgbBuf.data()));
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDelivRgb.tex, dW, dH, fmtRGBA32, true, true, false, true));
				}
				else
				{
					uploadUv(recUvTex, curCb, curCr);
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDelivUv.tex, dW, dH, fmtRGBA32, true, true, false, true));
				}

				ChromaGpuInput zcIn{};
				zcIn.seq = static_cast<uint64_t>(710 + fi);
				zcIn.lumaW = lumaW;
				zcIn.lumaH = lumaH;
				zcIn.deliveredFormat = delivFmt;
				zcIn.codedSubsampling = ChromaSubsampling::Yuv420;
				zcIn.gridStatus = ChromaGridStatus::Unidentified;
				zcIn.siting = siting;
				zcIn.matrix = mat;
				zcIn.activeRect = lumaFull;
				zcIn.receivedTex = isRgb ? recRgbTex.tex : recUvTex.tex;
				zcIn.cleanUvTex = cleanUvTex.tex;
				zcIn.hasCleanSplit = true;
				zcIn.prevPeak = { prevUvTex.tex, &stillMf, false };
				zcIn.nextPeak = { nextUvTex.tex, &stillMf, false };
				zcIn.hasStepPrior = true;
				zcIn.estimatedStepDelta = 24.0;
				zcIn.deadZonePhi = kDeadZonePhiInter;
				zcIn.zeroCoeffFraction = 0.35;

				ChromaGpuOutput zcOut{};
				zcOut.outCodedDeltaTex = outCodedDelta.tex;
				zcOut.outDeliveredDeltaTex = outDelivDelta.tex;
				zcOut.outDeliveredUvTex = isRgb ? nullptr : outDelivUv.tex;
				zcOut.outDeliveredRgbTex = isRgb ? outDelivRgb.tex : nullptr;

				ChromaGpuGuard zcGuard(ctx.gpu, cfg);
				StandInRegistry siZc;
				unidArena.BeginFrame();
				const bool okZc = zcGuard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, zcIn, zcOut, &unidArena, &siZc);
				Assert::IsTrue(okZc && zcOut.succeeded && !zcOut.scratchShort && !zcGuard.engine.ScratchShort(),
					L"OPUS-M1: zero-copy ProcessChromaGpu on Unidentified arena must succeed cleanly");
				Assert::IsTrue(zcOut.coarseBandOnly, L"OPUS-M1: zero-copy ProcessChromaGpu must set coarseBandOnly == true");
				Assert::AreEqual(dW, zcOut.codedW);
				Assert::AreEqual(dH, zcOut.codedH);
				Assert::IsTrue(siZc.IsActive(StandInKind::ChromaGridDetector));
				Assert::IsFalse(siZc.IsActive(StandInKind::GpuScratchShortage));
				Assert::IsFalse(siZc.IsActive(StandInKind::GpuPrivateScratchFallback));
				Assert::AreEqual(static_cast<uint64_t>(0u), unidArena.NestedScopes());

				std::vector<float> zcDelivDelta(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
				Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, outDelivDelta.tex, zcDelivDelta.data()));
				for (size_t i = 0; i < cpuRes.deliveredDeltaCb.v.size(); ++i)
				{
					Assert::IsTrue(std::fabs(static_cast<double>(zcDelivDelta[i * 2u + 0u]) - cpuRes.deliveredDeltaCb.v[i]) < 1e-2 &&
					               std::fabs(static_cast<double>(zcDelivDelta[i * 2u + 1u]) - cpuRes.deliveredDeltaCr.v[i]) < 1e-2,
						L"OPUS-M1: zero-copy ProcessChromaGpu deliveredDelta must match CPU Branch-A within < 1e-2 codes");
				}

				// 2. Mandatory Same-Input Negative Control:
				// Planning an arena with gridStatus = ChromaGridStatus::Identified (Yuv420 hint -> 48x48 scratch) and passing
				// the exact same Unidentified 4:4:4 / RGB input MUST fail visibly and engage StandInKind::GpuScratchShortage!
				const std::vector<VpSkeleton::ArenaRequest> idReqs = TdV2ChromaGpuEngine::ArenaRequests(
					ctx.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, delivFmt, kCanonicalBlockPx, ChromaGridStatus::Identified);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> idArena(backend);
				Assert::IsTrue(idArena.Reserve(VpSkeleton::PlanArena(idReqs, VpSkeleton::Tier::T1)));

				ChromaGpuGuard shortPlanesGuard(ctx.gpu, cfg);
				StandInRegistry siShortPlanes;
				idArena.BeginFrame();
				const ChromaGpuProcessResult rShort =
					shortPlanesGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &idArena, &siShortPlanes);
				Assert::IsFalse(rShort.succeeded,
					L"Negative control: Identified Yuv420-sized arena must fail visibly on Unidentified 4:4:4 / RGB");
				Assert::IsTrue(rShort.scratchShort && shortPlanesGuard.engine.ScratchShort(),
					L"Negative control: ProcessChromaPlanes must report scratchShort == true");
				Assert::IsTrue(siShortPlanes.IsActive(StandInKind::GpuScratchShortage),
					L"Negative control: must visibly engage StandInKind::GpuScratchShortage");
				Assert::IsFalse(siShortPlanes.IsActive(StandInKind::GpuPrivateScratchFallback),
					L"Negative control: zero silent private scratch fallback allowed");
				Assert::AreEqual(static_cast<uint64_t>(0u), idArena.NestedScopes());

				ChromaGpuGuard shortZcGuard(ctx.gpu, cfg);
				ChromaGpuOutput shortZcOut{};
				shortZcOut.outCodedDeltaTex = outCodedDelta.tex;
				shortZcOut.outDeliveredDeltaTex = outDelivDelta.tex;
				shortZcOut.outDeliveredUvTex = isRgb ? nullptr : outDelivUv.tex;
				shortZcOut.outDeliveredRgbTex = isRgb ? outDelivRgb.tex : nullptr;
				StandInRegistry siShortZc;
				idArena.BeginFrame();
				const bool okShortZc =
					shortZcGuard.engine.ProcessChromaGpu(ctx.gpu, ctx.dispatch, zcIn, shortZcOut, &idArena, &siShortZc);
				Assert::IsFalse(okShortZc,
					L"Negative control: ProcessChromaGpu on Identified Yuv420-sized arena must return false");
				Assert::IsFalse(shortZcOut.succeeded,
					L"Negative control: ProcessChromaGpu shortZcOut.succeeded must be false");
				Assert::IsTrue(shortZcOut.scratchShort && shortZcGuard.engine.ScratchShort(),
					L"Negative control: ProcessChromaGpu must set scratchShort == true");
				Assert::IsTrue(siShortZc.IsActive(StandInKind::GpuScratchShortage),
					L"Negative control: ProcessChromaGpu must visibly engage StandInKind::GpuScratchShortage");
				Assert::IsFalse(siShortZc.IsActive(StandInKind::GpuPrivateScratchFallback),
					L"Negative control: zero silent private scratch fallback allowed on ProcessChromaGpu");
				Assert::AreEqual(static_cast<uint64_t>(0u), idArena.NestedScopes());
			}

			// 3. Reserve a real ScratchArena<PlTexBackend> from format-agnostic agnosticUnidReqs and execute
			// ProcessChromaPlanes with gridStatus = ChromaGridStatus::Unidentified across ALL 4 delivered formats
			// (Yuv420, Yuv422, Yuv444, Rgb444) on the shared format-agnostic arena:
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> agnosticUnidArena(backend);
			Assert::IsTrue(agnosticUnidArena.Reserve(agnosticUnidPlan),
				L"OPUS-M1: ScratchArena Reserve must succeed for format-agnostic Unidentified union plan");

			const std::vector<VpSkeleton::ArenaRequest> only444Reqs = TdV2ChromaGpuEngine::ArenaRequests(
				ctx.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444,
				kCanonicalBlockPx, ChromaGridStatus::Unidentified);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> only444Arena(backend);
			Assert::IsTrue(only444Arena.Reserve(VpSkeleton::PlanArena(only444Reqs, VpSkeleton::Tier::T1)),
				L"OPUS-M1: single-shape 4:4:4 negative-control arena Reserve must succeed");

			const DeliveredChromaFormat allFormats[4] = {
				DeliveredChromaFormat::Yuv420,
				DeliveredChromaFormat::Yuv422,
				DeliveredChromaFormat::Yuv444,
				DeliveredChromaFormat::Rgb444
			};
			for (int af = 0; af < 4; ++af)
			{
				const DeliveredChromaFormat afFmt = allFormats[af];
				const bool afIsRgb = (afFmt == DeliveredChromaFormat::Rgb444);
				const int afSubX = DeliveredSubsampleX(afFmt);
				const int afSubY = DeliveredSubsampleY(afFmt);
				const int afW = ChromaDetail::SubsampledDim(lumaW, afSubX);
				const int afH = ChromaDetail::SubsampledDim(lumaH, afSubY);
				const ActiveRect afFull = ActiveRect::Full(afW, afH);

				TdV2Plane afMasterCb(afW, afH, 512.0), afMasterCr(afW, afH, 512.0);
				TdV2Plane afCurCb(afW, afH, 512.0), afCurCr(afW, afH, 512.0);
				TdV2Plane afLumaY(lumaW, lumaH, 500.0);
				for (int y = 0; y < afH; ++y)
				{
					for (int x = 0; x < afW; ++x)
					{
						const double lowCb = 480.0 + 0.60 * static_cast<double>(x * afSubX) + 0.20 * static_cast<double>(y * afSubY);
						const double lowCr = 544.0 - 0.55 * static_cast<double>(x * afSubX) - 0.18 * static_cast<double>(y * afSubY);
						afMasterCb.at(x, y) = lowCb;
						afMasterCr.at(x, y) = lowCr;
						afCurCb.at(x, y) = std::nearbyint(lowCb / 20.0) * 20.0;
						afCurCr.at(x, y) = std::nearbyint(lowCr / 20.0) * 20.0;
					}
				}

				TdV2Plane afPrevCb = afMasterCb, afPrevCr = afMasterCr;
				TdV2Plane afNextCb = afMasterCb, afNextCr = afMasterCr;
				{
					std::mt19937 rP(9801u + static_cast<uint32_t>(af)), rN(9811u + static_cast<uint32_t>(af));
					std::normal_distribution<double> nd(0.0, 0.35);
					for (size_t i = 0; i < afPrevCb.v.size(); ++i)
					{
						afPrevCb.v[i] += nd(rP); afPrevCr.v[i] += nd(rP);
						afNextCb.v[i] += nd(rN); afNextCr.v[i] += nd(rN);
					}
				}

				ChromaProcessInput afIn;
				afIn.seq = static_cast<uint64_t>(750 + af);
				afIn.lumaW = lumaW;
				afIn.lumaH = lumaH;
				afIn.deliveredFormat = afFmt;
				afIn.codedSubsampling = ChromaSubsampling::Yuv420;
				afIn.gridStatus = ChromaGridStatus::Unidentified;
				afIn.siting = siting;
				afIn.matrix = mat;
				afIn.activeRect = lumaFull;
				afIn.hasCleanSplit = true;
				afIn.cleanCb = afCurCb;
				afIn.cleanCr = afCurCr;
				afIn.prevPeak = { &afPrevCb, &afPrevCr, &stillMf, false };
				afIn.nextPeak = { &afNextCb, &afNextCr, &stillMf, false };
				afIn.hasStepPrior = true;
				afIn.estimatedStepDelta = 24.0;
				afIn.deadZonePhi = kDeadZonePhiInter;
				afIn.zeroCoeffFraction = 0.35;

				if (afIsRgb)
				{
					afIn.receivedR = TdV2Plane(afW, afH, 0.0);
					afIn.receivedG = TdV2Plane(afW, afH, 0.0);
					afIn.receivedB = TdV2Plane(afW, afH, 0.0);
					const double kR = 2.0 * (1.0 - mat.kr);
					const double kB = 2.0 * (1.0 - mat.kb);
					for (size_t i = 0; i < afCurCb.v.size(); ++i)
					{
						const double yv = afLumaY.v[i];
						const double cb0 = afCurCb.v[i] - midCode;
						const double cr0 = afCurCr.v[i] - midCode;
						afIn.receivedR.v[i] = yv + kR * cr0;
						afIn.receivedB.v[i] = yv + kB * cb0;
						afIn.receivedG.v[i] = yv - (mat.kr * kR / mat.kg) * cr0 - (mat.kb * kB / mat.kg) * cb0;
					}
				}
				else
				{
					afIn.receivedCb = afCurCb;
					afIn.receivedCr = afCurCr;
				}

				TdV2ChromaEngine afCpuEng(cfg);
				ChromaGpuGuard afGpuGuard(ctx.gpu, cfg);
				StandInRegistry siAfCpu, siAfGpu;
				agnosticUnidArena.BeginFrame();
				const ChromaProcessResult afCpuRes = afCpuEng.ProcessFrame(afIn, &siAfCpu);
				const ChromaGpuProcessResult afGpuRes =
					afGpuGuard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, afIn, &agnosticUnidArena, &siAfGpu);

				Assert::IsTrue(afGpuRes.succeeded && !afGpuRes.scratchShort && !afGpuGuard.engine.ScratchShort(),
					FmtMsg("OPUS-M1 agnosticUnidArena ProcessChromaPlanes must succeed on fmt=%.0f", static_cast<double>(af)).c_str());
				Assert::IsTrue(afGpuRes.coarseBandOnly && afCpuRes.coarseBandOnly,
					L"OPUS-M1: agnosticUnidArena run must set coarseBandOnly == true on every delivered format");
				Assert::IsTrue(siAfGpu.IsActive(StandInKind::ChromaGridDetector) && siAfCpu.IsActive(StandInKind::ChromaGridDetector),
					L"OPUS-M1: agnosticUnidArena run must visibly engage StandInKind::ChromaGridDetector on every format");
				Assert::IsFalse(siAfGpu.IsActive(StandInKind::GpuScratchShortage),
					L"OPUS-M1: agnosticUnidArena run must NOT engage StandInKind::GpuScratchShortage on any format");
				Assert::IsFalse(siAfGpu.IsActive(StandInKind::GpuPrivateScratchFallback),
					L"OPUS-M1: agnosticUnidArena run must NOT engage StandInKind::GpuPrivateScratchFallback on any format");
				Assert::AreEqual(static_cast<uint64_t>(0u), agnosticUnidArena.NestedScopes(),
					L"OPUS-M1: agnosticUnidArena scope must close cleanly on every format");
				Assert::AreEqual(afW, afGpuRes.codedW, L"OPUS-M1: Unidentified codedW must match delivered width on every format");
				Assert::AreEqual(afH, afGpuRes.codedH, L"OPUS-M1: Unidentified codedH must match delivered height on every format");

				Assert::IsTrue(MaxAbsDiffRect(afCpuRes.deliveredDeltaCb, afGpuRes.deliveredDeltaCb, afFull) < 1e-2 &&
				               MaxAbsDiffRect(afCpuRes.deliveredDeltaCr, afGpuRes.deliveredDeltaCr, afFull) < 1e-2 &&
				               MaxAbsDiffRect(afCpuRes.codedDeltaCb, afGpuRes.codedDeltaCb, afFull) < 1e-2 &&
				               MaxAbsDiffRect(afCpuRes.codedDeltaCr, afGpuRes.codedDeltaCr, afFull) < 1e-2,
					FmtMsg("OPUS-M1 agnosticUnidArena fmt=%.0f CPU/GPU Branch-A parity failed", static_cast<double>(af)).c_str());
				if (afIsRgb)
				{
					Assert::IsTrue(MaxAbsDiffRect(afCpuRes.outputR, afGpuRes.outputR, afFull) < 1e-2 &&
					               MaxAbsDiffRect(afCpuRes.outputG, afGpuRes.outputG, afFull) < 1e-2 &&
					               MaxAbsDiffRect(afCpuRes.outputB, afGpuRes.outputB, afFull) < 1e-2,
						L"OPUS-M1: agnosticUnidArena Rgb444 output RGB must match CPU Branch-A within < 1e-2 codes");
				}
				else
				{
					Assert::IsTrue(MaxAbsDiffRect(afCpuRes.outputCb, afGpuRes.outputCb, afFull) < 1e-2 &&
					               MaxAbsDiffRect(afCpuRes.outputCr, afGpuRes.outputCr, afFull) < 1e-2,
						L"OPUS-M1: agnosticUnidArena YUV output Cb/Cr must match CPU Branch-A within < 1e-2 codes");
				}

				// Same-input negative control on Yuv420 & Yuv422: sizing an arena ONLY for 4:4:4 (without the 4:2:0/4:2:2 union)
				// fails exact TexShape matching in ScratchArena::Acquire and visibly engages StandInKind::GpuScratchShortage!
				if (af < 2)
				{
					ChromaGpuGuard only444Guard(ctx.gpu, cfg);
					StandInRegistry siOnly444;
					only444Arena.BeginFrame();
					const ChromaGpuProcessResult rOnly444 =
						only444Guard.engine.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, afIn, &only444Arena, &siOnly444);
					Assert::IsFalse(rOnly444.succeeded,
						L"Negative control: 4:4:4-only arena must fail visibly on Unidentified Yuv420 / Yuv422 due to exact TexShape mismatch");
					Assert::IsTrue(rOnly444.scratchShort && only444Guard.engine.ScratchShort(),
						L"Negative control: 4:4:4-only arena on Unidentified Yuv420 / Yuv422 must report scratchShort == true");
					Assert::IsTrue(siOnly444.IsActive(StandInKind::GpuScratchShortage),
						L"Negative control: 4:4:4-only arena on Unidentified Yuv420 / Yuv422 must engage StandInKind::GpuScratchShortage");
					Assert::IsFalse(siOnly444.IsActive(StandInKind::GpuPrivateScratchFallback),
						L"Negative control: zero silent private scratch fallback allowed");
					Assert::AreEqual(static_cast<uint64_t>(0u), only444Arena.NestedScopes());
				}
			}
		}
	};
} // namespace TdV2Sota3ChromaGpuNs
} // namespace Tests
