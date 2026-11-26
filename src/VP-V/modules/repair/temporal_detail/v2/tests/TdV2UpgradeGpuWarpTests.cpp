// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Modular Upgrades Cross-Model Adversarial D3D11 WARP GPU Parity Suite
// (`Category=GpuWarp`, real D3D11 WARP execution, <= 1e-3 code CPU-vs-GPU parity, ZERO silent skips):
//
// Verifies:
//   1. TD-U2 exact active-set solver & TD-U3 per-band + joint statistical bounds (`kappa in {1, 2, 3}`)
//   2. TD-U1 `ComputeQualityEvidenceGpu` + `A2e` `kPass2EbPriorReduce` + one-sided `K=1` hold + `Legacy1x1` zero-init
//   3. TD-U4 band-dependent pooling + EB shrinkage + 2-frame same-role stats pool on luma AND 4:2:0 chroma
//   4. TD-U5 GPU `kPassAnchorHeldOutCheck` + `kPassAnchorDriftAudit` (`td.maps` real `kDrift != 0`) + 32-byte counter summary (`M-9`)
//   5. Owner D5 `TdV2GpuLateCarryController` (`2/3 -> 1/3 -> 0` non-double-decay, `Overlay`/`DynamicTexture` zeroing,
//      opposite-sign rejection, `OnLateResultArrived` stale discard, persistent VRAM budget & destructor cleanup)
//   6. GPU Chroma `6.2` hard FB gate + `6.6` `TdV2GpuEngine -> TdV2ChromaGpuEngine` luma-weight reuse & role mapping + bokeh straddle
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ChromaGpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2GpuLateCarry.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Settings.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
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
namespace TdV2UpgradeWarpNs
{
	using namespace TdV2;
	using namespace TdV2::Gpu;

	constexpr double kParityTol = 1e-3;

	struct WarpContext
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string logErrors;

		static void OnPlLog(void* priv, enum pl_log_level level, const char* msg)
		{
			if (priv != nullptr && msg != nullptr && level <= PL_LOG_ERR)
			{
				auto* self = static_cast<WarpContext*>(priv);
				if (self->logErrors.size() < 65536u)
				{
					self->logErrors += msg;
					self->logErrors += "\n";
				}
			}
		}

		WarpContext()
		{
			pl_log_params lp = pl_log_default_params;
			lp.log_level = PL_LOG_ERR;
			lp.log_cb = &WarpContext::OnPlLog;
			lp.log_priv = this;
			log = pl_log_create(PL_API_VER, &lp);
			Assert::IsNotNull(log, L"pl_log_create failed on WARP");

			pl_d3d11_params dp = pl_d3d11_default_params;
			dp.force_software = true;
			dp.allow_software = true;
			d3d11 = pl_d3d11_create(log, &dp);
			Assert::IsNotNull(d3d11, L"pl_d3d11_create (WARP) failed");
			gpu = d3d11->gpu;
			Assert::IsNotNull(gpu, L"WARP pl_gpu is null");

			dispatch = pl_dispatch_create(log, gpu);
			Assert::IsNotNull(dispatch, L"pl_dispatch_create failed on WARP");
		}

		~WarpContext()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
	};

	struct LumaGpuGuard
	{
		pl_gpu gpu;
		TdV2GpuEngine eng;
		LumaGpuGuard(pl_gpu g, const Config& c) : gpu(g), eng(c) {}
		~LumaGpuGuard() { eng.Release(gpu); }
	};

	struct ChromaGpuGuard
	{
		pl_gpu gpu;
		TdV2ChromaGpuEngine eng;
		ChromaGpuGuard(pl_gpu g, const Config& c) : gpu(g), eng(c) {}
		~ChromaGpuGuard() { eng.Release(gpu); }
	};

	static std::wstring WMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0)
	{
		char buf[384];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	static double MaxAbsPlaneDiff(const TdV2Plane& a, const TdV2Plane& b)
	{
		Assert::IsTrue(a.w == b.w && a.h == b.h && a.v.size() == b.v.size(), L"Plane dimension mismatch");
		double m = 0.0;
		for (size_t i = 0; i < a.v.size(); ++i)
			m = (std::max)(m, std::fabs(a.v[i] - b.v[i]));
		return m;
	}

	static TdV2Plane MakeAnalyticalMaster(int w, int h, double sx, double sy)
	{
		TdV2Plane p(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const double X = x + sx;
				const double Y = y + sy;
				const double edge = 95.0 / (1.0 + std::exp(-(X - 0.48 * w) * 1.25));
				const double tex = (X > 0.25 * w || Y > 0.25 * h)
					? (36.0 * std::sin(0.58 * X + 0.22 * Y) + 22.0 * std::cos(1.12 * Y - 0.41 * X))
					: 0.0;
				p.at(x, y) = (std::max)(64.0, (std::min)(940.0, 210.0 + 3.1 * X + 1.4 * Y + edge + tex));
			}
		}
		return p;
	}

	static TdV2Plane MakeValleyLoss(const TdV2Plane& m, double step, uint32_t seed)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> n(0.0, 0.4);
		TdV2Plane out(m.w, m.h, 0.0);
		for (int y = 0; y < m.h; ++y)
		{
			for (int x = 0; x < m.w; ++x)
			{
				double blur = 0.0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						blur += m.at((std::max)(0, (std::min)(m.w - 1, x + dx)), (std::max)(0, (std::min)(m.h - 1, y + dy)));
				blur /= 9.0;
				const double v = 0.45 * m.at(x, y) + 0.55 * blur;
				out.at(x, y) = std::round(v / step) * step + n(rng);
			}
		}
		return out;
	}

	static pl_tex UploadR32Tex(pl_gpu gpu, const TdV2Plane& p, bool storable = false, bool readable = false)
	{
		pl_tex_params tp{};
		tp.w = p.w; tp.h = p.h;
		tp.format = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
		tp.sampleable = true; tp.storable = storable; tp.host_writable = true; tp.host_readable = readable;
		pl_tex t = pl_tex_create(gpu, &tp);
		Assert::IsNotNull(t, L"UploadR32Tex create failed");
		std::vector<float> buf(p.v.size());
		for (size_t i = 0; i < p.v.size(); ++i) buf[i] = static_cast<float>(p.v[i]);
		pl_tex_transfer_params x{}; x.tex = t; x.ptr = buf.data();
		Assert::IsTrue(pl_tex_upload(gpu, &x), L"UploadR32Tex upload failed");
		return t;
	}

	static TdV2Plane DownloadR32Tex(pl_gpu gpu, pl_tex t)
	{
		TdV2Plane p(t->params.w, t->params.h, 0.0);
		std::vector<float> buf(p.v.size(), 0.0f);
		pl_tex_transfer_params x{}; x.tex = t; x.ptr = buf.data();
		Assert::IsTrue(pl_tex_download(gpu, &x), L"DownloadR32Tex failed");
		for (size_t i = 0; i < p.v.size(); ++i) p.v[i] = static_cast<double>(buf[i]);
		return p;
	}
} // namespace TdV2UpgradeWarpNs

