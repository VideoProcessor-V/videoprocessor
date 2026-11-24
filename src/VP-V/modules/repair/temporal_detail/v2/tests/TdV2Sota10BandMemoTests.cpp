// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 -- Unit 10 (TD-SOTA-10) Adversarial Red-Team WARP GPU Tests:
// kBandMemo Shared Current-Frame Low-Band Zero-Duplicate Reuse (curLowBandTex in TdV2GpuEngine):
//   1. GpuBandMemoReferenceAndRealTimeDropDispatchesAndMatchSelfComputedBands (GpuWarp):
//      - Preset::Reference: valid full-res curLowBandTex strictly drops dispatchCount by -1 vs curLowBandTex==nullptr
//        baseline, sets usedExternalBandMemo=true, and matches self-computed BandFromBase output (< 1e-3 RMS).
//      - Preset::RealTime: BOTH valid full-res (w x h) AND valid half-res (wHalf x hHalf) curLowBandTex strictly
//        drop dispatchCount by -2 vs curLowBandTex==nullptr baseline, set usedExternalBandMemo=true, and match
//        self-computed BandFromBase output (< 1e-3 RMS).
//      - Includes adversarial perturbed-memo control proving curLowBandTex is genuinely consumed in Pass 1C!
//   2. GpuBandMemoDimensionAndFormatMismatchFailClosedVisiblyZeroSilentFallback (GpuWarp):
//      - Wrong dimensions ((w-4) x h), half-res curLowBandTex passed to Preset::Reference, and non-float UNORM r8
//        curLowBandTex ALL fail closed visibly (succeeded=false, externalBandMemoRejected=true, dispatchCount=0,
//        StandInKind::BandMemoW2 & StandInKind::GpuDispatchFailed active -- ZERO silent fallback!).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
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
namespace TdV2Sota10BandMemoNs
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

	struct LumaGpuGuard
	{
		pl_gpu gpu = nullptr;
		TdV2GpuEngine engine;
		explicit LumaGpuGuard(pl_gpu g, const Config& cfg) : gpu(g), engine(cfg) {}
		~LumaGpuGuard() { engine.Release(gpu); }
		LumaGpuGuard(const LumaGpuGuard&) = delete;
		LumaGpuGuard& operator=(const LumaGpuGuard&) = delete;
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
		char buf[360];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	static double RmsDiff(const TdV2Plane& a, const TdV2Plane& b)
	{
		double sumSq = 0.0;
		const size_t n = (std::min)(a.v.size(), b.v.size());
		for (size_t i = 0; i < n; ++i)
		{
			const double d = a.v[i] - b.v[i];
			sumSq += d * d;
		}
		return n > 0 ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;
	}

	static double MaxAbsDiff(const TdV2Plane& a, const TdV2Plane& b)
	{
		double mx = 0.0;
		const size_t n = (std::min)(a.v.size(), b.v.size());
		for (size_t i = 0; i < n; ++i)
		{
			const double d = std::fabs(a.v[i] - b.v[i]);
			if (d > mx)
				mx = d;
		}
		return mx;
	}

	static pl_tex CreateFloatTex(pl_gpu gpu, int w, int h, int comps, bool sampleable, bool storable, const float* initData)
	{
		const pl_fmt_caps caps = static_cast<pl_fmt_caps>(
			(sampleable ? static_cast<int>(PL_FMT_CAP_SAMPLEABLE) : 0) |
			(storable ? static_cast<int>(PL_FMT_CAP_STORABLE) : 0));
		pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, comps, 32, 32, caps);
		Assert::IsNotNull(fmt);
		pl_tex_params tp{};
		tp.w = w;
		tp.h = h;
		tp.d = 0;
		tp.format = fmt;
		tp.sampleable = sampleable;
		tp.storable = storable;
		tp.host_writable = (initData == nullptr);
		tp.host_readable = storable;
		tp.initial_data = initData;
		pl_tex tex = pl_tex_create(gpu, &tp);
		Assert::IsNotNull(tex);
		return tex;
	}

	static std::vector<float> PlaneToFloats(const TdV2Plane& p)
	{
		std::vector<float> v(p.v.size(), 0.0f);
		for (size_t i = 0; i < p.v.size(); ++i)
			v[i] = static_cast<float>(p.v[i]);
		return v;
	}

	// Exact half-res low band matching RealTime kPass1HalfH + kPass1HalfV (2x2 box downsample + G(0.5 * kSigmaBase)).
	static TdV2Plane ComputeExactHalfLowBand(const TdV2Plane& cleanFull)
	{
		const int w = cleanFull.w;
		const int h = cleanFull.h;
		const int wH = (w + 1) / 2;
		const int hH = (h + 1) / 2;
		TdV2Plane box(wH, hH, 0.0);
		for (int hy = 0; hy < hH; ++hy)
		{
			const int fy0 = (std::min)(h - 1, 2 * hy);
			const int fy1 = (std::min)(h - 1, 2 * hy + 1);
			for (int hx = 0; hx < wH; ++hx)
			{
				const int fx0 = (std::min)(w - 1, 2 * hx);
				const int fx1 = (std::min)(w - 1, 2 * hx + 1);
				box.at(hx, hy) = 0.25 * (cleanFull.at(fx0, fy0) + cleanFull.at(fx1, fy0) +
				                         cleanFull.at(fx0, fy1) + cleanFull.at(fx1, fy1));
			}
		}

		const auto taps = VpUnit::BandFromBase::Taps<double>(0.5 * VpUnit::BandFromBase::kSigmaBase, 3.0, true, 0.0);
		const int rad = static_cast<int>(taps.size() / 2);
		TdV2Plane tmp(wH, hH, 0.0);
		TdV2Plane out(wH, hH, 0.0);
		for (int y = 0; y < hH; ++y)
		{
			for (int x = 0; x < wH; ++x)
			{
				double acc = 0.0;
				for (int k = -rad; k <= rad; ++k)
				{
					const int sx = (std::max)(0, (std::min)(wH - 1, x + k));
					acc += taps[static_cast<size_t>(k + rad)] * box.at(sx, y);
				}
				tmp.at(x, y) = acc;
			}
		}
		for (int y = 0; y < hH; ++y)
		{
			for (int x = 0; x < wH; ++x)
			{
				double acc = 0.0;
				for (int k = -rad; k <= rad; ++k)
				{
					const int sy = (std::max)(0, (std::min)(hH - 1, y + k));
					acc += taps[static_cast<size_t>(k + rad)] * tmp.at(x, sy);
				}
				out.at(x, y) = acc;
			}
		}
		return out;
	}

	TEST_CLASS(TdV2Sota10BandMemoTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuBandMemoReferenceAndRealTimeDropDispatchesAndMatchSelfComputedBands)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuBandMemoReferenceAndRealTimeDropDispatchesAndMatchSelfComputedBands)
		{
			WarpContext wc;
			VpSkeleton::PlTexBackend backend(wc.gpu);

			const int w = 96;
			const int h = 64;
			const int wHalf = w / 2;
			const int hHalf = h / 2;
			const int cw = w / 16;
			const int ch = h / 16;

			TdV2Plane master(w, h, 0.0);
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const double fx = static_cast<double>(x);
					const double fy = static_cast<double>(y);
					master.at(x, y) = 470.0
						+ 38.0 * std::sin(2.0 * kPi * fx / 32.0 + 0.2) * std::cos(2.0 * kPi * fy / 28.0)
						+ 24.0 * std::sin(2.0 * kPi * (fx / 7.5 + fy / 8.0) + 0.4)
						+ 16.0 * std::cos(2.0 * kPi * (fx / 3.4 - fy / 3.8) + 1.1);
				}
			}

			const ActiveRect full = ActiveRect::Full(w, h);
			const ThreeBandPlanes mb = DecomposeActiveBands(master, full);
			TdV2Plane curClean(w, h, 0.0);
			TdV2Plane recY(w, h, 0.0);
			TdV2Plane prevRef = master;
			TdV2Plane nextRef = master;
			std::mt19937 rng(707u);
			std::normal_distribution<double> qn(0.0, 2.0);
			std::normal_distribution<double> gn(0.0, 2.6);
			std::normal_distribution<double> pn(0.0, 0.6);
			for (size_t i = 0; i < master.v.size(); ++i)
			{
				const double bandedL = std::nearbyint(mb.L.v[i] / 12.0) * 12.0;
				curClean.v[i] = bandedL + 0.26 * mb.M.v[i] + 0.22 * mb.H.v[i] + qn(rng);
				recY.v[i] = curClean.v[i] + gn(rng);
				prevRef.v[i] += pn(rng);
				nextRef.v[i] += pn(rng);
			}

			const TdV2Plane curLowFull = DecomposeActiveBands(curClean, full).L;
			const TdV2Plane curLowHalf = ComputeExactHalfLowBand(curClean);
			TdV2Plane curLowPerturbed = curLowFull;
			for (size_t i = 0; i < curLowPerturbed.v.size(); ++i)
				curLowPerturbed.v[i] += 35.0;

			const std::vector<float> recF = PlaneToFloats(recY);
			const std::vector<float> cleanF = PlaneToFloats(curClean);
			const std::vector<float> prevF = PlaneToFloats(prevRef);
			const std::vector<float> nextF = PlaneToFloats(nextRef);
			const std::vector<float> memoFullF = PlaneToFloats(curLowFull);
			const std::vector<float> memoHalfF = PlaneToFloats(curLowHalf);
			const std::vector<float> memoPertF = PlaneToFloats(curLowPerturbed);

			OwnedTex tRec(wc.gpu), tClean(wc.gpu), tPrev(wc.gpu), tNext(wc.gpu);
			OwnedTex tMemoFull(wc.gpu), tMemoHalf(wc.gpu), tMemoPert(wc.gpu);
			tRec.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, recF.data());
			tClean.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, cleanF.data());
			tPrev.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, prevF.data());
			tNext.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, nextF.data());
			tMemoFull.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, memoFullF.data());
			tMemoHalf.tex = CreateFloatTex(wc.gpu, wHalf, hHalf, 1, true, false, memoHalfF.data());
			tMemoPert.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, memoPertF.data());

			const CellMotionField zeroMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			auto runCase = [&](Preset preset, pl_tex memoTex, GpuFrameOutput& gOut, TdV2Plane& outY, TdV2Plane& outClean)
			{
				Config cfg;
				cfg.preset = preset;
				cfg.mode = Mode::On;
				cfg.sourceBits = 10;
				cfg.cellPx = 16;

				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
					TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, preset), VpSkeleton::Tier::T1)));

				LumaGpuGuard eng(wc.gpu, cfg);
				OwnedTex tOutY(wc.gpu), tOutClean(wc.gpu);
				tOutY.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);
				tOutClean.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);

				GpuFrameInput gIn;
				gIn.seq = 0;
				gIn.w = w;
				gIn.h = h;
				gIn.receivedYTex = tRec.tex;
				gIn.cleanYTex = tClean.tex;
				gIn.hasCleanSplit = true;
				gIn.hasStepPrior = true;
				gIn.estimatedStepDelta = 14.0;
				gIn.prevPeak.cleanTex = tPrev.tex;
				gIn.prevPeak.motion = &zeroMf;
				gIn.nextPeak.cleanTex = tNext.tex;
				gIn.nextPeak.motion = &zeroMf;
				gIn.curLowBandTex = memoTex;

				gOut = GpuFrameOutput{};
				gOut.outputYTex = tOutY.tex;
				gOut.outputCleanTex = tOutClean.tex;
				gOut.readbackCellDiagnostics = true;

				arena.BeginFrame();
				const bool ok = eng.engine.ProcessFrameGpu(wc.gpu, wc.dispatch, gIn, gOut, &arena, nullptr);
				Assert::IsTrue(ok && gOut.succeeded);
				Assert::IsTrue(Detail::DownloadPlaneR32F(wc.gpu, tOutY.tex, w, h, outY));
				Assert::IsTrue(Detail::DownloadPlaneR32F(wc.gpu, tOutClean.tex, w, h, outClean));
			};

			// ---- Part A: Preset::Reference (-1 dispatch, exact BandFromBase parity) ----
			GpuFrameOutput refBaseOut, refMemoOut, refPertOut;
			TdV2Plane refBaseY, refBaseClean, refMemoY, refMemoClean, refPertY, refPertClean;
			runCase(Preset::Reference, nullptr, refBaseOut, refBaseY, refBaseClean);
			runCase(Preset::Reference, tMemoFull.tex, refMemoOut, refMemoY, refMemoClean);
			runCase(Preset::Reference, tMemoPert.tex, refPertOut, refPertY, refPertClean);

			Assert::IsFalse(refBaseOut.usedExternalBandMemo);
			Assert::IsFalse(refBaseOut.externalBandMemoRejected);
			Assert::IsTrue(refMemoOut.usedExternalBandMemo);
			Assert::IsFalse(refMemoOut.externalBandMemoRejected);
			Assert::AreEqual(refBaseOut.dispatchCount - 1, refMemoOut.dispatchCount,
				FmtMsg("Reference curLowBandTex must save -1 dispatch: base=%0.f memo=%.0f",
					static_cast<double>(refBaseOut.dispatchCount), static_cast<double>(refMemoOut.dispatchCount)).c_str());

			Assert::IsTrue(RmsDiff(refMemoClean, refBaseClean) < 2e-4 && MaxAbsDiff(refMemoClean, refBaseClean) < 2e-3,
				FmtMsg("Reference BandMemo clean mismatch vs baseline: rms=%.6f max=%.6f",
					RmsDiff(refMemoClean, refBaseClean), MaxAbsDiff(refMemoClean, refBaseClean)).c_str());
			Assert::IsTrue(RmsDiff(refMemoY, refBaseY) < 2e-4,
				FmtMsg("Reference BandMemo outputY mismatch vs baseline: rms=%.6f", RmsDiff(refMemoY, refBaseY)).c_str());
			Assert::IsTrue(RmsDiff(refMemoClean, master) < 0.60 * RmsDiff(curClean, master));

			// Perturbed curLowBandTex control proves curLowBandTex is genuinely consumed (not bypassed!).
			Assert::IsTrue(RmsDiff(refPertClean, refMemoClean) > 1.0,
				FmtMsg("curLowBandTex appears unused: perturbed vs true memo rms=%.6f", RmsDiff(refPertClean, refMemoClean)).c_str());

			// ---- Part B: Preset::RealTime (-2 dispatches for BOTH full-res and half-res curLowBandTex!) ----
			GpuFrameOutput rtBaseOut, rtFullOut, rtHalfOut;
			TdV2Plane rtBaseY, rtBaseClean, rtFullY, rtFullClean, rtHalfY, rtHalfClean;
			runCase(Preset::RealTime, nullptr, rtBaseOut, rtBaseY, rtBaseClean);
			runCase(Preset::RealTime, tMemoFull.tex, rtFullOut, rtFullY, rtFullClean);
			runCase(Preset::RealTime, tMemoHalf.tex, rtHalfOut, rtHalfY, rtHalfClean);

			Assert::IsFalse(rtBaseOut.usedExternalBandMemo);
			Assert::IsTrue(rtFullOut.usedExternalBandMemo && !rtFullOut.externalBandMemoRejected);
			Assert::IsTrue(rtHalfOut.usedExternalBandMemo && !rtHalfOut.externalBandMemoRejected);
			Assert::AreEqual(rtBaseOut.dispatchCount - 2, rtFullOut.dispatchCount,
				FmtMsg("RealTime full-res curLowBandTex must save -2 dispatches: base=%.0f memo=%.0f",
					static_cast<double>(rtBaseOut.dispatchCount), static_cast<double>(rtFullOut.dispatchCount)).c_str());
			Assert::AreEqual(rtBaseOut.dispatchCount - 2, rtHalfOut.dispatchCount,
				FmtMsg("RealTime half-res curLowBandTex must save -2 dispatches: base=%.0f memo=%.0f",
					static_cast<double>(rtBaseOut.dispatchCount), static_cast<double>(rtHalfOut.dispatchCount)).c_str());

			Assert::IsTrue(RmsDiff(rtFullClean, rtBaseClean) < 2e-4 && MaxAbsDiff(rtFullClean, rtBaseClean) < 2e-3,
				FmtMsg("RealTime full-res BandMemo mismatch: rms=%.6f", RmsDiff(rtFullClean, rtBaseClean)).c_str());
			Assert::IsTrue(RmsDiff(rtHalfClean, rtBaseClean) < 2e-4 && MaxAbsDiff(rtHalfClean, rtBaseClean) < 2e-3,
				FmtMsg("RealTime half-res BandMemo mismatch: rms=%.6f", RmsDiff(rtHalfClean, rtBaseClean)).c_str());
			Assert::IsTrue(RmsDiff(rtHalfY, rtBaseY) < 2e-4);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuBandMemoDimensionAndFormatMismatchFailClosedVisiblyZeroSilentFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuBandMemoDimensionAndFormatMismatchFailClosedVisiblyZeroSilentFallback)
		{
			WarpContext wc;
			VpSkeleton::PlTexBackend backend(wc.gpu);

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const std::vector<float> flatF(static_cast<size_t>(w * h), 400.0f);
			const std::vector<float> halfF(static_cast<size_t>((w / 2) * (h / 2)), 400.0f);
			const std::vector<float> badDimF(static_cast<size_t>((w - 4) * h), 400.0f);
			const std::vector<uint8_t> unormBytes(static_cast<size_t>(w * h), 128u);

			OwnedTex tRec(wc.gpu), tPrev(wc.gpu), tNext(wc.gpu), tOutY(wc.gpu);
			OwnedTex tBadDim(wc.gpu), tHalfMemo(wc.gpu), tUnormMemo(wc.gpu);
			tRec.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, flatF.data());
			tPrev.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, flatF.data());
			tNext.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, flatF.data());
			tOutY.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);
			tBadDim.tex = CreateFloatTex(wc.gpu, w - 4, h, 1, true, false, badDimF.data());
			tHalfMemo.tex = CreateFloatTex(wc.gpu, w / 2, h / 2, 1, true, false, halfF.data());

			pl_fmt fmtR8 = pl_find_fmt(wc.gpu, PL_FMT_UNORM, 1, 8, 8, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(fmtR8);
			pl_tex_params r8Params{};
			r8Params.w = w;
			r8Params.h = h;
			r8Params.format = fmtR8;
			r8Params.sampleable = true;
			r8Params.initial_data = unormBytes.data();
			tUnormMemo.tex = pl_tex_create(wc.gpu, &r8Params);
			Assert::IsNotNull(tUnormMemo.tex);

			const CellMotionField zeroMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			auto assertRejected = [&](Preset preset, pl_tex badMemo, const wchar_t* label)
			{
				Config cfg;
				cfg.preset = preset;
				cfg.mode = Mode::On;
				cfg.sourceBits = 10;
				cfg.cellPx = 16;

				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
					TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, preset), VpSkeleton::Tier::T1)));

				LumaGpuGuard eng(wc.gpu, cfg);
				GpuFrameInput gIn;
				gIn.seq = 0;
				gIn.w = w;
				gIn.h = h;
				gIn.receivedYTex = tRec.tex;
				gIn.cleanYTex = tRec.tex;
				gIn.hasCleanSplit = true;
				gIn.hasStepPrior = true;
				gIn.estimatedStepDelta = 12.0;
				gIn.prevPeak.cleanTex = tPrev.tex;
				gIn.prevPeak.motion = &zeroMf;
				gIn.nextPeak.cleanTex = tNext.tex;
				gIn.nextPeak.motion = &zeroMf;
				gIn.curLowBandTex = badMemo;

				GpuFrameOutput gOut;
				gOut.outputYTex = tOutY.tex;
				StandInRegistry st;

				arena.BeginFrame();
				const bool ok = eng.engine.ProcessFrameGpu(wc.gpu, wc.dispatch, gIn, gOut, &arena, &st);
				Assert::IsFalse(ok, label);
				Assert::IsFalse(gOut.succeeded, label);
				Assert::IsTrue(gOut.externalBandMemoRejected, label);
				Assert::IsFalse(gOut.usedExternalBandMemo, label);
				Assert::AreEqual(0, gOut.dispatchCount, label);
				Assert::IsFalse(gOut.lastError.empty(), label);
				Assert::IsTrue(st.IsActive(StandInKind::BandMemoW2), label);
				Assert::IsTrue(st.IsActive(StandInKind::GpuDispatchFailed), label);
			};

			// 1. Mismatched dimensions (60 x 64) in Reference & RealTime -> visible rejection, zero silent fallback.
			assertRejected(Preset::Reference, tBadDim.tex, L"Reference wrong-dim curLowBandTex must fail closed");
			assertRejected(Preset::RealTime, tBadDim.tex, L"RealTime wrong-dim curLowBandTex must fail closed");

			// 2. Half-res (32 x 32) passed to Preset::Reference (only legal in RealTime!) -> visible rejection.
			assertRejected(Preset::Reference, tHalfMemo.tex, L"Reference half-res curLowBandTex must fail closed");

			// 3. Non-float UNORM r8 texture -> visible rejection.
			assertRejected(Preset::RealTime, tUnormMemo.tex, L"UNORM r8 curLowBandTex must fail closed");
		}
	};
}
}
