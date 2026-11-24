// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 & TD-S5-OPT Adversarial WARP GPU Tests (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5, s3.4):
// Independent Red-Team verification on real D3D11 WARP (100% deterministic, ZERO silent skips,
// mandatory in-test negative controls per Rule 3 & Rule 8):
//   1. GpuMatchesCpuReferenceWithin1e3CodeAcrossPanObjectLetterboxStrictAndCell
//   2. GpuChromaDeltaApplyMatchesCpuTdS4OnYuvAndRgbPreservingBtbWtw
//   3. ScratchArenaShortageAndAbsentInputsFailVisiblyWithoutSilentFallback
//   4. ZeroNanInfPropagationOnGpuShadersAcrossRefMotionAndGrainInputs
//   5. OffAndShadowModesByteIdenticalAndOffIssuesZeroGpuDispatches
//   6. M1ReAuditCleanupDeliveredRefNanMalformedSplatAndRgbGuards
//   7. RealTimeCompactBandPathPreservesBtbWtwGrainFocusAndCutsVram
//   8. ProcessFrameGpuZeroCopyAndCachedChromaReuseMatchCpuWrapper
//   9. GpuSubtleFaceMicroMotionMatchesCpuWithoutWaxSmearing
//  10. PrepackedMotionFixedSlotsPrevNullAndTextureDimensionGuards
//  11. ProcessFrameGpuRejectsInputOutputTextureAliasingVisibly
//  12. GpuPass1FaceMaskForcesExactZeroDeltaOnCpuUploadAndZeroCopy
//  13. GpuCutSeedZeroHygieneZeroCopyRefValidationSingleNanClosureAndNon16PillarboxGeometry
//  14. SigmaDeltaWarpShaderQ9bAboveBlackNeverDipsBtbWhileTrueBtbPreserved
//  15. GpuMatchesCpuOnPrevPeak2CodecModelPbInvalidMotionPocsSkipAndSkyBandingParsevalBound
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/ScratchArena.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/output/dither/CustomHookParameters.h>
#include <VP-V/modules/output/dither/VpShaderInclude.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Shaders.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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

	static std::string RepoRootFromFile()
	{
		std::string path = __FILE__;
		std::replace(path.begin(), path.end(), '/', '\\');
		const size_t pos = path.rfind("\\src\\");
		Assert::IsTrue(pos != std::string::npos, L"Could not locate \\src\\ in __FILE__ path");
		path.resize(pos + 1);
		return path;
	}

	// Exact IEEE-754 binary16 (FP16) quantization & overflow model for Rule-3 precision negative controls:
	// 10-bit significand (ulp = 2^(e-10) for normal values), max finite = 65504.0, overflows to +/-Inf beyond.
	static double SimulateFp16Quantize(double v)
	{
		if (!std::isfinite(v))
			return v;
		const double av = std::fabs(v);
		if (av > 65504.0)
			return (v >= 0.0) ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
		if (av == 0.0)
			return 0.0;
		const int exp = (std::max)(-14, static_cast<int>(std::floor(std::log2(av))));
		const double step = std::ldexp(1.0, exp - 10);
		return std::nearbyint(v / step) * step;
	}

	// Exact SC-05 half-res L-band operator (2x2 area downsample -> HalfSigma(kSigmaBase) separable Gaussian -> bilinear upsample,
	// mirroring kPass1HalfH/kPass1HalfV/td_sample_half_ld3 in TdV2Shaders.h) to derive SC-05 transfer error bounds from physics.
	static TdV2Plane ComputeSc05HalfResLBand(const TdV2Plane& src, const ActiveRect& active)
	{
		const int w = src.w;
		const int h = src.h;
		TdV2Plane out(w, h, 0.0);
		const ActiveRect ar = active.ClampedTo(w, h);
		if (ar.Empty())
			return out;
		const int wHalf = (w + 1) / 2;
		const int hHalf = (h + 1) / 2;
		const int hax0 = (std::max)(0, (std::min)(wHalf, ar.x0 / 2));
		const int hay0 = (std::max)(0, (std::min)(hHalf, ar.y0 / 2));
		const int hax1 = (std::max)(hax0, (std::min)(wHalf, (ar.x1 + 1) / 2));
		const int hay1 = (std::max)(hay0, (std::min)(hHalf, (ar.y1 + 1) / 2));
		if (hax1 <= hax0 || hay1 <= hay0)
			return out;

		auto clampInt = [](int v, int lo, int hi) { return (std::max)(lo, (std::min)(hi, v)); };
		TdV2Plane halfIn(wHalf, hHalf, 0.0);
		for (int hy = hay0; hy < hay1; ++hy)
		{
			const int sy0 = clampInt(2 * hy,     ar.y0, ar.y1 - 1);
			const int sy1 = clampInt(2 * hy + 1, ar.y0, ar.y1 - 1);
			for (int hx = hax0; hx < hax1; ++hx)
			{
				const int sx0 = clampInt(2 * hx,     ar.x0, ar.x1 - 1);
				const int sx1 = clampInt(2 * hx + 1, ar.x0, ar.x1 - 1);
				halfIn.at(hx, hy) = 0.25 * (src.at(sx0, sy0) + src.at(sx1, sy0) + src.at(sx0, sy1) + src.at(sx1, sy1));
			}
		}

		const double hsBase = VpUnit::BandFromBase::HalfSigma(VpUnit::BandFromBase::kSigmaBase);
		const auto taps = VpUnit::BandFromBase::Taps<double>(hsBase, 3.0, true, 0.0);
		const int rad = static_cast<int>(taps.size() / 2);

		TdV2Plane halfTmp(wHalf, hHalf, 0.0);
		for (int hy = hay0; hy < hay1; ++hy)
		{
			for (int hx = hax0; hx < hax1; ++hx)
			{
				double acc = 0.0;
				for (int k = -rad; k <= rad; ++k)
					acc += taps[static_cast<size_t>(k + rad)] * halfIn.at(clampInt(hx + k, hax0, hax1 - 1), hy);
				halfTmp.at(hx, hy) = acc;
			}
		}
		TdV2Plane halfL(wHalf, hHalf, 0.0);
		for (int hy = hay0; hy < hay1; ++hy)
		{
			for (int hx = hax0; hx < hax1; ++hx)
			{
				double acc = 0.0;
				for (int k = -rad; k <= rad; ++k)
					acc += taps[static_cast<size_t>(k + rad)] * halfTmp.at(hx, clampInt(hy + k, hay0, hay1 - 1));
				halfL.at(hx, hy) = acc;
			}
		}

		for (int y = ar.y0; y < ar.y1; ++y)
		{
			const double uy = (std::max)(static_cast<double>(hay0), (std::min)(static_cast<double>(hay1 - 1), (static_cast<double>(y) + 0.5) * 0.5 - 0.5));
			const int iy0 = clampInt(static_cast<int>(std::floor(uy)), hay0, hay1 - 1);
			const int iy1 = clampInt(iy0 + 1, hay0, hay1 - 1);
			const double fy = uy - static_cast<double>(iy0);
			for (int x = ar.x0; x < ar.x1; ++x)
			{
				const double ux = (std::max)(static_cast<double>(hax0), (std::min)(static_cast<double>(hax1 - 1), (static_cast<double>(x) + 0.5) * 0.5 - 0.5));
				const int ix0 = clampInt(static_cast<int>(std::floor(ux)), hax0, hax1 - 1);
				const int ix1 = clampInt(ix0 + 1, hax0, hax1 - 1);
				const double fx = ux - static_cast<double>(ix0);
				const double v00 = halfL.at(ix0, iy0), v10 = halfL.at(ix1, iy0);
				const double v01 = halfL.at(ix0, iy1), v11 = halfL.at(ix1, iy1);
				out.at(x, y) = (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy;
			}
		}
		return out;
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

	static double PlaneMse(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& r)
	{
		if (a.w != b.w || a.h != b.h)
			return std::numeric_limits<double>::infinity();
		double sumSq = 0.0;
		size_t n = 0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double d = a.at(x, y) - b.at(x, y);
				sumSq += d * d;
				++n;
			}
		}
		return n > 0 ? (sumSq / static_cast<double>(n)) : 0.0;
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
					// Negative control: verify un-guarded FP32 overflows to +Inf! (volatile inputs avoid MSVC C4756 constant-fold warning)
					volatile float vNearMax = kNearMaxF;
					volatile float vBigDelta = kBigDeltaF;
					volatile float vKR709 = static_cast<float>(2.0 * (1.0 - YCbCrMatrixSpec::Bt709().kr));
					const float vOverflow = vNearMax + vKR709 * vBigDelta;
					Assert::IsFalse(std::isfinite(vOverflow),
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

				// D3b (M-NEW-1): RGB non-finite deliveredR at active-delta pixel, zero-delta pixel, AND u_all_zero_delta!
				{
					// Negative control (Rule 3): un-guarded RGB addition/passthrough propagates NaN/+Inf.
					volatile float vNanR = static_cast<float>(kNaN);
					volatile float vZeroDelta = 0.0f;
					volatile float vActiveDelta = 5.0f;
					Assert::IsFalse(std::isfinite(static_cast<float>(vNanR + vZeroDelta)),
						L"Negative control: un-guarded NaN deliveredR + 0.0 must remain NaN");
					Assert::IsFalse(std::isfinite(static_cast<float>(vNanR + vActiveDelta)),
						L"Negative control: un-guarded NaN deliveredR + active delta must remain NaN");

					// Case 1: Active-delta R(8,12)=NaN, zero-delta R(48,24)=+Inf, G(12,16)=NaN, B(14,18)=-Inf,
					//         and letterbox bar (outside ActiveRect y=1<4) R(10,1)=NaN on mixed delta plane (LOW-3).
					TdV2Plane nanDelivR = delivR;
					TdV2Plane nanDelivG = delivG;
					TdV2Plane nanDelivB = delivB;
					nanDelivR.at(8, 12) = kNaN;
					nanDelivR.at(48, 24) = kPInf;
					nanDelivG.at(12, 16) = kNaN;
					nanDelivB.at(14, 18) = -kPInf;
					nanDelivR.at(10, 1) = kNaN;

					ChromaGpuApplyInput rgbNanDeliv;
					rgbNanDeliv.lumaW = lumaW;
					rgbNanDeliv.lumaH = lumaH;
					rgbNanDeliv.deliveredFormat = DeliveredChromaFormat::Rgb444;
					rgbNanDeliv.codedSubsampling = ChromaSubsampling::Yuv420;
					rgbNanDeliv.siting = SitingSpec::Left();
					rgbNanDeliv.activeRect = active;
					rgbNanDeliv.matrix = YCbCrMatrixSpec::Bt709();
					rgbNanDeliv.deliveredR = nanDelivR;
					rgbNanDeliv.deliveredG = nanDelivG;
					rgbNanDeliv.deliveredB = nanDelivB;
					rgbNanDeliv.codedDeltaCb = codedDeltaCb;
					rgbNanDeliv.codedDeltaCr = codedDeltaCr;

					StandInRegistry siRgbNan;
					const ChromaGpuApplyResult rRgbNan = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbNanDeliv, &siRgbNan);
					Assert::IsTrue(rRgbNan.succeeded, L"M-NEW-1 RGB non-finite deliveredRGB must complete cleanly");
					Assert::AreEqual(1, rRgbNan.dispatchCount, L"M-NEW-1 RGB non-finite deliveredRGB must issue 1 dispatch");
					AssertAllFinitePlane(rRgbNan.outR, L"M-NEW-1 RGB non-finite deliveredRGB must not propagate NaN into outR");
					AssertAllFinitePlane(rRgbNan.outG, L"M-NEW-1 RGB non-finite deliveredRGB must keep outG finite");
					AssertAllFinitePlane(rRgbNan.outB, L"M-NEW-1 RGB non-finite deliveredRGB must keep outB finite");
					AssertAllFinitePlane(rRgbNan.deliveredDeltaCb, L"M-NEW-1 RGB deliveredDeltaCb must be finite");
					AssertAllFinitePlane(rRgbNan.deliveredDeltaCr, L"M-NEW-1 RGB deliveredDeltaCr must be finite");

					// Active-delta pixel (8, 12): non-finite R closed to 0.0, finite G/B preserved, deliveredDelta zeroed.
					Assert::AreEqual(0.0, rRgbNan.outR.at(8, 12), L"M-NEW-1 RGB active-delta NaN deliveredR must close to 0.0");
					Assert::AreEqual(delivG.at(8, 12), rRgbNan.outG.at(8, 12), 1e-5, L"M-NEW-1 RGB active-delta finite G must be preserved");
					Assert::AreEqual(delivB.at(8, 12), rRgbNan.outB.at(8, 12), 1e-5, L"M-NEW-1 RGB active-delta finite B must be preserved");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCb.at(8, 12), L"M-NEW-1 RGB active-delta NaN pixel must zero deliveredDeltaCb");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCr.at(8, 12), L"M-NEW-1 RGB active-delta NaN pixel must zero deliveredDeltaCr");

					// Zero-delta region pixel (48, 24): non-finite +Inf R closed to 0.0, finite G/B preserved, delta zeroed.
					Assert::AreEqual(0.0, rRgbNan.outR.at(48, 24), L"M-NEW-1 RGB zero-delta +Inf deliveredR must close to 0.0");
					Assert::AreEqual(delivG.at(48, 24), rRgbNan.outG.at(48, 24), 1e-5, L"M-NEW-1 RGB zero-delta finite G must be preserved");
					Assert::AreEqual(delivB.at(48, 24), rRgbNan.outB.at(48, 24), 1e-5, L"M-NEW-1 RGB zero-delta finite B must be preserved");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCb.at(48, 24), L"M-NEW-1 RGB zero-delta +Inf pixel must zero deliveredDeltaCb");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCr.at(48, 24), L"M-NEW-1 RGB zero-delta +Inf pixel must zero deliveredDeltaCr");

					// Full channel symmetry (LOW-3): deliveredG(12, 16)=NaN and deliveredB(14, 18)=-Inf closed cleanly.
					Assert::AreEqual(delivR.at(12, 16), rRgbNan.outR.at(12, 16), 1e-5, L"LOW-3 RGB NaN deliveredG must preserve finite R");
					Assert::AreEqual(0.0, rRgbNan.outG.at(12, 16), L"LOW-3 RGB NaN deliveredG must close G to 0.0");
					Assert::AreEqual(delivB.at(12, 16), rRgbNan.outB.at(12, 16), 1e-5, L"LOW-3 RGB NaN deliveredG must preserve finite B");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCb.at(12, 16), L"LOW-3 RGB NaN deliveredG must zero deliveredDeltaCb");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCr.at(12, 16), L"LOW-3 RGB NaN deliveredG must zero deliveredDeltaCr");

					Assert::AreEqual(delivR.at(14, 18), rRgbNan.outR.at(14, 18), 1e-5, L"LOW-3 RGB -Inf deliveredB must preserve finite R");
					Assert::AreEqual(delivG.at(14, 18), rRgbNan.outG.at(14, 18), 1e-5, L"LOW-3 RGB -Inf deliveredB must preserve finite G");
					Assert::AreEqual(0.0, rRgbNan.outB.at(14, 18), L"LOW-3 RGB -Inf deliveredB must close B to 0.0");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCb.at(14, 18), L"LOW-3 RGB -Inf deliveredB must zero deliveredDeltaCb");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCr.at(14, 18), L"LOW-3 RGB -Inf deliveredB must zero deliveredDeltaCr");

					// Letterbox bar pixel outside ActiveRect (10, 1): NaN deliveredR closed cleanly to 0.0 with finite G/B preserved.
					Assert::AreEqual(0.0, rRgbNan.outR.at(10, 1), L"LOW-3 RGB letterbox bar NaN deliveredR must close to 0.0");
					Assert::AreEqual(delivG.at(10, 1), rRgbNan.outG.at(10, 1), 1e-5, L"LOW-3 RGB letterbox bar finite G must be preserved");
					Assert::AreEqual(delivB.at(10, 1), rRgbNan.outB.at(10, 1), 1e-5, L"LOW-3 RGB letterbox bar finite B must be preserved");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCb.at(10, 1), L"LOW-3 RGB letterbox bar NaN must zero deliveredDeltaCb");
					Assert::AreEqual(0.0, rRgbNan.deliveredDeltaCr.at(10, 1), L"LOW-3 RGB letterbox bar NaN must zero deliveredDeltaCr");

					Assert::AreEqual(static_cast<size_t>(5u), rRgbNan.nonFinitePixels,
						L"M-NEW-1/LOW-3 RGB mixed-delta non-finite deliveredRGB + bar must report exact nonFinitePixels == 5");
					Assert::IsTrue(siRgbNan.IsActive(StandInKind::NonFiniteClosure),
						L"M-NEW-1/LOW-3 RGB mixed-delta non-finite deliveredRGB must engage NonFiniteClosure");

					// Case 2: u_all_zero_delta == 1.0 (entire codedDeltaCb/Cr plane strictly 0.0) with NaN deliveredR at (20, 20).
					TdV2Plane nanDelivRAllZero = delivR;
					nanDelivRAllZero.at(20, 20) = kNaN;
					ChromaGpuApplyInput rgbAllZeroNan = rgbNanDeliv;
					rgbAllZeroNan.deliveredR = nanDelivRAllZero;
					rgbAllZeroNan.deliveredG = delivG;
					rgbAllZeroNan.deliveredB = delivB;
					rgbAllZeroNan.codedDeltaCb = TdV2Plane(cW, cH, 0.0);
					rgbAllZeroNan.codedDeltaCr = TdV2Plane(cW, cH, 0.0);

					StandInRegistry siRgbZeroNan;
					const ChromaGpuApplyResult rRgbZeroNan = gpuGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbAllZeroNan, &siRgbZeroNan);
					Assert::IsTrue(rRgbZeroNan.succeeded, L"M-NEW-1 RGB u_all_zero_delta non-finite deliveredR must complete cleanly");
					Assert::AreEqual(1, rRgbZeroNan.dispatchCount, L"M-NEW-1 RGB u_all_zero_delta must issue 1 dispatch");
					AssertAllFinitePlane(rRgbZeroNan.outR, L"M-NEW-1 RGB u_all_zero_delta outR must be 100% finite");
					AssertAllFinitePlane(rRgbZeroNan.outG, L"M-NEW-1 RGB u_all_zero_delta outG must be 100% finite");
					AssertAllFinitePlane(rRgbZeroNan.outB, L"M-NEW-1 RGB u_all_zero_delta outB must be 100% finite");
					Assert::AreEqual(0.0, rRgbZeroNan.outR.at(20, 20), L"M-NEW-1 RGB u_all_zero_delta NaN deliveredR must close to 0.0");
					Assert::AreEqual(delivG.at(20, 20), rRgbZeroNan.outG.at(20, 20), 1e-5, L"M-NEW-1 RGB u_all_zero_delta finite G must be preserved");
					Assert::AreEqual(delivB.at(20, 20), rRgbZeroNan.outB.at(20, 20), 1e-5, L"M-NEW-1 RGB u_all_zero_delta finite B must be preserved");
					Assert::AreEqual(0.0, rRgbZeroNan.deliveredDeltaCb.at(20, 20), L"M-NEW-1 RGB u_all_zero_delta must zero deliveredDeltaCb");
					Assert::AreEqual(0.0, rRgbZeroNan.deliveredDeltaCr.at(20, 20), L"M-NEW-1 RGB u_all_zero_delta must zero deliveredDeltaCr");
					Assert::AreEqual(static_cast<size_t>(1u), rRgbZeroNan.nonFinitePixels,
						L"M-NEW-1 RGB u_all_zero_delta NaN deliveredR must report nonFinitePixels == 1");
					Assert::IsTrue(siRgbZeroNan.IsActive(StandInKind::NonFiniteClosure),
						L"M-NEW-1 RGB u_all_zero_delta NaN deliveredR must engage NonFiniteClosure");
					// Unaffected finite neighbor stays bit-exact passthrough under u_all_zero_delta.
					Assert::AreEqual(delivR.at(10, 10), rRgbZeroNan.outR.at(10, 10), L"M-NEW-1 RGB u_all_zero_delta finite pixel R must pass through bit-exact");
					Assert::AreEqual(delivG.at(10, 10), rRgbZeroNan.outG.at(10, 10), L"M-NEW-1 RGB u_all_zero_delta finite pixel G must pass through bit-exact");
					Assert::AreEqual(delivB.at(10, 10), rRgbZeroNan.outB.at(10, 10), L"M-NEW-1 RGB u_all_zero_delta finite pixel B must pass through bit-exact");
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

				Assert::IsTrue(gpuRes.succeeded, L"H3: NaN stepDelta/zeroFrac must fail closed cleanly without GLSL error");
				Assert::IsTrue(cpuSi.IsActive(StandInKind::A2StatsInvalid) && gpuSi.IsActive(StandInKind::A2StatsInvalid),
					L"H3: NaN stepDelta/zeroFrac with hasStepPrior=true must engage A2StatsInvalid on CPU & GPU");
				Assert::IsTrue(cpuSi.IsActive(StandInKind::NonFiniteClosure) && gpuSi.IsActive(StandInKind::NonFiniteClosure),
					L"H3: NaN stepDelta/zeroFrac with hasStepPrior=true must engage NonFiniteClosure on CPU & GPU");
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

			// ---- Case J (MEDIUM-6): GPU Pass 3B (transformIdentified8x8) non-finite pred_clean / cur_clean closes
			//      entire 8x8 block to delta == 0.0 (NEVER projecting against 0.0!) and increments nonFiniteCells! ----
			{
				struct LocalTex
				{
					pl_gpu gpu = nullptr;
					pl_tex tex = nullptr;
					explicit LocalTex(pl_gpu g) : gpu(g) {}
					~LocalTex() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				};
				const pl_fmt fmtR16    = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R16);
				const pl_fmt fmtR32    = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
				const pl_fmt fmtRG32   = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
				const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
				Assert::IsNotNull(fmtR16); Assert::IsNotNull(fmtR32); Assert::IsNotNull(fmtRG32); Assert::IsNotNull(fmtRGBA32);

				// (J1) Direct WARP dispatch of Shaders::kPass3bDctPocs with single NaN in pred_clean inside block [0..8)^2
				//      while neighbor block [8..16)^2 is 100% finite (positive control):
				const int pw = 16, ph = 16;
				LocalTex dInTex(ctx.gpu), curTex(ctx.gpu), predTex(ctx.gpu), focusTex(ctx.gpu), flagsTex(ctx.gpu), dOutTex(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, dInTex.tex, pw, ph, fmtRG32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, curTex.tex, pw, ph, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, predTex.tex, pw, ph, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, focusTex.tex, 1, 1, fmtRGBA32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, flagsTex.tex, pw, ph, fmtR16, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, dOutTex.tex, pw, ph, fmtRG32, true, true, false, true));

				std::vector<float> dInBuf(static_cast<size_t>(pw * ph) * 2u, 0.0f);
				std::vector<float> curBuf(static_cast<size_t>(pw * ph), 500.0f);
				std::vector<float> predBuf(static_cast<size_t>(pw * ph), 500.0f);
				std::vector<uint16_t> flagsBuf(static_cast<size_t>(pw * ph), static_cast<uint16_t>(1u));
				for (int i = 0; i < pw * ph; ++i)
					dInBuf[static_cast<size_t>(i) * 2u] = 6.0f; // active non-zero delta inside [-theta, +theta]
				predBuf[3 * pw + 3] = std::numeric_limits<float>::quiet_NaN(); // single NaN in pred_clean block (0,0)!
				const float focusZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, dInTex.tex, dInBuf.data()));
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, curTex.tex, curBuf.data()));
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, predTex.tex, predBuf.data()));
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, focusTex.tex, focusZero));
				{
					pl_tex_transfer_params tp{};
					tp.tex = flagsTex.tex;
					tp.ptr = flagsBuf.data();
					Assert::IsTrue(pl_tex_upload(ctx.gpu, &tp));
				}

				const std::string p3bHdr = Detail::StaticGeomDefines(pw, ph, 1, 1, 16)
					+ Shaders::BuildDctBasisHeader() + Shaders::kCoreHelpers + Shaders::kPass3bDctHeader;
				pl_shader_desc p3bD[6] = {
					VpvGpu::Sampled("delta_in", dInTex.tex),
					VpvGpu::Sampled("cur_clean", curTex.tex),
					VpvGpu::Sampled("pred_clean", predTex.tex),
					VpvGpu::Sampled("cell_focus", focusTex.tex),
					VpvGpu::Sampled("ref_flags", flagsTex.tex),
					VpvGpu::Storage("delta_out", dOutTex.tex)
				};
				float uBx0 = 0.0f, uBy0 = 0.0f, uBx1 = 16.0f, uBy1 = 16.0f;
				float uIters = 2.0f, uResMode = 0.0f, uStep = 24.0f, uPhi = static_cast<float>(kDeadZonePhiInter), uPeak = 1023.0f;
				float uHasExplicitPb = 1.0f;
				pl_shader_var p3bV[10] = {
					VpvGpu::Var("u_bx0", &uBx0), VpvGpu::Var("u_by0", &uBy0),
					VpvGpu::Var("u_bx1", &uBx1), VpvGpu::Var("u_by1", &uBy1),
					VpvGpu::Var("u_pocs_iters", &uIters), VpvGpu::Var("u_residual_mode", &uResMode),
					VpvGpu::Var("u_step_delta", &uStep), VpvGpu::Var("u_deadzone_phi", &uPhi),
					VpvGpu::Var("u_peak_code", &uPeak), VpvGpu::Var("u_has_explicit_pb", &uHasExplicitPb)
				};
				Assert::IsTrue(VpvGpu::Pass(ctx.dispatch, "td_v2_test_m6_p3b_nan_pred", p3bHdr, Shaders::kPass3bDctPocs,
				                            p3bD, 6, p3bV, 10, 2, 2, (5u * 64u) * sizeof(float), 8, 8),
					L"MEDIUM-6: direct kPass3bDctPocs dispatch failed");
				std::vector<float> dOutBuf(static_cast<size_t>(pw * ph) * 2u, -99.0f);
				Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, dOutTex.tex, dOutBuf.data()));
				// Block [0..8)x[0..8) with NaN pred_clean MUST close ALL 64 pixels to exact delta==0.0, flag==2.0!
				for (int y = 0; y < 8; ++y)
				{
					for (int x = 0; x < 8; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * pw + x) * 2u;
						Assert::AreEqual(0.0f, dOutBuf[idx], L"MEDIUM-6: NaN pred_clean block must close delta to exact 0.0");
						Assert::AreEqual(2.0f, dOutBuf[idx + 1u], L"MEDIUM-6: NaN pred_clean block must set nonFinite flag 2.0");
					}
				}
				// Positive control: neighbor valid block [8..16)x[0..8) keeps active non-zero delta (> 1.0)!
				Assert::IsTrue(std::fabs(dOutBuf[(0u * pw + 8u) * 2u]) > 1.0f,
					L"MEDIUM-6 positive control: valid neighbor 8x8 block must retain non-zero delta");
				Assert::AreEqual(0.0f, dOutBuf[(0u * pw + 8u) * 2u + 1u],
					L"MEDIUM-6 positive control: valid neighbor 8x8 block must not set nonFinite flag");

				// (J2) End-to-end ProcessFrameGpu with transformIdentified8x8=true + single NaN in curCleanTex
				//      inside block [24..32)x[24..32): entire 8x8 block closes to delta==0.0 & increments nonFiniteCells!
				const int ew = 64, eh = 64;
				const ActiveRect eFull = ActiveRect::Full(ew, eh);
				const CellMotionField eStill = CellMotionField::Uniform(4, 4, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const TdV2Plane eMaster = RenderScene(ew, eh, 0.0, 0.0);
				const TdV2Plane eNoisy  = AddCodingDamage(eMaster, 8.0, 4410u, 0.0, eFull);
				TdV2Plane eCleanNan = eNoisy;
				eCleanNan.at(26, 26) = kNaN;

				LocalTex eRec(ctx.gpu), eClean(ctx.gpu), eRef(ctx.gpu), eOutY(ctx.gpu), eOutClean(ctx.gpu), eOutDelta(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, eRec.tex, ew, eh, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, eClean.tex, ew, eh, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, eRef.tex, ew, eh, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, eOutY.tex, ew, eh, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, eOutClean.tex, ew, eh, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, eOutDelta.tex, ew, eh, fmtRG32, true, true, false, true));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, eRec.tex, eNoisy));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, eClean.tex, eCleanNan));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, eRef.tex, eMaster));

				for (ResidualMode rm : { ResidualMode::Cell, ResidualMode::Strict })
				{
					Config c8 = cfg;
					c8.residualMode = rm;
					GpuEngineGuard g8(ctx.gpu, c8);
					GpuFrameInput z8In;
					z8In.seq = (rm == ResidualMode::Strict) ? 4412 : 4411;
					z8In.w = ew; z8In.h = eh;
					z8In.receivedYTex = eRec.tex;
					z8In.cleanYTex = eClean.tex;
					z8In.hasCleanSplit = true;
					z8In.activeRect = eFull;
					z8In.prevPeak = { eRef.tex, &eStill, false };
					z8In.nextPeak = { eRef.tex, &eStill, false };
					z8In.hasStepPrior = true;
					z8In.estimatedStepDelta = 24.0;
					z8In.deadZonePhi = kDeadZonePhiInter;
					z8In.zeroCoeffFraction = 0.35;
					z8In.transformIdentified8x8 = true;

					GpuFrameOutput z8Out;
					z8Out.outputYTex = eOutY.tex;
					z8Out.outputCleanTex = eOutClean.tex;
					z8Out.tdDeltaTex = eOutDelta.tex;
					z8Out.readbackCellDiagnostics = true;
					StandInRegistry z8Si;
					Assert::IsTrue(g8.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, z8In, z8Out, nullptr, &z8Si),
						L"MEDIUM-6: ProcessFrameGpu 8x8 with NaN curClean failed");
					Assert::IsTrue(z8Out.nonFiniteCells >= 1u, L"MEDIUM-6: 8x8 NaN block must increment nonFiniteCells >= 1");
					Assert::IsTrue(z8Si.IsActive(StandInKind::NonFiniteClosure),
						L"MEDIUM-6: 8x8 NaN block must engage NonFiniteClosure");
					TdV2Plane dlY, dlDelta;
					bool bnd = false;
					Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, eOutY.tex, ew, eh, dlY));
					Assert::IsTrue(Detail::DownloadDeltaRG32F(ctx.gpu, eOutDelta.tex, ew, eh, dlDelta, bnd));
					AssertAllFinitePlane(dlY, L"MEDIUM-6: 8x8 NaN block outputY must be 100% finite");
					AssertAllFinitePlane(dlDelta, L"MEDIUM-6: 8x8 NaN block tdDelta must be 100% finite");
					Assert::AreEqual(0.0, MaxAbsPlane(dlDelta, ActiveRect{ 24, 24, 32, 32 }),
						L"MEDIUM-6: entire 8x8 block containing NaN must close to exact delta == 0.0");
				}
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

			// ---- MEDIUM-2: GPU Carry Lifecycle Parity With CPU (Mode::On->Off->On & Non-Consecutive seq Jump!) ----
			{
				ComplexSceneBundle c0 = BuildComplexFrame(0, 1.5, 0.5, false);
				ComplexSceneBundle c1 = BuildComplexFrame(1, 1.5, 0.5, false);
				FrameProcessInput seedIn = MakeFrameInput(c0, 10, /*withPeaks=*/false, false);
				FrameProcessInput nextConsec = MakeFrameInput(c1, 11, /*withPeaks=*/true, false);
				FrameProcessInput nextJump   = MakeFrameInput(c1, 15, /*withPeaks=*/true, false);

				Config cfgOff = cfgOn;
				cfgOff.mode = Mode::Off;

				// (1) Positive control: consecutive seq 10 -> 11 uses carry on BOTH CPU and GPU!
				{
					TdV2ReferenceEngine cpuConsec(cfgOn);
					GpuEngineGuard gpuConsec(ctx.gpu, cfgOn);
					cpuConsec.ProcessFrame(seedIn, nullptr);
					const GpuProcessResult gSeed = gpuConsec.engine.ProcessFrame(ctx.gpu, ctx.dispatch, seedIn, nullptr, nullptr);
					Assert::IsTrue(gSeed.succeeded && gpuConsec.engine.HasCarry());
					const FrameProcessResult cR = cpuConsec.ProcessFrame(nextConsec, nullptr);
					const GpuProcessResult gR = gpuConsec.engine.ProcessFrame(ctx.gpu, ctx.dispatch, nextConsec, nullptr, nullptr);
					Assert::IsTrue(cR.carryUsed && !cR.carryReset, L"MEDIUM-2 positive control: CPU seq 10->11 must use carry");
					Assert::IsTrue(gR.succeeded && gR.carryUsed && !gR.carryReset,
						L"MEDIUM-2 positive control: GPU seq 10->11 must use carry without reset");
				}

				// (2) Mode::On -> Mode::Off -> Mode::On resets carry on BOTH CPU and GPU (no stale pre-Off carry)!
				{
					TdV2ReferenceEngine cpuToggle(cfgOn);
					GpuEngineGuard gpuToggle(ctx.gpu, cfgOn);
					cpuToggle.ProcessFrame(seedIn, nullptr);
					const GpuProcessResult gSeed = gpuToggle.engine.ProcessFrame(ctx.gpu, ctx.dispatch, seedIn, nullptr, nullptr);
					Assert::IsTrue(gSeed.succeeded && gpuToggle.engine.HasCarry());

					cpuToggle.SetConfig(cfgOff);
					gpuToggle.engine.SetConfig(cfgOff);
					Assert::IsFalse(cpuToggle.HasCarry(), L"MEDIUM-2: CPU SetConfig(Off) must reset carry");
					Assert::IsFalse(gpuToggle.engine.HasCarry(), L"MEDIUM-2: GPU SetConfig(Off) must reset carry");
					const GpuProcessResult gOff = gpuToggle.engine.ProcessFrame(ctx.gpu, ctx.dispatch, nextConsec, nullptr, nullptr);
					Assert::IsTrue(gOff.succeeded && gOff.dispatchCount == 0);

					cpuToggle.SetConfig(cfgOn);
					gpuToggle.engine.SetConfig(cfgOn);
					const FrameProcessResult cAfterOff = cpuToggle.ProcessFrame(nextConsec, nullptr);
					const GpuProcessResult gAfterOff = gpuToggle.engine.ProcessFrame(ctx.gpu, ctx.dispatch, nextConsec, nullptr, nullptr);
					Assert::IsFalse(cAfterOff.carryUsed, L"MEDIUM-2: CPU must NOT use stale pre-Off carry");
					Assert::IsTrue(gAfterOff.succeeded && !gAfterOff.carryUsed,
						L"MEDIUM-2: GPU must NOT use stale pre-Off carry after On->Off->On");
					AssertCpuGpuFrameParity(cAfterOff, gAfterOff, c1.curClean, c1.receivedY, c1.active, "M2-OnOffOn");
				}

				// (3) Non-consecutive seq jump (seq 10 -> 15) resets carry on BOTH CPU and GPU (ProcessFrame & ProcessFrameGpu)!
				{
					TdV2ReferenceEngine cpuJump(cfgOn);
					GpuEngineGuard gpuJump(ctx.gpu, cfgOn);
					cpuJump.ProcessFrame(seedIn, nullptr);
					const GpuProcessResult gSeed = gpuJump.engine.ProcessFrame(ctx.gpu, ctx.dispatch, seedIn, nullptr, nullptr);
					Assert::IsTrue(gSeed.succeeded && gpuJump.engine.HasCarry());

					const FrameProcessResult cJ = cpuJump.ProcessFrame(nextJump, nullptr);
					const GpuProcessResult gJ = gpuJump.engine.ProcessFrame(ctx.gpu, ctx.dispatch, nextJump, nullptr, nullptr);
					Assert::IsTrue(cJ.carryReset && !cJ.carryUsed, L"MEDIUM-2: CPU seq jump 10->15 must reset carry");
					Assert::IsTrue(gJ.succeeded && gJ.carryReset && !gJ.carryUsed,
						L"MEDIUM-2: GPU ProcessFrame seq jump 10->15 must reset carry");
					AssertCpuGpuFrameParity(cJ, gJ, c1.curClean, c1.receivedY, c1.active, "M2-SeqJump");

					// Zero-copy ProcessFrameGpu seq jump (10 -> 15) parity:
					struct LocalTex
					{
						pl_gpu gpu = nullptr;
						pl_tex tex = nullptr;
						explicit LocalTex(pl_gpu g) : gpu(g) {}
						~LocalTex() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
					};
					const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
					LocalTex r0(ctx.gpu), r1(ctx.gpu), p1(ctx.gpu), n1(ctx.gpu), outY(ctx.gpu);
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, r0.tex, c0.w, c0.h, fmtR32, true, false, true, false));
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, r1.tex, c1.w, c1.h, fmtR32, true, false, true, false));
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, p1.tex, c1.w, c1.h, fmtR32, true, false, true, false));
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, n1.tex, c1.w, c1.h, fmtR32, true, false, true, false));
					Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outY.tex, c1.w, c1.h, fmtR32, true, true, false, true));
					Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, r0.tex, c0.curClean));
					Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, r1.tex, c1.curClean));
					Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, p1.tex, c1.prevClean));
					Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, n1.tex, c1.nextClean));

					GpuEngineGuard zcJump(ctx.gpu, cfgOn);
					GpuFrameInput zi0;
					zi0.seq = 10; zi0.w = c0.w; zi0.h = c0.h;
					zi0.receivedYTex = r0.tex; zi0.cleanYTex = r0.tex; zi0.hasCleanSplit = true; zi0.activeRect = c0.active;
					GpuFrameOutput zo0; zo0.outputYTex = outY.tex;
					Assert::IsTrue(zcJump.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zi0, zo0, nullptr, nullptr));
					Assert::IsTrue(zcJump.engine.HasCarry());

					GpuFrameInput ziJump;
					ziJump.seq = 15; ziJump.w = c1.w; ziJump.h = c1.h;
					ziJump.receivedYTex = r1.tex; ziJump.cleanYTex = r1.tex; ziJump.hasCleanSplit = true; ziJump.activeRect = c1.active;
					ziJump.prevPeak = { p1.tex, &c1.prevMf, false };
					ziJump.nextPeak = { n1.tex, &c1.nextMf, false };
					ziJump.carryMotion = c1.carryMf; ziJump.hasCarryMotion = true;
					ziJump.hasStepPrior = true; ziJump.estimatedStepDelta = 24.0;
					GpuFrameOutput zoJump; zoJump.outputYTex = outY.tex;
					Assert::IsTrue(zcJump.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, ziJump, zoJump, nullptr, nullptr));
					Assert::IsTrue(zoJump.succeeded && zoJump.carryReset && !zoJump.carryUsed,
						L"MEDIUM-2: ProcessFrameGpu seq jump 10->15 must reset carry visibly");
				}
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

				// L4 / H9: Null splatMotion on frame 0 seeds cleanly without MotionFieldStandIn,
				//          while null splatMotion on an already-seeded canvas (frame 1+) MUST refuse whole-frame Still blend
				//          (returns false, leaves AccumulatedFrames == 1, engages MissingMotionNoWholeBlend & MotionFieldStandIn)!
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

				Assert::IsFalse(nullMotCanvas.AccumulateAnchorFrame(
					codedCb, codedCr, &cleanY, lumaW, lumaH, ChromaSubsampling::Yuv420,
					SitingSpec::Left(), active, nullptr, cellPx, 10, &nullMotSi),
					L"L4/H9: Null splatMotion on already-seeded canvas (frame 1+) must return false (no fabricated Still blend)");
				Assert::AreEqual(static_cast<size_t>(1u), nullMotCanvas.AccumulatedFrames(),
					L"L4/H9: Null splatMotion on frame 1+ must NOT mutate AccumulatedFrames");
				Assert::IsTrue(nullMotSi.IsActive(StandInKind::MissingMotionNoWholeBlend),
					L"L4/H9: Null splatMotion on frame 1+ MUST engage MissingMotionNoWholeBlend");
				Assert::IsTrue(nullMotSi.IsActive(StandInKind::MotionFieldStandIn),
					L"L4/H9: Null splatMotion on frame 1+ MUST engage MotionFieldStandIn");
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

		// =========================================================================
		// 7. TD-S5-OPT: Preset::RealTime Compact Band Path Cuts >55% VRAM, Preserves
		//    12-bit BTB/WTW Without FP16 Clipping, Preserves Two-Seed Grain, and
		//    Maintains Physics Rack-Focus Veto
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(RealTimeCompactBandPathPreservesBtbWtwGrainFocusAndCutsVram)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RealTimeCompactBandPathPreservesBtbWtwGrainFocusAndCutsVram)
		{
			WarpGpuContext ctx;

			// ---- Part A: 4K VRAM Budget Reduction & MakeVramUser Contract ----
			// Per-pixel FP32 footprint derivation (plan L60 — strictly FP32, never FP16):
			//   Reference: 5*RGBA32F(80) + carry(4) + warped(4) + 2*R16(4) + delta(8) + CPU staging(28) = 128 B/px.
			//   RealTime:  4*RG32F(32) + 5/4*RG32F(10) + carry(4) + warped(4) + 2*R16(4) + delta(8) = 62 B/px.
			//   Exact pixel ratio = 62 / 128 = 0.4844 (+ ~0.008 cell-grid overhead = 0.492 < 0.52).
			const uint64_t refVram4k = TdV2GpuEngine::EstimatedVramBytes(3840, 2160, 16, Preset::Reference);
			const uint64_t rtVram4k  = TdV2GpuEngine::EstimatedVramBytes(3840, 2160, 16, Preset::RealTime);
			Assert::IsTrue(refVram4k > 0u && rtVram4k > 0u, L"4K VRAM estimates must be > 0");
			Assert::IsTrue(static_cast<double>(rtVram4k) < 0.52 * static_cast<double>(refVram4k),
				FormatMsg("Preset::RealTime 4K FP32 VRAM (%.0f B) must be < 0.52x Reference (%.0f B)",
					static_cast<double>(rtVram4k), static_cast<double>(refVram4k)).c_str());

			const VpSkeleton::VramUser rtUser = TdV2GpuEngine::MakeVramUser(3840, 2160, 16, Preset::RealTime);
			Assert::AreEqual(static_cast<int>(VpSkeleton::kUserTemporalDetail), static_cast<int>(rtUser.id),
				L"MakeVramUser(RealTime) id must be kUserTemporalDetail");
			Assert::AreEqual(static_cast<int>(VpSkeleton::Tier::T1), static_cast<int>(rtUser.tier),
				L"MakeVramUser(RealTime) tier must be Tier::T1");
			Assert::IsTrue(rtUser.levels.size() >= 2u, L"MakeVramUser(RealTime) must have full + shed-to-0 levels");
			Assert::AreEqual(rtVram4k, rtUser.levels[0],
				L"MakeVramUser(RealTime) levels[0] must match EstimatedVramBytes(..., Preset::RealTime)");
			Assert::AreEqual(static_cast<uint64_t>(0u), rtUser.levels.back(),
				L"MakeVramUser(RealTime) shed level must be 0");

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;

			// Reserve a ScratchArena strictly sized for Preset::RealTime (full-res FP32 RG32F (M,H) + half-res FP32 RG32F (L,D3)).
			const std::vector<VpSkeleton::ArenaRequest> rtReqs = TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, 16, Preset::RealTime);
			Assert::IsFalse(rtReqs.empty(), L"ArenaRequests(..., Preset::RealTime) must be non-empty");
			const VpSkeleton::ArenaPlan rtPlan = VpSkeleton::PlanArena(rtReqs, VpSkeleton::Tier::T1);
			Assert::IsFalse(rtPlan.Empty(), L"Preset::RealTime Tier::T1 ArenaPlan must be non-empty");

			VpSkeleton::PlTexBackend texBackend(ctx.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> rtArena(texBackend);
			Assert::IsTrue(rtArena.Reserve(rtPlan), L"ScratchArena::Reserve(RealTime T1) failed");

			// NEGATIVE CONTROL (Compact Arena vs Reference):
			// Preset::Reference MUST fail visibly (scratchShort == true) on a Preset::RealTime-sized arena,
			// proving Preset::RealTime genuinely operates on compact textures rather than full RGBA32F bands!
			{
				ComplexSceneBundle negB = BuildComplexFrame(1, 1.5, 0.5, false);
				FrameProcessInput negIn = MakeFrameInput(negB, 700, true, false);
				Config cfgRef = MakePreset(Preset::Reference, Mode::On);
				cfgRef.enableLongAnchor = false;
				GpuEngineGuard refGuard(ctx.gpu, cfgRef);
				StandInRegistry refSi;
				rtArena.BeginFrame();
				const GpuProcessResult refOnRtArena = refGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, negIn, &rtArena, &refSi);
				Assert::IsFalse(refOnRtArena.succeeded,
					L"Negative control: Preset::Reference must fail visibly on a Preset::RealTime-sized arena");
				Assert::IsTrue(refOnRtArena.scratchShort,
					L"Negative control: Preset::Reference on RealTime-sized arena must set scratchShort == true");
				Assert::IsTrue(refSi.IsActive(StandInKind::GpuScratchShortage),
					L"Negative control: Preset::Reference on RealTime-sized arena must engage GpuScratchShortage");
				Assert::AreEqual(static_cast<uint64_t>(0u), rtArena.NestedScopes(),
					L"Negative control: ArenaScopeGuard must close cleanly on RealTime-arena shortage");
			}

			// ---- Part B1 (H7a): Genuine Preset::RealTime GPU vs CPU Parity on Moving Content with Distinct Refs ----
			// Distinct moving frames (cur != prev != next, camera pan (1.5, 0.5) px/frame) with letterbox bars:
			// verifies FP32 moments + honest SC-05 half-res L/D3 band split stay within physics-derived tolerance
			// and Plan s3.3 shortcut ledger (<= 10% gain loss vs CPU), with a forced-degraded broken-motion control!
			{
				const double camDx = 1.5;
				const double camDy = 0.5;
				const ActiveRect active{ 0, 4, w, h - 4 };

				const TdV2Plane masterCur  = RenderScene(w, h, 1.0 * camDx, 1.0 * camDy);
				const TdV2Plane masterPrev = RenderScene(w, h, 0.0 * camDx, 0.0 * camDy);
				const TdV2Plane masterNext = RenderScene(w, h, 2.0 * camDx, 2.0 * camDy);

				const double noiseSigma = 8.0;
				TdV2Plane curClean  = AddCodingDamage(masterCur,  noiseSigma, 7051u, 0.25, active);
				TdV2Plane prevClean = AddCodingDamage(masterPrev, 1.5,        7052u, 0.0,  active);
				TdV2Plane nextClean = AddCodingDamage(masterNext, 1.5,        7053u, 0.0,  active);
				FillLetterboxBars(curClean,  active, 64.0,  72.0);
				FillLetterboxBars(prevClean, active, 130.0, 145.0);
				FillLetterboxBars(nextClean, active, 135.0, 150.0);

				const CellMotionField prevMf = CellMotionField::Uniform(cw, ch, -camDx, -camDy, -camDx, -camDy, RegionMotionClass::Camera);
				const CellMotionField nextMf = CellMotionField::Uniform(cw, ch, +camDx, +camDy, +camDx, +camDy, RegionMotionClass::Camera);

				FrameProcessInput rtIn;
				rtIn.seq = 705;
				rtIn.receivedY = curClean;
				rtIn.hasCleanSplit = true;
				rtIn.cleanY = curClean;
				rtIn.activeRect = active;
				rtIn.prevPeak = { &prevClean, &prevMf, false };
				rtIn.nextPeak = { &nextClean, &nextMf, false };
				rtIn.hasStepPrior = true;
				rtIn.estimatedStepDelta = 24.0;
				rtIn.deadZonePhi = kDeadZonePhiInter;
				rtIn.zeroCoeffFraction = 0.35;

				Config cfgRt = MakePreset(Preset::RealTime, Mode::On);
				cfgRt.enableLongAnchor = false;
				cfgRt.enableCarryForward = false;

				TdV2ReferenceEngine cpuRtEng(cfgRt);
				const FrameProcessResult cpuRt = cpuRtEng.ProcessFrame(rtIn, nullptr);

				GpuEngineGuard gpuRtGuard(ctx.gpu, cfgRt);
				rtArena.BeginFrame();
				const GpuProcessResult gpuRt = gpuRtGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, rtIn, &rtArena, nullptr);
				Assert::IsTrue(gpuRt.succeeded && !gpuRt.scratchShort, L"H7a: RealTime moving GPU ProcessFrame failed");
				AssertAllFinitePlane(gpuRt.outputY, L"H7a: RealTime moving outputY must be finite");

				// Physics-derived SC-05 tolerance (Rule 1): with FP32 moments/M/H, the ONLY algorithmic difference between GPU
				// RealTime and CPU RealTime is SC-05 half-res L/D3 band split acting on the temporal difference plane.
				// Factor 2 accounts for both L and M = G_fine - L shifting by +/- (H_SC05 - H_Full)(diff) under convex weights.
				const TdV2Plane curErrPlane = SubtractPlanes(curClean, masterCur);
				const ThreeBandPlanes curErrFull = DecomposeActiveBands(curErrPlane, active);
				const TdV2Plane curErrSc05L = ComputeSc05HalfResLBand(curErrPlane, active);
				const double sc05DiffRmse = std::sqrt(PlaneMse(curErrSc05L, curErrFull.L, active));
				Assert::IsTrue(sc05DiffRmse > 1e-3, L"H7a: SC-05 L-band transfer error must be non-trivial");

				const double baseRmse  = std::sqrt(PlaneMse(curClean, masterCur, active));
				const double cpuRtRmse = std::sqrt(PlaneMse(cpuRt.outputY, masterCur, active));
				const double gpuRtRmse = std::sqrt(PlaneMse(gpuRt.outputY, masterCur, active));
				const double rtVsCpuRmse = std::sqrt(PlaneMse(gpuRt.outputY, cpuRt.outputY, active));
				const double cpuGain = baseRmse - cpuRtRmse;
				const double gpuGain = baseRmse - gpuRtRmse;
				const double sc05RmseTol = 2.0 * sc05DiffRmse + kGpuCpuParityTol;

				Assert::IsTrue(cpuGain > 1.5,
					FormatMsg("H7a non-vacuity: CPU RealTime gain=%.3f <= 1.5 codes", cpuGain).c_str());
				Assert::IsTrue(rtVsCpuRmse <= sc05RmseTol,
					FormatMsg("H7a: RealTime GPU vs CPU RMSE=%.4f > derived SC-05 bound %.4f", rtVsCpuRmse, sc05RmseTol).c_str());
				Assert::IsTrue(gpuGain >= 0.90 * cpuGain,
					FormatMsg("H7a shortcut ledger: GPU RT gain=%.3f < 0.90 * CPU RT gain (%.3f)", gpuGain, cpuGain).c_str());
				Assert::AreEqual(0.0, MaxAbsDiffRect(gpuRt.outputY, curClean, ActiveRect{ 0, 0, w, active.y0 }),
					L"H7a: RealTime top letterbox bar corrupted");
				Assert::AreEqual(0.0, MaxAbsDiffRect(gpuRt.outputY, curClean, ActiveRect{ 0, active.y1, w, h }),
					L"H7a: RealTime bottom letterbox bar corrupted");

				// FORCED-DEGRADED NEGATIVE CONTROL (Rule 3): Broken/uncompensated motion (vx=vy=0) on the EXACT SAME
				// moving frames diverges far beyond the SC-05 parity bound and loses substantial fidelity!
				const CellMotionField brokenMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				FrameProcessInput brokenIn = rtIn;
				brokenIn.seq = 706;
				brokenIn.prevPeak = { &prevClean, &brokenMf, false };
				brokenIn.nextPeak = { &nextClean, &brokenMf, false };
				rtArena.BeginFrame();
				const GpuProcessResult gpuBroken = gpuRtGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, brokenIn, &rtArena, nullptr);
				Assert::IsTrue(gpuBroken.succeeded, L"H7a negative control dispatch failed");
				const double brokenVsCpuRmse = std::sqrt(PlaneMse(gpuBroken.outputY, cpuRt.outputY, active));
				const double brokenOutRmse   = std::sqrt(PlaneMse(gpuBroken.outputY, masterCur, active));
				Assert::IsTrue(brokenVsCpuRmse > 2.0 * sc05RmseTol,
					FormatMsg("H7a negative control: broken-motion vs CPU RMSE=%.3f <= %.3f", brokenVsCpuRmse, 2.0 * sc05RmseTol).c_str());
				Assert::IsTrue(brokenOutRmse > gpuRtRmse + 1.0,
					FormatMsg("H7a negative control: broken-motion RMSE=%.3f did not degrade vs aligned=%.3f", brokenOutRmse, gpuRtRmse).c_str());
			}

			// ---- Part B2 (H7b): Non-Vacuous Moving 12-bit BTB/WTW ([4..4090]) + 18-bit FP32 Overflow Proof ----
			{
				const ActiveRect full = ActiveRect::Full(w, h);
				const double camDx = 1.5;
				const double camDy = 0.5;
				const CellMotionField prevMf = CellMotionField::Uniform(cw, ch, -camDx, -camDy, -camDx, -camDy, RegionMotionClass::Camera);
				const CellMotionField nextMf = CellMotionField::Uniform(cw, ch, +camDx, +camDy, +camDx, +camDy, RegionMotionClass::Camera);

				// Build distinct rigidly-moving 12-bit frames (cur != prev != next) spanning BTB [4..250], midtones, and WTW [3800..4090]
				// with smooth Hermite band transitions in gy (eliminating stationary-step seam artifacts under subpixel pan)
				// and large M/H contrast (~700-900 codes) so naive FP16 quantization incurs >= 0.25-0.5 code error!
				auto smoothstep = [](double e0, double e1, double t) -> double
				{
					const double u = (std::max)(0.0, (std::min)(1.0, (t - e0) / (e1 - e0)));
					return u * u * (3.0 - 2.0 * u);
				};
				auto render12BitScene = [&](double sx, double sy) -> TdV2Plane
				{
					TdV2Plane p(w, h, 0.0);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double gx = static_cast<double>(x) - sx;
							const double gy = static_cast<double>(y) - sy;
							const double vBtb = 128.0 + 58.0 * std::sin(2.0 * kPi * gx / 13.0) + 42.0 * std::cos(2.0 * kPi * gy / 11.0);
							const double vMid = 2048.0
								+ 880.0 * std::sin(2.0 * kPi * (gx / 9.0 + gy / 11.0))
								+ 520.0 * std::cos(2.0 * kPi * (gx / 4.5 - gy / 5.5));
							const double vWtw = 3930.0 + 75.0 * std::sin(2.0 * kPi * gx / 13.0) + 60.0 * std::cos(2.0 * kPi * gy / 11.0);
							const double tMid = smoothstep(17.0, 23.0, gy);
							const double tWtw = smoothstep(41.0, 47.0, gy);
							const double val = (vBtb + (vMid - vBtb) * tMid) * (1.0 - tWtw) + vWtw * tWtw;
							p.at(x, y) = (std::max)(4.0, (std::min)(4090.0, val));
						}
					}
					return p;
				};

				const TdV2Plane mCur12  = render12BitScene(1.0 * camDx, 1.0 * camDy);
				const TdV2Plane mPrev12 = render12BitScene(0.0 * camDx, 0.0 * camDy);
				const TdV2Plane mNext12 = render12BitScene(2.0 * camDx, 2.0 * camDy);

				// Physically self-consistent 12-bit coding damage & step prior:
				// noise12 = 32.0 (var = 1024 code^2), estimatedStepDelta = 96.0 with zeroCoeffFraction = 0.35
				// -> A2 totalVar = 977.1 code^2, matching injected coding damage cleanly under A3_ConservativeMin!
				const double noise12 = 32.0;
				const TdV2Plane cur12  = AddCodingDamage(mCur12,  noise12, 7120u, 0.0, full);
				const TdV2Plane prev12 = AddCodingDamage(mPrev12, 4.0,     7121u, 0.0, full);
				const TdV2Plane next12 = AddCodingDamage(mNext12, 4.0,     7122u, 0.0, full);

				FrameProcessInput in12;
				in12.seq = 712;
				in12.receivedY = cur12;
				in12.hasCleanSplit = true;
				in12.cleanY = cur12;
				in12.activeRect = full;
				in12.prevPeak = { &prev12, &prevMf, false };
				in12.nextPeak = { &next12, &nextMf, false };
				in12.hasStepPrior = true;
				in12.estimatedStepDelta = 96.0;
				in12.deadZonePhi = kDeadZonePhiInter;
				in12.zeroCoeffFraction = 0.35;

				// 1. Reference FP32 GPU vs CPU at 12-bit: parity <= DepthScale(12) * 1e-3 = 4e-3 codes!
				Config cfgRef12 = MakePreset(Preset::Reference, Mode::On);
				cfgRef12.sourceBits = 12;
				cfgRef12.enableLongAnchor = false;
				cfgRef12.enableCarryForward = false;
				TdV2ReferenceEngine cpuRef12Eng(cfgRef12);
				GpuEngineGuard gpuRef12Guard(ctx.gpu, cfgRef12);
				const FrameProcessResult cpuRef12 = cpuRef12Eng.ProcessFrame(in12, nullptr);
				const GpuProcessResult gpuRef12 = gpuRef12Guard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in12, nullptr, nullptr);
				Assert::IsTrue(gpuRef12.succeeded, L"H7b: 12-bit Reference GPU failed");
				const double ref12Tol = DepthScale(12) * kGpuCpuParityTol; // 4e-3 12-bit codes
				const double ref12Err = MaxAbsDiffFull(gpuRef12.outputY, cpuRef12.outputY);
				Assert::IsTrue(ref12Err <= ref12Tol,
					FormatMsg("H7b: 12-bit Reference |GPU-CPU|=%.6f > %.6f", ref12Err, ref12Tol).c_str());

				// 2. RealTime FP32 GPU vs CPU at 12-bit on compact ScratchArena: active repair + BTB/WTW preserved!
				Config cfgRt12 = MakePreset(Preset::RealTime, Mode::On);
				cfgRt12.sourceBits = 12;
				cfgRt12.enableLongAnchor = false;
				cfgRt12.enableCarryForward = false;
				TdV2ReferenceEngine cpuRt12Eng(cfgRt12);
				GpuEngineGuard gpuRt12Guard(ctx.gpu, cfgRt12);
				const FrameProcessResult cpuRt12 = cpuRt12Eng.ProcessFrame(in12, nullptr);
				rtArena.BeginFrame();
				const GpuProcessResult gpuRt12 = gpuRt12Guard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in12, &rtArena, nullptr);
				Assert::IsTrue(gpuRt12.succeeded && !gpuRt12.scratchShort, L"H7b: 12-bit RealTime GPU failed");
				AssertAllFinitePlane(gpuRt12.outputY, L"H7b: 12-bit RealTime outputY must be finite");

				// Derive 12-bit SC-05 half-res L/D3 transfer bound directly from the exact linear SC-05 operator on cur12 - mCur12.
				const TdV2Plane cur12Err = SubtractPlanes(cur12, mCur12);
				const ThreeBandPlanes cur12ErrFull = DecomposeActiveBands(cur12Err, full);
				const TdV2Plane cur12ErrSc05L = ComputeSc05HalfResLBand(cur12Err, full);
				const double sc05Diff12Rmse = std::sqrt(PlaneMse(cur12ErrSc05L, cur12ErrFull.L, full));
				const double sc05Tol12 = 2.0 * sc05Diff12Rmse + ref12Tol;

				const double base12Rmse = std::sqrt(PlaneMse(cur12, mCur12, full));
				const double rt12Rmse   = std::sqrt(PlaneMse(gpuRt12.outputY, mCur12, full));
				const double rt12VsCpu  = std::sqrt(PlaneMse(gpuRt12.outputY, cpuRt12.outputY, full));
				Assert::IsTrue(rt12Rmse < base12Rmse * 0.75,
					FormatMsg("H7b: 12-bit RealTime failed to repair moving BTB/WTW (out=%.2f vs base=%.2f)", rt12Rmse, base12Rmse).c_str());
				Assert::IsTrue(rt12VsCpu <= sc05Tol12,
					FormatMsg("H7b: 12-bit RealTime GPU vs CPU RMSE=%.3f > derived SC-05 bound %.3f", rt12VsCpu, sc05Tol12).c_str());

				// Verify BTB (< 256) and WTW (> 3760) are genuinely preserved in the moving output (not studio-clamped!).
				double minBtbOut = 1e9, maxWtwOut = -1e9;
				for (int y = 2; y < 16; ++y)
					for (int x = 4; x < w - 4; ++x)
						minBtbOut = (std::min)(minBtbOut, gpuRt12.outputY.at(x, y));
				for (int y = 48; y < h - 2; ++y)
					for (int x = 4; x < w - 4; ++x)
						maxWtwOut = (std::max)(maxWtwOut, gpuRt12.outputY.at(x, y));
				Assert::IsTrue(minBtbOut < 180.0, FormatMsg("H7b: 12-bit BTB clipped (min=%.1f >= 180)", minBtbOut).c_str());
				Assert::IsTrue(maxWtwOut > 3950.0, FormatMsg("H7b: 12-bit WTW clipped (max=%.1f <= 3950)", maxWtwOut).c_str());

				// 3. REAL FORCED-FP16 FUSION & STUDIO-CLAMP NEGATIVE CONTROLS (Rule 3) on the EXACT SAME 12-bit frames:
				//    (i) Forced-FP16 M/H band storage inside actual Wiener solve + band recomposition (modeling pre-fix RG16F!)
				//        diverges from exact FP64 recomposition by >= 0.15 codes (> 35x worse than FP32 GPU <= 4e-3)!
				{
					TdV2Plane wPrev12, wNext12;
					std::vector<uint8_t> vp12, vn12;
					WarpCleanReference(prev12, cur12, full, prevMf.vx, prevMf.vy, cw, ch, 16, wPrev12, vp12, nullptr);
					WarpCleanReference(next12, cur12, full, nextMf.vx, nextMf.vy, cw, ch, 16, wNext12, vn12, nullptr);
					const ThreeBandPlanes bCur = DecomposeActiveBands(cur12, full);
					const ThreeBandPlanes bPrev = DecomposeActiveBands(wPrev12, full);
					const ThreeBandPlanes bNext = DecomposeActiveBands(wNext12, full);

					TdV2Plane outFp64 = cur12;
					TdV2Plane outFp16 = cur12;
					const BandWhiteNoiseGains bg12 = ComputeBandWhiteNoiseGains();
					const RoundingCellStats a2_12 = ComputeRoundingCellStats(96.0, kDeadZonePhiInter, 0.35);

					auto runBandFusion = [&](bool quantizeMhToFp16, TdV2Plane& dstOut)
					{
						auto qMh = [&](double v, int b) { return (quantizeMhToFp16 && b >= 1) ? SimulateFp16Quantize(v) : v; };
						for (int cy = 0; cy < ch; ++cy)
						{
							for (int cx = 0; cx < cw; ++cx)
							{
								for (int b = 0; b < 3; ++b)
								{
									const TdV2Plane& cP = (b == 0) ? bCur.L : ((b == 1) ? bCur.M : bCur.H);
									const TdV2Plane& pP = (b == 0) ? bPrev.L : ((b == 1) ? bPrev.M : bPrev.H);
									const TdV2Plane& nP = (b == 0) ? bNext.L : ((b == 1) ? bNext.M : bNext.H);
									double C[2][2] = {};
									int cnt = 0;
									for (int y = cy * 16; y < (cy + 1) * 16; ++y)
									{
										for (int x = cx * 16; x < (cx + 1) * 16; ++x)
										{
											const size_t idx = static_cast<size_t>(y) * w + x;
											if (!vp12[idx] || !vn12[idx]) continue;
											const double d0 = qMh(cP.at(x, y), b) - qMh(pP.at(x, y), b);
											const double d1 = qMh(cP.at(x, y), b) - qMh(nP.at(x, y), b);
											C[0][0] += d0 * d0; C[0][1] += d0 * d1; C[1][1] += d1 * d1;
											++cnt;
										}
									}
									if (cnt <= 0) continue;
									const double invN = 1.0 / static_cast<double>(cnt);
									double cMat[kMaxBlendRefs][kMaxBlendRefs] = {};
									cMat[0][0] = C[0][0] * invN;
									cMat[0][1] = C[0][1] * invN;
									cMat[1][0] = C[0][1] * invN;
									cMat[1][1] = C[1][1] * invN;
									const double cA2 = a2_12.totalVar * ((b == 0) ? bg12.low : ((b == 1) ? bg12.mid : bg12.fine));
									const double cA1 = (std::max)(0.0, cMat[0][1]);
									const double cEst = (std::min)(cA1, cA2);
									double cVec[kMaxBlendRefs] = {};
									cVec[0] = (std::min)(cEst, cMat[0][0]);
									cVec[1] = (std::min)(cEst, cMat[1][1]);
									const BlendSolveResult sr = SolveConvexWeights(cMat, cVec, 2, kDefaultTraceRegScale, nullptr);
									if (sr.singular || sr.nonFiniteClosed) continue;
									for (int y = cy * 16; y < (cy + 1) * 16; ++y)
									{
										for (int x = cx * 16; x < (cx + 1) * 16; ++x)
										{
											const size_t idx = static_cast<size_t>(y) * w + x;
											if (!vp12[idx] || !vn12[idx]) continue;
											const double d0 = qMh(cP.at(x, y), b) - qMh(pP.at(x, y), b);
											const double d1 = qMh(cP.at(x, y), b) - qMh(nP.at(x, y), b);
											dstOut.at(x, y) -= (sr.weights[0] * d0 + sr.weights[1] * d1);
										}
									}
								}
							}
						}
					};
					runBandFusion(/*quantizeMhToFp16=*/false, outFp64);
					runBandFusion(/*quantizeMhToFp16=*/true,  outFp16);
					const double fp16FusionMaxErr = MaxAbsDiffFull(outFp16, outFp64);
					Assert::IsTrue(fp16FusionMaxErr >= 0.15,
						FormatMsg("H7b negative control: forced-FP16 M/H band fusion error=%.4f < 0.15 (>35x FP32 ref12Tol)",
							fp16FusionMaxErr).c_str());
				}
				//    (ii) Real studio-clamp engine negative control: running TdV2ReferenceEngine on studio-[256,3760]-clamped
				//         copies of cur12/prev12/next12 corrupts BTB/WTW output by > 150 codes vs unclipped GPU output!
				{
					auto clampStudio12 = [](const TdV2Plane& src) -> TdV2Plane
					{
						TdV2Plane c = src;
						for (double& v : c.v) v = (std::max)(256.0, (std::min)(3760.0, v));
						return c;
					};
					const TdV2Plane sCur12 = clampStudio12(cur12);
					const TdV2Plane sPrev12 = clampStudio12(prev12);
					const TdV2Plane sNext12 = clampStudio12(next12);
					FrameProcessInput sIn12 = in12;
					sIn12.receivedY = sCur12; sIn12.cleanY = sCur12;
					sIn12.prevPeak = { &sPrev12, &prevMf, false };
					sIn12.nextPeak = { &sNext12, &nextMf, false };
					TdV2ReferenceEngine studioEng(cfgRt12);
					const FrameProcessResult studioRes = studioEng.ProcessFrame(sIn12, nullptr);
					const double studioOutErr = MaxAbsDiffFull(studioRes.outputY, gpuRt12.outputY);
					Assert::IsTrue(studioOutErr > 150.0,
						FormatMsg("H7b negative control: studio-clamped engine output error=%.1f <= 150.0", studioOutErr).c_str());
				}

				// 4. High-bit-depth 18-bit FP32 overflow proof (sourceBits=18, peak=262143 > FP16 max 65504!):
				//    With RG16F, M/H bands > 65504 overflow to +/-Inf and kill focus veto / corrupt output!
				//    With FP32 RG32F, Preset::RealTime stays 100% finite with 0 nonFiniteCells!
				const double scale18 = DepthScale(18);
				const double dx18 = 2.0, dy18 = 1.0;
				const CellMotionField prevMf18 = CellMotionField::Uniform(cw, ch, -dx18, -dy18, -dx18, -dy18, RegionMotionClass::Camera);
				const CellMotionField nextMf18 = CellMotionField::Uniform(cw, ch, +dx18, +dy18, +dx18, +dy18, RegionMotionClass::Camera);
				TdV2Plane cur18(w, h, 0.0), prev18(w, h, 0.0), next18(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const double gxC = static_cast<double>(x) - dx18;
						const double gyC = static_cast<double>(y) - dy18;
						const double gxP = static_cast<double>(x);
						const double gyP = static_cast<double>(y);
						const double gxN = static_cast<double>(x) - 2.0 * dx18;
						const double gyN = static_cast<double>(y) - 2.0 * dy18;
						auto sample18 = [](double gx, double gy)
						{
							// Sum of period-4 orthogonal sinusoids under integer pan (2, 1):
							// stays strictly inside valid 18-bit [8024..254120] while |H| peaks at ~86,651 > FP16 max 65,504 on ~480 px!
							return 131072.0 + 110000.0 * 0.55 * (std::sin(2.0 * kPi * gx / 4.0) + std::cos(2.0 * kPi * gy / 4.0));
						};
						cur18.at(x, y)  = sample18(gxC, gyC) + 8.0 * scale18 * std::sin(static_cast<double>(x * 7 + y * 13));
						prev18.at(x, y) = sample18(gxP, gyP);
						next18.at(x, y) = sample18(gxN, gyN);
					}
				}
				const ThreeBandPlanes bands18 = DecomposeActiveBands(cur18, full);
				size_t fp16OverflowPx = 0;
				for (size_t i = 0; i < cur18.v.size(); ++i)
				{
					if (!std::isfinite(SimulateFp16Quantize(bands18.M.v[i])) ||
					    !std::isfinite(SimulateFp16Quantize(bands18.H.v[i])))
					{
						++fp16OverflowPx;
					}
				}
				Assert::IsTrue(fp16OverflowPx > 50u,
					FormatMsg("H7b negative control: 18-bit M/H bands must overflow FP16 (>50 px), got %.0f",
						static_cast<double>(fp16OverflowPx)).c_str());

				Config cfgRt18 = MakePreset(Preset::RealTime, Mode::On);
				cfgRt18.sourceBits = 18;
				cfgRt18.enableLongAnchor = false;
				cfgRt18.enableCarryForward = false;
				FrameProcessInput in18 = in12;
				in18.seq = 718;
				in18.receivedY = cur18;
				in18.cleanY = cur18;
				in18.prevPeak = { &prev18, &prevMf18, false };
				in18.nextPeak = { &next18, &nextMf18, false };
				in18.estimatedStepDelta = 24.0 * scale18;

				GpuEngineGuard gpuRt18Guard(ctx.gpu, cfgRt18);
				StandInRegistry si18;
				rtArena.BeginFrame();
				const GpuProcessResult gpuRt18 = gpuRt18Guard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in18, &rtArena, &si18);
				Assert::IsTrue(gpuRt18.succeeded && !gpuRt18.scratchShort, L"H7b: 18-bit RealTime FP32 GPU failed");
				AssertAllFinitePlane(gpuRt18.outputY, L"H7b: 18-bit RealTime FP32 outputY must be 100% finite (zero FP16 overflow!)");
				AssertAllFiniteTdMaps(gpuRt18.tdMaps, L"H7b: 18-bit RealTime FP32 tdMaps must be 100% finite");
				Assert::AreEqual(static_cast<size_t>(0u), gpuRt18.nonFiniteCells,
					L"H7b: 18-bit RealTime FP32 must report 0 nonFiniteCells (no RG16F Inf overflow)");
				Assert::IsFalse(si18.IsActive(StandInKind::NonFiniteClosure),
					L"H7b: 18-bit RealTime FP32 must NOT trip NonFiniteClosure");
				// L2 non-vacuity: verify 18-bit FP32 GPU RealTime performed genuine non-zero repair (not a no-op)!
				const double peakDelta18 = MaxAbsPlane(gpuRt18.tdDelta, full);
				Assert::IsTrue(peakDelta18 > 1.0 * scale18,
					FormatMsg("H7b L2 non-vacuity: 18-bit FP32 GPU RealTime |delta|=%.1f <= %.1f (no-op?)",
						peakDelta18, 1.0 * scale18).c_str());
			}

			// ---- Part C (HIGH-5): Two-Seed Planted Grain (15% Leak, 2 Consecutive Frames, Full Authoritative Bar + Layer-2-Off GPU Negative Control!) ----
			{
				const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
				const ActiveRect full = ActiveRect::Full(w, h);
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				const TdV2Plane masterClean = RenderScene(w, h, 0.0, 0.0);
				const TdV2Plane prevCleanTrue = AddCodingDamage(masterClean, 0.8, 7202u, 0.0, full);
				const TdV2Plane nextCleanTrue = AddCodingDamage(masterClean, 0.8, 7203u, 0.0, full);

				// Genuine L+M band compression wear on two consecutive valley frames so compression error lives in L/M
				// while 15%-leaked film grain lives predominantly in H (testing Layer-2 grain-leak budget on GPU RealTime!).
				auto addLowMidCodingWear = [&](const TdV2Plane& m, double rawSigma, uint32_t seed) -> TdV2Plane
				{
					const TdV2Plane whiteNoise = AddCodingDamage(TdV2Plane(w, h, 0.0), rawSigma, seed, 0.0, full);
					const ThreeBandPlanes nb = DecomposeActiveBands(whiteNoise, full);
					TdV2Plane out = m;
					for (size_t i = 0; i < n; ++i)
						out.v[i] = m.v[i] + nb.L.v[i] + nb.M.v[i];
					return out;
				};
				const TdV2Plane f0CleanTrue = addLowMidCodingWear(masterClean, 12.0, 7200u);
				const TdV2Plane f1CleanTrue = addLowMidCodingWear(masterClean, 12.0, 7201u);

				std::mt19937 gRng(7210u);
				const double grainSigma = 14.0;
				std::normal_distribution<double> gDist(0.0, grainSigma);
				std::vector<double> prevOwnGrain(n), ownGrain(n), foreignGrainPrev(n), foreignGrainNext(n);
				TdV2Plane f0Received(w, h, 0.0), f1Received(w, h, 0.0);
				for (size_t i = 0; i < n; ++i)
				{
					prevOwnGrain[i] = gDist(gRng);
					ownGrain[i] = gDist(gRng);
					foreignGrainPrev[i] = gDist(gRng);
					foreignGrainNext[i] = gDist(gRng);
					f0Received.v[i] = f0CleanTrue.v[i] + prevOwnGrain[i];
					f1Received.v[i] = f1CleanTrue.v[i] + ownGrain[i];
				}

				// Realistic >= 10% imperfect grain separation (leak = 15% of own & foreign grain left in clean planes!).
				const double leak = 0.15;
				TdV2Plane f0CleanImp(w, h, 0.0), f1CleanImp(w, h, 0.0), prevCleanImp(w, h, 0.0), nextCleanImp(w, h, 0.0);
				for (size_t i = 0; i < n; ++i)
				{
					f0CleanImp.v[i]   = f0CleanTrue.v[i]   + leak * prevOwnGrain[i];
					f1CleanImp.v[i]   = f1CleanTrue.v[i]   + leak * ownGrain[i];
					prevCleanImp.v[i] = prevCleanTrue.v[i] + leak * foreignGrainPrev[i];
					nextCleanImp.v[i] = nextCleanTrue.v[i] + leak * foreignGrainNext[i];
				}
				const double leakedVar = (leak * grainSigma) * (leak * grainSigma);
				const std::vector<double> retainedVar(static_cast<size_t>(cw) * static_cast<size_t>(ch), leakedVar);

				Config cfgRt = MakePreset(Preset::RealTime, Mode::On);
				cfgRt.enableLongAnchor = false;
				cfgRt.enableCarryForward = true;

				auto makeGrainFrame = [&](uint64_t seq, const TdV2Plane& rec, const TdV2Plane& cln, bool withLayer2, bool withCarry)
				{
					FrameProcessInput gIn;
					gIn.seq = seq;
					gIn.receivedY = rec;
					gIn.hasCleanSplit = true;
					gIn.cleanY = cln;
					if (withLayer2)
						gIn.retainedGrainVar = retainedVar;
					gIn.activeRect = full;
					gIn.prevPeak = { &prevCleanImp, &stillMf, false };
					gIn.nextPeak = { &nextCleanImp, &stillMf, false };
					if (withCarry)
					{
						gIn.hasCarryMotion = true;
						gIn.carryMotion = stillMf;
					}
					gIn.hasStepPrior = true;
					gIn.estimatedStepDelta = 12.0;
					gIn.deadZonePhi = kDeadZonePhiInter;
					gIn.zeroCoeffFraction = 0.35;
					return gIn;
				};

				// 1. MANDATORY SAME-INPUT NEGATIVE CONTROL (Rule 3 / HIGH-5):
				//    Run the EXACT SAME 15%-leaked clean frames on GPU RealTime with Layer-2 disabled (retainedGrainVar empty):
				//    MUST FAIL the authoritative grain preservation bar!
				{
					GpuEngineGuard noL2Guard(ctx.gpu, cfgRt);
					rtArena.BeginFrame();
					const GpuProcessResult r0NoL2 = noL2Guard.engine.ProcessFrame(
						ctx.gpu, ctx.dispatch, makeGrainFrame(710, f0Received, f0CleanImp, false, false), &rtArena, nullptr);
					Assert::IsTrue(r0NoL2.succeeded && !r0NoL2.scratchShort, L"Layer-2-off negative control frame 0 failed");
					rtArena.BeginFrame();
					const GpuProcessResult r1NoL2 = noL2Guard.engine.ProcessFrame(
						ctx.gpu, ctx.dispatch, makeGrainFrame(711, f1Received, f1CleanImp, false, true), &rtArena, nullptr);
					Assert::IsTrue(r1NoL2.succeeded && !r1NoL2.scratchShort, L"Layer-2-off negative control frame 1 failed");

					std::vector<double> prevResNoL2(n);
					for (size_t i = 0; i < n; ++i)
						prevResNoL2[i] = r0NoL2.outputY.v[i] - masterClean.v[i];
					const auto noL2Rep = EvaluateGrainTransfer(
						masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), r1NoL2.outputY.v.data(), n,
						prevResNoL2.data(), prevOwnGrain.data());
					Assert::IsTrue(noL2Rep.valid && noL2Rep.lag1Evaluated,
						L"Negative control: Layer-2-off grain regression + lag-1 must be valid");
					Assert::IsFalse(noL2Rep.passesAuthoritativeBar,
						FormatMsg("Negative control failed: Layer-2-off GPU on 15%%-leaked clean passed bar (betaOwn=%.4f, betaFor=%.4f)",
							noL2Rep.betaOwn, noL2Rep.betaForeign).c_str());
				}

				// 2. PROTECTED GPU Preset::RealTime (2 consecutive frames with Layer-2 retainedGrainVar enabled):
				//    MUST pass clean-layer repair AND FULL authoritative grain bar (lag1Evaluated == true)!
				{
					GpuEngineGuard rtGuard(ctx.gpu, cfgRt);
					rtArena.BeginFrame();
					const GpuProcessResult gRes0 = rtGuard.engine.ProcessFrame(
						ctx.gpu, ctx.dispatch, makeGrainFrame(720, f0Received, f0CleanImp, true, false), &rtArena, nullptr);
					Assert::IsTrue(gRes0.succeeded && !gRes0.scratchShort, L"RealTime two-seed grain frame 0 failed");
					rtArena.BeginFrame();
					const GpuProcessResult gRes1 = rtGuard.engine.ProcessFrame(
						ctx.gpu, ctx.dispatch, makeGrainFrame(721, f1Received, f1CleanImp, true, true), &rtArena, nullptr);
					Assert::IsTrue(gRes1.succeeded && !gRes1.scratchShort, L"RealTime two-seed grain frame 1 failed");
					AssertAllFinitePlane(gRes1.outputY, L"RealTime grain outputY must be finite");
					AssertAllFinitePlane(gRes1.outputClean, L"RealTime grain outputClean must be finite");

					// Clean-layer L+M coding wear genuinely repaired vs independent masterClean (isolate clean delta
					// without the intentionally preserved 15%-leaked H-band grain floor inside f1CleanImp / outputClean):
					TdV2Plane repairedTrueClean(w, h, 0.0);
					for (size_t i = 0; i < n; ++i)
						repairedTrueClean.v[i] = f1CleanTrue.v[i] + (gRes1.outputClean.v[i] - f1CleanImp.v[i]);
					const double cleanBaseRmse = std::sqrt(PlaneMse(f1CleanTrue, masterClean, interior));
					const double cleanOutRmse  = std::sqrt(PlaneMse(repairedTrueClean, masterClean, interior));
					Assert::IsTrue(cleanOutRmse < cleanBaseRmse * 0.72,
						FormatMsg("RealTime failed to repair clean coding wear: out=%.3f vs base=%.3f",
							cleanOutRmse, cleanBaseRmse).c_str());

					// Full authoritative grain bar across 2 consecutive frames (scored strictly vs independent masterClean!):
					std::vector<double> prevResMaster(n);
					for (size_t i = 0; i < n; ++i)
						prevResMaster[i] = gRes0.outputY.v[i] - masterClean.v[i];
					const auto rtRepPrev = EvaluateGrainTransfer(
						masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), gRes1.outputY.v.data(), n,
						prevResMaster.data(), prevOwnGrain.data());
					const auto rtRepNext = EvaluateGrainTransfer(
						masterClean.v.data(), ownGrain.data(), foreignGrainNext.data(), gRes1.outputY.v.data(), n,
						prevResMaster.data(), prevOwnGrain.data());
					Assert::IsTrue(rtRepPrev.valid && rtRepNext.valid && rtRepPrev.lag1Evaluated && rtRepNext.lag1Evaluated,
						L"RealTime 2-frame grain regression + lag-1 must be valid");
					Assert::IsTrue(rtRepPrev.passesAuthoritativeBar,
						FormatMsg("RealTime 15%%-leak GPU failed authoritative bar (betaOwn=%.4f, betaFor=%.4f)",
							rtRepPrev.betaOwn, rtRepPrev.betaForeign).c_str());
					Assert::IsTrue(rtRepNext.passesAuthoritativeBar,
						FormatMsg("RealTime 15%%-leak GPU failed authoritative bar vs next (stdRatio=%.4f, lag1Rise=%.4f)",
							rtRepNext.stdRatio, rtRepNext.lag1Rise).c_str());
					Assert::IsTrue(rtRepPrev.betaOwn >= kGrainBarMinBetaOwn);
					Assert::IsTrue(std::fabs(rtRepPrev.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(rtRepNext.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(rtRepPrev.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(rtRepPrev.lag1Rise <= kGrainBarMaxLag1Rise);
				}
			}

			// ---- Part D (Full-Diff HIGH-1): Full All-Band Rack-Focus Veto + GPU BokehMode::Untouched vs LowBandCellOnly Parity ----
			{
				// Dedicated interior rack-focus fixture: sharp periodic bars on prev/next in interior cell (1, 1)
				// ([16..32) x [16..32)) vs optically defocused sigma=4.5 counterpart in curClean -> Parseval
				// rack-focus veto MUST fire on ALL bands (kBandMask == 0) under compact RealTime!
				const ActiveRect full = ActiveRect::Full(w, h);
				const ActiveRect rfRect{ 16, 16, 32, 32 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				TdV2Plane curRf = RenderScene(w, h, 0.0, 0.0);
				TdV2Plane prevRf = curRf;
				TdV2Plane nextRf = curRf;

				auto sharpRf = [](double lx, double ly) -> double
				{
					const double env = (lx < 8.0) ? 340.0 : 580.0;
					return env + 120.0 * std::sin(2.0 * kPi * lx / 8.0) + 40.0 * std::cos(2.0 * kPi * ly / 8.0);
				};
				const double blurSigma = 4.5;
				const double invDenom = 1.0 / (std::sqrt(2.0) * blurSigma);
				const double atten = std::exp(-2.0 * kPi * kPi * blurSigma * blurSigma / 64.0);
				for (int y = 12; y < 36; ++y)
				{
					for (int x = 12; x < 36; ++x)
					{
						const double lx = static_cast<double>(x - 16);
						const double ly = static_cast<double>(y - 16);
						const double sVal = sharpRf(lx, ly);
						prevRf.at(x, y) = sVal;
						nextRf.at(x, y) = sVal;
						const double envBlur = 340.0 + 240.0 * 0.5 * (1.0 + std::erf((lx - 8.0) * invDenom));
						const double barsBlur = atten * (120.0 * std::sin(2.0 * kPi * lx / 8.0) + 40.0 * std::cos(2.0 * kPi * ly / 8.0));
						curRf.at(x, y) = envBlur + barsBlur;
					}
				}

				FrameProcessInput rfIn;
				rfIn.seq = 730;
				rfIn.receivedY = curRf;
				rfIn.hasCleanSplit = true;
				rfIn.cleanY = curRf;
				rfIn.activeRect = full;
				rfIn.prevPeak = { &prevRf, &stillMf, false };
				rfIn.nextPeak = { &nextRf, &stillMf, false };
				rfIn.hasStepPrior = true;
				rfIn.estimatedStepDelta = 16.0;
				rfIn.deadZonePhi = kDeadZonePhiInter;
				rfIn.zeroCoeffFraction = 0.35;

				Config cfgRt = MakePreset(Preset::RealTime, Mode::On);
				cfgRt.enableLongAnchor = false;
				GpuEngineGuard rtGuard(ctx.gpu, cfgRt);

				rtArena.BeginFrame();
				const GpuProcessResult rfRes = rtGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, rfIn, &rtArena, nullptr);
				Assert::IsTrue(rfRes.succeeded && !rfRes.scratchShort, L"RealTime rack-focus ProcessFrame failed");
				Assert::IsTrue(rfRes.focusVetoCells >= 1u,
					FormatMsg("RealTime rack-focus veto failed to fire (focusVetoCells=%.0f)",
						static_cast<double>(rfRes.focusVetoCells)).c_str());

				// Full-Diff HIGH-1: rack-focus (focusChange == true) MUST hard-veto ALL 3 bands (kBandMask == 0)
				// and keep full-plane drift < 1.0 code!
				const size_t rfCellIdx = 1u * static_cast<size_t>(cw) + 1u;
				const int rfBandMask = static_cast<int>(std::lround(rfRes.tdMaps[rfCellIdx].f[TdMapsView::kBandMask]));
				Assert::AreEqual(0, rfBandMask,
					L"RealTime TdMaps kBandMask must be 0 (ALL bands L/M/H vetoed on rack-focus cell (1, 1))");

				// NEGATIVE CONTROL (Rack-focus): Naive unprotected temporal average corrupts defocused cell > 20 codes!
				TdV2Plane naiveAvg(w, h, 0.0);
				for (size_t i = 0; i < naiveAvg.v.size(); ++i)
					naiveAvg.v[i] = (curRf.v[i] + prevRf.v[i] + nextRf.v[i]) / 3.0;
				const double naiveFullCorrupt = std::sqrt(PlaneMse(naiveAvg, curRf, rfRect));
				const double rtFullDrift      = std::sqrt(PlaneMse(rfRes.outputClean, curRf, rfRect));
				Assert::IsTrue(naiveFullCorrupt > 20.0,
					FormatMsg("Negative control failed: naive rack-focus full-plane corruption=%.2f <= 20.0", naiveFullCorrupt).c_str());
				Assert::IsTrue(rtFullDrift < 1.0,
					FormatMsg("RealTime rack-focus full-plane drift=%.3f >= 1.0 despite all-band veto", rtFullDrift).c_str());

				// Matched-focus BokehMode::Untouched vs BokehMode::LowBandCellOnly GPU==CPU parity (Q4 / Full-Diff HIGH-1):
				// All 3 frames share the SAME optically defocused blurred step edge (sigma=3.5 -> isDefocused==true, focusChange==false)
				// while cur carries low-frequency L-band coding wear (+18-code wave in L).
				TdV2Plane bokehBase(w, h, 0.0);
				const double bSig = 3.5;
				const double bInv = 1.0 / (std::sqrt(2.0) * bSig);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						bokehBase.at(x, y) = 340.0 + 240.0 * 0.5 * (1.0 + std::erf((static_cast<double>(x) - 32.0) * bInv));

				TdV2Plane lWearRaw(w, h, 0.0);
				std::mt19937 bRng(7350u);
				std::normal_distribution<double> bDist(0.0, 28.0);
				for (double& v : lWearRaw.v) v = bDist(bRng);
				const ThreeBandPlanes lWearBands = DecomposeActiveBands(lWearRaw, full);
				TdV2Plane bokehCur = bokehBase;
				for (size_t i = 0; i < bokehCur.v.size(); ++i)
					bokehCur.v[i] += lWearBands.L.v[i];

				std::vector<uint8_t> bokehMap(static_cast<size_t>(cw * ch), 1u);
				FrameProcessInput bIn;
				bIn.seq = 735;
				bIn.receivedY = bokehCur;
				bIn.hasCleanSplit = true;
				bIn.cleanY = bokehCur;
				bIn.activeRect = full;
				bIn.prevPeak = { &bokehBase, &stillMf, false };
				bIn.nextPeak = { &bokehBase, &stillMf, false };
				bIn.explicitDefocusedCells = &bokehMap;
				bIn.hasStepPrior = true;
				bIn.estimatedStepDelta = 16.0;
				bIn.deadZonePhi = kDeadZonePhiInter;
				bIn.zeroCoeffFraction = 0.35;

				const ActiveRect bokehEdgeCell{ 16, 16, 48, 48 };
				const size_t bokehCellIdx = 1u * static_cast<size_t>(cw) + 1u;

				// (a) BokehMode::Untouched: GPU matches CPU <= 1e-3, kBandMask == 0 on defocused bokeh cell, delta == 0.0!
				{
					Config cfgUnt = MakePreset(Preset::Reference, Mode::On);
					cfgUnt.bokehMode = BokehMode::Untouched;
					cfgUnt.enableLongAnchor = false;
					TdV2ReferenceEngine cpuUnt(cfgUnt);
					GpuEngineGuard gpuUnt(ctx.gpu, cfgUnt);
					const FrameProcessResult cU = cpuUnt.ProcessFrame(bIn, nullptr);
					const GpuProcessResult gU = gpuUnt.engine.ProcessFrame(ctx.gpu, ctx.dispatch, bIn, nullptr, nullptr);
					AssertCpuGpuFrameParity(cU, gU, bokehCur, bokehCur, full, "Bokeh-Untouched");
					Assert::AreEqual(0, static_cast<int>(std::lround(gU.tdMaps[bokehCellIdx].f[TdMapsView::kBandMask])),
						L"BokehMode::Untouched must veto ALL bands (kBandMask == 0) on defocused cell");
					Assert::AreEqual(0.0, MaxAbsPlane(gU.tdDelta, ActiveRect{ 16, 16, 32, 32 }),
						L"BokehMode::Untouched must keep defocused cell delta == 0.0");
				}
				// (b) BokehMode::LowBandCellOnly: GPU matches CPU <= 1e-3, kBandMask == 1 (L-band ONLY; M/H == 0!), repairs L!
				{
					Config cfgLow = MakePreset(Preset::Reference, Mode::On);
					cfgLow.bokehMode = BokehMode::LowBandCellOnly;
					cfgLow.enableLongAnchor = false;
					TdV2ReferenceEngine cpuLow(cfgLow);
					GpuEngineGuard gpuLow(ctx.gpu, cfgLow);
					const FrameProcessResult cL = cpuLow.ProcessFrame(bIn, nullptr);
					const GpuProcessResult gL = gpuLow.engine.ProcessFrame(ctx.gpu, ctx.dispatch, bIn, nullptr, nullptr);
					AssertCpuGpuFrameParity(cL, gL, bokehCur, bokehCur, full, "Bokeh-LowBandCellOnly");
					const int bMaskLow = static_cast<int>(std::lround(gL.tdMaps[bokehCellIdx].f[TdMapsView::kBandMask]));
					Assert::AreEqual(1, bMaskLow,
						L"BokehMode::LowBandCellOnly must allow ONLY L-band (kBandMask == 1, M/H == 0) on defocused cell");
					Assert::IsTrue(MaxAbsPlane(gL.tdDelta, bokehEdgeCell) > 0.5,
						L"BokehMode::LowBandCellOnly must actively repair L-band coding wear (> 0.5 codes)");
				}

				Assert::AreEqual(static_cast<uint64_t>(0u), rtArena.NestedScopes(),
					L"RealTime ScratchArena must finish with NestedScopes == 0");
			}
		}

		// =========================================================================
		// 8. TD-S5-OPT: ProcessFrameGpu Zero-Copy Parity (Seed + Active K=3 + Shadow
		//    with outputYTex=nullptr) & Cached ApplyChromaDeltaGpu Reuse with
		//    0-Dispatch All-Zero Fast Path + Un-Bypassed M-NEW-1 NaN Closure
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(ProcessFrameGpuZeroCopyAndCachedChromaReuseMatchCpuWrapper)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ProcessFrameGpuZeroCopyAndCachedChromaReuseMatchCpuWrapper)
		{
			WarpGpuContext ctx;

			// RAII guard for caller-owned pl_tex resources (always destroyed BEFORE WarpGpuContext tears down pl_d3d11!).
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

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
			Assert::IsNotNull(fmtR32, L"R32F unsupported on WARP");
			Assert::IsNotNull(fmtRG32, L"RG32F unsupported on WARP");
			Assert::IsNotNull(fmtRGBA32, L"RGBA32F unsupported on WARP");

			// =====================================================================
			// Part A: ProcessFrameGpu Zero-Copy vs ProcessFrame CPU Wrapper Parity
			// =====================================================================
			{
				OwnedTex inRecTex(ctx.gpu), inCleanTex(ctx.gpu), inPrevTex(ctx.gpu), inNextTex(ctx.gpu);
				OwnedTex outYTex(ctx.gpu), outCleanTex(ctx.gpu), outDeltaTex(ctx.gpu);
				OwnedTex outMapsTex(ctx.gpu), outDiagTex(ctx.gpu), outGuardTex(ctx.gpu), outWarpDiagTex(ctx.gpu);

				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, inRecTex.tex, w, h, fmtR32, true, false, true, false), L"inRecTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, inCleanTex.tex, w, h, fmtR32, true, false, true, false), L"inCleanTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, inPrevTex.tex, w, h, fmtR32, true, false, true, false), L"inPrevTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, inNextTex.tex, w, h, fmtR32, true, false, true, false), L"inNextTex alloc");

				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYTex.tex, w, h, fmtR32, true, true, true, true), L"outYTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCleanTex.tex, w, h, fmtR32, true, true, false, true), L"outCleanTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDeltaTex.tex, w, h, fmtRG32, true, true, false, true), L"outDeltaTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outMapsTex.tex, cw, 3 * ch, fmtRGBA32, true, true, false, true), L"outMapsTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDiagTex.tex, cw, ch, fmtRGBA32, true, true, false, true), L"outDiagTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outGuardTex.tex, cw, ch, fmtRG32, true, true, false, true), L"outGuardTex alloc");
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outWarpDiagTex.tex, cw, ch, fmtRG32, true, true, false, true), L"outWarpDiagTex alloc");

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				GpuEngineGuard wrapGuard(ctx.gpu, cfg);
				GpuEngineGuard zcGuard(ctx.gpu, cfg);

				// ---- Negative control (ProcessFrameGpu invalid input): null receivedYTex fails visibly ----
				{
					GpuFrameInput badIn;
					badIn.seq = 800;
					badIn.w = w;
					badIn.h = h;
					badIn.receivedYTex = nullptr;
					GpuFrameOutput badOut;
					StandInRegistry badSi;
					const bool ok = zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, badIn, badOut, nullptr, &badSi);
					Assert::IsFalse(ok, L"Negative control: ProcessFrameGpu with null receivedYTex must return false");
					Assert::IsFalse(badOut.succeeded, L"Negative control: ProcessFrameGpu null receivedYTex must set succeeded=false");
					Assert::IsFalse(badOut.lastError.empty(), L"Negative control: ProcessFrameGpu null receivedYTex must set lastError");
					Assert::IsTrue(badSi.IsActive(StandInKind::GpuDispatchFailed),
						L"Negative control: ProcessFrameGpu null receivedYTex must engage GpuDispatchFailed");
				}

				// ---- Frame 1: Seed frame (no peaks -> establishes persistent GPU carryCleanTex_) ----
				ComplexSceneBundle f0 = BuildComplexFrame(0, 1.5, 0.5, /*stampSpecialRegions=*/false);
				FrameProcessInput wrapIn0 = MakeFrameInput(f0, 801, /*withPeaks=*/false, /*transformIdentified8x8=*/false);
				const GpuProcessResult wrapRes0 = wrapGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, wrapIn0, nullptr, nullptr);
				Assert::IsTrue(wrapRes0.succeeded && wrapGuard.engine.HasCarry(), L"Wrapper seed frame failed");

				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, inRecTex.tex, f0.receivedY), L"Seed upload recY");
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, inCleanTex.tex, f0.curClean), L"Seed upload cleanY");

				GpuFrameInput zcIn0;
				zcIn0.seq = 801;
				zcIn0.w = w;
				zcIn0.h = h;
				zcIn0.receivedYTex = inRecTex.tex;
				zcIn0.cleanYTex = inCleanTex.tex;
				zcIn0.hasCleanSplit = true;
				zcIn0.activeRect = f0.active;
				zcIn0.retainedGrainVar = f0.retainedGrainVar;
				zcIn0.hasStepPrior = true;
				zcIn0.estimatedStepDelta = 24.0;
				zcIn0.deadZonePhi = kDeadZonePhiInter;
				zcIn0.zeroCoeffFraction = 0.35;

				GpuFrameOutput zcOut0;
				zcOut0.outputYTex = outYTex.tex;
				zcOut0.outputCleanTex = outCleanTex.tex;
				zcOut0.tdDeltaTex = outDeltaTex.tex;
				zcOut0.tdMapsTex = outMapsTex.tex;
				zcOut0.readbackCellDiagnostics = true;

				Assert::IsTrue(zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zcIn0, zcOut0, nullptr, nullptr),
					L"ProcessFrameGpu seed frame failed");
				Assert::IsTrue(zcOut0.succeeded, L"ProcessFrameGpu seed succeeded must be true");
				Assert::AreEqual(2, zcOut0.dispatchCount,
					L"ProcessFrameGpu seed frame with tdMapsTex must issue 2 dispatches (seed copy + cell zero)");
				Assert::AreEqual(static_cast<size_t>(cw * ch), zcOut0.tdMaps.size(),
					L"ProcessFrameGpu seed frame must populate out.tdMaps");
				Assert::IsTrue(zcGuard.engine.HasCarry(), L"ProcessFrameGpu seed frame must establish GPU carry");

				TdV2Plane zcSeedY, zcSeedClean;
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, zcSeedY), L"Download seed outY");
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outCleanTex.tex, w, h, zcSeedClean), L"Download seed outClean");
				Assert::IsTrue(MaxAbsDiffFull(zcSeedY, wrapRes0.outputY) <= 1e-4, L"Seed outputY zero-copy vs wrapper mismatch > 1e-4");
				Assert::IsTrue(MaxAbsDiffFull(zcSeedClean, wrapRes0.outputClean) <= 1e-4, L"Seed outputClean zero-copy vs wrapper mismatch > 1e-4");

				// ---- Frame 2: Active K=3 frame WITH carry + prevPeak + nextPeak + all special regions ----
				ComplexSceneBundle f1 = BuildComplexFrame(1, 1.5, 0.5, /*stampSpecialRegions=*/true);
				FrameProcessInput wrapIn1 = MakeFrameInput(f1, 802, /*withPeaks=*/true, /*transformIdentified8x8=*/false);
				StandInRegistry wrapSi1, zcSi1;
				const GpuProcessResult wrapRes1 = wrapGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, wrapIn1, nullptr, &wrapSi1);
				Assert::IsTrue(wrapRes1.succeeded && wrapRes1.carryUsed, L"Wrapper active K=3 frame failed");
				Assert::AreEqual(static_cast<size_t>(3u), wrapRes1.activeRefCount, L"Wrapper active frame must use K=3 refs");

				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, inRecTex.tex, f1.receivedY), L"F1 upload recY");
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, inCleanTex.tex, f1.curClean), L"F1 upload cleanY");
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, inPrevTex.tex, f1.prevClean), L"F1 upload prevClean");
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, inNextTex.tex, f1.nextClean), L"F1 upload nextClean");

				GpuFrameInput zcIn1 = zcIn0;
				zcIn1.seq = 802;
				zcIn1.activeRect = f1.active;
				zcIn1.prevPeak = { inPrevTex.tex, &f1.prevMf, false };
				zcIn1.nextPeak = { inNextTex.tex, &f1.nextMf, false };
				zcIn1.carryMotion = f1.carryMf;
				zcIn1.hasCarryMotion = true;
				zcIn1.retainedGrainVar = f1.retainedGrainVar;

				GpuFrameOutput zcOut1;
				zcOut1.outputYTex = outYTex.tex;
				zcOut1.outputCleanTex = outCleanTex.tex;
				zcOut1.tdDeltaTex = outDeltaTex.tex;
				zcOut1.tdMapsTex = outMapsTex.tex;
				zcOut1.diagFlagsTex = outDiagTex.tex;
				zcOut1.guardCellTex = outGuardTex.tex;
				zcOut1.warpDiagTex = outWarpDiagTex.tex;
				zcOut1.readbackCellDiagnostics = true;

				Assert::IsTrue(zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zcIn1, zcOut1, nullptr, &zcSi1),
					L"ProcessFrameGpu active K=3 frame failed");
				Assert::IsTrue(zcOut1.succeeded && zcOut1.carryUsed, L"ProcessFrameGpu active frame must succeed and use carry");
				Assert::AreEqual(static_cast<size_t>(3u), zcOut1.activeRefCount, L"ProcessFrameGpu active frame must use K=3 refs");
				Assert::IsTrue(zcOut1.dispatchCount > 0, L"ProcessFrameGpu active frame must issue compute dispatches");

				TdV2Plane zcOutY1, zcOutClean1, zcDelta1;
				bool anyBound = false;
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, zcOutY1), L"Download F1 outY");
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outCleanTex.tex, w, h, zcOutClean1), L"Download F1 outClean");
				Assert::IsTrue(Detail::DownloadDeltaRG32F(ctx.gpu, outDeltaTex.tex, w, h, zcDelta1, anyBound), L"Download F1 tdDelta");

				// Parity within 1e-4 across outputY, outputClean, tdDelta, and tdMaps!
				Assert::IsTrue(MaxAbsDiffFull(zcOutY1, wrapRes1.outputY) <= 1e-4,
					FormatMsg("ProcessFrameGpu K=3 outputY diff=%.6f > 1e-4", MaxAbsDiffFull(zcOutY1, wrapRes1.outputY)).c_str());
				Assert::IsTrue(MaxAbsDiffFull(zcOutClean1, wrapRes1.outputClean) <= 1e-4,
					FormatMsg("ProcessFrameGpu K=3 outputClean diff=%.6f > 1e-4", MaxAbsDiffFull(zcOutClean1, wrapRes1.outputClean)).c_str());
				Assert::IsTrue(MaxAbsDiffFull(zcDelta1, wrapRes1.tdDelta) <= 1e-4,
					FormatMsg("ProcessFrameGpu K=3 tdDelta diff=%.6f > 1e-4", MaxAbsDiffFull(zcDelta1, wrapRes1.tdDelta)).c_str());
				Assert::IsTrue(MaxTdMapsDiff(zcOut1.tdMaps, wrapRes1.tdMaps) <= 1e-4,
					FormatMsg("ProcessFrameGpu K=3 tdMaps diff=%.6f > 1e-4", MaxTdMapsDiff(zcOut1.tdMaps, wrapRes1.tdMaps)).c_str());
				Assert::AreEqual(wrapRes1.focusVetoCells, zcOut1.focusVetoCells, L"ProcessFrameGpu focusVetoCells parity");
				Assert::AreEqual(wrapRes1.guardViolationCells, zcOut1.guardViolationCells, L"ProcessFrameGpu guardViolationCells parity");

				// Non-vacuity: active K=3 zero-copy repair materially modifies outputY (> 2.0 codes vs receivedY)!
				Assert::IsTrue(MaxAbsDiffRect(zcOutY1, f1.receivedY, f1.active) > 2.0,
					L"ProcessFrameGpu active K=3 repair must differ from receivedY by > 2.0 codes");
				Assert::IsTrue(MaxAbsPlane(zcDelta1, f1.active) > 2.0,
					L"ProcessFrameGpu active K=3 tdDelta must exceed 2.0 codes");

				// ---- Frame 3: Mode::Shadow with outputYTex == nullptr ----
				Config cfgShadow = cfg;
				cfgShadow.mode = Mode::Shadow;
				wrapGuard.engine.SetConfig(cfgShadow);
				zcGuard.engine.SetConfig(cfgShadow);

				FrameProcessInput wrapInSh = MakeFrameInput(f1, 803, /*withPeaks=*/true, /*transformIdentified8x8=*/false);
				const GpuProcessResult wrapResSh = wrapGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, wrapInSh, nullptr, nullptr);
				Assert::IsTrue(wrapResSh.succeeded, L"Wrapper Shadow frame failed");

				GpuFrameInput zcInSh = zcIn1;
				zcInSh.seq = 803;
				GpuFrameOutput zcOutSh;
				zcOutSh.outputYTex = nullptr; // Explicitly nullptr in Mode::Shadow!
				zcOutSh.outputCleanTex = outCleanTex.tex;
				zcOutSh.tdDeltaTex = outDeltaTex.tex;
				zcOutSh.tdMapsTex = outMapsTex.tex;
				zcOutSh.diagFlagsTex = outDiagTex.tex;
				zcOutSh.guardCellTex = outGuardTex.tex;
				zcOutSh.warpDiagTex = outWarpDiagTex.tex;
				zcOutSh.readbackCellDiagnostics = true;

				Assert::IsTrue(zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zcInSh, zcOutSh, nullptr, nullptr),
					L"ProcessFrameGpu Mode::Shadow with outputYTex=nullptr must succeed");
				Assert::IsTrue(zcOutSh.succeeded && zcOutSh.dispatchCount > 0,
					L"ProcessFrameGpu Mode::Shadow must succeed with > 0 dispatches");

				TdV2Plane zcShClean, zcShDelta;
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outCleanTex.tex, w, h, zcShClean), L"Download Shadow outClean");
				Assert::IsTrue(Detail::DownloadDeltaRG32F(ctx.gpu, outDeltaTex.tex, w, h, zcShDelta, anyBound), L"Download Shadow tdDelta");
				Assert::IsTrue(MaxAbsDiffFull(zcShClean, wrapResSh.outputClean) <= 1e-4, L"Shadow zero-copy outputClean diff > 1e-4");
				Assert::IsTrue(MaxAbsDiffFull(zcShDelta, wrapResSh.tdDelta) <= 1e-4, L"Shadow zero-copy tdDelta diff > 1e-4");
				Assert::IsTrue(MaxTdMapsDiff(zcOutSh.tdMaps, wrapResSh.tdMaps) <= 1e-4, L"Shadow zero-copy tdMaps diff > 1e-4");
				Assert::IsTrue(MaxAbsPlane(zcShDelta, f1.active) > 2.0, L"Shadow zero-copy tdDelta must remain non-vacuously active");
			}

			// =====================================================================
			// Part B: Repeated ApplyChromaDeltaGpu Across YUV & RGB (Cached Texture
			//         Reuse + 0-Dispatch All-Zero Finite Fast Path + M-NEW-1 NaN
			//         Closure NEVER Bypassed!)
			// =====================================================================
			{
				const double kNaN = std::numeric_limits<double>::quiet_NaN();
				const int lumaW = 64, lumaH = 64;
				const int cW = lumaW / 2, cH = lumaH / 2;
				const int dW422 = lumaW / 2, dH422 = lumaH;
				const ActiveRect active{ 0, 4, lumaW, 60 };

				TdV2Plane delivCb422(dW422, dH422, 0.0), delivCr422(dW422, dH422, 0.0);
				for (int y = 0; y < dH422; ++y)
				{
					for (int x = 0; x < dW422; ++x)
					{
						delivCb422.at(x, y) = static_cast<double>(static_cast<float>(500.0 + 30.0 * std::sin(2.0 * kPi * x / 15.0)));
						delivCr422.at(x, y) = static_cast<double>(static_cast<float>(520.0 - 25.0 * std::cos(2.0 * kPi * y / 19.0)));
					}
				}
				TdV2Plane delivR(lumaW, lumaH, 0.0), delivG(lumaW, lumaH, 0.0), delivB(lumaW, lumaH, 0.0);
				for (int y = 0; y < lumaH; ++y)
				{
					for (int x = 0; x < lumaW; ++x)
					{
						delivR.at(x, y) = static_cast<double>(static_cast<float>(430.0 + 25.0 * std::sin(2.0 * kPi * x / 23.0)));
						delivG.at(x, y) = static_cast<double>(static_cast<float>(470.0 + 20.0 * std::cos(2.0 * kPi * y / 29.0)));
						delivB.at(x, y) = static_cast<double>(static_cast<float>(510.0 - 18.0 * std::sin(2.0 * kPi * (x + y) / 31.0)));
					}
				}

				TdV2Plane activeDeltaCb(cW, cH, 0.0), activeDeltaCr(cW, cH, 0.0);
				for (int cy = 2; cy < 30; ++cy)
				{
					for (int cx = 0; cx < 16; ++cx)
					{
						activeDeltaCb.at(cx, cy) = +11.0;
						activeDeltaCr.at(cx, cy) = -9.0;
					}
				}
				const TdV2Plane zeroDeltaCb(cW, cH, 0.0);
				const TdV2Plane zeroDeltaCr(cW, cH, 0.0);

				GpuEngineGuard chGuard(ctx.gpu, MakePreset(Preset::Reference, Mode::On));

				// 1. Active YUV 4:2:0->4:2:2 call (allocates cached textures, dispatchCount == 1).
				ChromaGpuApplyInput yuvIn;
				yuvIn.lumaW = lumaW;
				yuvIn.lumaH = lumaH;
				yuvIn.deliveredFormat = DeliveredChromaFormat::Yuv422;
				yuvIn.codedSubsampling = ChromaSubsampling::Yuv420;
				yuvIn.siting = SitingSpec::Left();
				yuvIn.activeRect = active;
				yuvIn.deliveredCb = delivCb422;
				yuvIn.deliveredCr = delivCr422;
				yuvIn.codedDeltaCb = activeDeltaCb;
				yuvIn.codedDeltaCr = activeDeltaCr;

				const ChromaGpuApplyResult rYuvAct = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, yuvIn, nullptr);
				Assert::IsTrue(rYuvAct.succeeded, L"Cached YUV active call failed");
				Assert::AreEqual(1, rYuvAct.dispatchCount, L"Active YUV call must issue 1 dispatch");
				Assert::IsTrue(MaxAbsDiffRect(rYuvAct.outCb, delivCb422, ActiveRect{ 0, 4, 14, 60 }) > 5.0,
					L"Active YUV call must materially update Cb");

				// 2. All-zero finite YUV call -> MUST take 0-dispatch fast path with bit-exact passthrough!
				ChromaGpuApplyInput yuvZero = yuvIn;
				yuvZero.codedDeltaCb = zeroDeltaCb;
				yuvZero.codedDeltaCr = zeroDeltaCr;
				StandInRegistry siYuvZero;
				const ChromaGpuApplyResult rYuvZero = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, yuvZero, &siYuvZero);
				Assert::IsTrue(rYuvZero.succeeded, L"All-zero finite YUV fast path failed");
				Assert::AreEqual(0, rYuvZero.dispatchCount, L"All-zero finite YUV MUST issue 0 GPU dispatches");
				Assert::AreEqual(static_cast<size_t>(0u), rYuvZero.nonFinitePixels, L"All-zero finite YUV nonFinitePixels must be 0");
				Assert::IsFalse(siYuvZero.IsActive(StandInKind::NonFiniteClosure), L"All-zero finite YUV must NOT engage NonFiniteClosure");
				Assert::AreEqual(0, std::memcmp(rYuvZero.outCb.v.data(), delivCb422.v.data(), delivCb422.v.size() * sizeof(double)),
					L"All-zero finite YUV outCb must be bit-exact passthrough");
				Assert::AreEqual(0, std::memcmp(rYuvZero.outCr.v.data(), delivCr422.v.data(), delivCr422.v.size() * sizeof(double)),
					L"All-zero finite YUV outCr must be bit-exact passthrough");
				Assert::AreEqual(0.0, MaxAbsPlane(rYuvZero.deliveredDeltaCb, ActiveRect::Full(dW422, dH422)),
					L"All-zero finite YUV deliveredDeltaCb must be 0");

				// Naive buggy 0-dispatch fast path (checks ONLY codedDelta == 0, omitting delivered finiteness scan)
				// used as the behavioral Rule-3 negative control on the EXACT SAME yuvZeroNan & rgbZeroNan inputs!
				auto naiveUncheckedZeroSkip = [](const ChromaGpuApplyInput& in) -> ChromaGpuApplyResult
				{
					ChromaGpuApplyResult r;
					bool allZero = true;
					for (size_t i = 0; i < in.codedDeltaCb.v.size(); ++i)
					{
						if (!(in.codedDeltaCb.v[i] == 0.0) || !(in.codedDeltaCr.v[i] == 0.0))
						{
							allZero = false;
							break;
						}
					}
					if (allZero)
					{
						if (in.deliveredFormat == DeliveredChromaFormat::Rgb444)
						{
							r.outR = in.deliveredR;
							r.outG = in.deliveredG;
							r.outB = in.deliveredB;
						}
						else
						{
							r.outCb = in.deliveredCb;
							r.outCr = in.deliveredCr;
						}
						r.dispatchCount = 0;
						r.nonFinitePixels = 0;
						r.succeeded = true;
					}
					return r;
				};

				// 3. Immediately follow with all-zero coded delta + NaN deliveredCb -> MUST NOT skip dispatch!
				//    M-NEW-1 NaN closure MUST run on GPU (dispatchCount == 1, nonFinitePixels == 1, NonFiniteClosure active)!
				TdV2Plane nanDelivCb = delivCb422;
				nanDelivCb.at(8, 12) = kNaN;

				ChromaGpuApplyInput yuvZeroNan = yuvZero;
				yuvZeroNan.deliveredCb = nanDelivCb;

				// NEGATIVE CONTROL (Rule 3): Naive 0-dispatch bypass on the exact same yuvZeroNan leaks NaN & reports 0 closures!
				const ChromaGpuApplyResult naiveYuvNan = naiveUncheckedZeroSkip(yuvZeroNan);
				Assert::IsTrue(naiveYuvNan.succeeded && naiveYuvNan.dispatchCount == 0 && naiveYuvNan.nonFinitePixels == 0u,
					L"Negative control: naive unchecked YUV skip must bypass GPU dispatch and miss closure");
				Assert::IsFalse(std::isfinite(naiveYuvNan.outCb.at(8, 12)),
					L"Negative control: naive unchecked YUV 0-dispatch skip must leak NaN into outCb");

				StandInRegistry siYuvZeroNan;
				const ChromaGpuApplyResult rYuvZeroNan = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, yuvZeroNan, &siYuvZeroNan);
				Assert::IsTrue(rYuvZeroNan.succeeded, L"All-zero YUV + NaN deliveredCb must complete cleanly");
				Assert::AreEqual(1, rYuvZeroNan.dispatchCount,
					L"All-zero YUV + NaN deliveredCb MUST dispatch on GPU (dispatchCount == 1, never bypassed!)");
				AssertAllFinitePlane(rYuvZeroNan.outCb, L"All-zero YUV + NaN outCb must be 100% finite");
				AssertAllFinitePlane(rYuvZeroNan.outCr, L"All-zero YUV + NaN outCr must be 100% finite");
				Assert::AreEqual(0.0, rYuvZeroNan.outCb.at(8, 12), L"All-zero YUV + NaN deliveredCb must close to 0.0");
				Assert::AreEqual(static_cast<size_t>(1u), rYuvZeroNan.nonFinitePixels,
					L"All-zero YUV + NaN deliveredCb must report nonFinitePixels == 1");
				Assert::IsTrue(siYuvZeroNan.IsActive(StandInKind::NonFiniteClosure),
					L"All-zero YUV + NaN deliveredCb must engage NonFiniteClosure");

				// 4. Active RGB 4:2:0->4:4:4 call on same engine (reuses cached codedDelta + allocates cached RGB textures).
				ChromaGpuApplyInput rgbIn;
				rgbIn.lumaW = lumaW;
				rgbIn.lumaH = lumaH;
				rgbIn.deliveredFormat = DeliveredChromaFormat::Rgb444;
				rgbIn.codedSubsampling = ChromaSubsampling::Yuv420;
				rgbIn.siting = SitingSpec::Left();
				rgbIn.activeRect = active;
				rgbIn.matrix = YCbCrMatrixSpec::Bt709();
				rgbIn.deliveredR = delivR;
				rgbIn.deliveredG = delivG;
				rgbIn.deliveredB = delivB;
				rgbIn.codedDeltaCb = activeDeltaCb;
				rgbIn.codedDeltaCr = activeDeltaCr;

				const ChromaGpuApplyResult rRgbAct = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbIn, nullptr);
				Assert::IsTrue(rRgbAct.succeeded, L"Cached RGB active call failed");
				Assert::AreEqual(1, rRgbAct.dispatchCount, L"Active RGB call must issue 1 dispatch");
				Assert::IsTrue(MaxAbsDiffRect(rRgbAct.outR, delivR, ActiveRect{ 0, 4, 28, 60 }) > 5.0,
					L"Active RGB call must materially update R");

				// 5. All-zero finite RGB call -> MUST take 0-dispatch fast path with bit-exact passthrough!
				ChromaGpuApplyInput rgbZero = rgbIn;
				rgbZero.codedDeltaCb = zeroDeltaCb;
				rgbZero.codedDeltaCr = zeroDeltaCr;
				StandInRegistry siRgbZero;
				const ChromaGpuApplyResult rRgbZero = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbZero, &siRgbZero);
				Assert::IsTrue(rRgbZero.succeeded, L"All-zero finite RGB fast path failed");
				Assert::AreEqual(0, rRgbZero.dispatchCount, L"All-zero finite RGB MUST issue 0 GPU dispatches");
				Assert::AreEqual(static_cast<size_t>(0u), rRgbZero.nonFinitePixels, L"All-zero finite RGB nonFinitePixels must be 0");
				Assert::IsFalse(siRgbZero.IsActive(StandInKind::NonFiniteClosure), L"All-zero finite RGB must NOT engage NonFiniteClosure");
				Assert::AreEqual(0, std::memcmp(rRgbZero.outR.v.data(), delivR.v.data(), delivR.v.size() * sizeof(double)),
					L"All-zero finite RGB outR must be bit-exact passthrough");
				Assert::AreEqual(0, std::memcmp(rRgbZero.outG.v.data(), delivG.v.data(), delivG.v.size() * sizeof(double)),
					L"All-zero finite RGB outG must be bit-exact passthrough");
				Assert::AreEqual(0, std::memcmp(rRgbZero.outB.v.data(), delivB.v.data(), delivB.v.size() * sizeof(double)),
					L"All-zero finite RGB outB must be bit-exact passthrough");

				// 6. Immediately follow with all-zero coded delta + NaN deliveredR -> MUST NOT bypass M-NEW-1!
				TdV2Plane nanDelivR = delivR;
				nanDelivR.at(12, 16) = kNaN;

				ChromaGpuApplyInput rgbZeroNan = rgbZero;
				rgbZeroNan.deliveredR = nanDelivR;

				// NEGATIVE CONTROL (Rule 3): Naive 0-dispatch bypass on the exact same rgbZeroNan leaks NaN & reports 0 closures!
				const ChromaGpuApplyResult naiveRgbNan = naiveUncheckedZeroSkip(rgbZeroNan);
				Assert::IsTrue(naiveRgbNan.succeeded && naiveRgbNan.dispatchCount == 0 && naiveRgbNan.nonFinitePixels == 0u,
					L"Negative control: naive unchecked RGB skip must bypass GPU dispatch and miss closure");
				Assert::IsFalse(std::isfinite(naiveRgbNan.outR.at(12, 16)),
					L"Negative control: naive unchecked RGB 0-dispatch skip must leak NaN into outR");
				StandInRegistry siRgbZeroNan;
				const ChromaGpuApplyResult rRgbZeroNan = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbZeroNan, &siRgbZeroNan);
				Assert::IsTrue(rRgbZeroNan.succeeded, L"All-zero RGB + NaN deliveredR must complete cleanly");
				Assert::AreEqual(1, rRgbZeroNan.dispatchCount,
					L"All-zero RGB + NaN deliveredR MUST dispatch on GPU (dispatchCount == 1, M-NEW-1 never bypassed!)");
				AssertAllFinitePlane(rRgbZeroNan.outR, L"All-zero RGB + NaN outR must be 100% finite");
				AssertAllFinitePlane(rRgbZeroNan.outG, L"All-zero RGB + NaN outG must be 100% finite");
				AssertAllFinitePlane(rRgbZeroNan.outB, L"All-zero RGB + NaN outB must be 100% finite");
				Assert::AreEqual(0.0, rRgbZeroNan.outR.at(12, 16), L"All-zero RGB + NaN deliveredR must close to 0.0");
				Assert::AreEqual(delivG.at(12, 16), rRgbZeroNan.outG.at(12, 16), 1e-5, L"All-zero RGB + NaN finite G must be preserved");
				Assert::AreEqual(delivB.at(12, 16), rRgbZeroNan.outB.at(12, 16), 1e-5, L"All-zero RGB + NaN finite B must be preserved");
				Assert::AreEqual(static_cast<size_t>(1u), rRgbZeroNan.nonFinitePixels,
					L"All-zero RGB + NaN deliveredR must report nonFinitePixels == 1");
				Assert::IsTrue(siRgbZeroNan.IsActive(StandInKind::NonFiniteClosure),
					L"All-zero RGB + NaN deliveredR must engage NonFiniteClosure");

				// 7. Geometry change (32x32 YUV then back to 64x64 RGB) on same cached engine succeeds cleanly!
				{
					ChromaGpuApplyInput smallYuv;
					smallYuv.lumaW = 32;
					smallYuv.lumaH = 32;
					smallYuv.deliveredFormat = DeliveredChromaFormat::Yuv420;
					smallYuv.codedSubsampling = ChromaSubsampling::Yuv420;
					smallYuv.siting = SitingSpec::Left();
					smallYuv.activeRect = ActiveRect::Full(32, 32);
					smallYuv.deliveredCb = TdV2Plane(16, 16, 512.0);
					smallYuv.deliveredCr = TdV2Plane(16, 16, 512.0);
					smallYuv.codedDeltaCb = TdV2Plane(16, 16, 6.0);
					smallYuv.codedDeltaCr = TdV2Plane(16, 16, -4.0);

					const ChromaGpuApplyResult rSmall = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, smallYuv, nullptr);
					Assert::IsTrue(rSmall.succeeded && rSmall.dispatchCount == 1, L"Cached chroma resize 64->32 failed");
					Assert::AreEqual(518.0, rSmall.outCb.at(8, 8), 1e-4, L"Cached chroma 32x32 outCb mismatch");
					Assert::AreEqual(508.0, rSmall.outCr.at(8, 8), 1e-4, L"Cached chroma 32x32 outCr mismatch");

					const ChromaGpuApplyResult rBackRgb = chGuard.engine.ApplyChromaDeltaGpu(ctx.gpu, ctx.dispatch, rgbIn, nullptr);
					Assert::IsTrue(rBackRgb.succeeded && rBackRgb.dispatchCount == 1, L"Cached chroma resize 32->64 RGB failed");
					Assert::IsTrue(MaxAbsDiffFull(rBackRgb.outR, rRgbAct.outR) <= 1e-5,
						L"Cached chroma resize back to 64x64 RGB must reproduce identical output");
				}
			}
		}

		// =========================================================================
		// 9. GPU Subtle Non-Rigid Face / Eyelid-Lip Micro-Motion Matches CPU Without
		//    Wax Smearing on Both Preset::Reference and Preset::RealTime
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuSubtleFaceMicroMotionMatchesCpuWithoutWaxSmearing)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuSubtleFaceMicroMotionMatchesCpuWithoutWaxSmearing)
		{
			WarpGpuContext ctx;

			const int w = 64;
			const int h = 64;
			const int cw = w / 16;
			const int ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);

			// Realistic 10-bit portrait skin patch + crisp eyelid/lip crease + highlight localized inside Still cell (2, 2).
			// Shifted by +2.0 px in current frame relative to prevPeak & nextPeak while 16x16 ME reports vx=vy=0 (Still).
			auto renderPortrait = [&](double eyelidShiftY) -> TdV2Plane
			{
				TdV2Plane p(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const double dx = static_cast<double>(x) - 32.0;
						const double dy = static_cast<double>(y) - 32.0;
						const double shading = 470.0 + 42.0 * std::cos(2.0 * kPi * dx / 96.0) * std::cos(2.0 * kPi * dy / 88.0);
						const double skinTex = 9.0 * std::sin(2.0 * kPi * (x / 7.3 + y / 9.1) + 0.3)
						                     + 7.0 * std::cos(2.0 * kPi * (x / 5.1 - y / 6.7) + 1.4);
						double val = shading + skinTex;

						if (x >= 34 && x < 46)
						{
							const double cx = static_cast<double>(x) - 39.5;
							const double arch = 0.04 * cx * cx;
							const double centerY = 39.0 + eyelidShiftY + arch;
							const double distCrease = static_cast<double>(y) - centerY;
							const double distHighlight = static_cast<double>(y) - (centerY - 1.6);
							const double horizTaper = std::exp(-0.5 * (cx / 4.2) * (cx / 4.2));
							val -= 175.0 * horizTaper * std::exp(-0.5 * (distCrease / 0.75) * (distCrease / 0.75));
							val +=  65.0 * horizTaper * std::exp(-0.5 * (distHighlight / 0.75) * (distHighlight / 0.75));
						}
						p.at(x, y) = val;
					}
				}
				return p;
			};

			const TdV2Plane refMaster = renderPortrait(0.0);
			const TdV2Plane curMaster = renderPortrait(2.0); // +2.0 px eyelid/lip micro-shift!

			const TdV2Plane curReceived = AddCodingDamage(curMaster, 7.0, 1201u, 0.15, full);
			const TdV2Plane prevPeak    = AddCodingDamage(refMaster, 1.2, 1202u, 0.0, full);
			const TdV2Plane nextPeak    = AddCodingDamage(refMaster, 1.2, 1203u, 0.0, full);

			const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			const ActiveRect microRect{ 34, 37, 46, 45 };
			const ActiveRect skinRect{ 2, 2, 30, 30 };
			const double curSkinRmse = std::sqrt(PlaneMse(curReceived, curMaster, skinRect));

			// Reserve a compact RealTime ScratchArena to exercise the full RealTime GPU path too.
			const std::vector<VpSkeleton::ArenaRequest> rtReqs = TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, 16, Preset::RealTime);
			const VpSkeleton::ArenaPlan rtPlan = VpSkeleton::PlanArena(rtReqs, VpSkeleton::Tier::T1);
			VpSkeleton::PlTexBackend texBackend(ctx.gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> rtArena(texBackend);
			Assert::IsTrue(rtArena.Reserve(rtPlan), L"ScratchArena::Reserve(RealTime) for portrait test failed");

			struct FaceModeCase
			{
				bool transform8x8;
				ResidualMode resMode;
				const char* tag;
			};
			const FaceModeCase modeCases[2] = {
				{ false, ResidualMode::Cell,   "GpuFace-UnknownCELL" },
				{ true,  ResidualMode::Strict, "GpuFace-8x8STRICT"   }
			};

			for (const FaceModeCase& mc : modeCases)
			{
				FrameProcessInput in;
				in.seq = mc.transform8x8 ? 902 : 901;
				in.receivedY = curReceived;
				in.hasCleanSplit = true;
				in.cleanY = curReceived;
				in.activeRect = full;
				in.prevPeak = { &prevPeak, &stillMf, false };
				in.nextPeak = { &nextPeak, &stillMf, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 12.0;
				in.deadZonePhi = kDeadZonePhiInter;
				in.zeroCoeffFraction = 0.35;
				in.transformIdentified8x8 = mc.transform8x8;

				// Pass-1 face mask (plan Q2 / s1.3 L127 / s3.3: moving face delta hash == 0):
				// Mask cell (2, 2) containing the moving eyelid/lip crease!
				std::vector<uint8_t> faceMask(static_cast<size_t>(cw) * static_cast<size_t>(ch), 0u);
				faceMask[2u * static_cast<size_t>(cw) + 2u] = 1u;
				in.faceMask = &faceMask;

				// 1. CPU Reference Engine with Pass-1 face mask
				Config cfgRef = MakePreset(Preset::Reference, Mode::On);
				cfgRef.residualMode = mc.resMode;
				cfgRef.cellProjIterations = 2;
				cfgRef.enableLongAnchor = false;
				TdV2ReferenceEngine cpuEng(cfgRef);
				StandInRegistry cpuFaceSi;
				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuFaceSi);
				Assert::IsTrue(cpuFaceSi.IsActive(StandInKind::FaceNonRigidMask),
					L"CPU Pass-1 faceMask must engage StandInKind::FaceNonRigidMask");

				// 2. GPU Preset::Reference -> MUST match CPU within <= 1e-3 10-bit code!
				GpuEngineGuard gpuRefGuard(ctx.gpu, cfgRef);
				StandInRegistry gpuRefSi;
				const GpuProcessResult gpuRefRes = gpuRefGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuRefSi);
				AssertCpuGpuFrameParity(cpuRes, gpuRefRes, curReceived, curReceived, full, mc.tag);
				Assert::IsTrue(gpuRefSi.IsActive(StandInKind::FaceNonRigidMask),
					L"GPU Reference Pass-1 faceMask must engage StandInKind::FaceNonRigidMask");

				// 3. GPU Preset::RealTime (compact FP32 band path on ScratchArena)
				Config cfgRt = MakePreset(Preset::RealTime, Mode::On);
				cfgRt.residualMode = mc.resMode;
				cfgRt.cellProjIterations = 2;
				cfgRt.enableLongAnchor = false;
				GpuEngineGuard gpuRtGuard(ctx.gpu, cfgRt);
				StandInRegistry gpuRtSi;
				rtArena.BeginFrame();
				const GpuProcessResult gpuRtRes = gpuRtGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, &rtArena, &gpuRtSi);
				Assert::IsTrue(gpuRtRes.succeeded && !gpuRtRes.scratchShort,
					FormatTagMsg(mc.tag, "GPU RealTime portrait ProcessFrame failed").c_str());
				AssertAllFinitePlane(gpuRtRes.outputY, L"GPU RealTime portrait outputY must be finite");
				Assert::IsTrue(gpuRtSi.IsActive(StandInKind::FaceNonRigidMask),
					L"GPU RealTime Pass-1 faceMask must engage StandInKind::FaceNonRigidMask");

				// Verify BOTH GPU Reference and GPU RealTime enforce exact delta == 0 on masked moving face cell (2,2)
				// (plan s3.3: "moving face delta hash = 0", zero extra harm) while still repairing unmasked background!
				TdV2Plane curReceivedF32(w, h, 0.0);
				for (size_t i = 0; i < curReceived.v.size(); ++i)
					curReceivedF32.v[i] = static_cast<double>(static_cast<float>(curReceived.v[i]));
				const double curMicroRmseF32 = std::sqrt(PlaneMse(curReceivedF32, curMaster, microRect));
				const ActiveRect faceCellRect{ 32, 32, 48, 48 };
				auto verifyPass1FaceProtected = [&](const GpuProcessResult& r, const char* presetTag)
				{
					Assert::AreEqual(0.0, MaxAbsPlane(r.tdDelta, faceCellRect),
						FormatTagMsg(presetTag, "Masked face cell tdDelta must be exact 0.0").c_str());
					Assert::AreEqual(0.0, MaxAbsDiffRect(r.outputY, curReceivedF32, faceCellRect),
						FormatTagMsg(presetTag, "Masked face cell outputY must equal R32F receivedY bit-exact").c_str());
					const double tdMicroRmse = std::sqrt(PlaneMse(r.outputY, curMaster, microRect));
					const double tdSkinRmse  = std::sqrt(PlaneMse(r.outputY, curMaster, skinRect));
					Assert::AreEqual(curMicroRmseF32, tdMicroRmse, 1e-12,
						FormatTagMsg(presetTag, "Masked face micro-RMSE=%.6f != R32F received=%.6f", tdMicroRmse, curMicroRmseF32).c_str());
					Assert::IsTrue(tdSkinRmse < curSkinRmse * 0.88,
						FormatTagMsg(presetTag, "Unmasked background skin not strongly repaired (td=%.2f >= 0.88*cur=%.2f)",
							tdSkinRmse, curSkinRmse * 0.88).c_str());
				};

				verifyPass1FaceProtected(gpuRefRes, mc.transform8x8 ? "GpuRef-8x8STRICT" : "GpuRef-UnknownCELL");
				verifyPass1FaceProtected(gpuRtRes,  mc.transform8x8 ? "GpuRT-8x8STRICT"  : "GpuRT-UnknownCELL");

				// NEGATIVE CONTROL (Unmasked run on exact same portrait): removing faceMask produces non-zero delta
				// inside faceCellRect, proving faceMask is actively suppressing temporal blending on the moving face!
				FrameProcessInput unmaskedIn = in;
				unmaskedIn.faceMask = nullptr;
				StandInRegistry unmaskedSi;
				const GpuProcessResult unmaskedRes = gpuRefGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, unmaskedIn, nullptr, &unmaskedSi);
				Assert::IsTrue(unmaskedRes.succeeded, L"Unmasked negative control failed");
				Assert::IsFalse(unmaskedSi.IsActive(StandInKind::FaceNonRigidMask),
					L"Negative control: unmasked run must NOT engage FaceNonRigidMask");
				Assert::IsTrue(MaxAbsPlane(unmaskedRes.tdDelta, faceCellRect) > 1.0,
					L"Negative control: unmasked portrait run must produce non-zero delta (> 1.0) on face cell");
			}

			// ---- Part B (H13b / Full-Diff HIGH-2): GPU Unmasked Subtle Cell-Boundary Micro-Motion
			//      enablePerCornerHuber = true vs enablePerCornerHuber = false on BOTH Reference & RealTime! ----
			{
				auto renderFold = [&](double foldShiftY) -> TdV2Plane
				{
					TdV2Plane p(w, h, 0.0);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double dx = static_cast<double>(x) - 32.0;
							const double dy = static_cast<double>(y) - 32.0;
							const double shading = 470.0 + 42.0 * std::cos(2.0 * kPi * dx / 96.0) * std::cos(2.0 * kPi * dy / 88.0);
							const double tex = 9.0 * std::sin(2.0 * kPi * (x / 7.3 + y / 9.1) + 0.3)
							                 + 7.0 * std::cos(2.0 * kPi * (x / 5.1 - y / 6.7) + 1.4);
							double val = shading + tex;
							// Subtle unmasked non-face fold localized right at the left boundary of cell (2, 2): x in [32, 42).
							if (x >= 32 && x < 42)
							{
								const double cx = static_cast<double>(x) - 36.5;
								const double centerY = 37.0 + foldShiftY;
								const double dist = static_cast<double>(y) - centerY;
								const double horizTaper = std::exp(-0.5 * (cx / 3.2) * (cx / 3.2));
								val -= 140.0 * horizTaper * std::exp(-0.5 * (dist / 0.75) * (dist / 0.75));
								val +=  50.0 * horizTaper * std::exp(-0.5 * ((dist + 1.6) / 0.75) * ((dist + 1.6) / 0.75));
							}
							p.at(x, y) = val;
						}
					}
					return p;
				};

				const TdV2Plane fRefMaster = renderFold(0.0);
				const TdV2Plane fCurMaster = renderFold(2.0);
				const TdV2Plane fCurRec    = AddCodingDamage(fCurMaster, 7.0, 1251u, 0.0, full);
				const TdV2Plane fPrevPeak  = AddCodingDamage(fRefMaster, 1.2, 1252u, 0.0, full);
				const TdV2Plane fNextPeak  = AddCodingDamage(fRefMaster, 1.2, 1253u, 0.0, full);

				const ActiveRect microCellRect{ 32, 32, 48, 48 };
				const ActiveRect bgRect{ 2, 2, 30, 30 };
				const double curFoldMicroRmse = std::sqrt(PlaneMse(fCurRec, fCurMaster, microCellRect));
				const double curFoldBgRmse    = std::sqrt(PlaneMse(fCurRec, fCurMaster, bgRect));

				FrameProcessInput hIn;
				hIn.seq = 950;
				hIn.receivedY = fCurRec;
				hIn.hasCleanSplit = true;
				hIn.cleanY = fCurRec;
				hIn.activeRect = full;
				hIn.prevPeak = { &fPrevPeak, &stillMf, false };
				hIn.nextPeak = { &fNextPeak, &stillMf, false };
				hIn.hasStepPrior = true;
				hIn.estimatedStepDelta = 14.0;
				hIn.deadZonePhi = kDeadZonePhiInter;
				hIn.zeroCoeffFraction = 0.20;
				hIn.faceMask = nullptr; // Explicitly UNMASKED non-face subtle micro-motion!

				// 1. Preset::Reference: GPU == CPU <= 1e-3 for BOTH enablePerCornerHuber = true AND false!
				Config cfgRefOn = MakePreset(Preset::Reference, Mode::On);
				cfgRefOn.enableLongAnchor = false;
				cfgRefOn.enablePerCornerHuber = true;
				Config cfgRefOff = cfgRefOn;
				cfgRefOff.enablePerCornerHuber = false;

				TdV2ReferenceEngine cpuRefOnEng(cfgRefOn), cpuRefOffEng(cfgRefOff);
				GpuEngineGuard gpuRefOnGuard(ctx.gpu, cfgRefOn), gpuRefOffGuard(ctx.gpu, cfgRefOff);
				const FrameProcessResult cRefOn  = cpuRefOnEng.ProcessFrame(hIn, nullptr);
				const FrameProcessResult cRefOff = cpuRefOffEng.ProcessFrame(hIn, nullptr);
				const GpuProcessResult   gRefOn  = gpuRefOnGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, hIn, nullptr, nullptr);
				const GpuProcessResult   gRefOff = gpuRefOffGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, hIn, nullptr, nullptr);
				AssertCpuGpuFrameParity(cRefOn,  gRefOn,  fCurRec, fCurRec, full, "H13b-RefHuberON");
				AssertCpuGpuFrameParity(cRefOff, gRefOff, fCurRec, fCurRec, full, "H13b-RefHuberOFF");

				const double gRefOnMicro  = std::sqrt(PlaneMse(gRefOn.outputY,  fCurMaster, microCellRect));
				const double gRefOffMicro = std::sqrt(PlaneMse(gRefOff.outputY, fCurMaster, microCellRect));
				const double gRefOnBg     = std::sqrt(PlaneMse(gRefOn.outputY,  fCurMaster, bgRect));
				Assert::IsTrue(gRefOnMicro <= curFoldMicroRmse,
					FormatMsg("H13b GPU Reference Huber-ON violated strict no-harm (%.3f > %.3f)", gRefOnMicro, curFoldMicroRmse).c_str());
				Assert::IsTrue(gRefOffMicro > gRefOnMicro && gRefOffMicro > curFoldMicroRmse,
					FormatMsg("H13b GPU Reference negative control: Huber-OFF (%.3f) must degrade vs Huber-ON (%.3f) and violate no-harm",
						gRefOffMicro, gRefOnMicro).c_str());
				Assert::IsTrue(gRefOnBg < curFoldBgRmse * 0.88,
					FormatMsg("H13b GPU Reference Huber-ON failed to repair stationary bg (%.2f >= %.2f)",
						gRefOnBg, curFoldBgRmse * 0.88).c_str());

				// 2. Preset::RealTime (poolR = 0): GPU Huber-ON satisfies strict no-harm while GPU Huber-OFF degrades!
				Config cfgRtOn = MakePreset(Preset::RealTime, Mode::On);
				cfgRtOn.enableLongAnchor = false;
				cfgRtOn.enablePerCornerHuber = true;
				Config cfgRtOff = cfgRtOn;
				cfgRtOff.enablePerCornerHuber = false;

				GpuEngineGuard gpuRtOnGuard(ctx.gpu, cfgRtOn), gpuRtOffGuard(ctx.gpu, cfgRtOff);
				rtArena.BeginFrame();
				const GpuProcessResult gRtOn  = gpuRtOnGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, hIn, &rtArena, nullptr);
				rtArena.BeginFrame();
				const GpuProcessResult gRtOff = gpuRtOffGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, hIn, &rtArena, nullptr);
				Assert::IsTrue(gRtOn.succeeded && !gRtOn.scratchShort, L"H13b GPU RealTime Huber-ON failed");
				Assert::IsTrue(gRtOff.succeeded && !gRtOff.scratchShort, L"H13b GPU RealTime Huber-OFF failed");

				const double gRtOnMicro  = std::sqrt(PlaneMse(gRtOn.outputY,  fCurMaster, microCellRect));
				const double gRtOffMicro = std::sqrt(PlaneMse(gRtOff.outputY, fCurMaster, microCellRect));
				const double gRtOnBg     = std::sqrt(PlaneMse(gRtOn.outputY,  fCurMaster, bgRect));
				Assert::IsTrue(gRtOnMicro <= curFoldMicroRmse,
					FormatMsg("H13b GPU RealTime Huber-ON violated strict no-harm (%.3f > %.3f)", gRtOnMicro, curFoldMicroRmse).c_str());
				Assert::IsTrue(gRtOffMicro > gRtOnMicro && gRtOffMicro > curFoldMicroRmse,
					FormatMsg("H13b GPU RealTime negative control: Huber-OFF (%.3f) must worsen vs Huber-ON (%.3f)",
						gRtOffMicro, gRtOnMicro).c_str());
				Assert::IsTrue(gRtOnBg < curFoldBgRmse * 0.88,
					FormatMsg("H13b GPU RealTime Huber-ON failed to repair stationary bg (%.2f >= %.2f)",
						gRtOnBg, curFoldBgRmse * 0.88).c_str());
			}
			Assert::AreEqual(static_cast<uint64_t>(0u), rtArena.NestedScopes(),
				L"Portrait RealTime ScratchArena must finish with NestedScopes == 0");
		}

		// =========================================================================
		// 10. H5: Prepacked motionVecTex/motionMetaTex Fixed Slots (prev=0, next=1,
		//     carry=2) With prevPeak == nullptr + Texture Dimension Mismatch Guards
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(PrepackedMotionFixedSlotsPrevNullAndTextureDimensionGuards)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PrepackedMotionFixedSlotsPrevNullAndTextureDimensionGuards)
		{
			WarpGpuContext ctx;

			struct OwnedTex
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				explicit OwnedTex(pl_gpu g) : gpu(g) {}
				~OwnedTex() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				OwnedTex(const OwnedTex&) = delete;
				OwnedTex& operator=(const OwnedTex&) = delete;
			};

			const int w = 64, h = 64;
			const int cw = w / 16, ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);
			const double dx = 2.0, dy = 1.0;

			const pl_fmt fmtR32    = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32   = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
			Assert::IsNotNull(fmtR32); Assert::IsNotNull(fmtRG32); Assert::IsNotNull(fmtRGBA32);

			// Frame 0 (seed carry): t = 0; Frame 1 (cur): t = 1 (+dx, +dy); Next peak: t = 2 (+2*dx, +2*dy).
			// True motion from cur(t=1) to:
			//   - slot 1 (nextPeak, t=2):  (+2.0, +1.0)
			//   - slot 2 (carry,    t=0):  (-2.0, -1.0)  <-- OPPOSITE direction to next!
			//   - slot 0 (prevPeak, NULL): poisoned garbage (+11.0, -9.0, valid=0)
			// High-contrast bandlimited 10-bit scene (strong M/H energy -> zero spurious focus veto)
			// paired with crisp I-frame carry seed (noise=0.5), noisy current valley (noise=8.0), and noisier next ref (noise=7.0)
			// so carry dominates true-slot fusion while wrong-slot carry warping diverges sharply (> 5.0 codes)!
			auto renderH5Scene = [&](double sx, double sy) -> TdV2Plane
			{
				TdV2Plane p(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const double gx = static_cast<double>(x) - sx;
						const double gy = static_cast<double>(y) - sy;
						p.at(x, y) = 512.0
							+ 120.0 * std::sin(2.0 * kPi * (gx / 11.0 + gy / 13.0))
							+  90.0 * std::cos(2.0 * kPi * (gx / 7.0  - gy / 9.0));
					}
				}
				return p;
			};
			const TdV2Plane m0 = renderH5Scene(0.0, 0.0);
			const TdV2Plane m1 = renderH5Scene(1.0 * dx, 1.0 * dy);
			const TdV2Plane m2 = renderH5Scene(2.0 * dx, 2.0 * dy);
			const TdV2Plane f0Clean = AddCodingDamage(m0, 0.5, 10001u, 0.0, full);
			const TdV2Plane f1Clean = AddCodingDamage(m1, 8.0, 10002u, 0.0, full);
			const TdV2Plane f2Clean = AddCodingDamage(m2, 7.0, 10003u, 0.0, full);

			const CellMotionField mfNext  = CellMotionField::Uniform(cw, ch, +dx, +dy, +dx, +dy, RegionMotionClass::Camera);
			const CellMotionField mfCarry = CellMotionField::Uniform(cw, ch, -dx, -dy, -dx, -dy, RegionMotionClass::Camera);

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;
			cfg.enableCarryForward = true;

			// 1. Ground-truth CPU reference: frame 0 seed -> frame 1 with prevPeak=nullptr, nextPeak=f2Clean, carry=f0Clean.
			TdV2ReferenceEngine cpuEng(cfg);
			{
				FrameProcessInput cIn0;
				cIn0.seq = 1000;
				cIn0.receivedY = f0Clean;
				cIn0.hasCleanSplit = true;
				cIn0.cleanY = f0Clean;
				cIn0.activeRect = full;
				cpuEng.ProcessFrame(cIn0, nullptr);
				Assert::IsTrue(cpuEng.HasCarry(), L"CPU frame 0 must seed carry");
			}
			FrameProcessInput cIn1;
			cIn1.seq = 1001;
			cIn1.receivedY = f1Clean;
			cIn1.hasCleanSplit = true;
			cIn1.cleanY = f1Clean;
			cIn1.activeRect = full;
			cIn1.prevPeak = { nullptr, nullptr, false }; // prevPeak strictly NULL!
			cIn1.nextPeak = { &f2Clean, &mfNext, false };
			cIn1.carryMotion = mfCarry;
			cIn1.hasCarryMotion = true;
			cIn1.hasStepPrior = true;
			cIn1.estimatedStepDelta = 24.0;
			cIn1.deadZonePhi = kDeadZonePhiInter;
			cIn1.zeroCoeffFraction = 0.35;
			const FrameProcessResult cpuRes1 = cpuEng.ProcessFrame(cIn1, nullptr);
			Assert::IsTrue(cpuRes1.carryUsed && cpuRes1.activeRefCount == 2u,
				L"CPU frame 1 must fuse 2 refs (next + carry) with prevPeak == nullptr");
			Assert::AreEqual(static_cast<size_t>(0u), cpuRes1.focusVetoCells,
				L"H5 high-contrast scene must have zero spurious focus veto cells");

			// 2. Zero-copy GPU ProcessFrameGpu with prepacked motionVecTex/motionMetaTex/cellPriorTex and prevPeak == nullptr!
			OwnedTex recTex(ctx.gpu), nextTex(ctx.gpu), outYTex(ctx.gpu), outCleanTex(ctx.gpu), outDeltaTex(ctx.gpu);
			OwnedTex motVecTex(ctx.gpu), motMetaTex(ctx.gpu), priorTex(ctx.gpu);
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, nextTex.tex, w, h, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYTex.tex, w, h, fmtR32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCleanTex.tex, w, h, fmtR32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDeltaTex.tex, w, h, fmtRG32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, motVecTex.tex, 3 * cw, ch, fmtRGBA32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, motMetaTex.tex, 3 * cw, ch, fmtRG32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, priorTex.tex, cw, ch, fmtRG32, true, false, true, false));

			// Pack fixed slots: slot 0 = prev (poisoned), slot 1 = next (+dx, +dy), slot 2 = carry (-dx, -dy).
			std::vector<float> vecBuf(static_cast<size_t>(3 * cw * ch) * 4u, 0.0f);
			std::vector<float> metaBuf(static_cast<size_t>(3 * cw * ch) * 2u, 0.0f);
			auto writeSlot = [&](int slot, float vx, float vy, float cls, float valid)
			{
				for (int cy = 0; cy < ch; ++cy)
				{
					for (int cx = 0; cx < cw; ++cx)
					{
						const size_t t = static_cast<size_t>(cy) * static_cast<size_t>(3 * cw) + static_cast<size_t>(slot * cw + cx);
						vecBuf[t * 4u + 0u] = vx;
						vecBuf[t * 4u + 1u] = vy;
						vecBuf[t * 4u + 2u] = vx;
						vecBuf[t * 4u + 3u] = vy;
						metaBuf[t * 2u + 0u] = cls;
						metaBuf[t * 2u + 1u] = valid;
					}
				}
			};
			writeSlot(0, +11.0f, -9.0f, static_cast<float>(RegionMotionClass::Camera), 0.0f);
			writeSlot(1, static_cast<float>(+dx), static_cast<float>(+dy), static_cast<float>(RegionMotionClass::Camera), 1.0f);
			writeSlot(2, static_cast<float>(-dx), static_cast<float>(-dy), static_cast<float>(RegionMotionClass::Camera), 1.0f);
			Assert::IsTrue(Detail::UploadFloats(ctx.gpu, motVecTex.tex, vecBuf.data()));
			Assert::IsTrue(Detail::UploadFloats(ctx.gpu, motMetaTex.tex, metaBuf.data()));
			std::vector<float> priorBuf(static_cast<size_t>(cw * ch) * 2u, 0.0f);
			Assert::IsTrue(Detail::UploadFloats(ctx.gpu, priorTex.tex, priorBuf.data()));

			GpuEngineGuard zcGuard(ctx.gpu, cfg);
			// Seed carry on frame 0
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, f0Clean));
			{
				GpuFrameInput zIn0;
				zIn0.seq = 1000; zIn0.w = w; zIn0.h = h;
				zIn0.receivedYTex = recTex.tex;
				zIn0.cleanYTex = recTex.tex;
				zIn0.hasCleanSplit = true;
				zIn0.activeRect = full;
				GpuFrameOutput zOut0;
				zOut0.outputYTex = outYTex.tex;
				zOut0.outputCleanTex = outCleanTex.tex;
				Assert::IsTrue(zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zIn0, zOut0, nullptr, nullptr),
					L"H5: zero-copy seed frame failed");
				Assert::IsTrue(zcGuard.engine.HasCarry(), L"H5: zero-copy seed must establish carry");
			}

			// Active frame 1 with prevPeak == nullptr, nextPeak != nullptr, carry active, prepacked fixed slots!
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, f1Clean));
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, nextTex.tex, f2Clean));
			GpuFrameInput zIn1;
			zIn1.seq = 1001; zIn1.w = w; zIn1.h = h;
			zIn1.receivedYTex = recTex.tex;
			zIn1.cleanYTex = recTex.tex;
			zIn1.hasCleanSplit = true;
			zIn1.activeRect = full;
			zIn1.prevPeak = { nullptr, nullptr, false };
			zIn1.nextPeak = { nextTex.tex, nullptr, false };
			zIn1.motionVecTex = motVecTex.tex;
			zIn1.motionMetaTex = motMetaTex.tex;
			zIn1.cellPriorTex = priorTex.tex;
			zIn1.hasStepPrior = true;
			zIn1.estimatedStepDelta = 24.0;
			zIn1.deadZonePhi = kDeadZonePhiInter;
			zIn1.zeroCoeffFraction = 0.35;

			GpuFrameOutput zOut1;
			zOut1.outputYTex = outYTex.tex;
			zOut1.outputCleanTex = outCleanTex.tex;
			zOut1.tdDeltaTex = outDeltaTex.tex;
			zOut1.readbackCellDiagnostics = true;

			Assert::IsTrue(zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zIn1, zOut1, nullptr, nullptr),
				L"H5: prepacked fixed-slot ProcessFrameGpu with prevPeak==nullptr failed");
			Assert::IsTrue(zOut1.succeeded && zOut1.carryUsed && zOut1.activeRefCount == 2u,
				L"H5: prepacked frame 1 must fuse 2 refs (next + carry)");

			TdV2Plane gpuOutY, gpuOutClean;
			Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, gpuOutY));
			Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outCleanTex.tex, w, h, gpuOutClean));
			const double parityErrY = MaxAbsDiffFull(gpuOutY, cpuRes1.outputY);
			const double parityErrClean = MaxAbsDiffFull(gpuOutClean, cpuRes1.outputClean);
			Assert::IsTrue(parityErrY <= kGpuCpuParityTol,
				FormatMsg("H5: prepacked prevPeak=null outputY |GPU-CPU|=%.6f > 1e-3 (wrong motion slot?)", parityErrY).c_str());
			Assert::IsTrue(parityErrClean <= kGpuCpuParityTol,
				FormatMsg("H5: prepacked prevPeak=null outputClean |GPU-CPU|=%.6f > 1e-3", parityErrClean).c_str());

			// NEGATIVE CONTROL (Exact pre-fix H5 slot-compaction bug on the exact same frames!):
			// Warping carry with NEXT's vectors (+dx, +dy instead of -dx, -dy) diverges sharply from true output!
			{
				TdV2ReferenceEngine buggyEng(cfg);
				FrameProcessInput bIn0;
				bIn0.seq = 1000; bIn0.receivedY = f0Clean; bIn0.hasCleanSplit = true; bIn0.cleanY = f0Clean; bIn0.activeRect = full;
				buggyEng.ProcessFrame(bIn0, nullptr);
				FrameProcessInput bIn1 = cIn1;
				bIn1.carryMotion = mfNext; // WRONG: next's opposite-direction motion applied to carry!
				const FrameProcessResult buggyRes1 = buggyEng.ProcessFrame(bIn1, nullptr);
				const double slotBugDiff = MaxAbsDiffFull(buggyRes1.outputY, cpuRes1.outputY);
				const double trueRmse = std::sqrt(PlaneMse(gpuOutY, m1, full));
				const double buggyRmse = std::sqrt(PlaneMse(buggyRes1.outputY, m1, full));
				Assert::IsTrue(slotBugDiff > 5.0,
					FormatMsg("H5 negative control: wrong carry motion slot diff=%.3f <= 5.0 codes", slotBugDiff).c_str());
				Assert::IsTrue(buggyRmse > trueRmse + 0.5,
					FormatMsg("H5 negative control: wrong carry motion RMSE=%.3f not worse than true=%.3f", buggyRmse, trueRmse).c_str());
			}

			// 2b. Round-3 MEDIUM-1: Prepacked 3*cw motion + prevPeak==null + nextPeak!=null + prevPeak2!=null + active carry:
			//     prevPeak2 has NO fixed slot in 3*cw prepacked motion -> skips visibly (MotionFieldStandIn, motionStandInRefs>=1)
			//     while carry stays locked to fixed slot 2 (NEVER pushed onto poisoned slot 0!) matching CPU < 1e-3!
			{
				OwnedTex p2Tex(ctx.gpu), m1VecTex(ctx.gpu), m1MetaTex(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, p2Tex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, m1VecTex.tex, 3 * cw, ch, fmtRGBA32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, m1MetaTex.tex, 3 * cw, ch, fmtRG32, true, false, true, false));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, p2Tex.tex, f0Clean));

				// Poison slot 0 with valid=1.0 and bogus (vx=12, vy=12) so any slot-0 hijack would severely corrupt output!
				std::vector<float> m1VecBuf(static_cast<size_t>(3 * cw * ch) * 4u, 0.0f);
				std::vector<float> m1MetaBuf(static_cast<size_t>(3 * cw * ch) * 2u, 0.0f);
				auto writeM1Slot = [&](int slot, float vx, float vy, float cls, float valid)
				{
					for (int cy = 0; cy < ch; ++cy)
					{
						for (int cx = 0; cx < cw; ++cx)
						{
							const size_t t = static_cast<size_t>(cy) * static_cast<size_t>(3 * cw) + static_cast<size_t>(slot * cw + cx);
							m1VecBuf[t * 4u + 0u] = vx;  m1VecBuf[t * 4u + 1u] = vy;
							m1VecBuf[t * 4u + 2u] = vx;  m1VecBuf[t * 4u + 3u] = vy;
							m1MetaBuf[t * 2u + 0u] = cls; m1MetaBuf[t * 2u + 1u] = valid;
						}
					}
				};
				writeM1Slot(0, +12.0f, +12.0f, static_cast<float>(RegionMotionClass::Camera), 1.0f);
				writeM1Slot(1, static_cast<float>(+dx), static_cast<float>(+dy), static_cast<float>(RegionMotionClass::Camera), 1.0f);
				writeM1Slot(2, static_cast<float>(-dx), static_cast<float>(-dy), static_cast<float>(RegionMotionClass::Camera), 1.0f);
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, m1VecTex.tex, m1VecBuf.data()));
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, m1MetaTex.tex, m1MetaBuf.data()));

				GpuEngineGuard m1Guard(ctx.gpu, cfg);
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, f0Clean));
				{
					GpuFrameInput sIn0;
					sIn0.seq = 1000; sIn0.w = w; sIn0.h = h;
					sIn0.receivedYTex = recTex.tex; sIn0.cleanYTex = recTex.tex; sIn0.hasCleanSplit = true; sIn0.activeRect = full;
					GpuFrameOutput sOut0; sOut0.outputYTex = outYTex.tex; sOut0.outputCleanTex = outCleanTex.tex;
					Assert::IsTrue(m1Guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, sIn0, sOut0, nullptr, nullptr));
					Assert::IsTrue(m1Guard.engine.HasCarry());
				}

				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, f1Clean));
				GpuFrameInput m1In = zIn1;
				m1In.prevPeak  = { nullptr, nullptr, false };
				m1In.nextPeak  = { nextTex.tex, nullptr, false };
				m1In.prevPeak2 = { p2Tex.tex, nullptr, false };
				m1In.motionVecTex  = m1VecTex.tex;
				m1In.motionMetaTex = m1MetaTex.tex;

				GpuFrameOutput m1Out;
				m1Out.outputYTex = outYTex.tex;
				m1Out.outputCleanTex = outCleanTex.tex;
				m1Out.tdDeltaTex = outDeltaTex.tex;
				m1Out.readbackCellDiagnostics = true;

				StandInRegistry m1Si;
				Assert::IsTrue(m1Guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, m1In, m1Out, nullptr, &m1Si),
					L"Round-3 MEDIUM-1: prepacked prevPeak2+carry ProcessFrameGpu failed");
				Assert::IsTrue(m1Out.succeeded && m1Out.carryUsed && m1Out.activeRefCount == 2u,
					L"Round-3 MEDIUM-1: must fuse next + carry (2 refs) while skipping slotless prevPeak2");
				Assert::IsTrue(m1Out.motionStandInRefs >= 1u,
					L"Round-3 MEDIUM-1: skipped prevPeak2 under 3*cw prepacked motion must increment motionStandInRefs >= 1");
				Assert::IsTrue(m1Si.IsActive(StandInKind::MotionFieldStandIn),
					L"Round-3 MEDIUM-1: skipped prevPeak2 under 3*cw prepacked motion must engage StandInKind::MotionFieldStandIn");

				TdV2Plane m1OutY;
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, m1OutY));
				const double m1ParityErr = MaxAbsDiffFull(m1OutY, cpuRes1.outputY);
				Assert::IsTrue(m1ParityErr < kGpuCpuParityTol,
					FormatMsg("Round-3 MEDIUM-1: carry slot-2 parity |GPU-CPU|=%.6f >= 1e-3 (carry hijacked onto poisoned slot 0?)",
						m1ParityErr).c_str());
			}

			// 3. Strict dimension-mismatch rejection tests for motionVecTex, motionMetaTex, and cellPriorTex!
			OwnedTex badVecTex(ctx.gpu), badMetaTex(ctx.gpu), badPriorTex(ctx.gpu);
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, badVecTex.tex, 2 * cw, ch, fmtRGBA32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, badMetaTex.tex, 3 * cw, ch + 1, fmtRG32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, badPriorTex.tex, cw + 1, ch, fmtRG32, true, false, true, false));

			struct BadDimCase { const char* name; pl_tex mv; pl_tex mm; pl_tex cp; };
			const BadDimCase badDims[3] = {
				{ "motionVecTex-badW",   badVecTex.tex,  motMetaTex.tex, priorTex.tex },
				{ "motionMetaTex-badH",  motVecTex.tex,  badMetaTex.tex, priorTex.tex },
				{ "cellPriorTex-badW",   motVecTex.tex,  motMetaTex.tex, badPriorTex.tex }
			};
			for (const BadDimCase& bc : badDims)
			{
				GpuFrameInput badIn = zIn1;
				badIn.motionVecTex = bc.mv;
				badIn.motionMetaTex = bc.mm;
				badIn.cellPriorTex = bc.cp;
				GpuFrameOutput badOut;
				badOut.outputYTex = outYTex.tex;
				StandInRegistry badSi;
				const uint64_t fBefore = zcGuard.engine.Failures();
				const bool ok = zcGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, badIn, badOut, nullptr, &badSi);
				Assert::IsFalse(ok, FormatTagMsg(bc.name, "Mis-sized prepacked texture must fail visibly").c_str());
				Assert::IsFalse(badOut.succeeded, FormatTagMsg(bc.name, "out.succeeded must be false").c_str());
				Assert::IsFalse(badOut.lastError.empty(), FormatTagMsg(bc.name, "out.lastError must be populated").c_str());
				Assert::IsTrue(badSi.IsActive(StandInKind::GpuDispatchFailed),
					FormatTagMsg(bc.name, "Must engage StandInKind::GpuDispatchFailed").c_str());
				Assert::AreEqual(fBefore + 1u, zcGuard.engine.Failures(),
					FormatTagMsg(bc.name, "Must increment Failures()").c_str());
			}
		}

		// =========================================================================
		// 11. H6: ProcessFrameGpu Rejects Any Input/Output pl_tex Aliasing Visibly
		//     (StandInKind::GpuInputOutputAlias) + Live WARP SRV/UAV Hazard Proof
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(ProcessFrameGpuRejectsInputOutputTextureAliasingVisibly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ProcessFrameGpuRejectsInputOutputTextureAliasingVisibly)
		{
			WarpGpuContext ctx;

			struct OwnedTex
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				explicit OwnedTex(pl_gpu g) : gpu(g) {}
				~OwnedTex() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				OwnedTex(const OwnedTex&) = delete;
				OwnedTex& operator=(const OwnedTex&) = delete;
			};

			const int w = 64, h = 64;
			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
			Assert::IsNotNull(fmtR32);

			// ---- MANDATORY NEGATIVE CONTROL (Real D3D11 WARP SRV/UAV aliasing hazard proof):
			//      Binding the SAME R32F texture (filled with 420.0f) as both sampled and storage in a raw compute pass
			//      either fails dispatch or reads back corrupted/zeroed data (D3D11 unbinds SRV when UAV aliases)! ----
			{
				OwnedTex aliasRaw(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, aliasRaw.tex, 4, 1, fmtR32, true, true, true, true));
				const float initVals[4] = { 420.0f, 420.0f, 420.0f, 420.0f };
				Assert::IsTrue(Detail::UploadFloats(ctx.gpu, aliasRaw.tex, initVals));
				const char* rawAliasBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= 4 || p.y >= 1) return;
