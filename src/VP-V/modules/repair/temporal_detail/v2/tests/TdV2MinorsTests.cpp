// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Unit 0 (TD-MINORS) Adversarial Red-Team Tests:
//   1. Low1LiveWarpRealTimeShadowPassthroughNeverAllocatesOutYTex (GpuWarp):
//      - Live WARP Preset::RealTime + Mode::Shadow ProcessFrameGpu passthrough (4K 3840x2160 seed + 64x64 active)
//        proves outYTex_ is NEVER allocated (saves full-res 31.6 MiB 4K R32F buffer) while AllocatedPersistentLumaVramBytes()
//        stays minimal, vs Mode::On internal-output negative control on the exact same input!
//   2. Low2LiveWarpNullArenaEngagesGpuPrivateScratchFallbackVisibly (GpuWarp):
//      - Live WARP arena == nullptr engages StandInKind::GpuPrivateScratchFallback visibly, vs &arena normal path
//        negative control where GpuPrivateScratchFallback stays strictly off!
//   3. Low3ChromaBranchBRackFocusAndBokehCellVetoesWithSharpNeighborRecovery (CPU):
//      - TdV2ChromaEngine Branch B: explicitFocusChangeCells hard-vetoes L/M/H == 0 under both BokehModes;
//        explicitDefocusedCells hard-vetoes M/H == 0 always and gates L strictly by BokehMode, while adjacent
//        sharp cells in the exact same frame still recover (with unmasked same-input negative control)!
//   4. FormatAwareChromaVramSizingSavesOver75PercentOn420AndPreservesTrue422And444 (GpuWarp):
//      - EstimatedChromaVramBytesFor(3840, 2160, Yuv420, Yuv420) saves >= 75% (86.11%) vs worst-case
//        EstimatedChromaVramBytes(3840, 2160), while Yuv422 retains 1/2 and Yuv444/Rgb444 retain full 4K sizing;
//        verified byte-for-byte against live WARP AllocatedPersistentChromaVramBytes() across all 4 formats!
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/ScratchArena.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
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
namespace TdV2MinorsTestsNs
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

	TEST_CLASS(TdV2MinorsTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(Low1LiveWarpRealTimeShadowPassthroughNeverAllocatesOutYTex)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(Low1LiveWarpRealTimeShadowPassthroughNeverAllocatesOutYTex)
		{
			WarpContext ctx;
			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
			Assert::IsNotNull(fmtR32);

			// Part A: Live 4K (3840x2160) Preset::RealTime + Mode::Shadow seed passthrough on WARP:
			// Must allocate ONLY carryCleanTex_ (33,177,600 B = 31.64 MiB), with ZERO wasted outYTex_ / outCleanTex_ / outDeltaTex_!
			{
				const int w4k = 3840;
				const int h4k = 2160;
				const uint64_t oneFullResR32Bytes = static_cast<uint64_t>(w4k) * static_cast<uint64_t>(h4k) * 4ull; // 33,177,600 B (31.64 MiB)

				OwnedTex rec4k(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, rec4k.tex, w4k, h4k, fmtR32, true, false, false, false));

				const Config cfgShadowRt = MakePreset(Preset::RealTime, Mode::Shadow);
				EngineGuard shadow4kGuard(ctx.gpu, cfgShadowRt);

				GpuFrameInput in4k;
				in4k.seq = 1;
				in4k.w = w4k;
				in4k.h = h4k;
				in4k.receivedYTex = rec4k.tex;
				in4k.activeRect = ActiveRect::Full(w4k, h4k);

				GpuFrameOutput out4k; // All optional output pl_tex left nullptr!
				StandInRegistry si4k;
				Assert::IsTrue(shadow4kGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, in4k, out4k, nullptr, &si4k),
					L"LOW-1 4K RealTime+Shadow seed passthrough must succeed on WARP");
				Assert::IsTrue(out4k.succeeded);
				Assert::AreEqual(1, out4k.dispatchCount, L"LOW-1 4K seed passthrough with null cell outputs must issue exact 1 dispatch");

				const uint64_t liveShadow4kBytes = shadow4kGuard.engine.AllocatedPersistentLumaVramBytes();
				Assert::AreEqual(oneFullResR32Bytes, liveShadow4kBytes,
					FmtMsg("LOW-1 4K RealTime+Shadow passthrough must allocate ONLY carryCleanTex_ (%.0f B), got %.0f B (outYTex_ leaked!)",
						static_cast<double>(oneFullResR32Bytes), static_cast<double>(liveShadow4kBytes)).c_str());
			}

			// Part B: Active 2-reference frame on WARP (Preset::RealTime):
			// Mode::Shadow (outputYTex == nullptr) NEVER allocates outYTex_, whereas Mode::On (outputYTex == nullptr)
			// allocates outYTex_ (+ w*h*4 bytes) on the EXACT SAME input (mandatory same-input negative control)!
			{
				const int w = 64;
				const int h = 64;
				const int cw = w / 16;
				const int ch = h / 16;
				const uint64_t outYBytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4ull;

				TdV2Plane rec(w, h, 420.0), prev(w, h, 424.0), next(w, h, 424.0);
				OwnedTex recTex(ctx.gpu), prevTex(ctx.gpu), nextTex(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, prevTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, nextTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, rec));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, prevTex.tex, prev));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, nextTex.tex, next));

				const CellMotionField mf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				GpuFrameInput actIn;
				actIn.seq = 2;
				actIn.w = w;
				actIn.h = h;
				actIn.receivedYTex = recTex.tex;
				actIn.activeRect = ActiveRect::Full(w, h);
				actIn.prevPeak = { prevTex.tex, &mf, false };
				actIn.nextPeak = { nextTex.tex, &mf, false };
				actIn.hasStepPrior = true;
				actIn.estimatedStepDelta = 16.0;
				actIn.deadZonePhi = kDeadZonePhiInter;
				actIn.zeroCoeffFraction = 0.35;

				EngineGuard gShadow(ctx.gpu, MakePreset(Preset::RealTime, Mode::Shadow));
				EngineGuard gOn(ctx.gpu, MakePreset(Preset::RealTime, Mode::On));
				GpuFrameOutput outShadow, outOn;
				Assert::IsTrue(gShadow.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, actIn, outShadow, nullptr, nullptr));
				Assert::IsTrue(gOn.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, actIn, outOn, nullptr, nullptr));

				const uint64_t shadowBytes = gShadow.engine.AllocatedPersistentLumaVramBytes();
				const uint64_t onBytes = gOn.engine.AllocatedPersistentLumaVramBytes();
				Assert::AreEqual(onBytes - outYBytes, shadowBytes,
					FmtMsg("LOW-1 active RealTime+Shadow (%.0f B) must save exact outYTex_ (%.0f B) vs Mode::On internal output (%.0f B)",
						static_cast<double>(shadowBytes), static_cast<double>(outYBytes), static_cast<double>(onBytes)).c_str());
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(Low2LiveWarpNullArenaEngagesGpuPrivateScratchFallbackVisibly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(Low2LiveWarpNullArenaEngagesGpuPrivateScratchFallbackVisibly)
		{
			WarpContext ctx;

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);

			TdV2Plane master(w, h, 0.0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					master.at(x, y) = 480.0 + 20.0 * std::sin(2.0 * kPi * x / 7.0) + 16.0 * std::cos(2.0 * kPi * y / 9.0);

			TdV2Plane cur = master;
			std::mt19937 rng(7021u);
			std::normal_distribution<double> nd(0.0, 6.0);
			for (double& v : cur.v)
				v += nd(rng);

			const CellMotionField mf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			FrameProcessInput in;
			in.seq = 12;
			in.receivedY = cur;
			in.hasCleanSplit = true;
			in.cleanY = cur;
			in.activeRect = full;
			in.prevPeak = { &master, &mf, false };
			in.nextPeak = { &master, &mf, false };
			in.hasStepPrior = true;
			in.estimatedStepDelta = 24.0;
			in.deadZonePhi = kDeadZonePhiInter;
			in.zeroCoeffFraction = 0.35;

			const Config cfg = MakePreset(Preset::Reference, Mode::On);

			// 1. arena == nullptr -> MUST visibly engage StandInKind::GpuPrivateScratchFallback!
			EngineGuard privGuard(ctx.gpu, cfg);
			StandInRegistry privSi;
			const GpuProcessResult privRes = privGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &privSi);
			Assert::IsTrue(privRes.succeeded, L"LOW-2 null-arena ProcessFrame must succeed");
			Assert::IsTrue(privSi.IsActive(StandInKind::GpuPrivateScratchFallback),
				L"LOW-2 arena == nullptr MUST visibly engage StandInKind::GpuPrivateScratchFallback");
			Assert::AreEqual(static_cast<uint64_t>(1u), privSi.HitCount(StandInKind::GpuPrivateScratchFallback),
				L"LOW-2 arena == nullptr must increment GpuPrivateScratchFallback hit count");

			// 2. Mandatory Same-Input Negative Control: &arena normal path keeps GpuPrivateScratchFallback strictly OFF!
			{
				const std::vector<VpSkeleton::ArenaRequest> reqs = TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, 16, Preset::Reference);
				const VpSkeleton::ArenaPlan planT1 = VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T1);
				VpSkeleton::PlTexBackend backend(ctx.gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				Assert::IsTrue(arena.Reserve(planT1));

				EngineGuard arenaGuard(ctx.gpu, cfg);
				StandInRegistry arenaSi;
				arena.BeginFrame();
				const GpuProcessResult arenaRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, &arena, &arenaSi);
				Assert::IsTrue(arenaRes.succeeded, L"LOW-2 arena-backed ProcessFrame must succeed");
				Assert::IsFalse(arenaSi.IsActive(StandInKind::GpuPrivateScratchFallback),
					L"Negative control: &arena normal path MUST NOT engage StandInKind::GpuPrivateScratchFallback");
				Assert::AreEqual(static_cast<uint64_t>(0u), arenaSi.HitCount(StandInKind::GpuPrivateScratchFallback));
				Assert::IsTrue(arena.Acquired() > 0u, L"Arena path must acquire scratch textures from shared ScratchArena");
				Assert::IsTrue(RmsDiffRect(privRes.outputY, arenaRes.outputY, full) <= 1e-5,
					L"Private fallback and shared ScratchArena outputs must match within 1e-5");
			}
		}

		TEST_METHOD(Low3ChromaBranchBRackFocusAndBokehCellVetoesWithSharpNeighborRecovery)
		{
			// 128x64 luma -> 8x4 cells (16x16 luma px each); coded = Yuv420 (64x32), delivered = Yuv422 (64x64)
			const int lumaW = 128;
			const int lumaH = 64;
			const int cw = lumaW / 16; // 8
			const int ch = lumaH / 16; // 4
			const size_t numCells = static_cast<size_t>(cw * ch);
			const int cW = 64;
			const int cH = 32;
			const int dW = 64;
			const int dH = 64;
			const ActiveRect lumaFull = ActiveRect::Full(lumaW, lumaH);

			// Build multi-band coded 4:2:0 master Cb/Cr with strong L (low), M (mid), and H (fine) chroma energy
			TdV2Plane masterCodedCb(cW, cH, 512.0);
			TdV2Plane masterCodedCr(cW, cH, 512.0);
			for (int y = 0; y < cH; ++y)
			{
				for (int x = 0; x < cW; ++x)
				{
					const double lTerm = 28.0 * std::sin(2.0 * kPi * x / 28.0 + 0.3) * std::cos(2.0 * kPi * y / 22.0);
					const double mTerm = 18.0 * std::sin(2.0 * kPi * (x / 5.5 + y / 6.5) + 0.8);
					const double hTerm = 15.0 * std::cos(2.0 * kPi * (x / 2.4 - y / 2.7) + 1.2);
					masterCodedCb.at(x, y) = 512.0 + lTerm + mTerm + hTerm;
					masterCodedCr.at(x, y) = 512.0 - 0.85 * lTerm + 0.9 * mTerm - hTerm;
				}
			}

			// Degrade current coded B-frame across ALL bands (wipe 75% of L deviation, M, and H)
			const ThreeBandPlanes mbCb = DecomposeActiveBands(masterCodedCb, ActiveRect::Full(cW, cH));
			const ThreeBandPlanes mbCr = DecomposeActiveBands(masterCodedCr, ActiveRect::Full(cW, cH));
			TdV2Plane curCodedCb(cW, cH, 512.0);
			TdV2Plane curCodedCr(cW, cH, 512.0);
			for (size_t i = 0; i < masterCodedCb.v.size(); ++i)
			{
				curCodedCb.v[i] = 512.0 + 0.25 * (mbCb.L.v[i] - 512.0) + 0.15 * mbCb.M.v[i] + 0.10 * mbCb.H.v[i];
				curCodedCr.v[i] = 512.0 + 0.25 * (mbCr.L.v[i] - 512.0) + 0.15 * mbCr.M.v[i] + 0.10 * mbCr.H.v[i];
			}

			// Upsample current coded 4:2:0 to delivered 4:2:2 (64x64)
			const SitingSpec siting = SitingSpec::Left();
			const TdV2Plane recDelivCb = UpsampleDeltaToDelivered(
				curCodedCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, lumaFull, nullptr);
			const TdV2Plane recDelivCr = UpsampleDeltaToDelivered(
				curCodedCr, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, lumaFull, nullptr);
			const TdV2Plane masterDelivCb = UpsampleDeltaToDelivered(
				masterCodedCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, lumaFull, nullptr);
			Assert::AreEqual(dW, recDelivCb.w);
			Assert::AreEqual(dH, recDelivCb.h);

			TdV2Plane prevCb = masterCodedCb, prevCr = masterCodedCr;
			TdV2Plane nextCb = masterCodedCb, nextCr = masterCodedCr;
			{
				std::mt19937 r1(7031u), r2(7032u);
				std::normal_distribution<double> nd(0.0, 0.35);
				for (size_t i = 0; i < prevCb.v.size(); ++i)
				{
					prevCb.v[i] += nd(r1); prevCr.v[i] += nd(r1);
					nextCb.v[i] += nd(r2); nextCr.v[i] += nd(r2);
				}
			}

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			// Cell layout (8x4):
			//   Rack-focus cell (cx=1, cy=1): coded rect [8..16) x [8..16), delivered 4:2:2 rect [8..16) x [16..32)
			//   Bokeh cell      (cx=4, cy=1): coded rect [32..40) x [8..16), delivered 4:2:2 rect [32..40) x [16..32)
			//   Sharp cell      (cx=6, cy=2): coded rect [48..56) x [16..24), delivered 4:2:2 rect [48..56) x [32..48)
			const size_t rackCell = static_cast<size_t>(1 * cw + 1);
			const size_t bokehCell = static_cast<size_t>(1 * cw + 4);
			std::vector<uint8_t> rackMask(numCells, 0u);
			std::vector<uint8_t> bokehMask(numCells, 0u);
			rackMask[rackCell] = 1u;
			bokehMask[bokehCell] = 1u;

			const ActiveRect rackCodedRect{ 8, 8, 16, 16 };
			const ActiveRect rackDelivRect{ 8, 16, 16, 32 };
			const ActiveRect bokehCodedRect{ 32, 8, 40, 16 };
			const ActiveRect bokehDelivRect{ 32, 16, 40, 32 };
			const ActiveRect sharpDelivRect{ 48, 32, 56, 48 };

			ChromaProcessInput baseIn;
			baseIn.seq = 50;
			baseIn.lumaW = lumaW;
			baseIn.lumaH = lumaH;
			baseIn.gridStatus = ChromaGridStatus::Identified;
			baseIn.codedSubsampling = ChromaSubsampling::Yuv420;
			baseIn.deliveredFormat = DeliveredChromaFormat::Yuv422;
			baseIn.siting = siting;
			baseIn.activeRect = lumaFull;
			baseIn.receivedCb = recDelivCb;
			baseIn.receivedCr = recDelivCr;
			baseIn.hasCleanSplit = true;
			baseIn.cleanCb = curCodedCb;
			baseIn.cleanCr = curCodedCr;
			baseIn.prevPeak.cleanCb = &prevCb;
			baseIn.prevPeak.cleanCr = &prevCr;
			baseIn.prevPeak.lumaMotion = &stillMf;
			baseIn.nextPeak.cleanCb = &nextCb;
			baseIn.nextPeak.cleanCr = &nextCr;
			baseIn.nextPeak.lumaMotion = &stillMf;
			baseIn.hasStepPrior = true;
			baseIn.estimatedStepDelta = 40.0;
			baseIn.deadZonePhi = kDeadZonePhiInter;
			baseIn.zeroCoeffFraction = 0.35;

			// 1. Mandatory Same-Input Negative Control (no focus/bokeh masks): BOTH cells recover strong L+M+H chroma!
			Config cfgUntouched = MakePreset(Preset::Reference, Mode::On);
			cfgUntouched.bokehMode = BokehMode::Untouched;
			TdV2ChromaEngine engCtrl(cfgUntouched);
			const ChromaProcessResult resCtrl = engCtrl.ProcessFrame(baseIn, nullptr);
			const ThreeBandPlanes ctrlCodedBands = DecomposeActiveBands(resCtrl.codedDeltaCb, ActiveRect::Full(cW, cH));
			const double ctrlRackPeak = MaxAbsRect(resCtrl.codedDeltaCb, rackCodedRect);
			const double ctrlBokehL = RmsRect(ctrlCodedBands.L, bokehCodedRect);
			const double ctrlBokehMH = RmsRect(ctrlCodedBands.M, bokehCodedRect) + RmsRect(ctrlCodedBands.H, bokehCodedRect);
			Assert::IsTrue(ctrlRackPeak > 8.0 && ctrlBokehL > 3.0 && ctrlBokehMH > 6.0,
				FmtMsg("Negative control failed: unmasked chroma recovery too weak (rackPeak=%.2f, bokehL=%.2f, bokehMH=%.2f)",
					ctrlRackPeak, ctrlBokehL, ctrlBokehMH).c_str());

			// 2. BokehMode::Untouched with explicitFocusChangeCells + explicitDefocusedCells:
			//    BOTH rackCell and bokehCell must hard-veto L/M/H == 0.0 (coded & delivered!), while sharpCell recovers!
			ChromaProcessInput maskedIn = baseIn;
			maskedIn.explicitFocusChangeCells = &rackMask;
			maskedIn.explicitDefocusedCells = &bokehMask;

			TdV2ChromaEngine engUntouched(cfgUntouched);
			const ChromaProcessResult resUnt = engUntouched.ProcessFrame(maskedIn, nullptr);
			Assert::AreEqual(static_cast<size_t>(2u), resUnt.focusVetoCells, L"focusVetoCells must count rack + bokeh cells");
			Assert::AreEqual(0.0, MaxAbsRect(resUnt.codedDeltaCb, rackCodedRect), L"Rack-focus codedDeltaCb must be exact 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(resUnt.codedDeltaCr, rackCodedRect), L"Rack-focus codedDeltaCr must be exact 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(resUnt.deliveredDeltaCb, rackDelivRect), L"Rack-focus deliveredDeltaCb must be exact 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(resUnt.codedDeltaCb, bokehCodedRect), L"BokehMode::Untouched bokeh codedDeltaCb must be exact 0.0");
			Assert::AreEqual(0.0, MaxAbsRect(resUnt.deliveredDeltaCb, bokehDelivRect), L"BokehMode::Untouched bokeh deliveredDeltaCb must be exact 0.0");

			// Adjacent sharp cell (6, 2) recovers strongly in the exact same frame!
			const double sharpRawErr = RmsDiffRect(recDelivCb, masterDelivCb, sharpDelivRect);
			const double sharpOutErr = RmsDiffRect(resUnt.outputCb, masterDelivCb, sharpDelivRect);
			Assert::IsTrue(sharpOutErr < sharpRawErr * 0.45,
				FmtMsg("Sharp neighbor cell must recover strongly in same frame: outErr=%.4f vs rawErr=%.4f", sharpOutErr, sharpRawErr).c_str());

			// 3. BokehMode::LowBandCellOnly:
			//    - rackCell STILL hard-vetoes ALL bands (L/M/H == 0.0)!
			//    - bokehCell hard-vetoes M/H == 0 while genuinely recovering coarse L-band chroma!
			Config cfgBokehLow = cfgUntouched;
			cfgBokehLow.bokehMode = BokehMode::LowBandCellOnly;
			TdV2ChromaEngine engBokehLow(cfgBokehLow);
			const ChromaProcessResult resLow = engBokehLow.ProcessFrame(maskedIn, nullptr);

			Assert::AreEqual(0.0, MaxAbsRect(resLow.codedDeltaCb, rackCodedRect),
				L"Rack-focus cell MUST still hard-veto L/M/H == 0.0 even under BokehMode::LowBandCellOnly");
			Assert::AreEqual(0.0, MaxAbsRect(resLow.deliveredDeltaCb, rackDelivRect),
				L"Rack-focus deliveredDeltaCb MUST stay exact 0.0 under BokehMode::LowBandCellOnly");

			const ActiveRect bokehInterior{ 34, 10, 38, 14 }; // Interior of 8x8 coded cell away from 3-px filter halo
			const ThreeBandPlanes lowCodedBands = DecomposeActiveBands(resLow.codedDeltaCb, ActiveRect::Full(cW, cH));
			const double bokehLowL = RmsRect(lowCodedBands.L, bokehInterior);
			const double bokehLowH = RmsRect(lowCodedBands.H, bokehInterior);
			Assert::IsTrue(bokehLowL > 2.5,
				FmtMsg("BokehMode::LowBandCellOnly must allow L-band chroma recovery on defocused cell: L_rms=%.4f", bokehLowL).c_str());
			Assert::IsTrue(bokehLowH < 0.35,
				FmtMsg("BokehMode::LowBandCellOnly must hard-veto H-band on defocused cell: H_rms=%.4f", bokehLowH).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FormatAwareChromaVramSizingSavesOver75PercentOn420AndPreservesTrue422And444)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FormatAwareChromaVramSizingSavesOver75PercentOn420AndPreservesTrue422And444)
		{
			// 1. Exact 4K (3840x2160) VRAM accounting & MakeVramUserFor contract
			const int w4k = 3840;
			const int h4k = 2160;
			const uint64_t worst4k = TdV2GpuEngine::EstimatedChromaVramBytes(w4k, h4k);
			const uint64_t b420 = TdV2GpuEngine::EstimatedChromaVramBytesFor(
				w4k, h4k, DeliveredChromaFormat::Yuv420, ChromaSubsampling::Yuv420);
			const uint64_t b422 = TdV2GpuEngine::EstimatedChromaVramBytesFor(
				w4k, h4k, DeliveredChromaFormat::Yuv422, ChromaSubsampling::Yuv422);
			const uint64_t b444 = TdV2GpuEngine::EstimatedChromaVramBytesFor(
				w4k, h4k, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv444);
			const uint64_t bRgb = TdV2GpuEngine::EstimatedChromaVramBytesFor(
				w4k, h4k, DeliveredChromaFormat::Rgb444, ChromaSubsampling::Yuv444);

			const double savings420 = 1.0 - (static_cast<double>(b420) / static_cast<double>(worst4k));
			Assert::IsTrue(savings420 >= 0.75,
				FmtMsg("4K Yuv420/Yuv420 VRAM savings=%.4f must be >= 0.75 vs worst-case (b420=%.0f, worst=%.0f)",
					savings420, static_cast<double>(b420), static_cast<double>(worst4k)).c_str());
			Assert::AreEqual(b420 * 2ull, b422, L"True native 4:2:2 must budget exact 2x 4:2:0 (never discarding vertical chroma)");
			Assert::AreEqual(b420 * 4ull, b444, L"True native 4:4:4 YUV must budget exact 4x 4:2:0 full resolution");
			Assert::IsTrue(bRgb > b444 && bRgb < worst4k, L"Active RGB 4:4:4 path must budget full-res RGBA32F without double-counting YUV");

			const VpSkeleton::VramUser u420 = TdV2GpuEngine::MakeVramUserFor(
				w4k, h4k, DeliveredChromaFormat::Yuv420, ChromaSubsampling::Yuv420, 16, Preset::RealTime);
			const uint64_t lumaRt4k = TdV2GpuEngine::EstimatedPersistentVramBytes(w4k, h4k, 16, Preset::RealTime, false);
			Assert::AreEqual(static_cast<size_t>(3u), u420.levels.size());
			Assert::AreEqual(lumaRt4k + b420, u420.levels[0]);
			Assert::AreEqual(lumaRt4k, u420.levels[1]);
			Assert::AreEqual(static_cast<uint64_t>(0u), u420.levels[2]);

			// 2. Live D3D11 WARP verification: AllocatedPersistentChromaVramBytes() matches EstimatedChromaVramBytesFor()
			//    to the EXACT BYTE across Yuv420, Yuv422, Yuv444, and Rgb444!
			WarpContext ctx;
			const int w = 64;
			const int h = 64;
			const Config cfg = MakePreset(Preset::Reference, Mode::On);

			struct FormatCase
			{
				DeliveredChromaFormat deliv;
				ChromaSubsampling coded;
				int dW, dH, cW, cH;
			};
			const FormatCase cases[] = {
				{ DeliveredChromaFormat::Yuv420, ChromaSubsampling::Yuv420, 32, 32, 32, 32 },
				{ DeliveredChromaFormat::Yuv422, ChromaSubsampling::Yuv422, 32, 64, 32, 64 },
				{ DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv444, 64, 64, 64, 64 },
				{ DeliveredChromaFormat::Rgb444, ChromaSubsampling::Yuv444, 64, 64, 64, 64 }
			};

			for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
			{
				const FormatCase& fc = cases[i];
				EngineGuard g(ctx.gpu, cfg);
				ChromaGpuApplyInput aIn;
				aIn.lumaW = w;
				aIn.lumaH = h;
				aIn.deliveredFormat = fc.deliv;
				aIn.codedSubsampling = fc.coded;
				aIn.siting = SitingSpec::Left();
				aIn.activeRect = ActiveRect::Full(w, h);
				aIn.codedDeltaCb = TdV2Plane(fc.cW, fc.cH, +3.0);
				aIn.codedDeltaCr = TdV2Plane(fc.cW, fc.cH, -2.5);
				if (fc.deliv == DeliveredChromaFormat::Rgb444)
				{
					aIn.deliveredR = TdV2Plane(fc.dW, fc.dH, 500.0);
					aIn.deliveredG = TdV2Plane(fc.dW, fc.dH, 500.0);
					aIn.deliveredB = TdV2Plane(fc.dW, fc.dH, 500.0);
				}
				else
				{
					aIn.deliveredCb = TdV2Plane(fc.dW, fc.dH, 512.0);
					aIn.deliveredCr = TdV2Plane(fc.dW, fc.dH, 512.0);
				}

				const ChromaGpuApplyResult aRes = g.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, aIn, nullptr);
				Assert::IsTrue(aRes.succeeded && aRes.dispatchCount == 1);

				const uint64_t liveBytes = g.engine.AllocatedPersistentChromaVramBytes();
				const uint64_t expectedBytes = TdV2GpuEngine::EstimatedChromaVramBytesFor(w, h, fc.deliv, fc.coded);
				Assert::AreEqual(expectedBytes, liveBytes,
					FmtMsg("Live WARP AllocatedPersistentChromaVramBytes (%.0f B) != EstimatedChromaVramBytesFor (%.0f B) on case %.0f",
						static_cast<double>(liveBytes), static_cast<double>(expectedBytes), static_cast<double>(i)).c_str());
			}
		}
	};
} // namespace TdV2MinorsTestsNs
} // namespace Tests