TEST_CLASS(TdV2UpgradeGpuWarpTests)
{
public:
	// 1. CPU vs WARP GPU parity (<= 1e-3 code) for TD-U2 exact active-set solver & TD-U3 statistical joint bounds.
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuMatchesCpuOnTdU2ActiveSetSolverAndTdU3StatisticalJointBounds)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuMatchesCpuOnTdU2ActiveSetSolverAndTdU3StatisticalJointBounds)
	{
		using namespace TdV2UpgradeWarpNs;
		WarpContext ctx;
		constexpr int w = 64, h = 48, cellPx = 16;
		const int cw = w / cellPx, ch = h / cellPx;

		const TdV2Plane master = MakeAnalyticalMaster(w, h, 0.0, 0.0);
		const TdV2Plane cur = MakeValleyLoss(master, 9.0, 10u);
		// Build two strongly correlated faded past peaks (F and 1.9*F) so ActiveSetExact and LegacyClipRescale diverge!
		TdV2Plane fadePeak1 = master, fadePeak2 = master;
		for (size_t i = 0; i < master.v.size(); ++i)
		{
			fadePeak1.v[i] += 14.0;
			fadePeak2.v[i] += 27.0;
		}
		const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

		VpSkeleton::PlTexBackend backend(ctx.gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
			TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, cellPx, Preset::Reference), VpSkeleton::Tier::T2)));

		for (SolverMode sm : { SolverMode::ActiveSetExact, SolverMode::LegacyClipRescale })
		{
			for (double kappa : { 1.0, 2.0, 3.0 })
			{
				for (bool jointOn : { true, false })
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.solverMode = sm;
					cfg.pixelBoundKappa = kappa;
					cfg.enableJointCrossBandBound = jointOn;

					FrameProcessInput in{};
					in.seq = 1; in.receivedY = cur;
					in.hasStepPrior = true; in.estimatedStepDelta = 9.0;
					in.prevPeak.cleanPlane = &fadePeak1; in.prevPeak.motion = &stillMv;
					in.prevPeak2.cleanPlane = &fadePeak2; in.prevPeak2.motion = &stillMv;

					TdV2ReferenceEngine cpuEng(cfg);
					LumaGpuGuard gpuEng(ctx.gpu, cfg);
					arena.BeginFrame();
					const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in);
					const GpuProcessResult gpuRes = gpuEng.eng.ProcessFrame(ctx.gpu, ctx.dispatch, in, &arena);
					if (!gpuRes.succeeded)
					{
						Logger::WriteMessage(("GPU Luma Error: " + gpuRes.lastError + "\nPL_LOG:\n" + ctx.logErrors).c_str());
					}
					Assert::IsTrue(gpuRes.succeeded, L"GPU dispatch failed in TD-U2/U3 parity test");

					const double maxDiff = MaxAbsPlaneDiff(cpuRes.outputY, gpuRes.outputY);
					Assert::IsTrue(maxDiff <= kParityTol,
						WMsg("TD-U2/U3 CPU-vs-GPU maxDiff=%.6f exceeded 1e-3 (kappa=%.1f)", maxDiff, kappa).c_str());
				}
			}
		}
		arena.Release();
	}

	// 2. TD-U1 `ComputeQualityEvidenceGpu` + `A2e` `kPass2EbPriorReduce` + one-sided K=1 hold + Legacy1x1 zero-init parity.
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuQualityEvidenceA2eHistoryOneSidedHoldAndLegacy1x1ZeroInitParity)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuQualityEvidenceA2eHistoryOneSidedHoldAndLegacy1x1ZeroInitParity)
	{
		using namespace TdV2UpgradeWarpNs;
		WarpContext ctx;
		constexpr int w = 64, h = 48, cellPx = 16;
		const int cw = w / cellPx, ch = h / cellPx;

		// Part A: Direct GPU `ComputeQualityEvidenceGpu` parity on staircase + sharpness frames!
		{
			TdV2Plane stair(w, h, 200.0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					stair.at(x, y) = 200.0 + 10.0 * std::floor(x / 5.0) + 10.0 * std::floor(y / 5.0);

			const ThreeBandPlanes bCur = DecomposeActiveBands(stair, ActiveRect::Full(w, h));
			pl_tex_params tpRgba{};
			tpRgba.w = w; tpRgba.h = h;
			tpRgba.format = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
			tpRgba.sampleable = true; tpRgba.host_writable = true;
			pl_tex curBandsTex = pl_tex_create(ctx.gpu, &tpRgba);
			std::vector<float> bBuf(static_cast<size_t>(w * h) * 4u, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(w * h); ++i)
			{
				bBuf[i * 4u + 0u] = static_cast<float>(bCur.L.v[i]);
				bBuf[i * 4u + 1u] = static_cast<float>(bCur.M.v[i]);
				bBuf[i * 4u + 2u] = static_cast<float>(bCur.H.v[i]);
				bBuf[i * 4u + 3u] = static_cast<float>(bCur.D3.v[i]);
			}
			pl_tex_transfer_params xb{}; xb.tex = curBandsTex; xb.ptr = bBuf.data();
			Assert::IsTrue(pl_tex_upload(ctx.gpu, &xb));

			pl_tex curCleanTex = UploadR32Tex(ctx.gpu, stair);
			pl_tex_params tpPrior{};
			tpPrior.w = cw; tpPrior.h = ch; tpPrior.format = tpRgba.format;
			tpPrior.sampleable = true; tpPrior.host_writable = true;
			pl_tex priorTex = pl_tex_create(ctx.gpu, &tpPrior);

			pl_tex_params tpOut{};
			tpOut.w = cw; tpOut.h = ch; tpOut.format = tpRgba.format;
			tpOut.sampleable = true; tpOut.storable = true; tpOut.host_readable = true;
			pl_tex outQualTex = pl_tex_create(ctx.gpu, &tpOut);
			std::vector<float> zeroCell(static_cast<size_t>(cw * ch) * 4u, 0.0f);
			pl_tex_transfer_params xcp{}; xcp.tex = priorTex; xcp.ptr = zeroCell.data();
			Assert::IsTrue(pl_tex_upload(ctx.gpu, &xcp));

			LumaGpuGuard gEng(ctx.gpu, MakePreset(Preset::Reference, Mode::On));
			AlignedSharpnessSummary gpuSharp{};
			double gpuStep = 0.0;
			Assert::IsTrue(gEng.eng.ComputeQualityEvidenceGpu(
				ctx.gpu, ctx.dispatch, curBandsTex, nullptr, curCleanTex, priorTex,
				nullptr, nullptr, outQualTex, w, h, ActiveRect::Full(w, h), 10, &gpuSharp, &gpuStep));

			std::vector<float> qRead(static_cast<size_t>(cw * ch) * 4u, -1.0f);
			pl_tex_transfer_params xqr{}; xqr.tex = outQualTex; xqr.ptr = qRead.data();
			(void)pl_tex_download(ctx.gpu, &xqr);
			TdV2NonBlockStepEstimator cpuNb;
			const CodingGridSummary cpuGrid = cpuNb.MeasureWithOverlay(stair.v.data(), nullptr, nullptr, w, h, 10, nullptr, cellPx, nullptr);
			if (!(gpuStep >= 8.0 && std::fabs(gpuStep - cpuGrid.stepMean) <= 1.5))
			{
				char qdbg[512];
				std::snprintf(qdbg, sizeof(qdbg), "QE cells[0..3]=(%.3f,%.3f,%.3f,%.1f) (%.3f,%.3f,%.3f,%.1f) (%.3f,%.3f,%.3f,%.1f) (%.3f,%.3f,%.3f,%.1f)\nPL_LOG:\n%s",
					qRead[0], qRead[1], qRead[2], qRead[3],
					qRead[4], qRead[5], qRead[6], qRead[7],
					qRead[8], qRead[9], qRead[10], qRead[11],
					qRead[12], qRead[13], qRead[14], qRead[15], ctx.logErrors.c_str());
				Logger::WriteMessage(qdbg);
			}
			Assert::IsTrue(gpuStep >= 8.0 && std::fabs(gpuStep - cpuGrid.stepMean) <= 1.5,
				WMsg("ComputeQualityEvidenceGpu step=%.3f vs CPU=%.3f", gpuStep, cpuGrid.stepMean).c_str());

			pl_tex_destroy(ctx.gpu, &curBandsTex);
			pl_tex_destroy(ctx.gpu, &curCleanTex);
			pl_tex_destroy(ctx.gpu, &priorTex);
			pl_tex_destroy(ctx.gpu, &outQualTex);
		}

		// Part B: Two-sided K=2 (`hasStepPrior = false`) -> one-sided K=1 A2e rolling-median hold parity + Legacy1x1 zero-init!
		const TdV2Plane m0 = MakeAnalyticalMaster(w, h, 0.0, 0.0);
		const TdV2Plane m1 = MakeAnalyticalMaster(w, h, 0.35, -0.20);
		const TdV2Plane m2 = MakeAnalyticalMaster(w, h, 0.70, -0.40);
		const TdV2Plane m3 = MakeAnalyticalMaster(w, h, 1.05, -0.60);
		const TdV2Plane pk0 = m0, pk3 = m3;
		const TdV2Plane val1 = MakeValleyLoss(m1, 10.0, 31u);
		const TdV2Plane val2 = MakeValleyLoss(m2, 10.0, 32u);

		const CellMotionField mv1To0 = CellMotionField::Uniform(cw, ch, 0.35, -0.20, 0.35, -0.20, RegionMotionClass::Camera);
		const CellMotionField mv1To3 = CellMotionField::Uniform(cw, ch, -0.70, 0.40, -0.70, 0.40, RegionMotionClass::Camera);
		const CellMotionField mv2To0 = CellMotionField::Uniform(cw, ch, 0.70, -0.40, 0.70, -0.40, RegionMotionClass::Camera);

		VpSkeleton::PlTexBackend backend(ctx.gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
			TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, cellPx, Preset::Reference), VpSkeleton::Tier::T2)));

		Config cfgA2e = MakePreset(Preset::Reference, Mode::On);
		cfgA2e.pixelBoundKappa = 2.0;
		cfgA2e.qualitySignalMode = QualitySignalMode::AlignedSharpnessA2e;
		cfgA2e.enableA2eEmpiricalPrior = true;

		TdV2ReferenceEngine cpuA2e(cfgA2e);
		LumaGpuGuard gpuA2e(ctx.gpu, cfgA2e);

		FrameProcessInput in1{};
		in1.seq = 1; in1.receivedY = val1; in1.hasStepPrior = false;
		in1.frameRole = QualityPattern::FrameRole::NonReference;
		in1.prevPeak.cleanPlane = &pk0; in1.prevPeak.motion = &mv1To0;
		in1.nextPeak.cleanPlane = &pk3; in1.nextPeak.motion = &mv1To3;
		arena.BeginFrame();
		const FrameProcessResult c1 = cpuA2e.ProcessFrame(in1);
		const GpuProcessResult g1 = gpuA2e.eng.ProcessFrame(ctx.gpu, ctx.dispatch, in1, &arena);
		Assert::IsTrue(g1.succeeded && MaxAbsPlaneDiff(c1.outputY, g1.outputY) <= kParityTol,
			WMsg("K=2 A2e parity diff=%.6f", MaxAbsPlaneDiff(c1.outputY, g1.outputY)).c_str());

		// Frame 2 is one-sided K=1 (`nextPeak` null): GPU must hold A2e from frame 1 matching CPU within 1e-3!
		FrameProcessInput in2{};
		in2.seq = 2; in2.receivedY = val2; in2.hasStepPrior = false;
		in2.frameRole = QualityPattern::FrameRole::NonReference;
		in2.prevPeak.cleanPlane = &pk0; in2.prevPeak.motion = &mv2To0;
		arena.BeginFrame();
		const FrameProcessResult c2 = cpuA2e.ProcessFrame(in2);
		const GpuProcessResult g2 = gpuA2e.eng.ProcessFrame(ctx.gpu, ctx.dispatch, in2, &arena);
		Assert::IsTrue(g2.succeeded && MaxAbsPlaneDiff(c2.outputY, g2.outputY) <= kParityTol,
			WMsg("One-sided K=1 held A2e parity diff=%.6f", MaxAbsPlaneDiff(c2.outputY, g2.outputY)).c_str());

		// Also verify Legacy1x1 (EB shrinkage OFF, A2e OFF) initializes `ebPriorTex_` cleanly to zero on first frame!
		TdV2RuntimeSettings legRs{};
		legRs.mode = Mode::On; legRs.preset = Preset::Reference;
		legRs.statsPoolingMode = StatsPoolingMode::Legacy1x1;
		legRs.qualitySignalMode = QualitySignalMode::LegacyBlockEdge8px;
		const Config cfgLeg = BuildTdV2Config(legRs, false);
		FrameProcessInput inLeg = in1;
		inLeg.hasStepPrior = true; inLeg.estimatedStepDelta = 8.0;
		arena.BeginFrame();
		const FrameProcessResult cLeg = TdV2ReferenceEngine(cfgLeg).ProcessFrame(inLeg);
		LumaGpuGuard gpuLeg(ctx.gpu, cfgLeg);
		const GpuProcessResult gLeg = gpuLeg.eng.ProcessFrame(ctx.gpu, ctx.dispatch, inLeg, &arena);
		Assert::IsTrue(gLeg.succeeded && MaxAbsPlaneDiff(cLeg.outputY, gLeg.outputY) <= kParityTol,
			WMsg("Legacy1x1 zero-init parity diff=%.6f", MaxAbsPlaneDiff(cLeg.outputY, gLeg.outputY)).c_str());

		arena.Release();
	}

	// 3. TD-U4 band-dependent pooling + EB shrinkage + 2-frame same-role stats parity on BOTH luma AND 4:2:0 chroma.
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuBandPoolingEbShrinkageAndTwoFrameSameRoleStatsParityLumaAndChroma420)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuBandPoolingEbShrinkageAndTwoFrameSameRoleStatsParityLumaAndChroma420)
	{
		using namespace TdV2UpgradeWarpNs;
		WarpContext ctx;
		constexpr int w = 64, h = 48, cellPx = 16;
		const int cw = w / cellPx, ch = h / cellPx;
		const int cW = w / 2, cH = h / 2;

		const TdV2Plane mY = MakeAnalyticalMaster(w, h, 0.0, 0.0);
		const TdV2Plane vY1 = MakeValleyLoss(mY, 8.0, 41u);
		const TdV2Plane vY2 = MakeValleyLoss(mY, 8.0, 42u);
		const TdV2Plane mCb = MakeAnalyticalMaster(cW, cH, 2.0, 1.0);
		const TdV2Plane mCr = MakeAnalyticalMaster(cW, cH, 5.0, 3.0);
		const TdV2Plane vCb = MakeValleyLoss(mCb, 8.0, 43u);
		const TdV2Plane vCr = MakeValleyLoss(mCr, 8.0, 44u);

		// Independent low-noise clean peak realizations so EB shrinkage + A1 cross-moments are non-collinear!
		auto addPeakJitter = [](const TdV2Plane& base, uint32_t seed) -> TdV2Plane
		{
			std::mt19937 rng(seed);
			std::normal_distribution<double> nd(0.0, 0.35);
			TdV2Plane p = base;
			for (double& v : p.v) v += nd(rng);
			return p;
		};
		const TdV2Plane pkPrevY = addPeakJitter(mY, 141u);
		const TdV2Plane pkNextY = addPeakJitter(mY, 142u);
		const TdV2Plane pkPrevCb = addPeakJitter(mCb, 143u);
		const TdV2Plane pkNextCb = addPeakJitter(mCb, 144u);
		const TdV2Plane pkPrevCr = addPeakJitter(mCr, 145u);
		const TdV2Plane pkNextCr = addPeakJitter(mCr, 146u);

		const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

		VpSkeleton::PlTexBackend backend(ctx.gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> lArena(backend), cArena(backend);
		Assert::IsTrue(lArena.Reserve(VpSkeleton::PlanArena(
			TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, cellPx, Preset::Reference), VpSkeleton::Tier::T2)));
		Assert::IsTrue(cArena.Reserve(VpSkeleton::PlanArena(
			TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv420, cellPx),
			VpSkeleton::Tier::T2)));

		Config cfg = MakePreset(Preset::Reference, Mode::On);
		cfg.enableBandDependentPool = true;
		cfg.enableEmpiricalBayesShrinkage = true;
		cfg.enableTwoFrameSameRoleStatsPool = true;
		cfg.pixelBoundKappa = 2.0;

		TdV2ReferenceEngine cpuL(cfg);
		LumaGpuGuard gpuL(ctx.gpu, cfg);
		for (int f = 0; f < 2; ++f)
		{
			FrameProcessInput in{};
			in.seq = static_cast<uint64_t>(f + 1);
			in.receivedY = (f == 0) ? vY1 : vY2;
			in.hasStepPrior = true; in.estimatedStepDelta = 8.0;
			in.frameRole = QualityPattern::FrameRole::NonReference;
			in.prevPeak.cleanPlane = &pkPrevY; in.prevPeak.motion = &stillMv;
			in.nextPeak.cleanPlane = &pkNextY; in.nextPeak.motion = &stillMv;
			lArena.BeginFrame();
			const FrameProcessResult cr = cpuL.ProcessFrame(in);
			const GpuProcessResult gr = gpuL.eng.ProcessFrame(ctx.gpu, ctx.dispatch, in, &lArena);
			Assert::IsTrue(gr.succeeded && MaxAbsPlaneDiff(cr.outputY, gr.outputY) <= kParityTol,
				WMsg("TD-U4 luma frame %d diff=%.6f", f, MaxAbsPlaneDiff(cr.outputY, gr.outputY)).c_str());
		}

		// 4:2:0 Chroma BandShrink + EB shrinkage CPU-vs-GPU parity!
		ChromaProcessInput cIn{};
		cIn.seq = 1; cIn.lumaW = w; cIn.lumaH = h;
		cIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
		cIn.codedSubsampling = ChromaSubsampling::Yuv420;
		cIn.gridStatus = ChromaGridStatus::Identified;
		cIn.receivedCb = vCb; cIn.receivedCr = vCr;
		cIn.hasStepPrior = true; cIn.estimatedStepDelta = 8.0;
		cIn.frameRole = QualityPattern::FrameRole::NonReference;
		cIn.prevPeak.cleanCb = &pkPrevCb; cIn.prevPeak.cleanCr = &pkPrevCr; cIn.prevPeak.lumaMotion = &stillMv;
		cIn.nextPeak.cleanCb = &pkNextCb; cIn.nextPeak.cleanCr = &pkNextCr; cIn.nextPeak.lumaMotion = &stillMv;

		TdV2ChromaEngine cpuC(cfg);
		ChromaGpuGuard gpuC(ctx.gpu, cfg);
		cArena.BeginFrame();
		const ChromaProcessResult ccRes = cpuC.ProcessFrame(cIn);
		const ChromaGpuProcessResult gcRes = gpuC.eng.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &cArena);
		Assert::IsTrue(gcRes.succeeded);
		Assert::IsTrue(MaxAbsPlaneDiff(ccRes.outputCb, gcRes.outputCb) <= kParityTol &&
		               MaxAbsPlaneDiff(ccRes.outputCr, gcRes.outputCr) <= kParityTol,
			WMsg("TD-U4 4:2:0 chroma diff Cb=%.6f Cr=%.6f",
				MaxAbsPlaneDiff(ccRes.outputCb, gcRes.outputCb), MaxAbsPlaneDiff(ccRes.outputCr, gcRes.outputCr)).c_str());

		lArena.Release();
		cArena.Release();
	}

	// 4. TD-U5 GPU `kPassAnchorHeldOutCheck` + `kPassAnchorDriftAudit` (`td.maps` real `kDrift != 0`) + 32-byte counter summary (`M-9`).
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuAnchoredCarryHeldOutCheckRealKDriftTdMapsAnd32ByteCounterSummary)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuAnchoredCarryHeldOutCheckRealKDriftTdMapsAnd32ByteCounterSummary)
	{
		using namespace TdV2UpgradeWarpNs;
		WarpContext ctx;
		constexpr int w = 64, h = 48, cellPx = 16;
		const int cw = w / cellPx, ch = h / cellPx;

		VpSkeleton::PlTexBackend backend(ctx.gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
			TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, cellPx, Preset::Reference), VpSkeleton::Tier::T2)));

		Config cfg = MakePreset(Preset::Reference, Mode::On);
		cfg.enableAnchoredCarry = true;
		cfg.pixelBoundKappa = 2.0;

		TdV2ReferenceEngine cpuEng(cfg);
		LumaGpuGuard gpuEng(ctx.gpu, cfg);

		const TdV2Plane m0 = MakeAnalyticalMaster(w, h, 0.0, 0.0);
		const TdV2Plane m1 = MakeAnalyticalMaster(w, h, 0.30, 0.0);
		const TdV2Plane m2 = MakeAnalyticalMaster(w, h, 0.60, 0.0);
		const TdV2Plane m3 = MakeAnalyticalMaster(w, h, 0.90, 0.0);
		const TdV2Plane v1 = MakeValleyLoss(m1, 8.0, 51u);
		const TdV2Plane v2 = MakeValleyLoss(m2, 8.0, 52u);

		// Seq 0: seed confirmed peak.
		FrameProcessInput s0{}; s0.seq = 0; s0.receivedY = m0; s0.isConfirmedPeak = true;
		(void)cpuEng.ProcessFrame(s0);
		arena.BeginFrame();
		Assert::IsTrue(gpuEng.eng.ProcessFrame(ctx.gpu, ctx.dispatch, s0, &arena).succeeded);

		// Seq 1: age 1 (`prevPeak == m0`) -> non-self-instrumentation parity!
		const CellMotionField mv1To0 = CellMotionField::Uniform(cw, ch, 0.30, 0.0, 0.30, 0.0, RegionMotionClass::Camera);
		FrameProcessInput s1{}; s1.seq = 1; s1.receivedY = v1; s1.hasStepPrior = true; s1.estimatedStepDelta = 8.0;
		s1.prevPeak.cleanPlane = &m0; s1.prevPeak.motion = &mv1To0;
		s1.hasAnchorComposedMotion = true; s1.anchorComposedMotion = mv1To0;
		arena.BeginFrame();
		const FrameProcessResult c1 = cpuEng.ProcessFrame(s1);
		const GpuProcessResult g1 = gpuEng.eng.ProcessFrame(ctx.gpu, ctx.dispatch, s1, &arena);
		Assert::IsTrue(g1.succeeded && MaxAbsPlaneDiff(c1.outputY, g1.outputY) <= kParityTol);

		// Seq 2: age 2, inject bad composed motion (+10 px) on cell (1, 1) to test GPU `kPassAnchorHeldOutCheck` parity!
		const CellMotionField mv2To0 = CellMotionField::Uniform(cw, ch, 0.60, 0.0, 0.60, 0.0, RegionMotionClass::Camera);
		const CellMotionField mv2To3 = CellMotionField::Uniform(cw, ch, -0.30, 0.0, -0.30, 0.0, RegionMotionClass::Camera);
		CellMotionField badAnc = mv2To0;
		badAnc.vx[1 * cw + 1] = 10.0;
		FrameProcessInput s2{}; s2.seq = 2; s2.receivedY = v2; s2.hasStepPrior = true; s2.estimatedStepDelta = 8.0;
		s2.nextPeak.cleanPlane = &m3; s2.nextPeak.motion = &mv2To3;
		s2.hasAnchorComposedMotion = true; s2.anchorComposedMotion = badAnc;
		arena.BeginFrame();
		const FrameProcessResult c2 = cpuEng.ProcessFrame(s2);
		const GpuProcessResult g2 = gpuEng.eng.ProcessFrame(ctx.gpu, ctx.dispatch, s2, &arena);
		Assert::IsTrue(g2.succeeded && MaxAbsPlaneDiff(c2.outputY, g2.outputY) <= kParityTol,
			WMsg("GPU held-out check parity diff=%.6f", MaxAbsPlaneDiff(c2.outputY, g2.outputY)).c_str());

		// Seq 3: confirmed peak with +0.35 px composed drift (>0.25 px) -> verify GPU writes REAL `kDrift > 0.25` into td.maps!
		const CellMotionField mv3To0True = CellMotionField::Uniform(cw, ch, 0.90, 0.0, 0.90, 0.0, RegionMotionClass::Camera);
		const CellMotionField mv3To0Bias = CellMotionField::Uniform(cw, ch, 1.25, 0.0, 1.25, 0.0, RegionMotionClass::Camera);
		FrameProcessInput s3{}; s3.seq = 3; s3.receivedY = m3; s3.isConfirmedPeak = true;
		s3.hasStepPrior = true; s3.estimatedStepDelta = 8.0;
		s3.prevPeak.cleanPlane = &m0; s3.prevPeak.motion = &mv3To0True;
		s3.hasAnchorComposedMotion = true; s3.anchorComposedMotion = mv3To0Bias;
		s3.hasDirectPeakToPeakMotion = true; s3.directPeakToPeakMotion = mv3To0True;
		arena.BeginFrame();
		const FrameProcessResult c3 = cpuEng.ProcessFrame(s3);
		const GpuProcessResult g3 = gpuEng.eng.ProcessFrame(ctx.gpu, ctx.dispatch, s3, &arena);
		Assert::IsTrue(g3.succeeded);
		Assert::IsTrue(g3.tdMaps[0].f[TdMapsView::kDrift] > 0.34f &&
		               std::fabs(g3.tdMaps[0].f[TdMapsView::kDrift] - c3.tdMaps[0].f[TdMapsView::kDrift]) < 1e-3f,
			WMsg("GPU td.maps kDrift=%.4f vs CPU=%.4f", g3.tdMaps[0].f[TdMapsView::kDrift], c3.tdMaps[0].f[TdMapsView::kDrift]).c_str());

		// M-9: Zero-copy `ProcessFrameGpu` (`readbackCellDiagnostics = false`, `enableGpuCounterSummary = true`)
		// MUST populate live `cellsLimited` / `focusVetoCells` & `StandInRegistry` via the 32-byte `counterSummaryTex_`!
		{
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> rtArena(backend);
			Assert::IsTrue(rtArena.Reserve(VpSkeleton::PlanArena(
				TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, cellPx, Preset::RealTime), VpSkeleton::Tier::T2)));

			Config zCfg = MakePreset(Preset::RealTime, Mode::On);
			zCfg.pixelBoundKappa = 1.0;
			zCfg.enableGpuCounterSummary = true;
			LumaGpuGuard zEng(ctx.gpu, zCfg);

			// Pure additive coding noise (zero optical blur so auto rack-focus stays 0 on cells 1..11, isolating expFocus[0]=1!)
			TdV2Plane noisyM1 = m1;
			{
				std::mt19937 rngN(991u);
				std::normal_distribution<double> ndN(0.0, 6.0);
				for (double& v : noisyM1.v) v += ndN(rngN);
			}
			pl_tex recTex = UploadR32Tex(ctx.gpu, noisyM1);
			pl_tex refTex = UploadR32Tex(ctx.gpu, m1);
			pl_tex outTex = UploadR32Tex(ctx.gpu, noisyM1, /*storable=*/true, /*readable=*/true);
			std::vector<uint8_t> expFocus(static_cast<size_t>(cw * ch), 0u);
			expFocus[0] = 1u; // Trigger 1 explicit focus-vetoed cell!

			const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			GpuFrameInput zIn{};
			zIn.seq = 1; zIn.w = w; zIn.h = h;
			zIn.receivedYTex = recTex;
			zIn.prevPeak.cleanTex = refTex; zIn.prevPeak.motion = &stillMv;
			zIn.nextPeak.cleanTex = refTex; zIn.nextPeak.motion = &stillMv;
			zIn.explicitFocusChangeCells = &expFocus;
			zIn.hasStepPrior = true; zIn.estimatedStepDelta = 2.0; // Tight step so statistical bound limits cells!

			GpuFrameOutput zOut{};
			zOut.outputYTex = outTex;
			zOut.readbackCellDiagnostics = false; // Zero-copy live mode!
			StandInRegistry zStandIns;
			rtArena.BeginFrame();
			Assert::IsTrue(zEng.eng.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zIn, zOut, &rtArena, &zStandIns),
				L"M-9 ProcessFrameGpu failed");
			Assert::AreEqual(static_cast<size_t>(1u), zOut.focusVetoCells, L"32-byte counter summary must count focusVetoCells in zero-copy mode");
			Assert::IsTrue(zOut.cellsLimited > 0u && zStandIns.IsActive(StandInKind::TransformUnknownBound),
				L"32-byte counter summary must count cellsLimited and engage TransformUnknownBound in zero-copy mode");

			rtArena.Release();
			pl_tex_destroy(ctx.gpu, &recTex);
			pl_tex_destroy(ctx.gpu, &refTex);
			pl_tex_destroy(ctx.gpu, &outTex);
		}

		arena.Release();
	}

	// 5. Owner D5 `TdV2GpuLateCarryController`: 2/3 -> 1/3 -> 0 non-double-decay, Overlay/DynamicTexture zeroing,
	//    opposite-sign rejection, `OnLateResultArrived` stale discard, and exact persistent VRAM accounting.
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuLateCarryControllerDecayGuardsStaleDiscardAndVramBudget)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuLateCarryControllerDecayGuardsStaleDiscardAndVramBudget)
	{
		using namespace TdV2UpgradeWarpNs;
		WarpContext ctx;
		constexpr int w = 64, h = 48, cellPx = 16;
		const int cw = w / cellPx, ch = h / cellPx;

		TdV2GpuLateCarryController lc;
		StandInRegistry standIns;

		// Seed on-time frame at seq=10: curClean = 200.0, curDelta = +3.0 (so target clean was 203.0).
		TdV2Plane clean10(w, h, 200.0);
		TdV2Plane delta10(w, h, 3.0);
		pl_tex clean10Tex = UploadR32Tex(ctx.gpu, clean10);
		pl_tex delta10Tex = UploadR32Tex(ctx.gpu, delta10);
		pl_tex outDeltaTex = UploadR32Tex(ctx.gpu, TdV2Plane(w, h, 0.0), /*storable=*/true, /*readable=*/true);

		Assert::IsTrue(lc.OnOnTimeFrameGpu(ctx.gpu, ctx.dispatch, 10, delta10Tex, clean10Tex, &standIns));
		Assert::IsTrue(lc.HasStoredDelta() && lc.LateStreak() == 0);

		// Build cell motion with cell (0,0) = Overlay, cell (1,0) = DynamicTexture, and remaining cells = Camera (0,0).
		CellMotionField mv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
		mv.cls[0] = RegionMotionClass::Overlay;
		mv.cls[1] = RegionMotionClass::DynamicTexture;

		// Late frame 1 (seq=11): curClean = 199.0 -> desiredDir = wClean(200) - curClean(199) = +4.0? Wait:
		// wClean is snapshot clean10 = 200.0; set curClean11 = 196.0 so desiredDir = 200 - 196 = +4.0 (same sign as +3.0, > +2.0!).
		// stepTotalVar = 16.0, kappa = 1.0 -> statistical bound = 4.0 >= 2.0, knee = 9*16 = 144 >= 16 (no Huber taper).
		pl_tex clean11Tex = UploadR32Tex(ctx.gpu, TdV2Plane(w, h, 196.0));
		GpuLateCarryInput in11{};
		in11.seq = 11;
		in11.curCleanTex = clean11Tex;
		in11.cpuMotion = &mv;
		in11.stepTotalVar = 16.0;
		in11.pixelBoundKappa = 1.0;

		const GpuLateCarryDecision d1 = lc.OnLateFrameGpu(ctx.gpu, ctx.dispatch, in11, outDeltaTex, &standIns);
		Assert::IsTrue(d1.carried && !d1.expired && d1.lateStreak == 1 && std::fabs(d1.decayScale - (2.0 / 3.0)) < 1e-6);
		const TdV2Plane out1 = DownloadR32Tex(ctx.gpu, outDeltaTex);
		Assert::AreEqual(0.0, out1.at(4, 4), 1e-6, L"Overlay cell (0,0) must be strictly zeroed on late carry");
		Assert::AreEqual(0.0, out1.at(20, 4), 1e-6, L"DynamicTexture cell (1,0) must be strictly zeroed on late carry");
		Assert::AreEqual(2.0, out1.at(40, 24), 1e-4, L"Hop 1 Camera cell must apply 2/3 * 3.0 = +2.0");

		// Check persistent VRAM accounting now that both ping-pong slots + CPU-fallback motion textures are allocated!
		Assert::AreEqual(
			TdV2GpuLateCarryController::EstimatedPersistentVramBytes(w, h, cellPx),
			lc.AllocatedPersistentVramBytes(),
			L"AllocatedPersistentVramBytes must match EstimatedPersistentVramBytes bit-for-bit");

		// Late frame 2 (seq=12): NON-DOUBLE-DECAY PROOF!
		// Hop 2 MUST output 1/3 * 3.0 = +1.000 (NOT 1/3 * 2.0 = 0.6667)!
		GpuLateCarryInput in12 = in11;
		in12.seq = 12;
		const GpuLateCarryDecision d2 = lc.OnLateFrameGpu(ctx.gpu, ctx.dispatch, in12, outDeltaTex, &standIns);
		Assert::IsTrue(d2.carried && !d2.expired && d2.lateStreak == 2 && std::fabs(d2.decayScale - (1.0 / 3.0)) < 1e-6);
		const TdV2Plane out2 = DownloadR32Tex(ctx.gpu, outDeltaTex);
		Assert::AreEqual(1.0, out2.at(40, 24), 1e-4, L"Hop 2 must apply 1/3 of base delta (+1.0), NOT double-decayed 2/9 (+0.667)!");

		// Late frame 3 (seq=13): cap reached -> expired, output delta strictly zeroed!
		GpuLateCarryInput in13 = in11;
		in13.seq = 13;
		const GpuLateCarryDecision d3 = lc.OnLateFrameGpu(ctx.gpu, ctx.dispatch, in13, outDeltaTex, &standIns);
		Assert::IsTrue(!d3.carried && d3.expired && standIns.IsActive(StandInKind::LateCarryExpired));
		const TdV2Plane out3 = DownloadR32Tex(ctx.gpu, outDeltaTex);
		Assert::AreEqual(0.0, out3.at(40, 24), 1e-6, L"Expired hop 3 must zero output delta");

		// Opposite-sign rejection: re-seed on-time at seq=20 with +3.0, then present curClean = 205.0 (> 200.0 -> desiredDir = -5.0 < 0)!
		Assert::IsTrue(lc.OnOnTimeFrameGpu(ctx.gpu, ctx.dispatch, 20, delta10Tex, clean10Tex, &standIns));
		pl_tex cleanOppTex = UploadR32Tex(ctx.gpu, TdV2Plane(w, h, 205.0));
		GpuLateCarryInput inOpp = in11;
		inOpp.seq = 21;
		inOpp.curCleanTex = cleanOppTex;
		(void)lc.OnLateFrameGpu(ctx.gpu, ctx.dispatch, inOpp, outDeltaTex, &standIns);
		const TdV2Plane outOpp = DownloadR32Tex(ctx.gpu, outDeltaTex);
		Assert::AreEqual(0.0, outOpp.at(40, 24), 1e-6, L"Opposite-sign desiredDir must reject carried delta to 0.0");

		// Stale late-arrival guard (`OnLateResultArrived`): resultSeq <= lastPresentedSeq (21) is discarded visibly!
		Assert::IsFalse(lc.OnLateResultArrived(/*resultSeq=*/20, /*lastPresentedSeq=*/21, &standIns));
		Assert::IsFalse(lc.OnLateResultArrived(/*resultSeq=*/21, /*lastPresentedSeq=*/21, &standIns));
		Assert::AreEqual(uint64_t(2u), lc.LateArrivalsDiscarded());
		Assert::IsTrue(lc.OnLateResultArrived(/*resultSeq=*/22, /*lastPresentedSeq=*/21, &standIns));

		lc.Reset(ctx.gpu);
		Assert::AreEqual(uint64_t(0u), lc.AllocatedPersistentVramBytes(), L"Reset(gpu) must free all persistent late-carry textures");

		for (pl_tex* t : { &clean10Tex, &delta10Tex, &outDeltaTex, &clean11Tex, &cleanOppTex })
			pl_tex_destroy(ctx.gpu, t);
	}

	// 6. GPU Chroma `6.2` hard FB gate + bokeh boundary straddle + `6.6` `TdV2GpuEngine -> TdV2ChromaGpuEngine` luma-weight reuse!
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaHardFbGateLumaWeightRoleMappingAndBokehBoundaryStraddleParity)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuChromaHardFbGateLumaWeightRoleMappingAndBokehBoundaryStraddleParity)
	{
		using namespace TdV2UpgradeWarpNs;
		WarpContext ctx;
		constexpr int w = 64, h = 48, cellPx = 16;
		const int cw = w / cellPx, ch = h / cellPx;
		const int cW = w / 2, cH = h / 2;
		const size_t nCells = static_cast<size_t>(cw * ch);

		VpSkeleton::PlTexBackend backend(ctx.gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> lArena(backend), cArena(backend);
		Assert::IsTrue(lArena.Reserve(VpSkeleton::PlanArena(
			TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, cellPx, Preset::Reference), VpSkeleton::Tier::T2)));
		Assert::IsTrue(cArena.Reserve(VpSkeleton::PlanArena(
			TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv420, cellPx),
			VpSkeleton::Tier::T2)));

		const TdV2Plane mY = MakeAnalyticalMaster(w, h, 0.0, 0.0);
		const TdV2Plane vY = MakeValleyLoss(mY, 9.0, 61u);
		const TdV2Plane mCb = MakeAnalyticalMaster(cW, cH, 1.0, 2.0);
		const TdV2Plane mCr = MakeAnalyticalMaster(cW, cH, 4.0, 1.0);
		const TdV2Plane vCb = MakeValleyLoss(mCb, 9.0, 62u);
		const TdV2Plane vCr = MakeValleyLoss(mCr, 9.0, 63u);

		// (A) 6.2 Hard >1.0 px FB occlusion gate on GPU chroma (vs enableHardFbOcclusionGate=false negative control) + CPU parity.
		{
			auto addChromaJitter = [](const TdV2Plane& base, uint32_t seed) -> TdV2Plane
			{
				std::mt19937 rng(seed);
				std::normal_distribution<double> nd(0.0, 0.35);
				TdV2Plane p = base;
				for (double& v : p.v) v += nd(rng);
				return p;
			};
			const TdV2Plane mCbPrev = addChromaJitter(mCb, 261u);
			const TdV2Plane mCbNext = addChromaJitter(mCb, 262u);
			const TdV2Plane mCrPrev = addChromaJitter(mCr, 263u);
			const TdV2Plane mCrNext = addChromaJitter(mCr, 264u);

			CellMotionField occMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
			occMv.confidence.assign(nCells, 0.85);
			occMv.fbErrorPx.assign(nCells, 1.40);

			for (bool hardGate : { true, false })
			{
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableHardFbOcclusionGate = hardGate;
				cfg.pixelBoundKappa = 2.0;

				ChromaProcessInput cIn{};
				cIn.seq = 1; cIn.lumaW = w; cIn.lumaH = h;
				cIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
				cIn.codedSubsampling = ChromaSubsampling::Yuv420;
				cIn.gridStatus = ChromaGridStatus::Identified;
				cIn.receivedCb = vCb; cIn.receivedCr = vCr;
				cIn.hasStepPrior = true; cIn.estimatedStepDelta = 24.0;
				cIn.prevPeak.cleanCb = &mCbPrev; cIn.prevPeak.cleanCr = &mCrPrev; cIn.prevPeak.lumaMotion = &occMv;
				cIn.nextPeak.cleanCb = &mCbNext; cIn.nextPeak.cleanCr = &mCrNext; cIn.nextPeak.lumaMotion = &occMv;

				cArena.BeginFrame();
				const ChromaProcessResult cpuR = TdV2ChromaEngine(cfg).ProcessFrame(cIn);
				ChromaGpuGuard gC(ctx.gpu, cfg);
				const ChromaGpuProcessResult gpuR = gC.eng.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &cArena);
				if (!gpuR.succeeded)
				{
					Logger::WriteMessage(("GPU Chroma Error: " + gpuR.lastError + "\nPL_LOG:\n" + ctx.logErrors).c_str());
				}
				Assert::IsTrue(gpuR.succeeded && MaxAbsPlaneDiff(cpuR.outputCb, gpuR.outputCb) <= kParityTol);
				if (hardGate)
					Assert::AreEqual(0.0, MaxAbsPlaneDiff(vCb, gpuR.outputCb), 1e-12, L"GPU chroma hard FB gate must zero delta");
				else
					Assert::IsTrue(MaxAbsPlaneDiff(vCb, gpuR.outputCb) > 0.25, L"Negative control (hard FB gate OFF) blends nonzero chroma");
			}
		}

		// (B) Bokeh sharp-to-defocus boundary straddle CPU-vs-GPU parity on chroma (`BokehMode::LowBandCellOnly`).
		{
			const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			std::vector<uint8_t> defocus(nCells, 0u);
			for (int cy = 0; cy < ch; ++cy)
				for (int cx = 0; cx < cw / 2; ++cx)
					defocus[static_cast<size_t>(cy) * cw + cx] = 1u;

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.bokehMode = BokehMode::LowBandCellOnly;
			cfg.pixelBoundKappa = 2.0;

			ChromaProcessInput cIn{};
			cIn.seq = 1; cIn.lumaW = w; cIn.lumaH = h;
			cIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
			cIn.codedSubsampling = ChromaSubsampling::Yuv420;
			cIn.gridStatus = ChromaGridStatus::Identified;
			cIn.receivedCb = vCb; cIn.receivedCr = vCr;
			cIn.explicitDefocusedCells = &defocus;
			cIn.hasStepPrior = true; cIn.estimatedStepDelta = 9.0;
			cIn.prevPeak.cleanCb = &mCb; cIn.prevPeak.cleanCr = &mCr; cIn.prevPeak.lumaMotion = &stillMv;
			cIn.nextPeak.cleanCb = &mCb; cIn.nextPeak.cleanCr = &mCr; cIn.nextPeak.lumaMotion = &stillMv;

			cArena.BeginFrame();
			const ChromaProcessResult cpuR = TdV2ChromaEngine(cfg).ProcessFrame(cIn);
			ChromaGpuGuard gC(ctx.gpu, cfg);
			const ChromaGpuProcessResult gpuR = gC.eng.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &cArena);
			Assert::IsTrue(gpuR.succeeded &&
			               MaxAbsPlaneDiff(cpuR.outputCb, gpuR.outputCb) <= kParityTol &&
			               MaxAbsPlaneDiff(cpuR.outputCr, gpuR.outputCr) <= kParityTol,
				WMsg("GPU chroma bokeh straddle parity diff=%.6f", MaxAbsPlaneDiff(cpuR.outputCb, gpuR.outputCb)).c_str());
		}

		// (C) Plan 6.6 End-to-End `TdV2GpuEngine -> TdV2ChromaGpuEngine` Luma-Weight Reuse & Role Mapping Parity!
		{
			const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			Config reuseCfg = MakePreset(Preset::Reference, Mode::On);
			reuseCfg.chromaReuseLumaWeights = true;
			reuseCfg.enablePerCornerHuber = true;
			reuseCfg.pixelBoundKappa = 2.0;

			FrameProcessInput lIn{};
			lIn.seq = 1; lIn.receivedY = vY;
			lIn.hasStepPrior = true; lIn.estimatedStepDelta = 9.0;
			// Use prevPeak + prevPeak2 (roles PrevPeak=0 and PrevPeak2=2) to test RefRole enum parity between luma & chroma GPU!
			lIn.prevPeak.cleanPlane = &mY; lIn.prevPeak.motion = &stillMv;
			lIn.prevPeak2.cleanPlane = &mY; lIn.prevPeak2.motion = &stillMv;

			TdV2ReferenceEngine cpuLEng(reuseCfg);
			const FrameProcessResult cpuLRes = cpuLEng.ProcessFrame(lIn);

			LumaGpuGuard gpuLEng(ctx.gpu, reuseCfg);
			lArena.BeginFrame();
			const GpuProcessResult gpuLRes = gpuLEng.eng.ProcessFrame(ctx.gpu, ctx.dispatch, lIn, &lArena);
			Assert::IsTrue(gpuLRes.succeeded && gpuLEng.eng.LastLumaCellWeightsTex() != nullptr);

			// Check that TdV2GpuEngine::LastLumaRefRole matches CPU TdV2ReferenceEngine::lumaRefRoles enum values!
			Assert::AreEqual(static_cast<int>(cpuLRes.lumaRefRoles[0]), gpuLEng.eng.LastLumaRefRole(0),
				L"GPU LastLumaRefRole(0) must match canonical TdV2::RefRole enum");
			Assert::AreEqual(static_cast<int>(cpuLRes.lumaRefRoles[1]), gpuLEng.eng.LastLumaRefRole(1),
				L"GPU LastLumaRefRole(1) must match canonical TdV2::RefRole enum (PrevPeak2 == 2, Carry == 3)");

			// Also verify `ProcessChromaPlanes` forwards lumaCellWeights matching `TdV2ChromaEngine` within 1e-3 code!
			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> packedLw;
			for (int b = 0; b < 3; ++b)
				for (int k = 0; k < kMaxBlendRefs; ++k)
					packedLw[b][k] = cpuLRes.lumaCellWeights[b][k];

			FrameProcessInput lInPrevNext = lIn;
			lInPrevNext.prevPeak2 = ReferenceInput{};
			lInPrevNext.nextPeak.cleanPlane = &mY; lInPrevNext.nextPeak.motion = &stillMv;
			const FrameProcessResult cpuL2 = cpuLEng.ProcessFrame(lInPrevNext);
			for (int b = 0; b < 3; ++b)
				for (int k = 0; k < kMaxBlendRefs; ++k)
					packedLw[b][k] = cpuL2.lumaCellWeights[b][k];

			// Test BOTH (i) blurred vCb/vCr (standalone chroma auto rack-focus parity under reuse_luma!) AND
			// (ii) additive coding-noise chroma (active non-zero weight reuse parity!)
			TdV2Plane noisyCb = mCb, noisyCr = mCr;
			{
				std::mt19937 rCh(771u);
				std::normal_distribution<double> ndCh(0.0, 3.5);
				for (size_t i = 0; i < noisyCb.v.size(); ++i)
				{
					noisyCb.v[i] += ndCh(rCh);
					noisyCr.v[i] += ndCh(rCh);
				}
			}
			for (int subCase = 0; subCase < 2; ++subCase)
			{
				ChromaProcessInput cIn{};
				cIn.seq = static_cast<uint64_t>(subCase + 1);
				cIn.lumaW = w; cIn.lumaH = h;
				cIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
				cIn.codedSubsampling = ChromaSubsampling::Yuv420;
				cIn.gridStatus = ChromaGridStatus::Identified;
				cIn.receivedCb = (subCase == 0) ? noisyCb : vCb;
				cIn.receivedCr = (subCase == 0) ? noisyCr : vCr;
				cIn.hasStepPrior = true; cIn.estimatedStepDelta = 18.0;
				cIn.prevPeak.cleanCb = &mCb; cIn.prevPeak.cleanCr = &mCr; cIn.prevPeak.lumaMotion = &stillMv;
				cIn.nextPeak.cleanCb = &mCb; cIn.nextPeak.cleanCr = &mCr; cIn.nextPeak.lumaMotion = &stillMv;
				cIn.lumaCellWeights = &packedLw;
				cIn.lumaRefRoles = cpuL2.lumaRefRoles;
				cIn.lumaActiveRefCount = cpuL2.lumaActiveRefCount;

				cArena.BeginFrame();
				const ChromaProcessResult cpuCRes = TdV2ChromaEngine(reuseCfg).ProcessFrame(cIn);
				ChromaGpuGuard gpuCEng(ctx.gpu, reuseCfg);
				const ChromaGpuProcessResult gpuCRes = gpuCEng.eng.ProcessChromaPlanes(ctx.gpu, ctx.dispatch, cIn, &cArena);
				Assert::IsTrue(gpuCRes.succeeded);
				if (subCase == 0)
					Assert::IsTrue(MaxAbsPlaneDiff(noisyCb, gpuCRes.outputCb) > 0.25, L"6.6 active reuse_luma case must blend non-zero chroma");
				Assert::IsTrue(MaxAbsPlaneDiff(cpuCRes.outputCb, gpuCRes.outputCb) <= kParityTol &&
				               MaxAbsPlaneDiff(cpuCRes.outputCr, gpuCRes.outputCr) <= kParityTol,
					WMsg("6.6 CPU-vs-GPU ProcessChromaPlanes reuse_luma subCase=%.0f diff Cb=%.6f Cr=%.6f",
						static_cast<double>(subCase), MaxAbsPlaneDiff(cpuCRes.outputCb, gpuCRes.outputCb), MaxAbsPlaneDiff(cpuCRes.outputCr, gpuCRes.outputCr)).c_str());
			}
		}

		lArena.Release();
		cArena.Release();
	}
};

} // namespace Tests