float v = texelFetch(src_alias, p, 0).x;
imageStore(dst_alias, p, vec4(v + 1.0, 0.0, 0.0, 0.0));
)GLSL";
				pl_shader_desc rawD[2] = {
					VpvGpu::Sampled("src_alias", aliasRaw.tex),
					VpvGpu::Storage("dst_alias", aliasRaw.tex)
				};
				const bool rawOk = VpvGpu::Pass(ctx.dispatch, "td_v2_negctrl_srv_uav_alias", "", rawAliasBody,
				                                rawD, 2, nullptr, 0, 1, 1, 0, 4, 1);
				float gotVals[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				const bool dlOk = rawOk && Detail::DownloadFloats(ctx.gpu, aliasRaw.tex, gotVals);
				const bool corrupted = !dlOk || (std::fabs(gotVals[0] - 421.0f) > 0.5f);
				Assert::IsTrue(corrupted,
					FormatMsg("H6 negative control: raw D3D11 SRV/UAV alias unexpectedly produced %.2f (expected fail or corrupted != 421)",
						static_cast<double>(gotVals[0])).c_str());
			}

			ComplexSceneBundle f1 = BuildComplexFrame(1, 1.5, 0.5, false);
			OwnedTex recTex(ctx.gpu), cleanTex(ctx.gpu), prevTex(ctx.gpu), nextTex(ctx.gpu);
			OwnedTex outYTex(ctx.gpu), outCleanTex(ctx.gpu);
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, true, true, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, cleanTex.tex, w, h, fmtR32, true, true, true, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, prevTex.tex, w, h, fmtR32, true, true, true, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, nextTex.tex, w, h, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYTex.tex, w, h, fmtR32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCleanTex.tex, w, h, fmtR32, true, true, false, true));

			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, f1.receivedY));
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, cleanTex.tex, f1.curClean));
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, prevTex.tex, f1.prevClean));
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, nextTex.tex, f1.nextClean));

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;
			cfg.enableCarryForward = false;
			GpuEngineGuard guard(ctx.gpu, cfg);

			GpuFrameInput baseIn;
			baseIn.seq = 1100; baseIn.w = w; baseIn.h = h;
			baseIn.receivedYTex = recTex.tex;
			baseIn.cleanYTex = cleanTex.tex;
			baseIn.hasCleanSplit = true;
			baseIn.activeRect = f1.active;
			baseIn.prevPeak = { prevTex.tex, &f1.prevMf, false };
			baseIn.nextPeak = { nextTex.tex, &f1.nextMf, false };
			baseIn.hasStepPrior = true;
			baseIn.estimatedStepDelta = 24.0;
			baseIn.zeroCoeffFraction = 0.35;

			struct AliasCase { const char* name; pl_tex outY; pl_tex outClean; };
			const AliasCase cases[5] = {
				{ "recY==outY",         recTex.tex,   outCleanTex.tex },
				{ "curClean==outClean", outYTex.tex,  cleanTex.tex    },
				{ "recY==outClean",     outYTex.tex,  recTex.tex      },
				{ "prevPeak==outY",     prevTex.tex,  outCleanTex.tex },
				{ "outY==outClean",     outYTex.tex,  outYTex.tex     }
			};

			for (const AliasCase& ac : cases)
			{
				GpuFrameOutput out;
				out.outputYTex = ac.outY;
				out.outputCleanTex = ac.outClean;
				StandInRegistry si;
				const uint64_t failsBefore = guard.engine.Failures();
				const bool ok = guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, baseIn, out, nullptr, &si);
				Assert::IsFalse(ok, FormatTagMsg(ac.name, "Aliased ProcessFrameGpu must return false").c_str());
				Assert::IsFalse(out.succeeded, FormatTagMsg(ac.name, "Aliased out.succeeded must be false").c_str());
				Assert::AreEqual(0, out.dispatchCount, FormatTagMsg(ac.name, "Aliased ProcessFrameGpu must issue 0 dispatches").c_str());
				Assert::IsFalse(out.lastError.empty(), FormatTagMsg(ac.name, "Aliased out.lastError must be set").c_str());
				Assert::IsTrue(si.IsActive(StandInKind::GpuInputOutputAlias),
					FormatTagMsg(ac.name, "Aliased ProcessFrameGpu MUST engage StandInKind::GpuInputOutputAlias").c_str());
				Assert::AreEqual(failsBefore + 1u, guard.engine.Failures(),
					FormatTagMsg(ac.name, "Aliased ProcessFrameGpu must increment Failures()").c_str());
			}

			// LOW-4: Aliasing on the seed/cut path (prevPeak==null, nextPeak==null, curClean==outClean & outY==outClean)
			// must ALSO fail visibly with StandInKind::GpuInputOutputAlias and ZERO dispatches!
			for (bool isCut : { false, true })
			{
				GpuFrameInput seedAliasIn = baseIn;
				seedAliasIn.seq = isCut ? 1102 : 1101;
				seedAliasIn.prevPeak = { nullptr, nullptr, false };
				seedAliasIn.nextPeak = { nullptr, nullptr, false };
				seedAliasIn.isCutOrSpanInvalid = isCut;
				GpuFrameOutput seedOut;
				seedOut.outputYTex = outYTex.tex;
				seedOut.outputCleanTex = cleanTex.tex; // Aliased with curCleanTex!
				StandInRegistry seedSi;
				const uint64_t failsBefore = guard.engine.Failures();
				const bool ok = guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, seedAliasIn, seedOut, nullptr, &seedSi);
				Assert::IsFalse(ok, L"LOW-4: Seed/cut curClean==outClean alias must return false");
				Assert::IsFalse(seedOut.succeeded, L"LOW-4: Seed/cut alias must set succeeded=false");
				Assert::AreEqual(0, seedOut.dispatchCount, L"LOW-4: Seed/cut alias must issue 0 dispatches");
				Assert::IsTrue(seedSi.IsActive(StandInKind::GpuInputOutputAlias),
					L"LOW-4: Seed/cut alias must engage StandInKind::GpuInputOutputAlias");
				Assert::AreEqual(failsBefore + 1u, guard.engine.Failures(),
					L"LOW-4: Seed/cut alias must increment Failures()");
			}

			// Positive control: exact same input with DISTINCT output textures succeeds cleanly!
			{
				GpuFrameOutput okOut;
				okOut.outputYTex = outYTex.tex;
				okOut.outputCleanTex = outCleanTex.tex;
				StandInRegistry okSi;
				Assert::IsTrue(guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, baseIn, okOut, nullptr, &okSi),
					L"H6 positive control: distinct input/output textures must succeed");
				Assert::IsTrue(okOut.succeeded);
				Assert::IsFalse(okSi.IsActive(StandInKind::GpuInputOutputAlias));
				TdV2Plane dlY;
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, dlY));
				Assert::IsTrue(MaxAbsDiffRect(dlY, f1.receivedY, f1.active) > 2.0,
					L"H6 positive control: distinct output must apply genuine repair");
			}
		}

		// =========================================================================
		// 12. H13 GPU: Pass-1 Face Mask (CPU-upload faceMask & Zero-Copy faceMaskTex)
		//     Forces Exact delta == 0 (out == in) and Engages StandInKind::FaceNonRigidMask
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuPass1FaceMaskForcesExactZeroDeltaOnCpuUploadAndZeroCopy)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuPass1FaceMaskForcesExactZeroDeltaOnCpuUploadAndZeroCopy)
		{
			WarpGpuContext ctx;

			struct OwnedTex
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				explicit OwnedTex(pl_gpu g) : gpu(g) {}
				~OwnedTex() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				OwnedTex(const OwnedTex&) = delete;
				OwnedTex& operator=(const OwnedTex&) = delete;
			};

			const int w = 64, h = 64;
			const int cw = w / 16, ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);
			const ActiveRect faceRect{ 16, 16, 48, 48 }; // Central 2x2 face cells: (1,1),(2,1),(1,2),(2,2)
			const ActiveRect bgRect{ 0, 0, 16, 16 };     // Unmasked background cell (0,0)

			const double dx = 1.5, dy = 0.5;
			const TdV2Plane mCur  = RenderScene(w, h, 1.0 * dx, 1.0 * dy);
			const TdV2Plane mPrev = RenderScene(w, h, 0.0, 0.0);
			const TdV2Plane mNext = RenderScene(w, h, 2.0 * dx, 2.0 * dy);
			const TdV2Plane curRec  = AddCodingDamage(mCur,  8.0, 12001u, 0.25, full);
			const TdV2Plane prevRef = AddCodingDamage(mPrev, 1.2, 12002u, 0.0,  full);
			const TdV2Plane nextRef = AddCodingDamage(mNext, 1.2, 12003u, 0.0,  full);
			const CellMotionField prevMf = CellMotionField::Uniform(cw, ch, -dx, -dy, -dx, -dy, RegionMotionClass::Camera);
			const CellMotionField nextMf = CellMotionField::Uniform(cw, ch, +dx, +dy, +dx, +dy, RegionMotionClass::Camera);

			std::vector<uint8_t> faceMask(static_cast<size_t>(cw * ch), 0u);
			faceMask[1u * cw + 1u] = 1u;
			faceMask[1u * cw + 2u] = 1u;
			faceMask[2u * cw + 1u] = 1u;
			faceMask[2u * cw + 2u] = 1u;

			FrameProcessInput in;
			in.seq = 1200;
			in.receivedY = curRec;
			in.hasCleanSplit = true;
			in.cleanY = curRec;
			in.activeRect = full;
			in.prevPeak = { &prevRef, &prevMf, false };
			in.nextPeak = { &nextRef, &nextMf, false };
			in.hasStepPrior = true;
			in.estimatedStepDelta = 24.0;
			in.zeroCoeffFraction = 0.35;
			in.faceMask = &faceMask;

			Config cfg = MakePreset(Preset::Reference, Mode::On);
			cfg.enableLongAnchor = false;
			cfg.enableCarryForward = false;

			// 1. CPU reference vs GPU CPU-upload path (faceMask)
			TdV2ReferenceEngine cpuEng(cfg);
			GpuEngineGuard gpuGuard(ctx.gpu, cfg);
			StandInRegistry cpuSi, gpuSi;
			const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in, &cpuSi);
			const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in, nullptr, &gpuSi);

			TdV2Plane curRecF32(w, h, 0.0);
			for (size_t i = 0; i < curRec.v.size(); ++i)
				curRecF32.v[i] = static_cast<double>(static_cast<float>(curRec.v[i]));

			AssertCpuGpuFrameParity(cpuRes, gpuRes, curRec, curRec, full, "H13-CpuUploadFaceMask");
			Assert::IsTrue(cpuSi.IsActive(StandInKind::FaceNonRigidMask), L"CPU must engage FaceNonRigidMask");
			Assert::IsTrue(gpuSi.IsActive(StandInKind::FaceNonRigidMask), L"GPU CPU-upload must engage FaceNonRigidMask");
			Assert::AreEqual(0.0, MaxAbsPlane(gpuRes.tdDelta, faceRect), L"CPU-upload masked face tdDelta must be exact 0.0");
			Assert::AreEqual(0.0, MaxAbsDiffRect(gpuRes.outputY, curRecF32, faceRect),
				L"CPU-upload masked face outputY must equal R32F receivedY bit-exact");
			Assert::IsTrue(MaxAbsPlane(gpuRes.tdDelta, bgRect) > 1.5,
				L"Unmasked background cell must still receive active repair (> 1.5 codes)");

			// 2. Zero-copy GPU path with pl_tex faceMaskTex (cw x ch R32F)
			const pl_fmt fmtR32  = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
			OwnedTex recTex(ctx.gpu), prevTex(ctx.gpu), nextTex(ctx.gpu), maskTex(ctx.gpu);
			OwnedTex outYTex(ctx.gpu), outCleanTex(ctx.gpu), outDeltaTex(ctx.gpu);
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, prevTex.tex, w, h, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, nextTex.tex, w, h, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, maskTex.tex, cw, ch, fmtR32, true, false, true, false));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYTex.tex, w, h, fmtR32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCleanTex.tex, w, h, fmtR32, true, true, false, true));
			Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDeltaTex.tex, w, h, fmtRG32, true, true, false, true));

			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, curRec));
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, prevTex.tex, prevRef));
			Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, nextTex.tex, nextRef));
			std::vector<float> maskFloats(static_cast<size_t>(cw * ch), 0.0f);
			for (size_t i = 0; i < faceMask.size(); ++i)
				maskFloats[i] = faceMask[i] ? 1.0f : 0.0f;
			Assert::IsTrue(Detail::UploadFloats(ctx.gpu, maskTex.tex, maskFloats.data()));

			GpuFrameInput zIn;
			zIn.seq = 1201; zIn.w = w; zIn.h = h;
			zIn.receivedYTex = recTex.tex;
			zIn.cleanYTex = recTex.tex;
			zIn.hasCleanSplit = true;
			zIn.activeRect = full;
			zIn.prevPeak = { prevTex.tex, &prevMf, false };
			zIn.nextPeak = { nextTex.tex, &nextMf, false };
			zIn.faceMaskTex = maskTex.tex;
			zIn.hasStepPrior = true;
			zIn.estimatedStepDelta = 24.0;
			zIn.zeroCoeffFraction = 0.35;

			GpuFrameOutput zOut;
			zOut.outputYTex = outYTex.tex;
			zOut.outputCleanTex = outCleanTex.tex;
			zOut.tdDeltaTex = outDeltaTex.tex;
			zOut.readbackCellDiagnostics = true;

			StandInRegistry zcSi;
			Assert::IsTrue(gpuGuard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zIn, zOut, nullptr, &zcSi),
				L"H13: zero-copy ProcessFrameGpu with faceMaskTex failed");
			Assert::IsTrue(zcSi.IsActive(StandInKind::FaceNonRigidMask),
				L"H13: zero-copy faceMaskTex MUST engage StandInKind::FaceNonRigidMask");

			TdV2Plane zOutY, zDelta;
			bool anyBnd = false;
			Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, zOutY));
			Assert::IsTrue(Detail::DownloadDeltaRG32F(ctx.gpu, outDeltaTex.tex, w, h, zDelta, anyBnd));
			Assert::AreEqual(0.0, MaxAbsPlane(zDelta, faceRect), L"Zero-copy masked face tdDelta must be exact 0.0");
			Assert::AreEqual(0.0, MaxAbsDiffRect(zOutY, curRecF32, faceRect),
				L"Zero-copy masked face outputY must equal R32F receivedY bit-exact");
			Assert::IsTrue(MaxAbsDiffFull(zOutY, gpuRes.outputY) <= 1e-4,
				L"Zero-copy faceMaskTex outputY must match CPU-upload faceMask within 1e-4");

			// 3. NEGATIVE CONTROL (Rule 3): Exact same GPU inputs WITHOUT faceMask / faceMaskTex produce
			//    large non-zero delta (> 2.0 codes) on the exact same face cells!
			{
				FrameProcessInput unmaskedIn = in;
				unmaskedIn.faceMask = nullptr;
				StandInRegistry umSi;
				const GpuProcessResult umRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, unmaskedIn, nullptr, &umSi);
				Assert::IsTrue(umRes.succeeded);
				Assert::IsFalse(umSi.IsActive(StandInKind::FaceNonRigidMask),
					L"Negative control: unmasked run must NOT engage FaceNonRigidMask");
				Assert::IsTrue(MaxAbsPlane(umRes.tdDelta, faceRect) > 2.0,
					L"Negative control: unmasked GPU run must produce non-zero delta (> 2.0 codes) on face cells");
				Assert::IsTrue(MaxAbsDiffRect(umRes.outputY, curRec, faceRect) > 2.0,
					L"Negative control: unmasked GPU run must modify face cells (> 2.0 codes)");
			}
		}

		// =========================================================================
		// 13. GPU Hygiene & Geometry:
		//     (a) Cut/Seed path zeros pre-dirtied tdDelta/tdMaps/diag & populates out.tdMaps
		//     (b) Zero-copy rejects mis-sized ref textures visibly
		//     (c) Zero-copy closes single injected NaN cleanly without 7x7 NaN spread
		//     (d) Non-16-multiple geometry (70x54, pillarbox+letterbox [8..62)x[4..50))
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuCutSeedZeroHygieneZeroCopyRefValidationSingleNanClosureAndNon16PillarboxGeometry)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuCutSeedZeroHygieneZeroCopyRefValidationSingleNanClosureAndNon16PillarboxGeometry)
		{
			WarpGpuContext ctx;

			struct OwnedTex
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				explicit OwnedTex(pl_gpu g) : gpu(g) {}
				~OwnedTex() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				OwnedTex(const OwnedTex&) = delete;
				OwnedTex& operator=(const OwnedTex&) = delete;
			};

			const pl_fmt fmtR32    = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32   = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(ctx.gpu, VpSkeleton::TexFormat::RGBA32F);
			Assert::IsNotNull(fmtR32); Assert::IsNotNull(fmtRG32); Assert::IsNotNull(fmtRGBA32);

			// ---- Part A: Cut/Seed Path Explicitly Zeros Pre-Dirtied Outputs & Populates out.tdMaps ----
			{
				const int w = 64, h = 64, cw = 4, ch = 4;
				OwnedTex recTex(ctx.gpu), outYTex(ctx.gpu), outCleanTex(ctx.gpu);
				OwnedTex outDeltaTex(ctx.gpu), outMapsTex(ctx.gpu), outDiagTex(ctx.gpu), outGuardTex(ctx.gpu), outWarpDiagTex(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYTex.tex, w, h, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCleanTex.tex, w, h, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDeltaTex.tex, w, h, fmtRG32, true, true, true, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outMapsTex.tex, cw, 3 * ch, fmtRGBA32, true, true, true, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDiagTex.tex, cw, ch, fmtRGBA32, true, true, true, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outGuardTex.tex, cw, ch, fmtRG32, true, true, true, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outWarpDiagTex.tex, cw, ch, fmtRG32, true, true, true, true));

				const TdV2Plane frame0 = RenderScene(w, h, 0.0, 0.0);
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, frame0));

				auto dirtyAllTargets = [&]()
				{
					std::vector<float> dDelta(static_cast<size_t>(w * h * 2), 99.0f);
					std::vector<float> dMaps(static_cast<size_t>(cw * 3 * ch * 4), 99.0f);
					std::vector<float> dDiag(static_cast<size_t>(cw * ch * 4), 99.0f);
					std::vector<float> dCell2(static_cast<size_t>(cw * ch * 2), 99.0f);
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, outDeltaTex.tex, dDelta.data()));
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, outMapsTex.tex, dMaps.data()));
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, outDiagTex.tex, dDiag.data()));
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, outGuardTex.tex, dCell2.data()));
					Assert::IsTrue(Detail::UploadFloats(ctx.gpu, outWarpDiagTex.tex, dCell2.data()));
					// Negative control: confirm sentinel 99.0f is genuinely present on GPU before cut/seed!
					std::vector<float> checkDelta(static_cast<size_t>(w * h * 2), 0.0f);
					Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, outDeltaTex.tex, checkDelta.data()));
					Assert::AreEqual(99.0f, checkDelta[0], L"Setup: pre-dirtied sentinel must be 99.0f");
				};

				auto assertAllTargetsZeroed = [&](const GpuFrameOutput& out, const wchar_t* tag)
				{
					Assert::IsTrue(out.succeeded, tag);
					Assert::AreEqual(static_cast<size_t>(cw * ch), out.tdMaps.size(),
						L"Cut/seed with readbackCellDiagnostics=true must populate non-empty out.tdMaps");
					for (const auto& blk : out.tdMaps)
						Assert::AreEqual(0.0f, blk.f[TdMapsView::kValidity], L"Cut/seed out.tdMaps kValidity must be 0.0f");

					auto checkTexZero = [&](pl_tex tex, size_t nFloats, const wchar_t* label)
					{
						std::vector<float> buf(nFloats, 1.0f);
						Assert::IsTrue(Detail::DownloadFloats(ctx.gpu, tex, buf.data()), label);
						for (float v : buf)
							Assert::AreEqual(0.0f, v, label);
					};
					checkTexZero(outDeltaTex.tex, static_cast<size_t>(w * h * 2), L"Cut/seed must zero tdDeltaTex");
					checkTexZero(outMapsTex.tex, static_cast<size_t>(cw * 3 * ch * 4), L"Cut/seed must zero tdMapsTex");
					checkTexZero(outDiagTex.tex, static_cast<size_t>(cw * ch * 4), L"Cut/seed must zero diagFlagsTex");
					checkTexZero(outGuardTex.tex, static_cast<size_t>(cw * ch * 2), L"Cut/seed must zero guardCellTex");
					checkTexZero(outWarpDiagTex.tex, static_cast<size_t>(cw * ch * 2), L"Cut/seed must zero warpDiagTex");
				};

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				GpuEngineGuard guard(ctx.gpu, cfg);

				for (bool isCut : { false, true })
				{
					dirtyAllTargets();
					GpuFrameInput zIn;
					zIn.seq = isCut ? 1302 : 1301;
					zIn.w = w; zIn.h = h;
					zIn.receivedYTex = recTex.tex;
					zIn.cleanYTex = recTex.tex;
					zIn.hasCleanSplit = true;
					zIn.activeRect = ActiveRect::Full(w, h);
					zIn.isCutOrSpanInvalid = isCut;

					GpuFrameOutput zOut;
					zOut.outputYTex = outYTex.tex;
					zOut.outputCleanTex = outCleanTex.tex;
					zOut.tdDeltaTex = outDeltaTex.tex;
					zOut.tdMapsTex = outMapsTex.tex;
					zOut.diagFlagsTex = outDiagTex.tex;
					zOut.guardCellTex = outGuardTex.tex;
					zOut.warpDiagTex = outWarpDiagTex.tex;
					zOut.readbackCellDiagnostics = true;

					Assert::IsTrue(guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, zIn, zOut, nullptr, nullptr),
						isCut ? L"Cut ProcessFrameGpu failed" : L"Seed ProcessFrameGpu failed");
					assertAllTargetsZeroed(zOut, isCut ? L"Cut hygiene failed" : L"Seed hygiene failed");
				}
			}

			// ---- Part B & C: Zero-Copy Ref Dimension Validation & Single-NaN Closure (No 7x7 Spread!) ----
			{
				const int w = 64, h = 64, cw = 4, ch = 4;
				const ActiveRect full = ActiveRect::Full(w, h);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				OwnedTex recTex(ctx.gpu), cleanTex(ctx.gpu), goodRefTex(ctx.gpu), wrongRefTex(ctx.gpu);
				OwnedTex outYTex(ctx.gpu), outCleanTex(ctx.gpu), outDeltaTex(ctx.gpu);
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, recTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, cleanTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, goodRefTex.tex, w, h, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, wrongRefTex.tex, 32, 32, fmtR32, true, false, true, false));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outYTex.tex, w, h, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outCleanTex.tex, w, h, fmtR32, true, true, false, true));
				Assert::IsTrue(Detail::EnsureTex(ctx.gpu, outDeltaTex.tex, w, h, fmtRG32, true, true, false, true));

				const TdV2Plane baseScene = RenderScene(w, h, 0.0, 0.0);
				const TdV2Plane curNoisy  = AddCodingDamage(baseScene, 7.0, 1310u, 0.2, full);
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, recTex.tex, curNoisy));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, cleanTex.tex, curNoisy));
				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, goodRefTex.tex, baseScene));

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;
				GpuEngineGuard guard(ctx.gpu, cfg);

				// (B) Mis-sized ref texture (32x32 for 64x64 frame) rejected visibly for BOTH prevPeak and nextPeak!
				for (bool testNext : { false, true })
				{
					GpuFrameInput badRefIn;
					badRefIn.seq = testNext ? 1312 : 1311;
					badRefIn.w = w; badRefIn.h = h;
					badRefIn.receivedYTex = recTex.tex;
					badRefIn.cleanYTex = cleanTex.tex;
					badRefIn.hasCleanSplit = true;
					badRefIn.activeRect = full;
					badRefIn.prevPeak = { testNext ? goodRefTex.tex : wrongRefTex.tex, &stillMf, false };
					badRefIn.nextPeak = { testNext ? wrongRefTex.tex : goodRefTex.tex, &stillMf, false };
					GpuFrameOutput badOut;
					badOut.outputYTex = outYTex.tex;
					StandInRegistry badSi;
					const bool ok = guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, badRefIn, badOut, nullptr, &badSi);
					Assert::IsFalse(ok, L"Zero-copy mis-sized ref texture must return false");
					Assert::IsFalse(badOut.succeeded, L"Zero-copy mis-sized ref texture must set succeeded=false");
					Assert::IsFalse(badOut.lastError.empty(), L"Zero-copy mis-sized ref texture must set lastError");
					Assert::IsTrue(badSi.IsActive(StandInKind::GpuDispatchFailed),
						L"Zero-copy mis-sized ref texture must engage GpuDispatchFailed");
				}

				// (C) Single injected NaN pixel in zero-copy curCleanTex: MUST close cleanly without a 7x7 NaN spread!
				TdV2Plane singleNanPlane = curNoisy;
				singleNanPlane.at(24, 24) = std::numeric_limits<double>::quiet_NaN();

				// Negative control (Rule 3): unguarded Gaussian band decomposition spreads 1 NaN into a >= 7x7 (49-px) NaN block!
				{
					const ThreeBandPlanes unguardedBands = DecomposeActiveBands(singleNanPlane, full);
					size_t spreadNaNs = 0;
					for (int y = 21; y <= 27; ++y)
						for (int x = 21; x <= 27; ++x)
							if (!std::isfinite(unguardedBands.H.at(x, y)) || !std::isfinite(unguardedBands.M.at(x, y)))
								++spreadNaNs;
					Assert::AreEqual(static_cast<size_t>(49u), spreadNaNs,
						L"Negative control: unguarded Gaussian blur of single NaN must poison all 49 pixels in 7x7 block");
				}

				Assert::IsTrue(Detail::UploadPlaneR32F(ctx.gpu, cleanTex.tex, singleNanPlane));
				GpuFrameInput nanIn;
				nanIn.seq = 1313; nanIn.w = w; nanIn.h = h;
				nanIn.receivedYTex = recTex.tex;
				nanIn.cleanYTex = cleanTex.tex;
				nanIn.hasCleanSplit = true;
				nanIn.activeRect = full;
				nanIn.prevPeak = { goodRefTex.tex, &stillMf, false };
				nanIn.nextPeak = { goodRefTex.tex, &stillMf, false };
				nanIn.hasStepPrior = true;
				nanIn.estimatedStepDelta = 20.0;
				nanIn.zeroCoeffFraction = 0.35;

				GpuFrameOutput nanOut;
				nanOut.outputYTex = outYTex.tex;
				nanOut.outputCleanTex = outCleanTex.tex;
				nanOut.tdDeltaTex = outDeltaTex.tex;
				nanOut.readbackCellDiagnostics = true;

				StandInRegistry nanSi;
				Assert::IsTrue(guard.engine.ProcessFrameGpu(ctx.gpu, ctx.dispatch, nanIn, nanOut, nullptr, &nanSi),
					L"Zero-copy single injected NaN must complete cleanly");
				Assert::IsTrue(nanSi.IsActive(StandInKind::NonFiniteClosure),
					L"Zero-copy single injected NaN must engage StandInKind::NonFiniteClosure");

				TdV2Plane nanOutY, nanOutClean, nanDelta;
				bool anyBnd = false;
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outYTex.tex, w, h, nanOutY));
				Assert::IsTrue(Detail::DownloadPlaneR32F(ctx.gpu, outCleanTex.tex, w, h, nanOutClean));
				Assert::IsTrue(Detail::DownloadDeltaRG32F(ctx.gpu, outDeltaTex.tex, w, h, nanDelta, anyBnd));
				AssertAllFinitePlane(nanOutY, L"Zero-copy single-NaN outputY must be 100% finite (zero 7x7 NaN spread!)");
				AssertAllFinitePlane(nanOutClean, L"Zero-copy single-NaN outputClean must be 100% finite (zero 7x7 NaN spread!)");
				AssertAllFinitePlane(nanDelta, L"Zero-copy single-NaN tdDelta must be 100% finite (zero 7x7 NaN spread!)");
				AssertAllFiniteTdMaps(nanOut.tdMaps, L"Zero-copy single-NaN tdMaps must be 100% finite");
			}

			// ---- Part D: Non-64x64 / Non-Multiple-of-16 Geometry (70x54, Pillarbox + Letterbox [8..62)x[4..50)) ----
			{
				const int w = 70, h = 54;
				const int cw = (w + 15) / 16; // 5 (right partial edge cell = 6 px)
				const int ch = (h + 15) / 16; // 4 (bottom partial edge cell = 6 px)
				const ActiveRect active{ 8, 4, 62, 50 };
				const double dx = 1.5, dy = 0.5;

				auto fillAllBars = [&](TdV2Plane& p, double barVal)
				{
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
							if (!active.Contains(x, y))
								p.at(x, y) = barVal;
				};

				const TdV2Plane mCur  = RenderScene(w, h, 1.0 * dx, 1.0 * dy);
				const TdV2Plane mPrev = RenderScene(w, h, 0.0, 0.0);
				const TdV2Plane mNext = RenderScene(w, h, 2.0 * dx, 2.0 * dy);
				TdV2Plane curRec  = AddCodingDamage(mCur,  8.0, 1341u, 0.25, active);
				TdV2Plane prevRef = AddCodingDamage(mPrev, 1.2, 1342u, 0.0,  active);
				TdV2Plane nextRef = AddCodingDamage(mNext, 1.2, 1343u, 0.0,  active);
				fillAllBars(curRec,  64.0);
				fillAllBars(prevRef, 185.0);
				fillAllBars(nextRef, 190.0);

				const CellMotionField prevMf = CellMotionField::Uniform(cw, ch, -dx, -dy, -dx, -dy, RegionMotionClass::Camera);
				const CellMotionField nextMf = CellMotionField::Uniform(cw, ch, +dx, +dy, +dx, +dy, RegionMotionClass::Camera);

				FrameProcessInput in70;
				in70.seq = 1340;
				in70.receivedY = curRec;
				in70.hasCleanSplit = true;
				in70.cleanY = curRec;
				in70.activeRect = active;
				in70.prevPeak = { &prevRef, &prevMf, false };
				in70.nextPeak = { &nextRef, &nextMf, false };
				in70.hasStepPrior = true;
				in70.estimatedStepDelta = 24.0;
				in70.zeroCoeffFraction = 0.35;

				// 1. Reference CPU vs GPU parity on 70x54 (both transformUnknown and 8x8 CELL)
				for (bool xform8x8 : { false, true })
				{
					in70.transformIdentified8x8 = xform8x8;
					Config cfgRef = MakePreset(Preset::Reference, Mode::On);
					cfgRef.enableLongAnchor = false;
					cfgRef.enableCarryForward = false;
					TdV2ReferenceEngine cpuEng(cfgRef);
					GpuEngineGuard gpuGuard(ctx.gpu, cfgRef);
					const FrameProcessResult cpuRes = cpuEng.ProcessFrame(in70, nullptr);
					const GpuProcessResult gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in70, nullptr, nullptr);
					AssertCpuGpuFrameParity(cpuRes, gpuRes, curRec, curRec, active,
						xform8x8 ? "Geom70x54-8x8CELL" : "Geom70x54-Unknown");

					// Left & right pillarbox bars strictly bit-exact untouched!
					const ActiveRect leftBar{ 0, 0, active.x0, h };
					const ActiveRect rightBar{ active.x1, 0, w, h };
					Assert::AreEqual(0.0, MaxAbsDiffRect(gpuRes.outputY, curRec, leftBar), L"70x54 left pillarbox bar corrupted");
					Assert::AreEqual(0.0, MaxAbsDiffRect(gpuRes.outputY, curRec, rightBar), L"70x54 right pillarbox bar corrupted");
					Assert::AreEqual(0.0, MaxAbsPlane(gpuRes.tdDelta, leftBar), L"70x54 left pillarbox tdDelta non-zero");
					Assert::AreEqual(0.0, MaxAbsPlane(gpuRes.tdDelta, rightBar), L"70x54 right pillarbox tdDelta non-zero");
					Assert::IsTrue(MaxAbsDiffRect(gpuRes.outputY, curRec, active) > 2.0,
						L"70x54 active interior must receive genuine repair (> 2.0 codes)");
				}

				// 2. RealTime GPU with ScratchArena sized for 70x54: pillarbox bars remain bit-exact untouched!
				{
					in70.transformIdentified8x8 = false;
					const auto rtReqs70 = TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, 16, Preset::RealTime);
					VpSkeleton::PlTexBackend tb70(ctx.gpu);
					VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> rtArena70(tb70);
					Assert::IsTrue(rtArena70.Reserve(VpSkeleton::PlanArena(rtReqs70, VpSkeleton::Tier::T1)));
					Config cfgRt = MakePreset(Preset::RealTime, Mode::On);
					cfgRt.enableLongAnchor = false;
					cfgRt.enableCarryForward = false;
					GpuEngineGuard rtGuard(ctx.gpu, cfgRt);
					rtArena70.BeginFrame();
					const GpuProcessResult rtRes = rtGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, in70, &rtArena70, nullptr);
					Assert::IsTrue(rtRes.succeeded && !rtRes.scratchShort, L"70x54 RealTime ScratchArena GPU failed");
					Assert::AreEqual(0.0, MaxAbsDiffRect(rtRes.outputY, curRec, ActiveRect{ 0, 0, active.x0, h }),
						L"70x54 RealTime left pillarbox bar corrupted");
					Assert::AreEqual(0.0, MaxAbsDiffRect(rtRes.outputY, curRec, ActiveRect{ active.x1, 0, w, h }),
						L"70x54 RealTime right pillarbox bar corrupted");
					Assert::IsTrue(MaxAbsDiffRect(rtRes.outputY, curRec, active) > 2.0,
						L"70x54 RealTime active interior must receive genuine repair");
				}

				// 3. NEGATIVE CONTROL (Rule 3): Naive ungated whole-frame average corrupts BOTH pillarbox bars > 20 codes!
				const TdV2Plane nPrev = NaiveUngatedWarpWholeFrame(prevRef, -dx, -dy);
				const TdV2Plane nNext = NaiveUngatedWarpWholeFrame(nextRef, +dx, +dy);
				TdV2Plane naiveAvg(w, h, 0.0);
				for (size_t i = 0; i < naiveAvg.v.size(); ++i)
					naiveAvg.v[i] = (curRec.v[i] + nPrev.v[i] + nNext.v[i]) / 3.0;
				Assert::IsTrue(MaxAbsDiffRect(naiveAvg, curRec, ActiveRect{ 0, 0, active.x0, h }) > 20.0,
					L"Negative control: naive whole-frame avg must corrupt left pillarbox bar > 20 codes");
				Assert::IsTrue(MaxAbsDiffRect(naiveAvg, curRec, ActiveRect{ active.x1, 0, w, h }) > 20.0,
					L"Negative control: naive whole-frame avg must corrupt right pillarbox bar > 20 codes");
			}
		}

		// =========================================================================
		// 14. Q9b Real WARP GPU Shader Parity (shaders/Temporal sigma-delta.glsl):
		//     Limited 8-bit 16.05 & 16.2 (amp=0.5) Produce ZERO BTB Dips (< 16),
		//     True BTB (12.2) Stays Preserved Down to 1 (ADR-0002), and Full-Range
		//     Control on Exact Same 16.05/16.2 Inputs Dips to 15!
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(SigmaDeltaWarpShaderQ9bAboveBlackNeverDipsBtbWhileTrueBtbPreserved)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SigmaDeltaWarpShaderQ9bAboveBlackNeverDipsBtbWhileTrueBtbPreserved)
		{
			WarpGpuContext ctx;

			const std::string shaderPath = RepoRootFromFile() + "shaders\\Temporal sigma-delta.glsl";
			std::ifstream f(shaderPath, std::ios::in | std::ios::binary);
			Assert::IsTrue(f.is_open(), L"Could not open shaders\\Temporal sigma-delta.glsl");
			std::string shaderSrc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			std::string includeErr;
			Assert::IsTrue(CustomHookChain::ExpandVpIncludes(shaderSrc, includeErr),
				L"ExpandVpIncludes failed on Temporal sigma-delta.glsl");

			const pl_fmt srcFmt = pl_find_fmt(ctx.gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			const pl_fmt dstFmt = pl_find_fmt(ctx.gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(srcFmt, L"WARP RGBA32F unsupported");
			Assert::IsNotNull(dstFmt, L"WARP RGBA8 unsupported");

			struct RunStats { int minCode = 255; int maxCode = 0; int countBelow16 = 0; int countAbove16 = 0; };
			double nextInstanceTag = 1.0;
			auto expandSizeTemplates = [](std::string src, int width, int height) -> std::string
			{
				auto replaceAll = [&](const std::string& token, const std::string& val)
				{
					size_t pos = 0;
					while ((pos = src.find(token, pos)) != std::string::npos)
					{
						src.replace(pos, token.size(), val);
						pos += val.size();
					}
				};
				const std::string sw = std::to_string(width);
				const std::string sh = std::to_string(height);
				const std::string swQ4 = std::to_string((2 * width + 10) / 8);
				const std::string shQ4 = std::to_string((2 * height + 10) / 8);
				const std::string swB16 = std::to_string((2 * width + 46) / 32);
				const std::string shB16 = std::to_string((2 * height + 46) / 32);
				replaceAll("{{output_width}}", sw);
				replaceAll("{{output_height}}", sh);
				replaceAll("{{output_width_q4}}", swQ4);
				replaceAll("{{output_height_q4}}", shQ4);
				replaceAll("{{output_width_b16}}", swB16);
				replaceAll("{{output_height_b16}}", shB16);
				return src;
			};

			auto runSigmaDeltaWarp = [&](bool fullRange, double codeLevel, int width, int height, int frames) -> RunStats
			{
				const std::string expandedSrc = expandSizeTemplates(shaderSrc, width, height);
				Assert::IsTrue(expandedSrc.find("{{") == std::string::npos,
					L"Unexpanded {{template}} token left in Temporal sigma-delta.glsl");
				const pl_hook* hook = pl_mpv_user_shader_parse(ctx.gpu, expandedSrc.data(), expandedSrc.size());
				Assert::IsNotNull(hook, L"libplacebo rejected Temporal sigma-delta.glsl");
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "enabled", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "levels_full", fullRange ? 1.0 : 0.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "swapchain_bits", 8.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "color_bits", 8.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "noise_amplitude", 0.5));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "motion_compensation", 0.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "instance_tag", nextInstanceTag));
				nextInstanceTag += 1.0;

				pl_renderer renderer = pl_renderer_create(ctx.log, ctx.gpu);
				Assert::IsNotNull(renderer, L"pl_renderer_create failed on WARP");

				std::vector<float> srcBuf(static_cast<size_t>(width * height) * 4u, 0.0f);
				const float normVal = static_cast<float>(codeLevel / 255.0);
				for (int i = 0; i < width * height; ++i)
				{
					srcBuf[static_cast<size_t>(i) * 4u + 0u] = normVal;
					srcBuf[static_cast<size_t>(i) * 4u + 1u] = normVal;
					srcBuf[static_cast<size_t>(i) * 4u + 2u] = normVal;
					srcBuf[static_cast<size_t>(i) * 4u + 3u] = 1.0f;
				}
				pl_tex_params sp{};
				sp.w = width; sp.h = height; sp.format = srcFmt; sp.sampleable = true; sp.initial_data = srcBuf.data();
				pl_tex srcTex = pl_tex_create(ctx.gpu, &sp);
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex dstTex = pl_tex_create(ctx.gpu, &tp);
				Assert::IsNotNull(srcTex); Assert::IsNotNull(dstTex);

				pl_frame img{};
				img.num_planes = 1;
				img.planes[0].texture = srcTex;
				img.planes[0].components = 4;
				img.planes[0].component_mapping[0] = 0;
				img.planes[0].component_mapping[1] = 1;
				img.planes[0].component_mapping[2] = 2;
				img.planes[0].component_mapping[3] = 3;
				img.repr.sys = PL_COLOR_SYSTEM_RGB;
				img.repr.levels = PL_COLOR_LEVELS_FULL;

				pl_frame tgt = img;
				tgt.planes[0].texture = dstTex;

				const pl_hook* hooks[1] = { hook };
				pl_render_params rparams = pl_render_fast_params;
				rparams.dither_params = nullptr;
				rparams.error_diffusion = nullptr;
				rparams.hooks = hooks;
				rparams.num_hooks = 1;

				std::vector<uint8_t> rgbaOut(static_cast<size_t>(width * height) * 4u, 0u);
				RunStats st;
				for (int fr = 0; fr < frames; ++fr)
				{
					Assert::IsTrue(pl_render_image(renderer, &img, &tgt, &rparams), L"pl_render_image failed on WARP");
					pl_gpu_finish(ctx.gpu);
					pl_tex_transfer_params dl{};
					dl.tex = dstTex;
					dl.ptr = rgbaOut.data();
					Assert::IsTrue(pl_tex_download(ctx.gpu, &dl), L"pl_tex_download failed on WARP");
					for (int i = 0; i < width * height; ++i)
					{
						const int rCode = static_cast<int>(rgbaOut[static_cast<size_t>(i) * 4u]);
						if (rCode < st.minCode) st.minCode = rCode;
						if (rCode > st.maxCode) st.maxCode = rCode;
						if (rCode < 16) ++st.countBelow16;
						if (rCode > 16) ++st.countAbove16;
					}
				}

				pl_tex_destroy(ctx.gpu, &srcTex);
				pl_tex_destroy(ctx.gpu, &dstTex);
				pl_renderer_destroy(&renderer);
				pl_mpv_user_shader_destroy(&hook);
				return st;
			};

			const int rw = 16, rh = 16, nFrames = 24;

			// 1. Limited-range 8-bit above-black levels 16.05 and 16.2 (amplitude = 0.5):
			//    ZERO outputs dip below 16, while upward dither to 17 still runs actively!
			for (double xAbove : { 16.05, 16.2 })
			{
				const RunStats limSt = runSigmaDeltaWarp(/*fullRange=*/false, xAbove, rw, rh, nFrames);
				Assert::AreEqual(0, limSt.countBelow16,
					FormatMsg("Q9b WARP violation: limited x=%.2f dipped below 16 (minCode=%.0f, dips=%.0f)",
						xAbove, static_cast<double>(limSt.minCode), static_cast<double>(limSt.countBelow16)).c_str());
				Assert::IsTrue(limSt.minCode >= 16, L"Q9b WARP: limited above-black minCode must be >= 16");
				Assert::IsTrue(limSt.countAbove16 > 0,
					FormatMsg("Q9b WARP non-vacuity: limited x=%.2f must still dither upward to 17", xAbove).c_str());

				// MANDATORY NEGATIVE CONTROL (Rule 3): Full-range (levels_full = 1) on the EXACT SAME shader &
				// EXACT SAME xAbove input (amp = 0.5) DOES dip to 15 (< 16), proving the Q9b limited-range
				// floor_code guard in Temporal sigma-delta.glsl is genuinely preventing BTB dips!
				const RunStats fullSt = runSigmaDeltaWarp(/*fullRange=*/true, xAbove, rw, rh, nFrames);
				Assert::IsTrue(fullSt.countBelow16 > 0 && fullSt.minCode <= 15,
					FormatMsg("Q9b WARP negative control: full-range x=%.2f must dip <= 15 (minCode=%.0f, dips=%.0f)",
						xAbove, static_cast<double>(fullSt.minCode), static_cast<double>(fullSt.countBelow16)).c_str());
			}

			// 2. TRUE BTB NEGATIVE CONTROL (ADR-0002): x = 12.2 < 16 on the EXACT SAME limited-range shader
			//    remains preserved untouched in [1, 15] (NEVER clamped up to 16)!
			{
				const RunStats btbSt = runSigmaDeltaWarp(/*fullRange=*/false, 12.2, rw, rh, nFrames);
				Assert::IsTrue(btbSt.minCode >= 1 && btbSt.maxCode <= 15,
					FormatMsg("ADR-0002 WARP violation: true BTB x=12.2 escaped [1, 15] (min=%.0f, max=%.0f)",
						static_cast<double>(btbSt.minCode), static_cast<double>(btbSt.maxCode)).c_str());
				Assert::IsTrue(btbSt.minCode <= 12 && btbSt.maxCode >= 13,
					FormatMsg("ADR-0002 WARP: true BTB x=12.2 must dither normally across 12..13 (min=%.0f, max=%.0f)",
						static_cast<double>(btbSt.minCode), static_cast<double>(btbSt.maxCode)).c_str());
			}
		}

		// =========================================================================
		// 15. Round-2 M1/M2/M3 GPU==CPU Parity:
		//     (a) M1: prevPeak2 causal 2-past-peak parity + participation control
		//     (b) M1: explicit in.codecModelPb parity + null-Pb stand-in control
		//     (c) M2: mf.valid == 0 invalid-motion fallback & Pass-3B POCS skip
		//     (d) M3: ApplyTransformUnknownBound Parseval a2BandPeakSq sky banding
		// =========================================================================
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuMatchesCpuOnPrevPeak2CodecModelPbInvalidMotionPocsSkipAndSkyBandingParsevalBound)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuMatchesCpuOnPrevPeak2CodecModelPbInvalidMotionPocsSkipAndSkyBandingParsevalBound)
		{
			WarpGpuContext ctx;
			const int w = 64, h = 64;
			const int cw = w / 16, ch = h / 16;
			const ActiveRect full = ActiveRect::Full(w, h);

			// ---- Part A (M1a): in.prevPeak2 Causal 2-Past-Peak Parity (nextPeak == nullptr) ----
			{
				const double dx = 1.5, dy = 0.5;
				const TdV2Plane mPrev2 = RenderScene(w, h, 0.0, 0.0);
				const TdV2Plane mPrev1 = RenderScene(w, h, 1.0 * dx, 1.0 * dy);
				const TdV2Plane mCur   = RenderScene(w, h, 2.0 * dx, 2.0 * dy);

				// Give prevPeak2 crisp low noise (0.8) vs noisier prevPeak (5.0) and noisy cur (10.0)
				// so prevPeak2 genuinely dominates causal 2-past-peak fusion & serves as independent A1 partner!
				const TdV2Plane prev2Clean = AddCodingDamage(mPrev2, 0.8,  15001u, 0.0, full);
				const TdV2Plane prev1Clean = AddCodingDamage(mPrev1, 5.0,  15002u, 0.0, full);
				const TdV2Plane curClean   = AddCodingDamage(mCur,   10.0, 15003u, 0.0, full);

				const CellMotionField mfPrev1 = CellMotionField::Uniform(cw, ch, -dx, -dy, -dx, -dy, RegionMotionClass::Camera);
				const CellMotionField mfPrev2 = CellMotionField::Uniform(cw, ch, -2.0 * dx, -2.0 * dy, -2.0 * dx, -2.0 * dy, RegionMotionClass::Camera);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;

				FrameProcessInput inBoth;
				inBoth.seq = 1501;
				inBoth.receivedY = curClean;
				inBoth.hasCleanSplit = true;
				inBoth.cleanY = curClean;
				inBoth.activeRect = full;
				inBoth.prevPeak  = { &prev1Clean, &mfPrev1, false };
				inBoth.nextPeak  = { nullptr, nullptr, false };
				inBoth.prevPeak2 = { &prev2Clean, &mfPrev2, false };
				inBoth.hasStepPrior = true;
				inBoth.estimatedStepDelta = 24.0;
				inBoth.deadZonePhi = kDeadZonePhiInter;
				inBoth.zeroCoeffFraction = 0.35;

				TdV2ReferenceEngine cpuEng(cfg);
				GpuEngineGuard gpuGuard(ctx.gpu, cfg);
				const FrameProcessResult cpuRes = cpuEng.ProcessFrame(inBoth, nullptr);
				const GpuProcessResult   gpuRes = gpuGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inBoth, nullptr, nullptr);
				Assert::AreEqual(static_cast<size_t>(2u), cpuRes.activeRefCount, L"M1a CPU must fuse 2 causal refs (prevPeak + prevPeak2)");
				Assert::AreEqual(static_cast<size_t>(2u), gpuRes.activeRefCount, L"M1a GPU must fuse 2 causal refs (prevPeak + prevPeak2)");
				AssertCpuGpuFrameParity(cpuRes, gpuRes, curClean, curClean, full, "M1a-PrevPeak2");

				// NEGATIVE CONTROL (prevPeak-only on exact same inputs): removing prevPeak2 diverges > 1.5 codes!
				FrameProcessInput inPrevOnly = inBoth;
				inPrevOnly.seq = 1502;
				inPrevOnly.prevPeak2 = { nullptr, nullptr, false };
				GpuEngineGuard gpuSingleGuard(ctx.gpu, cfg);
				const GpuProcessResult gpuPrevOnly = gpuSingleGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inPrevOnly, nullptr, nullptr);
				Assert::IsTrue(gpuPrevOnly.succeeded && gpuPrevOnly.activeRefCount == 1u);
				const double prev2Diff = MaxAbsDiffFull(gpuRes.outputY, gpuPrevOnly.outputY);
				Assert::IsTrue(prev2Diff > 1.5,
					FormatMsg("M1a negative control: prevPeak2 vs prevPeak-only GPU diff=%.3f <= 1.5 codes", prev2Diff).c_str());
			}

			// ---- Part B (M1b): Explicit in.codecModelPb Parity (transformIdentified8x8=true, ResidualMode::Strict) ----
			{
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const TdV2Plane master = RenderScene(w, h, 0.0, 0.0);
				const TdV2Plane curNoisy = AddCodingDamage(master, 9.0, 15101u, 0.0, full);
				const TdV2Plane refPrev  = AddCodingDamage(master, 1.0, 15102u, 0.0, full);
				const TdV2Plane refNext  = AddCodingDamage(master, 1.0, 15103u, 0.0, full);

				// Explicit Field B prediction P_B distinctly offset (+48 codes) from prevPeak so sent DCT coefficients differ!
				TdV2Plane explicitPb = master;
				for (double& v : explicitPb.v) v += 48.0;

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.residualMode = ResidualMode::Strict;
				cfg.cellProjIterations = 2;
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;

				FrameProcessInput inPb;
				inPb.seq = 1510;
				inPb.receivedY = curNoisy;
				inPb.hasCleanSplit = true;
				inPb.cleanY = curNoisy;
				inPb.activeRect = full;
				inPb.prevPeak = { &refPrev, &stillMf, false };
				inPb.nextPeak = { &refNext, &stillMf, false };
				inPb.codecModelPb = &explicitPb;
				inPb.hasStepPrior = true;
				inPb.estimatedStepDelta = 24.0;
				inPb.deadZonePhi = kDeadZonePhiInter;
				inPb.zeroCoeffFraction = 0.35;
				inPb.transformIdentified8x8 = true;

				TdV2ReferenceEngine cpuPbEng(cfg);
				GpuEngineGuard gpuPbGuard(ctx.gpu, cfg);
				StandInRegistry cpuPbSi, gpuPbSi;
				const FrameProcessResult cpuPb = cpuPbEng.ProcessFrame(inPb, &cpuPbSi);
				const GpuProcessResult   gpuPb = gpuPbGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inPb, nullptr, &gpuPbSi);
				AssertCpuGpuFrameParity(cpuPb, gpuPb, curNoisy, curNoisy, full, "M1b-ExplicitPb");
				Assert::IsFalse(cpuPbSi.IsActive(StandInKind::CodecModelPbStandIn),
					L"M1b: CPU with explicit codecModelPb must NOT engage CodecModelPbStandIn");
				Assert::IsFalse(gpuPbSi.IsActive(StandInKind::CodecModelPbStandIn),
					L"M1b: GPU with explicit codecModelPb must NOT engage CodecModelPbStandIn");

				// SAME-INPUT NEGATIVE CONTROL (in.codecModelPb = nullptr): engages CodecModelPbStandIn & alters output!
				FrameProcessInput inNullPb = inPb;
				inNullPb.seq = 1511;
				inNullPb.codecModelPb = nullptr;
				TdV2ReferenceEngine cpuNullEng(cfg);
				GpuEngineGuard gpuNullGuard(ctx.gpu, cfg);
				StandInRegistry cpuNullSi, gpuNullSi;
				const FrameProcessResult cpuNull = cpuNullEng.ProcessFrame(inNullPb, &cpuNullSi);
				const GpuProcessResult   gpuNull = gpuNullGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inNullPb, nullptr, &gpuNullSi);
				AssertCpuGpuFrameParity(cpuNull, gpuNull, curNoisy, curNoisy, full, "M1b-NullPb");
				Assert::IsTrue(cpuNullSi.IsActive(StandInKind::CodecModelPbStandIn),
					L"M1b negative control: CPU with null codecModelPb MUST engage CodecModelPbStandIn");
				Assert::IsTrue(gpuNullSi.IsActive(StandInKind::CodecModelPbStandIn),
					L"M1b negative control: GPU with null codecModelPb MUST engage CodecModelPbStandIn");
				const double pbVsNullDiff = MaxAbsDiffFull(gpuPb.outputY, gpuNull.outputY);
				Assert::IsTrue(pbVsNullDiff > 1.0,
					FormatMsg("M1b negative control: explicit codecModelPb vs null Pb diff=%.3f <= 1.0 code", pbVsNullDiff).c_str());
			}

			// ---- Part C (M2): Luma mf.valid == 0 (Bogus vx=8, vy=8 in [16,32)^2) + Pass-3B POCS Skip Parity ----
			{
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				CellMotionField badPrevMf = stillMf;
				const size_t invalidCell = 1u * static_cast<size_t>(cw) + 1u; // [16, 32)^2
				badPrevMf.valid[invalidCell] = 0u;
				badPrevMf.vx[invalidCell] = 8.0;
				badPrevMf.vy[invalidCell] = 8.0;

				const TdV2Plane master = RenderScene(w, h, 0.0, 0.0);
				const TdV2Plane curNoisy = AddCodingDamage(master, 9.0, 15201u, 0.0, full);
				const TdV2Plane refPrev  = AddCodingDamage(master, 1.0, 15202u, 0.0, full);
				const TdV2Plane refNext  = AddCodingDamage(master, 1.0, 15203u, 0.0, full);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.residualMode = ResidualMode::Strict;
				cfg.cellProjIterations = 2;
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;

				FrameProcessInput inM2;
				inM2.seq = 1520;
				inM2.receivedY = curNoisy;
				inM2.hasCleanSplit = true;
				inM2.cleanY = curNoisy;
				inM2.activeRect = full;
				inM2.prevPeak = { &refPrev, &badPrevMf, false };
				inM2.nextPeak = { &refNext, &stillMf, false };
				inM2.hasStepPrior = true;
				inM2.estimatedStepDelta = 24.0;
				inM2.deadZonePhi = kDeadZonePhiInter;
				inM2.zeroCoeffFraction = 0.35;
				inM2.transformIdentified8x8 = true;

				TdV2ReferenceEngine cpuM2Eng(cfg);
				GpuEngineGuard gpuM2Guard(ctx.gpu, cfg);
				const FrameProcessResult cpuM2 = cpuM2Eng.ProcessFrame(inM2, nullptr);
				const GpuProcessResult   gpuM2 = gpuM2Guard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inM2, nullptr, nullptr);
				AssertCpuGpuFrameParity(cpuM2, gpuM2, curNoisy, curNoisy, full, "M2-InvalidMotionPocsSkip");

				// Explicit check inside [16, 32)^2 (bogus vector cell) + valid-motion negative control!
				const ActiveRect invRect{ 16, 16, 32, 32 };
				Assert::IsTrue(MaxAbsDiffRect(gpuM2.outputY, cpuM2.outputY, invRect) <= kGpuCpuParityTol,
					L"M2: GPU vs CPU mismatch > 1e-3 inside mf.valid==0 cell [16,32)^2");

				FrameProcessInput inValidM2 = inM2;
				inValidM2.seq = 1521;
				inValidM2.prevPeak = { &refPrev, &stillMf, false };
				GpuEngineGuard gpuValGuard(ctx.gpu, cfg);
				const GpuProcessResult gpuVal = gpuValGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inValidM2, nullptr, nullptr);
				Assert::IsTrue(gpuVal.succeeded);
				Assert::IsTrue(MaxAbsDiffRect(gpuM2.outputY, gpuVal.outputY, invRect) > 0.5,
					L"M2 negative control: valid motion vs mf.valid==0 must produce distinct output inside [16,32)^2");
			}

			// ---- Part D (M3): Luma ApplyTransformUnknownBound Parseval a2BandPeakSq on 8-Code Sky Banding Step ----
			{
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				TdV2Plane skyMaster(w, h, 0.0);
				TdV2Plane skyBanded(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						// Wide smooth sky gradient (slope 0.25 code/px -> 32-px wide 8-code banding step, ~89% energy in Band L,
						// up to +/-4 codes) so the old white-noise L bound (~1.14 codes) would severely over-clip it!
						const double v = 320.0 + 0.25 * static_cast<double>(x);
						skyMaster.at(x, y) = v;
						skyBanded.at(x, y) = std::floor(v / 8.0 + 0.5) * 8.0;
					}
				}

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.enableLongAnchor = false;
				cfg.enableCarryForward = false;

				FrameProcessInput inSky;
				inSky.seq = 1530;
				inSky.receivedY = skyBanded;
				inSky.hasCleanSplit = true;
				inSky.cleanY = skyBanded;
				inSky.activeRect = full;
				inSky.prevPeak = { &skyMaster, &stillMf, false };
				inSky.nextPeak = { &skyMaster, &stillMf, false };
				inSky.hasStepPrior = true;
				inSky.estimatedStepDelta = 8.0;
				inSky.deadZonePhi = 0.5;
				inSky.zeroCoeffFraction = 0.0;
				inSky.transformIdentified8x8 = false;

				TdV2ReferenceEngine cpuSkyEng(cfg);
				GpuEngineGuard gpuSkyGuard(ctx.gpu, cfg);
				const FrameProcessResult cpuSky = cpuSkyEng.ProcessFrame(inSky, nullptr);
				const GpuProcessResult   gpuSky = gpuSkyGuard.engine.ProcessFrame(ctx.gpu, ctx.dispatch, inSky, nullptr, nullptr);
				AssertCpuGpuFrameParity(cpuSky, gpuSky, skyBanded, skyBanded, full, "M3-SkyBandingParseval");

				const double peakSkyDelta = MaxAbsPlane(gpuSky.tdDelta, full);
				Assert::IsTrue(peakSkyDelta > 1.5,
					FormatMsg("M3: 8-code sky banding delta=%.3f <= 1.5 (over-clipped by old white-noise L bound?)",
						peakSkyDelta).c_str());

				// NEGATIVE CONTROL (old white-noise L-band bound): compute old 3*sqrt(totalVar * gains.low) (~1.14 codes)
				// and verify Parseval a2BandPeakSq[0]=1.0 genuinely lifts coherent L-band sky repair well past it!
				const RoundingCellStats skyA2 = ComputeRoundingCellStats(8.0, 0.5, 0.0);
				const BandWhiteNoiseGains skyGains = ComputeBandWhiteNoiseGains();
				const double oldWhiteNoiseLBound = kPixelBoundSigma * std::sqrt(skyA2.totalVar * skyGains.low);
				Assert::IsTrue(oldWhiteNoiseLBound < 1.25 && peakSkyDelta > oldWhiteNoiseLBound + 0.5,
					FormatMsg("M3 negative control: peakSkyDelta=%.3f did not clear old white-noise L bound=%.3f + 0.5",
						peakSkyDelta, oldWhiteNoiseLBound).c_str());
			}
		}
	};
} // namespace TdV2GpuWarpNs
} // namespace Tests
