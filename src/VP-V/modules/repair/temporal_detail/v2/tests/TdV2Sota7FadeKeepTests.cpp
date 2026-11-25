// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 -- Unit 7 (TD-SOTA-7) Adversarial Red-Team CPU + WARP GPU Tests:
// Upstream U7 Gradual fadeKeep ([0, 1]) Partial-Reset Across Luma and Chroma:
//   1. CpuResolveFadeKeepUnitAndClampInvariants:
//      - Verifies ResolveFadeKeep identity at 1.0, linear pass-through in (0, 1), clamping on (<0, >1),
//        and fail-closed 0.0 + outNonFinite=true on NaN/+Inf/-Inf.
//   2. GpuAndCpuLumaGradualFadeKeepTapersCarryWithoutGrainNormalizerCancellation (GpuWarp):
//      - Seeds recurrent carry on Frame 0, then tests Frame 1 with Carry active under unsplit/leaked own grain
//        so BOTH Layer-2/3 grain-budget normalization and Layer-4 TdGuardPort are active!
//      - Proves fadeKeep in {1.0, 0.75, 0.40, 0.0} smoothly and monotonically tapers Carry recovery
//        (proving neither Layer-2/3 grain-budget scaling nor Layer-4 TdGuardPort cancels fadeKeep!)
//        and blends carryClean_ = fadeKeep * newClean + (1 - fadeKeep) * curClean for Frame 2,
//        while fadeKeep = NaN fails closed to 0.0 (carryReset=true + NonFiniteClosure) with CPU/GPU parity.
//   3. GpuAndCpuChromaGradualFadeKeepTapersAnchorAndDeltaWithoutGuardCancellation (GpuWarp):
//      - CPU TdV2ChromaEngine + WARP GPU TdV2ChromaGpuEngine (coded 4:2:0 -> delivered 4:4:4):
//      - Proves fadeKeep = 0.4 scales accumulated 4:4:4 AnchorCanvas weights by 0.4 without resetting and
//        scales output chroma delta by 0.4 strictly AFTER Layer-4 TdGuardPort (vs 1.0 full-keep and 0.0 hard-cut
//        same-input negative controls), and fadeKeep = NaN fails closed to 0.0 + NonFiniteClosure.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
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
namespace TdV2Sota7FadeKeepNs
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

	static double RmsAbs(const TdV2Plane& p)
	{
		double sumSq = 0.0;
		for (size_t i = 0; i < p.v.size(); ++i)
			sumSq += p.v[i] * p.v[i];
		return !p.v.empty() ? std::sqrt(sumSq / static_cast<double>(p.v.size())) : 0.0;
	}

	static TdV2Plane MakeTexturedMaster(int w, int h, double phaseShift)
	{
		TdV2Plane p(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const double fx = static_cast<double>(x);
				const double fy = static_cast<double>(y);
				const double low = 480.0 + 36.0 * std::sin(2.0 * kPi * fx / 32.0 + phaseShift)
				                         * std::cos(2.0 * kPi * fy / 28.0 - 0.2);
				const double mid = 22.0 * std::sin(2.0 * kPi * (fx / 7.5 + fy / 8.5) + phaseShift)
				                 + 16.0 * std::cos(2.0 * kPi * (fx / 9.0 - fy / 7.0));
				const double fine = 13.0 * std::sin(2.0 * kPi * (fx / 3.3 + fy / 3.9) + phaseShift);
				p.at(x, y) = low + mid + fine;
			}
		}
		return p;
	}

	static TdV2Plane MakeDegradedFromMaster(const TdV2Plane& master, uint32_t seed, double noiseStd, double grainStd, TdV2Plane& outOwnGrain)
	{
		const ActiveRect full = ActiveRect::Full(master.w, master.h);
		const ThreeBandPlanes mb = DecomposeActiveBands(master, full);
		TdV2Plane cleanDeg(master.w, master.h, 0.0);
		outOwnGrain = TdV2Plane(master.w, master.h, 0.0);
		std::mt19937 rng(seed);
		std::normal_distribution<double> qn(0.0, noiseStd);
		std::normal_distribution<double> gn(0.0, grainStd);
		for (size_t i = 0; i < master.v.size(); ++i)
		{
			const double bandedL = std::nearbyint(mb.L.v[i] / 14.0) * 14.0;
			cleanDeg.v[i] = bandedL + 0.30 * mb.M.v[i] + 0.25 * mb.H.v[i] + qn(rng);
			outOwnGrain.v[i] = gn(rng);
		}
		return cleanDeg;
	}

	static TdV2Plane AddPlane(const TdV2Plane& a, const TdV2Plane& b)
	{
		TdV2Plane r(a.w, a.h, 0.0);
		for (size_t i = 0; i < r.v.size(); ++i)
			r.v[i] = a.v[i] + b.v[i];
		return r;
	}

	static TdV2Plane AddGaussianNoise(const TdV2Plane& src, uint32_t seed, double sigma)
	{
		TdV2Plane out = src;
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, sigma);
		for (size_t i = 0; i < out.v.size(); ++i)
			out.v[i] += nd(rng);
		return out;
	}

	TEST_CLASS(TdV2Sota7FadeKeepTests)
	{
	public:
		TEST_METHOD(CpuResolveFadeKeepUnitAndClampInvariants)
		{
			bool nf = true;
			Assert::AreEqual(1.0, ResolveFadeKeep(1.0, nf), 1e-15);
			Assert::IsFalse(nf);

			Assert::AreEqual(0.4, ResolveFadeKeep(0.4, nf), 1e-15);
			Assert::IsFalse(nf);

			Assert::AreEqual(0.0, ResolveFadeKeep(0.0, nf), 1e-15);
			Assert::IsFalse(nf);

			Assert::AreEqual(0.0, ResolveFadeKeep(-0.35, nf), 1e-15);
			Assert::IsFalse(nf);

			Assert::AreEqual(1.0, ResolveFadeKeep(1.85, nf), 1e-15);
			Assert::IsFalse(nf);

			Assert::AreEqual(0.0, ResolveFadeKeep(std::numeric_limits<double>::quiet_NaN(), nf), 1e-15);
			Assert::IsTrue(nf);

			Assert::AreEqual(0.0, ResolveFadeKeep(std::numeric_limits<double>::infinity(), nf), 1e-15);
			Assert::IsTrue(nf);

			Assert::AreEqual(0.0, ResolveFadeKeep(-std::numeric_limits<double>::infinity(), nf), 1e-15);
			Assert::IsTrue(nf);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuAndCpuLumaGradualFadeKeepTapersCarryWithoutGrainNormalizerCancellation)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuAndCpuLumaGradualFadeKeepTapersCarryWithoutGrainNormalizerCancellation)
		{
			WarpContext wc;
			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const size_t numCells = static_cast<size_t>(cw * ch);

			VpSkeleton::PlTexBackend backend(wc.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
				TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, Preset::Reference), VpSkeleton::Tier::T1)));

			Config cfg;
			cfg.preset = Preset::Reference;
			cfg.mode = Mode::On;
			cfg.sourceBits = 10;
			cfg.cellPx = 16;
			cfg.grainCellPx = 16;
			cfg.enableCarryForward = true;

			const TdV2Plane master = MakeTexturedMaster(w, h, 0.15);
			const TdV2Plane refPrev0 = AddGaussianNoise(master, 101u, 0.8);
			const TdV2Plane refNext0 = AddGaussianNoise(master, 102u, 0.8);
			TdV2Plane grain0, grain1, grain2;
			const TdV2Plane clean0 = MakeDegradedFromMaster(master, 201u, 2.0, 3.2, grain0);
			const TdV2Plane clean1 = MakeDegradedFromMaster(master, 202u, 2.4, 3.2, grain1);
			const TdV2Plane clean2 = MakeDegradedFromMaster(master, 203u, 2.2, 3.2, grain2);
			const TdV2Plane rec0 = AddPlane(clean0, grain0);
			// Unsplit grainy input on Frame 1 (hasCleanSplit = false, retainedGrainVar > 0) forces BOTH
			// Layer-2/3 grain-budget normalization (remGrainBudget clamping solved weights!) AND Layer-4 TdGuardPort
			// to engage simultaneously -- proving neither cancels gradual fadeKeep!
			const TdV2Plane rec1Unsplit = AddPlane(clean1, grain1);
			const TdV2Plane rec2Split = AddPlane(clean2, grain2);

			const CellMotionField zeroMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			const std::vector<double> grainPrior(numCells, 3.2 * 3.2);

			struct FadeRunOut
			{
				FrameProcessResult cpu1;
				GpuProcessResult gpu1;
				FrameProcessResult cpu2;
				GpuProcessResult gpu2;
				StandInRegistry cpuStand1;
				StandInRegistry gpuStand1;
			};

			auto runSequence = [&](double fadeKeepFrame1) -> FadeRunOut
			{
				FadeRunOut r;
				TdV2ReferenceEngine cpuEng(cfg);
				LumaGpuGuard gpuEng(wc.gpu, cfg);

				// Frame 0: seed sharp recurrent carryClean_ from prevPeak + nextPeak.
				FrameProcessInput f0;
				f0.seq = 0;
				f0.receivedY = rec0;
				f0.cleanY = clean0;
				f0.hasCleanSplit = true;
				f0.hasStepPrior = true;
				f0.estimatedStepDelta = 48.0;
				f0.prevPeak.cleanPlane = &refPrev0;
				f0.prevPeak.motion = &zeroMf;
				f0.nextPeak.cleanPlane = &refNext0;
				f0.nextPeak.motion = &zeroMf;
				const FrameProcessResult c0 = cpuEng.ProcessFrame(f0, nullptr);
				arena.BeginFrame();
				const GpuProcessResult g0 = gpuEng.engine.ProcessFrame(wc.gpu, wc.dispatch, f0, &arena, nullptr);
				Assert::IsTrue(g0.succeeded);
				Assert::IsTrue(RmsDiff(c0.outputClean, master) < RmsDiff(clean0, master) * 0.65);

				// Frame 1: Carry-ONLY under unsplit grain + retainedGrainVar (saturates Layer-2/3 maxBandW!).
				FrameProcessInput f1;
				f1.seq = 1;
				f1.receivedY = rec1Unsplit;
				f1.hasCleanSplit = false;
				f1.retainedGrainVar = grainPrior;
				f1.hasStepPrior = true;
				f1.estimatedStepDelta = 48.0;
				f1.hasCarryMotion = true;
				f1.carryMotion = zeroMf;
				f1.fadeKeep = fadeKeepFrame1;
				r.cpu1 = cpuEng.ProcessFrame(f1, &r.cpuStand1);
				arena.BeginFrame();
				r.gpu1 = gpuEng.engine.ProcessFrame(wc.gpu, wc.dispatch, f1, &arena, &r.gpuStand1);
				Assert::IsTrue(r.gpu1.succeeded);

				// Frame 2: Carry-ONLY clean split with fadeKeep = 1.0 to probe decayed carryClean_ stored by Frame 1!
				FrameProcessInput f2;
				f2.seq = 2;
				f2.receivedY = rec2Split;
				f2.cleanY = clean2;
				f2.hasCleanSplit = true;
				f2.hasStepPrior = true;
				f2.estimatedStepDelta = 48.0;
				f2.hasCarryMotion = true;
				f2.carryMotion = zeroMf;
				f2.fadeKeep = 1.0;
				r.cpu2 = cpuEng.ProcessFrame(f2, nullptr);
				arena.BeginFrame();
				r.gpu2 = gpuEng.engine.ProcessFrame(wc.gpu, wc.dispatch, f2, &arena, nullptr);
				Assert::IsTrue(r.gpu2.succeeded);
				return r;
			};

			const FadeRunOut r100 = runSequence(1.0);
			const FadeRunOut r075 = runSequence(0.75);
			const FadeRunOut r040 = runSequence(0.40);
			const FadeRunOut r000 = runSequence(0.0);
			const FadeRunOut rNan = runSequence(std::numeric_limits<double>::quiet_NaN());

			// 1. CPU vs GPU parity across all fadeKeep regimes.
			Assert::IsTrue(RmsDiff(r100.cpu1.outputY, r100.gpu1.outputY) < 1e-3);
			Assert::IsTrue(RmsDiff(r075.cpu1.outputY, r075.gpu1.outputY) < 1e-3);
			Assert::IsTrue(RmsDiff(r040.cpu1.outputY, r040.gpu1.outputY) < 1e-3);
			Assert::IsTrue(RmsDiff(r000.cpu1.outputY, r000.gpu1.outputY) < 1e-3);
			Assert::IsTrue(RmsDiff(rNan.cpu1.outputY, rNan.gpu1.outputY) < 1e-3);
			Assert::IsTrue(RmsDiff(r040.cpu2.outputClean, r040.gpu2.outputClean) < 1e-3);

			// 2. Carry state flags: (0, 1] keeps carry alive; 0.0 and NaN reset carry cleanly.
			Assert::IsTrue(r100.cpu1.carryUsed && !r100.cpu1.carryReset);
			Assert::IsTrue(r100.gpu1.carryUsed && !r100.gpu1.carryReset);
			Assert::IsTrue(r040.cpu1.carryUsed && !r040.cpu1.carryReset);
			Assert::IsTrue(r040.gpu1.carryUsed && !r040.gpu1.carryReset);
			Assert::IsTrue(!r000.cpu1.carryUsed && r000.cpu1.carryReset);
			Assert::IsTrue(!r000.gpu1.carryUsed && r000.gpu1.carryReset);

			// 3. Measure clean delta applied on Frame 1 (outputClean - rec1Unsplit):
			//    Even with Layer-2/3 grain-budget clamping saturated AND Layer-4 TdGuardPort active,
			//    0.75 and 0.40 strictly and proportionally taper Carry delta without normalizer cancellation!
			const double d100Cpu = RmsDiff(r100.cpu1.outputClean, rec1Unsplit);
			const double d075Cpu = RmsDiff(r075.cpu1.outputClean, rec1Unsplit);
			const double d040Cpu = RmsDiff(r040.cpu1.outputClean, rec1Unsplit);
			const double d000Cpu = RmsDiff(r000.cpu1.outputClean, rec1Unsplit);

			const double d100Gpu = RmsDiff(r100.gpu1.outputClean, rec1Unsplit);
			const double d075Gpu = RmsDiff(r075.gpu1.outputClean, rec1Unsplit);
			const double d040Gpu = RmsDiff(r040.gpu1.outputClean, rec1Unsplit);
			const double d000Gpu = RmsDiff(r000.gpu1.outputClean, rec1Unsplit);

			Assert::AreEqual(0.0, d000Cpu, 1e-9);
			Assert::AreEqual(0.0, d000Gpu, 1e-6);
			Assert::IsTrue(d100Cpu > 1.2, FmtMsg("Expected strong Frame-1 carry delta at fadeKeep=1.0, got %.4f", d100Cpu).c_str());
			Assert::IsTrue(d075Cpu < 0.82 * d100Cpu && d075Cpu > 0.68 * d100Cpu,
				FmtMsg("CPU fadeKeep=0.75 cancelled by grain normalizer: d075=%.4f d100=%.4f", d075Cpu, d100Cpu).c_str());
			Assert::IsTrue(d040Cpu < 0.46 * d100Cpu && d040Cpu > 0.34 * d100Cpu,
				FmtMsg("CPU fadeKeep=0.40 out of linear taper band: d040=%.4f d100=%.4f", d040Cpu, d100Cpu).c_str());
			Assert::IsTrue(d075Gpu < 0.82 * d100Gpu && d075Gpu > 0.68 * d100Gpu,
				FmtMsg("GPU fadeKeep=0.75 cancelled by grain normalizer: d075=%.4f d100=%.4f", d075Gpu, d100Gpu).c_str());
			Assert::IsTrue(d040Gpu < 0.46 * d100Gpu && d040Gpu > 0.34 * d100Gpu,
				FmtMsg("GPU fadeKeep=0.40 out of linear taper band: d040=%.4f d100=%.4f", d040Gpu, d100Gpu).c_str());

			// Also verify outputY (post-Layer-4 TdGuardPort) strictly tapers monotonically: 0 < dOut(0.4) < dOut(0.75) < dOut(1.0).
			const double y100Gpu = RmsDiff(r100.gpu1.outputY, rec1Unsplit);
			const double y075Gpu = RmsDiff(r075.gpu1.outputY, rec1Unsplit);
			const double y040Gpu = RmsDiff(r040.gpu1.outputY, rec1Unsplit);
			Assert::IsTrue(y040Gpu > 0.25 * y100Gpu && y040Gpu < y075Gpu && y075Gpu < y100Gpu,
				FmtMsg("Post-guard outputY failed monotonic taper: y040=%.4f y075=%.4f y100=%.4f", y040Gpu, y075Gpu, y100Gpu).c_str());

			// 4. Frame-2 carry persistence check: Frame 1 with fadeKeep=0.4 decayed carryClean_ toward clean1,
			//    so Frame 2 clean error sits strictly between fadeKeep=1.0 (sharpest carry) and fadeKeep=0.0 (carry reset on F1).
			const double errF2_100 = RmsDiff(r100.gpu2.outputClean, master);
			const double errF2_040 = RmsDiff(r040.gpu2.outputClean, master);
			const double errF2_000 = RmsDiff(r000.gpu2.outputClean, master);
			Assert::IsTrue(errF2_100 < errF2_040 && errF2_040 < errF2_000,
				FmtMsg("Frame-2 decayed carry ordering failed: e100=%.4f e040=%.4f e000=%.4f", errF2_100, errF2_040, errF2_000).c_str());

			// 5. NaN fadeKeep fails closed to 0.0 and engages NonFiniteClosure visibly on BOTH CPU and GPU.
			Assert::IsTrue(rNan.cpu1.carryReset && !rNan.cpu1.carryUsed);
			Assert::IsTrue(rNan.gpu1.carryReset && !rNan.gpu1.carryUsed);
			Assert::AreEqual(numCells, rNan.cpu1.nonFiniteCells);
			Assert::AreEqual(numCells, rNan.gpu1.nonFiniteCells);
			Assert::IsTrue(rNan.cpuStand1.IsActive(StandInKind::NonFiniteClosure));
			Assert::IsTrue(rNan.gpuStand1.IsActive(StandInKind::NonFiniteClosure));
			Assert::AreEqual(0.0, RmsDiff(rNan.cpu1.outputY, rec1Unsplit), 1e-9);
			Assert::AreEqual(0.0, RmsDiff(rNan.gpu1.outputY, rec1Unsplit), 1e-6);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuAndCpuChromaGradualFadeKeepTapersAnchorAndDeltaWithoutGuardCancellation)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuAndCpuChromaGradualFadeKeepTapersAnchorAndDeltaWithoutGuardCancellation)
		{
			WarpContext wc;
			const int lumaW = 64;
			const int lumaH = 64;
			const int cW = 32;
			const int cH = 32;
			const int cw = lumaW / 16;
			const int ch = lumaH / 16;
			const size_t numCells = static_cast<size_t>(cw * ch);

			VpSkeleton::PlTexBackend backend(wc.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
				TdV2ChromaGpuEngine::ArenaRequests(wc.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, 16), VpSkeleton::Tier::T1)));

			Config cfg;
			cfg.preset = Preset::Reference;
			cfg.mode = Mode::On;
			cfg.sourceBits = 10;
			cfg.cellPx = 16;
			cfg.grainCellPx = 16;

			const SitingSpec siting{ -0.5f, 0.0f };
			const ActiveRect fullLuma = ActiveRect::Full(lumaW, lumaH);
			const CellMotionField zeroMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			const TdV2Plane masterCb444 = MakeTexturedMaster(lumaW, lumaH, 0.25);
			const TdV2Plane masterCr444 = MakeTexturedMaster(lumaW, lumaH, 1.10);
			const TdV2Plane codedMasterCb = ReduceToCodedGrid(masterCb444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma);
			const TdV2Plane codedMasterCr = ReduceToCodedGrid(masterCr444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma);

			const TdV2Plane prevCb = AddGaussianNoise(codedMasterCb, 301u, 0.7);
			const TdV2Plane prevCr = AddGaussianNoise(codedMasterCr, 302u, 0.7);
			const TdV2Plane nextCb = AddGaussianNoise(codedMasterCb, 303u, 0.7);
			const TdV2Plane nextCr = AddGaussianNoise(codedMasterCr, 304u, 0.7);

			TdV2Plane codedOwnCb, codedOwnCr;
			const TdV2Plane codedDegCb = MakeDegradedFromMaster(codedMasterCb, 401u, 2.2, 2.5, codedOwnCb);
			const TdV2Plane codedDegCr = MakeDegradedFromMaster(codedMasterCr, 402u, 2.2, 2.5, codedOwnCr);
			Assert::AreEqual(cW, codedDegCb.w);
			Assert::AreEqual(cH, codedDegCb.h);

			const TdV2Plane delivCleanCb = UpsampleDeltaToDelivered(codedDegCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, fullLuma);
			const TdV2Plane delivCleanCr = UpsampleDeltaToDelivered(codedDegCr, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, fullLuma);
			const TdV2Plane delivGrainCb = AddGaussianNoise(TdV2Plane(lumaW, lumaH, 0.0), 501u, 2.8);
			const TdV2Plane delivGrainCr = AddGaussianNoise(TdV2Plane(lumaW, lumaH, 0.0), 502u, 2.8);
			const TdV2Plane delivRecCb = AddPlane(delivCleanCb, delivGrainCb);
			const TdV2Plane delivRecCr = AddPlane(delivCleanCr, delivGrainCr);
			// Small leaked grain inside clean chroma engages BOTH Layer-2/3 leak budget and Layer-4 TdGuardPort!
			const std::vector<double> leakVar(numCells, 0.8);

			auto runChromaPair = [&](double fadeVal, ChromaProcessResult& outCpu, ChromaGpuProcessResult& outGpu,
			                         double& outWeightBefore, double& outWeightAfter, bool& outAnchorHasData,
			                         StandInRegistry& cpuSt, StandInRegistry& gpuSt)
			{
				TdV2ChromaEngine cpuEng(cfg);
				ChromaGpuGuard gpuEng(wc.gpu, cfg);

				// Seed CPU 4:4:4 AnchorCanvas on Frame 0 (isAnchorFrame = true, splitActive = true).
				ChromaProcessInput seedIn;
				seedIn.seq = 0;
				seedIn.lumaW = lumaW;
				seedIn.lumaH = lumaH;
				seedIn.codedSubsampling = ChromaSubsampling::Yuv420;
				seedIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
				seedIn.siting = siting;
				seedIn.receivedCb = masterCb444;
				seedIn.receivedCr = masterCr444;
				seedIn.cleanCb = masterCb444;
				seedIn.cleanCr = masterCr444;
				seedIn.hasCleanSplit = true;
				seedIn.isAnchorFrame = true;
				seedIn.hasStepPrior = true;
				seedIn.estimatedStepDelta = 8.0;
				(void)cpuEng.ProcessFrame(seedIn, nullptr);
				Assert::IsTrue(cpuEng.AnchorCanvas().HasData());
				outWeightBefore = RmsAbs(cpuEng.AnchorCanvas().WeightPlane());
				Assert::IsTrue(outWeightBefore > 0.0);

				// Frame 1: test fadeKeep on peak-assisted chroma fusion (CPU + WARP GPU).
				// Pass explicit prevPeak + nextPeak (disable anchorCanvas ref on CPU only when testing pure CPU/GPU delta parity,
				// and also verify AnchorCanvas().WeightPlane() decay on CPU!).
				ChromaProcessInput in;
				in.seq = 1;
				in.lumaW = lumaW;
				in.lumaH = lumaH;
				in.codedSubsampling = ChromaSubsampling::Yuv420;
				in.deliveredFormat = DeliveredChromaFormat::Yuv444;
				in.siting = siting;
				in.receivedCb = delivRecCb;
				in.receivedCr = delivRecCr;
				in.cleanCb = delivCleanCb;
				in.cleanCr = delivCleanCr;
				in.hasCleanSplit = true;
				in.retainedGrainVarCb = leakVar;
				in.retainedGrainVarCr = leakVar;
				in.hasStepPrior = true;
				in.estimatedStepDelta = 14.0;
				in.prevPeak.cleanCb = &prevCb;
				in.prevPeak.cleanCr = &prevCr;
				in.prevPeak.lumaMotion = &zeroMf;
				in.nextPeak.cleanCb = &nextCb;
				in.nextPeak.cleanCr = &nextCr;
				in.nextPeak.lumaMotion = &zeroMf;
				in.fadeKeep = fadeVal;

				outCpu = cpuEng.ProcessFrame(in, &cpuSt);
				outAnchorHasData = cpuEng.AnchorCanvas().HasData();
				outWeightAfter = outAnchorHasData ? RmsAbs(cpuEng.AnchorCanvas().WeightPlane()) : 0.0;

				arena.BeginFrame();
				outGpu = gpuEng.engine.ProcessChromaPlanes(wc.gpu, wc.dispatch, in, &arena, &gpuSt);
				Assert::IsTrue(outGpu.succeeded);
			};

			ChromaProcessResult cpu100, cpu040, cpu000, cpuNan;
			ChromaGpuProcessResult gpu100, gpu040, gpu000, gpuNan;
			double wBef100 = 0.0, wAft100 = 0.0, wBef040 = 0.0, wAft040 = 0.0, wBef000 = 0.0, wAft000 = 0.0, wBefNan = 0.0, wAftNan = 0.0;
			bool has100 = false, has040 = false, has000 = false, hasNan = false;
			StandInRegistry cs100, gs100, cs040, gs040, cs000, gs000, csNan, gsNan;

			runChromaPair(1.0, cpu100, gpu100, wBef100, wAft100, has100, cs100, gs100);
			runChromaPair(0.4, cpu040, gpu040, wBef040, wAft040, has040, cs040, gs040);
			runChromaPair(0.0, cpu000, gpu000, wBef000, wAft000, has000, cs000, gs000);
			runChromaPair(std::numeric_limits<double>::quiet_NaN(), cpuNan, gpuNan, wBefNan, wAftNan, hasNan, csNan, gsNan);

			// 1. CPU 4:4:4 AnchorCanvas weight decay: fadeKeep=1.0 untouched, fadeKeep=0.4 scales weights by exact 0.4,
			//    fadeKeep=0.0 and NaN reset anchorCanvas cleanly!
			Assert::IsTrue(has100 && !cpu100.anchorReset);
			Assert::AreEqual(wBef100, wAft100, 1e-12);
			Assert::IsTrue(has040 && !cpu040.anchorReset);
			Assert::AreEqual(0.4 * wBef040, wAft040, 1e-12);
			Assert::IsTrue(!has000 && cpu000.anchorReset);
			Assert::IsTrue(!hasNan && cpuNan.anchorReset);

			// 2. Post-guard delivered delta scaling on BOTH CPU and WARP GPU:
			//    Because fadeKeep scales codedDelta AFTER Layer-4 TdGuardPort, RMS(fadeKeep=0.4) == 0.4 * RMS(fadeKeep=1.0)!
			const double cpuRms100 = RmsAbs(cpu100.deliveredDeltaCb);
			const double cpuRms040 = RmsAbs(cpu040.deliveredDeltaCb);
			const double gpuRms100 = RmsAbs(gpu100.deliveredDeltaCb);
			const double gpuRms040 = RmsAbs(gpu040.deliveredDeltaCb);

			Assert::IsTrue(cpuRms100 > 1.5 && gpuRms100 > 1.5,
				FmtMsg("Expected strong chroma delta at fadeKeep=1.0: cpu=%.4f gpu=%.4f", cpuRms100, gpuRms100).c_str());
			Assert::AreEqual(0.4 * cpuRms100, cpuRms040, 0.02 * cpuRms100);
			Assert::AreEqual(0.4 * gpuRms100, gpuRms040, 0.005 * gpuRms100);
			Assert::AreEqual(0.0, RmsAbs(cpu000.deliveredDeltaCb), 1e-9);
			Assert::AreEqual(0.0, RmsAbs(gpu000.deliveredDeltaCb), 1e-6);

			// Also verify unseeded CPU vs GPU chroma parity at fadeKeep=0.4 (exact same 2-peak refs without anchor ref).
			{
				TdV2ChromaEngine cpuNoAnchor(cfg);
				ChromaProcessInput inPlain;
				inPlain.seq = 1;
				inPlain.lumaW = lumaW;
				inPlain.lumaH = lumaH;
				inPlain.codedSubsampling = ChromaSubsampling::Yuv420;
				inPlain.deliveredFormat = DeliveredChromaFormat::Yuv444;
				inPlain.siting = siting;
				inPlain.receivedCb = delivRecCb;
				inPlain.receivedCr = delivRecCr;
				inPlain.cleanCb = delivCleanCb;
				inPlain.cleanCr = delivCleanCr;
				inPlain.hasCleanSplit = true;
				inPlain.retainedGrainVarCb = leakVar;
				inPlain.retainedGrainVarCr = leakVar;
				inPlain.hasStepPrior = true;
				inPlain.estimatedStepDelta = 14.0;
				inPlain.prevPeak.cleanCb = &prevCb;
				inPlain.prevPeak.cleanCr = &prevCr;
				inPlain.prevPeak.lumaMotion = &zeroMf;
				inPlain.nextPeak.cleanCb = &nextCb;
				inPlain.nextPeak.cleanCr = &nextCr;
				inPlain.nextPeak.lumaMotion = &zeroMf;
				inPlain.fadeKeep = 0.4;
				const ChromaProcessResult cpuPlain040 = cpuNoAnchor.ProcessFrame(inPlain, nullptr);
				Assert::IsTrue(RmsDiff(cpuPlain040.outputCb, gpu040.outputCb) < 1e-2,
					FmtMsg("Chroma CPU vs GPU mismatch at fadeKeep=0.4: %.5f", RmsDiff(cpuPlain040.outputCb, gpu040.outputCb)).c_str());
			}

			// 3. NaN fadeKeep fails closed visibly on BOTH CPU and WARP GPU chroma.
			Assert::AreEqual(numCells, cpuNan.nonFiniteCells);
			Assert::AreEqual(numCells, gpuNan.nonFiniteCells);
			Assert::IsTrue(csNan.IsActive(StandInKind::NonFiniteClosure));
			Assert::IsTrue(gsNan.IsActive(StandInKind::NonFiniteClosure));
			Assert::AreEqual(0.0, RmsAbs(cpuNan.deliveredDeltaCb), 1e-9);
			Assert::AreEqual(0.0, RmsAbs(gpuNan.deliveredDeltaCb), 1e-6);
			Assert::AreEqual(0.0, RmsDiff(cpuNan.outputCb, delivRecCb), 1e-9);
			Assert::AreEqual(0.0, RmsDiff(gpuNan.outputCb, delivRecCb), 1e-5);
		}
	};
}
}
