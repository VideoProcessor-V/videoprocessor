// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S4 (2026-11-23_TD-consolidated-plan.md s1.5, s3.2 TD-S4):
// Coded-Grid Chroma Recovery across delivered formats (4:2:0, 4:2:2, 4:4:4, RGB):
//   - Coded-grid separation (99 s2D): reduces delivered chroma to coded grid (exact identity when
//     coded == delivered), fuses strictly on grain-free coded Cb/Cr using luma motion vectors scaled
//     to the coded chroma grid and honoring core/ChromaSiting.h, and adds ONLY the rebuilt signed
//     delta back (unclipped on RGB via core/LumaWeights.h, preserving BTB/WTW bit-exact when delta == 0).
//   - 4:4:4 Anchor Memory Canvas (TdV2ChromaAnchorCanvas): accumulates grain-free coded chroma samples
//     at exact motion-compensated sited luma coordinates (x_luma - vx, y_luma - vy) strictly inside
//     ActiveRect (NEVER accumulating chroma grain), and fits per-cell anchor CfL (a, b) from anchor
//     clean luma sampled at sited coded coordinates and anchor clean chroma.
//   - Carried CfL (H5): applies anchor-carried (a, b) STRICTLY to OUR reference-aligned prediction
//     luma P_Y = Warp(anchorCleanY) — NEVER to current frame's luma curY! Falls back visibly via
//     StandInKind::ChromaCflFallback (emitting per-cell single-frame ChromaCfl fallback fits) whenever
//     per-cell chroma residual disagrees beyond 3-sigma (kHuberKneeSq * max(cellRefDiffVar, stepTotalVar)).
//   - Unidentified coded grid: visibly engages StandInKind::ChromaGridDetector and runs strictly
//     coarse-band-only fusion (sigma = 8.0 px on 1/4 luma grid = 32.0 full-res luma px = 16.0 4:2:0
//     coded-chroma px, M/H weights = 0, own chroma grain untouched; plan s1.5 L168).
//
// Every constant is mathematically or physically derived and documented inline (Rule 1).
// Every stand-in / fallback is visibly recorded in StandInRegistry and result counters (Rule 4).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/LumaWeights.h>
#include <VP-V/core/OnePole.h>
#include <VP-V/core/Ramp.h>
#include <VP-V/modules/motion/estimation/composed/WarpSampler.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2GrainQ5.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace TdV2
{
	// ---- Rule-1 Derived Chroma Constants ----

	// Plan s1.5 #1 (L168): unidentified coded-grid coarse-band mode uses sigma = 8 px on the 1/4 luma grid
	// (quarter-luma scale factor = 4), i.e., sigma = 8.0 * 4.0 = 32.0 full-resolution luma pixels
	// (= 16.0 coded-chroma pixels on 4:2:0, strictly below any chroma Nyquist).
	constexpr double kQuarterLumaGridScale = 4.0;
	constexpr double kUnidentifiedChromaSigmaQuarterLumaPx = 0.5 * static_cast<double>(kCanonicalBlockPx);
	constexpr double kUnidentifiedChromaSigmaLumaPx = kUnidentifiedChromaSigmaQuarterLumaPx * kQuarterLumaGridScale;

	// Derived minimum 4:4:4 canvas accumulated footprint weight (Rule 1):
	// Exact uniform weight share of a 4x4 bicubic sampling footprint = 1 / (4 * 4) = 1/16.
	// Rejects near-zero grazing splat corners while accepting fresh and near-I decayed bilinear splats.
	constexpr double kMinCanvasFootprintWeight = 1.0 / 16.0;

	// Derived rate-aware near-I anchor canvas decay time constant in seconds (Rule 1 & U23 OnePole):
	// Contract back-reference horizon kContractBackRefs (8 frames) at cinema kDefaultSourceHz (24000/1001 Hz).
	// Applied via OnePole::AlphaFor(dtSec, kAnchorCanvasTauSec) so accumulation stays strictly bounded across time.
	constexpr double kAnchorCanvasTauSec = static_cast<double>(kContractBackRefs) / kDefaultSourceHz;

	// ---- Chroma Format & Siting Vocabulary ----

	enum class ChromaSubsampling : uint8_t
	{
		Yuv420 = 0,
		Yuv422 = 1,
		Yuv444 = 2
	};

	inline int SubsampleX(ChromaSubsampling s)
	{
		return (s == ChromaSubsampling::Yuv444) ? 1 : 2;
	}

	inline int SubsampleY(ChromaSubsampling s)
	{
		return (s == ChromaSubsampling::Yuv420) ? 2 : 1;
	}

	enum class DeliveredChromaFormat : uint8_t
	{
		Yuv420 = 0,
		Yuv422 = 1,
		Yuv444 = 2,
		Rgb444 = 3
	};

	inline ChromaSubsampling DeliveredSubsampling(DeliveredChromaFormat fmt)
	{
		switch (fmt)
		{
		case DeliveredChromaFormat::Yuv420: return ChromaSubsampling::Yuv420;
		case DeliveredChromaFormat::Yuv422: return ChromaSubsampling::Yuv422;
		case DeliveredChromaFormat::Yuv444:
		case DeliveredChromaFormat::Rgb444:
			return ChromaSubsampling::Yuv444;
		}
		return ChromaSubsampling::Yuv444;
	}

	inline int DeliveredSubsampleX(DeliveredChromaFormat fmt) { return SubsampleX(DeliveredSubsampling(fmt)); }
	inline int DeliveredSubsampleY(DeliveredChromaFormat fmt) { return SubsampleY(DeliveredSubsampling(fmt)); }

	enum class ChromaGridStatus : uint8_t
	{
		Identified = 0,
		Unidentified = 1
	};

	// Siting specification in libplacebo's luma-relative shift units (matching core/ChromaSiting.h).
	struct SitingSpec
	{
		float shiftX = -0.5f;
		float shiftY = 0.0f;

		static SitingSpec Left() { return { -0.5f, 0.0f }; }
		static SitingSpec TopLeftType2() { return { -0.5f, -0.5f }; }
		static SitingSpec Center() { return { 0.0f, 0.0f }; }

		// Exact coordinate mapping between coded sample index c and luma pixel coordinate:
		//   luma = (c + 0.5) * sub - 0.5 + shift   (on subsampled axes sub > 1; co-sited when sub == 1).
		double CodedToLumaX(double cx, int subX) const
		{
			const double s = (subX > 1) ? static_cast<double>(shiftX) : 0.0;
			return (cx + 0.5) * static_cast<double>(subX) - 0.5 + s;
		}

		double CodedToLumaY(double cy, int subY) const
		{
			const double s = (subY > 1) ? static_cast<double>(shiftY) : 0.0;
			return (cy + 0.5) * static_cast<double>(subY) - 0.5 + s;
		}

		// Exact algebraic inverse of CodedToLumaX/Y.
		double LumaToCodedX(double lx, int subX) const
		{
			if (subX <= 0)
				return 0.0;
			const double s = (subX > 1) ? static_cast<double>(shiftX) : 0.0;
			return (lx + 0.5 - s) / static_cast<double>(subX) - 0.5;
		}

		double LumaToCodedY(double ly, int subY) const
		{
			if (subY <= 0)
				return 0.0;
			const double s = (subY > 1) ? static_cast<double>(shiftY) : 0.0;
			return (ly + 0.5 - s) / static_cast<double>(subY) - 0.5;
		}

		// Edge-aligned resample coordinate-scaling law matching core/ChromaSiting.h ShiftAfterResample:
		//   shift' = shift * factor.
		SitingSpec Resampled(double factorX, double factorY) const
		{
			return {
				static_cast<float>(static_cast<double>(shiftX) * factorX),
				static_cast<float>(static_cast<double>(shiftY) * factorY)
			};
		}
	};

	// ---- Signalled Y'CbCr <-> R'G'B' Matrix via shared U26 LumaCoeff (ZERO private luma literals) ----
	struct YCbCrMatrixSpec
	{
		double kr = LumaCoeff::kBt709Kr;
		double kg = LumaCoeff::kBt709Kg;
		double kb = LumaCoeff::kBt709Kb;

		static YCbCrMatrixSpec FromColorSystem(
			enum pl_color_system system,
			enum pl_color_primaries primaries = PL_COLOR_PRIM_UNKNOWN)
		{
			double w[3] = { LumaCoeff::kBt709Kr, LumaCoeff::kBt709Kg, LumaCoeff::kBt709Kb };
			LumaCoeff::ForSystem(system, primaries, w);
			return { w[0], w[1], w[2] };
		}

		static YCbCrMatrixSpec Bt709()
		{
			return FromColorSystem(PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709);
		}

		static YCbCrMatrixSpec Bt2020()
		{
			return FromColorSystem(PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020);
		}

		void RgbToYCbCr(double r, double g, double b, double midCode,
		                double& y, double& cb, double& cr) const
		{
			y = kr * r + kg * g + kb * b;
			const double denomB = 2.0 * (1.0 - kb);
			const double denomR = 2.0 * (1.0 - kr);
			cb = (denomB > kEngineNumFloor) ? (midCode + (b - y) / denomB) : midCode;
			cr = (denomR > kEngineNumFloor) ? (midCode + (r - y) / denomR) : midCode;
		}

		// Apply signed chroma deltas (dCb, dCr) directly to (r, g, b) with dY = 0 and STRICTLY NO CLIPPING
		// so BTB (< 64) and WTW (> 940) are preserved and (dCb == 0 && dCr == 0) is bit-exact!
		// Returns false and visibly engages NonFiniteClosure if delta is non-finite or matrix kg is degenerate.
		bool ApplySignedChromaDeltaToRgb(
			double r, double g, double b,
			double dCb, double dCr,
			double& outR, double& outG, double& outB,
			StandInRegistry* standIns = nullptr) const
		{
			if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
			{
				outR = std::isfinite(r) ? r : 0.0;
				outG = std::isfinite(g) ? g : 0.0;
				outB = std::isfinite(b) ? b : 0.0;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "ApplySignedChromaDeltaToRgb non-finite input RGB");
				return false;
			}
			if (dCb == 0.0 && dCr == 0.0)
			{
				outR = r;
				outG = g;
				outB = b;
				return true;
			}
			if (!std::isfinite(dCb) || !std::isfinite(dCr) ||
			    !std::isfinite(kr) || !std::isfinite(kb) || !std::isfinite(kg) || !(kg > kEngineNumFloor))
			{
				outR = r;
				outG = g;
				outB = b;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "ApplySignedChromaDeltaToRgb invalid delta or matrix");
				return false;
			}
			const double kR = 2.0 * (1.0 - kr);
			const double kB = 2.0 * (1.0 - kb);
			const double dR = kR * dCr;
			const double dB = kB * dCb;
			const double dG = -(kr * kR / kg) * dCr - (kb * kB / kg) * dCb;
			const double nr = r + dR;
			const double ng = g + dG;
			const double nb = b + dB;
			if (!std::isfinite(nr) || !std::isfinite(ng) || !std::isfinite(nb))
			{
				outR = r;
				outG = g;
				outB = b;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "ApplySignedChromaDeltaToRgb non-finite output");
				return false;
			}
			outR = nr;
			outG = ng;
			outB = nb;
			return true;
		}
	};

	// ---- Internal Plane & Sampling Helpers ----
	namespace ChromaDetail
	{
		inline int SubsampledDim(int lumaDim, int sub)
		{
			if (lumaDim <= 0 || sub <= 0)
				return 0;
			return (lumaDim + sub - 1) / sub;
		}

		// Map luma ActiveRect into subsampled plane ActiveRect strictly interior to bar boundaries.
		inline ActiveRect ToSubsampledActiveRect(
			const ActiveRect& lumaActive,
			int lumaW,
			int lumaH,
			int subX,
			int subY,
			int planeW,
			int planeH)
		{
			const ActiveRect ar = lumaActive.Empty()
				? ActiveRect::Full(lumaW, lumaH)
				: lumaActive.ClampedTo(lumaW, lumaH);
			if (ar.Empty() || subX <= 0 || subY <= 0 || planeW <= 0 || planeH <= 0)
				return {};
			ActiveRect cr;
			cr.x0 = (ar.x0 + subX - 1) / subX;
			cr.y0 = (ar.y0 + subY - 1) / subY;
			cr.x1 = (ar.x1 >= lumaW) ? SubsampledDim(ar.x1, subX) : (ar.x1 / subX);
			cr.y1 = (ar.y1 >= lumaH) ? SubsampledDim(ar.y1, subY) : (ar.y1 / subY);
			return cr.ClampedTo(planeW, planeH);
		}

		inline double SampleBilinearClampedLocal(const ActiveSubPlaneView& view, double sxLocal, double syLocal)
		{
			const double cx = (std::max)(0.0, (std::min)(static_cast<double>(view.w - 1), sxLocal));
			const double cy = (std::max)(0.0, (std::min)(static_cast<double>(view.h - 1), syLocal));
			const int ix0 = static_cast<int>(std::floor(cx));
			const int iy0 = static_cast<int>(std::floor(cy));
			const int ix1 = (std::min)(view.w - 1, ix0 + 1);
			const int iy1 = (std::min)(view.h - 1, iy0 + 1);
			const double fx = cx - static_cast<double>(ix0);
			const double fy = cy - static_cast<double>(iy0);
			const double v00 = view.at(ix0, iy0);
			const double v10 = view.at(ix1, iy0);
			const double v01 = view.at(ix0, iy1);
			const double v11 = view.at(ix1, iy1);
			return (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy;
		}

		// Sample via shared VpComposed::SampleBicubic strictly inside ActiveSubPlaneView, with visible
		// active-border bilinear fallback for 1-2 px boundary coordinates inside [0, w-1] x [0, h-1].
		inline bool SampleSitedActive(
			const ActiveSubPlaneView& view,
			double sxLocal,
			double syLocal,
			double& outVal,
			bool& usedBorderBilinear,
			bool allowEdgeClampOutside = false)
		{
			usedBorderBilinear = false;
			if (view.src == nullptr || view.w <= 0 || view.h <= 0 || !std::isfinite(sxLocal) || !std::isfinite(syLocal))
				return false;
			if (VpComposed::SampleBicubic(view, sxLocal, syLocal, outVal))
				return std::isfinite(outVal);
			const bool inActiveBounds = (sxLocal >= 0.0 && syLocal >= 0.0 &&
			                             sxLocal <= static_cast<double>(view.w - 1) &&
			                             syLocal <= static_cast<double>(view.h - 1));
			if (inActiveBounds || allowEdgeClampOutside)
			{
				outVal = SampleBilinearClampedLocal(view, sxLocal, syLocal);
				usedBorderBilinear = true;
				return std::isfinite(outVal);
			}
			return false;
		}

		// Separable anisotropic low-pass blur strictly inside `active` using shared U2 BandFromBase::Taps.
		// Also outputs exact 2D separable white-noise variance transfer gain = sum(tx^2) * sum(ty^2).
		inline TdV2Plane BlurActiveAnisotropic(
			const TdV2Plane& src,
			const ActiveRect& active,
			double sigmaX,
			double sigmaY,
			double& outNoiseGain)
		{
			outNoiseGain = 1.0;
			TdV2Plane out(src.w, src.h, 0.0);
			const ActiveRect ar = active.ClampedTo(src.w, src.h);
			if (ar.Empty())
				return out;

			const auto tapsX = VpUnit::BandFromBase::Taps<double>(sigmaX, 3.0, true, 0.0);
			const auto tapsY = VpUnit::BandFromBase::Taps<double>(sigmaY, 3.0, true, 0.0);
			double sumX2 = 0.0, sumY2 = 0.0;
			for (double t : tapsX) sumX2 += t * t;
			for (double t : tapsY) sumY2 += t * t;
			outNoiseGain = sumX2 * sumY2;

			const int aw = ar.Width();
			const int ah = ar.Height();
			const int rx = static_cast<int>(tapsX.size() / 2);
			const int ry = static_cast<int>(tapsY.size() / 2);

			std::vector<double> tmp(static_cast<size_t>(aw) * static_cast<size_t>(ah), 0.0);
			for (int y = 0; y < ah; ++y)
			{
				for (int x = 0; x < aw; ++x)
				{
					double acc = 0.0;
					for (int k = -rx; k <= rx; ++k)
					{
						const int sx = (std::max)(0, (std::min)(aw - 1, x + k));
						acc += tapsX[static_cast<size_t>(k + rx)] * src.at(ar.x0 + sx, ar.y0 + y);
					}
					tmp[static_cast<size_t>(y) * aw + x] = acc;
				}
			}
			for (int y = 0; y < ah; ++y)
			{
				for (int x = 0; x < aw; ++x)
				{
					double acc = 0.0;
					for (int k = -ry; k <= ry; ++k)
					{
						const int sy = (std::max)(0, (std::min)(ah - 1, y + k));
						acc += tapsY[static_cast<size_t>(k + ry)] * tmp[static_cast<size_t>(sy) * aw + x];
					}
					out.at(ar.x0 + x, ar.y0 + y) = acc;
				}
			}
			return out;
		}

		// Warp a grain-free chroma plane on its own (subX, subY) grid using luma-grid motion vectors
		// scaled by (1/subX, 1/subY), honoring exact siting and hard ActiveRect boundaries.
		// Invalid-motion, non-finite, and OOB pixels keep fallbackPlane (codedClean) so DecomposeActiveBands
		// never bleeds 0.0-holes or mismatched edges into valid neighbours, while outValid stays 0.
		inline WarpDiagnostics WarpChromaPlaneWithLumaMotion(
			const TdV2Plane& refPlane,
			const TdV2Plane& fallbackPlane,
			const ActiveRect& planeActive,
			int subX,
			int subY,
			const SitingSpec& siting,
			const std::vector<double>& lumaCellVx,
			const std::vector<double>& lumaCellVy,
			int cellsW,
			int cellsH,
			int cellPx,
			TdV2Plane& outWarped,
			std::vector<uint8_t>& outValid,
			StandInRegistry* standIns = nullptr,
			const CellMotionField* lumaMf = nullptr)
		{
			WarpDiagnostics diag;
			const int w = refPlane.w;
			const int h = refPlane.h;
			outWarped = fallbackPlane;
			outValid.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);

			const ActiveRect ar = planeActive.ClampedTo(w, h);
			const size_t expectedCells = (cellsW > 0 && cellsH > 0)
				? static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH)
				: 0u;
			if (ar.Empty() || expectedCells == 0u || cellPx <= 0 || subX <= 0 || subY <= 0 ||
			    lumaCellVx.size() != expectedCells || lumaCellVy.size() != expectedCells)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::MotionFieldStandIn, "WarpChromaPlaneWithLumaMotion invalid dimensions");
				return diag;
			}
			const bool hasCellValid = (lumaMf != nullptr && lumaMf->valid.size() == expectedCells);

			// Sanitize non-finite reference pixels inside active rect before sampling.
			TdV2Plane safeRef = refPlane;
			std::vector<uint8_t> refFinite(static_cast<size_t>(w) * static_cast<size_t>(h), 1u);
			for (int y = ar.y0; y < ar.y1; ++y)
			{
				for (int x = ar.x0; x < ar.x1; ++x)
				{
					if (!std::isfinite(safeRef.at(x, y)))
					{
						safeRef.at(x, y) = fallbackPlane.at(x, y);
						refFinite[static_cast<size_t>(y) * w + x] = 0u;
						++diag.nonFinitePixels;
					}
				}
			}

			ActiveSubPlaneView view{ &safeRef, ar.x0, ar.y0, ar.Width(), ar.Height() };

			auto footprintHasNonFinite = [&](int x0Local, int y0Local, int x1Local, int y1Local) -> bool
			{
				if (diag.nonFinitePixels == 0)
					return false;
				for (int yy = y0Local; yy <= y1Local; ++yy)
				{
					const int gy = ar.y0 + (std::max)(0, (std::min)(ar.Height() - 1, yy));
					for (int xx = x0Local; xx <= x1Local; ++xx)
					{
						const int gx = ar.x0 + (std::max)(0, (std::min)(ar.Width() - 1, xx));
						if (refFinite[static_cast<size_t>(gy) * w + gx] == 0u)
							return true;
					}
				}
				return false;
			};

			const double invSubX = 1.0 / static_cast<double>(subX);
			const double invSubY = 1.0 / static_cast<double>(subY);

			for (int y = ar.y0; y < ar.y1; ++y)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(y), subY);
				const int mcy = (std::max)(0, (std::min)(cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
				for (int x = ar.x0; x < ar.x1; ++x)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(x), subX);
					const int mcx = (std::max)(0, (std::min)(cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
					const size_t cIdx = static_cast<size_t>(mcy) * cellsW + mcx;
					if (hasCellValid && lumaMf->valid[cIdx] == 0u)
						continue;
					const double vx = lumaCellVx[cIdx];
					const double vy = lumaCellVy[cIdx];
					if (!std::isfinite(vx) || !std::isfinite(vy))
					{
						++diag.nonFinitePixels;
						continue;
					}

					const double sxLocal = static_cast<double>(x - ar.x0) + vx * invSubX;
					const double syLocal = static_cast<double>(y - ar.y0) + vy * invSubY;
					double val = 0.0;
					if (VpComposed::SampleBicubic(view, sxLocal, syLocal, val) && std::isfinite(val))
					{
						const int ix0 = static_cast<int>(std::floor(sxLocal));
						const int iy0 = static_cast<int>(std::floor(syLocal));
						outWarped.at(x, y) = val;
						if (!footprintHasNonFinite(ix0 - 1, iy0 - 1, ix0 + 2, iy0 + 2))
							outValid[static_cast<size_t>(y) * w + x] = 1u;
					}
					else if (sxLocal >= 0.0 && syLocal >= 0.0 &&
					         sxLocal <= static_cast<double>(ar.Width() - 1) &&
					         syLocal <= static_cast<double>(ar.Height() - 1))
					{
						const double bVal = SampleBilinearClampedLocal(view, sxLocal, syLocal);
						const int ix0 = static_cast<int>(std::floor(sxLocal));
						const int iy0 = static_cast<int>(std::floor(syLocal));
						outWarped.at(x, y) = bVal;
						++diag.borderBilinearPixels;
						if (std::isfinite(bVal) && !footprintHasNonFinite(ix0, iy0, ix0 + 1, iy0 + 1))
							outValid[static_cast<size_t>(y) * w + x] = 1u;
					}
					else
					{
						// Out-of-bounds landing: keep codedClean fallback (H3: zero 0.0-holes and zero mismatched edge bleed), outValid = 0.
						const double fbVal = fallbackPlane.at(x, y);
						outWarped.at(x, y) = std::isfinite(fbVal)
							? fbVal
							: SampleBilinearClampedLocal(view, sxLocal, syLocal);
					}
				}
			}

			if (diag.borderBilinearPixels > 0 && standIns != nullptr)
				standIns->Engage(StandInKind::BorderBilinearFallback);
			if (diag.nonFinitePixels > 0 && standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "WarpChromaPlaneWithLumaMotion non-finite ref or motion");
			return diag;
		}
	}

	// ---- Coded-Grid Separation (99 s2D): ReduceToCodedGrid & UpsampleDeltaToDelivered ----

	// Reduce delivered chroma plane to coded chroma grid honoring exact SitingSpec and ActiveRect.
	// Exact identity copy when codedSub == DeliveredSubsampling(deliveredFmt).
	inline TdV2Plane ReduceToCodedGrid(
		const TdV2Plane& delivered,
		int lumaW,
		int lumaH,
		DeliveredChromaFormat deliveredFmt,
		ChromaSubsampling codedSub,
		const SitingSpec& siting,
		const ActiveRect& lumaActive,
		StandInRegistry* standIns = nullptr)
	{
		if (delivered.Empty() || lumaW <= 0 || lumaH <= 0)
			return {};

		const ChromaSubsampling delivSub = DeliveredSubsampling(deliveredFmt);
		const int dSubX = SubsampleX(delivSub);
		const int dSubY = SubsampleY(delivSub);
		const int cSubX = SubsampleX(codedSub);
		const int cSubY = SubsampleY(codedSub);
		const int cW = ChromaDetail::SubsampledDim(lumaW, cSubX);
		const int cH = ChromaDetail::SubsampledDim(lumaH, cSubY);

		// Exact identity when coded == delivered!
		if (codedSub == delivSub && delivered.w == cW && delivered.h == cH)
			return delivered;

		TdV2Plane out(cW, cH, 0.0);
		const ActiveRect dActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, dSubX, dSubY, delivered.w, delivered.h);
		const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, cSubX, cSubY, cW, cH);
		if (dActive.Empty() || cActive.Empty())
			return out;

		ActiveSubPlaneView dView{ &delivered, dActive.x0, dActive.y0, dActive.Width(), dActive.Height() };
		bool anyBorderFallback = false;
		bool anyNonFinite = false;

		for (int cy = cActive.y0; cy < cActive.y1; ++cy)
		{
			const double ly = siting.CodedToLumaY(static_cast<double>(cy), cSubY);
			const double dyLocal = siting.LumaToCodedY(ly, dSubY) - static_cast<double>(dActive.y0);
			for (int cx = cActive.x0; cx < cActive.x1; ++cx)
			{
				const double lx = siting.CodedToLumaX(static_cast<double>(cx), cSubX);
				const double dxLocal = siting.LumaToCodedX(lx, dSubX) - static_cast<double>(dActive.x0);
				double val = 0.0;
				bool borderBilin = false;
				if (ChromaDetail::SampleSitedActive(dView, dxLocal, dyLocal, val, borderBilin, /*allowEdgeClampOutside=*/true))
				{
					out.at(cx, cy) = val;
					if (borderBilin)
						anyBorderFallback = true;
				}
				else
				{
					// Preserve non-finite flag on coded plane so downstream warpers sanitize visibly with validPx = 0.
					out.at(cx, cy) = std::numeric_limits<double>::quiet_NaN();
					anyNonFinite = true;
				}
			}
		}

		if (anyBorderFallback && standIns != nullptr)
			standIns->Engage(StandInKind::BorderBilinearFallback, "ReduceToCodedGrid active border");
		if (anyNonFinite && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "ReduceToCodedGrid non-finite sample");
		return out;
	}

	// Rebuild ONLY the signed coded chroma delta back to the delivered chroma grid honoring exact
	// SitingSpec and ActiveRect. Guarantees exact identity when coded == delivered, exact
	// 0.0 -> 0.0 preservation, and strictly 0.0 outside ActiveRect.
	inline TdV2Plane UpsampleDeltaToDelivered(
		const TdV2Plane& codedDelta,
		int lumaW,
		int lumaH,
		ChromaSubsampling codedSub,
		DeliveredChromaFormat deliveredFmt,
		const SitingSpec& siting,
		const ActiveRect& lumaActive,
		StandInRegistry* standIns = nullptr)
	{
		if (codedDelta.Empty() || lumaW <= 0 || lumaH <= 0)
			return {};

		const ChromaSubsampling delivSub = DeliveredSubsampling(deliveredFmt);
		const int dSubX = SubsampleX(delivSub);
		const int dSubY = SubsampleY(delivSub);
		const int cSubX = SubsampleX(codedSub);
		const int cSubY = SubsampleY(codedSub);
		const int dW = ChromaDetail::SubsampledDim(lumaW, dSubX);
		const int dH = ChromaDetail::SubsampledDim(lumaH, dSubY);

		// Exact identity when coded == delivered!
		if (codedSub == delivSub && codedDelta.w == dW && codedDelta.h == dH)
			return codedDelta;

		TdV2Plane out(dW, dH, 0.0);
		const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, cSubX, cSubY, codedDelta.w, codedDelta.h);
		const ActiveRect dActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, dSubX, dSubY, dW, dH);
		if (cActive.Empty() || dActive.Empty())
			return out;

		// Fast exact-zero shortcut: zero coded delta produces bit-exact 0.0 delivered delta with zero border fallbacks.
		bool allZero = true;
		for (int cy = cActive.y0; cy < cActive.y1 && allZero; ++cy)
			for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				if (codedDelta.at(cx, cy) != 0.0) { allZero = false; break; }
		if (allZero)
			return out;

		ActiveSubPlaneView cView{ &codedDelta, cActive.x0, cActive.y0, cActive.Width(), cActive.Height() };
		bool anyBorderFallback = false;
		bool anyNonFinite = false;

		for (int dy = dActive.y0; dy < dActive.y1; ++dy)
		{
			const double ly = siting.CodedToLumaY(static_cast<double>(dy), dSubY);
			const double cyLocal = siting.LumaToCodedY(ly, cSubY) - static_cast<double>(cActive.y0);
			for (int dx = dActive.x0; dx < dActive.x1; ++dx)
			{
				const double lx = siting.CodedToLumaX(static_cast<double>(dx), dSubX);
				const double cxLocal = siting.LumaToCodedX(lx, cSubX) - static_cast<double>(cActive.x0);
				double val = 0.0;
				bool borderBilin = false;
				if (ChromaDetail::SampleSitedActive(cView, cxLocal, cyLocal, val, borderBilin, /*allowEdgeClampOutside=*/true))
				{
					out.at(dx, dy) = (val == 0.0) ? 0.0 : val;
					if (borderBilin)
						anyBorderFallback = true;
				}
				else
				{
					out.at(dx, dy) = 0.0;
					anyNonFinite = true;
				}
			}
		}

		if (anyBorderFallback && standIns != nullptr)
			standIns->Engage(StandInKind::BorderBilinearFallback, "UpsampleDeltaToDelivered active border");
		if (anyNonFinite && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "UpsampleDeltaToDelivered non-finite sample");
		return out;
	}

	// ---- 4:4:4 Anchor Memory Canvas & Carried CfL (plan s1.5 #2, H5) ----

	struct AnchorCflCell
	{
		double aCb = 0.0;
		double bCb = 0.0;
		double aCr = 0.0;
		double bCr = 0.0;
		bool valid = false;
	};

	class TdV2ChromaAnchorCanvas
	{
	public:
		void Reset()
		{
			w_ = 0;
			h_ = 0;
			cellsW_ = 0;
			cellsH_ = 0;
			cellPx_ = kCanonicalBlockPx;
			codedSubX_ = 2;
			codedSubY_ = 2;
			active_ = {};
			originOffX_ = 0.0;
			originOffY_ = 0.0;
			sumCb_ = TdV2Plane();
			sumCr_ = TdV2Plane();
			weight_ = TdV2Plane();
			anchorCleanY_ = TdV2Plane();
			anchorCleanYValid_.clear();
			cflStats_.clear();
			cflCells_.clear();
			accumulatedFrames_ = 0;
			lastAccumSeq_ = 0;
			hasLastAccumSeq_ = false;
			hasData_ = false;
			hasCfl_ = false;
		}

		// Reset per-anchor CfL (a, b) and reference-aligned P_Y when starting a new anchor without a scene cut.
		void ResetCfl()
		{
			anchorCleanY_ = TdV2Plane();
			anchorCleanYValid_.clear();
			cflStats_.assign(static_cast<size_t>(cellsW_) * static_cast<size_t>(cellsH_), CflAccum{});
			cflCells_.assign(static_cast<size_t>(cellsW_) * static_cast<size_t>(cellsH_), AnchorCflCell{});
			hasCfl_ = false;
		}

		bool HasData() const { return hasData_; }
		bool HasCfl() const { return hasCfl_; }
		int Width() const { return w_; }
		int Height() const { return h_; }
		int CellsW() const { return cellsW_; }
		int CellsH() const { return cellsH_; }
		const ActiveRect& Active() const { return active_; }
		size_t AccumulatedFrames() const { return accumulatedFrames_; }
		const TdV2Plane& AnchorCleanY() const { return anchorCleanY_; }
		const TdV2Plane& WeightPlane() const { return weight_; }
		const std::vector<AnchorCflCell>& CflCells() const { return cflCells_; }

		// Accumulate one grain-free anchor frame's coded chroma (and optional anchor clean luma for CfL)
		// into the full luma-resolution (lumaW x lumaH) 4:4:4 memory canvas with rate-aware OnePole decay
		// over the near-I horizon kAnchorCanvasTauSec (bounded weight, zero unbounded drift).
		// Splat convention: `splatMotion` (vx, vy) gives the anchor frame's displacement from the canvas
		// origin (x_frame = x_canvas + v), so coded sample at luma (x_luma, y_luma) splats onto canvas
		// coordinate (x_luma - vx, y_luma - vy) strictly inside `lumaActive`, and anchorCleanY_ is aligned
		// into the exact same canvas coordinate frame. NEVER pass grainy chroma!
		bool AccumulateAnchorFrame(
			const TdV2Plane& codedCleanCb,
			const TdV2Plane& codedCleanCr,
			const TdV2Plane* anchorCleanY,
			int lumaW,
			int lumaH,
			ChromaSubsampling codedSub,
			const SitingSpec& siting,
			const ActiveRect& lumaActive,
			const CellMotionField* splatMotion,
			int cellPx,
			int sourceBits,
			StandInRegistry* standIns = nullptr,
			double sourceHz = kDefaultSourceHz,
			uint64_t frameSeq = 0,
			bool resetCflForNewAnchor = false)
		{
			if (codedCleanCb.Empty() || codedCleanCr.Empty() || lumaW <= 0 || lumaH <= 0 || cellPx <= 0)
				return false;
			if (!(PeakCodeForBits(sourceBits) > 0.0))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaAnchorCanvas invalid sourceBits");
				return false;
			}

			const int cSubX = SubsampleX(codedSub);
			const int cSubY = SubsampleY(codedSub);
			const int cW = ChromaDetail::SubsampledDim(lumaW, cSubX);
			const int cH = ChromaDetail::SubsampledDim(lumaH, cSubY);
			if (codedCleanCb.w != cW || codedCleanCb.h != cH || codedCleanCr.w != cW || codedCleanCr.h != cH)
				return false;

			const ActiveRect ar = lumaActive.Empty() ? ActiveRect::Full(lumaW, lumaH) : lumaActive.ClampedTo(lumaW, lumaH);
			if (ar.Empty())
				return false;

			const int cw = (lumaW + cellPx - 1) / cellPx;
			const int ch = (lumaH + cellPx - 1) / cellPx;
			const size_t numCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);

			// Reject malformed splatMotion before mutating canvas state (consistent with tryAddPeakRef and anchorReadMotion).
			if (splatMotion != nullptr && !splatMotion->IsWellFormed(cw, ch))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaAnchorCanvas malformed splatMotion");
				return false;
			}

			// Fractional siting origin offset so coded samples align to exact integer canvas lattice sites
			// for TopLeftType2 (0, 0), Left (0, 0.5), and Center (0.5, 0.5), eliminating half-pixel box blur
			// on static/shifted anchors and preventing odd-dimension edge row/column drops.
			const double baseX = siting.CodedToLumaX(0.0, cSubX);
			const double baseY = siting.CodedToLumaY(0.0, cSubY);
			const double offX = baseX - std::floor(baseX);
			const double offY = baseY - std::floor(baseY);

			// Re-initialize canvas if dimensions, cell grid, ActiveRect, subsampling, or siting origin changed.
			const bool needsReset = (!hasData_ || w_ != lumaW || h_ != lumaH || cellsW_ != cw || cellsH_ != ch || cellPx_ != cellPx ||
			                         codedSubX_ != cSubX || codedSubY_ != cSubY ||
			                         active_.x0 != ar.x0 || active_.y0 != ar.y0 || active_.x1 != ar.x1 || active_.y1 != ar.y1 ||
			                         std::fabs(originOffX_ - offX) > kEngineNumFloor || std::fabs(originOffY_ - offY) > kEngineNumFloor);

			// H9 (plan s1.2 L108): allow identity (0, 0) splat ONLY when seeding the first anchor (!HasData() / reset).
			// On an already-populated canvas, refuse to fabricate whole-frame "Still" when splatMotion is null!
			const bool useMotion = (splatMotion != nullptr);
			if (!useMotion && !needsReset)
			{
				if (standIns != nullptr)
				{
					standIns->Engage(StandInKind::MotionFieldStandIn, "anchor splatMotion null on populated canvas; skipped per L4");
					standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "L4 missing anchor splatMotion: accumulation skipped (no Still assumption)");
				}
				return false;
			}

			if (needsReset)
			{
				Reset();
				w_ = lumaW;
				h_ = lumaH;
				cellsW_ = cw;
				cellsH_ = ch;
				cellPx_ = cellPx;
				codedSubX_ = cSubX;
				codedSubY_ = cSubY;
				active_ = ar;
				originOffX_ = offX;
				originOffY_ = offY;
				sumCb_ = TdV2Plane(w_, h_, 0.0);
				sumCr_ = TdV2Plane(w_, h_, 0.0);
				weight_ = TdV2Plane(w_, h_, 0.0);
				cflStats_.assign(numCells, CflAccum{});
				cflCells_.assign(numCells, AnchorCflCell{});
			}

			// Rate-aware exponential decay of prior accumulated weights via shared U23 OnePole::AlphaFor
			// so accumulated weights stay strictly bounded <= 1 / alpha across arbitrary stream lengths.
			// If elapsed time exceeds the near-I horizon or caller requests a new anchor CfL, reset per-anchor CfL.
			if (hasData_)
			{
				const double effHz = (std::isfinite(sourceHz) && sourceHz > 0.0) ? sourceHz : kDefaultSourceHz;
				const double dtFrames = (hasLastAccumSeq_ && frameSeq > lastAccumSeq_)
					? static_cast<double>(frameSeq - lastAccumSeq_)
					: 1.0;
				const double dtSec = dtFrames / effHz;
				if (resetCflForNewAnchor || dtSec > kAnchorCanvasTauSec)
					ResetCfl();
				const double decayAlpha = OnePole::AlphaFor(dtSec, kAnchorCanvasTauSec);
				const double retain = RampMath::Clamp01(1.0 - decayAlpha);
				for (int y = ar.y0; y < ar.y1; ++y)
				{
					for (int x = ar.x0; x < ar.x1; ++x)
					{
						sumCb_.at(x, y) *= retain;
						sumCr_.at(x, y) *= retain;
						weight_.at(x, y) *= retain;
					}
				}
			}

			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(ar, lumaW, lumaH, cSubX, cSubY, cW, cH);
			const bool hasValidAnchorY = (anchorCleanY != nullptr && !anchorCleanY->Empty() &&
			                              anchorCleanY->w == lumaW && anchorCleanY->h == lumaH);
			ActiveSubPlaneView yView{ anchorCleanY, ar.x0, ar.y0, ar.Width(), ar.Height() };

			bool anyNonFinite = false;
			bool anyBorderBilinear = false;

			// Validate anchorCleanY finite if provided.
			bool anchorYCleanFinite = hasValidAnchorY;
			if (hasValidAnchorY)
			{
				for (int y = ar.y0; y < ar.y1 && anchorYCleanFinite; ++y)
				{
					for (int x = ar.x0; x < ar.x1; ++x)
					{
						if (!std::isfinite(anchorCleanY->at(x, y)))
						{
							anchorYCleanFinite = false;
							anyNonFinite = true;
							break;
						}
					}
				}
			}

			// Plan s1.5 #2: fit anchor CfL (a, b) ONCE per anchor (strictly locked to this anchor's P_Y so
			// subsequent near-I splats never drift CfL away from anchorCleanY_).
			const bool fitCflThisAnchor = anchorYCleanFinite && !hasCfl_;

			size_t splattedSamples = 0;
			for (int cy = cActive.y0; cy < cActive.y1; ++cy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(cy), cSubY);
				const int mcy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ly)) / cellPx));
				for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				{
					const double cbVal = codedCleanCb.at(cx, cy);
					const double crVal = codedCleanCr.at(cx, cy);
					if (!std::isfinite(cbVal) || !std::isfinite(crVal))
					{
						anyNonFinite = true;
						continue;
					}

					const double lx = siting.CodedToLumaX(static_cast<double>(cx), cSubX);
					const int mcx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(lx)) / cellPx));
					const size_t cIdx = static_cast<size_t>(mcy) * cw + mcx;

					double vx = 0.0, vy = 0.0;
					if (useMotion)
					{
						if (splatMotion->valid[cIdx] == 0u)
							continue;
						vx = splatMotion->vx[cIdx];
						vy = splatMotion->vy[cIdx];
						if (!std::isfinite(vx) || !std::isfinite(vy))
						{
							anyNonFinite = true;
							continue;
						}
					}

					// Splat strictly inside active luma canvas at siting-aligned motion-compensated coordinate
					// (ax - originOffX_, ay - originOffY_), and gate CfL accumulation under the exact same
					// in-bounds active guard so out-of-ActiveRect panning pixels never pollute border CfL cells.
					const double ax = lx - vx;
					const double ay = ly - vy;
					const double axAligned = ax - originOffX_;
					const double ayAligned = ay - originOffY_;
					if (axAligned >= static_cast<double>(ar.x0) && axAligned <= static_cast<double>(ar.x1 - 1) &&
					    ayAligned >= static_cast<double>(ar.y0) && ayAligned <= static_cast<double>(ar.y1 - 1))
					{
						const int ix0 = static_cast<int>(std::floor(axAligned));
						const int iy0 = static_cast<int>(std::floor(ayAligned));
						const int ix1 = (std::min)(ar.x1 - 1, ix0 + 1);
						const int iy1 = (std::min)(ar.y1 - 1, iy0 + 1);
						const double fx = axAligned - static_cast<double>(ix0);
						const double fy = ayAligned - static_cast<double>(iy0);

						const double w00 = (1.0 - fx) * (1.0 - fy);
						const double w10 = fx * (1.0 - fy);
						const double w01 = (1.0 - fx) * fy;
						const double w11 = fx * fy;

						auto splat = [&](int px, int py, double wt)
						{
							if (wt <= 0.0) return;
							sumCb_.at(px, py) += wt * cbVal;
							sumCr_.at(px, py) += wt * crVal;
							weight_.at(px, py) += wt;
						};
						splat(ix0, iy0, w00);
						splat(ix1, iy0, w10);
						splat(ix0, iy1, w01);
						splat(ix1, iy1, w11);
						++splattedSamples;

						// Fit per-cell anchor CfL (a, b) from anchor clean luma sampled at exact sited coded coords
						// and anchor clean chroma (strictly on the in-bounds anchor cell corresponding to (axAligned, ayAligned)).
						if (fitCflThisAnchor)
						{
							double ySample = 0.0;
							bool borderBilin = false;
							const double yLocalX = lx - static_cast<double>(ar.x0);
							const double yLocalY = ly - static_cast<double>(ar.y0);
							if (ChromaDetail::SampleSitedActive(yView, yLocalX, yLocalY, ySample, borderBilin, /*allowEdgeClampOutside=*/true))
							{
								if (borderBilin)
									anyBorderBilinear = true;
								const int acx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(axAligned)) / cellPx));
								const int acy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ayAligned)) / cellPx));
								CflAccum& acc = cflStats_[static_cast<size_t>(acy) * cw + acx];
								acc.sumY += ySample;
								acc.sumYY += ySample * ySample;
								acc.sumCb += cbVal;
								acc.sumYCb += ySample * cbVal;
								acc.sumCr += crVal;
								acc.sumYCr += ySample * crVal;
								++acc.count;
							}
						}
					}
				}
			}

			if (splattedSamples > 0)
			{
				hasData_ = true;
				++accumulatedFrames_;
				lastAccumSeq_ = frameSeq;
				hasLastAccumSeq_ = true;
			}

			// Capture and align anchor clean luma P_Y into the EXACT canvas coordinate system:
			// since x_frame = x_canvas + vx, canvas pixel (x, y) samples anchorCleanY at (x + vx, y + vy).
			// Eliminates P_Y coordinate offset whenever the anchor splat has non-zero motion (vx, vy) != (0, 0).
			if (fitCflThisAnchor && anchorCleanY_.Empty())
			{
				anchorCleanY_ = *anchorCleanY;
				anchorCleanYValid_.assign(static_cast<size_t>(w_) * static_cast<size_t>(h_), 1u);
				if (useMotion)
				{
					bool anyNonZeroSplat = false;
					for (size_t i = 0; i < numCells; ++i)
					{
						if (splatMotion->valid[i] == 0u ||
						    splatMotion->vx[i] != 0.0 || splatMotion->vy[i] != 0.0)
						{
							anyNonZeroSplat = true;
							break;
						}
					}
					if (anyNonZeroSplat)
					{
						for (int y = ar.y0; y < ar.y1; ++y)
						{
							const int mcy = (std::max)(0, (std::min)(ch - 1, y / cellPx));
							for (int x = ar.x0; x < ar.x1; ++x)
							{
								const int mcx = (std::max)(0, (std::min)(cw - 1, x / cellPx));
								const size_t cIdx = static_cast<size_t>(mcy) * cw + mcx;
								const size_t pIdx = static_cast<size_t>(y) * w_ + x;
								if (splatMotion->valid[cIdx] == 0u)
								{
									anchorCleanYValid_[pIdx] = 0u;
									continue;
								}
								const double vx = splatMotion->vx[cIdx];
								const double vy = splatMotion->vy[cIdx];
								if (!std::isfinite(vx) || !std::isfinite(vy))
								{
									anyNonFinite = true;
									anchorCleanYValid_[pIdx] = 0u;
									continue;
								}
								if (vx == 0.0 && vy == 0.0)
									continue;
								const double sxLocal = static_cast<double>(x - ar.x0) + vx;
								const double syLocal = static_cast<double>(y - ar.y0) + vy;
								double yVal = 0.0;
								bool borderBilin = false;
								if (ChromaDetail::SampleSitedActive(yView, sxLocal, syLocal, yVal, borderBilin, /*allowEdgeClampOutside=*/false))
								{
									anchorCleanY_.at(x, y) = yVal;
									if (borderBilin)
										anyBorderBilinear = true;
								}
								else
								{
									anchorCleanY_.at(x, y) = ChromaDetail::SampleBilinearClampedLocal(yView, sxLocal, syLocal);
									anchorCleanYValid_[pIdx] = 0u;
								}
							}
						}
					}
				}
			}

			if (anyNonFinite && standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaAnchorCanvas non-finite sample");
			if (anyBorderBilinear && standIns != nullptr)
				standIns->Engage(StandInKind::BorderBilinearFallback, "TdV2ChromaAnchorCanvas CfL luma border");

			// Fit per-cell OLS CfL (aCb, bCb, aCr, bCr) with derived uniform 1-LSB quantization variance ridge (Rule 1).
			if (fitCflThisAnchor)
			{
				// Exact uniform 1-LSB quantization variance in native integer code units:
				// Var(Uniform(-delta/2, +delta/2)) = delta^2 / 12.0 with delta = kDefaultStepPriorLsb = 1.0.
				const double lsbQuantVar = (kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0;
				const double ridgeFloor = (std::max)(lsbQuantVar, kEngineNumFloor);
				bool anyValidCfl = false;
				for (size_t i = 0; i < numCells; ++i)
				{
					const CflAccum& acc = cflStats_[i];
					AnchorCflCell& outCell = cflCells_[i];
					if (acc.count < 2)
					{
						outCell = AnchorCflCell{};
						continue;
					}
					const double invN = 1.0 / static_cast<double>(acc.count);
					const double meanY = acc.sumY * invN;
					const double meanCb = acc.sumCb * invN;
					const double meanCr = acc.sumCr * invN;
					const double varY = (std::max)(0.0, acc.sumYY * invN - meanY * meanY);
					const double covYCb = acc.sumYCb * invN - meanY * meanCb;
					const double covYCr = acc.sumYCr * invN - meanY * meanCr;

					const double denom = varY + ridgeFloor;
					outCell.aCb = covYCb / denom;
					outCell.bCb = meanCb - outCell.aCb * meanY;
					outCell.aCr = covYCr / denom;
					outCell.bCr = meanCr - outCell.aCr * meanY;
					outCell.valid = std::isfinite(outCell.aCb) && std::isfinite(outCell.bCb) &&
					                std::isfinite(outCell.aCr) && std::isfinite(outCell.bCr);
					if (outCell.valid)
						anyValidCfl = true;
				}
				hasCfl_ = anyValidCfl && !anchorCleanY_.Empty();
			}

			return hasData_;
		}

		// Materialize normalized hole-free 4:4:4 Cb/Cr planes strictly inside active_.
		// Direct observations require weight >= kMinCanvasFootprintWeight (1/16, derived from 4x4 bicubic support).
		// Unpopulated gaps are smoothly interpolated so bicubic sampling never rings on 0.0 holes, while:
		//   - Any gap-fill increments *outGapFilled and visibly engages StandInKind::ChromaCanvasGapFilled (Rule 4 / H4).
		//   - Pixels are marked trusted (outTrusted == 1) ONLY when directly supported (weight >= kMinCanvasFootprintWeight)
		//     OR gap-filled across a span <= subsampling factor (<= maxValidGapX / <= maxValidGapY, e.g. <= 2 px for 4:2:0).
		//     Any wider gap stays outTrusted == 0 so invented gap-fill is never fused as a real observation!
		bool MaterializeCanvas(
			TdV2Plane& outCb,
			TdV2Plane& outCr,
			uint64_t* outGapFilled = nullptr,
			StandInRegistry* standIns = nullptr,
			std::vector<uint8_t>* outTrusted = nullptr,
			int maxValidGapX = 0,
			int maxValidGapY = 0) const
		{
			if (!hasData_ || w_ <= 0 || h_ <= 0 || active_.Empty())
				return false;
			outCb = TdV2Plane(w_, h_, 0.0);
			outCr = TdV2Plane(w_, h_, 0.0);
			const size_t totalPx = static_cast<size_t>(w_) * static_cast<size_t>(h_);
			std::vector<uint8_t> populated(totalPx, 0u);
			std::vector<uint8_t> trusted(totalPx, 0u);

			const int effMaxGapX = maxValidGapX > 0 ? maxValidGapX : codedSubX_;
			const int effMaxGapY = maxValidGapY > 0 ? maxValidGapY : codedSubY_;

			size_t directCount = 0;
			size_t gapCount = 0;
			for (int y = active_.y0; y < active_.y1; ++y)
			{
				for (int x = active_.x0; x < active_.x1; ++x)
				{
					const double wt = weight_.at(x, y);
					if (wt >= kMinCanvasFootprintWeight)
					{
						const double invW = 1.0 / wt;
						outCb.at(x, y) = sumCb_.at(x, y) * invW;
						outCr.at(x, y) = sumCr_.at(x, y) * invW;
						const size_t idx = static_cast<size_t>(y) * w_ + x;
						populated[idx] = 1u;
						trusted[idx] = 1u;
						++directCount;
					}
					else
					{
						++gapCount;
					}
				}
			}

			if (directCount == 0)
				return false;

			if (gapCount == 0)
			{
				if (outTrusted != nullptr)
					*outTrusted = std::move(trusted);
				return true;
			}

			uint64_t filledPixels = 0;

			// Pass 1: Horizontal linear gap-fill along each active row between nearest directly populated columns.
			for (int y = active_.y0; y < active_.y1; ++y)
			{
				int lastPop = -1;
				for (int x = active_.x0; x < active_.x1; ++x)
				{
					if (populated[static_cast<size_t>(y) * w_ + x] == 0u)
						continue;
					if (lastPop < 0)
					{
						const int edgeGap = x - active_.x0;
						const uint8_t edgeTrust = (edgeGap <= effMaxGapX) ? 1u : 0u;
						for (int gx = active_.x0; gx < x; ++gx)
						{
							outCb.at(gx, y) = outCb.at(x, y);
							outCr.at(gx, y) = outCr.at(x, y);
							const size_t gIdx = static_cast<size_t>(y) * w_ + gx;
							populated[gIdx] = 1u;
							trusted[gIdx] = edgeTrust;
							++filledPixels;
						}
					}
					else if (x > lastPop + 1)
					{
						const int gapLen = x - lastPop - 1;
						const uint8_t gapTrust = (gapLen <= effMaxGapX) ? 1u : 0u;
						const double span = static_cast<double>(x - lastPop);
						const double cb0 = outCb.at(lastPop, y), cb1 = outCb.at(x, y);
						const double cr0 = outCr.at(lastPop, y), cr1 = outCr.at(x, y);
						for (int gx = lastPop + 1; gx < x; ++gx)
						{
							const double t = static_cast<double>(gx - lastPop) / span;
							outCb.at(gx, y) = RampMath::Mix(cb0, cb1, t);
							outCr.at(gx, y) = RampMath::Mix(cr0, cr1, t);
							const size_t gIdx = static_cast<size_t>(y) * w_ + gx;
							populated[gIdx] = 1u;
							trusted[gIdx] = gapTrust;
							++filledPixels;
						}
					}
					lastPop = x;
				}
				if (lastPop >= 0 && lastPop + 1 < active_.x1)
				{
					const int edgeGap = (active_.x1 - 1) - lastPop;
					const uint8_t edgeTrust = (edgeGap <= effMaxGapX) ? 1u : 0u;
					for (int gx = lastPop + 1; gx < active_.x1; ++gx)
					{
						outCb.at(gx, y) = outCb.at(lastPop, y);
						outCr.at(gx, y) = outCr.at(lastPop, y);
						const size_t gIdx = static_cast<size_t>(y) * w_ + gx;
						populated[gIdx] = 1u;
						trusted[gIdx] = edgeTrust;
						++filledPixels;
					}
				}
			}

			// Pass 2: Vertical linear gap-fill along each active column for any empty rows (e.g. odd rows on 1-frame Type2 4:2:0).
			// Vertical fills inherit trust ONLY when both vertical endpoints are trusted AND gapLen <= effMaxGapY.
			for (int x = active_.x0; x < active_.x1; ++x)
			{
				int lastPop = -1;
				for (int y = active_.y0; y < active_.y1; ++y)
				{
					if (populated[static_cast<size_t>(y) * w_ + x] == 0u)
						continue;
					if (lastPop < 0)
					{
						const int edgeGap = y - active_.y0;
						const uint8_t endTrust = trusted[static_cast<size_t>(y) * w_ + x];
						const uint8_t edgeTrust = (edgeGap <= effMaxGapY && endTrust != 0u) ? 1u : 0u;
						for (int gy = active_.y0; gy < y; ++gy)
						{
							outCb.at(x, gy) = outCb.at(x, y);
							outCr.at(x, gy) = outCr.at(x, y);
							const size_t gIdx = static_cast<size_t>(gy) * w_ + x;
							populated[gIdx] = 1u;
							trusted[gIdx] = edgeTrust;
							++filledPixels;
						}
					}
					else if (y > lastPop + 1)
					{
						const int gapLen = y - lastPop - 1;
						const uint8_t t0 = trusted[static_cast<size_t>(lastPop) * w_ + x];
						const uint8_t t1 = trusted[static_cast<size_t>(y) * w_ + x];
						const uint8_t gapTrust = (gapLen <= effMaxGapY && t0 != 0u && t1 != 0u) ? 1u : 0u;
						const double span = static_cast<double>(y - lastPop);
						const double cb0 = outCb.at(x, lastPop), cb1 = outCb.at(x, y);
						const double cr0 = outCr.at(x, lastPop), cr1 = outCr.at(x, y);
						for (int gy = lastPop + 1; gy < y; ++gy)
						{
							const double t = static_cast<double>(gy - lastPop) / span;
							outCb.at(x, gy) = RampMath::Mix(cb0, cb1, t);
							outCr.at(x, gy) = RampMath::Mix(cr0, cr1, t);
							const size_t gIdx = static_cast<size_t>(gy) * w_ + x;
							populated[gIdx] = 1u;
							trusted[gIdx] = gapTrust;
							++filledPixels;
						}
					}
					lastPop = y;
				}
				if (lastPop >= 0 && lastPop + 1 < active_.y1)
				{
					const int edgeGap = (active_.y1 - 1) - lastPop;
					const uint8_t endTrust = trusted[static_cast<size_t>(lastPop) * w_ + x];
					const uint8_t edgeTrust = (edgeGap <= effMaxGapY && endTrust != 0u) ? 1u : 0u;
					for (int gy = lastPop + 1; gy < active_.y1; ++gy)
					{
						outCb.at(x, gy) = outCb.at(x, lastPop);
						outCr.at(x, gy) = outCr.at(x, lastPop);
						const size_t gIdx = static_cast<size_t>(gy) * w_ + x;
						populated[gIdx] = 1u;
						trusted[gIdx] = edgeTrust;
						++filledPixels;
					}
				}
			}

			if (filledPixels > 0)
			{
				if (outGapFilled != nullptr)
					*outGapFilled += filledPixels;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::ChromaCanvasGapFilled);
			}
			if (outTrusted != nullptr)
				*outTrusted = std::move(trusted);
			return true;
		}

		// Sample the accumulated 4:4:4 canvas at coded-grid coordinates mapped to (x_luma(cx) + vx, y_luma(cy) + vy)
		// strictly inside `lumaActive`. `lumaMotion` is the Current -> Anchor luma motion field (null => Still 0,0).
		// Invalid-motion, non-finite, OOB, and untrusted wide-gap pixels keep fallbackCodedCb/Cr (H3: zero 0.0 holes)
		// with outValid == 0.
		bool SampleAtCodedGrid(
			ChromaSubsampling codedSub,
			const SitingSpec& siting,
			const ActiveRect& lumaActive,
			const CellMotionField* lumaMotion,
			int cellPx,
			TdV2Plane& outCb,
			TdV2Plane& outCr,
			std::vector<uint8_t>& outValid,
			StandInRegistry* standIns = nullptr,
			const TdV2Plane* fallbackCodedCb = nullptr,
			const TdV2Plane* fallbackCodedCr = nullptr,
			uint64_t* outGapFilled = nullptr) const
		{
			if (!hasData_ || w_ <= 0 || h_ <= 0 || cellPx <= 0)
				return false;

			const ActiveRect ar = lumaActive.Empty() ? ActiveRect::Full(w_, h_) : lumaActive.ClampedTo(w_, h_);
			const int cSubX = SubsampleX(codedSub);
			const int cSubY = SubsampleY(codedSub);
			const int cW = ChromaDetail::SubsampledDim(w_, cSubX);
			const int cH = ChromaDetail::SubsampledDim(h_, cSubY);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(ar, w_, h_, cSubX, cSubY, cW, cH);

			TdV2Plane canvasCb, canvasCr;
			std::vector<uint8_t> canvasTrusted;
			if (!MaterializeCanvas(canvasCb, canvasCr, outGapFilled, standIns, &canvasTrusted, cSubX, cSubY))
				return false;

			const bool hasFallback = (fallbackCodedCb != nullptr && fallbackCodedCr != nullptr &&
			                          fallbackCodedCb->w == cW && fallbackCodedCb->h == cH &&
			                          fallbackCodedCr->w == cW && fallbackCodedCr->h == cH);
			outCb = hasFallback ? *fallbackCodedCb : TdV2Plane(cW, cH, 0.0);
			outCr = hasFallback ? *fallbackCodedCr : TdV2Plane(cW, cH, 0.0);
			outValid.assign(static_cast<size_t>(cW) * static_cast<size_t>(cH), 0u);
			if (ar.Empty() || cActive.Empty())
				return false;

			const int cw = (w_ + cellPx - 1) / cellPx;
			const int ch = (h_ + cellPx - 1) / cellPx;
			const bool useMotion = (lumaMotion != nullptr && lumaMotion->IsWellFormed(cw, ch));
			if (lumaMotion != nullptr && !useMotion && standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaAnchorCanvas malformed read motion");

			ActiveSubPlaneView cbView{ &canvasCb, ar.x0, ar.y0, ar.Width(), ar.Height() };
			ActiveSubPlaneView crView{ &canvasCr, ar.x0, ar.y0, ar.Width(), ar.Height() };
			bool anyBorder = false;
			bool anyNonFinite = false;

			auto fillHole = [&](int cx, int cy, double sxLocal, double syLocal)
			{
				if (hasFallback)
				{
					const double fCb = fallbackCodedCb->at(cx, cy);
					const double fCr = fallbackCodedCr->at(cx, cy);
					if (std::isfinite(fCb) && std::isfinite(fCr))
					{
						outCb.at(cx, cy) = fCb;
						outCr.at(cx, cy) = fCr;
						return;
					}
				}
				outCb.at(cx, cy) = ChromaDetail::SampleBilinearClampedLocal(cbView, sxLocal, syLocal);
				outCr.at(cx, cy) = ChromaDetail::SampleBilinearClampedLocal(crView, sxLocal, syLocal);
			};

			auto footprintTrusted = [&](double sxLocal, double syLocal) -> bool
			{
				const double cxClamped = (std::max)(0.0, (std::min)(static_cast<double>(ar.Width() - 1), sxLocal));
				const double cyClamped = (std::max)(0.0, (std::min)(static_cast<double>(ar.Height() - 1), syLocal));
				const int ix0 = ar.x0 + static_cast<int>(std::floor(cxClamped));
				const int iy0 = ar.y0 + static_cast<int>(std::floor(cyClamped));
				const int ix1 = (std::min)(ar.x1 - 1, ix0 + 1);
				const int iy1 = (std::min)(ar.y1 - 1, iy0 + 1);
				const double fx = cxClamped - std::floor(cxClamped);
				const double fy = cyClamped - std::floor(cyClamped);
				const int xEnd = (fx > kEngineNumFloor) ? ix1 : ix0;
				const int yEnd = (fy > kEngineNumFloor) ? iy1 : iy0;
				for (int py = iy0; py <= yEnd; ++py)
				{
					for (int px = ix0; px <= xEnd; ++px)
					{
						if (canvasTrusted[static_cast<size_t>(py) * w_ + px] == 0u)
							return false;
					}
				}
				return true;
			};

			for (int cy = cActive.y0; cy < cActive.y1; ++cy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(cy), cSubY);
				const int mcy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ly)) / cellPx));
				for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(cx), cSubX);
					const int mcx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(lx)) / cellPx));
					const size_t cIdx = static_cast<size_t>(mcy) * cw + mcx;
					const double sxBase = (lx - originOffX_) - static_cast<double>(ar.x0);
					const double syBase = (ly - originOffY_) - static_cast<double>(ar.y0);

					double vx = 0.0, vy = 0.0;
					if (useMotion)
					{
						if (lumaMotion->valid[cIdx] == 0u)
						{
							fillHole(cx, cy, sxBase, syBase);
							continue;
						}
						vx = lumaMotion->vx[cIdx];
						vy = lumaMotion->vy[cIdx];
						if (!std::isfinite(vx) || !std::isfinite(vy))
						{
							anyNonFinite = true;
							fillHole(cx, cy, sxBase, syBase);
							continue;
						}
					}

					const double sxLocal = ((lx + vx) - originOffX_) - static_cast<double>(ar.x0);
					const double syLocal = ((ly + vy) - originOffY_) - static_cast<double>(ar.y0);
					double cbVal = 0.0, crVal = 0.0;
					bool bCb = false, bCr = false;
					if (ChromaDetail::SampleSitedActive(cbView, sxLocal, syLocal, cbVal, bCb, /*allowEdgeClampOutside=*/false) &&
					    ChromaDetail::SampleSitedActive(crView, sxLocal, syLocal, crVal, bCr, /*allowEdgeClampOutside=*/false) &&
					    footprintTrusted(sxLocal, syLocal))
					{
						outCb.at(cx, cy) = cbVal;
						outCr.at(cx, cy) = crVal;
						outValid[static_cast<size_t>(cy) * cW + cx] = 1u;
						if (bCb || bCr)
							anyBorder = true;
					}
					else
					{
						// Fill with codedClean (or clamped canvas sample) to prevent 0.0-hole ringing while keeping outValid == 0.
						fillHole(cx, cy, sxLocal, syLocal);
					}
				}
			}

			if (anyBorder && standIns != nullptr)
				standIns->Engage(StandInKind::BorderBilinearFallback, "TdV2ChromaAnchorCanvas sample border");
			if (anyNonFinite && standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaAnchorCanvas non-finite motion");
			return true;
		}

		// Carried CfL (H5): applies anchor-carried (a, b) STRICTLY to OUR reference-aligned prediction luma
		// P_Y = Warp(anchorCleanY) at (x_luma(cx) + vx, y_luma(cy) + vy). NEVER reads current frame's luma!
		// Invalid-motion, non-finite, invalid-CfL, and OOB pixels keep fallbackCodedCb/Cr (H3: zero 0.0 holes)
		// with outValid == 0.
		bool PredictCarriedCflAtCodedGrid(
			ChromaSubsampling codedSub,
			const SitingSpec& siting,
			const ActiveRect& lumaActive,
			const CellMotionField* lumaMotion,
			int cellPx,
			TdV2Plane& outCb,
			TdV2Plane& outCr,
			std::vector<uint8_t>& outValid,
			StandInRegistry* standIns = nullptr,
			const TdV2Plane* fallbackCodedCb = nullptr,
			const TdV2Plane* fallbackCodedCr = nullptr) const
		{
			if (!hasCfl_ || anchorCleanY_.Empty() || w_ <= 0 || h_ <= 0 || cellPx <= 0)
				return false;

			const ActiveRect ar = lumaActive.Empty() ? ActiveRect::Full(w_, h_) : lumaActive.ClampedTo(w_, h_);
			const int cSubX = SubsampleX(codedSub);
			const int cSubY = SubsampleY(codedSub);
			const int cW = ChromaDetail::SubsampledDim(w_, cSubX);
			const int cH = ChromaDetail::SubsampledDim(h_, cSubY);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(ar, w_, h_, cSubX, cSubY, cW, cH);

			const bool hasFallback = (fallbackCodedCb != nullptr && fallbackCodedCr != nullptr &&
			                          fallbackCodedCb->w == cW && fallbackCodedCb->h == cH &&
			                          fallbackCodedCr->w == cW && fallbackCodedCr->h == cH);
			outCb = hasFallback ? *fallbackCodedCb : TdV2Plane(cW, cH, 0.0);
			outCr = hasFallback ? *fallbackCodedCr : TdV2Plane(cW, cH, 0.0);
			outValid.assign(static_cast<size_t>(cW) * static_cast<size_t>(cH), 0u);
			if (ar.Empty() || cActive.Empty())
				return false;

			const int cw = (w_ + cellPx - 1) / cellPx;
			const int ch = (h_ + cellPx - 1) / cellPx;
			const bool useMotion = (lumaMotion != nullptr && lumaMotion->IsWellFormed(cw, ch));
			if (lumaMotion != nullptr && !useMotion && standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaAnchorCanvas malformed CfL motion");

			ActiveSubPlaneView yView{ &anchorCleanY_, ar.x0, ar.y0, ar.Width(), ar.Height() };
			const bool hasYValidMask = (anchorCleanYValid_.size() == static_cast<size_t>(w_) * static_cast<size_t>(h_));
			bool anyBorder = false;
			bool anyNonFinite = false;

			auto anchorYFootprintValid = [&](double sxLocal, double syLocal) -> bool
			{
				if (!hasYValidMask)
					return true;
				const double cxClamped = (std::max)(0.0, (std::min)(static_cast<double>(ar.Width() - 1), sxLocal));
				const double cyClamped = (std::max)(0.0, (std::min)(static_cast<double>(ar.Height() - 1), syLocal));
				const int ix0 = ar.x0 + static_cast<int>(std::floor(cxClamped));
				const int iy0 = ar.y0 + static_cast<int>(std::floor(cyClamped));
				const int ix1 = (std::min)(ar.x1 - 1, ix0 + 1);
				const int iy1 = (std::min)(ar.y1 - 1, iy0 + 1);
				const double fx = cxClamped - std::floor(cxClamped);
				const double fy = cyClamped - std::floor(cyClamped);
				const int xEnd = (fx > kEngineNumFloor) ? ix1 : ix0;
				const int yEnd = (fy > kEngineNumFloor) ? iy1 : iy0;
				for (int py = iy0; py <= yEnd; ++py)
				{
					for (int px = ix0; px <= xEnd; ++px)
					{
						if (anchorCleanYValid_[static_cast<size_t>(py) * w_ + px] == 0u)
							return false;
					}
				}
				return true;
			};

			auto fillCflHole = [&](int cx, int cy, double sxLocal, double syLocal, const AnchorCflCell* optCfl)
			{
				if (hasFallback)
				{
					const double fCb = fallbackCodedCb->at(cx, cy);
					const double fCr = fallbackCodedCr->at(cx, cy);
					if (std::isfinite(fCb) && std::isfinite(fCr))
					{
						outCb.at(cx, cy) = fCb;
						outCr.at(cx, cy) = fCr;
						return;
					}
				}
				if (optCfl != nullptr && optCfl->valid)
				{
					const double edgePy = ChromaDetail::SampleBilinearClampedLocal(yView, sxLocal, syLocal);
					const double cbF = optCfl->aCb * edgePy + optCfl->bCb;
					const double crF = optCfl->aCr * edgePy + optCfl->bCr;
					if (std::isfinite(cbF) && std::isfinite(crF))
					{
						outCb.at(cx, cy) = cbF;
						outCr.at(cx, cy) = crF;
						return;
					}
				}
			};

			for (int cy = cActive.y0; cy < cActive.y1; ++cy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(cy), cSubY);
				const int mcy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ly)) / cellPx));
				for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(cx), cSubX);
					const int mcx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(lx)) / cellPx));
					const size_t cIdx = static_cast<size_t>(mcy) * cw + mcx;
					const double sxBase = lx - static_cast<double>(ar.x0);
					const double syBase = ly - static_cast<double>(ar.y0);

					double vx = 0.0, vy = 0.0;
					if (useMotion)
					{
						if (lumaMotion->valid[cIdx] == 0u)
						{
							const int acx0 = (std::max)(0, (std::min)(cellsW_ - 1, static_cast<int>(std::floor(lx - originOffX_)) / cellPx_));
							const int acy0 = (std::max)(0, (std::min)(cellsH_ - 1, static_cast<int>(std::floor(ly - originOffY_)) / cellPx_));
							fillCflHole(cx, cy, sxBase, syBase, &cflCells_[static_cast<size_t>(acy0) * cellsW_ + acx0]);
							continue;
						}
						vx = lumaMotion->vx[cIdx];
						vy = lumaMotion->vy[cIdx];
						if (!std::isfinite(vx) || !std::isfinite(vy))
						{
							anyNonFinite = true;
							const int acx0 = (std::max)(0, (std::min)(cellsW_ - 1, static_cast<int>(std::floor(lx - originOffX_)) / cellPx_));
							const int acy0 = (std::max)(0, (std::min)(cellsH_ - 1, static_cast<int>(std::floor(ly - originOffY_)) / cellPx_));
							fillCflHole(cx, cy, sxBase, syBase, &cflCells_[static_cast<size_t>(acy0) * cellsW_ + acx0]);
							continue;
						}
					}

					const double ax = lx + vx;
					const double ay = ly + vy;
					const double sxLocal = ax - static_cast<double>(ar.x0);
					const double syLocal = ay - static_cast<double>(ar.y0);

					// Carry (a, b) from the anchor cell at siting-aligned (ax - originOffX_, ay - originOffY_).
					const int acx = (std::max)(0, (std::min)(cellsW_ - 1, static_cast<int>(std::floor(ax - originOffX_)) / cellPx_));
					const int acy = (std::max)(0, (std::min)(cellsH_ - 1, static_cast<int>(std::floor(ay - originOffY_)) / cellPx_));
					const AnchorCflCell& cfl = cflCells_[static_cast<size_t>(acy) * cellsW_ + acx];
					if (!cfl.valid)
					{
						fillCflHole(cx, cy, sxLocal, syLocal, nullptr);
						continue;
					}

					// Allow exact <= 0.5 px siting margin on subsampled axes at active edges (clamped strictly inside ar,
					// never crossing bar boundaries) so odd-dimension last coded chroma row/col is preserved.
					const double maxLocalX = static_cast<double>(ar.Width() - 1);
					const double maxLocalY = static_cast<double>(ar.Height() - 1);
					const double marginX = (cSubX > 1) ? 0.5 : 0.0;
					const double marginY = (cSubY > 1) ? 0.5 : 0.0;

					double pyVal = 0.0;
					bool borderBilin = false;
					bool sampledOk = ChromaDetail::SampleSitedActive(yView, sxLocal, syLocal, pyVal, borderBilin, /*allowEdgeClampOutside=*/false);
					double checkSx = sxLocal;
					double checkSy = syLocal;
					if (!sampledOk &&
					    sxLocal >= -marginX && sxLocal <= maxLocalX + marginX &&
					    syLocal >= -marginY && syLocal <= maxLocalY + marginY)
					{
						checkSx = (std::max)(0.0, (std::min)(maxLocalX, sxLocal));
						checkSy = (std::max)(0.0, (std::min)(maxLocalY, syLocal));
						sampledOk = ChromaDetail::SampleSitedActive(yView, checkSx, checkSy, pyVal, borderBilin, /*allowEdgeClampOutside=*/true);
					}
					if (sampledOk && anchorYFootprintValid(checkSx, checkSy))
					{
						const double predCb = cfl.aCb * pyVal + cfl.bCb;
						const double predCr = cfl.aCr * pyVal + cfl.bCr;
						if (std::isfinite(predCb) && std::isfinite(predCr))
						{
							outCb.at(cx, cy) = predCb;
							outCr.at(cx, cy) = predCr;
							outValid[static_cast<size_t>(cy) * cW + cx] = 1u;
							if (borderBilin)
								anyBorder = true;
						}
						else
						{
							anyNonFinite = true;
							fillCflHole(cx, cy, sxLocal, syLocal, &cfl);
						}
					}
					else
					{
						fillCflHole(cx, cy, sxLocal, syLocal, &cfl);
					}
				}
			}

			if (anyBorder && standIns != nullptr)
				standIns->Engage(StandInKind::BorderBilinearFallback, "PredictCarriedCflAtCodedGrid luma border");
			if (anyNonFinite && standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "PredictCarriedCflAtCodedGrid non-finite");
			return true;
		}

	private:
		struct CflAccum
		{
			double sumY = 0.0;
			double sumYY = 0.0;
			double sumCb = 0.0;
			double sumYCb = 0.0;
			double sumCr = 0.0;
			double sumYCr = 0.0;
			size_t count = 0;
		};

		int w_ = 0;
		int h_ = 0;
		int cellsW_ = 0;
		int cellsH_ = 0;
		int cellPx_ = kCanonicalBlockPx;
		int codedSubX_ = 2;
		int codedSubY_ = 2;
		ActiveRect active_{};
		double originOffX_ = 0.0;
		double originOffY_ = 0.0;
		TdV2Plane sumCb_;
		TdV2Plane sumCr_;
		TdV2Plane weight_;
		TdV2Plane anchorCleanY_;
		std::vector<uint8_t> anchorCleanYValid_;
		std::vector<CflAccum> cflStats_;
		std::vector<AnchorCflCell> cflCells_;
		size_t accumulatedFrames_ = 0;
		uint64_t lastAccumSeq_ = 0;
		bool hasLastAccumSeq_ = false;
		bool hasData_ = false;
		bool hasCfl_ = false;
	};

	// ---- Chroma Reference, Process Input & Process Result ----

	struct ChromaReferenceInput
	{
		const TdV2Plane* cleanCb = nullptr;           // Grain-free Cb (coded grid or delivered grid)
		const TdV2Plane* cleanCr = nullptr;           // Grain-free Cr (coded grid or delivered grid)
		const CellMotionField* lumaMotion = nullptr;  // Luma-grid Current -> Reference motion field
		bool isExactCopyOfCurrent = false;
	};

	struct ChromaProcessInput
	{
		uint64_t seq = 0;
		DeliveredChromaFormat deliveredFormat = DeliveredChromaFormat::Yuv420;
		ChromaSubsampling codedSubsampling = ChromaSubsampling::Yuv420;
		ChromaGridStatus gridStatus = ChromaGridStatus::Identified;
		SitingSpec siting = SitingSpec::Left();
		YCbCrMatrixSpec matrix = YCbCrMatrixSpec::Bt709();

		int lumaW = 0;
		int lumaH = 0;
		ActiveRect activeRect;                        // Luma-resolution ActiveRect (empty => full lumaW x lumaH)

		// Delivered planes:
		// - Yuv420 / Yuv422 / Yuv444: populate receivedCb, receivedCr (receivedY optional).
		// - Rgb444: populate receivedR, receivedG, receivedB (lumaW x lumaH).
		TdV2Plane receivedY;
		TdV2Plane receivedCb;
		TdV2Plane receivedCr;
		TdV2Plane receivedR;
		TdV2Plane receivedG;
		TdV2Plane receivedB;

		// Optional Layer-1 grain-free chroma split (delivered grid or coded grid).
		bool hasCleanSplit = false;
		TdV2Plane cleanCb;
		TdV2Plane cleanCr;
		std::vector<double> retainedGrainVarCb;       // Per-cell mean(f'^2) * N_b for Cb
		std::vector<double> retainedGrainVarCr;       // Per-cell mean(f'^2) * N_b for Cr

		// Anchor frame ingestion into 4:4:4 canvas + carried CfL fitting (grain-free only!).
		bool isAnchorFrame = false;
		bool resetAnchorCfl = false;                  // Explicitly refit per-anchor CfL (a, b) & P_Y on new anchor
		TdV2Plane anchorCleanY;                       // Grain-free luma of anchor frame for CfL fitting & H5 P_Y
		CellMotionField anchorSplatMotion;            // Frame displacement from canvas origin (empty => 0)
		bool hasAnchorSplatMotion = false;

		// Temporal references:
		ChromaReferenceInput prevPeak;
		ChromaReferenceInput nextPeak;
		CellMotionField anchorReadMotion;             // Luma-grid Current -> Anchor canvas motion field
		bool hasAnchorReadMotion = false;
		bool enableAnchorCanvasRef = true;
		bool enableCarriedCfl = true;

		// Optional explicit CodecModel prediction P_B for transform-identified STRICT/CELL POCS (coded or delivered grid).
		// When null, refs[0].clean is used as a visible stand-in (StandInKind::CodecModelPbStandIn, plan s1.2 L95).
		const TdV2Plane* codecModelPbCb = nullptr;
		const TdV2Plane* codecModelPbCr = nullptr;

		// Coding step prior (P16): when false, derives 1-LSB default with visible StandInKind::StepPriorDefault.
		bool hasStepPrior = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool transformIdentified8x8 = false;

		bool isCutOrSpanInvalid = false;              // Resets 4:4:4 anchor canvas cleanly

		// Optional Pass-1 face / non-rigid cell mask (cellsW * cellsH luma-cell layout matching FrameProcessInput::faceMask;
		// non-zero forces delta == 0 on all masked pixels and excludes masked cells from 3x3 covariance pooling; H13).
		const std::vector<uint8_t>* faceMask = nullptr;

		// Optional luma-parity rack-focus & matched-focus optical bokeh cell masks (cellsW * cellsH, LOW-3):
		// - explicitFocusChangeCells != 0: hard-vetoes ALL coded chroma bands (L, M, H -> delta == 0).
		// - explicitDefocusedCells != 0: hard-vetoes coded chroma M & H always; L band allowed ONLY under BokehMode::LowBandCellOnly.
		const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
		const std::vector<uint8_t>* explicitDefocusedCells = nullptr;
	};

	struct ChromaProcessResult
	{
		// Final delivered outputs (bit-exact passthrough in Off/Shadow modes and outside ActiveRect).
		TdV2Plane outputCb;
		TdV2Plane outputCr;
		TdV2Plane outputR;
		TdV2Plane outputG;
		TdV2Plane outputB;

		// Signed deltas on coded grid and delivered grid (populated in Shadow & On modes).
		TdV2Plane codedDeltaCb;
		TdV2Plane codedDeltaCr;
		TdV2Plane deliveredDeltaCb;
		TdV2Plane deliveredDeltaCr;

		// Visible diagnostics & Rule-4 counters.
		int cellsW = 0;
		int cellsH = 0;
		size_t activeRefCount = 0;
		size_t guardViolationCells = 0;
		size_t cflFallbackCells = 0;
		size_t faceMaskedCells = 0;
		size_t focusVetoCells = 0;
		size_t nonFiniteCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t a2InvalidCells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
		uint64_t canvasGapFilledPixels = 0;
		uint64_t codecModelPbStandInCells = 0;
		uint64_t grainPriorOobCells = 0;
		uint64_t excludedZeroEnergyBlocks = 0;
		double estimatedBlindNlfCb = 0.0;
		double estimatedBlindNlfCr = 0.0;
		std::vector<uint8_t> cflFallbackMask;
		std::vector<AnchorCflCell> singleFrameCflFallback;
		bool coarseBandOnly = false;
		bool anchorReset = false;
		bool anchorCanvasUsed = false;
		bool carriedCflUsed = false;
	};

	// ---- Stage TD-S4 Coded-Grid Chroma Recovery Engine ----
	class TdV2ChromaEngine
	{
	public:
		explicit TdV2ChromaEngine(const Config& cfg = Config{})
			: cfg_(cfg)
		{
		}

		void SetConfig(const Config& cfg)
		{
			cfg_ = cfg;
			if (!cfg_.IsEnabled())
				ResetAnchor();
		}
		const Config& GetConfig() const { return cfg_; }

		void ResetAnchor() { anchorCanvas_.Reset(); }
		TdV2ChromaAnchorCanvas& AnchorCanvas() { return anchorCanvas_; }
		const TdV2ChromaAnchorCanvas& AnchorCanvas() const { return anchorCanvas_; }

		ChromaProcessResult ProcessFrame(const ChromaProcessInput& in, StandInRegistry* standIns = nullptr)
		{
			ChromaProcessResult res;
			const bool isRgb = (in.deliveredFormat == DeliveredChromaFormat::Rgb444);

			// Initialize passthrough outputs immediately so every early return / Off / Shadow / outside ActiveRect is bit-exact!
			if (isRgb)
			{
				res.outputR = in.receivedR;
				res.outputG = in.receivedG;
				res.outputB = in.receivedB;
			}
			else
			{
				res.outputCb = in.receivedCb;
				res.outputCr = in.receivedCr;
			}

			// Infer luma dimensions if caller omitted lumaW/lumaH.
			int lumaW = in.lumaW;
			int lumaH = in.lumaH;
			const int dSubX = DeliveredSubsampleX(in.deliveredFormat);
			const int dSubY = DeliveredSubsampleY(in.deliveredFormat);
			if (lumaW <= 0 || lumaH <= 0)
			{
				if (isRgb && !in.receivedR.Empty())
				{
					lumaW = in.receivedR.w;
					lumaH = in.receivedR.h;
				}
				else if (!in.receivedY.Empty())
				{
					lumaW = in.receivedY.w;
					lumaH = in.receivedY.h;
				}
				else if (!in.receivedCb.Empty())
				{
					lumaW = in.receivedCb.w * dSubX;
					lumaH = in.receivedCb.h * dSubY;
				}
			}
			if (lumaW <= 0 || lumaH <= 0)
				return res;

			const int dW = ChromaDetail::SubsampledDim(lumaW, dSubX);
			const int dH = ChromaDetail::SubsampledDim(lumaH, dSubY);
			if (isRgb)
			{
				if (in.receivedR.Empty() || in.receivedG.Empty() || in.receivedB.Empty() ||
				    in.receivedR.w != lumaW || in.receivedR.h != lumaH ||
				    in.receivedG.w != lumaW || in.receivedG.h != lumaH ||
				    in.receivedB.w != lumaW || in.receivedB.h != lumaH)
				{
					return res;
				}
			}
			else
			{
				if (in.receivedCb.Empty() || in.receivedCr.Empty() ||
				    in.receivedCb.w != dW || in.receivedCb.h != dH ||
				    in.receivedCr.w != dW || in.receivedCr.h != dH)
				{
					return res;
				}
			}

			const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
			const int grainCellPx = cfg_.grainCellPx > 0 ? cfg_.grainCellPx : cellPx;
			res.cellsW = (lumaW + cellPx - 1) / cellPx;
			res.cellsH = (lumaH + cellPx - 1) / cellPx;
			const size_t numCells = static_cast<size_t>(res.cellsW) * static_cast<size_t>(res.cellsH);

			// Mode::Off -> reset any stale anchor canvas so Off->On never blends stale history, then bit-exact passthrough!
			if (!cfg_.IsEnabled())
			{
				if (anchorCanvas_.HasData())
				{
					ResetAnchor();
					res.anchorReset = true;
				}
				return res;
			}

			// Upfront sourceBits validation (Rule 4).
			const double peakCode = PeakCodeForBits(cfg_.sourceBits);
			if (!(peakCode > 0.0))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine invalid sourceBits");
				ResetAnchor();
				res.anchorReset = true;
				res.nonFiniteCells = numCells;
				return res;
			}

			const ActiveRect lumaActive = in.activeRect.Empty()
				? ActiveRect::Full(lumaW, lumaH)
				: in.activeRect.ClampedTo(lumaW, lumaH);

			// Cut / invalid span, resolution change, or ActiveRect change (e.g., aspect-ratio switch) resets 4:4:4 anchor canvas cleanly.
			const ActiveRect& prevActive = anchorCanvas_.Active();
			const bool activeChanged = (prevActive.x0 != lumaActive.x0 || prevActive.y0 != lumaActive.y0 ||
			                            prevActive.x1 != lumaActive.x1 || prevActive.y1 != lumaActive.y1);
			if (in.isCutOrSpanInvalid ||
			    (anchorCanvas_.HasData() && (anchorCanvas_.Width() != lumaW || anchorCanvas_.Height() != lumaH || activeChanged)))
			{
				ResetAnchor();
				res.anchorReset = true;
			}

			// Validate all delivered pixels are finite (Rule 4).
			auto planeAllFinite = [](const TdV2Plane& p) -> bool
			{
				for (double v : p.v)
					if (!std::isfinite(v)) return false;
				return true;
			};
			if (isRgb)
			{
				if (!planeAllFinite(in.receivedR) || !planeAllFinite(in.receivedG) || !planeAllFinite(in.receivedB))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine non-finite delivered RGB");
					ResetAnchor();
					res.anchorReset = true;
					res.nonFiniteCells = numCells;
					return res;
				}
			}
			else
			{
				if (!planeAllFinite(in.receivedCb) || !planeAllFinite(in.receivedCr))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine non-finite delivered Cb/Cr");
					ResetAnchor();
					res.anchorReset = true;
					res.nonFiniteCells = numCells;
					return res;
				}
			}

			if (lumaActive.Empty())
				return res;

			const ActiveRect delivActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, dSubX, dSubY, dW, dH);
			if (delivActive.Empty())
				return res;

			// Extract delivered Cb/Cr (for RGB, convert via U26 LumaCoeff-backed matrix).
			const double midCode = std::ldexp(1.0, cfg_.sourceBits - 1);
			TdV2Plane delivCb(dW, dH, 0.0);
			TdV2Plane delivCr(dW, dH, 0.0);
			TdV2Plane rgbDerivedY;
			if (isRgb)
			{
				rgbDerivedY = TdV2Plane(lumaW, lumaH, 0.0);
				for (int y = 0; y < lumaH; ++y)
				{
					for (int x = 0; x < lumaW; ++x)
					{
						double yv = 0.0, cbv = 0.0, crv = 0.0;
						in.matrix.RgbToYCbCr(in.receivedR.at(x, y), in.receivedG.at(x, y), in.receivedB.at(x, y),
						                     midCode, yv, cbv, crv);
						rgbDerivedY.at(x, y) = yv;
						delivCb.at(x, y) = cbv;
						delivCr.at(x, y) = crv;
					}
				}
			}
			else
			{
				delivCb = in.receivedCb;
				delivCr = in.receivedCr;
			}

			// Coding step prior (1:1 parity with TdV2ReferenceEngine.h):
			// - When hasStepPrior == false: true 1-LSB uniform output rounding (delta = 1.0 code, phi = 0.5, pi0 = 0),
			//   visibly tagged via StandInKind::StepPriorDefault.
			// - When hasStepPrior == true: validate finite positive stepDelta, deadZonePhi strictly in (0, 1),
			//   and zeroCoeffFraction in [0, 1]; invalid A2 stats fail closed visibly (delta == 0, StandInKind::A2StatsInvalid).
			double effStepDelta = in.estimatedStepDelta;
			double effPhi = in.deadZonePhi;
			double effZeroFrac = in.zeroCoeffFraction;
			bool a2InputNonFinite = !std::isfinite(effPhi) || !std::isfinite(effZeroFrac);
			if (!in.hasStepPrior)
			{
				if (!a2InputNonFinite)
				{
					effStepDelta = kDefaultStepPriorLsb; // True 1-LSB quantization step in native integer code units (1.0)
					effPhi = kUniformRoundingPhi;        // Unbiased uniform output rounding (phi = 0.5 -> V1 = 1/12)
					effZeroFrac = 0.0;                   // Uniform rounding cell without invented pi0
					if (standIns != nullptr)
						standIns->Engage(StandInKind::StepPriorDefault);
				}
				else
				{
					effStepDelta = -1.0;
				}
			}
			else
			{
				if (!std::isfinite(effStepDelta))
					a2InputNonFinite = true;
				if (a2InputNonFinite || effStepDelta <= 0.0 || effPhi <= 0.0 || effPhi >= 1.0 ||
				    effZeroFrac < 0.0 || effZeroFrac > 1.0)
				{
					effStepDelta = -1.0;
				}
			}
			const RoundingCellStats a2Stats = ComputeRoundingCellStats(effStepDelta, effPhi, effZeroFrac);
			if (!a2Stats.valid)
			{
				if (standIns != nullptr)
				{
					if (a2InputNonFinite)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine non-finite A2 step/phi stats");
					standIns->Engage(StandInKind::A2StatsInvalid, "invalid stepDelta/deadZonePhi/zeroFrac; delta closed to 0");
				}
				res.a2InvalidCells = numCells;
				if (a2InputNonFinite)
					res.nonFiniteCells = numCells;
				return res;
			}

			// Pass-1 face / non-rigid cell mask (H13 parity with TdV2ReferenceEngine.h): count unique masked cells once upfront.
			// If non-empty faceMask size mismatches numCells, engage FaceMaskSizeMismatch visibly (and mask trailing cells fail-closed when undersized).
			const bool faceMaskSizeMismatch = (in.faceMask != nullptr && !in.faceMask->empty() && in.faceMask->size() != numCells);
			if (faceMaskSizeMismatch && standIns != nullptr)
			{
				standIns->Engage(
					StandInKind::FaceMaskSizeMismatch,
					in.faceMask->size() < numCells
						? "TdV2ChromaEngine faceMask undersized vs cell grid; masking trailing cells fail-closed"
						: "TdV2ChromaEngine faceMask oversized vs cell grid");
			}
			auto isFaceMaskedCell = [&](size_t cellIdx) -> bool
			{
				if (in.faceMask == nullptr || in.faceMask->empty())
					return false;
				if (cellIdx >= in.faceMask->size())
					return true;
				return (*in.faceMask)[cellIdx] != 0u;
			};
			for (size_t ci = 0; ci < numCells; ++ci)
			{
				if (isFaceMaskedCell(ci))
					++res.faceMaskedCells;
			}
			if (res.faceMaskedCells > 0 && standIns != nullptr)
				standIns->Engage(StandInKind::FaceNonRigidMask);

			// Rack-focus & optical bokeh hard-veto parity with luma TdV2ReferenceEngine (LOW-3 & Owner Q4):
			// - explicitFocusChangeCells != 0: vetoes ALL bands (L, M, H -> delta == 0).
			// - explicitDefocusedCells != 0: vetoes M & H always; L band allowed ONLY under BokehMode::LowBandCellOnly.
			auto isFocusChangeCell = [&](size_t cellIdx) -> bool
			{
				return in.explicitFocusChangeCells != nullptr &&
				       cellIdx < in.explicitFocusChangeCells->size() &&
				       (*in.explicitFocusChangeCells)[cellIdx] != 0u;
			};
			auto isDefocusedCell = [&](size_t cellIdx) -> bool
			{
				return in.explicitDefocusedCells != nullptr &&
				       cellIdx < in.explicitDefocusedCells->size() &&
				       (*in.explicitDefocusedCells)[cellIdx] != 0u;
			};
			auto isFocusVetoedBand = [&](size_t cellIdx, int bandIdx) -> bool
			{
				if (isFocusChangeCell(cellIdx))
					return true;
				if (isDefocusedCell(cellIdx))
				{
					if (bandIdx >= 1)
						return true;
					return cfg_.bokehMode != BokehMode::LowBandCellOnly;
				}
				return false;
			};
			for (size_t ci = 0; ci < numCells; ++ci)
			{
				if (isFocusChangeCell(ci) || isDefocusedCell(ci))
					++res.focusVetoCells;
			}

			// =========================================================================
			// Branch A: Unidentified coded grid -> Visible coarse-band-only chroma mode
			// =========================================================================
			if (in.gridStatus == ChromaGridStatus::Unidentified)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::ChromaGridDetector, "chroma: coarse band only (coded chroma grid not identified)");
				res.coarseBandOnly = true;
				res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
				res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);
				res.codedDeltaCb = res.deliveredDeltaCb;
				res.codedDeltaCr = res.deliveredDeltaCr;

				if (in.isCutOrSpanInvalid)
					return res;

				// Separate delivered clean vs own grain if supplied (own grain NEVER enters coarse blend).
				TdV2Plane curCleanCb = delivCb;
				TdV2Plane curCleanCr = delivCr;
				TdV2Plane ownGrainCb(dW, dH, 0.0);
				TdV2Plane ownGrainCr(dW, dH, 0.0);
				bool splitActive = false;
				if (in.hasCleanSplit && !in.cleanCb.Empty() && !in.cleanCr.Empty())
				{
					if (in.cleanCb.w != dW || in.cleanCb.h != dH || in.cleanCr.w != dW || in.cleanCr.h != dH)
					{
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "chroma coarse cleanCb/Cr dimension mismatch");
						res.nonFiniteCells = numCells;
						return res;
					}
					if (!planeAllFinite(in.cleanCb) || !planeAllFinite(in.cleanCr))
					{
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine non-finite cleanCb/Cr");
						res.nonFiniteCells = numCells;
						return res;
					}
					curCleanCb = in.cleanCb;
					curCleanCr = in.cleanCr;
					for (size_t i = 0; i < delivCb.v.size(); ++i)
					{
						ownGrainCb.v[i] = delivCb.v[i] - curCleanCb.v[i];
						ownGrainCr.v[i] = delivCr.v[i] - curCleanCr.v[i];
					}
					splitActive = true;
				}
				else
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::P10gGrainSeparator,
							in.hasCleanSplit
								? "chroma coarse: hasCleanSplit set with empty clean planes; treating as unsplit"
								: "chroma coarse: no upstream chroma split");
				}

				const double sigmaX = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubX);
				const double sigmaY = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubY);
				const auto gxCoarse = VpUnit::BandFromBase::Taps<double>(sigmaX, 3.0, true, 0.0);
				const auto gyCoarse = VpUnit::BandFromBase::Taps<double>(sigmaY, 3.0, true, 0.0);
				double coarseGain = 1.0;
				const TdV2Plane curCoarseCb = ChromaDetail::BlurActiveAnisotropic(curCleanCb, delivActive, sigmaX, sigmaY, coarseGain);
				const TdV2Plane curCoarseCr = ChromaDetail::BlurActiveAnisotropic(curCleanCr, delivActive, sigmaX, sigmaY, coarseGain);

				struct CoarseRef
				{
					TdV2Plane warpedCb;
					TdV2Plane warpedCr;
					TdV2Plane coarseCb;
					TdV2Plane coarseCr;
					std::vector<uint8_t> validPx;
					std::vector<double> motionVx;
					std::vector<double> motionVy;
					MotionGateResult motionGate;
					bool isExactCopy = false;
				};
				std::vector<CoarseRef> cRefs;
				cRefs.reserve(2);
				std::vector<uint8_t> nonFiniteCellFlag(numCells, 0u);
				std::vector<uint8_t> singleRefA2CellFlag(numCells, 0u);
				std::vector<uint8_t> singularCellFlag(numCells, 0u);

				auto markNonFiniteRefCells = [&](const TdV2Plane& refP)
				{
					for (int y = delivActive.y0; y < delivActive.y1; ++y)
					{
						const double ly = in.siting.CodedToLumaY(static_cast<double>(y), dSubY);
						const int cy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
						for (int x = delivActive.x0; x < delivActive.x1; ++x)
						{
							if (!std::isfinite(refP.at(x, y)))
							{
								const double lx = in.siting.CodedToLumaX(static_cast<double>(x), dSubX);
								const int cx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
								nonFiniteCellFlag[static_cast<size_t>(cy) * res.cellsW + cx] = 1u;
							}
						}
					}
				};

				auto tryAddCoarseRef = [&](const ChromaReferenceInput& rIn)
				{
					if (rIn.cleanCb == nullptr || rIn.cleanCr == nullptr ||
					    rIn.cleanCb->w != dW || rIn.cleanCb->h != dH ||
					    rIn.cleanCr->w != dW || rIn.cleanCr->h != dH)
						return;

					const CellMotionField* useMf = rIn.lumaMotion;
					if (useMf == nullptr)
					{
						// H9 (plan s1.2 L108): missing motion field MUST NEVER whole-frame blend as "Still".
						++res.motionStandInRefs;
						if (standIns != nullptr)
						{
							standIns->Engage(StandInKind::MotionFieldStandIn, "chroma coarse null motion field; ref skipped per L4");
							standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "L4 missing motion field: chroma coarse ref skipped (no whole-frame Still blend)");
						}
						return;
					}
					if (!useMf->IsWellFormed(res.cellsW, res.cellsH))
					{
						++res.motionStandInRefs;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::MotionFieldStandIn, "chroma coarse mismatched motion; ref rejected");
						return;
					}

					CoarseRef cr;
					cr.isExactCopy = rIn.isExactCopyOfCurrent;
					cr.motionVx = useMf->vx;
					cr.motionVy = useMf->vy;
					std::vector<uint8_t> validCb, validCr;
					const WarpDiagnostics dCb = ChromaDetail::WarpChromaPlaneWithLumaMotion(
						*rIn.cleanCb, curCleanCb, delivActive, dSubX, dSubY, in.siting,
						useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, cr.warpedCb, validCb, standIns, useMf);
					const WarpDiagnostics dCr = ChromaDetail::WarpChromaPlaneWithLumaMotion(
						*rIn.cleanCr, curCleanCr, delivActive, dSubX, dSubY, in.siting,
						useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, cr.warpedCr, validCr, standIns, useMf);
					res.borderBilinearPixels += dCb.borderBilinearPixels + dCr.borderBilinearPixels;
					if (dCb.nonFinitePixels > 0)
						markNonFiniteRefCells(*rIn.cleanCb);
					if (dCr.nonFinitePixels > 0)
						markNonFiniteRefCells(*rIn.cleanCr);
					cr.validPx.resize(validCb.size(), 0u);
					for (size_t i = 0; i < validCb.size(); ++i)
						cr.validPx[i] = (validCb[i] != 0u && validCr[i] != 0u) ? 1u : 0u;
					double dummyGain = 1.0;
					cr.coarseCb = ChromaDetail::BlurActiveAnisotropic(cr.warpedCb, delivActive, sigmaX, sigmaY, dummyGain);
					cr.coarseCr = ChromaDetail::BlurActiveAnisotropic(cr.warpedCr, delivActive, sigmaX, sigmaY, dummyGain);
					cr.motionGate = EvaluateCameraRelativeMotionGate(*useMf, standIns);
					if (cr.motionGate.nonFiniteCells > 0)
					{
						for (size_t ci = 0; ci < numCells && ci < cr.motionGate.nonFiniteCellMask.size(); ++ci)
							if (cr.motionGate.nonFiniteCellMask[ci] != 0u)
								nonFiniteCellFlag[ci] = 1u;
					}
					cRefs.push_back(std::move(cr));
				};

				tryAddCoarseRef(in.prevPeak);
				tryAddCoarseRef(in.nextPeak);
				res.activeRefCount = cRefs.size();
				if (cRefs.empty())
					return res;

				auto fuseCoarseSinglePlane = [&](
					const TdV2Plane& delivRec,
					const TdV2Plane& curClean,
					const TdV2Plane& curCoarse,
					bool isCb,
					const std::vector<double>& retainedGrainVar,
					TdV2Plane& outDelta)
				{
					const int K = static_cast<int>(cRefs.size());
					std::array<std::vector<double>, kMaxBlendRefs> cellW;
					std::array<std::vector<double>, kMaxBlendRefs> cellDiagC;
					for (int k = 0; k < K; ++k)
					{
						cellW[k].assign(numCells, 0.0);
						cellDiagC[k].assign(numCells, 0.0);
					}
					const int poolR = cfg_.momentPoolRadiusCells > 0 ? cfg_.momentPoolRadiusCells : 0;
					const int gW = (lumaW + grainCellPx - 1) / grainCellPx;
					std::vector<double> blindCellEnergies;
					std::vector<double> blindCellIrs;

					for (int cy = 0; cy < res.cellsH; ++cy)
					{
						for (int cx = 0; cx < res.cellsW; ++cx)
						{
							const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
							// Pass-1 face / non-rigid mask (H13) and rack-focus/bokeh L-band veto (LOW-3): skip cell solve so cellW[k][cIdx] stays 0.0.
							if (isFaceMaskedCell(cIdx) || isFocusVetoedBand(cIdx, 0))
								continue;

							double cellGrainVar = 0.0;
							bool hasCellGrainPrior = false;
							if (!retainedGrainVar.empty())
							{
								const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
								const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
								const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
								if (gIdx < retainedGrainVar.size())
								{
									const double gv = retainedGrainVar[gIdx];
									if (std::isfinite(gv) && gv >= 0.0)
									{
										cellGrainVar = gv;
										hasCellGrainPrior = true;
									}
									else
									{
										nonFiniteCellFlag[cIdx] = 1u;
										if (standIns != nullptr)
											standIns->Engage(StandInKind::NonFiniteClosure, "chroma coarse non-finite retainedGrainVar");
									}
								}
								else
								{
									++res.grainPriorOobCells;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::GrainPriorOutOfRange, "chroma coarse retainedGrainVar index out of range");
								}
							}

							MomentAccumulatorFp32 acc(K);
							double ownSumSq[kMaxBlendRefs] = {};
							size_t ownSampleCount = 0;
							const int py0 = (std::max)(delivActive.y0, ((cy - poolR) * cellPx) / dSubY);
							const int py1 = (std::min)(delivActive.y1, ((cy + poolR + 1) * cellPx + dSubY - 1) / dSubY);
							const int px0 = (std::max)(delivActive.x0, ((cx - poolR) * cellPx) / dSubX);
							const int px1 = (std::min)(delivActive.x1, ((cx + poolR + 1) * cellPx + dSubX - 1) / dSubX);

							double sumCurHi2 = 0.0;
							double ownCurHi2 = 0.0;
							size_t ownHiCount = 0;
							double sumRefHi2[kMaxBlendRefs] = {};
							double sumCrossHi[kMaxBlendRefs] = {};
							size_t hiCount = 0;

							for (int y = py0; y < py1; ++y)
							{
								const double ly = in.siting.CodedToLumaY(static_cast<double>(y), dSubY);
								const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
								for (int x = px0; x < px1; ++x)
								{
									const double lx = in.siting.CodedToLumaX(static_cast<double>(x), dSubX);
									const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
									const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
									if (isFaceMaskedCell(pCell) || isFocusVetoedBand(pCell, 0))
										continue;
									const size_t pIdx = static_cast<size_t>(y) * dW + x;

									bool allValid = true;
									float diffs[kMaxBlendRefs] = {};
									const double curV = curCoarse.at(x, y);
									const double cHi = (!splitActive && !hasCellGrainPrior)
										? (curClean.at(x, y) - curV)
										: 0.0;
									if (!splitActive && !hasCellGrainPrior && pCell == cIdx)
									{
										ownCurHi2 += cHi * cHi;
										++ownHiCount;
									}
									for (int k = 0; k < K; ++k)
									{
										if (cRefs[k].validPx[pIdx] == 0u || cRefs[k].motionGate.gateL[pCell] <= 0.0)
										{
											allValid = false;
											break;
										}
										const TdV2Plane& refP = isCb ? cRefs[k].coarseCb : cRefs[k].coarseCr;
										diffs[k] = static_cast<float>(curV - refP.at(x, y));
									}
									if (allValid)
									{
										acc.AddSample(diffs);
										if (pCell == cIdx)
										{
											++ownSampleCount;
											for (int k = 0; k < K; ++k)
												ownSumSq[k] += static_cast<double>(diffs[k]) * static_cast<double>(diffs[k]);
										}
										if (!splitActive && !hasCellGrainPrior)
										{
											sumCurHi2 += cHi * cHi;
											for (int k = 0; k < K; ++k)
											{
												const TdV2Plane& rRaw = isCb ? cRefs[k].warpedCb : cRefs[k].warpedCr;
												const TdV2Plane& rCoarse = isCb ? cRefs[k].coarseCb : cRefs[k].coarseCr;
												const double rHi = rRaw.at(x, y) - rCoarse.at(x, y);
												sumRefHi2[k] += rHi * rHi;
												sumCrossHi[k] += cHi * rHi;
											}
											++hiCount;
										}
									}
								}
							}

							if (acc.sampleCount == 0)
								continue;

							if (!splitActive && !hasCellGrainPrior && hiCount > 0)
							{
								const double invHi = 1.0 / static_cast<double>(hiCount);
								const double meanCur = sumCurHi2 * invHi;
								const size_t rxCoarse = gxCoarse.size() >> 1u;
								const size_t ryCoarse = gyCoarse.size() >> 1u;
								const double g0X = !gxCoarse.empty() ? gxCoarse[rxCoarse] : 0.0;
								const double g0Y = !gyCoarse.empty() ? gyCoarse[ryCoarse] : 0.0;
								// Exact Parseval white-noise variance gain ||I - G_x G_y||_2^2 (Rule 1).
								const double hiGain = (std::max)(kEngineNumFloor, 1.0 - 2.0 * g0X * g0Y + coarseGain);
								// Exact 1-LSB uniform quantization variance transferred through the high-pass band (Rule 1).
								const double refQuantFloor = ((kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0) * hiGain;
								double sumProjIr = 0.0;
								size_t validRefCnt = 0;
								for (int k = 0; k < K; ++k)
								{
									const double meanRef = sumRefHi2[k] * invHi;
									const double cross = sumCrossHi[k] * invHi;
									const double vxD = (cIdx < cRefs[k].motionVx.size())
										? (cRefs[k].motionVx[cIdx] / static_cast<double>(dSubX))
										: 0.0;
									const double vyD = (cIdx < cRefs[k].motionVy.size())
										? (cRefs[k].motionVy[cIdx] / static_cast<double>(dSubY))
										: 0.0;
									const double warpBeta = HighPassBicubicWarpVarianceGain(vxD, vyD, gxCoarse, gyCoarse);
									if (meanRef > refQuantFloor)
									{
										sumProjIr += ComputeProjectionIr(meanCur, cross, meanRef, standIns, warpBeta);
										++validRefCnt;
									}
								}
								cellGrainVar = (validRefCnt > 0) ? ((sumProjIr / static_cast<double>(validRefCnt)) / hiGain) : 0.0;
								const double ownE = (ownHiCount > 0)
									? (ownCurHi2 / static_cast<double>(ownHiCount))
									: meanCur;
								blindCellEnergies.push_back(ownE);
								blindCellIrs.push_back(cellGrainVar);
							}

							double C[kMaxBlendRefs][kMaxBlendRefs] = {};
							acc.SecondMoment(C);
							// Store unpooled own-cell second moment capped by pooled C[k][k] (1:1 parity with TdV2ReferenceEngine.h).
							for (int k = 0; k < K; ++k)
							{
								const double ownDiag = (ownSampleCount > 0)
									? (ownSumSq[k] / static_cast<double>(ownSampleCount))
									: C[k][k];
								cellDiagC[k][cIdx] = (std::min)(ownDiag, C[k][k]);
							}

							double cVec[kMaxBlendRefs] = {};
							for (int k = 0; k < K; ++k)
							{
								// Unit-sum low-pass DC transfer gain = (sum G_sigma)^2 = 1.0 (Rule 1),
								// capped to [0, C[k][k]] per Plan L81 (c_k <= E[(e_0 - e_k)^2] = C_kk).
								const double cA2 = (std::min)(EstimateC_A2(a2Stats, cRefs[k].isExactCopy), C[k][k]);
								double cA1 = cA2;
								if (K >= 2)
								{
									const int partner = (k == 0) ? 1 : 0;
									cA1 = EstimateC_A1(C[k][partner], C[k][k], C[partner][partner], cellGrainVar, coarseGain);
								}
								else if (cfg_.estimator != VarianceEstimator::A2_RoundingCells)
								{
									singleRefA2CellFlag[cIdx] = 1u;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
								}
								cVec[k] = SelectBlindC(cfg_.estimator, cA1, cA2);
								if (cRefs[k].isExactCopy)
									cVec[k] = 0.0;
							}

							const BlendSolveResult solved = SolveConvexWeights(C, cVec, K, kDefaultTraceRegScale, standIns);
							if (solved.nonFiniteClosed) { nonFiniteCellFlag[cIdx] = 1u; continue; }
							if (solved.singular) { singularCellFlag[cIdx] = 1u; continue; }

							for (int k = 0; k < K; ++k)
								cellW[k][cIdx] = solved.weights[k] * cRefs[k].motionGate.gateL[cIdx];
						}
					}

					// Q5(b) integration (LOW-3 parity with Branch B): exclude crushed-black zero-energy flat blocks
					// strictly by measured high-pass energy when aggregating blind unsplit NLF diagnostics.
					if (!splitActive && !blindCellEnergies.empty())
					{
						const size_t delivCellPxW = static_cast<size_t>((std::max)(1, cellPx / dSubX));
						const size_t delivCellPxH = static_cast<size_t>((std::max)(1, cellPx / dSubY));
						const DarkNlfResult nlfRes = AccumulateDarkNlfExcludingZeroEnergy(
							blindCellEnergies, blindCellIrs, delivCellPxW * delivCellPxH,
							1.0, 0.5, standIns);
						res.excludedZeroEnergyBlocks += nlfRes.excludedZeroEnergyBlocks;
						if (nlfRes.valid)
						{
							if (isCb) res.estimatedBlindNlfCb = nlfRes.nlfEstimate;
							else      res.estimatedBlindNlfCr = nlfRes.nlfEstimate;
						}
					}

					// Unit-sum DC transfer gain = 1.0 for coherent coarse-band quantization-cell bound (Rule 1).
					const double bandVar = a2Stats.valid ? a2Stats.totalVar : 0.0;
					for (int y = delivActive.y0; y < delivActive.y1; ++y)
					{
						const double ly = in.siting.CodedToLumaY(static_cast<double>(y), dSubY);
						const int cy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
						for (int x = delivActive.x0; x < delivActive.x1; ++x)
						{
							const double lx = in.siting.CodedToLumaX(static_cast<double>(x), dSubX);
							const int cx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
							const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
							const size_t pIdx = static_cast<size_t>(y) * dW + x;

							if (isFaceMaskedCell(cIdx) || isFocusVetoedBand(cIdx, 0))
							{
								outDelta.at(x, y) = 0.0;
								continue;
							}

							const double curV = curCoarse.at(x, y);
							double delta = 0.0;
							for (int k = 0; k < K; ++k)
							{
								if (cRefs[k].validPx[pIdx] == 0u || cellW[k][cIdx] <= 0.0)
									continue;
								const TdV2Plane& refP = isCb ? cRefs[k].coarseCb : cRefs[k].coarseCr;
								const double diff = curV - refP.at(x, y);
								const double wEff = cfg_.enablePerCornerHuber
									? SpatialBilinearHuberWeight(cellW[k].data(), cellDiagC[k].data(), res.cellsW, res.cellsH, lx, ly, cellPx, diff)
									: SpatialBilinearWeight(cellW[k].data(), res.cellsW, res.cellsH, lx, ly, cellPx);
								if (wEff <= 0.0)
									continue;
								delta -= wEff * diff;
							}
							if (std::fabs(delta) > 0.0)
								delta = ApplyTransformUnknownBound(delta, bandVar, standIns);
							outDelta.at(x, y) = (delta == 0.0) ? 0.0 : delta;
						}
					}

					// H2: When !splitActive, ALWAYS evaluate Layer-4 TdGuardPort on every cell with target = inFineE.
					if (!splitActive)
					{
						TdV2Plane candOut = delivRec;
						for (int y = delivActive.y0; y < delivActive.y1; ++y)
							for (int x = delivActive.x0; x < delivActive.x1; ++x)
								candOut.at(x, y) = delivRec.at(x, y) + outDelta.at(x, y);
						const ThreeBandPlanes inFullBands = DecomposeActiveBands(delivRec, delivActive, nullptr);
						const ThreeBandPlanes outFullBands = DecomposeActiveBands(candOut, delivActive, nullptr);
						for (int cy = 0; cy < res.cellsH; ++cy)
						{
							const int y0 = (std::max)(delivActive.y0, (cy * cellPx) / dSubY);
							const int y1 = (std::min)(delivActive.y1, ((cy + 1) * cellPx + dSubY - 1) / dSubY);
							if (y1 <= y0) continue;
							for (int cx = 0; cx < res.cellsW; ++cx)
							{
								const int x0 = (std::max)(delivActive.x0, (cx * cellPx) / dSubX);
								const int x1 = (std::min)(delivActive.x1, ((cx + 1) * cellPx + dSubX - 1) / dSubX);
								if (x1 <= x0) continue;
								if (isFaceMaskedCell(static_cast<size_t>(cy) * res.cellsW + cx))
									continue;
								double inFineE = 0.0, outFineE = 0.0;
								size_t cnt = 0;
								for (int y = y0; y < y1; ++y)
								{
									if ((std::min)(res.cellsH - 1, (y * dSubY) / cellPx) != cy)
										continue;
									for (int x = x0; x < x1; ++x)
									{
										if ((std::min)(res.cellsW - 1, (x * dSubX) / cellPx) != cx)
											continue;
										const double inf = inFullBands.H.at(x, y);
										const double outf = outFullBands.H.at(x, y);
										inFineE += inf * inf;
										outFineE += outf * outf;
										++cnt;
									}
								}
								if (cnt == 0) continue;
								const double invC = 1.0 / static_cast<double>(cnt);
								const auto gDec = TdGuardPort::Evaluate(inFineE * invC, outFineE * invC, standIns);
								if (gDec.violated && gDec.addBackFraction > 0.0)
								{
									++res.guardViolationCells;
									for (int y = y0; y < y1; ++y)
									{
										if ((std::min)(res.cellsH - 1, (y * dSubY) / cellPx) != cy)
											continue;
										for (int x = x0; x < x1; ++x)
										{
											if ((std::min)(res.cellsW - 1, (x * dSubX) / cellPx) != cx)
												continue;
											outDelta.at(x, y) += gDec.addBackFraction * (inFullBands.H.at(x, y) - outFullBands.H.at(x, y));
										}
									}
								}
							}
						}
					}
				};

				fuseCoarseSinglePlane(delivCb, curCleanCb, curCoarseCb, true, in.retainedGrainVarCb, res.deliveredDeltaCb);
				fuseCoarseSinglePlane(delivCr, curCleanCr, curCoarseCr, false, in.retainedGrainVarCr, res.deliveredDeltaCr);
				res.codedDeltaCb = res.deliveredDeltaCb;
				res.codedDeltaCr = res.deliveredDeltaCr;

				if (cfg_.ModifiesPixels())
					ApplyDeliveredDelta(in, delivActive, res.deliveredDeltaCb, res.deliveredDeltaCr, res, standIns,
					                    &nonFiniteCellFlag, res.cellsW, res.cellsH, cellPx);
				for (size_t ci = 0; ci < numCells; ++ci)
				{
					if (nonFiniteCellFlag[ci] != 0u) ++res.nonFiniteCells;
					if (singleRefA2CellFlag[ci] != 0u) ++res.singleRefA2Cells;
					if (singularCellFlag[ci] != 0u) ++res.singularCells;
				}
				return res;
			}

			// =========================================================================
			// Branch B: Identified coded grid -> Exact Coded-Grid Chroma Recovery
			// =========================================================================
			const int cSubX = SubsampleX(in.codedSubsampling);
			const int cSubY = SubsampleY(in.codedSubsampling);
			const int cW = ChromaDetail::SubsampledDim(lumaW, cSubX);
			const int cH = ChromaDetail::SubsampledDim(lumaH, cSubY);
			const ActiveRect codedActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, cSubX, cSubY, cW, cH);
			if (codedActive.Empty())
				return res;

			res.codedDeltaCb = TdV2Plane(cW, cH, 0.0);
			res.codedDeltaCr = TdV2Plane(cW, cH, 0.0);
			res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
			res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);

			// B1: Reduce delivered Cb/Cr to coded grid (exact identity when coded == delivered).
			const TdV2Plane codedRecCb = ReduceToCodedGrid(
				delivCb, lumaW, lumaH, in.deliveredFormat, in.codedSubsampling, in.siting, lumaActive, standIns);
			const TdV2Plane codedRecCr = ReduceToCodedGrid(
				delivCr, lumaW, lumaH, in.deliveredFormat, in.codedSubsampling, in.siting, lumaActive, standIns);

			// B2: Layer-1 own chroma grain separation strictly on the coded grid.
			TdV2Plane codedCleanCb(cW, cH, 0.0);
			TdV2Plane codedCleanCr(cW, cH, 0.0);
			TdV2Plane ownGrainCb(cW, cH, 0.0);
			TdV2Plane ownGrainCr(cW, cH, 0.0);
			bool splitActive = false;

			if (in.hasCleanSplit && !in.cleanCb.Empty() && !in.cleanCr.Empty())
			{
				if (!planeAllFinite(in.cleanCb) || !planeAllFinite(in.cleanCr))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine non-finite cleanCb/Cr");
					ResetAnchor();
					res.anchorReset = true;
					res.nonFiniteCells = numCells;
					return res;
				}
				if (in.cleanCb.w == cW && in.cleanCb.h == cH && in.cleanCr.w == cW && in.cleanCr.h == cH)
				{
					codedCleanCb = in.cleanCb;
					codedCleanCr = in.cleanCr;
				}
				else if (in.cleanCb.w == dW && in.cleanCb.h == dH && in.cleanCr.w == dW && in.cleanCr.h == dH)
				{
					codedCleanCb = ReduceToCodedGrid(
						in.cleanCb, lumaW, lumaH, in.deliveredFormat, in.codedSubsampling, in.siting, lumaActive, standIns);
					codedCleanCr = ReduceToCodedGrid(
						in.cleanCr, lumaW, lumaH, in.deliveredFormat, in.codedSubsampling, in.siting, lumaActive, standIns);
				}
				else
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaEngine mismatched cleanCb/Cr dimensions");
					res.nonFiniteCells = numCells;
					return res;
				}
				for (size_t i = 0; i < codedRecCb.v.size(); ++i)
				{
					ownGrainCb.v[i] = codedRecCb.v[i] - codedCleanCb.v[i];
					ownGrainCr.v[i] = codedRecCr.v[i] - codedCleanCr.v[i];
				}
				splitActive = true;
			}
			else
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::P10gGrainSeparator,
						in.hasCleanSplit
							? "chroma: hasCleanSplit set with empty clean planes; treating as unsplit"
							: "chroma: no upstream P10g chroma split supplied");
				codedCleanCb = codedRecCb;
				codedCleanCr = codedRecCr;
			}

			// Helper to pick grain-free anchor luma for anchor ingestion (ONLY called when splitActive == true).
			auto resolveAnchorCleanY = [&]() -> const TdV2Plane*
			{
				if (!in.anchorCleanY.Empty() && in.anchorCleanY.w == lumaW && in.anchorCleanY.h == lumaH)
					return &in.anchorCleanY;
				return nullptr;
			};

			// Helper to ingest anchor into 4:4:4 canvas ONLY when grain-free splitActive is true (H2 / plan s1.5 #2:
			// NEVER accumulate unsplit grainy chroma into the anchor canvas!).
			auto ingestAnchorIfSplit = [&]()
			{
				if (!in.isAnchorFrame)
					return;
				if (!splitActive)
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::P10gGrainSeparator, "chroma anchor ingest skipped: no grain-free clean split");
					return;
				}
				anchorCanvas_.AccumulateAnchorFrame(
					codedCleanCb, codedCleanCr, resolveAnchorCleanY(),
					lumaW, lumaH, in.codedSubsampling, in.siting, lumaActive,
					in.hasAnchorSplatMotion ? &in.anchorSplatMotion : nullptr,
					cellPx, cfg_.sourceBits, standIns,
					cfg_.sourceHz, in.seq, in.resetAnchorCfl);
			};

			// On cut / span-invalid frames, never blend across the cut; seed new shot's anchor canvas if isAnchorFrame && splitActive.
			if (in.isCutOrSpanInvalid)
			{
				ingestAnchorIfSplit();
				return res;
			}

			// B3: Decompose current grain-free coded Cb/Cr into L, M, H via shared U2 BandFromBase.
			const ThreeBandPlanes curBandsCb = DecomposeActiveBands(codedCleanCb, codedActive, standIns);
			const ThreeBandPlanes curBandsCr = DecomposeActiveBands(codedCleanCr, codedActive, standIns);

			// B4: Assemble up to kMaxBlendRefs (4) grain-free coded references:
			//   prevPeak, nextPeak, 4:4:4 anchor canvas, carried CfL (H5).
			struct WarpedChromaRef
			{
				TdV2Plane cleanCb;
				TdV2Plane cleanCr;
				ThreeBandPlanes bandsCb;
				ThreeBandPlanes bandsCr;
				std::vector<uint8_t> validPx;
				std::vector<double> motionVx;
				std::vector<double> motionVy;
				MotionGateResult motionGate;
				bool isExactCopy = false;
				bool isCfl = false;
				bool isAnchorDerived = false;
			};
			std::vector<WarpedChromaRef> refs;
			refs.reserve(kMaxBlendRefs);
			std::vector<uint8_t> nonFiniteCellFlag(numCells, 0u);
			std::vector<uint8_t> singleRefA2CellFlag(numCells, 0u);
			std::vector<uint8_t> singularCellFlag(numCells, 0u);

			auto markNonFiniteCodedRefCells = [&](const TdV2Plane& refP)
			{
				for (int y = codedActive.y0; y < codedActive.y1; ++y)
				{
					const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
					const int cy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
					for (int x = codedActive.x0; x < codedActive.x1; ++x)
					{
						if (!std::isfinite(refP.at(x, y)))
						{
							const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
							const int cx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
							nonFiniteCellFlag[static_cast<size_t>(cy) * res.cellsW + cx] = 1u;
						}
					}
				}
			};

			auto mergeMotionNonFinite = [&](const MotionGateResult& mg)
			{
				if (mg.nonFiniteCells == 0)
					return;
				for (size_t ci = 0; ci < numCells && ci < mg.nonFiniteCellMask.size(); ++ci)
					if (mg.nonFiniteCellMask[ci] != 0u)
						nonFiniteCellFlag[ci] = 1u;
			};

			auto prepareCodedRefPlane = [&](const TdV2Plane* src, TdV2Plane& tempOut) -> const TdV2Plane*
			{
				if (src == nullptr || src->Empty())
					return nullptr;
				if (src->w == cW && src->h == cH)
					return src;
				if (src->w == dW && src->h == dH)
				{
					tempOut = ReduceToCodedGrid(
						*src, lumaW, lumaH, in.deliveredFormat, in.codedSubsampling, in.siting, lumaActive, standIns);
					return &tempOut;
				}
				return nullptr;
			};

			auto tryAddPeakRef = [&](const ChromaReferenceInput& rIn)
			{
				if (refs.size() >= static_cast<size_t>(kMaxBlendRefs))
					return;
				TdV2Plane tmpCb, tmpCr;
				const TdV2Plane* refCb = prepareCodedRefPlane(rIn.cleanCb, tmpCb);
				const TdV2Plane* refCr = prepareCodedRefPlane(rIn.cleanCr, tmpCr);
				if (refCb == nullptr || refCr == nullptr)
					return;

				const CellMotionField* useMf = rIn.lumaMotion;
				if (useMf == nullptr)
				{
					// H9 (plan s1.2 L108): missing motion field MUST NEVER whole-frame blend as "Still".
					++res.motionStandInRefs;
					if (standIns != nullptr)
					{
						standIns->Engage(StandInKind::MotionFieldStandIn, "chroma null motion field; ref skipped per L4");
						standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "L4 missing motion field: chroma peak ref skipped (no whole-frame Still blend)");
					}
					return;
				}
				if (!useMf->IsWellFormed(res.cellsW, res.cellsH))
				{
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "chroma mismatched motion field; ref rejected");
					return;
				}

				WarpedChromaRef wr;
				wr.isExactCopy = rIn.isExactCopyOfCurrent;
				wr.motionVx = useMf->vx;
				wr.motionVy = useMf->vy;
				std::vector<uint8_t> validCb, validCr;
				const WarpDiagnostics dCb = ChromaDetail::WarpChromaPlaneWithLumaMotion(
					*refCb, codedCleanCb, codedActive, cSubX, cSubY, in.siting,
					useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, wr.cleanCb, validCb, standIns, useMf);
				const WarpDiagnostics dCr = ChromaDetail::WarpChromaPlaneWithLumaMotion(
					*refCr, codedCleanCr, codedActive, cSubX, cSubY, in.siting,
					useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, wr.cleanCr, validCr, standIns, useMf);
				res.borderBilinearPixels += dCb.borderBilinearPixels + dCr.borderBilinearPixels;
				if (dCb.nonFinitePixels > 0)
					markNonFiniteCodedRefCells(*refCb);
				if (dCr.nonFinitePixels > 0)
					markNonFiniteCodedRefCells(*refCr);
				wr.validPx.resize(validCb.size(), 0u);
				for (size_t i = 0; i < validCb.size(); ++i)
					wr.validPx[i] = (validCb[i] != 0u && validCr[i] != 0u) ? 1u : 0u;
				wr.bandsCb = DecomposeActiveBands(wr.cleanCb, codedActive, standIns);
				wr.bandsCr = DecomposeActiveBands(wr.cleanCr, codedActive, standIns);
				wr.motionGate = EvaluateCameraRelativeMotionGate(*useMf, standIns);
				mergeMotionNonFinite(wr.motionGate);
				refs.push_back(std::move(wr));
			};

			tryAddPeakRef(in.prevPeak);
			tryAddPeakRef(in.nextPeak);

			// Anchor read motion field for 4:4:4 canvas and carried CfL (H5 & H9).
			const CellMotionField* anchorReadMf = nullptr;
			bool anchorMotionRejected = false;
			if (in.hasAnchorReadMotion)
			{
				if (in.anchorReadMotion.IsWellFormed(res.cellsW, res.cellsH))
					anchorReadMf = &in.anchorReadMotion;
				else
				{
					anchorMotionRejected = true;
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "chroma anchorReadMotion malformed; anchor ref skipped");
				}
			}
			else if ((in.enableAnchorCanvasRef && anchorCanvas_.HasData()) || (in.enableCarriedCfl && anchorCanvas_.HasCfl()))
			{
				// H9 (plan s1.2 L108): missing anchor read motion MUST NEVER fabricate whole-frame "Still".
				anchorMotionRejected = true;
				++res.motionStandInRefs;
				if (standIns != nullptr)
				{
					standIns->Engage(StandInKind::MotionFieldStandIn, "chroma anchorReadMotion missing; anchor ref skipped per L4");
					standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "L4 missing anchorReadMotion: anchor/CfL ref skipped (no whole-frame Still blend)");
				}
			}

			// 4:4:4 Anchor Memory Canvas reference (H3: codedClean fallback fills invalid pixels; H4: gap-fill visible).
			if (!anchorMotionRejected && anchorReadMf != nullptr && in.enableAnchorCanvasRef && anchorCanvas_.HasData() &&
			    refs.size() < static_cast<size_t>(kMaxBlendRefs))
			{
				WarpedChromaRef wr;
				wr.isAnchorDerived = true;
				wr.motionVx = anchorReadMf->vx;
				wr.motionVy = anchorReadMf->vy;
				uint64_t gapFilled = 0;
				if (anchorCanvas_.SampleAtCodedGrid(
						in.codedSubsampling, in.siting, lumaActive, anchorReadMf, cellPx,
						wr.cleanCb, wr.cleanCr, wr.validPx, standIns,
						&codedCleanCb, &codedCleanCr, &gapFilled))
				{
					res.canvasGapFilledPixels += gapFilled;
					wr.bandsCb = DecomposeActiveBands(wr.cleanCb, codedActive, standIns);
					wr.bandsCr = DecomposeActiveBands(wr.cleanCr, codedActive, standIns);
					wr.motionGate = EvaluateCameraRelativeMotionGate(*anchorReadMf, standIns);
					mergeMotionNonFinite(wr.motionGate);
					refs.push_back(std::move(wr));
					res.anchorCanvasUsed = true;
				}
			}

			// Carried CfL (H5) reference: strictly applied to P_Y = Warp(anchorCleanY), NEVER current luma!
			int cflRefIdx = -1;
			if (!anchorMotionRejected && anchorReadMf != nullptr && in.enableCarriedCfl && anchorCanvas_.HasCfl() &&
			    refs.size() < static_cast<size_t>(kMaxBlendRefs))
			{
				WarpedChromaRef wr;
				wr.isCfl = true;
				wr.isAnchorDerived = true;
				wr.motionVx = anchorReadMf->vx;
				wr.motionVy = anchorReadMf->vy;
				if (anchorCanvas_.PredictCarriedCflAtCodedGrid(
						in.codedSubsampling, in.siting, lumaActive, anchorReadMf, cellPx,
						wr.cleanCb, wr.cleanCr, wr.validPx, standIns,
						&codedCleanCb, &codedCleanCr))
				{
					wr.bandsCb = DecomposeActiveBands(wr.cleanCb, codedActive, standIns);
					wr.bandsCr = DecomposeActiveBands(wr.cleanCr, codedActive, standIns);
					wr.motionGate = EvaluateCameraRelativeMotionGate(*anchorReadMf, standIns);
					mergeMotionNonFinite(wr.motionGate);
					cflRefIdx = static_cast<int>(refs.size());
					refs.push_back(std::move(wr));
					res.carriedCflUsed = true;
				}
			}

			res.activeRefCount = refs.size();
			if (refs.empty())
			{
				// Still ingest grain-free anchor frame into 4:4:4 canvas if isAnchorFrame && splitActive!
				ingestAnchorIfSplit();
				return res;
			}

			// B5: Per-cell Carried CfL disagreement check (kHuberKneeSq = 9.0 * max(cellRefDiffVar, stepTotalVar)).
			// Whenever chroma residual disagrees with luma-derived carried CfL on a cell, fall back visibly
			// to single-frame ChromaCfl (engaging StandInKind::ChromaCflFallback, marking res.cflFallbackMask,
			// and fitting single-frame per-cell OLS (a, b) into res.singleFrameCflFallback when current luma is available).
			std::vector<uint8_t> cflRejectCb(numCells, 0u);
			std::vector<uint8_t> cflRejectCr(numCells, 0u);
			const double stepVar = a2Stats.valid ? a2Stats.totalVar : 0.0;

			if (cflRefIdx >= 0)
			{
				res.cflFallbackMask.assign(numCells, 0u);
				res.singleFrameCflFallback.assign(numCells, AnchorCflCell{});

				const TdV2Plane* curLumaForSfCfl = nullptr;
				if (!in.anchorCleanY.Empty() && in.anchorCleanY.w == lumaW && in.anchorCleanY.h == lumaH && planeAllFinite(in.anchorCleanY))
					curLumaForSfCfl = &in.anchorCleanY;
				else if (!in.receivedY.Empty() && in.receivedY.w == lumaW && in.receivedY.h == lumaH && planeAllFinite(in.receivedY))
					curLumaForSfCfl = &in.receivedY;
				else if (isRgb && !rgbDerivedY.Empty() && planeAllFinite(rgbDerivedY))
					curLumaForSfCfl = &rgbDerivedY;

				ActiveSubPlaneView curYView{ curLumaForSfCfl, lumaActive.x0, lumaActive.y0, lumaActive.Width(), lumaActive.Height() };
				// Exact uniform 1-LSB quantization variance in native integer code units: delta^2 / 12.0 with delta = 1.0.
				const double sfRidgeFloor = (std::max)((kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0, kEngineNumFloor);

				const WarpedChromaRef& cflRef = refs[static_cast<size_t>(cflRefIdx)];
				for (int cy = 0; cy < res.cellsH; ++cy)
				{
					const int py0 = (std::max)(codedActive.y0, (cy * cellPx) / cSubY);
					const int py1 = (std::min)(codedActive.y1, ((cy + 1) * cellPx + cSubY - 1) / cSubY);
					for (int cx = 0; cx < res.cellsW; ++cx)
					{
						const int px0 = (std::max)(codedActive.x0, (cx * cellPx) / cSubX);
						const int px1 = (std::min)(codedActive.x1, ((cx + 1) * cellPx + cSubX - 1) / cSubX);
						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;

						double cflErrCb = 0.0, cflErrCr = 0.0;
						size_t cflCnt = 0;
						double refErrCb = 0.0, refErrCr = 0.0;
						size_t refCnt = 0;

						for (int y = py0; y < py1; ++y)
						{
							const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
							const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
							if (pcy != cy) continue;
							for (int x = px0; x < px1; ++x)
							{
								const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
								const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
								if (pcx != cx) continue;

								const size_t pIdx = static_cast<size_t>(y) * cW + x;
								if (cflRef.validPx[pIdx] != 0u)
								{
									const double dcb = codedCleanCb.at(x, y) - cflRef.cleanCb.at(x, y);
									const double dcr = codedCleanCr.at(x, y) - cflRef.cleanCr.at(x, y);
									cflErrCb += dcb * dcb;
									cflErrCr += dcr * dcr;
									++cflCnt;
								}
								for (int k = 0; k < static_cast<int>(refs.size()); ++k)
								{
									if (k == cflRefIdx || refs[static_cast<size_t>(k)].validPx[pIdx] == 0u)
										continue;
									const double dcb = codedCleanCb.at(x, y) - refs[static_cast<size_t>(k)].cleanCb.at(x, y);
									const double dcr = codedCleanCr.at(x, y) - refs[static_cast<size_t>(k)].cleanCr.at(x, y);
									refErrCb += dcb * dcb;
									refErrCr += dcr * dcr;
									++refCnt;
								}
							}
						}

						if (cflCnt == 0)
							continue;
						const double meanCflSqCb = cflErrCb / static_cast<double>(cflCnt);
						const double meanCflSqCr = cflErrCr / static_cast<double>(cflCnt);
						const double cellRefVarCb = refCnt > 0 ? (refErrCb / static_cast<double>(refCnt)) : 0.0;
						const double cellRefVarCr = refCnt > 0 ? (refErrCr / static_cast<double>(refCnt)) : 0.0;
						const double threshCb = kHuberKneeSq * (std::max)(cellRefVarCb, stepVar);
						const double threshCr = kHuberKneeSq * (std::max)(cellRefVarCr, stepVar);

						bool cellFallback = false;
						if (meanCflSqCb > threshCb)
						{
							cflRejectCb[cIdx] = 1u;
							cellFallback = true;
						}
						if (meanCflSqCr > threshCr)
						{
							cflRejectCr[cIdx] = 1u;
							cellFallback = true;
						}
						if (cellFallback)
						{
							res.cflFallbackMask[cIdx] = 1u;
							++res.cflFallbackCells;
							if (standIns != nullptr)
								standIns->Engage(StandInKind::ChromaCflFallback);

							// Single-frame ChromaCfl fallback fit on current frame's luma & coded chroma for downstream ChromaCfl handoff.
							if (curLumaForSfCfl != nullptr)
							{
								double sumY = 0.0, sumYY = 0.0, sumCb = 0.0, sumYCb = 0.0, sumCr = 0.0, sumYCr = 0.0;
								size_t sfCnt = 0;
								for (int y = py0; y < py1; ++y)
								{
									const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
									const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
									if (pcy != cy) continue;
									for (int x = px0; x < px1; ++x)
									{
										const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
										const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
										if (pcx != cx) continue;
										double yVal = 0.0;
										bool bBilin = false;
										if (ChromaDetail::SampleSitedActive(
												curYView, lx - static_cast<double>(lumaActive.x0), ly - static_cast<double>(lumaActive.y0),
												yVal, bBilin, /*allowEdgeClampOutside=*/true))
										{
											const double cbv = codedCleanCb.at(x, y);
											const double crv = codedCleanCr.at(x, y);
											sumY += yVal;
											sumYY += yVal * yVal;
											sumCb += cbv;
											sumYCb += yVal * cbv;
											sumCr += crv;
											sumYCr += yVal * crv;
											++sfCnt;
										}
									}
								}
								if (sfCnt >= 2)
								{
									const double invN = 1.0 / static_cast<double>(sfCnt);
									const double mY = sumY * invN;
									const double mCb = sumCb * invN;
									const double mCr = sumCr * invN;
									const double vY = (std::max)(0.0, sumYY * invN - mY * mY);
									const double den = vY + sfRidgeFloor;
									AnchorCflCell sf;
									sf.aCb = (sumYCb * invN - mY * mCb) / den;
									sf.bCb = mCb - sf.aCb * mY;
									sf.aCr = (sumYCr * invN - mY * mCr) / den;
									sf.bCr = mCr - sf.aCr * mY;
									sf.valid = std::isfinite(sf.aCb) && std::isfinite(sf.bCb) &&
									           std::isfinite(sf.aCr) && std::isfinite(sf.bCr);
									res.singleFrameCflFallback[cIdx] = sf;
								}
							}
						}
					}
				}
			}

			// B6: Per-band fusion on coded Cb and coded Cr independently using shared TdV2BlendMath + Layer-4 TdGuardPort.
			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double bandTransfer[3] = { gains.low, gains.mid, gains.fine };

			// Exact Parseval passband supremum ||H_b||_inf^2 for A2 rounding-cell variance upper bound
			// (1:1 parity with TdV2ReferenceEngine.h): L=1.0, M=midPassbandPeak^2, H=1.0.
			const double s1Sq = VpUnit::BandFromBase::kSigmaFine * VpUnit::BandFromBase::kSigmaFine;
			const double s2Sq = VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase;
			const double wStarSq = (2.0 * std::log(VpUnit::BandFromBase::kSigmaBase / VpUnit::BandFromBase::kSigmaFine)) / (s2Sq - s1Sq);
			const double midPassbandPeak = std::exp(-(s1Sq * wStarSq) / 2.0) - std::exp(-(s2Sq * wStarSq) / 2.0);
			const double a2BandPeakSq[3] = { 1.0, midPassbandPeak * midPassbandPeak, 1.0 };

			auto bandPlaneOf = [](const ThreeBandPlanes& bp, int b) -> const TdV2Plane&
			{
				return (b == 0) ? bp.L : ((b == 1) ? bp.M : bp.H);
			};

			auto fuseCodedPlane = [&](
				bool isCb,
				const TdV2Plane& codedRec,
				const TdV2Plane& codedClean,
				const TdV2Plane& ownGrain,
				const ThreeBandPlanes& curBands,
				const std::vector<double>& retainedGrainVar,
				const std::vector<uint8_t>& cflReject,
				TdV2Plane& outCodedDelta)
			{
				const int K = static_cast<int>(refs.size());
				std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> cellW;
				std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> cellDiagC;
				for (int b = 0; b < 3; ++b)
				{
					for (int k = 0; k < K; ++k)
					{
						cellW[b][k].assign(numCells, 0.0);
						cellDiagC[b][k].assign(numCells, 0.0);
					}
				}

				const int poolR = cfg_.momentPoolRadiusCells > 0 ? cfg_.momentPoolRadiusCells : 0;
				const int gW = (lumaW + grainCellPx - 1) / grainCellPx;
				std::vector<double> cellRetainedGrain(numCells, 0.0);

				// Collect per-cell fine-band energies & blind symmetric innovations for Q5(b) zero-energy dark block filter
				// whenever !splitActive and no explicit retainedGrainVar prior covers the frame.
				std::vector<double> blindCellEnergies;
				std::vector<double> blindCellIrs;
				if (!splitActive)
				{
					blindCellEnergies.reserve(numCells);
					blindCellIrs.reserve(numCells);
				}

				const auto g1 = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);

				for (int cy = 0; cy < res.cellsH; ++cy)
				{
					for (int cx = 0; cx < res.cellsW; ++cx)
					{
						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
						// Pass-1 face / non-rigid mask (H13): skip cell solve so cellW[b][k][cIdx] stays 0.0.
						if (isFaceMaskedCell(cIdx))
							continue;

						double cellGrainVar = 0.0;
						bool hasCellGrainPrior = false;
						if (!retainedGrainVar.empty())
						{
							const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
							const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
							const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
							if (gIdx < retainedGrainVar.size())
							{
								const double gv = retainedGrainVar[gIdx];
								if (std::isfinite(gv) && gv >= 0.0)
								{
									cellGrainVar = gv;
									hasCellGrainPrior = true;
								}
								else
								{
									nonFiniteCellFlag[cIdx] = 1u;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::NonFiniteClosure, "chroma non-finite retainedGrainVar");
								}
							}
							else
							{
								++res.grainPriorOobCells;
								if (standIns != nullptr)
									standIns->Engage(StandInKind::GrainPriorOutOfRange, "chroma retainedGrainVar index out of range");
							}
						}

						// Active references for this cell (excluding CfL if rejected by disagreement check).
						int activeIdx[kMaxBlendRefs] = {};
						int activeK = 0;
						for (int k = 0; k < K; ++k)
						{
							if (refs[static_cast<size_t>(k)].isCfl && cflReject[cIdx] != 0u)
								continue;
							activeIdx[activeK++] = k;
						}
						if (activeK == 0)
						{
							cellRetainedGrain[cIdx] = cellGrainVar;
							continue;
						}

						const int py0 = (std::max)(codedActive.y0, ((cy - poolR) * cellPx) / cSubY);
						const int py1 = (std::min)(codedActive.y1, ((cy + poolR + 1) * cellPx + cSubY - 1) / cSubY);
						const int px0 = (std::max)(codedActive.x0, ((cx - poolR) * cellPx) / cSubX);
						const int px1 = (std::min)(codedActive.x1, ((cx + poolR + 1) * cellPx + cSubX - 1) / cSubX);

						// H2 + Q5(a) + Q5(b): When !splitActive and no explicit per-cell grain prior was supplied, derive
						// bias-corrected projection innovation ComputeProjectionIr(..., HighPassBicubicWarpVarianceGain) / gains.fine
						// over the exact same pooled window (immune to sub-pixel bicubic texture attenuation on coded chroma pans).
						// Clean grain-free refs below the exact 1-LSB quantization floor contribute 0.0.
						if (!splitActive && !hasCellGrainPrior)
						{
							double sumCurH2 = 0.0;
							double sumRefH2[kMaxBlendRefs] = {};
							double sumCrossH[kMaxBlendRefs] = {};
							size_t hCnt = 0;
							double ownCellH2 = 0.0;
							size_t ownCellCnt = 0;

							for (int y = py0; y < py1; ++y)
							{
								const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
								const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
								for (int x = px0; x < px1; ++x)
								{
									const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
									const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
									const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
									if (isFaceMaskedCell(pCell) || isFocusVetoedBand(pCell, 2))
										continue;
									const size_t pIdx = static_cast<size_t>(y) * cW + x;

									const double hCur = curBands.H.at(x, y);
									if (pcy == cy && pcx == cx)
									{
										ownCellH2 += hCur * hCur;
										++ownCellCnt;
									}

									bool allValidH = true;
									double hRefs[kMaxBlendRefs] = {};
									for (int ai = 0; ai < activeK; ++ai)
									{
										const int k = activeIdx[ai];
										const WarpedChromaRef& wr = refs[static_cast<size_t>(k)];
										if (wr.validPx[pIdx] == 0u || wr.motionGate.gateMH[pCell] <= 0.0)
										{
											allValidH = false;
											break;
										}
										const ThreeBandPlanes& rBands = isCb ? wr.bandsCb : wr.bandsCr;
										hRefs[ai] = rBands.H.at(x, y);
									}
									if (allValidH)
									{
										sumCurH2 += hCur * hCur;
										for (int ai = 0; ai < activeK; ++ai)
										{
											sumRefH2[ai] += hRefs[ai] * hRefs[ai];
											sumCrossH[ai] += hCur * hRefs[ai];
										}
										++hCnt;
									}
								}
							}

							double cellInnov = 0.0;
							if (hCnt > 0 && gains.fine > kEngineNumFloor)
							{
								const double invH = 1.0 / static_cast<double>(hCnt);
								const double meanCur = sumCurH2 * invH;
								// Exact 1-LSB uniform quantization variance transferred through the fine band (Rule 1).
								const double refQuantFloor = ((kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0) * gains.fine;
								double sumProjIr = 0.0;
								size_t validRefCnt = 0;
								for (int ai = 0; ai < activeK; ++ai)
								{
									const int k = activeIdx[ai];
									const double meanRef = sumRefH2[ai] * invH;
									const double cross = sumCrossH[ai] * invH;
									const double vxC = (cIdx < refs[static_cast<size_t>(k)].motionVx.size())
										? (refs[static_cast<size_t>(k)].motionVx[cIdx] / static_cast<double>(cSubX))
										: 0.0;
									const double vyC = (cIdx < refs[static_cast<size_t>(k)].motionVy.size())
										? (refs[static_cast<size_t>(k)].motionVy[cIdx] / static_cast<double>(cSubY))
										: 0.0;
									const double warpBeta = HighPassBicubicWarpVarianceGain(vxC, vyC, g1, g1);
									if (meanRef > refQuantFloor)
									{
										sumProjIr += ComputeProjectionIr(meanCur, cross, meanRef, standIns, warpBeta);
										++validRefCnt;
									}
								}
								cellInnov = (validRefCnt > 0) ? ((sumProjIr / static_cast<double>(validRefCnt)) / gains.fine) : 0.0;
								cellGrainVar = cellInnov;
							}
							if (ownCellCnt > 0)
							{
								blindCellEnergies.push_back(ownCellH2 / static_cast<double>(ownCellCnt));
								blindCellIrs.push_back(cellInnov);
							}
						}
						cellRetainedGrain[cIdx] = cellGrainVar;

						// Compute pooled own-grain variance over the exact same [px0, px1) x [py0, py1) window as C
						// when splitActive == true (eliminates 8x8 coded 4:2:0 cell variance inflation in alphaLeak).
						double cellOwnGrainVar = 0.0;
						if (splitActive)
						{
							double ogSumSq = 0.0;
							size_t ogCnt = 0;
							for (int y = py0; y < py1; ++y)
							{
								for (int x = px0; x < px1; ++x)
								{
									const double og = ownGrain.at(x, y);
									ogSumSq += og * og;
									++ogCnt;
								}
							}
							if (ogCnt > 0)
								cellOwnGrainVar = ogSumSq / static_cast<double>(ogCnt);
						}

						for (int b = 0; b < 3; ++b)
						{
							const TdV2Plane& curB = bandPlaneOf(curBands, b);
							MomentAccumulatorFp32 acc(activeK);
							double ownSumSq[kMaxBlendRefs] = {};
							size_t ownSampleCount = 0;

							for (int y = py0; y < py1; ++y)
							{
								const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
								const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
								for (int x = px0; x < px1; ++x)
								{
									const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
									const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
									const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
									if (isFaceMaskedCell(pCell) || isFocusVetoedBand(pCell, b))
										continue;
									const size_t pIdx = static_cast<size_t>(y) * cW + x;

									bool allValid = true;
									float diffs[kMaxBlendRefs] = {};
									const double curV = curB.at(x, y);
									for (int ai = 0; ai < activeK; ++ai)
									{
										const int k = activeIdx[ai];
										const WarpedChromaRef& wr = refs[static_cast<size_t>(k)];
										const double pGate = (b == 0) ? wr.motionGate.gateL[pCell] : wr.motionGate.gateMH[pCell];
										if (wr.validPx[pIdx] == 0u || pGate <= 0.0)
										{
											allValid = false;
											break;
										}
										const ThreeBandPlanes& rBands = isCb ? wr.bandsCb : wr.bandsCr;
										diffs[ai] = static_cast<float>(curV - bandPlaneOf(rBands, b).at(x, y));
									}
									if (allValid)
									{
										acc.AddSample(diffs);
										if (pCell == cIdx)
										{
											++ownSampleCount;
											for (int ai = 0; ai < activeK; ++ai)
												ownSumSq[ai] += static_cast<double>(diffs[ai]) * static_cast<double>(diffs[ai]);
										}
									}
								}
							}

							if (acc.sampleCount == 0)
								continue;

							double C[kMaxBlendRefs][kMaxBlendRefs] = {};
							acc.SecondMoment(C);
							// Store unpooled own-cell second moment capped by pooled C[ai][ai] (1:1 parity with TdV2ReferenceEngine.h).
							for (int ai = 0; ai < activeK; ++ai)
							{
								const double ownDiag = (ownSampleCount > 0)
									? (ownSumSq[ai] / static_cast<double>(ownSampleCount))
									: C[ai][ai];
								cellDiagC[b][activeIdx[ai]][cIdx] = (std::min)(ownDiag, C[ai][ai]);
							}

							double cVec[kMaxBlendRefs] = {};
							for (int ai = 0; ai < activeK; ++ai)
							{
								const int k = activeIdx[ai];
								// A2 rounding-cell prior using Parseval passband supremum ||H_b||_inf^2,
								// capped to [0, C[ai][ai]] per Plan L81 (c_k <= C_kk).
								const double cA2 = (std::min)(EstimateC_A2(a2Stats, refs[static_cast<size_t>(k)].isExactCopy) * a2BandPeakSq[b], C[ai][ai]);
								double cA1 = cA2;

								// Find an independent A1 instrument partner (LOW-7): NEVER allow two anchor-derived
								// refs (e.g., anchorCanvas & carriedCfl) to instrument each other!
								int partner = -1;
								const bool selfAnchor = refs[static_cast<size_t>(k)].isAnchorDerived;
								for (int cand = activeK - 1; cand >= 0; --cand)
								{
									if (cand == ai) continue;
									const bool candAnchor = refs[static_cast<size_t>(activeIdx[cand])].isAnchorDerived;
									if (!candAnchor)
									{
										partner = cand;
										break;
									}
									if (!selfAnchor && candAnchor)
										partner = cand;
								}

								if (partner >= 0)
								{
									cA1 = EstimateC_A1(C[ai][partner], C[ai][ai], C[partner][partner],
									                   cellGrainVar, bandTransfer[b]);
								}
								else if (cfg_.estimator != VarianceEstimator::A2_RoundingCells)
								{
									singleRefA2CellFlag[cIdx] = 1u;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
									if (activeK >= 2)
									{
										// Cap cA1 from above by non-copy cross-moments so C^-1 c never inverts weights.
										for (int j = 0; j < activeK; ++j)
										{
											if (j == ai || refs[static_cast<size_t>(activeIdx[j])].isExactCopy)
												continue;
											const double cap = EstimateC_A1(C[ai][j], C[ai][ai], C[j][j],
											                                cellGrainVar, bandTransfer[b]);
											if (cap < cA1)
												cA1 = cap;
										}
									}
									// When unsplit grain is present in a single-ref cell, cap c by excess difference
									// variance above independent two-frame grain variance 2 * cellGrainVar * bandTransfer[b].
									if (!splitActive && cellGrainVar > kEngineNumFloor)
									{
										const double excess = C[ai][ai] - 2.0 * cellGrainVar * bandTransfer[b];
										cA1 = (std::min)(cA1, excess > 0.0 ? excess : 0.0);
									}
								}
								cVec[ai] = SelectBlindC(cfg_.estimator, cA1, cA2);
								if (refs[static_cast<size_t>(k)].isExactCopy)
									cVec[ai] = 0.0;
							}

							const BlendSolveResult solved = SolveConvexWeights(C, cVec, activeK, kDefaultTraceRegScale, standIns);
							if (solved.nonFiniteClosed) { nonFiniteCellFlag[cIdx] = 1u; continue; }
							if (solved.singular) { singularCellFlag[cIdx] = 1u; continue; }

							const double focusGate = isFocusVetoedBand(cIdx, b) ? 0.0 : 1.0;
							for (int ai = 0; ai < activeK; ++ai)
							{
								const int k = activeIdx[ai];
								const WarpedChromaRef& wr = refs[static_cast<size_t>(k)];
								const double mGate = (b == 0) ? wr.motionGate.gateL[cIdx] : wr.motionGate.gateMH[cIdx];
								cellW[b][k][cIdx] = solved.weights[ai] * mGate * focusGate;
							}
						}

						// Layer 2/3 coarse-to-fine leaked-grain transfer budget (1:1 parity with TdV2ReferenceEngine.h;
						// refs/111 s7.3 & TdGuardPort::kMinVarianceRetention = 0.98^2):
						// Protects minor leaked grain in split chroma (e.g. 15% leak with retainedGrainVar supplied)
						// and raw unsplit grain (!splitActive) while allowing full L/M compression repair.
						const bool minorSplitLeak = splitActive && (cellOwnGrainVar > cellGrainVar);
						const bool rawUnsplitGrain = !splitActive;
						if (cfg_.estimator != VarianceEstimator::A2_RoundingCells &&
						    cellGrainVar > kEngineNumFloor && (minorSplitLeak || rawUnsplitGrain))
						{
							double alphaLeak = 1.0;
							if (minorSplitLeak)
							{
								const double sigmaLeak = std::sqrt(cellGrainVar);
								const double sigmaOwn = std::sqrt(cellOwnGrainVar);
								const double sigmaTotal = sigmaOwn + sigmaLeak;
								alphaLeak = (sigmaTotal > kEngineNumFloor) ? (sigmaLeak / sigmaTotal) : 0.0;
							}
							if (alphaLeak > kEngineNumFloor)
							{
								const double bandOrigin[3] = { gains.originLow, gains.originMid, gains.originFine };
								// Rule-1 quadrature split (1:1 parity with TdV2ReferenceEngine.h & TdV2Shaders.h):
								// allocate tau / sqrt(2) = (1 - sqrt(kMinVarianceRetention)) / sqrt(2) to deterministic
								// attenuation so deterministic + finite-sample variance stay strictly inside the 2% bar.
								double remGrainBudget = (1.0 - std::sqrt(TdGuardPort::kMinVarianceRetention)) / std::sqrt(2.0);
								for (int b = 0; b < 3; ++b)
								{
									double bandWSum = 0.0;
									for (int k = 0; k < K; ++k)
										bandWSum += cellW[b][k][cIdx];
									const double bandLossPerUnitW = alphaLeak * bandOrigin[b];
									if (bandLossPerUnitW > kEngineNumFloor && bandWSum > kEngineNumFloor)
									{
										const double maxBandW = remGrainBudget / bandLossPerUnitW;
										if (bandWSum > maxBandW)
										{
											const double scale = (maxBandW > 0.0) ? (maxBandW / bandWSum) : 0.0;
											for (int k = 0; k < K; ++k)
												cellW[b][k][cIdx] *= scale;
											bandWSum *= scale;
										}
										remGrainBudget = (std::max)(0.0, remGrainBudget - bandLossPerUnitW * bandWSum);
									}
								}
							}
						}
					}
				}

				// Q5(b) integration: exclude crushed-black zero-energy flat blocks strictly by measured fine-band
				// energy (never code value, preserving BTB/WTW) when aggregating blind unsplit NLF diagnostics.
				if (!splitActive && !blindCellEnergies.empty())
				{
					const size_t codedCellPxW = static_cast<size_t>((std::max)(1, cellPx / cSubX));
					const size_t codedCellPxH = static_cast<size_t>((std::max)(1, cellPx / cSubY));
					const DarkNlfResult nlfRes = AccumulateDarkNlfExcludingZeroEnergy(
						blindCellEnergies, blindCellIrs, codedCellPxW * codedCellPxH,
						1.0, 0.5, standIns);
					res.excludedZeroEnergyBlocks += nlfRes.excludedZeroEnergyBlocks;
					if (nlfRes.valid)
					{
						if (isCb) res.estimatedBlindNlfCb = nlfRes.nlfEstimate;
						else      res.estimatedBlindNlfCr = nlfRes.nlfEstimate;
					}
				}

				// Codec-frame-grid-aligned interior 8x8 DCT bounds on coded chroma plane (if transformIdentified8x8).
				const int bxStart = ((codedActive.x0 + 7) / 8) * 8;
				const int byStart = ((codedActive.y0 + 7) / 8) * 8;
				const int bxEnd = (codedActive.x1 / 8) * 8;
				const int byEnd = (codedActive.y1 / 8) * 8;

				// H14 + H3: Resolve CodecModel P_B (explicit in.codecModelPbCb/Cr if supplied, else refs[0].clean stand-in)
				// and validate per-8x8-block prediction pixel validity BEFORE applying pixel-domain vs DCT bounds.
				TdV2Plane explicitPbTmp;
				const TdV2Plane* predCleanPtr = nullptr;
				const std::vector<uint8_t>* predValidPtr = nullptr;
				bool usingPbStandIn = false;
				bool canRunPocs = in.transformIdentified8x8 && effStepDelta > 0.0 && (bxStart < bxEnd) && (byStart < byEnd);
				if (canRunPocs)
				{
					const TdV2Plane* explicitPbSrc = isCb ? in.codecModelPbCb : in.codecModelPbCr;
					if (explicitPbSrc != nullptr)
					{
						const TdV2Plane* resolved = prepareCodedRefPlane(explicitPbSrc, explicitPbTmp);
						if (resolved != nullptr && planeAllFinite(*resolved))
						{
							predCleanPtr = resolved;
							predValidPtr = nullptr;
							usingPbStandIn = false;
						}
						else
						{
							canRunPocs = false;
							nonFiniteCellFlag.assign(numCells, 1u);
							if (standIns != nullptr)
								standIns->Engage(StandInKind::NonFiniteClosure, "chroma explicit codecModelPb invalid or non-finite");
						}
					}
					else
					{
						predCleanPtr = isCb ? &refs[0].cleanCb : &refs[0].cleanCr;
						predValidPtr = &refs[0].validPx;
						usingPbStandIn = true;
					}
				}

				auto isBlockPredValid = [&](int bx, int by) -> bool
				{
					if (!canRunPocs || predCleanPtr == nullptr)
						return false;
					for (int j = 0; j < 8; ++j)
					{
						for (int i = 0; i < 8; ++i)
						{
							const size_t pIdx = static_cast<size_t>(by + j) * cW + static_cast<size_t>(bx + i);
							if (predValidPtr != nullptr && (*predValidPtr)[pIdx] == 0u)
								return false;
							if (!std::isfinite(predCleanPtr->at(bx + i, by + j)))
								return false;
						}
					}
					return true;
				};

				for (int y = codedActive.y0; y < codedActive.y1; ++y)
				{
					const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
					const int cy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
					for (int x = codedActive.x0; x < codedActive.x1; ++x)
					{
						const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
						const int cx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
						const size_t pIdx = static_cast<size_t>(y) * cW + x;

						// Pass-1 face / non-rigid mask: exact delta == 0.0 at every masked pixel (H13).
						if (isFaceMaskedCell(cIdx))
						{
							outCodedDelta.at(x, y) = 0.0;
							continue;
						}

						const bool inFull8x8Block = canRunPocs &&
						                            (x >= bxStart) && (x < bxEnd) && (y >= byStart) && (y < byEnd) &&
						                            isBlockPredValid((x / 8) * 8, (y / 8) * 8);

						double totalDelta = 0.0;
						for (int b = 0; b < 3; ++b)
						{
							const double curV = bandPlaneOf(curBands, b).at(x, y);
							double bandDelta = 0.0;
							for (int k = 0; k < K; ++k)
							{
								if (refs[static_cast<size_t>(k)].validPx[pIdx] == 0u || cellW[b][k][cIdx] <= 0.0)
									continue;
								const ThreeBandPlanes& rBands = isCb ? refs[static_cast<size_t>(k)].bandsCb : refs[static_cast<size_t>(k)].bandsCr;
								const double diff = curV - bandPlaneOf(rBands, b).at(x, y);
								const double wEff = cfg_.enablePerCornerHuber
									? SpatialBilinearHuberWeight(cellW[b][k].data(), cellDiagC[b][k].data(), res.cellsW, res.cellsH, lx, ly, cellPx, diff)
									: SpatialBilinearWeight(cellW[b][k].data(), res.cellsW, res.cellsH, lx, ly, cellPx);
								if (wEff <= 0.0)
									continue;
								bandDelta -= wEff * diff;
							}
							if (!inFull8x8Block)
							{
								// Use exact Parseval passband supremum a2BandPeakSq[b] (consistent with cA2 and Branch A)
								// so coherent L/M quantization wear is bounded at its true passband limit without over-clipping.
								const double bandVar = a2Stats.valid ? (a2Stats.totalVar * a2BandPeakSq[b]) : 0.0;
								if (std::fabs(bandDelta) > 0.0)
									bandDelta = ApplyTransformUnknownBound(bandDelta, bandVar, standIns);
							}
							totalDelta += bandDelta;
						}
						outCodedDelta.at(x, y) = (totalDelta == 0.0) ? 0.0 : totalDelta;
					}
				}

				// Identified 8x8 DCT STRICT/CELL POCS projection on coded chroma plane (codec-grid-aligned).
				// H3: Skips any 8x8 block containing an invalid prediction pixel (validPx == 0).
				// H14: Engages StandInKind::CodecModelPbStandIn visibly whenever refs[0].clean stands in for P_B.
				if (canRunPocs && predCleanPtr != nullptr)
				{
					const TdV2Plane& predClean = *predCleanPtr;
					const int iters = (std::max)(1, (std::min)(2, cfg_.cellProjIterations));
					for (int it = 0; it < iters; ++it)
					{
						for (int by = byStart; by < byEnd; by += 8)
						{
							for (int bx = bxStart; bx < bxEnd; bx += 8)
							{
								// H3: Skip POCS on blocks where any prediction pixel is invalid (never project against garbage/0.0).
								if (!isBlockPredValid(bx, by))
									continue;

								// Skip untouched / vetoed zero-delta 8x8 blocks so FP64 DCT round-trip stays bit-exact.
								bool anyNonZeroDelta = false;
								for (int j = 0; j < 8 && !anyNonZeroDelta; ++j)
									for (int i = 0; i < 8; ++i)
										if (outCodedDelta.at(bx + i, by + j) != 0.0) { anyNonZeroDelta = true; break; }
								if (!anyNonZeroDelta)
									continue;

								if (it == 0 && usingPbStandIn)
								{
									++res.codecModelPbStandInCells;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::CodecModelPbStandIn);
								}

								double sentRes[64] = {}, blendRes[64] = {}, projRes[64] = {};
								for (int j = 0; j < 8; ++j)
								{
									for (int i = 0; i < 8; ++i)
									{
										const double cVal = codedClean.at(bx + i, by + j);
										const double pVal = predClean.at(bx + i, by + j);
										sentRes[j * 8 + i] = cVal - pVal;
										blendRes[j * 8 + i] = (cVal + outCodedDelta.at(bx + i, by + j)) - pVal;
									}
								}
								ProjectResidual8x8Dct(sentRes, blendRes, effStepDelta, effPhi, cfg_.residualMode, projRes);
								for (int j = 0; j < 8; ++j)
								{
									const double ly = in.siting.CodedToLumaY(static_cast<double>(by + j), cSubY);
									const int cy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
									for (int i = 0; i < 8; ++i)
									{
										const double lx = in.siting.CodedToLumaX(static_cast<double>(bx + i), cSubX);
										const int cx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
										const size_t bCellIdx = static_cast<size_t>(cy) * res.cellsW + cx;
										if (isFaceMaskedCell(bCellIdx) || isFocusChangeCell(bCellIdx) ||
										    (isDefocusedCell(bCellIdx) && cfg_.bokehMode != BokehMode::LowBandCellOnly))
										{
											outCodedDelta.at(bx + i, by + j) = 0.0;
											continue;
										}
										const double cVal = codedClean.at(bx + i, by + j);
										const double pVal = predClean.at(bx + i, by + j);
										outCodedDelta.at(bx + i, by + j) = (pVal + projRes[j * 8 + i]) - cVal;
									}
								}
							}
						}
						for (int by = byStart; by < byEnd; by += 8)
						{
							for (int bx = bxStart; bx < bxEnd; bx += 8)
							{
								if (!isBlockPredValid(bx, by))
									continue;
								for (int j = 0; j < 8; ++j)
								{
									for (int i = 0; i < 8; ++i)
									{
										const int x = bx + i;
										const int y = by + j;
										const double dVal = outCodedDelta.at(x, y);
										if (dVal == 0.0)
											continue;
										const double cVal = codedClean.at(x, y);
										const double clamped = (std::max)(0.0, (std::min)(peakCode, cVal + dVal));
										outCodedDelta.at(x, y) = clamped - cVal;
									}
								}
							}
						}
					}
				}

				// Layer 4: Shared Output Grain Guard (TdGuardPort) on output ONLY (never touching anchorCanvas_!).
				// H2: Use `splitActive` consistently; when !splitActive, ALWAYS evaluate TdGuardPort on every cell
				// using target = inFineE (NEVER skip past the guard when ownGrain == 0 && retainedGrainVar == 0!).
				TdV2Plane candOut = codedRec;
				for (int y = codedActive.y0; y < codedActive.y1; ++y)
					for (int x = codedActive.x0; x < codedActive.x1; ++x)
						candOut.at(x, y) = codedClean.at(x, y) + outCodedDelta.at(x, y) + ownGrain.at(x, y);

				const ThreeBandPlanes inFullBands = DecomposeActiveBands(codedRec, codedActive, nullptr);
				const ThreeBandPlanes outFullBands = DecomposeActiveBands(candOut, codedActive, nullptr);
				const ThreeBandPlanes ownGrainBands = splitActive
					? DecomposeActiveBands(ownGrain, codedActive, nullptr)
					: ThreeBandPlanes{};

				for (int cy = 0; cy < res.cellsH; ++cy)
				{
					const int y0 = (std::max)(codedActive.y0, (cy * cellPx) / cSubY);
					const int y1 = (std::min)(codedActive.y1, ((cy + 1) * cellPx + cSubY - 1) / cSubY);
					if (y1 <= y0) continue;
					for (int cx = 0; cx < res.cellsW; ++cx)
					{
						const int x0 = (std::max)(codedActive.x0, (cx * cellPx) / cSubX);
						const int x1 = (std::min)(codedActive.x1, ((cx + 1) * cellPx + cSubX - 1) / cSubX);
						if (x1 <= x0) continue;

						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
						if (isFaceMaskedCell(cIdx) || isFocusChangeCell(cIdx) ||
						    (isDefocusedCell(cIdx) && cfg_.bokehMode != BokehMode::LowBandCellOnly))
							continue;
						double ownGrainE = 0.0, ownGrainFineE = 0.0, inFineE = 0.0, outFineE = 0.0;
						double inCovOwn = 0.0, outCovOwn = 0.0;
						size_t cnt = 0;
						for (int y = y0; y < y1; ++y)
						{
							if ((std::min)(res.cellsH - 1, (y * cSubY) / cellPx) != cy)
								continue;
							for (int x = x0; x < x1; ++x)
							{
								if ((std::min)(res.cellsW - 1, (x * cSubX) / cellPx) != cx)
									continue;
								const double og = ownGrain.at(x, y);
								const double inf = inFullBands.H.at(x, y);
								const double outf = outFullBands.H.at(x, y);
								ownGrainE += og * og;
								if (splitActive)
								{
									const double ogf = ownGrainBands.H.at(x, y);
									ownGrainFineE += ogf * ogf;
									inCovOwn += inf * ogf;
									outCovOwn += outf * ogf;
								}
								inFineE += inf * inf;
								outFineE += outf * outf;
								++cnt;
							}
						}
						if (cnt == 0) continue;
						const double invC = 1.0 / static_cast<double>(cnt);
						if (splitActive && ownGrainE * invC <= kEngineNumFloor && cellRetainedGrain[cIdx] <= kEngineNumFloor)
							continue;

						const double leakedFineE = cellRetainedGrain[cIdx] * gains.fine;
						// Account for positive correlation between ownGrain and leaked own grain inside cleanCb/Cr:
						// E[(H(own) + H(leaked))^2] = (sqrt(ownFineE) + sqrt(leakedFineE))^2, capped by inFineE.
						const double targetStd = std::sqrt(ownGrainFineE * invC) + std::sqrt(leakedFineE > 0.0 ? leakedFineE : 0.0);
						const double targetFineE = splitActive
							? (std::min)(inFineE * invC, targetStd * targetStd)
							: (inFineE * invC);

						// Texture-immune own-grain fine-band covariance check when leaked grain is reported
						// (1:1 parity with TdV2ReferenceEngine.h): fine scene texture cancels out of
						// Cov(out_H, ownGrain_H) / Cov(in_H, ownGrain_H).
						double effOutFineE = outFineE * invC;
						if (splitActive && ownGrainFineE * invC > kEngineNumFloor && cellRetainedGrain[cIdx] > kEngineNumFloor &&
						    inCovOwn > kEngineNumFloor)
						{
							const double covRatio = outCovOwn > 0.0 ? (outCovOwn / inCovOwn) : 0.0;
							const double covImpliedOutE = targetFineE * covRatio * covRatio;
							if (covImpliedOutE < effOutFineE)
								effOutFineE = covImpliedOutE;
						}

						const auto gDec = TdGuardPort::Evaluate(targetFineE, effOutFineE, standIns);
						if (gDec.violated && gDec.addBackFraction > 0.0)
						{
							++res.guardViolationCells;
							for (int y = y0; y < y1; ++y)
							{
								if ((std::min)(res.cellsH - 1, (y * cSubY) / cellPx) != cy)
									continue;
								for (int x = x0; x < x1; ++x)
								{
									if ((std::min)(res.cellsW - 1, (x * cSubX) / cellPx) != cx)
										continue;
									const double addBack = gDec.addBackFraction * (inFullBands.H.at(x, y) - outFullBands.H.at(x, y));
									outCodedDelta.at(x, y) += addBack;
								}
							}
						}
					}
				}
			};

			fuseCodedPlane(true, codedRecCb, codedCleanCb, ownGrainCb, curBandsCb,
			               in.retainedGrainVarCb, cflRejectCb, res.codedDeltaCb);
			fuseCodedPlane(false, codedRecCr, codedCleanCr, ownGrainCr, curBandsCr,
			               in.retainedGrainVarCr, cflRejectCr, res.codedDeltaCr);

			// B7: Upsample ONLY the signed coded delta back to the delivered chroma grid.
			res.deliveredDeltaCb = UpsampleDeltaToDelivered(
				res.codedDeltaCb, lumaW, lumaH, in.codedSubsampling, in.deliveredFormat, in.siting, lumaActive, standIns);
			res.deliveredDeltaCr = UpsampleDeltaToDelivered(
				res.codedDeltaCr, lumaW, lumaH, in.codedSubsampling, in.deliveredFormat, in.siting, lumaActive, standIns);

			// Keep delivered delta strictly 0.0 at every Pass-1 face-masked or rack-focus-vetoed pixel
			// (even when coded->delivered upsampling straddles a mask edge).
			if (res.faceMaskedCells > 0 || res.focusVetoCells > 0)
			{
				for (int y = delivActive.y0; y < delivActive.y1; ++y)
				{
					const double ly = in.siting.CodedToLumaY(static_cast<double>(y), dSubY);
					const int cy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
					for (int x = delivActive.x0; x < delivActive.x1; ++x)
					{
						const double lx = in.siting.CodedToLumaX(static_cast<double>(x), dSubX);
						const int cx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
						const size_t dCellIdx = static_cast<size_t>(cy) * res.cellsW + cx;
						if (isFaceMaskedCell(dCellIdx) || isFocusChangeCell(dCellIdx) ||
						    (isDefocusedCell(dCellIdx) && cfg_.bokehMode != BokehMode::LowBandCellOnly))
						{
							res.deliveredDeltaCb.at(x, y) = 0.0;
							res.deliveredDeltaCr.at(x, y) = 0.0;
						}
					}
				}
			}

			// B8: Apply delivered signed delta strictly inside ActiveRect (Mode::On only; Shadow stays passthrough).
			if (cfg_.ModifiesPixels())
				ApplyDeliveredDelta(in, delivActive, res.deliveredDeltaCb, res.deliveredDeltaCr, res, standIns,
				                    &nonFiniteCellFlag, res.cellsW, res.cellsH, cellPx);

			// Tally harmonized unique-cell counters (LOW-5: unique cells across refs, bands, and Cb/Cr planes).
			for (size_t ci = 0; ci < numCells; ++ci)
			{
				if (nonFiniteCellFlag[ci] != 0u) ++res.nonFiniteCells;
				if (singleRefA2CellFlag[ci] != 0u) ++res.singleRefA2Cells;
				if (singularCellFlag[ci] != 0u) ++res.singularCells;
			}

			// B9: If this frame is an anchor frame AND splitActive is true, accumulate its grain-free coded chroma + clean luma
			// into the 4:4:4 anchor memory canvas (strictly grain-free, NEVER contaminated by Layer-4 add-back or unsplit grain!).
			ingestAnchorIfSplit();

			return res;
		}

	private:
		static void ApplyDeliveredDelta(
			const ChromaProcessInput& in,
			const ActiveRect& delivActive,
			const TdV2Plane& dCbPlane,
			const TdV2Plane& dCrPlane,
			ChromaProcessResult& res,
			StandInRegistry* standIns,
			std::vector<uint8_t>* nonFiniteCellFlag = nullptr,
			int cellsW = 0,
			int cellsH = 0,
			int cellPx = 0)
		{
			if (in.deliveredFormat == DeliveredChromaFormat::Rgb444)
			{
				const bool trackCells = (nonFiniteCellFlag != nullptr && cellsW > 0 && cellsH > 0 && cellPx > 0 &&
				                         nonFiniteCellFlag->size() == static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH));
				bool anyRgbApplyFailed = false;
				for (int y = delivActive.y0; y < delivActive.y1; ++y)
				{
					const int cy = trackCells ? (std::min)(cellsH - 1, y / cellPx) : 0;
					for (int x = delivActive.x0; x < delivActive.x1; ++x)
					{
						if (!in.matrix.ApplySignedChromaDeltaToRgb(
								in.receivedR.at(x, y), in.receivedG.at(x, y), in.receivedB.at(x, y),
								dCbPlane.at(x, y), dCrPlane.at(x, y),
								res.outputR.at(x, y), res.outputG.at(x, y), res.outputB.at(x, y),
								standIns))
						{
							anyRgbApplyFailed = true;
							if (trackCells)
							{
								const int cx = (std::min)(cellsW - 1, x / cellPx);
								(*nonFiniteCellFlag)[static_cast<size_t>(cy) * cellsW + cx] = 1u;
							}
						}
					}
				}
				if (anyRgbApplyFailed && !trackCells)
					++res.nonFiniteCells;
			}
			else
			{
				for (int y = delivActive.y0; y < delivActive.y1; ++y)
				{
					for (int x = delivActive.x0; x < delivActive.x1; ++x)
					{
						const double dcb = dCbPlane.at(x, y);
						const double dcr = dCrPlane.at(x, y);
						if (dcb != 0.0)
							res.outputCb.at(x, y) = in.receivedCb.at(x, y) + dcb;
						if (dcr != 0.0)
							res.outputCr.at(x, y) = in.receivedCr.at(x, y) + dcr;
					}
				}
			}
		}

		Config cfg_;
		TdV2ChromaAnchorCanvas anchorCanvas_;
	};
}
