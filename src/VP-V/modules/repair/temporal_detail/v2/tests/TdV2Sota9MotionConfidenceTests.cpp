// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 -- Unit 9 (TD-SOTA-9) Adversarial Red-Team CPU + WARP GPU Tests:
// P3r Continuous Per-Cell Motion Confidence [0..1] Across Luma and Chroma:
//   1. CpuEvaluateMotionGateContinuousConfidenceAndNanClosure:
//      - Verifies EvaluateCameraRelativeMotionGate scales gateL & gateMH continuously across Still, Camera,
//        Object, and DynamicTexture cells, clamps out-of-range values to [0, 1], preserves empty-vector
//        bit-identity (1.0), and fails NaN closed to 0.0 + NonFiniteClosure.
//   2. GpuAndCpuLumaAndChromaContinuousConfidenceSweepMonotonicWithoutCliff (GpuWarp):
//      - Sweeps confidence c in {1.0, 0.75, 0.50, 0.25, 0.0} across CPU & WARP GPU luma and chroma:
//        proves smooth monotonic proportional feathering without binary cliff popping, plus spatial ramp
//        continuity across adjacent cells vs binary [1, 1, 0, 0] cliff negative control!
//   3. GpuZeroCopyLegacyRg32fBackCompatExplicitRgba32fAndNanFailClosed (GpuWarp):
//      - Proves zero-copy ProcessFrameGpu & ProcessChromaGpu keep legacy 2-channel RG32F motionMetaTex
//        bit-identical to confidence == 1.0, honor explicit 4-channel RGBA32F motionMetaTex.z, and fail
//        NaN closed to 0.0 + StandInKind::NonFiniteClosure.
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
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
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
namespace TdV2Sota9MotionConfidenceNs
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

	struct ChromaGpuGuard
	{
		pl_gpu gpu = nullptr;
		TdV2ChromaGpuEngine engine;
		explicit ChromaGpuGuard(pl_gpu g, const Config& cfg) : gpu(g), engine(cfg) {}
		~ChromaGpuGuard() { engine.Release(gpu); }
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

	static double RmsAbs(const TdV2Plane& p)
	{
		double sumSq = 0.0;
		for (size_t i = 0; i < p.v.size(); ++i)
			sumSq += p.v[i] * p.v[i];
		return !p.v.empty() ? std::sqrt(sumSq / static_cast<double>(p.v.size())) : 0.0;
	}

	static TdV2Plane MakeRichScene(int w, int h, double phase)
	{
		TdV2Plane p(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const double fx = static_cast<double>(x);
				const double fy = static_cast<double>(y);
				p.at(x, y) = 490.0
					+ 32.0 * std::sin(2.0 * kPi * fx / 30.0 + phase) * std::cos(2.0 * kPi * fy / 26.0)
					+ 24.0 * std::sin(2.0 * kPi * (fx / 7.0 + fy / 8.0) + phase)
					+ 16.0 * std::cos(2.0 * kPi * (fx / 3.3 - fy / 3.7) + 0.5);
			}
		}
		return p;
	}

	static TdV2Plane MakeWipedScene(const TdV2Plane& master, uint32_t seed)
	{
		const ThreeBandPlanes b = DecomposeActiveBands(master, ActiveRect::Full(master.w, master.h));
		TdV2Plane deg(master.w, master.h, 0.0);
		std::mt19937 rng(seed);
		std::normal_distribution<double> qn(0.0, 2.0);
		for (size_t i = 0; i < master.v.size(); ++i)
		{
			const double bandedL = std::nearbyint(b.L.v[i] / 12.0) * 12.0;
			deg.v[i] = bandedL + 0.25 * b.M.v[i] + 0.20 * b.H.v[i] + qn(rng);
		}
		return deg;
	}

	static TdV2Plane AddSmallNoise(const TdV2Plane& src, uint32_t seed, double sigma)
	{
		TdV2Plane out = src;
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, sigma);
		for (size_t i = 0; i < out.v.size(); ++i)
			out.v[i] += nd(rng);
		return out;
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

	TEST_CLASS(TdV2Sota9MotionConfidenceTests)
	{
	public:
		TEST_METHOD(CpuEvaluateMotionGateContinuousConfidenceAndNanClosure)
		{
			CellMotionField base = CellMotionField::Uniform(4, 1, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			base.cls[0] = RegionMotionClass::Still;
			base.cls[1] = RegionMotionClass::Camera;
			base.cls[2] = RegionMotionClass::Object;
			base.cls[3] = RegionMotionClass::DynamicTexture;

			const MotionGateResult gEmpty = EvaluateCameraRelativeMotionGate(base, nullptr);
			Assert::AreEqual(1.0, gEmpty.gateL[0], 1e-12);
			Assert::AreEqual(1.0, gEmpty.gateMH[0], 1e-12);
			Assert::AreEqual(1.0, gEmpty.gateL[3], 1e-12);
			Assert::AreEqual(0.0, gEmpty.gateMH[3], 1e-12);

			base.confidence = { 0.5, -0.4, 1.7, 0.35 };
			StandInRegistry st;
			const MotionGateResult gConf = EvaluateCameraRelativeMotionGate(base, &st);
			Assert::AreEqual(size_t(0), gConf.nonFiniteCells);
			Assert::IsFalse(st.IsActive(StandInKind::NonFiniteClosure));
			Assert::AreEqual(0.5, gConf.gateL[0], 1e-12);
			Assert::AreEqual(0.5, gConf.gateMH[0], 1e-12);
			Assert::AreEqual(0.0, gConf.gateL[1], 1e-12);
			Assert::AreEqual(0.0, gConf.gateMH[1], 1e-12);
			Assert::AreEqual(1.0, gConf.gateL[2], 1e-12);
			Assert::AreEqual(1.0, gConf.gateMH[2], 1e-12);
			Assert::AreEqual(0.35, gConf.gateL[3], 1e-12);
			Assert::AreEqual(0.0, gConf.gateMH[3], 1e-12);

			base.confidence[2] = std::numeric_limits<double>::quiet_NaN();
			StandInRegistry stNan;
			const MotionGateResult gNan = EvaluateCameraRelativeMotionGate(base, &stNan);
			Assert::AreEqual(size_t(1), gNan.nonFiniteCells);
			Assert::IsTrue(stNan.IsActive(StandInKind::NonFiniteClosure));
			Assert::AreEqual(0.0, gNan.gateL[2], 1e-12);
			Assert::AreEqual(0.0, gNan.gateMH[2], 1e-12);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuAndCpuLumaAndChromaContinuousConfidenceSweepMonotonicWithoutCliff)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuAndCpuLumaAndChromaContinuousConfidenceSweepMonotonicWithoutCliff)
		{
			WarpContext wc;
			const int w = 80;
			const int h = 64;
			const int cw = w / 16; // 5 columns
			const int ch = h / 16; // 4 rows
			const size_t numCells = static_cast<size_t>(cw * ch);

			VpSkeleton::PlTexBackend backend(wc.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> lumaArena(backend);
			Assert::IsTrue(lumaArena.Reserve(VpSkeleton::PlanArena(
				TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, Preset::Reference), VpSkeleton::Tier::T1)));
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> chromaArena(backend);
			Assert::IsTrue(chromaArena.Reserve(VpSkeleton::PlanArena(
				TdV2ChromaGpuEngine::ArenaRequests(wc.gpu, w, h, ChromaSubsampling::Yuv420, 16), VpSkeleton::Tier::T1)));

			const TdV2Plane masterY = MakeRichScene(w, h, 0.2);
			const TdV2Plane prevY = AddSmallNoise(masterY, 101u, 0.6);
			const TdV2Plane nextY = AddSmallNoise(masterY, 102u, 0.6);
			const TdV2Plane curDegY = MakeWipedScene(masterY, 103u);

			const SitingSpec siting{ -0.5f, 0.0f };
			const ActiveRect full = ActiveRect::Full(w, h);
			const TdV2Plane masterCb = MakeRichScene(w, h, 0.9);
			const TdV2Plane masterCr = MakeRichScene(w, h, 1.7);
			const TdV2Plane codedMasterCb = ReduceToCodedGrid(masterCb, w, h, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, full);
			const TdV2Plane codedMasterCr = ReduceToCodedGrid(masterCr, w, h, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, full);
			const TdV2Plane codedPrevCb = AddSmallNoise(codedMasterCb, 201u, 0.5);
			const TdV2Plane codedPrevCr = AddSmallNoise(codedMasterCr, 202u, 0.5);
			const TdV2Plane codedNextCb = AddSmallNoise(codedMasterCb, 203u, 0.5);
			const TdV2Plane codedNextCr = AddSmallNoise(codedMasterCr, 204u, 0.5);
			const TdV2Plane codedDegCb = MakeWipedScene(codedMasterCb, 205u);
			const TdV2Plane codedDegCr = MakeWipedScene(codedMasterCr, 206u);
			const TdV2Plane delivDegCb = UpsampleDeltaToDelivered(codedDegCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, full);
			const TdV2Plane delivDegCr = UpsampleDeltaToDelivered(codedDegCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, full);

			Config cfg;
			cfg.preset = Preset::Reference;
			cfg.mode = Mode::On;
			cfg.sourceBits = 10;
			cfg.cellPx = 16;

			const double confSteps[5] = { 1.0, 0.75, 0.50, 0.25, 0.0 };
			double lumaRmsCpu[5] = {};
			double lumaRmsGpu[5] = {};
			double chromaRmsCpu[5] = {};
			double chromaRmsGpu[5] = {};

			for (int si = 0; si < 5; ++si)
			{
				CellMotionField mf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
				mf.confidence.assign(numCells, confSteps[si]);

				FrameProcessInput lIn;
				lIn.seq = 0;
				lIn.receivedY = curDegY;
				lIn.cleanY = curDegY;
				lIn.hasCleanSplit = true;
				lIn.hasStepPrior = true;
				lIn.estimatedStepDelta = 28.0;
				lIn.prevPeak.cleanPlane = &prevY;
				lIn.prevPeak.motion = &mf;
				lIn.nextPeak.cleanPlane = &nextY;
				lIn.nextPeak.motion = &mf;

				TdV2ReferenceEngine cpuLuma(cfg);
				LumaGpuGuard gpuLuma(wc.gpu, cfg);
				const FrameProcessResult lc = cpuLuma.ProcessFrame(lIn, nullptr);
				lumaArena.BeginFrame();
				const GpuProcessResult lg = gpuLuma.engine.ProcessFrame(wc.gpu, wc.dispatch, lIn, &lumaArena, nullptr);
				Assert::IsTrue(lg.succeeded);
				Assert::IsTrue(RmsDiff(lc.outputY, lg.outputY) < 1e-3);

				lumaRmsCpu[si] = RmsDiff(lc.outputY, curDegY);
				lumaRmsGpu[si] = RmsAbs(lg.tdDelta);
				if (si == 4)
				{
					// R32F outputY carries 1-ULP FP32 quantization (~500 * 2^-24 ~= 3e-5) relative to FP64 curDegY when tdDelta == 0:
					Assert::AreEqual(0.0, RmsDiff(lg.outputY, curDegY), 5e-5);
				}

				ChromaProcessInput cIn;
				cIn.seq = 0;
				cIn.lumaW = w;
				cIn.lumaH = h;
				cIn.codedSubsampling = ChromaSubsampling::Yuv420;
				cIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
				cIn.siting = siting;
				cIn.receivedCb = delivDegCb;
				cIn.receivedCr = delivDegCr;
				cIn.cleanCb = delivDegCb;
				cIn.cleanCr = delivDegCr;
				cIn.hasCleanSplit = true;
				cIn.hasStepPrior = true;
				cIn.estimatedStepDelta = 28.0;
				cIn.prevPeak.cleanCb = &codedPrevCb;
				cIn.prevPeak.cleanCr = &codedPrevCr;
				cIn.prevPeak.lumaMotion = &mf;
				cIn.nextPeak.cleanCb = &codedNextCb;
				cIn.nextPeak.cleanCr = &codedNextCr;
				cIn.nextPeak.lumaMotion = &mf;

				TdV2ChromaEngine cpuCh(cfg);
				ChromaGpuGuard gpuCh(wc.gpu, cfg);
				const ChromaProcessResult cc = cpuCh.ProcessFrame(cIn, nullptr);
				chromaArena.BeginFrame();
				const ChromaGpuProcessResult cg = gpuCh.engine.ProcessChromaPlanes(wc.gpu, wc.dispatch, cIn, &chromaArena, nullptr);
				Assert::IsTrue(cg.succeeded);
				Assert::IsTrue(RmsDiff(cc.outputCb, cg.outputCb) < 1e-2);

				chromaRmsCpu[si] = RmsAbs(cc.deliveredDeltaCb);
				chromaRmsGpu[si] = RmsAbs(cg.deliveredDeltaCb);
			}

			// Verify strict smooth monotonic scaling (zero binary cliff at 0.5!).
			Assert::AreEqual(0.0, lumaRmsCpu[4], 1e-9);
			Assert::AreEqual(0.0, lumaRmsGpu[4], 1e-6);
			Assert::AreEqual(0.0, chromaRmsCpu[4], 1e-9);
			Assert::AreEqual(0.0, chromaRmsGpu[4], 1e-6);

			for (int si = 0; si < 4; ++si)
			{
				Assert::IsTrue(lumaRmsGpu[si] > lumaRmsGpu[si + 1] + 0.15 * lumaRmsGpu[0],
					FmtMsg("Luma confidence sweep non-monotonic at step %.0f: %.4f vs %.4f", static_cast<double>(si), lumaRmsGpu[si], lumaRmsGpu[si + 1]).c_str());
				Assert::IsTrue(chromaRmsGpu[si] > chromaRmsGpu[si + 1] + 0.15 * chromaRmsGpu[0],
					FmtMsg("Chroma confidence sweep non-monotonic at step %.0f: %.4f vs %.4f", static_cast<double>(si), chromaRmsGpu[si], chromaRmsGpu[si + 1]).c_str());
			}
			Assert::AreEqual(0.50 * lumaRmsGpu[0], lumaRmsGpu[2], 0.04 * lumaRmsGpu[0]);
			Assert::AreEqual(0.50 * chromaRmsGpu[0], chromaRmsGpu[2], 0.04 * chromaRmsGpu[0]);

			// Spatial continuity proof vs binary cliff negative control across 5 horizontal cells [1.0, 0.75, 0.50, 0.25, 0.0] vs [1, 1, 0, 0, 0]:
			CellMotionField mfSmooth = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
			CellMotionField mfBinary = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
			mfSmooth.confidence.resize(numCells, 1.0);
			mfBinary.confidence.resize(numCells, 1.0);
			for (int cy = 0; cy < ch; ++cy)
			{
				for (int cx = 0; cx < cw; ++cx)
				{
					const size_t idx = static_cast<size_t>(cy * cw + cx);
					mfSmooth.confidence[idx] = confSteps[cx];
					mfBinary.confidence[idx] = (cx < 2) ? 1.0 : 0.0;
				}
			}

			FrameProcessInput inSmooth, inBinary;
			inSmooth.seq = 0;
			inSmooth.receivedY = curDegY;
			inSmooth.cleanY = curDegY;
			inSmooth.hasCleanSplit = true;
			inSmooth.hasStepPrior = true;
			inSmooth.estimatedStepDelta = 28.0;
			inSmooth.prevPeak.cleanPlane = &prevY;
			inSmooth.prevPeak.motion = &mfSmooth;
			inSmooth.nextPeak.cleanPlane = &nextY;
			inSmooth.nextPeak.motion = &mfSmooth;

			inBinary = inSmooth;
			inBinary.prevPeak.motion = &mfBinary;
			inBinary.nextPeak.motion = &mfBinary;

			LumaGpuGuard gSmooth(wc.gpu, cfg), gBinary(wc.gpu, cfg);
			lumaArena.BeginFrame();
			const GpuProcessResult resSmooth = gSmooth.engine.ProcessFrame(wc.gpu, wc.dispatch, inSmooth, &lumaArena, nullptr);
			lumaArena.BeginFrame();
			const GpuProcessResult resBinary = gBinary.engine.ProcessFrame(wc.gpu, wc.dispatch, inBinary, &lumaArena, nullptr);
			Assert::IsTrue(resSmooth.succeeded && resBinary.succeeded);

			// Measure inter-column delta jump across column boundary x = 31 -> 32 (between cell 1 and cell 2):
			auto colDeltaRms = [&](const TdV2Plane& outP, int cxCol) -> double
			{
				double s = 0.0;
				size_t n = 0;
				for (int y = 0; y < h; ++y)
				{
					for (int x = cxCol * 16; x < (cxCol + 1) * 16; ++x)
					{
						const double d = outP.at(x, y) - curDegY.at(x, y);
						s += d * d;
						++n;
					}
				}
				return std::sqrt(s / static_cast<double>(n));
			};
			const double jumpSmooth = std::fabs(colDeltaRms(resSmooth.outputY, 1) - colDeltaRms(resSmooth.outputY, 2));
			const double jumpBinary = std::fabs(colDeltaRms(resBinary.outputY, 1) - colDeltaRms(resBinary.outputY, 2));
			Assert::IsTrue(jumpSmooth < 0.45 * jumpBinary,
				FmtMsg("Continuous confidence failed to reduce inter-cell cliff jump: smooth=%.4f binary=%.4f", jumpSmooth, jumpBinary).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuZeroCopyLegacyRg32fBackCompatExplicitRgba32fAndNanFailClosed)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuZeroCopyLegacyRg32fBackCompatExplicitRgba32fAndNanFailClosed)
		{
			WarpContext wc;
			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const size_t numCells = static_cast<size_t>(cw * ch);

			VpSkeleton::PlTexBackend backend(wc.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> lumaArena(backend);
			Assert::IsTrue(lumaArena.Reserve(VpSkeleton::PlanArena(
				TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, Preset::Reference), VpSkeleton::Tier::T1)));
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> chromaArena(backend);
			Assert::IsTrue(chromaArena.Reserve(VpSkeleton::PlanArena(
				TdV2ChromaGpuEngine::ArenaRequests(wc.gpu, w, h, ChromaSubsampling::Yuv444, 16), VpSkeleton::Tier::T1)));

			const TdV2Plane masterY = MakeRichScene(w, h, 0.4);
			const TdV2Plane prevY = AddSmallNoise(masterY, 301u, 0.6);
			const TdV2Plane nextY = AddSmallNoise(masterY, 302u, 0.6);
			const TdV2Plane curDegY = MakeWipedScene(masterY, 303u);

			Config cfg;
			cfg.preset = Preset::Reference;
			cfg.mode = Mode::On;
			cfg.sourceBits = 10;
			cfg.cellPx = 16;

			// Upload 32-bit float luma textures for zero-copy ProcessFrameGpu.
			auto toFloatVec = [](const TdV2Plane& p) -> std::vector<float>
			{
				std::vector<float> v(p.v.size(), 0.0f);
				for (size_t i = 0; i < p.v.size(); ++i)
					v[i] = static_cast<float>(p.v[i]);
				return v;
			};
			const std::vector<float> recF = toFloatVec(curDegY);
			const std::vector<float> prevF = toFloatVec(prevY);
			const std::vector<float> nextF = toFloatVec(nextY);

			OwnedTex tRec(wc.gpu), tClean(wc.gpu), tPrev(wc.gpu), tNext(wc.gpu);
			OwnedTex tOutLegacy(wc.gpu), tOutRgba1(wc.gpu), tOutRgba04(wc.gpu), tOutNan(wc.gpu);
			tRec.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, recF.data());
			tClean.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, recF.data());
			tPrev.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, prevF.data());
			tNext.tex = CreateFloatTex(wc.gpu, w, h, 1, true, false, nextF.data());
			tOutLegacy.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);
			tOutRgba1.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);
			tOutRgba04.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);
			tOutNan.tex = CreateFloatTex(wc.gpu, w, h, 1, true, true, nullptr);

			const std::vector<float> mvZero(static_cast<size_t>(3 * cw * ch) * 4u, 0.0f);
			std::vector<float> mmLegacyRg(static_cast<size_t>(3 * cw * ch) * 2u, 0.0f);
			std::vector<float> mmRgba1(static_cast<size_t>(3 * cw * ch) * 4u, 0.0f);
			std::vector<float> mmRgba04(static_cast<size_t>(3 * cw * ch) * 4u, 0.0f);
			std::vector<float> mmRgbaNan(static_cast<size_t>(3 * cw * ch) * 4u, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(3 * cw * ch); ++i)
			{
				const float clsCam = static_cast<float>(static_cast<int>(RegionMotionClass::Camera));
				mmLegacyRg[i * 2u + 0u] = clsCam;
				mmLegacyRg[i * 2u + 1u] = 1.0f;

				mmRgba1[i * 4u + 0u] = clsCam;
				mmRgba1[i * 4u + 1u] = 1.0f;
				mmRgba1[i * 4u + 2u] = 1.0f;

				mmRgba04[i * 4u + 0u] = clsCam;
				mmRgba04[i * 4u + 1u] = 1.0f;
				mmRgba04[i * 4u + 2u] = 0.4f;

				mmRgbaNan[i * 4u + 0u] = clsCam;
				mmRgbaNan[i * 4u + 1u] = 1.0f;
				mmRgbaNan[i * 4u + 2u] = std::numeric_limits<float>::quiet_NaN();
			}

			OwnedTex tMv(wc.gpu), tMmLegacy(wc.gpu), tMmRgba1(wc.gpu), tMmRgba04(wc.gpu), tMmNan(wc.gpu);
			tMv.tex = CreateFloatTex(wc.gpu, 3 * cw, ch, 4, true, false, mvZero.data());
			tMmLegacy.tex = CreateFloatTex(wc.gpu, 3 * cw, ch, 2, true, false, mmLegacyRg.data());
			tMmRgba1.tex = CreateFloatTex(wc.gpu, 3 * cw, ch, 4, true, false, mmRgba1.data());
			tMmRgba04.tex = CreateFloatTex(wc.gpu, 3 * cw, ch, 4, true, false, mmRgba04.data());
			tMmNan.tex = CreateFloatTex(wc.gpu, 3 * cw, ch, 4, true, false, mmRgbaNan.data());

			auto runZeroCopyLuma = [&](pl_tex metaTex, pl_tex outTex, GpuFrameOutput& gOut, StandInRegistry& st) -> TdV2Plane
			{
				LumaGpuGuard eng(wc.gpu, cfg);
				GpuFrameInput gIn;
				gIn.seq = 0;
				gIn.w = w;
				gIn.h = h;
				gIn.receivedYTex = tRec.tex;
				gIn.cleanYTex = tClean.tex;
				gIn.hasCleanSplit = true;
				gIn.hasStepPrior = true;
				gIn.estimatedStepDelta = 28.0;
				gIn.prevPeak.cleanTex = tPrev.tex;
				gIn.nextPeak.cleanTex = tNext.tex;
				gIn.motionVecTex = tMv.tex;
				gIn.motionMetaTex = metaTex;

				gOut = GpuFrameOutput{};
				gOut.outputYTex = outTex;
				gOut.readbackCellDiagnostics = true;
				lumaArena.BeginFrame();
				const bool ok = eng.engine.ProcessFrameGpu(wc.gpu, wc.dispatch, gIn, gOut, &lumaArena, &st);
				Assert::IsTrue(ok && gOut.succeeded);

				TdV2Plane pOut;
				Assert::IsTrue(Detail::DownloadPlaneR32F(wc.gpu, outTex, w, h, pOut));
				return pOut;
			};

			GpuFrameOutput oLegacy, oRgba1, oRgba04, oNan;
			StandInRegistry sLegacy, sRgba1, sRgba04, sNan;
			const TdV2Plane pLegacy = runZeroCopyLuma(tMmLegacy.tex, tOutLegacy.tex, oLegacy, sLegacy);
			const TdV2Plane pRgba1 = runZeroCopyLuma(tMmRgba1.tex, tOutRgba1.tex, oRgba1, sRgba1);
			const TdV2Plane pRgba04 = runZeroCopyLuma(tMmRgba04.tex, tOutRgba04.tex, oRgba04, sRgba04);
			const TdV2Plane pNan = runZeroCopyLuma(tMmNan.tex, tOutNan.tex, oNan, sNan);

			// 1. Legacy RG32F motionMetaTex is bit-identical to RGBA32F with confidence == 1.0!
			Assert::AreEqual(0.0, MaxAbsDiff(pLegacy, pRgba1), 1e-6);

			// 2. Explicit RGBA32F confidence == 0.4 scales delta smoothly to ~0.4x of legacy RG32F!
			const double rms1 = RmsDiff(pLegacy, curDegY);
			const double rms04 = RmsDiff(pRgba04, curDegY);
			Assert::IsTrue(rms1 > 1.5);
			Assert::AreEqual(0.4 * rms1, rms04, 0.03 * rms1);

			// 3. NaN confidence in RGBA32F motionMetaTex.z fails closed to passthrough + NonFiniteClosure!
			Assert::AreEqual(numCells, oNan.nonFiniteCells);
			Assert::IsTrue(sNan.IsActive(StandInKind::NonFiniteClosure));
			Assert::AreEqual(0.0, RmsDiff(pNan, curDegY), 1e-5);

			// 4. Also verify zero-copy Chroma ProcessChromaGpu legacy RG32F == RGBA32F(1.0) and NaN fail-closed!
			std::vector<float> uvDeg(static_cast<size_t>(w * h) * 2u, 0.0f);
			std::vector<float> uvPrev(static_cast<size_t>(w * h) * 2u, 0.0f);
			std::vector<float> uvNext(static_cast<size_t>(w * h) * 2u, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(w * h); ++i)
			{
				uvDeg[i * 2u + 0u] = recF[i];  uvDeg[i * 2u + 1u] = recF[i];
				uvPrev[i * 2u + 0u] = prevF[i]; uvPrev[i * 2u + 1u] = prevF[i];
				uvNext[i * 2u + 0u] = nextF[i]; uvNext[i * 2u + 1u] = nextF[i];
			}
			OwnedTex tUvRec(wc.gpu), tUvPrev(wc.gpu), tUvNext(wc.gpu);
			OwnedTex tCdLeg(wc.gpu), tCd1(wc.gpu), tCd04(wc.gpu), tCdNan(wc.gpu);
			tUvRec.tex = CreateFloatTex(wc.gpu, w, h, 2, true, false, uvDeg.data());
			tUvPrev.tex = CreateFloatTex(wc.gpu, w, h, 2, true, false, uvPrev.data());
			tUvNext.tex = CreateFloatTex(wc.gpu, w, h, 2, true, false, uvNext.data());
			tCdLeg.tex = CreateFloatTex(wc.gpu, w, h, 2, true, true, nullptr);
			tCd1.tex = CreateFloatTex(wc.gpu, w, h, 2, true, true, nullptr);
			tCd04.tex = CreateFloatTex(wc.gpu, w, h, 2, true, true, nullptr);
			tCdNan.tex = CreateFloatTex(wc.gpu, w, h, 2, true, true, nullptr);

			auto runZeroCopyChroma = [&](pl_tex metaTex, pl_tex outDeltaTex, ChromaGpuOutput& cOut, StandInRegistry& st) -> std::vector<float>
			{
				ChromaGpuGuard cEng(wc.gpu, cfg);
				ChromaGpuInput cIn;
				cIn.seq = 0;
				cIn.lumaW = w;
				cIn.lumaH = h;
				cIn.codedSubsampling = ChromaSubsampling::Yuv444;
				cIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
				cIn.receivedTex = tUvRec.tex;
				cIn.cleanUvTex = tUvRec.tex;
				cIn.hasCleanSplit = true;
				cIn.hasStepPrior = true;
				cIn.estimatedStepDelta = 28.0;
				cIn.prevPeak.cleanUvTex = tUvPrev.tex;
				cIn.nextPeak.cleanUvTex = tUvNext.tex;
				cIn.motionVecTex = tMv.tex;
				cIn.motionMetaTex = metaTex;

				cOut = ChromaGpuOutput{};
				cOut.outCodedDeltaTex = outDeltaTex;
				cOut.readbackCellDiagnostics = true;
				chromaArena.BeginFrame();
				const bool ok = cEng.engine.ProcessChromaGpu(wc.gpu, wc.dispatch, cIn, cOut, &chromaArena, &st);
				Assert::IsTrue(ok && cOut.succeeded);

				std::vector<float> buf(static_cast<size_t>(w * h) * 2u, 0.0f);
				Assert::IsTrue(Detail::DownloadFloats(wc.gpu, outDeltaTex, buf.data()));
				return buf;
			};

			ChromaGpuOutput coLeg, co1, co04, coNan;
			StandInRegistry csLeg, cs1, cs04, csNan;
			const std::vector<float> dLeg = runZeroCopyChroma(tMmLegacy.tex, tCdLeg.tex, coLeg, csLeg);
			const std::vector<float> d1 = runZeroCopyChroma(tMmRgba1.tex, tCd1.tex, co1, cs1);
			const std::vector<float> d04 = runZeroCopyChroma(tMmRgba04.tex, tCd04.tex, co04, cs04);
			const std::vector<float> dNan = runZeroCopyChroma(tMmNan.tex, tCdNan.tex, coNan, csNan);

			double maxChDiff = 0.0, sq1 = 0.0, sq04 = 0.0, sqNan = 0.0;
			for (size_t i = 0; i < dLeg.size(); ++i)
			{
				maxChDiff = (std::max)(maxChDiff, static_cast<double>(std::fabs(dLeg[i] - d1[i])));
				sq1 += static_cast<double>(dLeg[i]) * static_cast<double>(dLeg[i]);
				sq04 += static_cast<double>(d04[i]) * static_cast<double>(d04[i]);
				sqNan += static_cast<double>(dNan[i]) * static_cast<double>(dNan[i]);
			}
			Assert::AreEqual(0.0, maxChDiff, 1e-6);
			const double cRms1 = std::sqrt(sq1 / static_cast<double>(dLeg.size()));
			const double cRms04 = std::sqrt(sq04 / static_cast<double>(dLeg.size()));
			Assert::IsTrue(cRms1 > 1.5);
			Assert::AreEqual(0.4 * cRms1, cRms04, 0.03 * cRms1);
			Assert::AreEqual(numCells, coNan.nonFiniteCells);
			Assert::IsTrue(csNan.IsActive(StandInKind::NonFiniteClosure));
			Assert::AreEqual(0.0, std::sqrt(sqNan / static_cast<double>(dNan.size())), 1e-6);
		}
	};
}
}
