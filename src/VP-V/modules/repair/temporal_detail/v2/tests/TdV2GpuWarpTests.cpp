// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 Adversarial WARP GPU Tests (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5, s3.4):
// Independent Red-Team verification on real D3D11 WARP (100% deterministic, ZERO silent skips,
// mandatory in-test negative controls per Rule 3 & Rule 8):
//   1. GpuMatchesCpuReferenceWithin1e3CodeAcrossPanObjectLetterboxStrictAndCell
//   2. GpuChromaDeltaApplyMatchesCpuTdS4OnYuvAndRgbPreservingBtbWtw
//   3. ScratchArenaShortageAndAbsentInputsFailVisiblyWithoutSilentFallback
//   4. ZeroNanInfPropagationOnGpuShadersAcrossRefMotionAndGrainInputs
//   5. OffAndShadowModesByteIdenticalAndOffIssuesZeroGpuDispatches
//   6. M1ReAuditCleanupDeliveredRefNanMalformedSplatAndRgbGuards
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/ScratchArena.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Shaders.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2GpuWarpNs
{
	using namespace TdV2;
	using namespace TdV2::Gpu;

	constexpr double kPi = 3.14159265358979323846;
	constexpr double kGpuCpuParityTol = 1e-3; // 1e-3 of a 10-bit code (plan s3.2 TD-S5)

	// ---- Deterministic RAII D3D11 WARP Context (ZERO silent skips!) ----
	struct WarpGpuContext
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;

		WarpGpuContext()
		{
			pl_log_params lp = pl_log_default_params;
			lp.log_level = PL_LOG_ERR;
			log = pl_log_create(PL_API_VER, &lp);
			Assert::IsNotNull(log, L"pl_log_create failed on WARP fixture");

			pl_d3d11_params dp = pl_d3d11_default_params;
			dp.force_software = true;
			dp.allow_software = true;
			d3d11 = pl_d3d11_create(log, &dp);
			Assert::IsNotNull(d3d11, L"pl_d3d11_create (force_software=true, allow_software=true) failed");

			gpu = d3d11->gpu;
			Assert::IsNotNull(gpu, L"WARP pl_d3d11->gpu is null");

			dispatch = pl_dispatch_create(log, gpu);
			Assert::IsNotNull(dispatch, L"pl_dispatch_create failed on WARP GPU");
		}

		~WarpGpuContext()
		{
			if (dispatch != nullptr)
				pl_dispatch_destroy(&dispatch);
			if (d3d11 != nullptr)
				pl_d3d11_destroy(&d3d11);
			if (log != nullptr)
				pl_log_destroy(&log);
		}

		WarpGpuContext(const WarpGpuContext&) = delete;
		WarpGpuContext& operator=(const WarpGpuContext&) = delete;
	};

	// RAII guard so TdV2GpuEngine textures are always destroyed before WarpGpuContext tears down pl_d3d11.
	struct GpuEngineGuard
	{
		pl_gpu gpu = nullptr;
		TdV2GpuEngine engine;

		explicit GpuEngineGuard(pl_gpu g, const Config& cfg = Config{})
			: gpu(g), engine(cfg)
		{
		}

		~GpuEngineGuard()
		{
			engine.Release(gpu);
		}

		GpuEngineGuard(const GpuEngineGuard&) = delete;
		GpuEngineGuard& operator=(const GpuEngineGuard&) = delete;
	};

	static std::wstring FormatMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0)
	{
		char buf[384];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	static std::wstring FormatTagMsg(const char* tag, const char* fmt, double a = 0.0, double b = 0.0)
	{
		char detail[256];
		std::snprintf(detail, sizeof(detail), fmt, a, b);
		std::string s = std::string(tag ? tag : "") + ": " + detail;
		return std::wstring(s.begin(), s.end());
	}

	// ---- Synthetic Physics Scene Generators (10-bit code domain) ----
	static double SyntheticSceneSample(double x, double y)
	{
		const double base = 460.0 + 85.0 * std::sin(2.0 * kPi * x / 67.0 + 0.3) * std::cos(2.0 * kPi * y / 59.0 - 0.2);
		const double mid  = 24.0 * std::sin(2.0 * kPi * (x / 11.0 + y / 15.0) + 0.7)
		                  + 18.0 * std::cos(2.0 * kPi * (x / 13.0 - y / 17.0) + 1.1);
		const double fine = 15.0 * std::sin(2.0 * kPi * (x / 4.3 + y / 6.7) + 0.4)
		                  + 12.0 * std::cos(2.0 * kPi * (x / 5.7 - y / 4.9) + 1.9);
		return base + mid + fine;
	}

	static TdV2Plane RenderScene(int w, int h, double shiftX, double shiftY)
	{
		TdV2Plane p(w, h, 0.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				p.at(x, y) = SyntheticSceneSample(static_cast<double>(x) - shiftX, static_cast<double>(y) - shiftY);
		return p;
	}

	static TdV2Plane AddCodingDamage(const TdV2Plane& master, double sigma, uint32_t seed, double fineAtten, const ActiveRect& ar)
	{
		TdV2Plane out = master;
		if (fineAtten > 0.0)
		{
			const ThreeBandPlanes b = DecomposeActiveBands(master, ar);
			for (int y = ar.y0; y < ar.y1; ++y)
				for (int x = ar.x0; x < ar.x1; ++x)
					out.at(x, y) = master.at(x, y) - fineAtten * b.H.at(x, y);
		}
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, sigma);
		for (int y = ar.y0; y < ar.y1; ++y)
			for (int x = ar.x0; x < ar.x1; ++x)
				out.at(x, y) += nd(rng);
		return out;
	}

	static void FillLetterboxBars(TdV2Plane& p, const ActiveRect& ar, double topVal, double botVal)
	{
		for (int y = 0; y < p.h; ++y)
		{
			if (y < ar.y0)
			{
				for (int x = 0; x < p.w; ++x)
					p.at(x, y) = topVal;
			}
			else if (y >= ar.y1)
			{
				for (int x = 0; x < p.w; ++x)
					p.at(x, y) = botVal;
			}
		}
	}

	static TdV2Plane SubtractPlanes(const TdV2Plane& a, const TdV2Plane& b)
	{
		Assert::AreEqual(a.w, b.w, L"SubtractPlanes width mismatch");
		Assert::AreEqual(a.h, b.h, L"SubtractPlanes height mismatch");
		TdV2Plane out(a.w, a.h, 0.0);
		for (size_t i = 0; i < out.v.size(); ++i)
			out.v[i] = a.v[i] - b.v[i];
		return out;
	}

	static double MaxAbsDiffRect(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& r)
	{
		if (a.w != b.w || a.h != b.h)
			return std::numeric_limits<double>::infinity();
		double m = 0.0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double av = a.at(x, y);
				const double bv = b.at(x, y);
				if (!std::isfinite(av) || !std::isfinite(bv))
					return std::numeric_limits<double>::infinity();
				const double d = std::fabs(av - bv);
				if (d > m) m = d;
			}
		}
		return m;
	}

	static double MaxAbsDiffFull(const TdV2Plane& a, const TdV2Plane& b)
	{
		return MaxAbsDiffRect(a, b, ActiveRect::Full(a.w, a.h));
	}

	static double MaxAbsPlane(const TdV2Plane& a, const ActiveRect& r)
	{
		double m = 0.0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double v = a.at(x, y);
				if (!std::isfinite(v))
					return std::numeric_limits<double>::infinity();
				const double d = std::fabs(v);
				if (d > m) m = d;
			}
		}
		return m;
	}

	static void AssertAllFinitePlane(const TdV2Plane& p, const wchar_t* label)
	{
		Assert::IsFalse(p.Empty(), label);
		for (size_t i = 0; i < p.v.size(); ++i)
		{
			if (!std::isfinite(p.v[i]))
			{
				Assert::Fail(label);
			}
		}
	}

	static void AssertAllFiniteTdMaps(const std::vector<TdMapsView::Block>& maps, const wchar_t* label)
	{
		Assert::IsFalse(maps.empty(), label);
		for (const auto& blk : maps)
		{
			for (int f = 0; f < TdMapsView::kFields; ++f)
			{
				if (!std::isfinite(blk.f[f]))
					Assert::Fail(label);
			}
		}
	}

	// Compare all 12 TdMapsView fields in consistent linear code / dimensionless units:
	// kResidual stores variance (code^2), so convert to RMS codes sqrt(max(0, v)) for 1e-3 code parity
	// while also checking exact integer parity on discrete flags kValidity and kBandMask.
	static double MaxTdMapsDiff(const std::vector<TdMapsView::Block>& a, const std::vector<TdMapsView::Block>& b,
	                            int* worstField = nullptr, double* worstA = nullptr, double* worstB = nullptr)
	{
		if (a.size() != b.size())
			return std::numeric_limits<double>::infinity();
		double m = 0.0;
		for (size_t i = 0; i < a.size(); ++i)
		{
			for (int f = 0; f < TdMapsView::kFields; ++f)
			{
				double av = static_cast<double>(a[i].f[f]);
				double bv = static_cast<double>(b[i].f[f]);
				if (!std::isfinite(av) || !std::isfinite(bv))
					return std::numeric_limits<double>::infinity();
				if (f == TdMapsView::kResidual)
				{
					av = std::sqrt((std::max)(0.0, av));
					bv = std::sqrt((std::max)(0.0, bv));
				}
				const double d = std::fabs(av - bv);
				if (d > m)
				{
					m = d;
					if (worstField) *worstField = f;
					if (worstA) *worstA = av;
					if (worstB) *worstB = bv;
				}
			}
		}
		return m;
	}

	// Naive unprotected bilinear full-frame warp (ignores ActiveRect bars and motion/overlay gates) for negative controls.
	static TdV2Plane NaiveUngatedWarpWholeFrame(const TdV2Plane& ref, double mvx, double mvy)
	{
		TdV2Plane out(ref.w, ref.h, 0.0);
		for (int y = 0; y < ref.h; ++y)
		{
			for (int x = 0; x < ref.w; ++x)
			{
				const double sx = (std::max)(0.0, (std::min)(static_cast<double>(ref.w - 1), static_cast<double>(x) + mvx));
				const double sy = (std::max)(0.0, (std::min)(static_cast<double>(ref.h - 1), static_cast<double>(y) + mvy));
				const int ix0 = static_cast<int>(std::floor(sx));
				const int iy0 = static_cast<int>(std::floor(sy));
				const int ix1 = (std::min)(ref.w - 1, ix0 + 1);
				const int iy1 = (std::min)(ref.h - 1, iy0 + 1);
				const double fx = sx - ix0;
				const double fy = sy - iy0;
				const double v00 = ref.at(ix0, iy0);
				const double v10 = ref.at(ix1, iy0);
				const double v01 = ref.at(ix0, iy1);
				const double v11 = ref.at(ix1, iy1);
				out.at(x, y) = (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy;
			}
		}
		return out;
	}

	// Plant a sharp focused target in cell (0, 2) on peak refs and its optically defocused (sigma=4.5)
	// counterpart in current clean so physics-derived Parseval rack-focus veto fires naturally on BOTH CPU and GPU.
	static void StampRackFocusCell(TdV2Plane& curClean, TdV2Plane& prevRef, TdV2Plane& nextRef,
	                               double prevMvx, double prevMvy, double nextMvx, double nextMvy)
	{
		// Cell (0, 2) is x in [0, 16), y in [32, 48).
		// Sharp periodic focused stripes (period 8 px in M-band passband) + broad step envelope in D3 band,
		// optically defocused with sigma = 4.5 in curClean -> refF ~ 0.65, curF ~ 0.04, refF - curF >> fShiftDelta (0.191).
		auto sharpTarget = [](double gx, double gy) -> double
		{
			const double env = (gx < 8.0) ? 340.0 : 580.0;
			const double bars = 120.0 * std::sin(2.0 * kPi * gx / 8.0) + 40.0 * std::cos(2.0 * kPi * gy / 8.0);
			return env + bars;
		};

		auto stampSharp = [&](TdV2Plane& p, double ox, double oy)
		{
			for (int y = 26; y < 54; ++y)
			{
				const int gy = y + static_cast<int>(std::lround(oy));
				if (gy < 4 || gy >= 60) continue;
				for (int x = 0; x < 22; ++x)
				{
					const double gx = static_cast<double>(x) - ox;
					p.at(x, gy) = sharpTarget(gx, static_cast<double>(y));
				}
			}
		};
		stampSharp(prevRef, prevMvx, prevMvy);
		stampSharp(nextRef, nextMvx, nextMvy);

		// Optical defocus of sharpTarget by Gaussian PSF sigma = 4.5:
		//   - sinusoid of period P is attenuated by exp(-2 * pi^2 * sigma^2 / P^2) ~= exp(-6.24) ~= 0.0019
		//   - step edge env blurs to erf((x - 8) / (sqrt(2) * sigma))
		const double blurSigma = 4.5;
		const double invDenom = 1.0 / (std::sqrt(2.0) * blurSigma);
		const double atten = std::exp(-2.0 * kPi * kPi * blurSigma * blurSigma / 64.0);
		for (int y = 32; y < 48; ++y)
		{
			for (int x = 0; x < 16; ++x)
			{
				const double gx = static_cast<double>(x);
				const double gy = static_cast<double>(y);
				const double envBlur = 340.0 + 240.0 * 0.5 * (1.0 + std::erf((gx - 8.0) * invDenom));
				const double barsBlur = atten * (120.0 * std::sin(2.0 * kPi * gx / 8.0) + 40.0 * std::cos(2.0 * kPi * gy / 8.0));
				curClean.at(x, y) = envBlur + barsBlur;
			}
		}
	}

	// Build a comprehensive multi-feature 64x64 frame covering pan, fast object, Overlay, DynamicTexture,
	// non-8-aligned letterbox bars (y0=4, y1=60), natural rack-focus cell, and Layer-4 TdGuardPort grain pressure.
	struct ComplexSceneBundle
	{
		int w = 64;
		int h = 64;
		int cellsW = 4;
		int cellsH = 4;
		ActiveRect active{ 0, 4, 64, 60 };
		TdV2Plane curClean;
		TdV2Plane ownGrain;
		TdV2Plane receivedY;
		TdV2Plane prevClean;
		TdV2Plane nextClean;
		CellMotionField prevMf;
		CellMotionField nextMf;
		CellMotionField carryMf;
		std::vector<double> retainedGrainVar;
	};

	static ComplexSceneBundle BuildComplexFrame(int frameIdx, double camDx, double camDy, bool stampSpecialRegions)
	{
		ComplexSceneBundle b;
		const double t = static_cast<double>(frameIdx);
		const TdV2Plane masterCur  = RenderScene(b.w, b.h, t * camDx, t * camDy);
		const TdV2Plane masterPrev = RenderScene(b.w, b.h, (t - 1.0) * camDx, (t - 1.0) * camDy);
		const TdV2Plane masterNext = RenderScene(b.w, b.h, (t + 1.0) * camDx, (t + 1.0) * camDy);

		// Coding noise on current valley frame, cleaner peak refs.
		b.curClean  = AddCodingDamage(masterCur, 8.0, 1001u + static_cast<uint32_t>(frameIdx * 37), 0.30, b.active);
		b.prevClean = AddCodingDamage(masterPrev, 1.5, 2003u + static_cast<uint32_t>(frameIdx * 37), 0.0, b.active);
		b.nextClean = AddCodingDamage(masterNext, 1.5, 3007u + static_cast<uint32_t>(frameIdx * 37), 0.0, b.active);

		// Motion fields: cur -> prev is (-camDx, -camDy); cur -> next is (+camDx, +camDy).
		b.prevMf  = CellMotionField::Uniform(b.cellsW, b.cellsH, -camDx, -camDy, -camDx, -camDy, RegionMotionClass::Camera);
		b.nextMf  = CellMotionField::Uniform(b.cellsW, b.cellsH, +camDx, +camDy, +camDx, +camDy, RegionMotionClass::Camera);
		b.carryMf = CellMotionField::Uniform(b.cellsW, b.cellsH, -camDx, -camDy, -camDx, -camDy, RegionMotionClass::Camera);

		b.ownGrain = TdV2Plane(b.w, b.h, 0.0);
		b.retainedGrainVar.assign(static_cast<size_t>(b.cellsW * b.cellsH), 0.0);

		if (stampSpecialRegions)
		{
			// 1. Cell (0, 1): Overlay (x in [0,16), y in [16,32)) — bright static subtitle/UI block.
			const size_t ovIdx = 1u * b.cellsW + 0u;
			b.prevMf.cls[ovIdx] = RegionMotionClass::Overlay;
			b.nextMf.cls[ovIdx] = RegionMotionClass::Overlay;
			b.carryMf.cls[ovIdx] = RegionMotionClass::Overlay;
			for (int y = 16; y < 32; ++y)
				for (int x = 0; x < 16; ++x)
					b.curClean.at(x, y) = ((x + y) % 4 < 2) ? 890.0 : 840.0;

			// 2. Cell (3, 1): DynamicTexture (x in [48,64), y in [16,32)) — chaotic shimmering water.
			const size_t dtIdx = 1u * b.cellsW + 3u;
			b.prevMf.cls[dtIdx] = RegionMotionClass::DynamicTexture;
			b.nextMf.cls[dtIdx] = RegionMotionClass::DynamicTexture;
			b.carryMf.cls[dtIdx] = RegionMotionClass::DynamicTexture;
			std::mt19937 dtRng(4011u + static_cast<uint32_t>(frameIdx * 13));
			std::uniform_real_distribution<double> dtDist(-45.0, 45.0);
			for (int y = 16; y < 32; ++y)
				for (int x = 48; x < 64; ++x)
					b.curClean.at(x, y) = 420.0 + dtDist(dtRng);

			// 3. Cell (2, 2): Fast moving Object (x in [32,48), y in [32,48)) — sharp bright moving box.
			const size_t objIdx = 2u * b.cellsW + 2u;
			b.prevMf.cls[objIdx] = RegionMotionClass::Object;
			b.nextMf.cls[objIdx] = RegionMotionClass::Object;
			b.carryMf.cls[objIdx] = RegionMotionClass::Object;
			b.prevMf.vx[objIdx]  = -camDx + 5.0;  b.prevMf.vy[objIdx]  = -camDy - 3.0;
			b.nextMf.vx[objIdx]  = +camDx - 5.0;  b.nextMf.vy[objIdx]  = +camDy + 3.0;
			b.carryMf.vx[objIdx] = -camDx + 5.0;  b.carryMf.vy[objIdx] = -camDy - 3.0;
			for (int y = 35; y < 45; ++y)
				for (int x = 35; x < 45; ++x)
					b.curClean.at(x, y) = 810.0;

			// 4. Cell (0, 2): Rack-focus cell (sharp edge in prev/next, optically defocused sigma=4.5 in cur).
			StampRackFocusCell(b.curClean, b.prevClean, b.nextClean, -camDx, -camDy, +camDx, +camDy);

			// 5. Layer-4 TdGuardPort grain pressure on smooth-background Camera cells (1, 0) and (2, 0):
			// Smooth sky/wall clean master (near-zero texture fine energy) + coding noise (sigma=5.0) +
			// leaked grain (sigma=5.5, retainedGrainVar=30.0) + ownGrain (sigma=1.5).
			// Temporal blending recovers the smooth background in candClean, dropping fine variance below
			// 0.98^2 * (ownGrainFineE + retainedGrainVar * gains.fine) -> TdGuardPort MUST intervene on
			// outputY ONLY while leaving outputClean / carryClean strictly grain-free!
			std::mt19937 gRng(7777u + static_cast<uint32_t>(frameIdx * 19));
			std::normal_distribution<double> gOwnDist(0.0, 1.5);
			std::normal_distribution<double> gLeakDist(0.0, 5.5);
			std::normal_distribution<double> gCodeDist(0.0, 5.0);
			for (int y = b.active.y0; y < 16; ++y)
			{
				for (int x = 16; x < 48; ++x)
				{
					const double smoothSky = 420.0 + 0.4 * static_cast<double>(x) + 0.3 * static_cast<double>(y);
					b.prevClean.at(x, y) = smoothSky;
					b.nextClean.at(x, y) = smoothSky;
					b.curClean.at(x, y) = smoothSky + gLeakDist(gRng) + gCodeDist(gRng);
					b.ownGrain.at(x, y) = gOwnDist(gRng);
				}
			}
			b.retainedGrainVar[0u * b.cellsW + 1u] = 30.0;
			b.retainedGrainVar[0u * b.cellsW + 2u] = 30.0;
		}

		// Distinct letterbox bars on current frame vs peak refs so any bar bleed is immediately exposed.
		FillLetterboxBars(b.curClean,  b.active, 64.0,  72.0);
		FillLetterboxBars(b.prevClean, b.active, 130.0, 145.0);
		FillLetterboxBars(b.nextClean, b.active, 135.0, 150.0);

		b.receivedY = TdV2Plane(b.w, b.h, 0.0);
		for (size_t i = 0; i < b.receivedY.v.size(); ++i)
			b.receivedY.v[i] = b.curClean.v[i] + b.ownGrain.v[i];

		return b;
	}

	static FrameProcessInput MakeFrameInput(ComplexSceneBundle& b, uint64_t seq, bool withPeaks,
	                                        bool transformIdentified8x8, bool isCut = false)
	{
		FrameProcessInput in;
		in.seq = seq;
		in.receivedY = b.receivedY;
		in.hasCleanSplit = true;
		in.cleanY = b.curClean;
		in.retainedGrainVar = b.retainedGrainVar;
		in.activeRect = b.active;
		in.hasStepPrior = true;
		in.estimatedStepDelta = 24.0;
		in.deadZonePhi = kDeadZonePhiInter;
		in.zeroCoeffFraction = 0.35;
		in.transformIdentified8x8 = transformIdentified8x8;
		in.isCutOrSpanInvalid = isCut;
		in.carryMotion = b.carryMf;
		in.hasCarryMotion = true;
		if (withPeaks)
		{
			in.prevPeak.cleanPlane = &b.prevClean;
			in.prevPeak.motion = &b.prevMf;
			in.nextPeak.cleanPlane = &b.nextClean;
			in.nextPeak.motion = &b.nextMf;
		}
		return in;
	}

	static void AssertCpuGpuFrameParity(const FrameProcessResult& cpu, const GpuProcessResult& gpu,
	                                    const TdV2Plane& curClean, const TdV2Plane& receivedY,
	                                    const ActiveRect& active, const char* tag)
	{
		Assert::IsTrue(gpu.succeeded, FormatTagMsg(tag, "GPU ProcessFrame failed").c_str());
		AssertAllFinitePlane(gpu.outputY, L"GPU outputY contains NaN/Inf");
		AssertAllFinitePlane(gpu.outputClean, L"GPU outputClean contains NaN/Inf");
		AssertAllFinitePlane(gpu.tdDelta, L"GPU tdDelta contains NaN/Inf");
		AssertAllFiniteTdMaps(gpu.tdMaps, L"GPU tdMaps contains NaN/Inf");

		const TdV2Plane cpuDelta = SubtractPlanes(cpu.outputClean, curClean);

		const double diffY = MaxAbsDiffFull(gpu.outputY, cpu.outputY);
		const double diffClean = MaxAbsDiffFull(gpu.outputClean, cpu.outputClean);
		const double diffDelta = MaxAbsDiffFull(gpu.tdDelta, cpuDelta);
		int worstMapField = -1;
		double worstMapGpu = 0.0, worstMapCpu = 0.0;
		const double diffMaps = MaxTdMapsDiff(gpu.tdMaps, cpu.tdMaps, &worstMapField, &worstMapGpu, &worstMapCpu);

		Assert::IsTrue(diffY <= kGpuCpuParityTol,
			FormatTagMsg(tag, "outputY |GPU-CPU|=%.6f > %.6f", diffY, kGpuCpuParityTol).c_str());
		Assert::IsTrue(diffClean <= kGpuCpuParityTol,
			FormatTagMsg(tag, "outputClean |GPU-CPU|=%.6f > %.6f", diffClean, kGpuCpuParityTol).c_str());
		Assert::IsTrue(diffDelta <= kGpuCpuParityTol,
			FormatTagMsg(tag, "tdDelta |GPU-CPU|=%.6f > %.6f", diffDelta, kGpuCpuParityTol).c_str());
		if (diffMaps > kGpuCpuParityTol)
		{
			char msg[256];
			std::snprintf(msg, sizeof(msg), "%s: tdMaps field=%s (%d) |GPU(%.6f)-CPU(%.6f)|=%.6f > %.6f",
				tag, TdMapsView::Name(worstMapField), worstMapField, worstMapGpu, worstMapCpu, diffMaps, kGpuCpuParityTol);
			std::string s(msg);
			Assert::Fail(std::wstring(s.begin(), s.end()).c_str());
		}
		// Exact discrete bitmask / validity parity on every cell of tdMaps.
		for (size_t i = 0; i < gpu.tdMaps.size(); ++i)
		{
			Assert::AreEqual(cpu.tdMaps[i].f[TdMapsView::kValidity], gpu.tdMaps[i].f[TdMapsView::kValidity],
				L"tdMaps kValidity exact mismatch");
			Assert::AreEqual(cpu.tdMaps[i].f[TdMapsView::kBandMask], gpu.tdMaps[i].f[TdMapsView::kBandMask],
				L"tdMaps kBandMask exact mismatch");
		}

		// Bit-identical letterbox bars (y < active.y0 and y >= active.y1).
		const ActiveRect topBar{ 0, 0, receivedY.w, active.y0 };
		const ActiveRect botBar{ 0, active.y1, receivedY.w, receivedY.h };
		Assert::AreEqual(0.0, MaxAbsDiffRect(gpu.outputY, receivedY, topBar), L"Top letterbox bar corrupted on GPU");
		Assert::AreEqual(0.0, MaxAbsDiffRect(gpu.outputY, receivedY, botBar), L"Bottom letterbox bar corrupted on GPU");
		Assert::AreEqual(0.0, MaxAbsPlane(gpu.tdDelta, topBar), L"Top letterbox tdDelta non-zero on GPU");
		Assert::AreEqual(0.0, MaxAbsPlane(gpu.tdDelta, botBar), L"Bottom letterbox tdDelta non-zero on GPU");

		// Exact diagnostic count parity between CPU and GPU.
		Assert::AreEqual(cpu.cellsW, gpu.cellsW, L"cellsW parity mismatch");
		Assert::AreEqual(cpu.cellsH, gpu.cellsH, L"cellsH parity mismatch");
		Assert::AreEqual(cpu.activeRefCount, gpu.activeRefCount, L"activeRefCount parity mismatch");
		Assert::AreEqual(cpu.focusVetoCells, gpu.focusVetoCells, L"focusVetoCells parity mismatch");
		Assert::AreEqual(cpu.guardViolationCells, gpu.guardViolationCells, L"guardViolationCells parity mismatch");
		Assert::AreEqual(cpu.nonFiniteCells, gpu.nonFiniteCells, L"nonFiniteCells parity mismatch");
		Assert::AreEqual(cpu.singularCells, gpu.singularCells, L"singularCells parity mismatch");
		Assert::AreEqual(cpu.singleRefA2Cells, gpu.singleRefA2Cells, L"singleRefA2Cells parity mismatch");
		Assert::AreEqual(cpu.motionStandInRefs, gpu.motionStandInRefs, L"motionStandInRefs parity mismatch");
		Assert::AreEqual(cpu.borderBilinearPixels, gpu.borderBilinearPixels, L"borderBilinearPixels parity mismatch");
		Assert::AreEqual(cpu.carryUsed, gpu.carryUsed, L"carryUsed parity mismatch");
		Assert::AreEqual(cpu.carryReset, gpu.carryReset, L"carryReset parity mismatch");
	}

	TEST_CLASS(TdV2GpuWarpTests)
	{
	public:
		// =========================================================================
		// 1. Multi-frame D3D11 WARP GPU vs CPU Identity across Pan, Fast Object,
		//    Overlay, DynamicTexture, Non-8-Aligned Letterbox, Carry, Cut,
		//    Rack-Focus, Layer-4 TdGuardPort, Transform-Unknown & 8x8 STRICT/CELL
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuMatchesCpuReferenceWithin1e3CodeAcrossPanObjectLetterboxStrictAndCell)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuMatchesCpuReferenceWithin1e3CodeAcrossPanObjectLetterboxStrictAndCell)
		{
			WarpGpuContext ctx;

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;
			cfg.residualMode = ResidualMode::Cell;
			cfg.cellProjIterations = 2;

			TdV2ReferenceEngine cpuEng(cfg);
			GpuEngineGuard gpuGuard(ctx.gpu, cfg);
			StandInRegistry cpuSi, gpuSi;

			const double camDx = 1.5;
			const double camDy = 0.5;

			// ---- Frame 0: Seed recurrent carryClean (no peaks yet) ----
			{
				ComplexSceneBundle f0 = BuildComplexFrame(0, camDx, camDy, /*stampSpecialRegions=*/true);
				FrameProcessInput in0 = MakeFrameInput(f0, 0, /*withPeaks=*/false, /*transformIdentified8x8=*/false);
				const FrameProcessResult cpu0 = cpuEng.ProcessFrame(in0, &cpuSi);
				const GpuProcessResult gpu0 = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in0, nullptr, &gpuSi);

				Assert::IsTrue(gpu0.succeeded, L"Frame 0 seed failed on GPU");
				Assert::IsTrue(cpuEng.HasCarry() && gpuGuard.engine.HasCarry(), L"Frame 0 must seed carry on CPU & GPU");
				Assert::AreEqual(static_cast<size_t>(0), gpu0.activeRefCount, L"Frame 0 activeRefCount must be 0");
				Assert::AreEqual(0.0, MaxAbsDiffFull(gpu0.outputY, cpu0.outputY), L"Frame 0 outputY mismatch");
				Assert::AreEqual(0.0, MaxAbsDiffFull(gpu0.outputClean, cpu0.outputClean), L"Frame 0 outputClean mismatch");
			}

			// ---- Frame 1: Transform-unknown ResidualMode::Cell + Carry (K=3) + All Hazards ----
			{
				ComplexSceneBundle f1 = BuildComplexFrame(1, camDx, camDy, /*stampSpecialRegions=*/true);
				FrameProcessInput in1 = MakeFrameInput(f1, 1, /*withPeaks=*/true, /*transformIdentified8x8=*/false);

				cpuSi.Reset();
				gpuSi.Reset();
				const FrameProcessResult cpu1 = cpuEng.ProcessFrame(in1, &cpuSi);
				const GpuProcessResult gpu1 = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpu1, gpu1, f1.curClean, f1.receivedY, f1.active, "Frame1-CellUnknown");

				// Exact StandInRegistry parity between CPU and GPU on Frame 1 (M3 / L4).
				for (size_t sk = 0; sk < static_cast<size_t>(StandInKind::Count); ++sk)
				{
					const auto kind = static_cast<StandInKind>(sk);
					Assert::AreEqual(cpuSi.IsActive(kind), gpuSi.IsActive(kind),
						FormatTagMsg(MetaOf(kind).catalogKey, "Frame 1 StandInRegistry CPU vs GPU mismatch").c_str());
				}
				Assert::IsTrue(gpuSi.IsActive(StandInKind::BandMemoW2), L"Frame 1 must engage BandMemoW2");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::FocusMapK32), L"Frame 1 must engage FocusMapK32");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::GrainGuardW4c), L"Frame 1 must engage GrainGuardW4c");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::TransformUnknownBound), L"Frame 1 must engage TransformUnknownBound");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::BorderBilinearFallback), L"Frame 1 must engage BorderBilinearFallback");

				// Non-vacuity checks: every feature genuinely exercised!
				Assert::IsTrue(gpu1.carryUsed, L"Frame 1 must use recurrent carry-forward");
				Assert::AreEqual(static_cast<size_t>(3), gpu1.activeRefCount, L"Frame 1 must fuse 3 refs (prev+next+carry)");
				Assert::IsTrue(gpu1.focusVetoCells >= 1u, L"Frame 1 natural rack-focus veto must trigger (>= 1 cell)");
				Assert::IsTrue(gpu1.guardViolationCells >= 1u, L"Frame 1 Layer-4 TdGuardPort must intervene (>= 1 cell)");
				Assert::IsTrue(gpu1.borderBilinearPixels > 0u, L"Frame 1 pan must exercise active-rect border bilinear");
				Assert::IsTrue(gpu1.dispatchCount > 0, L"Frame 1 must execute GPU dispatches");

				// Exact Overlay zero delta in cell (0, 1): x in [0, 16), y in [16, 32).
				const ActiveRect overlayRect{ 0, 16, 16, 32 };
				Assert::AreEqual(0.0, MaxAbsPlane(gpu1.tdDelta, overlayRect), L"Overlay cell must have exact 0.0 tdDelta on GPU");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gpu1.outputY, f1.receivedY, overlayRect), L"Overlay cell must be untouched on GPU");

				// Fast moving object cell (2, 2): x in [32, 48), y in [32, 48) protected.
				const ActiveRect objRect{ 32, 32, 48, 48 };
				Assert::AreEqual(0.0, MaxAbsPlane(gpu1.tdDelta, objRect), L"Fast object cell must have exact 0.0 tdDelta on GPU");

				// Layer-4 purity invariant: outputClean / carryClean stays strictly grain-free (curClean + tdDelta),
				// while outputY receives genuine Layer-4 add-back on violated cells!
				TdV2Plane expectedClean(f1.w, f1.h, 0.0);
				TdV2Plane cleanPlusOwnGrain(f1.w, f1.h, 0.0);
				for (size_t i = 0; i < expectedClean.v.size(); ++i)
				{
					expectedClean.v[i] = f1.curClean.v[i] + gpu1.tdDelta.v[i];
					cleanPlusOwnGrain.v[i] = gpu1.outputClean.v[i] + f1.ownGrain.v[i];
				}
				const double cleanPurityErr = MaxAbsDiffRect(gpu1.outputClean, expectedClean, f1.active);
				Assert::IsTrue(cleanPurityErr <= 1e-4,
					FormatMsg("outputClean contaminated by Layer-4 add-back: err=%.6f", cleanPurityErr).c_str());
				const double guardAddBackPeak = MaxAbsDiffRect(gpu1.outputY, cleanPlusOwnGrain, f1.active);
				Assert::IsTrue(guardAddBackPeak > 0.05,
					FormatMsg("Layer-4 TdGuardPort add-back on outputY too small (vacuous): peak=%.6f", guardAddBackPeak).c_str());

				// ---- NEGATIVE CONTROL (a): No-op receivedY differs substantially from CPU reference (> 2.0 codes) ----
				const double noopDiff = MaxAbsDiffRect(f1.receivedY, cpu1.outputY, f1.active);
				Assert::IsTrue(noopDiff > 2.0,
					FormatMsg("Negative control failed: no-op receivedY diff=%.4f <= 2.0 codes (parity vacuous)", noopDiff).c_str());

				// ---- NEGATIVE CONTROL (b): Naive un-gated temporal average smears fast object, bleeds into Overlay,
				//      and corrupts letterbox bars on the EXACT SAME input! ----
				const TdV2Plane naivePrev = NaiveUngatedWarpWholeFrame(f1.prevClean, -camDx, -camDy);
				const TdV2Plane naiveNext = NaiveUngatedWarpWholeFrame(f1.nextClean, +camDx, +camDy);
				TdV2Plane naiveAvg(f1.w, f1.h, 0.0);
				for (size_t i = 0; i < naiveAvg.v.size(); ++i)
					naiveAvg.v[i] = (f1.receivedY.v[i] + naivePrev.v[i] + naiveNext.v[i]) / 3.0;

				const double naiveObjSmear = MaxAbsDiffRect(naiveAvg, f1.curClean, objRect);
				const double naiveOverlayBleed = MaxAbsDiffRect(naiveAvg, f1.receivedY, overlayRect);
				const double naiveBarCorrupt = (std::max)(
					MaxAbsDiffRect(naiveAvg, f1.receivedY, ActiveRect{ 0, 0, f1.w, f1.active.y0 }),
					MaxAbsDiffRect(naiveAvg, f1.receivedY, ActiveRect{ 0, f1.active.y1, f1.w, f1.h }));

				Assert::IsTrue(naiveObjSmear > 15.0,
					FormatMsg("Negative control failed: naive avg fast-object smear=%.2f <= 15.0", naiveObjSmear).c_str());
				Assert::IsTrue(naiveOverlayBleed > 50.0,
					FormatMsg("Negative control failed: naive avg Overlay bleed=%.2f <= 50.0", naiveOverlayBleed).c_str());
				Assert::IsTrue(naiveBarCorrupt > 20.0,
					FormatMsg("Negative control failed: naive avg bar corruption=%.2f <= 20.0", naiveBarCorrupt).c_str());
			}

			// ---- Frame 2: transformIdentified8x8 = true, ResidualMode::Cell, 2 POCS iterations + Carry ----
			ComplexSceneBundle f2 = BuildComplexFrame(2, camDx, camDy, /*stampSpecialRegions=*/true);
			TdV2Plane gpuCellOutY;
			{
				FrameProcessInput in2 = MakeFrameInput(f2, 2, /*withPeaks=*/true, /*transformIdentified8x8=*/true);
				const FrameProcessResult cpu2 = cpuEng.ProcessFrame(in2, &cpuSi);
				const GpuProcessResult gpu2 = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in2, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpu2, gpu2, f2.curClean, f2.receivedY, f2.active, "Frame2-8x8CellPocs2");
				Assert::IsTrue(gpu2.carryUsed, L"Frame 2 must use carry-forward");
				Assert::AreEqual(0.0, MaxAbsPlane(gpu2.tdDelta, ActiveRect{ 0, 16, 16, 32 }), L"Frame 2 Overlay delta must be 0");
				gpuCellOutY = gpu2.outputY;
			}

			// ---- Frame 3: transformIdentified8x8 = true, ResidualMode::Strict, 2 POCS iterations + Carry ----
			{
				cfg.residualMode = ResidualMode::Strict;
				cpuEng.SetConfig(cfg);
				gpuGuard.engine.SetConfig(cfg);

				ComplexSceneBundle f3 = BuildComplexFrame(3, camDx, camDy, /*stampSpecialRegions=*/true);
				FrameProcessInput in3 = MakeFrameInput(f3, 3, /*withPeaks=*/true, /*transformIdentified8x8=*/true);
				const FrameProcessResult cpu3 = cpuEng.ProcessFrame(in3, &cpuSi);
				const GpuProcessResult gpu3 = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in3, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpu3, gpu3, f3.curClean, f3.receivedY, f3.active, "Frame3-8x8StrictPocs2");
				Assert::IsTrue(gpu3.carryUsed, L"Frame 3 STRICT must use carry-forward");
				Assert::AreEqual(0.0, MaxAbsPlane(gpu3.tdDelta, ActiveRect{ 0, 16, 16, 32 }), L"Frame 3 STRICT Overlay delta must be 0");
			}

			// ---- Side-by-side non-vacuity check: STRICT vs CELL produce genuinely distinct GPU outputs on identical input ----
			{
				Config cfgCell = MakePreset(Preset::Reference, Mode::On);
				cfgCell.enableLongAnchor = false;
				cfgCell.residualMode = ResidualMode::Cell;
				cfgCell.cellProjIterations = 2;

				Config cfgStrict = cfgCell;
				cfgStrict.residualMode = ResidualMode::Strict;

				GpuEngineGuard gCell(ctx.gpu, cfgCell);
				GpuEngineGuard gStrict(ctx.gpu, cfgStrict);
				FrameProcessInput inSame = MakeFrameInput(f2, 20, /*withPeaks=*/true, /*transformIdentified8x8=*/true);

				const GpuProcessResult rCell = gCell.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inSame, nullptr, nullptr);
				const GpuProcessResult rStrict = gStrict.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inSame, nullptr, nullptr);
				Assert::IsTrue(rCell.succeeded && rStrict.succeeded, L"Side-by-side CELL/STRICT failed");
				const double modeDiff = MaxAbsDiffRect(rCell.outputY, rStrict.outputY, f2.active);
				Assert::IsTrue(modeDiff > 0.1,
					FormatMsg("STRICT vs CELL GPU diff=%.6f <= 0.1 (mode switch vacuous)", modeDiff).c_str());
			}

			// ---- Frame 4: Scene Cut / Span Invalid -> Carry Reset ----
			{
				ComplexSceneBundle f4 = BuildComplexFrame(4, camDx, camDy, /*stampSpecialRegions=*/true);
				FrameProcessInput in4 = MakeFrameInput(f4, 4, /*withPeaks=*/true, /*transformIdentified8x8=*/false, /*isCut=*/true);
				const FrameProcessResult cpu4 = cpuEng.ProcessFrame(in4, &cpuSi);
				const GpuProcessResult gpu4 = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in4, nullptr, &gpuSi);

				Assert::IsTrue(gpu4.succeeded, L"Frame 4 cut frame failed on GPU");
				Assert::IsTrue(cpu4.carryReset && gpu4.carryReset, L"Frame 4 cut must set carryReset == true");
				Assert::IsFalse(cpu4.carryUsed || gpu4.carryUsed, L"Frame 4 cut must NOT use carry");
				Assert::AreEqual(static_cast<size_t>(0), gpu4.activeRefCount, L"Frame 4 cut must fuse 0 refs");
				Assert::AreEqual(0.0, MaxAbsDiffFull(gpu4.outputY, f4.receivedY), L"Frame 4 cut outputY must be bit-exact receivedY");
				Assert::AreEqual(0.0, MaxAbsDiffFull(gpu4.outputClean, f4.curClean), L"Frame 4 cut outputClean must equal curClean");
			}
		}

		// =========================================================================
		// 2. GPU Chroma Delta Apply vs CPU TD-S4 across 4:2:0->4:2:2 (Left/Type2)
		//    and 4:2:0->RGB 4:4:4 (BT.709/BT.2020) preserving BTB/WTW & zero-delta
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuChromaDeltaApplyMatchesCpuTdS4OnYuvAndRgbPreservingBtbWtw)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuChromaDeltaApplyMatchesCpuTdS4OnYuvAndRgbPreservingBtbWtw)
		{
			WarpGpuContext ctx;
			GpuEngineGuard gpuGuard(ctx.gpu, MakePreset(Preset::Reference, Mode::On));

			const int lumaW = 64;
			const int lumaH = 64;
			const ActiveRect active{ 0, 4, 64, 60 }; // Letterbox bars at y < 4 and y >= 60

			// Coded 4:2:0 deltas (32 x 32):
			// - Left half (cx in [0, 12)) inside active rect has sharp vertical step edge at cy = 15 + smooth variation
			// - Right half (cx in [16, 32)) is strictly 0.0 so delivered dx >= 36 (RGB) / dx >= 18 (4:2:2) is exact zero-delta!
			const int cW = 32;
			const int cH = 32;
			TdV2Plane codedDeltaCb(cW, cH, 0.0);
			TdV2Plane codedDeltaCr(cW, cH, 0.0);
			for (int cy = 2; cy < 30; ++cy)
			{
				for (int cx = 0; cx < 12; ++cx)
				{
					const double stepCb = (cy < 15) ? -14.0 : +16.0;
					const double stepCr = (cy < 15) ? +12.0 : -15.0;
					codedDeltaCb.at(cx, cy) = stepCb + 3.5 * std::sin(2.0 * kPi * cx / 11.0);
					codedDeltaCr.at(cx, cy) = stepCr + 3.0 * std::cos(2.0 * kPi * cx / 9.0);
				}
			}

			// ---- Part A: Coded 4:2:0 -> Delivered 4:2:2 (Left AND TopLeftType2 siting) ----
			const int dW422 = 32;
			const int dH422 = 64;
			TdV2Plane delivCb422(dW422, dH422, 512.0);
			TdV2Plane delivCr422(dW422, dH422, 512.0);
			for (int y = 0; y < dH422; ++y)
			{
				for (int x = 0; x < dW422; ++x)
				{
					// FP32-exact delivered video samples so RG32F upload round-trips bit-exact on zero-delta passthrough.
					delivCb422.at(x, y) = static_cast<double>(static_cast<float>(
						480.0 + 40.0 * std::sin(2.0 * kPi * x / 19.0) + 15.0 * std::cos(2.0 * kPi * y / 23.0)));
					delivCr422.at(x, y) = static_cast<double>(static_cast<float>(
						530.0 - 35.0 * std::cos(2.0 * kPi * x / 17.0) + 18.0 * std::sin(2.0 * kPi * y / 29.0)));
				}
			}

			TdV2Plane cpuDeltaCbLeft422, cpuDeltaCbType2422;
			for (const SitingSpec& siting : { SitingSpec::Left(), SitingSpec::TopLeftType2() })
			{
				ChromaGpuApplyInput gIn;
				gIn.lumaW = lumaW;
				gIn.lumaH = lumaH;
				gIn.deliveredFormat = DeliveredChromaFormat::Yuv422;
				gIn.codedSubsampling = ChromaSubsampling::Yuv420;
				gIn.siting = siting;
				gIn.activeRect = active;
				gIn.deliveredCb = delivCb422;
				gIn.deliveredCr = delivCr422;
				gIn.codedDeltaCb = codedDeltaCb;
				gIn.codedDeltaCr = codedDeltaCr;

				StandInRegistry ch422Si;
				const ChromaGpuApplyResult gRes = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, gIn, &ch422Si);
				Assert::IsTrue(gRes.succeeded, L"ApplyChromaDeltaGpu YUV 4:2:2 failed");
				Assert::AreEqual(1, gRes.dispatchCount, L"ApplyChromaDeltaGpu YUV must issue exactly 1 dispatch");
				Assert::AreEqual(static_cast<size_t>(0u), gRes.nonFinitePixels, L"Clean 4:2:2 apply must have 0 nonFinitePixels");
				Assert::IsTrue(gRes.borderBilinearPixels > 0u, L"4:2:0->4:2:2 active border must report borderBilinearPixels > 0");
				Assert::IsTrue(ch422Si.IsActive(StandInKind::BorderBilinearFallback),
					L"4:2:0->4:2:2 active border must engage StandInKind::BorderBilinearFallback");

				const TdV2Plane cpuDeltaCb = UpsampleDeltaToDelivered(
					codedDeltaCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, active);
				const TdV2Plane cpuDeltaCr = UpsampleDeltaToDelivered(
					codedDeltaCr, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, active);
				TdV2Plane cpuOutCb = delivCb422;
				TdV2Plane cpuOutCr = delivCr422;
				for (size_t i = 0; i < cpuOutCb.v.size(); ++i)
				{
					if (cpuDeltaCb.v[i] != 0.0) cpuOutCb.v[i] += cpuDeltaCb.v[i];
					if (cpuDeltaCr.v[i] != 0.0) cpuOutCr.v[i] += cpuDeltaCr.v[i];
				}

				if (siting.shiftY == 0.0f) cpuDeltaCbLeft422 = cpuDeltaCb;
				else cpuDeltaCbType2422 = cpuDeltaCb;

				const double dCbErr = MaxAbsDiffFull(gRes.deliveredDeltaCb, cpuDeltaCb);
				const double dCrErr = MaxAbsDiffFull(gRes.deliveredDeltaCr, cpuDeltaCr);
				const double outCbErr = MaxAbsDiffFull(gRes.outCb, cpuOutCb);
				const double outCrErr = MaxAbsDiffFull(gRes.outCr, cpuOutCr);

				Assert::IsTrue(dCbErr <= kGpuCpuParityTol, FormatMsg("4:2:2 deltaCb |GPU-CPU|=%.6f", dCbErr).c_str());
				Assert::IsTrue(dCrErr <= kGpuCpuParityTol, FormatMsg("4:2:2 deltaCr |GPU-CPU|=%.6f", dCrErr).c_str());
				Assert::IsTrue(outCbErr <= kGpuCpuParityTol, FormatMsg("4:2:2 outCb |GPU-CPU|=%.6f", outCbErr).c_str());
				Assert::IsTrue(outCrErr <= kGpuCpuParityTol, FormatMsg("4:2:2 outCr |GPU-CPU|=%.6f", outCrErr).c_str());

				// Bit-exact passthrough on zero-delta active right region (x >= 18) and letterbox bars!
				const ActiveRect zeroRegion422{ 18, 4, dW422, 60 };
				Assert::AreEqual(0.0, MaxAbsPlane(gRes.deliveredDeltaCb, zeroRegion422), L"4:2:2 zero-delta region non-zero Cb");
				Assert::AreEqual(0.0, MaxAbsPlane(gRes.deliveredDeltaCr, zeroRegion422), L"4:2:2 zero-delta region non-zero Cr");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outCb, delivCb422, zeroRegion422), L"4:2:2 zero-delta outCb not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outCr, delivCr422, zeroRegion422), L"4:2:2 zero-delta outCr not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outCb, delivCb422, ActiveRect{ 0, 0, dW422, 4 }), L"4:2:2 top bar corrupted");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outCb, delivCb422, ActiveRect{ 0, 60, dW422, 64 }), L"4:2:2 bottom bar corrupted");
			}

			// ---- NEGATIVE CONTROL (Siting): Wrong-siting (Left vs TopLeftType2) creates > 5.0 code color fringing ----
			const double wrongSitingFringing = MaxAbsDiffRect(cpuDeltaCbLeft422, cpuDeltaCbType2422, ActiveRect{ 0, 4, 12, 60 });
			Assert::IsTrue(wrongSitingFringing > 5.0,
				FormatMsg("Negative control failed: wrong siting diff=%.4f <= 5.0 codes", wrongSitingFringing).c_str());

			// ---- Part B: Coded 4:2:0 -> Delivered RGB 4:4:4 (BT.709 & BT.2020) with explicit BTB (<64) and WTW (>940) ----
			TdV2Plane delivR(lumaW, lumaH, 500.0);
			TdV2Plane delivG(lumaW, lumaH, 500.0);
			TdV2Plane delivB(lumaW, lumaH, 500.0);
			for (int y = 0; y < lumaH; ++y)
			{
				for (int x = 0; x < lumaW; ++x)
				{
					if (y >= 6 && y < 20)
					{
						// Explicit BTB (< 64) across BOTH active-delta left half and zero-delta right half!
						delivR.at(x, y) = 24.0;
						delivG.at(x, y) = 30.0;
						delivB.at(x, y) = 36.0;
					}
					else if (y >= 40 && y < 56)
					{
						// Explicit WTW (> 940) across BOTH active-delta left half and zero-delta right half!
						delivR.at(x, y) = 982.0;
						delivG.at(x, y) = 994.0;
						delivB.at(x, y) = 1004.0;
					}
					else
					{
						delivR.at(x, y) = static_cast<double>(static_cast<float>(420.0 + 30.0 * std::sin(2.0 * kPi * x / 31.0)));
						delivG.at(x, y) = static_cast<double>(static_cast<float>(460.0 + 25.0 * std::cos(2.0 * kPi * y / 29.0)));
						delivB.at(x, y) = static_cast<double>(static_cast<float>(490.0 - 20.0 * std::sin(2.0 * kPi * (x + y) / 37.0)));
					}
				}
			}

			struct RgbCase { YCbCrMatrixSpec mat; SitingSpec siting; const char* name; };
			const RgbCase rgbCases[2] = {
				{ YCbCrMatrixSpec::Bt709(),  SitingSpec::Left(),         "BT.709-Left" },
				{ YCbCrMatrixSpec::Bt2020(), SitingSpec::TopLeftType2(), "BT.2020-Type2" }
			};

			for (const RgbCase& rc : rgbCases)
			{
				ChromaGpuApplyInput gIn;
				gIn.lumaW = lumaW;
				gIn.lumaH = lumaH;
				gIn.deliveredFormat = DeliveredChromaFormat::Rgb444;
				gIn.codedSubsampling = ChromaSubsampling::Yuv420;
				gIn.siting = rc.siting;
				gIn.activeRect = active;
				gIn.matrix = rc.mat;
				gIn.deliveredR = delivR;
				gIn.deliveredG = delivG;
				gIn.deliveredB = delivB;
				gIn.codedDeltaCb = codedDeltaCb;
				gIn.codedDeltaCr = codedDeltaCr;

				StandInRegistry chRgbSi;
				const ChromaGpuApplyResult gRes = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, gIn, &chRgbSi);
				Assert::IsTrue(gRes.succeeded, L"ApplyChromaDeltaGpu RGB 4:4:4 failed");
				Assert::AreEqual(1, gRes.dispatchCount, L"ApplyChromaDeltaGpu RGB must issue exactly 1 dispatch");
				Assert::AreEqual(static_cast<size_t>(0u), gRes.nonFinitePixels, L"Clean RGB apply must have 0 nonFinitePixels");
				Assert::IsTrue(gRes.borderBilinearPixels > 0u, L"4:2:0->RGB 4:4:4 active border must report borderBilinearPixels > 0");
				Assert::IsTrue(chRgbSi.IsActive(StandInKind::BorderBilinearFallback),
					L"4:2:0->RGB 4:4:4 active border must engage StandInKind::BorderBilinearFallback");

				const TdV2Plane cpuDeltaCb = UpsampleDeltaToDelivered(
					codedDeltaCb, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, rc.siting, active);
				const TdV2Plane cpuDeltaCr = UpsampleDeltaToDelivered(
					codedDeltaCr, lumaW, lumaH, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, rc.siting, active);

				TdV2Plane cpuOutR = delivR;
				TdV2Plane cpuOutG = delivG;
				TdV2Plane cpuOutB = delivB;
				for (int y = active.y0; y < active.y1; ++y)
				{
					for (int x = active.x0; x < active.x1; ++x)
					{
						Assert::IsTrue(rc.mat.ApplySignedChromaDeltaToRgb(
							delivR.at(x, y), delivG.at(x, y), delivB.at(x, y),
							cpuDeltaCb.at(x, y), cpuDeltaCr.at(x, y),
							cpuOutR.at(x, y), cpuOutG.at(x, y), cpuOutB.at(x, y), nullptr),
							L"CPU ApplySignedChromaDeltaToRgb failed unexpectedly");
					}
				}

				const double errR = MaxAbsDiffFull(gRes.outR, cpuOutR);
				const double errG = MaxAbsDiffFull(gRes.outG, cpuOutG);
				const double errB = MaxAbsDiffFull(gRes.outB, cpuOutB);
				const double errDcb = MaxAbsDiffFull(gRes.deliveredDeltaCb, cpuDeltaCb);
				const double errDcr = MaxAbsDiffFull(gRes.deliveredDeltaCr, cpuDeltaCr);

				Assert::IsTrue(errR <= kGpuCpuParityTol, FormatTagMsg(rc.name, "outR |GPU-CPU|=%.6f", errR).c_str());
				Assert::IsTrue(errG <= kGpuCpuParityTol, FormatTagMsg(rc.name, "outG |GPU-CPU|=%.6f", errG).c_str());
				Assert::IsTrue(errB <= kGpuCpuParityTol, FormatTagMsg(rc.name, "outB |GPU-CPU|=%.6f", errB).c_str());
				Assert::IsTrue(errDcb <= kGpuCpuParityTol, FormatTagMsg(rc.name, "delivDeltaCb |GPU-CPU|=%.6f", errDcb).c_str());
				Assert::IsTrue(errDcr <= kGpuCpuParityTol, FormatTagMsg(rc.name, "delivDeltaCr |GPU-CPU|=%.6f", errDcr).c_str());

				// Bit-exact passthrough on zero-delta active right region (x >= 36), including BTB & WTW pixels!
				const ActiveRect zeroRgbRect{ 36, 4, lumaW, 60 };
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outR, delivR, zeroRgbRect), L"RGB zero-delta outR not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outG, delivG, zeroRgbRect), L"RGB zero-delta outG not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outB, delivB, zeroRgbRect), L"RGB zero-delta outB not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outR, delivR, ActiveRect{ 0, 0, lumaW, 4 }), L"RGB top bar not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gRes.outR, delivR, ActiveRect{ 0, 60, lumaW, 64 }), L"RGB bottom bar not bit-exact");

				// Verify unclipped BTB (< 64) and WTW (> 940) are genuinely preserved on modified active-delta pixels too!
				Assert::IsTrue(gRes.outB.at(8, 10) < 55.0, L"BTB pixel was unexpectedly clipped on GPU");
				Assert::IsTrue(gRes.outB.at(8, 48) > 955.0, L"WTW pixel was unexpectedly clipped on GPU");

				// ---- NEGATIVE CONTROL (BTB/WTW): Naive [64, 940] RGB studio-range clamping corrupts BTB/WTW by > 25 codes! ----
				double maxClampCorrupt = 0.0;
				for (int y = active.y0; y < active.y1; ++y)
				{
					for (int x = active.x0; x < active.x1; ++x)
					{
						const double cR = (std::max)(64.0, (std::min)(940.0, cpuOutR.at(x, y)));
						const double cG = (std::max)(64.0, (std::min)(940.0, cpuOutG.at(x, y)));
						const double cB = (std::max)(64.0, (std::min)(940.0, cpuOutB.at(x, y)));
						maxClampCorrupt = (std::max)(maxClampCorrupt, std::fabs(cR - cpuOutR.at(x, y)));
						maxClampCorrupt = (std::max)(maxClampCorrupt, std::fabs(cG - cpuOutG.at(x, y)));
						maxClampCorrupt = (std::max)(maxClampCorrupt, std::fabs(cB - cpuOutB.at(x, y)));
					}
				}
				Assert::IsTrue(maxClampCorrupt > 25.0,
					FormatMsg("Negative control failed: studio clamp corruption=%.2f <= 25.0 codes", maxClampCorrupt).c_str());
			}

			// ---- Part C (M2): TD_IDENTITY_GRID == 1 Branch Coverage (4:2:0->4:2:0 YUV & 4:4:4->RGB 4:4:4) ----
			{
				// C1: 4:2:0 -> 4:2:0 YUV identity grid
				TdV2Plane delivCb420(cW, cH, 512.0);
				TdV2Plane delivCr420(cW, cH, 512.0);
				for (int y = 0; y < cH; ++y)
				{
					for (int x = 0; x < cW; ++x)
					{
						delivCb420.at(x, y) = static_cast<double>(static_cast<float>(500.0 + 30.0 * std::sin(2.0 * kPi * x / 13.0)));
						delivCr420.at(x, y) = static_cast<double>(static_cast<float>(520.0 - 25.0 * std::cos(2.0 * kPi * y / 11.0)));
					}
				}
				ChromaGpuApplyInput idYuvIn;
				idYuvIn.lumaW = lumaW;
				idYuvIn.lumaH = lumaH;
				idYuvIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
				idYuvIn.codedSubsampling = ChromaSubsampling::Yuv420;
				idYuvIn.siting = SitingSpec::Left();
				idYuvIn.activeRect = active;
				idYuvIn.deliveredCb = delivCb420;
				idYuvIn.deliveredCr = delivCr420;
				idYuvIn.codedDeltaCb = codedDeltaCb;
				idYuvIn.codedDeltaCr = codedDeltaCr;

				StandInRegistry idYuvSi;
				const ChromaGpuApplyResult idYuvRes = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, idYuvIn, &idYuvSi);
				Assert::IsTrue(idYuvRes.succeeded, L"Identity 4:2:0->4:2:0 GPU apply failed");
				Assert::AreEqual(1, idYuvRes.dispatchCount, L"Identity 4:2:0 must issue 1 dispatch");
				Assert::AreEqual(static_cast<size_t>(0u), idYuvRes.borderBilinearPixels, L"Identity grid must have 0 borderBilinearPixels");
				Assert::IsFalse(idYuvSi.IsActive(StandInKind::BorderBilinearFallback), L"Identity grid must NOT engage BorderBilinearFallback");

				TdV2Plane cpuOutCb420 = delivCb420;
				TdV2Plane cpuOutCr420 = delivCr420;
				for (int cy = 2; cy < 30; ++cy)
				{
					for (int cx = 0; cx < cW; ++cx)
					{
						if (codedDeltaCb.at(cx, cy) != 0.0) cpuOutCb420.at(cx, cy) += codedDeltaCb.at(cx, cy);
						if (codedDeltaCr.at(cx, cy) != 0.0) cpuOutCr420.at(cx, cy) += codedDeltaCr.at(cx, cy);
					}
				}
				Assert::IsTrue(MaxAbsDiffFull(idYuvRes.outCb, cpuOutCb420) <= kGpuCpuParityTol, L"Identity 4:2:0 outCb |GPU-CPU| > 1e-3");
				Assert::IsTrue(MaxAbsDiffFull(idYuvRes.outCr, cpuOutCr420) <= kGpuCpuParityTol, L"Identity 4:2:0 outCr |GPU-CPU| > 1e-3");
				Assert::AreEqual(0.0, MaxAbsDiffRect(idYuvRes.outCb, delivCb420, ActiveRect{ 16, 2, cW, 30 }), L"Identity 4:2:0 zero-delta not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(idYuvRes.outCb, delivCb420, ActiveRect{ 0, 0, cW, 2 }), L"Identity 4:2:0 top bar corrupted");
				Assert::AreEqual(0.0, MaxAbsDiffRect(idYuvRes.outCb, delivCb420, ActiveRect{ 0, 30, cW, 32 }), L"Identity 4:2:0 bottom bar corrupted");
				Assert::IsTrue(MaxAbsDiffRect(idYuvRes.outCb, delivCb420, ActiveRect{ 0, 2, 12, 30 }) > 5.0, L"Identity 4:2:0 active delta vacuous");

				// C2: 4:4:4 -> RGB 4:4:4 identity grid with BTB/WTW
				TdV2Plane codedDeltaCb444(lumaW, lumaH, 0.0);
				TdV2Plane codedDeltaCr444(lumaW, lumaH, 0.0);
				for (int y = active.y0; y < active.y1; ++y)
				{
					for (int x = 0; x < 24; ++x)
					{
						codedDeltaCb444.at(x, y) = (y < 32) ? -10.0 : +12.0;
						codedDeltaCr444.at(x, y) = (y < 32) ? +9.0 : -11.0;
					}
				}
				ChromaGpuApplyInput idRgbIn;
				idRgbIn.lumaW = lumaW;
				idRgbIn.lumaH = lumaH;
				idRgbIn.deliveredFormat = DeliveredChromaFormat::Rgb444;
				idRgbIn.codedSubsampling = ChromaSubsampling::Yuv444;
				idRgbIn.siting = SitingSpec::Center();
				idRgbIn.activeRect = active;
				idRgbIn.matrix = YCbCrMatrixSpec::Bt709();
				idRgbIn.deliveredR = delivR;
				idRgbIn.deliveredG = delivG;
				idRgbIn.deliveredB = delivB;
				idRgbIn.codedDeltaCb = codedDeltaCb444;
				idRgbIn.codedDeltaCr = codedDeltaCr444;

				StandInRegistry idRgbSi;
				const ChromaGpuApplyResult idRgbRes = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, idRgbIn, &idRgbSi);
				Assert::IsTrue(idRgbRes.succeeded, L"Identity 4:4:4->RGB GPU apply failed");
				Assert::AreEqual(1, idRgbRes.dispatchCount, L"Identity RGB must issue 1 dispatch");
				Assert::AreEqual(static_cast<size_t>(0u), idRgbRes.borderBilinearPixels, L"Identity RGB must have 0 borderBilinearPixels");
				Assert::IsFalse(idRgbSi.IsActive(StandInKind::BorderBilinearFallback), L"Identity RGB must NOT engage BorderBilinearFallback");

				TdV2Plane cpuIdR = delivR, cpuIdG = delivG, cpuIdB = delivB;
				for (int y = active.y0; y < active.y1; ++y)
				{
					for (int x = active.x0; x < active.x1; ++x)
					{
						Assert::IsTrue(idRgbIn.matrix.ApplySignedChromaDeltaToRgb(
							delivR.at(x, y), delivG.at(x, y), delivB.at(x, y),
							codedDeltaCb444.at(x, y), codedDeltaCr444.at(x, y),
							cpuIdR.at(x, y), cpuIdG.at(x, y), cpuIdB.at(x, y), nullptr));
					}
				}
				Assert::IsTrue(MaxAbsDiffFull(idRgbRes.outR, cpuIdR) <= kGpuCpuParityTol, L"Identity RGB outR |GPU-CPU| > 1e-3");
				Assert::IsTrue(MaxAbsDiffFull(idRgbRes.outG, cpuIdG) <= kGpuCpuParityTol, L"Identity RGB outG |GPU-CPU| > 1e-3");
				Assert::IsTrue(MaxAbsDiffFull(idRgbRes.outB, cpuIdB) <= kGpuCpuParityTol, L"Identity RGB outB |GPU-CPU| > 1e-3");
				Assert::AreEqual(0.0, MaxAbsDiffRect(idRgbRes.outR, delivR, ActiveRect{ 28, 4, lumaW, 60 }), L"Identity RGB zero-delta not bit-exact");
				Assert::AreEqual(0.0, MaxAbsDiffRect(idRgbRes.outR, delivR, ActiveRect{ 0, 0, lumaW, 4 }), L"Identity RGB top bar corrupted");
				Assert::IsTrue(idRgbRes.outB.at(8, 10) < 55.0 && idRgbRes.outB.at(8, 48) > 955.0, L"Identity RGB must preserve BTB/WTW");
			}

			// ---- Part D (H2): ApplyChromaDeltaGpu NaN/Inf, FP32 Overflow, Invalid Siting & Degenerate Matrix ----
			{
				const double kNaN = std::numeric_limits<double>::quiet_NaN();
				const double kPInf = std::numeric_limits<double>::infinity();

				// D1: NaN/+Inf in codedDeltaCb/Cr on BOTH YUV 4:2:0->4:2:2 AND RGB 4:2:0->4:4:4
				{
					TdV2Plane badCb = codedDeltaCb;
					TdV2Plane badCr = codedDeltaCr;
					badCb.at(4, 10) = kNaN;
					badCr.at(6, 12) = kPInf;

					ChromaGpuApplyInput yuvBad;
					yuvBad.lumaW = lumaW;
					yuvBad.lumaH = lumaH;
					yuvBad.deliveredFormat = DeliveredChromaFormat::Yuv422;
					yuvBad.codedSubsampling = ChromaSubsampling::Yuv420;
					yuvBad.siting = SitingSpec::Left();
					yuvBad.activeRect = active;
					yuvBad.deliveredCb = delivCb422;
					yuvBad.deliveredCr = delivCr422;
					yuvBad.codedDeltaCb = badCb;
					yuvBad.codedDeltaCr = badCr;

					StandInRegistry siY;
					const ChromaGpuApplyResult rY = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, yuvBad, &siY);
					Assert::IsTrue(rY.succeeded, L"H2 YUV codedDelta NaN/Inf must complete cleanly");
					AssertAllFinitePlane(rY.outCb, L"H2 YUV outCb must be 100% finite");
					AssertAllFinitePlane(rY.outCr, L"H2 YUV outCr must be 100% finite");
					AssertAllFinitePlane(rY.deliveredDeltaCb, L"H2 YUV deliveredDeltaCb must be 100% finite");
					AssertAllFinitePlane(rY.deliveredDeltaCr, L"H2 YUV deliveredDeltaCr must be 100% finite");
					Assert::IsTrue(rY.nonFinitePixels > 0u, L"H2 YUV codedDelta NaN/Inf must report nonFinitePixels > 0");
					Assert::IsTrue(siY.IsActive(StandInKind::NonFiniteClosure), L"H2 YUV codedDelta NaN/Inf must engage NonFiniteClosure");

					ChromaGpuApplyInput rgbBad;
					rgbBad.lumaW = lumaW;
					rgbBad.lumaH = lumaH;
					rgbBad.deliveredFormat = DeliveredChromaFormat::Rgb444;
					rgbBad.codedSubsampling = ChromaSubsampling::Yuv420;
					rgbBad.siting = SitingSpec::Left();
					rgbBad.activeRect = active;
					rgbBad.matrix = YCbCrMatrixSpec::Bt709();
					rgbBad.deliveredR = delivR;
					rgbBad.deliveredG = delivG;
					rgbBad.deliveredB = delivB;
					rgbBad.codedDeltaCb = badCb;
					rgbBad.codedDeltaCr = badCr;

					StandInRegistry siR;
					const ChromaGpuApplyResult rR = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbBad, &siR);
					Assert::IsTrue(rR.succeeded, L"H2 RGB codedDelta NaN/Inf must complete cleanly");
					AssertAllFinitePlane(rR.outR, L"H2 RGB outR must be 100% finite");
					AssertAllFinitePlane(rR.outG, L"H2 RGB outG must be 100% finite");
					AssertAllFinitePlane(rR.outB, L"H2 RGB outB must be 100% finite");
					Assert::IsTrue(rR.nonFinitePixels > 0u, L"H2 RGB codedDelta NaN/Inf must report nonFinitePixels > 0");
					Assert::IsTrue(siR.IsActive(StandInKind::NonFiniteClosure), L"H2 RGB codedDelta NaN/Inf must engage NonFiniteClosure");
				}

				// D2: RGB FP32 overflow (r = 3.4e38, large +dCr -> kR * dCr + r overflows FP32 max to +Inf)
				{
					const float kNearMaxF = 3.4e38f;
					const float kBigDeltaF = 1.0e38f;
					// Negative control: verify un-guarded FP32 overflows to +Inf!
					const float kR709 = static_cast<float>(2.0 * (1.0 - YCbCrMatrixSpec::Bt709().kr));
					volatile float vOverflow = kNearMaxF + kR709 * kBigDeltaF;
					Assert::IsFalse(std::isfinite(static_cast<float>(vOverflow)),
						L"Negative control: un-guarded FP32 RGB add must overflow to +Inf");

					TdV2Plane ovfR = delivR;
					TdV2Plane ovfG = delivG;
					TdV2Plane ovfB = delivB;
					ovfR.at(10, 12) = static_cast<double>(kNearMaxF);
					TdV2Plane ovfDcb(lumaW, lumaH, 0.0);
					TdV2Plane ovfDcr(lumaW, lumaH, 0.0);
					ovfDcr.at(10, 12) = static_cast<double>(kBigDeltaF);

					ChromaGpuApplyInput rgbOvf;
					rgbOvf.lumaW = lumaW;
					rgbOvf.lumaH = lumaH;
					rgbOvf.deliveredFormat = DeliveredChromaFormat::Rgb444;
					rgbOvf.codedSubsampling = ChromaSubsampling::Yuv444;
					rgbOvf.siting = SitingSpec::Center();
					rgbOvf.activeRect = active;
					rgbOvf.matrix = YCbCrMatrixSpec::Bt709();
					rgbOvf.deliveredR = ovfR;
					rgbOvf.deliveredG = ovfG;
					rgbOvf.deliveredB = ovfB;
					rgbOvf.codedDeltaCb = ovfDcb;
					rgbOvf.codedDeltaCr = ovfDcr;

					StandInRegistry siOvf;
					const ChromaGpuApplyResult rOvf = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbOvf, &siOvf);
					Assert::IsTrue(rOvf.succeeded, L"H2 RGB FP32 overflow must complete cleanly");
					AssertAllFinitePlane(rOvf.outR, L"H2 RGB overflow outR must be 100% finite");
					AssertAllFinitePlane(rOvf.outG, L"H2 RGB overflow outG must be 100% finite");
					AssertAllFinitePlane(rOvf.outB, L"H2 RGB overflow outB must be 100% finite");
					Assert::AreEqual(static_cast<double>(kNearMaxF), rOvf.outR.at(10, 12),
						L"H2 RGB overflow must restore original finite R");
					Assert::AreEqual(ovfG.at(10, 12), rOvf.outG.at(10, 12),
						L"H2 RGB overflow must restore original finite G");
					Assert::AreEqual(ovfB.at(10, 12), rOvf.outB.at(10, 12),
						L"H2 RGB overflow must restore original finite B");
					Assert::AreEqual(0.0, rOvf.deliveredDeltaCr.at(10, 12), L"H2 RGB overflow must zero deltaCr");
					Assert::IsTrue(rOvf.nonFinitePixels > 0u, L"H2 RGB overflow must report nonFinitePixels > 0");
					Assert::IsTrue(siOvf.IsActive(StandInKind::NonFiniteClosure), L"H2 RGB overflow must engage NonFiniteClosure");
				}

				// D3: YUV FP32 overflow AND YUV non-finite deliveredCb
				{
					const float kNearMaxF = 3.4e38f;
					const float kBigDeltaF = 1.0e38f;
					TdV2Plane ovfCb(cW, cH, 512.0);
					TdV2Plane ovfCr(cW, cH, 512.0);
					ovfCb.at(6, 10) = static_cast<double>(kNearMaxF);
					TdV2Plane ovfDcb(cW, cH, 0.0);
					TdV2Plane ovfDcr(cW, cH, 0.0);
					ovfDcb.at(6, 10) = static_cast<double>(kBigDeltaF);

					ChromaGpuApplyInput yuvOvf;
					yuvOvf.lumaW = lumaW;
					yuvOvf.lumaH = lumaH;
					yuvOvf.deliveredFormat = DeliveredChromaFormat::Yuv420;
					yuvOvf.codedSubsampling = ChromaSubsampling::Yuv420;
					yuvOvf.siting = SitingSpec::Left();
					yuvOvf.activeRect = active;
					yuvOvf.deliveredCb = ovfCb;
					yuvOvf.deliveredCr = ovfCr;
					yuvOvf.codedDeltaCb = ovfDcb;
					yuvOvf.codedDeltaCr = ovfDcr;

					StandInRegistry siYOvf;
					const ChromaGpuApplyResult rYOvf = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, yuvOvf, &siYOvf);
					Assert::IsTrue(rYOvf.succeeded, L"H2 YUV FP32 overflow must complete cleanly");
					AssertAllFinitePlane(rYOvf.outCb, L"H2 YUV overflow outCb must be 100% finite");
					AssertAllFinitePlane(rYOvf.outCr, L"H2 YUV overflow outCr must be 100% finite");
					Assert::AreEqual(static_cast<double>(kNearMaxF), rYOvf.outCb.at(6, 10),
						L"H2 YUV overflow must restore finite deliveredCb");
					Assert::IsTrue(rYOvf.nonFinitePixels > 0u, L"H2 YUV overflow must report nonFinitePixels > 0");
					Assert::IsTrue(siYOvf.IsActive(StandInKind::NonFiniteClosure), L"H2 YUV overflow must engage NonFiniteClosure");

					// Non-finite deliveredCb (NaN) -> closed cleanly without propagating NaN/Inf!
					TdV2Plane nanDelivCb(cW, cH, 512.0);
					nanDelivCb.at(8, 12) = kNaN;
					ChromaGpuApplyInput yuvNanDeliv = yuvOvf;
					yuvNanDeliv.deliveredCb = nanDelivCb;
					yuvNanDeliv.codedDeltaCb = codedDeltaCb;
					yuvNanDeliv.codedDeltaCr = codedDeltaCr;
					StandInRegistry siYNan;
					const ChromaGpuApplyResult rYNan = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, yuvNanDeliv, &siYNan);
					Assert::IsTrue(rYNan.succeeded, L"H2 YUV non-finite deliveredCb must complete cleanly");
					AssertAllFinitePlane(rYNan.outCb, L"H2 YUV non-finite deliveredCb must not propagate NaN into outCb");
					AssertAllFinitePlane(rYNan.outCr, L"H2 YUV non-finite deliveredCb must not propagate NaN into outCr");
					Assert::AreEqual(0.0, rYNan.outCb.at(8, 12), L"H2 YUV non-finite deliveredCb pixel must close to 0.0");
					Assert::AreEqual(0.0, rYNan.deliveredDeltaCb.at(8, 12), L"H2 YUV non-finite deliveredCb delta must be 0.0");
					Assert::IsTrue(rYNan.nonFinitePixels > 0u, L"H2 YUV non-finite deliveredCb must count nonFinitePixels > 0");
					Assert::IsTrue(siYNan.IsActive(StandInKind::NonFiniteClosure),
						L"H2 YUV non-finite deliveredCb must engage NonFiniteClosure");
				}

				// D4: Non-finite siting.shiftX / shiftY -> fails closed visibly with NonFiniteClosure!
				{
					ChromaGpuApplyInput badSiting;
					badSiting.lumaW = lumaW;
					badSiting.lumaH = lumaH;
					badSiting.deliveredFormat = DeliveredChromaFormat::Yuv422;
					badSiting.codedSubsampling = ChromaSubsampling::Yuv420;
					badSiting.siting = { static_cast<float>(kNaN), 0.0f };
					badSiting.activeRect = active;
					badSiting.deliveredCb = delivCb422;
					badSiting.deliveredCr = delivCr422;
					badSiting.codedDeltaCb = codedDeltaCb;
					badSiting.codedDeltaCr = codedDeltaCr;

					StandInRegistry siSit;
					const uint64_t failsBefore = gpuGuard.engine.Failures();
					const ChromaGpuApplyResult rSit = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, badSiting, &siSit);
					Assert::IsFalse(rSit.succeeded, L"H2 non-finite siting.shiftX must fail closed");
					Assert::IsFalse(rSit.lastError.empty(), L"H2 non-finite siting.shiftX must set lastError");
					Assert::AreEqual(failsBefore + 1u, gpuGuard.engine.Failures(), L"H2 invalid siting must increment Failures()");
					Assert::IsTrue(siSit.IsActive(StandInKind::NonFiniteClosure), L"H2 invalid siting must engage NonFiniteClosure");
				}

				// D5: Degenerate matrix.kg <= 0 -> fails closed visibly with NonFiniteClosure!
				for (double badKg : { 0.0, -0.25 })
				{
					ChromaGpuApplyInput badMat;
					badMat.lumaW = lumaW;
					badMat.lumaH = lumaH;
					badMat.deliveredFormat = DeliveredChromaFormat::Rgb444;
					badMat.codedSubsampling = ChromaSubsampling::Yuv420;
					badMat.siting = SitingSpec::Left();
					badMat.activeRect = active;
					badMat.matrix = YCbCrMatrixSpec::Bt709();
					badMat.matrix.kg = badKg;
					badMat.deliveredR = delivR;
					badMat.deliveredG = delivG;
					badMat.deliveredB = delivB;
					badMat.codedDeltaCb = codedDeltaCb;
					badMat.codedDeltaCr = codedDeltaCr;

					StandInRegistry siMat;
					const uint64_t failsBefore = gpuGuard.engine.Failures();
					const ChromaGpuApplyResult rMat = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, badMat, &siMat);
					Assert::IsFalse(rMat.succeeded, L"H2 degenerate matrix.kg<=0 must fail closed");
					Assert::IsFalse(rMat.lastError.empty(), L"H2 degenerate matrix.kg<=0 must set lastError");
					Assert::AreEqual(failsBefore + 1u, gpuGuard.engine.Failures(), L"H2 degenerate matrix must increment Failures()");
					Assert::IsTrue(siMat.IsActive(StandInKind::NonFiniteClosure), L"H2 degenerate matrix must engage NonFiniteClosure");
				}
			}
		}

		// =========================================================================
		// 3. ScratchArena Shortage, VRAM User Declaration & Absent/Invalid Inputs
		//    Fail Visibly Without Silent Fallback
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(ScratchArenaShortageAndAbsentInputsFailVisiblyWithoutSilentFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ScratchArenaShortageAndAbsentInputsFailVisiblyWithoutSilentFallback)
		{
			WarpGpuContext ctx;

			const int w = 64;
			const int h = 64;
			const uint64_t estBytes = TdV2GpuEngine::EstimatedVramBytes(w, h);
			Assert::IsTrue(estBytes > 0u, L"EstimatedVramBytes(64,64) must be > 0");
			Assert::AreEqual(static_cast<uint64_t>(0u), TdV2GpuEngine::EstimatedVramBytes(0, 64), L"EstimatedVramBytes(0,64) must be 0");

			// Verify MakeVramUser contract under VpSkeleton::kUserTemporalDetail.
			const VpSkeleton::VramUser vUser = TdV2GpuEngine::MakeVramUser(w, h);
			Assert::AreEqual(static_cast<int>(VpSkeleton::kUserTemporalDetail), static_cast<int>(vUser.id),
				L"MakeVramUser id must be kUserTemporalDetail");
			Assert::AreEqual(static_cast<int>(VpSkeleton::Tier::T1), static_cast<int>(vUser.tier),
				L"MakeVramUser tier must be Tier::T1");
			Assert::IsTrue(vUser.levels.size() >= 2u, L"MakeVramUser must declare full level + shed-to-0 level");
			Assert::AreEqual(estBytes, vUser.levels[0], L"MakeVramUser levels[0] must equal EstimatedVramBytes(w, h)");
			Assert::AreEqual(static_cast<uint64_t>(0u), vUser.levels.back(), L"MakeVramUser shed level must be 0");

			// Null GPU yields empty ArenaRequests.
			Assert::IsTrue(TdV2GpuEngine::ArenaRequests(nullptr, w, h).empty(), L"ArenaRequests(nullptr) must be empty");

			// Build full Tier::T1 ScratchArena plan.
			const std::vector<VpSkeleton::ArenaRequest> reqs = TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h);
			Assert::IsFalse(reqs.empty(), L"ArenaRequests(gpu, 64, 64) must be non-empty");
			const VpSkeleton::ArenaPlan planT1 = VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T1);
			Assert::IsFalse(planT1.Empty(), L"Tier::T1 ArenaPlan must be non-empty");

			VpSkeleton::PlTexBackend texBackend(ctx.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(texBackend);
			Assert::IsTrue(arena.Reserve(planT1), L"ScratchArena::Reserve(planT1) failed");

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;

			ComplexSceneBundle f1 = BuildComplexFrame(1, 1.5, 0.5, /*stampSpecialRegions=*/false);
			FrameProcessInput in1 = MakeFrameInput(f1, 1, /*withPeaks=*/true, /*transformIdentified8x8=*/false);

			// Run private-texture GPU engine (arena == nullptr) as baseline.
			GpuEngineGuard privGuard(ctx.gpu, cfg);
			StandInRegistry privSi;
			const GpuProcessResult privRes = privGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, nullptr, &privSi);
			Assert::IsTrue(privRes.succeeded && !privRes.scratchShort, L"Private-texture GPU baseline failed");
			Assert::IsTrue(MaxAbsDiffRect(privRes.outputY, f1.receivedY, f1.active) > 2.0,
				L"Negative control: baseline GPU output must differ from receivedY by > 2.0 codes");

			// Run arena-backed GPU engine on full Tier::T1 plan -> succeeds & matches private within 1e-5!
			GpuEngineGuard arenaGuard(ctx.gpu, cfg);
			StandInRegistry arenaSi;
			arena.BeginFrame();
			const GpuProcessResult arenaRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, &arena, &arenaSi);
			Assert::IsTrue(arenaRes.succeeded, L"Arena-backed Tier::T1 ProcessFrame failed");
			Assert::IsFalse(arenaRes.scratchShort || arenaGuard.engine.ScratchShort(), L"Full T1 plan must not be scratchShort");
			Assert::IsFalse(arenaSi.IsActive(StandInKind::GpuScratchShortage), L"GpuScratchShortage must not be active on T1");
			Assert::IsFalse(arena.FrameDecision().engaged, L"ScratchArena FrameDecision must not be engaged on T1");
			Assert::IsTrue(arena.Acquired() > 0u, L"ScratchArena must record acquired textures");
			Assert::IsTrue(arenaGuard.engine.HasCarry(), L"Successful arena frame must establish carry");

			Assert::IsTrue(MaxAbsDiffFull(arenaRes.outputY, privRes.outputY) <= 1e-5, L"Arena vs private outputY mismatch > 1e-5");
			Assert::IsTrue(MaxAbsDiffFull(arenaRes.outputClean, privRes.outputClean) <= 1e-5, L"Arena vs private outputClean mismatch > 1e-5");
			Assert::IsTrue(MaxAbsDiffFull(arenaRes.tdDelta, privRes.tdDelta) <= 1e-5, L"Arena vs private tdDelta mismatch > 1e-5");

			// ---- Force arena shortage #1: Shed to Tier::T0 (sheds all T1 TD requests) ----
			const VpSkeleton::ArenaPlan planT0 = VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T0);
			Assert::IsTrue(arena.Reserve(planT0), L"ScratchArena::Reserve(planT0) failed");

			StandInRegistry shortSi;
			const uint64_t failBefore = arenaGuard.engine.Failures();
			arena.BeginFrame();
			const GpuProcessResult shortRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, &arena, &shortSi);

			Assert::IsFalse(shortRes.succeeded, L"Shed-to-T0 arena shortage MUST fail visibly (succeeded == false)");
			Assert::IsTrue(shortRes.scratchShort && arenaGuard.engine.ScratchShort(), L"scratchShort must be true on T0 shed");
			Assert::AreEqual(failBefore + 1u, arenaGuard.engine.Failures(), L"Failures() must increment on scratch shortage");
			Assert::IsFalse(arenaGuard.engine.LastError().empty(), L"LastError() must be non-empty on scratch shortage");
			Assert::IsFalse(shortRes.lastError.empty(), L"res.lastError must be non-empty on scratch shortage");
			Assert::IsTrue(shortSi.IsActive(StandInKind::GpuScratchShortage), L"StandInKind::GpuScratchShortage must be engaged");
			Assert::IsTrue(arena.FrameDecision().engaged, L"ScratchArena::FrameDecision must be engaged on shortage");
			Assert::IsFalse(arenaGuard.engine.HasCarry(), L"Carry must be reset on scratch shortage");
			Assert::IsTrue(shortRes.carryReset, L"carryReset must be true when pre-existing carry was reset by shortage");
			Assert::AreEqual(0, shortRes.dispatchCount, L"Zero GPU dispatches allowed on scratch shortage");
			Assert::AreEqual(0.0, MaxAbsDiffFull(shortRes.outputY, f1.receivedY),
				L"outputY must stay unmodified on scratch shortage (ZERO silent fallback to private textures!)");
			Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(),
				L"ArenaScopeGuard must cleanly close scope on T0 shortage (NestedScopes == 0)");

			// ---- M3: Seed & Cut frames (refSpecs.empty()) MUST succeed WITHOUT tripping GpuScratchShortage
			//      even under a tight Tier::T0 arena (0 scratch textures reserved for TD)! ----
			{
				FrameProcessInput seedIn = MakeFrameInput(f1, 10, /*withPeaks=*/false, /*transformIdentified8x8=*/false);
				StandInRegistry seedSi;
				arena.BeginFrame();
				const GpuProcessResult seedRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, seedIn, &arena, &seedSi);
				Assert::IsTrue(seedRes.succeeded, L"M3: Seed frame (no refs) must succeed under tight Tier::T0 arena");
				Assert::IsFalse(seedRes.scratchShort, L"M3: Seed frame must NOT trip scratchShort under tight arena");
				Assert::IsFalse(seedSi.IsActive(StandInKind::GpuScratchShortage), L"M3: Seed frame must NOT engage GpuScratchShortage");
				Assert::IsTrue(arenaGuard.engine.HasCarry(), L"M3: Seed frame under tight arena must establish carry");
				Assert::AreEqual(0, seedRes.dispatchCount, L"M3: Seed frame must issue 0 compute dispatches");
				Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(), L"M3: Seed frame NestedScopes must be 0");

				FrameProcessInput cutIn = MakeFrameInput(f1, 11, /*withPeaks=*/true, /*transformIdentified8x8=*/false, /*isCut=*/true);
				StandInRegistry cutSi;
				arena.BeginFrame();
				const GpuProcessResult cutRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, cutIn, &arena, &cutSi);
				Assert::IsTrue(cutRes.succeeded, L"M3: Cut frame (refSpecs empty) must succeed under tight Tier::T0 arena");
				Assert::IsFalse(cutRes.scratchShort, L"M3: Cut frame must NOT trip scratchShort under tight arena");
				Assert::IsFalse(cutSi.IsActive(StandInKind::GpuScratchShortage), L"M3: Cut frame must NOT engage GpuScratchShortage");
				Assert::IsTrue(cutRes.carryReset && arenaGuard.engine.HasCarry(), L"M3: Cut frame must reset and re-seed carry");
				Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(), L"M3: Cut frame NestedScopes must be 0");

				// Negative control: immediately following the successful seed/cut on the EXACT SAME Tier::T0 arena,
				// a frame WITH active peaks MUST fail visibly with GpuScratchShortage!
				StandInRegistry negShortSi;
				arena.BeginFrame();
				const GpuProcessResult negShortRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, &arena, &negShortSi);
				Assert::IsFalse(negShortRes.succeeded, L"M3 neg-ctrl: active frame on T0 arena must fail visibly");
				Assert::IsTrue(negShortRes.scratchShort, L"M3 neg-ctrl: active frame on T0 arena must set scratchShort");
				Assert::IsTrue(negShortSi.IsActive(StandInKind::GpuScratchShortage), L"M3 neg-ctrl: must engage GpuScratchShortage");
				Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(), L"M3 neg-ctrl: NestedScopes must stay 0");
			}

			// ---- Force arena shortage #2 (1-texture undersized budget): missing just 1 RGBA32F texture ----
			std::vector<VpSkeleton::ArenaRequest> partialReqs = reqs;
			bool reducedOne = false;
			for (auto& r : partialReqs)
			{
				if (r.shape.fmt == VpSkeleton::TexFormat::RGBA32F && r.count > 1)
				{
					--r.count;
					reducedOne = true;
					break;
				}
			}
			Assert::IsTrue(reducedOne, L"Test setup failed to reduce RGBA32F request count by 1");
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(partialReqs, VpSkeleton::Tier::T1)), L"Reserve(partial) failed");
			StandInRegistry partialSi;
			arena.BeginFrame();
			const GpuProcessResult partialRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, &arena, &partialSi);
			Assert::IsFalse(partialRes.succeeded, L"1-texture undersized arena must fail visibly");
			Assert::IsTrue(partialRes.scratchShort, L"1-texture undersized arena must set scratchShort == true");
			Assert::IsTrue(partialSi.IsActive(StandInKind::GpuScratchShortage), L"GpuScratchShortage must be active on partial shortage");
			Assert::AreEqual(0.0, MaxAbsDiffFull(partialRes.outputY, f1.receivedY), L"Partial shortage must leave outputY unmodified");
			Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(),
				L"ArenaScopeGuard must cleanly close scope on partial shortage (NestedScopes == 0)");

			// ---- Automatic recovery when Tier::T1 is restored ----
			Assert::IsTrue(arena.Reserve(planT1), L"Re-reserve T1 failed");
			StandInRegistry recovSi;
			arena.BeginFrame();
			const GpuProcessResult recovRes = arenaGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, &arena, &recovSi);
			Assert::IsTrue(recovRes.succeeded && !recovRes.scratchShort, L"Recovery on restored T1 failed");
			Assert::IsTrue(MaxAbsDiffRect(recovRes.outputY, f1.receivedY, f1.active) > 2.0, L"Recovered T1 frame must resume real repair");
			Assert::AreEqual(static_cast<uint64_t>(0u), arena.NestedScopes(),
				L"ArenaScopeGuard must maintain NestedScopes == 0 through recovery");

			// ---- Absent / Invalid Inputs: null pl_gpu, null pl_dispatch, empty frame, invalid sourceBits (0, 25) ----
			{
				GpuEngineGuard errGuard(ctx.gpu, cfg);
				uint64_t expectedFails = 0;

				// 1. Null pl_gpu
				StandInRegistry si;
				const GpuProcessResult rNullGpu = errGuard.engine.ProcessFrame(nullptr, ctx.dispatch, in1, nullptr, &si);
				Assert::IsFalse(rNullGpu.succeeded, L"Null pl_gpu must fail visibly");
				Assert::IsTrue(si.IsActive(StandInKind::GpuDispatchFailed), L"Null pl_gpu must engage GpuDispatchFailed");
				Assert::IsFalse(errGuard.engine.LastError().empty(), L"Null pl_gpu must set LastError()");
				Assert::AreEqual(++expectedFails, errGuard.engine.Failures(), L"Null pl_gpu must increment Failures()");

				// 2. Null pl_dispatch
				si.Reset();
				const GpuProcessResult rNullDisp = errGuard.engine.ProcessFrame(ctx.gpu, nullptr, in1, nullptr, &si);
				Assert::IsFalse(rNullDisp.succeeded, L"Null pl_dispatch must fail visibly");
				Assert::IsTrue(si.IsActive(StandInKind::GpuDispatchFailed), L"Null pl_dispatch must engage GpuDispatchFailed");
				Assert::IsFalse(errGuard.engine.LastError().empty(), L"Null pl_dispatch must set LastError()");
				Assert::AreEqual(++expectedFails, errGuard.engine.Failures(), L"Null pl_dispatch must increment Failures()");

				// 3. Empty frame
				si.Reset();
				FrameProcessInput emptyIn;
				const GpuProcessResult rEmpty = errGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, emptyIn, nullptr, &si);
				Assert::IsFalse(rEmpty.succeeded, L"Empty frame must fail visibly");
				Assert::IsTrue(si.IsActive(StandInKind::GpuDispatchFailed), L"Empty frame must engage GpuDispatchFailed");
				Assert::IsFalse(errGuard.engine.LastError().empty(), L"Empty frame must set LastError()");
				Assert::AreEqual(++expectedFails, errGuard.engine.Failures(), L"Empty frame must increment Failures()");

				// 4 & 5. Invalid sourceBits (0 and 25)
				for (int badBits : { 0, 25 })
				{
					Config badCfg = cfg;
					badCfg.sourceBits = badBits;
					errGuard.engine.SetConfig(badCfg);
					si.Reset();
					const GpuProcessResult rBits = errGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in1, nullptr, &si);
					Assert::IsFalse(rBits.succeeded, FormatMsg("Invalid sourceBits=%.0f must fail visibly", static_cast<double>(badBits)).c_str());
					Assert::IsTrue(si.IsActive(StandInKind::NonFiniteClosure), L"Invalid sourceBits must engage NonFiniteClosure");
					Assert::IsFalse(errGuard.engine.LastError().empty(), L"Invalid sourceBits must set LastError()");
					Assert::AreEqual(++expectedFails, errGuard.engine.Failures(), L"Invalid sourceBits must increment Failures()");
					Assert::IsFalse(errGuard.engine.HasCarry(), L"Invalid sourceBits must reset carry");
					Assert::IsTrue(rBits.carryReset, L"Invalid sourceBits must set carryReset == true");
				}
			}
		}

		// =========================================================================
		// 4. Zero NaN / +Inf / -Inf Propagation on GPU Shaders Across receivedY,
		//    cleanY, prevPeak.cleanPlane, CellMotionField, and retainedGrainVar
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(ZeroNanInfPropagationOnGpuShadersAcrossRefMotionAndGrainInputs)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ZeroNanInfPropagationOnGpuShadersAcrossRefMotionAndGrainInputs)
		{
			WarpGpuContext ctx;

			const double kNaN  = std::numeric_limits<double>::quiet_NaN();
			const double kPInf = std::numeric_limits<double>::infinity();
			const double kNInf = -std::numeric_limits<double>::infinity();

			// ---- NEGATIVE CONTROL (Real D3D11 WARP Shader + CPU FP32):
			//      Prove un-guarded FP32 arithmetic on D3D11 WARP genuinely propagates NaN / +Inf / -Inf
			//      (so IEEE-754 bitmask guard td_is_bad is actively protecting the TD shaders, NOT vacuous!). ----
			{
				const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
				Assert::IsNotNull(fmtR32, L"R32F unsupported on WARP");
				pl_tex inTex = nullptr;
				pl_tex outTex = nullptr;
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, inTex, 4, 1, fmtR32, true, false, true, false), L"NegCtrl inTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outTex, 4, 1, fmtR32, true, true, false, true), L"NegCtrl outTex alloc");

				const float badInputs[4] = {
					static_cast<float>(kNaN),
					static_cast<float>(kPInf),
					static_cast<float>(kNInf),
					42.0f
				};
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, inTex, badInputs), L"NegCtrl upload failed");

				const char* unguardedBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= 4 || p.y >= 1) return;
