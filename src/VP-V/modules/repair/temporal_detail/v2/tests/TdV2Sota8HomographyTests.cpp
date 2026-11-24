// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 -- Unit 8 (TD-SOTA-8) Adversarial Red-Team CPU + WARP GPU Tests:
// P7 3x3 Camera Homography Per-Pixel Background Warp Across Luma and Chroma:
//   1. CpuHomography3x3ProjectionAndDegenerateClosureInvariants:
//      - Verifies default valid=false, identity, projective zoom/rotation/perspective math, and degenerate
//        w ~= 0 / NaN / Inf rejection.
//   2. GpuAndCpuLumaHomographyEliminatesZoomRotationTileSeamsWhilePreservingObjectMvs (GpuWarp):
//      - Synthetic camera zoom (s=1.045) + rotation (0.035 rad, ~0.91 px intra-cell drift per 16x16 cell)
//        combined with an independently translating foreground RegionMotionClass::Object patch!
//      - Proves cameraHomography.valid=true on Camera cells dramatically reduces clean RMSE and eliminates
//        16-px tile-border seams vs cell-constant valid=false negative control, while leaving moving Object
//        cells strictly on their own object MVs (vs mislabeled-as-Camera negative control!).
//   3. GpuAndCpuChromaHomographySited420WarpEliminatesSeamsAndPreservesObjectMvs (GpuWarp):
//      - Verifies sited coded->luma->H->coded warp on 4:2:0 (shiftX=-0.5) -> 4:4:4 chroma across CPU & WARP GPU.
//   4. GpuAndCpuHomographySingularAndNonFiniteFailClosedVisibly (GpuWarp):
//      - Proves singular (w == 0) and NaN homographies fail closed cleanly (finite passthrough on Camera cells,
//        Object cells unaffected) AND engage StandInKind::NonFiniteClosure with nonFiniteCells > 0 on ALL 4 engines.
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
namespace TdV2Sota8HomographyNs
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

	// Build 3x3 similarity + mild perspective camera homography around (cx0, cy0) mapping current (x, y) -> ref (sx, sy).
	static Homography3x3 MakeZoomRotHomography(double cx0, double cy0, double scale, double thetaRad, double perspX = 0.0)
	{
		const double cs = scale * std::cos(thetaRad);
		const double sn = scale * std::sin(thetaRad);
		Homography3x3 hm;
		hm.valid = true;
		// Numerator before perspective: cx0 + cs*(x - cx0) - sn*(y - cy0)
		hm.h[0] = cs + cx0 * perspX;
		hm.h[1] = -sn;
		hm.h[2] = cx0 - cs * cx0 + sn * cy0 - cx0 * perspX * cx0;
		hm.h[3] = sn + cy0 * perspX;
		hm.h[4] = cs;
		hm.h[5] = cy0 - sn * cx0 - cs * cy0 - cy0 * perspX * cx0;
		hm.h[6] = perspX;
		hm.h[7] = 0.0;
		hm.h[8] = 1.0 - perspX * cx0;
		return hm;
	}

	// Invert 3x3 matrix so we can render exact continuous ground-truth reference frames (ref pixel (rx, ry) -> world (wx, wy)).
	static Homography3x3 InvertHomography(const Homography3x3& m)
	{
		const double a = m.h[0], b = m.h[1], c = m.h[2];
		const double d = m.h[3], e = m.h[4], f = m.h[5];
		const double g = m.h[6], hh = m.h[7], k = m.h[8];

		const double A = e * k - f * hh;
		const double B = -(d * k - f * g);
		const double C = d * hh - e * g;
		const double D = -(b * k - c * hh);
		const double E = a * k - c * g;
		const double F = -(a * hh - b * g);
		const double G = b * f - c * e;
		const double H = -(a * f - c * d);
		const double K = a * e - b * d;
		const double det = a * A + b * B + c * C;

		Homography3x3 inv;
		inv.valid = true;
		inv.h[0] = A / det; inv.h[1] = D / det; inv.h[2] = G / det;
		inv.h[3] = B / det; inv.h[4] = E / det; inv.h[5] = H / det;
		inv.h[6] = C / det; inv.h[7] = F / det; inv.h[8] = K / det;
		return inv;
	}

	static double BgFieldSample(double wx, double wy, double phase, bool bandlimit420 = false)
	{
		const double low = 500.0 + 34.0 * std::sin(2.0 * kPi * wx / 36.0 + phase)
		                         * std::cos(2.0 * kPi * wy / 30.0 - 0.3);
		const double mid = 28.0 * std::sin(2.0 * kPi * (wx / 6.4 + wy / 7.2) + phase)
		                 + 24.0 * std::cos(2.0 * kPi * (wx / 7.8 - wy / 6.0) + 0.7);
		if (bandlimit420)
			return low + mid + 18.0 * std::sin(2.0 * kPi * (wx / 11.0 - wy / 10.0) + 0.3);
		const double fine = 22.0 * std::sin(2.0 * kPi * (wx / 3.1 + wy / 3.5) + phase)
		                  + 18.0 * std::cos(2.0 * kPi * (wx / 3.6 - wy / 2.9) - 0.5);
		return low + mid + fine;
	}

	static double ObjFieldSample(double ox, double oy, double phase, bool bandlimit420 = false)
	{
		const double base = 610.0 + 26.0 * std::cos(2.0 * kPi * ox / 12.0 + phase);
		const double mid = 26.0 * std::sin(2.0 * kPi * (ox / 6.2 - oy / 6.4) + phase);
		if (bandlimit420)
			return base + mid;
		const double fine = 20.0 * std::cos(2.0 * kPi * (ox / 2.9 + oy / 3.3) + 0.4);
		return base + mid + fine;
	}

	// Foreground Object lives in cell (cx=4, cy=2) -> x in [64..80), y in [32..48) on current frame,
	// and translates by (objShiftX, objShiftY) into reference coordinates (x_ref = x_cur + objShift).
	static TdV2Plane RenderZoomRotAndObjFrame(
		int w, int h,
		const Homography3x3* curToThisH,
		double objShiftX, double objShiftY,
		double phase,
		bool bandlimit420 = false)
	{
		TdV2Plane p(w, h, 0.0);
		const Homography3x3 thisToCur = (curToThisH != nullptr) ? InvertHomography(*curToThisH) : Homography3x3{};
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				double wx = static_cast<double>(x);
				double wy = static_cast<double>(y);
				if (curToThisH != nullptr)
				{
					const bool ok = thisToCur.Project(wx, wy, wx, wy);
					Assert::IsTrue(ok);
				}
				p.at(x, y) = BgFieldSample(wx, wy, phase, bandlimit420);

				// Check if pixel lands inside the foreground moving Object patch [64..80) x [32..48) in current-frame coords:
				const double curOx = static_cast<double>(x) - objShiftX;
				const double curOy = static_cast<double>(y) - objShiftY;
				if (curOx >= 54.0 && curOx < 90.0 && curOy >= 22.0 && curOy < 58.0)
				{
					// Place crisp object covering cell (4, 2) plus a 10-px Gaussian+bicubic guard apron so object warp has zero bg bleed.
					p.at(x, y) = ObjFieldSample(curOx, curOy, phase, bandlimit420);
				}
			}
		}
		return p;
	}

	static TdV2Plane WipeDetailForBFrame(const TdV2Plane& cleanMaster, uint32_t seed)
	{
		const ThreeBandPlanes b = DecomposeActiveBands(cleanMaster, ActiveRect::Full(cleanMaster.w, cleanMaster.h));
		TdV2Plane deg(cleanMaster.w, cleanMaster.h, 0.0);
		std::mt19937 rng(seed);
		std::normal_distribution<double> qn(0.0, 1.8);
		for (size_t i = 0; i < cleanMaster.v.size(); ++i)
			deg.v[i] = b.L.v[i] + 0.22 * b.M.v[i] + 0.18 * b.H.v[i] + qn(rng);
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

	static CellMotionField BuildHybridMotionField(
		int cw, int ch, int cellPx,
		const Homography3x3& camH,
		double objVx, double objVy,
		bool mislabelObjAsCamera)
	{
		CellMotionField mf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
		for (int cy = 0; cy < ch; ++cy)
		{
			for (int cx = 0; cx < cw; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy * cw + cx);
				const double centerX = static_cast<double>(cx * cellPx) + 0.5 * static_cast<double>(cellPx - 1);
				const double centerY = static_cast<double>(cy * cellPx) + 0.5 * static_cast<double>(cellPx - 1);
				double sx = centerX, sy = centerY;
				(void)camH.Project(centerX, centerY, sx, sy);
				const double cvx = sx - centerX;
				const double cvy = sy - centerY;
				mf.camVx[idx] = cvx;
				mf.camVy[idx] = cvy;

				// Cell (4, 2) and its 4-px apron cells? Keep strictly cell (4, 2) as foreground Object!
				if (cx == 4 && cy == 2)
				{
					mf.cls[idx] = mislabelObjAsCamera ? RegionMotionClass::Camera : RegionMotionClass::Object;
					// Mild relative object motion (<= 0.5 px relative to camVx so speedGate stays 1.0, while actual
					// absolute vector (-1.5, +0.75) differs by > 2.8 px from camH (+1.35, -0.45) at cell (4, 2)!).
					mf.vx[idx] = objVx;
					mf.vy[idx] = objVy;
					mf.camVx[idx] = objVx; // Local background/rig speed matches so speedGate = 1.0 isolates pure warp accuracy!
					mf.camVy[idx] = objVy;
				}
				else
				{
					mf.cls[idx] = RegionMotionClass::Camera;
					mf.vx[idx] = cvx;
					mf.vy[idx] = cvy;
				}
			}
		}
		return mf;
	}

	static bool IsNearObjectPatch(int x, int y)
	{
		return (x >= 46 && y >= 14);
	}

	static double ComputeCameraBgRmse(const TdV2Plane& est, const TdV2Plane& gt, int borderMargin, int modeAll0Border1Interior2)
	{
		double sumSq = 0.0;
		size_t cnt = 0;
		for (int y = borderMargin; y < est.h - borderMargin; ++y)
		{
			for (int x = borderMargin; x < est.w - borderMargin; ++x)
			{
				if (IsNearObjectPatch(x, y))
					continue;
				const int rx = x % 16;
				const int ry = y % 16;
				const bool onSeam = (rx <= 1 || rx >= 14 || ry <= 1 || ry >= 14);
				if (modeAll0Border1Interior2 == 1 && !onSeam)
					continue;
				if (modeAll0Border1Interior2 == 2 && onSeam)
					continue;
				const double d = est.at(x, y) - gt.at(x, y);
				sumSq += d * d;
				++cnt;
			}
		}
		return cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
	}

	static double ComputeObjectCoreRmse(const TdV2Plane& est, const TdV2Plane& gt)
	{
		double sumSq = 0.0;
		size_t cnt = 0;
		for (int y = 34; y < 46; ++y)
		{
			for (int x = 66; x < 78; ++x)
			{
				const double d = est.at(x, y) - gt.at(x, y);
				sumSq += d * d;
				++cnt;
			}
		}
		return cnt > 0 ? std::sqrt(sumSq / static_cast<double>(cnt)) : 0.0;
	}

	static bool PlaneAllFinite(const TdV2Plane& p)
	{
		for (size_t i = 0; i < p.v.size(); ++i)
			if (!std::isfinite(p.v[i]))
				return false;
		return true;
	}

	static double RmsDiffFull(const TdV2Plane& a, const TdV2Plane& b)
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

	TEST_CLASS(TdV2Sota8HomographyTests)
	{
	public:
		TEST_METHOD(CpuHomography3x3ProjectionAndDegenerateClosureInvariants)
		{
			Homography3x3 hDef;
			Assert::IsFalse(hDef.valid);
			double sx = -1.0, sy = -1.0;
			Assert::IsFalse(hDef.Project(20.0, 15.0, sx, sy));

			hDef.valid = true;
			Assert::IsTrue(hDef.Project(20.0, 15.0, sx, sy));
			Assert::AreEqual(20.0, sx, 1e-12);
			Assert::AreEqual(15.0, sy, 1e-12);

			const Homography3x3 hProj = MakeZoomRotHomography(47.5, 31.5, 1.045, 0.035, 1.5e-4);
			const Homography3x3 hInv = InvertHomography(hProj);
			Assert::IsTrue(hProj.Project(12.0, 50.0, sx, sy));
			double bx = 0.0, by = 0.0;
			Assert::IsTrue(hInv.Project(sx, sy, bx, by));
			Assert::AreEqual(12.0, bx, 1e-9);
			Assert::AreEqual(50.0, by, 1e-9);

			// Singular w == 0 and near-zero |w| <= kEngineNumFloor fail closed.
			Homography3x3 hZeroW = hProj;
			hZeroW.h[6] = 0.0;
			hZeroW.h[7] = 0.0;
			hZeroW.h[8] = 0.0;
			Assert::IsFalse(hZeroW.Project(10.0, 10.0, sx, sy));

			hZeroW.h[8] = 0.5 * kSolvePivotFloor;
			Assert::IsFalse(hZeroW.Project(10.0, 10.0, sx, sy));

			// NaN / Inf matrix entries fail closed.
			Homography3x3 hNan = hProj;
			hNan.h[2] = std::numeric_limits<double>::quiet_NaN();
			Assert::IsFalse(hNan.AllFinite());
			Assert::IsFalse(hNan.Project(10.0, 10.0, sx, sy));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuAndCpuLumaHomographyEliminatesZoomRotationTileSeamsWhilePreservingObjectMvs)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuAndCpuLumaHomographyEliminatesZoomRotationTileSeamsWhilePreservingObjectMvs)
		{
			WarpContext wc;
			VpSkeleton::PlTexBackend backend(wc.gpu);

			const int w = 96;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;

			// Genuine camera zoom (4.5%) + rotation (2.0 deg = 0.035 rad) + perspective -> ~0.91 px drift per 16x16 cell!
			const Homography3x3 hPrev = MakeZoomRotHomography(47.5, 31.5, 1.045, 0.035, 1.2e-4);
			const Homography3x3 hNext = MakeZoomRotHomography(47.5, 31.5, 1.0 / 1.045, -0.035, -1.2e-4);

			const double objPrevVx = -1.5, objPrevVy = +0.75;
			const double objNextVx = +1.5, objNextVy = -0.75;

			const TdV2Plane curMaster = RenderZoomRotAndObjFrame(w, h, nullptr, 0.0, 0.0, 0.2);
			const TdV2Plane prevRef = AddSmallNoise(RenderZoomRotAndObjFrame(w, h, &hPrev, objPrevVx, objPrevVy, 0.2), 11u, 0.6);
			const TdV2Plane nextRef = AddSmallNoise(RenderZoomRotAndObjFrame(w, h, &hNext, objNextVx, objNextVy, 0.2), 12u, 0.6);
			const TdV2Plane curDeg = WipeDetailForBFrame(curMaster, 21u);

			const CellMotionField mfPrev = BuildHybridMotionField(cw, ch, 16, hPrev, objPrevVx, objPrevVy, false);
			const CellMotionField mfNext = BuildHybridMotionField(cw, ch, 16, hNext, objNextVx, objNextVy, false);
			// Negative control 2: mislabeling the moving Object cell as Camera overwrites its MV with background H!
			const CellMotionField mfPrevMislabeled = BuildHybridMotionField(cw, ch, 16, hPrev, objPrevVx, objPrevVy, true);
			const CellMotionField mfNextMislabeled = BuildHybridMotionField(cw, ch, 16, hNext, objNextVx, objNextVy, true);

			for (int presetIdx = 0; presetIdx < 2; ++presetIdx)
			{
				Config cfg;
				cfg.preset = (presetIdx == 0) ? Preset::Reference : Preset::RealTime;
				cfg.mode = Mode::On;
				cfg.sourceBits = 10;
				cfg.cellPx = 16;
				cfg.momentPoolRadiusCells = 0; // Isolate cell-level warp accuracy cleanly

				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
					TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, cfg.preset), VpSkeleton::Tier::T1)));

				FrameProcessInput inNoH;
				inNoH.seq = 0;
				inNoH.receivedY = curDeg;
				inNoH.cleanY = curDeg;
				inNoH.hasCleanSplit = true;
				inNoH.hasStepPrior = true;
				inNoH.estimatedStepDelta = 14.0;
				inNoH.prevPeak.cleanPlane = &prevRef;
				inNoH.prevPeak.motion = &mfPrev;
				inNoH.prevPeak.cameraHomography.valid = false;
				inNoH.nextPeak.cleanPlane = &nextRef;
				inNoH.nextPeak.motion = &mfNext;
				inNoH.nextPeak.cameraHomography.valid = false;

				FrameProcessInput inWithH = inNoH;
				inWithH.prevPeak.cameraHomography = hPrev;
				inWithH.nextPeak.cameraHomography = hNext;

				FrameProcessInput inMislabeled = inWithH;
				inMislabeled.prevPeak.motion = &mfPrevMislabeled;
				inMislabeled.nextPeak.motion = &mfNextMislabeled;

				TdV2ReferenceEngine cpuNoH(cfg), cpuWithH(cfg), cpuMis(cfg);
				LumaGpuGuard gpuNoH(wc.gpu, cfg), gpuWithH(wc.gpu, cfg), gpuMis(wc.gpu, cfg);

				const FrameProcessResult cNoH = cpuNoH.ProcessFrame(inNoH, nullptr);
				const FrameProcessResult cWithH = cpuWithH.ProcessFrame(inWithH, nullptr);
				const FrameProcessResult cMis = cpuMis.ProcessFrame(inMislabeled, nullptr);

				arena.BeginFrame();
				const GpuProcessResult gNoH = gpuNoH.engine.ProcessFrame(wc.gpu, wc.dispatch, inNoH, &arena, nullptr);
				arena.BeginFrame();
				const GpuProcessResult gWithH = gpuWithH.engine.ProcessFrame(wc.gpu, wc.dispatch, inWithH, &arena, nullptr);
				arena.BeginFrame();
				const GpuProcessResult gMis = gpuMis.engine.ProcessFrame(wc.gpu, wc.dispatch, inMislabeled, &arena, nullptr);
				Assert::IsTrue(gNoH.succeeded && gWithH.succeeded && gMis.succeeded);

				// 1. CPU vs WARP GPU parity on Reference preset.
				if (cfg.preset == Preset::Reference)
				{
					Assert::IsTrue(RmsDiffFull(cWithH.outputY, gWithH.outputY) < 2e-3,
						FmtMsg("Luma homography CPU vs GPU diff=%.5f", RmsDiffFull(cWithH.outputY, gWithH.outputY)).c_str());
				}

				// 2. Camera background RMSE: per-pixel homography dramatically beats cell-constant MVs AND raw degraded input!
				const double bgDeg = ComputeCameraBgRmse(curDeg, curMaster, 6, 0);
				const double bgCpuNoH = ComputeCameraBgRmse(cNoH.outputY, curMaster, 6, 0);
				const double bgCpuWithH = ComputeCameraBgRmse(cWithH.outputY, curMaster, 6, 0);
				const double bgGpuNoH = ComputeCameraBgRmse(gNoH.outputY, curMaster, 6, 0);
				const double bgGpuWithH = ComputeCameraBgRmse(gWithH.outputY, curMaster, 6, 0);

				Assert::IsTrue(bgCpuWithH < 0.62 * bgCpuNoH && bgCpuWithH < 0.50 * bgDeg,
					FmtMsg("CPU homography failed to beat cell-constant control: withH=%.4f noH=%.4f deg=%.4f", bgCpuWithH, bgCpuNoH, bgDeg).c_str());
				Assert::IsTrue(bgGpuWithH < 0.62 * bgGpuNoH && bgGpuWithH < 0.50 * bgDeg,
					FmtMsg("GPU homography failed to beat cell-constant control: withH=%.4f noH=%.4f deg=%.4f", bgGpuWithH, bgGpuNoH, bgDeg).c_str());

				// 3. 16-px tile-boundary seam elimination: cell-constant MVs jump across 16-px cell borders, inflating
				//    seam RMSE relative to cell-center interior RMSE; per-pixel homography eliminates the tile seam!
				const double seamNoH = ComputeCameraBgRmse(gNoH.outputY, curMaster, 6, 1);
				const double seamWithH = ComputeCameraBgRmse(gWithH.outputY, curMaster, 6, 1);
				const double interWithH = ComputeCameraBgRmse(gWithH.outputY, curMaster, 6, 2);
				Assert::IsTrue(seamWithH < 0.58 * seamNoH,
					FmtMsg("GPU 16-px tile seam not eliminated: seamWithH=%.4f seamNoH=%.4f", seamWithH, seamNoH).c_str());
				Assert::IsTrue(seamWithH < 1.15 * interWithH,
					FmtMsg("GPU seam vs interior excess with homography: seam=%.4f inter=%.4f", seamWithH, interWithH).c_str());

				// 4. Foreground Object MV preservation: Object cell (4, 2) keeps its own MVs (matches valid=false recovery
				//    and dramatically beats the mislabeled-as-Camera negative control!).
				const double objDeg = ComputeObjectCoreRmse(curDeg, curMaster);
				const double objGpuNoH = ComputeObjectCoreRmse(gNoH.outputY, curMaster);
				const double objGpuWithH = ComputeObjectCoreRmse(gWithH.outputY, curMaster);
				const double objGpuMis = ComputeObjectCoreRmse(gMis.outputY, curMaster);
				const double objCpuWithH = ComputeObjectCoreRmse(cWithH.outputY, curMaster);
				const double objCpuMis = ComputeObjectCoreRmse(cMis.outputY, curMaster);

				Assert::IsTrue(objGpuWithH < 0.55 * objDeg && std::fabs(objGpuWithH - objGpuNoH) < 0.05,
					FmtMsg("GPU Object cell MVs corrupted by cameraHomography: withH=%.4f noH=%.4f deg=%.4f", objGpuWithH, objGpuNoH, objDeg).c_str());
				Assert::IsTrue(objGpuWithH < 0.55 * objGpuMis && objCpuWithH < 0.55 * objCpuMis,
					FmtMsg("Object preservation vs mislabeled negative control failed: objWithH=%.4f objMis=%.4f", objGpuWithH, objGpuMis).c_str());
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuAndCpuChromaHomographySited420WarpEliminatesSeamsAndPreservesObjectMvs)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuAndCpuChromaHomographySited420WarpEliminatesSeamsAndPreservesObjectMvs)
		{
			WarpContext wc;
			const int lumaW = 96;
			const int lumaH = 64;
			const int cw = lumaW / 16;
			const int ch = lumaH / 16;
			const SitingSpec siting{ -0.5f, 0.0f };
			const ActiveRect fullLuma = ActiveRect::Full(lumaW, lumaH);

			VpSkeleton::PlTexBackend backend(wc.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(
				TdV2ChromaGpuEngine::ArenaRequests(wc.gpu, lumaW, lumaH, ChromaSubsampling::Yuv420, 16), VpSkeleton::Tier::T1)));

			// Stronger zoom (7.5%) + rotation (0.055 rad) so coded 4:2:0 sub-pixel drift is ~0.75 coded-px per cell!
			const Homography3x3 hPrev = MakeZoomRotHomography(47.5, 31.5, 1.075, 0.055, 0.0);
			const Homography3x3 hNext = MakeZoomRotHomography(47.5, 31.5, 1.0 / 1.075, -0.055, 0.0);
			const double objPrevVx = -2.0, objPrevVy = +1.0;
			const double objNextVx = +2.0, objNextVy = -1.0;

			const TdV2Plane curCb444 = RenderZoomRotAndObjFrame(lumaW, lumaH, nullptr, 0.0, 0.0, 0.35, true);
			const TdV2Plane curCr444 = RenderZoomRotAndObjFrame(lumaW, lumaH, nullptr, 0.0, 0.0, 1.25, true);
			const TdV2Plane prevCb444 = RenderZoomRotAndObjFrame(lumaW, lumaH, &hPrev, objPrevVx, objPrevVy, 0.35, true);
			const TdV2Plane prevCr444 = RenderZoomRotAndObjFrame(lumaW, lumaH, &hPrev, objPrevVx, objPrevVy, 1.25, true);
			const TdV2Plane nextCb444 = RenderZoomRotAndObjFrame(lumaW, lumaH, &hNext, objNextVx, objNextVy, 0.35, true);
			const TdV2Plane nextCr444 = RenderZoomRotAndObjFrame(lumaW, lumaH, &hNext, objNextVx, objNextVy, 1.25, true);

			const TdV2Plane codedCurMasterCb = ReduceToCodedGrid(curCb444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma);
			const TdV2Plane codedCurMasterCr = ReduceToCodedGrid(curCr444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma);
			const TdV2Plane codedPrevCb = AddSmallNoise(ReduceToCodedGrid(prevCb444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma), 31u, 0.5);
			const TdV2Plane codedPrevCr = AddSmallNoise(ReduceToCodedGrid(prevCr444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma), 32u, 0.5);
			const TdV2Plane codedNextCb = AddSmallNoise(ReduceToCodedGrid(nextCb444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma), 33u, 0.5);
			const TdV2Plane codedNextCr = AddSmallNoise(ReduceToCodedGrid(nextCr444, lumaW, lumaH, DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv420, siting, fullLuma), 34u, 0.5);

			const TdV2Plane codedDegCb = WipeDetailForBFrame(codedCurMasterCb, 41u);
			const TdV2Plane codedDegCr = WipeDetailForBFrame(codedCurMasterCr, 42u);
			const TdV2Plane delivDegCb = UpsampleDeltaToDelivered(codedDegCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, fullLuma);
			const TdV2Plane delivDegCr = UpsampleDeltaToDelivered(codedDegCr, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, fullLuma);
			const TdV2Plane delivTargetCb = UpsampleDeltaToDelivered(codedCurMasterCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, siting, fullLuma);

			const CellMotionField mfPrev = BuildHybridMotionField(cw, ch, 16, hPrev, objPrevVx, objPrevVy, false);
			const CellMotionField mfNext = BuildHybridMotionField(cw, ch, 16, hNext, objNextVx, objNextVy, false);

			Config cfg;
			cfg.preset = Preset::Reference;
			cfg.mode = Mode::On;
			cfg.sourceBits = 10;
			cfg.cellPx = 16;
			cfg.momentPoolRadiusCells = 0;

			ChromaProcessInput inNoH;
			inNoH.seq = 0;
			inNoH.lumaW = lumaW;
			inNoH.lumaH = lumaH;
			inNoH.codedSubsampling = ChromaSubsampling::Yuv420;
			inNoH.deliveredFormat = DeliveredChromaFormat::Yuv444;
			inNoH.siting = siting;
			inNoH.receivedCb = delivDegCb;
			inNoH.receivedCr = delivDegCr;
			inNoH.cleanCb = delivDegCb;
			inNoH.cleanCr = delivDegCr;
			inNoH.hasCleanSplit = true;
			inNoH.hasStepPrior = true;
			inNoH.estimatedStepDelta = 14.0;
			inNoH.prevPeak.cleanCb = &codedPrevCb;
			inNoH.prevPeak.cleanCr = &codedPrevCr;
			inNoH.prevPeak.lumaMotion = &mfPrev;
			inNoH.nextPeak.cleanCb = &codedNextCb;
			inNoH.nextPeak.cleanCr = &codedNextCr;
			inNoH.nextPeak.lumaMotion = &mfNext;

			ChromaProcessInput inWithH = inNoH;
			inWithH.prevPeak.cameraHomography = hPrev;
			inWithH.nextPeak.cameraHomography = hNext;

			TdV2ChromaEngine cpuNoH(cfg), cpuWithH(cfg);
			ChromaGpuGuard gpuNoH(wc.gpu, cfg), gpuWithH(wc.gpu, cfg);

			const ChromaProcessResult cNoH = cpuNoH.ProcessFrame(inNoH, nullptr);
			const ChromaProcessResult cWithH = cpuWithH.ProcessFrame(inWithH, nullptr);
			arena.BeginFrame();
			const ChromaGpuProcessResult gNoH = gpuNoH.engine.ProcessChromaPlanes(wc.gpu, wc.dispatch, inNoH, &arena, nullptr);
			arena.BeginFrame();
			const ChromaGpuProcessResult gWithH = gpuWithH.engine.ProcessChromaPlanes(wc.gpu, wc.dispatch, inWithH, &arena, nullptr);
			Assert::IsTrue(gNoH.succeeded && gWithH.succeeded);

			Assert::IsTrue(RmsDiffFull(cWithH.outputCb, gWithH.outputCb) < 1e-2,
				FmtMsg("Chroma homography CPU vs GPU diff=%.5f", RmsDiffFull(cWithH.outputCb, gWithH.outputCb)).c_str());

			const double bgCpuNoH = ComputeCameraBgRmse(cNoH.outputCb, delivTargetCb, 8, 0);
			const double bgCpuWithH = ComputeCameraBgRmse(cWithH.outputCb, delivTargetCb, 8, 0);
			const double bgGpuNoH = ComputeCameraBgRmse(gNoH.outputCb, delivTargetCb, 8, 0);
			const double bgGpuWithH = ComputeCameraBgRmse(gWithH.outputCb, delivTargetCb, 8, 0);
			Assert::IsTrue(bgCpuWithH < 0.78 * bgCpuNoH && bgGpuWithH < 0.78 * bgGpuNoH,
				FmtMsg("Chroma sited homography failed to beat cell-constant control: withH=%.4f noH=%.4f", bgGpuWithH, bgGpuNoH).c_str());

			const double objGpuNoH = ComputeObjectCoreRmse(gNoH.outputCb, delivTargetCb);
			const double objGpuWithH = ComputeObjectCoreRmse(gWithH.outputCb, delivTargetCb);
			Assert::IsTrue(std::fabs(objGpuWithH - objGpuNoH) < 0.08,
				FmtMsg("Chroma Object cell MVs altered by cameraHomography: withH=%.4f noH=%.4f", objGpuWithH, objGpuNoH).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuAndCpuHomographySingularAndNonFiniteFailClosedVisibly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuAndCpuHomographySingularAndNonFiniteFailClosedVisibly)
		{
			WarpContext wc;
			const int w = 96;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;

			VpSkeleton::PlTexBackend backend(wc.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> lumaArena(backend);
			Assert::IsTrue(lumaArena.Reserve(VpSkeleton::PlanArena(
				TdV2GpuEngine::ArenaRequests(wc.gpu, w, h, 16, Preset::Reference), VpSkeleton::Tier::T1)));
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> chromaArena(backend);
			Assert::IsTrue(chromaArena.Reserve(VpSkeleton::PlanArena(
				TdV2ChromaGpuEngine::ArenaRequests(wc.gpu, w, h, ChromaSubsampling::Yuv444, 16), VpSkeleton::Tier::T1)));

			const Homography3x3 hIdentity;
			const TdV2Plane curMaster = RenderZoomRotAndObjFrame(w, h, nullptr, 0.0, 0.0, 0.1);
			const TdV2Plane refClean = AddSmallNoise(curMaster, 71u, 0.5);
			const TdV2Plane curDeg = WipeDetailForBFrame(curMaster, 72u);
			const CellMotionField mf = BuildHybridMotionField(cw, ch, 16, hIdentity, 0.0, 0.0, false);

			// Case 1: Singular homography (w == 0 everywhere on Camera cells).
			Homography3x3 hSing;
			hSing.valid = true;
			hSing.h[6] = 0.0;
			hSing.h[7] = 0.0;
			hSing.h[8] = 0.0;

			// Case 2: Non-finite NaN entry in homography.
			Homography3x3 hNan;
			hNan.valid = true;
			hNan.h[2] = std::numeric_limits<double>::quiet_NaN();

			Config cfg;
			cfg.preset = Preset::Reference;
			cfg.mode = Mode::On;
			cfg.sourceBits = 10;
			cfg.cellPx = 16;
			cfg.momentPoolRadiusCells = 0;

			const Homography3x3 badCases[2] = { hSing, hNan };
			for (int ci = 0; ci < 2; ++ci)
			{
				FrameProcessInput lIn;
				lIn.seq = 0;
				lIn.receivedY = curDeg;
				lIn.cleanY = curDeg;
				lIn.hasCleanSplit = true;
				lIn.hasStepPrior = true;
				lIn.estimatedStepDelta = 14.0;
				lIn.prevPeak.cleanPlane = &refClean;
				lIn.prevPeak.motion = &mf;
				lIn.prevPeak.cameraHomography = badCases[ci];
				lIn.nextPeak.cleanPlane = &refClean;
				lIn.nextPeak.motion = &mf;
				lIn.nextPeak.cameraHomography = badCases[ci];

				StandInRegistry cpuLumaSt, gpuLumaSt;
				TdV2ReferenceEngine cpuLuma(cfg);
				LumaGpuGuard gpuLuma(wc.gpu, cfg);

				const FrameProcessResult cRes = cpuLuma.ProcessFrame(lIn, &cpuLumaSt);
				lumaArena.BeginFrame();
				const GpuProcessResult gRes = gpuLuma.engine.ProcessFrame(wc.gpu, wc.dispatch, lIn, &lumaArena, &gpuLumaSt);
				Assert::IsTrue(gRes.succeeded);
				Assert::IsTrue(PlaneAllFinite(cRes.outputY) && PlaneAllFinite(gRes.outputY));

				// Foreground Object cell (4, 2) must STILL recover cleanly while Camera cells fail closed to passthrough!
				Assert::IsTrue(ComputeObjectCoreRmse(gRes.outputY, curMaster) < 0.65 * ComputeObjectCoreRmse(curDeg, curMaster));
				Assert::AreEqual(0.0, ComputeCameraBgRmse(cRes.outputY, curDeg, 4, 0), 1e-9);
				Assert::AreEqual(0.0, ComputeCameraBgRmse(gRes.outputY, curDeg, 4, 0), 1e-5);

				// Rule 4 + CPU/GPU parity: singular / NaN homography MUST report nonFiniteCells > 0 AND engage NonFiniteClosure!
				Assert::IsTrue(cRes.nonFiniteCells > 0, L"CPU luma failed to count nonFiniteCells on degenerate homography");
				Assert::IsTrue(cpuLumaSt.IsActive(StandInKind::NonFiniteClosure), L"CPU luma failed to engage NonFiniteClosure");
				Assert::IsTrue(gRes.nonFiniteCells > 0, L"GPU luma silently swallowed degenerate homography (nonFiniteCells == 0)");
				Assert::IsTrue(gpuLumaSt.IsActive(StandInKind::NonFiniteClosure), L"GPU luma failed to engage NonFiniteClosure on degenerate homography");

				// Same check on CPU & WARP GPU Chroma (4:4:4 identity grid for clean isolation).
				ChromaProcessInput cIn;
				cIn.seq = 0;
				cIn.lumaW = w;
				cIn.lumaH = h;
				cIn.codedSubsampling = ChromaSubsampling::Yuv444;
				cIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
				cIn.receivedCb = curDeg;
				cIn.receivedCr = curDeg;
				cIn.cleanCb = curDeg;
				cIn.cleanCr = curDeg;
				cIn.hasCleanSplit = true;
				cIn.hasStepPrior = true;
				cIn.estimatedStepDelta = 14.0;
				cIn.prevPeak.cleanCb = &refClean;
				cIn.prevPeak.cleanCr = &refClean;
				cIn.prevPeak.lumaMotion = &mf;
				cIn.prevPeak.cameraHomography = badCases[ci];
				cIn.nextPeak.cleanCb = &refClean;
				cIn.nextPeak.cleanCr = &refClean;
				cIn.nextPeak.lumaMotion = &mf;
				cIn.nextPeak.cameraHomography = badCases[ci];

				StandInRegistry cpuChSt, gpuChSt;
				TdV2ChromaEngine cpuCh(cfg);
				ChromaGpuGuard gpuCh(wc.gpu, cfg);

				const ChromaProcessResult ccRes = cpuCh.ProcessFrame(cIn, &cpuChSt);
				chromaArena.BeginFrame();
				const ChromaGpuProcessResult gcRes = gpuCh.engine.ProcessChromaPlanes(wc.gpu, wc.dispatch, cIn, &chromaArena, &gpuChSt);
				Assert::IsTrue(gcRes.succeeded);
				Assert::IsTrue(PlaneAllFinite(ccRes.outputCb) && PlaneAllFinite(gcRes.outputCb));
				Assert::IsTrue(ccRes.nonFiniteCells > 0, L"CPU chroma failed to count nonFiniteCells on degenerate homography");
				Assert::IsTrue(cpuChSt.IsActive(StandInKind::NonFiniteClosure), L"CPU chroma failed to engage NonFiniteClosure");
				Assert::IsTrue(gcRes.nonFiniteCells > 0, L"GPU chroma silently swallowed degenerate homography (nonFiniteCells == 0)");
				Assert::IsTrue(gpuChSt.IsActive(StandInKind::NonFiniteClosure), L"GPU chroma failed to engage NonFiniteClosure on degenerate homography");
			}
		}
	};
}
}