float v = texelFetch(src_bad, p, 0).x;
float y = v * 0.5 + 10.0;
imageStore(dst_bad, p, vec4(y, 0.0, 0.0, 0.0));
)GLSL";
				pl_shader_desc d[2] = {
					VpvGpu::Sampled("src_bad", inTex),
					VpvGpu::Storage("dst_bad", outTex)
				};
				const bool dispOk = VpvGpu::Pass(ctx.dispatch, "td_v2_negctrl_unguarded_fp32", "", unguardedBody,
				                                 d, 2, nullptr, 0, 1, 1, 0, 4, 1);
				float badOutputs[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				const bool dlOk = dispOk && Detail::DownloadFloats(ctx.gpu, outTex, badOutputs);
				pl_tex_destroy(ctx.gpu, &inTex);
				pl_tex_destroy(ctx.gpu, &outTex);

				Assert::IsTrue(dlOk, L"NegCtrl un-guarded D3D11 compute pass failed");
				Assert::IsTrue(std::isnan(badOutputs[0]), L"Negative control: D3D11 WARP must propagate NaN when un-guarded");
				Assert::IsTrue(std::isinf(badOutputs[1]) && badOutputs[1] > 0.0f, L"Negative control: D3D11 WARP must propagate +Inf when un-guarded");
				Assert::IsTrue(std::isinf(badOutputs[2]) && badOutputs[2] < 0.0f, L"Negative control: D3D11 WARP must propagate -Inf when un-guarded");
				Assert::IsTrue(std::isfinite(badOutputs[3]), L"Negative control: finite lane sanity check");
			}

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;
			cfg.enableCarryForward = false;

			// ---- Case A: NaN / +Inf / -Inf in receivedY -> fail-closed passthrough + carryReset + NonFiniteClosure ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.receivedY.at(10, 10) = kNaN;
				b.receivedY.at(20, 20) = kPInf;
				b.receivedY.at(30, 30) = kNInf;
				FrameProcessInput in = MakeFrameInput(b, 1, true, false);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				Assert::IsTrue(gpuRes.succeeded, L"receivedY NaN fail-closed must return cleanly");
				Assert::IsTrue(gpuRes.carryReset && cpuRes.carryReset, L"receivedY NaN must reset carry");
				Assert::AreEqual(cpuRes.nonFiniteCells, gpuRes.nonFiniteCells, L"receivedY NaN nonFiniteCells parity");
				Assert::AreEqual(static_cast<size_t>(b.cellsW * b.cellsH), gpuRes.nonFiniteCells, L"receivedY NaN must count all cells");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure) && cpuSi.IsActive(StandInKind::NonFiniteClosure),
					L"receivedY NaN must engage NonFiniteClosure");
				Assert::AreEqual(0.0, MaxAbsPlane(gpuRes.tdDelta, ActiveRect::Full(b.w, b.h)), L"receivedY NaN tdDelta must be 0");
				Assert::AreEqual(0, gpuRes.dispatchCount, L"receivedY NaN fail-closed must issue 0 dispatches");
			}

			// ---- Case B: NaN / +Inf / -Inf in cleanY (hasCleanSplit = true) -> fail-closed passthrough ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.curClean.at(12, 14) = kNaN;
				b.curClean.at(22, 24) = kPInf;
				b.curClean.at(32, 34) = kNInf;
				FrameProcessInput in = MakeFrameInput(b, 2, true, false);
				// Keep receivedY finite so we specifically test the cleanY guard!
				in.receivedY = RenderScene(b.w, b.h, 1.0, 0.5);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				Assert::IsTrue(gpuRes.succeeded, L"cleanY NaN fail-closed must return cleanly");
				Assert::IsTrue(gpuRes.carryReset && cpuRes.carryReset, L"cleanY NaN must reset carry");
				Assert::AreEqual(cpuRes.nonFiniteCells, gpuRes.nonFiniteCells, L"cleanY NaN nonFiniteCells parity");
				Assert::AreEqual(static_cast<size_t>(b.cellsW * b.cellsH), gpuRes.nonFiniteCells, L"cleanY NaN must count all cells");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure), L"cleanY NaN must engage NonFiniteClosure");
				Assert::AreEqual(0.0, MaxAbsDiffFull(gpuRes.outputY, in.receivedY), L"cleanY NaN must pass receivedY untouched");
			}

			// ---- Case C: NaN / +Inf / -Inf inside prevPeak.cleanPlane (GPU Pass 1B warp + Pass 2 + Pass 3 + Pass 4) ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.prevClean.at(10, 12) = kNaN;
				b.prevClean.at(24, 26) = kPInf;
				b.prevClean.at(38, 40) = kNInf;
				FrameProcessInput in = MakeFrameInput(b, 3, true, false);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, "CaseC-RefCleanNaN");
				Assert::IsTrue(gpuRes.nonFiniteCells > 0u, L"prevPeak NaN/Inf must increment nonFiniteCells");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure), L"prevPeak NaN/Inf must engage NonFiniteClosure");
				// Uncontaminated cells still produce real repair (> 1.5 codes).
				Assert::IsTrue(MaxAbsDiffRect(gpuRes.outputY, b.receivedY, ActiveRect{ 48, 4, 64, 24 }) > 1.5,
					L"Uncontaminated region should still be repaired when other pixels in prevPeak are NaN");
			}

			// ---- Case D: NaN / +Inf / -Inf inside CellMotionField (vx, vy, camVx) ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.prevMf.vx[1u * b.cellsW + 0u] = kNaN;
				b.prevMf.vy[2u * b.cellsW + 1u] = kPInf;
				b.prevMf.camVx[1u * b.cellsW + 2u] = kNInf;
				FrameProcessInput in = MakeFrameInput(b, 4, true, false);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, "CaseD-MotionNaN");
				Assert::IsTrue(gpuRes.nonFiniteCells >= 3u, L"Motion NaN/+Inf/-Inf must report >= 3 nonFiniteCells");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure), L"Motion NaN/Inf must engage NonFiniteClosure");
			}

			// ---- Case E: NaN / +Inf / -Inf / negative inside retainedGrainVar ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.retainedGrainVar[0] = kNaN;
				b.retainedGrainVar[5] = kPInf;
				b.retainedGrainVar[10] = kNInf;
				b.retainedGrainVar[15] = -4.0;
				FrameProcessInput in = MakeFrameInput(b, 5, true, false);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, "CaseE-GrainVarNaN");
				Assert::AreEqual(static_cast<size_t>(4u), gpuRes.nonFiniteCells,
					L"4 invalid retainedGrainVar cells must produce exact nonFiniteCells == 4");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure), L"retainedGrainVar NaN/Inf must engage NonFiniteClosure");
			}

			// ---- Case F: Combined simultaneous NaN/+Inf/-Inf across prevPeak + CellMotionField + retainedGrainVar ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.prevClean.at(8, 10) = kNaN;
				b.prevClean.at(28, 30) = kPInf;
				b.prevMf.vx[0u * b.cellsW + 3u] = kNInf;
				b.prevMf.camVx[3u * b.cellsW + 0u] = kNaN;
				b.retainedGrainVar[6] = kPInf;
				b.retainedGrainVar[9] = kNaN;
				FrameProcessInput in = MakeFrameInput(b, 6, true, false);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, "CaseF-CombinedNaN");
				Assert::IsTrue(gpuRes.nonFiniteCells >= 5u, L"Combined NaN/Inf must count all closures");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure), L"Combined NaN/Inf must engage NonFiniteClosure");
			}

			// ---- Case G (M1): Simultaneous NaN/+Inf in BOTH prevPeak AND nextPeak -> exact +2 nonFiniteCells parity! ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				b.prevClean.at(10, 12) = kNaN;
				b.nextClean.at(36, 38) = kPInf;
				FrameProcessInput in = MakeFrameInput(b, 7, true, false);

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, "CaseG-MultiRefNaN");
				Assert::AreEqual(static_cast<size_t>(2u), cpuRes.nonFiniteCells,
					L"M1 CPU baseline: simultaneous NaN in prevPeak + nextPeak must yield 2 nonFiniteCells");
				Assert::AreEqual(cpuRes.nonFiniteCells, gpuRes.nonFiniteCells,
					L"M1 GPU parity: simultaneous NaN in prevPeak + nextPeak must report +2 (NOT +1)!");
				Assert::IsTrue(gpuSi.IsActive(StandInKind::NonFiniteClosure), L"M1 multi-ref NaN must engage NonFiniteClosure");
			}

			// ---- Case H (H3): NaN deadZonePhi & NaN step/zeroFrac must NOT crash GLSL compile & must match CPU <= 1e-3 ----
			for (bool xform8x8 : { false, true })
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				FrameProcessInput in = MakeFrameInput(b, xform8x8 ? 8 : 9, true, xform8x8);
				in.deadZonePhi = kNaN;

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				Assert::IsTrue(gpuRes.succeeded, L"H3: NaN deadZonePhi must NOT crash GLSL compilation or dispatch");
				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active,
					xform8x8 ? "CaseH-NaNPhi-8x8" : "CaseH-NaNPhi-Unknown");
			}
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				FrameProcessInput in = MakeFrameInput(b, 10, true, false);
				in.estimatedStepDelta = kNaN;
				in.zeroCoeffFraction = kNaN;

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				StandInRegistry cpuSi, gpuSi;

				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
				const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

				Assert::IsTrue(gpuRes.succeeded, L"H3: NaN stepDelta/zeroFrac must fall back cleanly without GLSL error");
				Assert::IsTrue(cpuSi.IsActive(StandInKind::StepPriorDefault) && gpuSi.IsActive(StandInKind::StepPriorDefault),
					L"H3: NaN stepDelta/zeroFrac must engage StepPriorDefault on CPU & GPU");
				AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, "CaseH-NaNStepPrior");
			}

			// ---- Case I (H3): Dynamic UBO per-frame P16 stepDelta & zeroCoeffFraction across consecutive frames ----
			{
				ComplexSceneBundle b = BuildComplexFrame(1, 1.0, 0.5, false);
				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);

				struct StepSpec { double delta; double zf; const char* tag; };
				const StepSpec specs[3] = {
					{ 4.0,  0.05, "CaseI-Step4" },
					{ 28.0, 0.55, "CaseI-Step28" },
					{ 12.0, 0.25, "CaseI-Step12" }
				};

				TdV2Plane gpuOutStep4, gpuOutStep28;
				for (int idx = 0; idx < 3; ++idx)
				{
					FrameProcessInput in = MakeFrameInput(b, 20 + static_cast<uint64_t>(idx), true, false);
					in.estimatedStepDelta = specs[idx].delta;
					in.zeroCoeffFraction = specs[idx].zf;

					const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, nullptr);
					const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
					AssertCpuGpuFrameParity(cpuRes, gpuRes, b.curClean, b.receivedY, b.active, specs[idx].tag);
					if (idx == 0) gpuOutStep4 = gpuRes.outputY;
					if (idx == 1) gpuOutStep28 = gpuRes.outputY;
				}
				// Negative control: changing stepDelta/zeroFrac on the same engine MUST materially alter GPU output!
				const double stepDiff = MaxAbsDiffRect(gpuOutStep4, gpuOutStep28, b.active);
				Assert::IsTrue(stepDiff > 1.0,
					FormatMsg("H3 negative control failed: stepDelta 4 vs 28 GPU diff=%.4f <= 1.0 (UBO stale?)", stepDiff).c_str());
			}
		}

		// =========================================================================
		// 5. Mode::Off & Mode::Shadow Byte-Identical Passthrough and Zero GPU
		//    Dispatches in Mode::Off
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(OffAndShadowModesByteIdenticalAndOffIssuesZeroGpuDispatches)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OffAndShadowModesByteIdenticalAndOffIssuesZeroGpuDispatches)
		{
			WarpGpuContext ctx;

			ComplexSceneBundle b = BuildComplexFrame(1, 1.5, 0.5, /*stampSpecialRegions=*/true);
			FrameProcessInput in = MakeFrameInput(b, 1, /*withPeaks=*/true, /*transformIdentified8x8=*/false);

			// ---- NEGATIVE CONTROL: Mode::On actually modifies pixels (> 2.0 codes) on the exact same input ----
			Config cfgOn = MakePreset(Preset::Reference, Mode::On);
			cfgOn.enableLongAnchor = false;
			GpuEngineGuard onGuard(ctx.gpu, cfgOn);
			const GpuProcessResult onRes = onGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
			Assert::IsTrue(onRes.succeeded, L"Mode::On negative control failed");
			Assert::IsTrue(onRes.dispatchCount > 0, L"Mode::On must issue > 0 GPU dispatches");
			const double onPixelChange = MaxAbsDiffRect(onRes.outputY, b.receivedY, b.active);
			const double onDeltaPeak = MaxAbsPlane(onRes.tdDelta, b.active);
			Assert::IsTrue(onPixelChange > 2.0 && onDeltaPeak > 2.0,
				FormatMsg("Negative control failed: Mode::On change=%.4f, delta=%.4f <= 2.0", onPixelChange, onDeltaPeak).c_str());

			// ---- Mode::Off (with valid GPU/dispatch AND with null GPU/dispatch) ----
			{
				Config cfgOff = cfgOn;
				cfgOff.mode = Mode::Off;
				GpuEngineGuard offGuard(ctx.gpu, cfgOff);

				const GpuProcessResult offRes = offGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
				Assert::IsTrue(offRes.succeeded, L"Mode::Off must succeed");
				Assert::AreEqual(0, offRes.dispatchCount, L"Mode::Off must issue ZERO GPU dispatches");
				Assert::AreEqual(0, offGuard.engine.LastDispatchCount(), L"Mode::Off LastDispatchCount() must be 0");
				Assert::AreEqual(b.receivedY.v.size(), offRes.outputY.v.size(), L"Mode::Off outputY size mismatch");
				Assert::AreEqual(0, std::memcmp(offRes.outputY.v.data(), b.receivedY.v.data(), b.receivedY.v.size() * sizeof(double)),
					L"Mode::Off outputY must be BIT-EXACT identical to receivedY");
				Assert::AreEqual(0.0, MaxAbsPlane(offRes.tdDelta, ActiveRect::Full(b.w, b.h)), L"Mode::Off tdDelta must be 0");

				// Even with null GPU & null dispatch, Mode::Off must succeed cleanly with 0 dispatches!
				const GpuProcessResult offNullRes = offGuard.engine.ProcessFrame(nullptr, nullptr, in, nullptr, nullptr);
				Assert::IsTrue(offNullRes.succeeded, L"Mode::Off with null GPU/dispatch must succeed");
				Assert::AreEqual(0, offNullRes.dispatchCount, L"Mode::Off null GPU dispatchCount must be 0");
				Assert::AreEqual(0, std::memcmp(offNullRes.outputY.v.data(), b.receivedY.v.data(), b.receivedY.v.size() * sizeof(double)),
					L"Mode::Off null GPU outputY must be bit-exact identical to receivedY");
			}

			// ---- Mode::Shadow ----
			{
				Config cfgShadow = cfgOn;
				cfgShadow.mode = Mode::Shadow;
				GpuEngineGuard shGuard(ctx.gpu, cfgShadow);

				const GpuProcessResult shRes = shGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, nullptr);
				Assert::IsTrue(shRes.succeeded, L"Mode::Shadow must succeed");
				Assert::IsTrue(shRes.dispatchCount > 0, L"Mode::Shadow must run full GPU compute passes (dispatchCount > 0)");
				Assert::AreEqual(shRes.dispatchCount, shGuard.engine.LastDispatchCount(), L"Shadow LastDispatchCount mismatch");
				Assert::AreEqual(b.receivedY.v.size(), shRes.outputY.v.size(), L"Shadow outputY size mismatch");
				Assert::AreEqual(0, std::memcmp(shRes.outputY.v.data(), b.receivedY.v.data(), b.receivedY.v.size() * sizeof(double)),
					L"Mode::Shadow outputY MUST be bit-exact identical to receivedY");

				// Shadow diagnostics (tdDelta, outputClean, tdMaps) must match Mode::On within 1e-3!
				const double dDelta = MaxAbsDiffFull(shRes.tdDelta, onRes.tdDelta);
				const double dClean = MaxAbsDiffFull(shRes.outputClean, onRes.outputClean);
				const double dMaps = MaxTdMapsDiff(shRes.tdMaps, onRes.tdMaps);
				Assert::IsTrue(dDelta <= kGpuCpuParityTol, FormatMsg("Shadow vs On tdDelta diff=%.6f", dDelta).c_str());
				Assert::IsTrue(dClean <= kGpuCpuParityTol, FormatMsg("Shadow vs On outputClean diff=%.6f", dClean).c_str());
				Assert::IsTrue(dMaps <= kGpuCpuParityTol, FormatMsg("Shadow vs On tdMaps diff=%.6f", dMaps).c_str());
				Assert::IsTrue(MaxAbsPlane(shRes.tdDelta, b.active) > 2.0, L"Shadow tdDelta must be non-vacuously active");
			}
		}

		// =========================================================================
		// 6. M1 Re-Audit Cleanup Verification in TdV2Chroma.h:
		//    (a) Delivered-grid peak ref with NaN on 4:2:0-coded delivered as 4:2:2 / RGB
		//    (b) Malformed splatMotion in AccumulateAnchorFrame leaves canvas unmutated
		//    (c) ApplySignedChromaDeltaToRgb guards (kg<=0, NaN delta, NaN kr/kb, +Inf overflow)
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(M1ReAuditCleanupDeliveredRefNanMalformedSplatAndRgbGuards)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(M1ReAuditCleanupDeliveredRefNanMalformedSplatAndRgbGuards)
		{
			const double kNaN  = std::numeric_limits<double>::quiet_NaN();
			const double kPInf = std::numeric_limits<double>::infinity();

			// =====================================================================
			// Part A: Delivered-grid peak ref with NaN on 4:2:0-coded delivered as 4:2:2 & RGB 4:4:4
			// =====================================================================
			{
				const int lumaW = 64;
				const int lumaH = 64;
				const ActiveRect active = ActiveRect::Full(lumaW, lumaH);
				const double trueCbVal = 660.0;
				const double trueCrVal = 370.0;

				// 1. Direct ReduceToCodedGrid proof on delivered 4:2:2 (32 x 64) with NaN at (10, 20):
				TdV2Plane delivRefCb422(32, 64, trueCbVal);
				TdV2Plane delivRefCr422(32, 64, trueCrVal);
				delivRefCb422.at(10, 20) = kNaN;
				delivRefCr422.at(10, 20) = kPInf;

				StandInRegistry redSi;
				const TdV2Plane codedRefCb = ReduceToCodedGrid(
					delivRefCb422, lumaW, lumaH, DeliveredChromaFormat::Yuv422, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, &redSi);
				Assert::IsTrue(redSi.IsActive(StandInKind::NonFiniteClosure),
					L"ReduceToCodedGrid must engage NonFiniteClosure on delivered NaN");
				// Coded sample (10, 10) sits at luma y = 20.5, whose bicubic footprint includes delivered y=20!
				Assert::IsFalse(std::isfinite(codedRefCb.at(10, 10)),
					L"ReduceToCodedGrid must preserve NaN at affected coded sample (NOT substitute 0.0!)");

				// NEGATIVE CONTROL (ReduceToCodedGrid 0.0-substitution bug):
				// Substituting 0.0 for NaN injects a massive > 100-code (-660 code!) spike!
				const double naiveZeroSubstErr = std::fabs(0.0 - trueCbVal);
				Assert::IsTrue(naiveZeroSubstErr > 100.0,
					FormatMsg("Negative control: 0.0-substitution error=%.1f <= 100.0", naiveZeroSubstErr).c_str());

				// 2. End-to-end TdV2ChromaEngine on 4:2:0-coded delivered as 4:2:2 with delivered-grid NaN peak ref:
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.estimator = VarianceEstimator::A2_RoundingCells;

				TdV2Plane curCb422(32, 64, trueCbVal);
				TdV2Plane curCr422(32, 64, trueCrVal);
				std::mt19937 rng(9123u);
				std::normal_distribution<double> nd(0.0, 6.0);
				for (size_t i = 0; i < curCb422.v.size(); ++i)
				{
					curCb422.v[i] += nd(rng);
					curCr422.v[i] += nd(rng);
				}

				// Inject a 2x4 NaN block in delivered 4:2:2 prevPeakCb so a 0.0-substitution bug would create a huge spike.
				TdV2Plane peakNanCb422(32, 64, trueCbVal);
				TdV2Plane peakNanCr422(32, 64, trueCrVal);
				TdV2Plane peakZeroSubstCb422(32, 64, trueCbVal);
				TdV2Plane peakZeroSubstCr422(32, 64, trueCrVal);
				for (int y = 20; y < 24; ++y)
				{
					for (int x = 10; x < 12; ++x)
					{
						peakNanCb422.at(x, y) = kNaN;
						peakZeroSubstCb422.at(x, y) = 0.0;
					}
				}

				const CellMotionField stillMf = CellMotionField::Uniform(4, 4, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				ChromaProcessInput cIn422;
				cIn422.deliveredFormat = DeliveredChromaFormat::Yuv422;
				cIn422.codedSubsampling = ChromaSubsampling::Yuv420;
				cIn422.gridStatus = ChromaGridStatus::Identified;
				cIn422.siting = SitingSpec::Left();
				cIn422.lumaW = lumaW;
				cIn422.lumaH = lumaH;
				cIn422.receivedCb = curCb422;
				cIn422.receivedCr = curCr422;
				cIn422.prevPeak.cleanCb = &peakNanCb422;
				cIn422.prevPeak.cleanCr = &peakNanCr422;
				cIn422.prevPeak.lumaMotion = &stillMf;
				cIn422.hasStepPrior = true;
				cIn422.estimatedStepDelta = 12.0;

				TdV2ChromaEngine chEng(cfg);
				StandInRegistry chSi;
				const ChromaProcessResult chRes422 = chEng.ProcessFrame(cIn422, &chSi);

				AssertAllFinitePlane(chRes422.outputCb, L"4:2:2 delivered-ref NaN outputCb must be 100% finite");
				AssertAllFinitePlane(chRes422.outputCr, L"4:2:2 delivered-ref NaN outputCr must be 100% finite");
				Assert::IsTrue(chRes422.nonFiniteCells > 0u, L"4:2:2 delivered-ref NaN must increment res.nonFiniteCells > 0");
				Assert::IsTrue(chSi.IsActive(StandInKind::NonFiniteClosure), L"4:2:2 delivered-ref NaN must engage NonFiniteClosure");

				// Must NOT inject a -512 (0.0-code) spike near (10..12, 20..24)!
				const TdV2Plane truthCb422(32, 64, trueCbVal);
				const double maxErrNearNan422 = MaxAbsDiffRect(chRes422.outputCb, truthCb422, ActiveRect{ 8, 16, 14, 28 });
				Assert::IsTrue(maxErrNearNan422 < 25.0,
					FormatMsg("4:2:2 NaN peak ref leaked spike error=%.2f >= 25.0 codes", maxErrNearNan422).c_str());

				// NEGATIVE CONTROL (End-to-end 0.0-substitution vs NaN preservation):
				// Substituting 0.0 for NaN in the delivered peak ref fails to engage NonFiniteClosure, reports
				// nonFiniteCells == 0, and drags the blended estimate strongly negative when step prior is large!
				{
					// Directly demonstrate naive 50/50 blend with 0.0-substituted reduced ref vs NaN-gated validPx==0:
					const TdV2Plane codedZeroSubst = ReduceToCodedGrid(
						peakZeroSubstCb422, lumaW, lumaH, DeliveredChromaFormat::Yuv422, ChromaSubsampling::Yuv420,
						SitingSpec::Left(), active, nullptr);
					const double naiveBlendErr = std::fabs(0.5 * (trueCbVal + codedZeroSubst.at(10, 10)) - trueCbVal);
					Assert::IsTrue(naiveBlendErr > 100.0,
						FormatMsg("Negative control: 0.0-substituted ref blend error=%.2f <= 100.0", naiveBlendErr).c_str());

					// Also verify TdV2ChromaEngine with 0.0-substituted ref silently misses the non-finite hazard!
					ChromaProcessInput cInZero = cIn422;
					cInZero.prevPeak.cleanCb = &peakZeroSubstCb422;
					cInZero.prevPeak.cleanCr = &peakZeroSubstCr422;
					TdV2ChromaEngine chEngZero(cfg);
					StandInRegistry zeroSi;
					const ChromaProcessResult zeroRes = chEngZero.ProcessFrame(cInZero, &zeroSi);
					Assert::AreEqual(static_cast<size_t>(0u), zeroRes.nonFiniteCells,
						L"Negative control: 0.0-substituted ref silently reports 0 nonFiniteCells");
					Assert::IsFalse(zeroSi.IsActive(StandInKind::NonFiniteClosure),
						L"Negative control: 0.0-substituted ref silently misses NonFiniteClosure");
				}

				// 3. End-to-end TdV2ChromaEngine on 4:2:0-coded delivered as RGB 4:4:4 with delivered-grid NaN peak ref:
				{
					const YCbCrMatrixSpec mat = YCbCrMatrixSpec::Bt709();
					TdV2Plane recR(lumaW, lumaH, 0.0), recG(lumaW, lumaH, 0.0), recB(lumaW, lumaH, 0.0);
					for (int y = 0; y < lumaH; ++y)
					{
						for (int x = 0; x < lumaW; ++x)
						{
							const double yv = 500.0;
							const double dcb = (trueCbVal + nd(rng)) - 512.0;
							const double dcr = (trueCrVal + nd(rng)) - 512.0;
							double r = yv, g = yv, b = yv;
							mat.ApplySignedChromaDeltaToRgb(yv, yv, yv, dcb, dcr, r, g, b, nullptr);
							recR.at(x, y) = r;
							recG.at(x, y) = g;
							recB.at(x, y) = b;
						}
					}
					TdV2Plane peakRgbCb(lumaW, lumaH, trueCbVal);
					TdV2Plane peakRgbCr(lumaW, lumaH, trueCrVal);
					peakRgbCb.at(20, 20) = kNaN;
					peakRgbCr.at(28, 28) = kPInf;

					ChromaProcessInput cInRgb;
					cInRgb.deliveredFormat = DeliveredChromaFormat::Rgb444;
					cInRgb.codedSubsampling = ChromaSubsampling::Yuv420;
					cInRgb.gridStatus = ChromaGridStatus::Identified;
					cInRgb.siting = SitingSpec::TopLeftType2();
					cInRgb.matrix = mat;
					cInRgb.lumaW = lumaW;
					cInRgb.lumaH = lumaH;
					cInRgb.receivedR = recR;
					cInRgb.receivedG = recG;
					cInRgb.receivedB = recB;
					cInRgb.prevPeak.cleanCb = &peakRgbCb;
					cInRgb.prevPeak.cleanCr = &peakRgbCr;
					cInRgb.prevPeak.lumaMotion = &stillMf;
					cInRgb.hasStepPrior = true;
					cInRgb.estimatedStepDelta = 12.0;

					TdV2ChromaEngine rgbEng(cfg);
					StandInRegistry rgbSi;
					const ChromaProcessResult rgbRes = rgbEng.ProcessFrame(cInRgb, &rgbSi);

					AssertAllFinitePlane(rgbRes.outputR, L"RGB delivered-ref NaN outputR must be 100% finite");
					AssertAllFinitePlane(rgbRes.outputG, L"RGB delivered-ref NaN outputG must be 100% finite");
					AssertAllFinitePlane(rgbRes.outputB, L"RGB delivered-ref NaN outputB must be 100% finite");
					Assert::IsTrue(rgbRes.nonFiniteCells > 0u, L"RGB delivered-ref NaN must increment nonFiniteCells > 0");
					Assert::IsTrue(rgbSi.IsActive(StandInKind::NonFiniteClosure), L"RGB delivered-ref NaN must engage NonFiniteClosure");
					Assert::IsTrue(MaxAbsPlane(rgbRes.deliveredDeltaCb, ActiveRect{ 16, 16, 26, 26 }) < 30.0,
						L"RGB delivered-ref NaN must NOT inject a -512 spike in Cb delta");
				}
			}

			// =====================================================================
			// Part B: Malformed splatMotion in TdV2ChromaAnchorCanvas::AccumulateAnchorFrame
			// =====================================================================
			{
				const int lumaW = 64;
				const int lumaH = 64;
				const int cellPx = 16;
				const ActiveRect active = ActiveRect::Full(lumaW, lumaH);

				TdV2Plane codedCb(32, 32, 540.0);
				TdV2Plane codedCr(32, 32, 480.0);
				TdV2Plane cleanY = RenderScene(lumaW, lumaH, 0.0, 0.0);
				for (int y = 0; y < 32; ++y)
				{
					for (int x = 0; x < 32; ++x)
					{
						codedCb.at(x, y) = 480.0 + 0.15 * cleanY.at(x * 2, y * 2);
						codedCr.at(x, y) = 540.0 - 0.12 * cleanY.at(x * 2, y * 2);
					}
				}

				TdV2ChromaAnchorCanvas canvas;
				StandInRegistry canvasSi;
				const CellMotionField goodMf = CellMotionField::Uniform(4, 4, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				Assert::IsTrue(canvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, &goodMf, cellPx, 10, &canvasSi),
					L"Initial valid AccumulateAnchorFrame must succeed");
				Assert::IsTrue(canvas.HasData() && canvas.HasCfl(), L"Canvas must hold data & CfL after valid frame 1");
				Assert::AreEqual(static_cast<size_t>(1u), canvas.AccumulatedFrames(), L"AccumulatedFrames must be 1");

				// Snapshot canvas state before malformed splatMotion calls.
				const std::vector<double> snapWeights = canvas.WeightPlane().v;
				const std::vector<AnchorCflCell> snapCfl = canvas.CflCells();

				// Malformed #1: Wrong grid dimensions (2 x 2 instead of 4 x 4).
				const CellMotionField wrongDimsMf = CellMotionField::Uniform(2, 2, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				canvasSi.Reset();
				Assert::IsFalse(canvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, &wrongDimsMf, cellPx, 10, &canvasSi),
					L"Malformed splatMotion (wrong dims) must return false");
				Assert::IsTrue(canvasSi.IsActive(StandInKind::MotionFieldStandIn),
					L"Malformed splatMotion (wrong dims) must engage MotionFieldStandIn");
				Assert::AreEqual(static_cast<size_t>(1u), canvas.AccumulatedFrames(), L"Canvas frame count must not mutate");
				Assert::AreEqual(0, std::memcmp(snapWeights.data(), canvas.WeightPlane().v.data(), snapWeights.size() * sizeof(double)),
					L"Canvas WeightPlane must not mutate on malformed splatMotion");

				// Malformed #2: Correct cellsW/cellsH (4 x 4) but truncated vx vector!
				CellMotionField truncatedMf = goodMf;
				truncatedMf.vx.pop_back();
				canvasSi.Reset();
				Assert::IsFalse(canvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, &truncatedMf, cellPx, 10, &canvasSi),
					L"Malformed splatMotion (truncated vx) must return false");
				Assert::IsTrue(canvasSi.IsActive(StandInKind::MotionFieldStandIn),
					L"Malformed splatMotion (truncated vx) must engage MotionFieldStandIn");
				Assert::AreEqual(static_cast<size_t>(1u), canvas.AccumulatedFrames(), L"Canvas frame count must stay 1");
				Assert::AreEqual(snapCfl.size(), canvas.CflCells().size(), L"Canvas CfL cells size must stay identical");
				for (size_t i = 0; i < snapCfl.size(); ++i)
				{
					Assert::AreEqual(snapCfl[i].aCb, canvas.CflCells()[i].aCb, L"Canvas CfL aCb mutated on malformed splatMotion");
					Assert::AreEqual(snapCfl[i].bCb, canvas.CflCells()[i].bCb, L"Canvas CfL bCb mutated on malformed splatMotion");
					Assert::AreEqual(snapCfl[i].valid, canvas.CflCells()[i].valid, L"Canvas CfL valid mutated on malformed splatMotion");
				}

				// NEGATIVE CONTROL: Valid 2nd accumulation DOES succeed and mutates canvas state to frame 2!
				canvasSi.Reset();
				Assert::IsTrue(canvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, &goodMf, cellPx, 10, &canvasSi),
					L"Negative control: valid 2nd AccumulateAnchorFrame must succeed");
				Assert::AreEqual(static_cast<size_t>(2u), canvas.AccumulatedFrames(),
					L"Negative control: valid 2nd AccumulateAnchorFrame must increment AccumulatedFrames to 2");

				// L4: Null splatMotion on frame 0 does NOT engage MotionFieldStandIn,
				//     while null splatMotion on frame 1+ DOES engage MotionFieldStandIn!
				TdV2ChromaAnchorCanvas nullMotCanvas;
				StandInRegistry nullMotSi;
				Assert::IsTrue(nullMotCanvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, nullptr, cellPx, 10, &nullMotSi),
					L"L4: Null splatMotion on frame 0 must succeed");
				Assert::AreEqual(static_cast<size_t>(1u), nullMotCanvas.AccumulatedFrames(),
					L"L4: Null splatMotion on frame 0 must set AccumulatedFrames == 1");
				Assert::IsFalse(nullMotSi.IsActive(StandInKind::MotionFieldStandIn),
					L"L4: Null splatMotion on frame 0 must NOT engage MotionFieldStandIn");

				Assert::IsTrue(nullMotCanvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, nullptr, cellPx, 10, &nullMotSi),
					L"L4: Null splatMotion on frame 1 must succeed");
				Assert::AreEqual(static_cast<size_t>(2u), nullMotCanvas.AccumulatedFrames(),
					L"L4: Null splatMotion on frame 1 must increment AccumulatedFrames == 2");
				Assert::IsTrue(nullMotSi.IsActive(StandInKind::MotionFieldStandIn),
					L"L4: Null splatMotion on frame 1+ MUST engage MotionFieldStandIn");
			}

			// =====================================================================
			// Part C: YCbCrMatrixSpec::ApplySignedChromaDeltaToRgb Guards
			//         (kg == 0, NaN delta, NaN kr/kb, +Inf overflow)
			// =====================================================================
			{
				const double r0 = 320.0;
				const double g0 = 512.0;
				const double b0 = 740.0;

				auto checkRgbGuardRejects = [&](const YCbCrMatrixSpec& mat, double r, double g, double b,
				                                double dCb, double dCr, const wchar_t* label)
				{
					StandInRegistry si;
					double outR = -999.0, outG = -999.0, outB = -999.0;
					const bool ok = mat.ApplySignedChromaDeltaToRgb(r, g, b, dCb, dCr, outR, outG, outB, &si);
					Assert::IsFalse(ok, label);
					Assert::IsTrue(si.IsActive(StandInKind::NonFiniteClosure), label);
					Assert::AreEqual(r, outR, label);
					Assert::AreEqual(g, outG, label);
					Assert::AreEqual(b, outB, label);
				};

				// 1. Degenerate kg == 0 and kg < 0
				YCbCrMatrixSpec matKgZero = YCbCrMatrixSpec::Bt709();
				matKgZero.kg = 0.0;
				checkRgbGuardRejects(matKgZero, r0, g0, b0, 10.0, -8.0, L"kg==0 must return false and preserve RGB");

				YCbCrMatrixSpec matKgNeg = YCbCrMatrixSpec::Bt709();
				matKgNeg.kg = -0.25;
				checkRgbGuardRejects(matKgNeg, r0, g0, b0, 10.0, -8.0, L"kg<0 must return false and preserve RGB");

				// 2. NaN / +Inf deltas
				const YCbCrMatrixSpec bt709 = YCbCrMatrixSpec::Bt709();
				checkRgbGuardRejects(bt709, r0, g0, b0, kNaN, -5.0, L"NaN dCb must return false and preserve RGB");
				checkRgbGuardRejects(bt709, r0, g0, b0, 5.0, kNaN,  L"NaN dCr must return false and preserve RGB");
				checkRgbGuardRejects(bt709, r0, g0, b0, kPInf, 0.0, L"+Inf dCb must return false and preserve RGB");

				// 3. NaN kr / kb / kg
				YCbCrMatrixSpec matNanKr = bt709; matNanKr.kr = kNaN;
				checkRgbGuardRejects(matNanKr, r0, g0, b0, 6.0, -6.0, L"NaN kr must return false and preserve RGB");
				YCbCrMatrixSpec matNanKb = bt709; matNanKb.kb = kNaN;
				checkRgbGuardRejects(matNanKb, r0, g0, b0, 6.0, -6.0, L"NaN kb must return false and preserve RGB");
				YCbCrMatrixSpec matNanKg = bt709; matNanKg.kg = kNaN;
				checkRgbGuardRejects(matNanKg, r0, g0, b0, 6.0, -6.0, L"NaN kg must return false and preserve RGB");

				// 4. +Inf overflow from finite inputs (nr = r + kR * dCr overflows double max to +Inf!)
				checkRgbGuardRejects(bt709, 1e308, 512.0, 512.0, 0.0, 1e308,
					L"+Inf overflow must return false and preserve RGB");

				// ---- NEGATIVE CONTROLS (RGB Guard):
				// (i) Valid BT.709 matrix with normal deltas succeeds & modifies RGB cleanly without NonFiniteClosure!
				{
					StandInRegistry okSi;
					double outR = 0.0, outG = 0.0, outB = 0.0;
					Assert::IsTrue(bt709.ApplySignedChromaDeltaToRgb(r0, g0, b0, 10.0, -8.0, outR, outG, outB, &okSi),
						L"Negative control: valid BT.709 RGB delta must succeed");
					Assert::IsFalse(okSi.IsActive(StandInKind::NonFiniteClosure),
						L"Negative control: valid BT.709 RGB delta must not engage NonFiniteClosure");
					Assert::IsTrue(std::fabs(outR - r0) > 5.0 && std::fabs(outB - b0) > 5.0,
						L"Negative control: valid RGB delta must materially update R and B");
				}
				// (ii) Un-guarded naive division on kg==0 and +Inf overflow produces non-finite IEEE-754 values!
				{
					volatile double vKgZero = 0.0;
					const double naiveG = g0 - (bt709.kr * 2.0 * (1.0 - bt709.kr) / vKgZero) * (-8.0);
					Assert::IsFalse(std::isfinite(naiveG), L"Negative control: un-guarded kg==0 must produce Inf/NaN");

					volatile double vHuge = 1e308;
					const double naiveOverflow = vHuge + 2.0 * (1.0 - bt709.kr) * vHuge;
					Assert::IsFalse(std::isfinite(naiveOverflow), L"Negative control: un-guarded overflow must produce +Inf");
				}
			}
		}
	};
} // namespace TdV2GpuWarpNs
} // namespace Tests
