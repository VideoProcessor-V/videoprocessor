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
//     luma P_Y = Warp(anchorCleanY) — NEVER to current frame's luma curY! Rejects CfL visibly via
//     StandInKind::ChromaCflFallback whenever per-cell chroma residual disagrees beyond 3-sigma
//     (kHuberKneeSq * max(cellRefDiffVar, stepTotalVar)).
//   - Unidentified coded grid: visibly engages StandInKind::ChromaGridDetector and runs strictly
//     coarse-band-only fusion (sigma = 0.5 * kCanonicalBlockPx = 8.0 luma px, M/H weights = 0,
//     own chroma grain untouched).
//
// Every constant is mathematically or physically derived and documented inline (Rule 1).
// Every stand-in / fallback is visibly recorded in StandInRegistry and result counters (Rule 4).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/LumaWeights.h>
#include <VP-V/core/Ramp.h>
#include <VP-V/modules/motion/estimation/composed/WarpSampler.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TdV2
{
	// ---- Rule-1 Derived Chroma Constants ----

	// Unidentified coded-grid coarse-band sigma in luma pixels: 0.5 * kCanonicalBlockPx = 8.0 luma px
	// (half a canonical macroblock, strictly below any 4:2:0 / 4:2:2 chroma Nyquist; plan s1.5 #1).
	constexpr double kUnidentifiedChromaSigmaLumaPx = 0.5 * static_cast<double>(kCanonicalBlockPx);

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
			if (dCb == 0.0 && dCr == 0.0)
			{
				outR = r;
				outG = g;
				outB = b;
				return true;
			}
			if (!std::isfinite(dCb) || !std::isfinite(dCr) || !(kg > kEngineNumFloor))
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
			outR = r + dR;
			outG = g + dG;
			outB = b + dB;
			return std::isfinite(outR) && std::isfinite(outG) && std::isfinite(outB);
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
			if (VpComposed::SampleBicubic(view, sxLocal, syLocal, outVal) && std::isfinite(outVal))
				return true;
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
			StandInRegistry* standIns = nullptr)
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
						// Out-of-bounds landing: fill with edge-clamped active sample (zero ringing), outValid = 0.
						outWarped.at(x, y) = SampleBilinearClampedLocal(view, sxLocal, syLocal);
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
			active_ = {};
			originOffX_ = 0.0;
			originOffY_ = 0.0;
			sumCb_ = TdV2Plane();
			sumCr_ = TdV2Plane();
			weight_ = TdV2Plane();
			anchorCleanY_ = TdV2Plane();
			cflStats_.clear();
			cflCells_.clear();
			accumulatedFrames_ = 0;
			hasData_ = false;
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
		// into the full luma-resolution (lumaW x lumaH) 4:4:4 memory canvas.
		// Splat convention: `splatMotion` (vx, vy) gives the anchor frame's displacement from the canvas
		// origin (x_frame = x_canvas + v), so coded sample at luma (x_luma, y_luma) splats onto canvas
		// coordinate (x_luma - vx, y_luma - vy) strictly inside `lumaActive`. NEVER pass grainy chroma!
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
			StandInRegistry* standIns = nullptr)
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

			// Fractional siting origin offset so coded samples align to exact integer canvas lattice sites
			// for TopLeftType2 (0, 0), Left (0, 0.5), and Center (0.5, 0.5), eliminating half-pixel box blur
			// on static/shifted anchors and preventing odd-dimension edge row/column drops.
			const double baseX = siting.CodedToLumaX(0.0, cSubX);
			const double baseY = siting.CodedToLumaY(0.0, cSubY);
			const double offX = baseX - std::floor(baseX);
			const double offY = baseY - std::floor(baseY);

			// Re-initialize canvas if dimensions, cell grid, ActiveRect, or siting origin changed.
			if (!hasData_ || w_ != lumaW || h_ != lumaH || cellsW_ != cw || cellsH_ != ch || cellPx_ != cellPx ||
			    active_.x0 != ar.x0 || active_.y0 != ar.y0 || active_.x1 != ar.x1 || active_.y1 != ar.y1 ||
			    std::fabs(originOffX_ - offX) > kEngineNumFloor || std::fabs(originOffY_ - offY) > kEngineNumFloor)
			{
				Reset();
				w_ = lumaW;
				h_ = lumaH;
				cellsW_ = cw;
				cellsH_ = ch;
				cellPx_ = cellPx;
				active_ = ar;
				originOffX_ = offX;
				originOffY_ = offY;
				sumCb_ = TdV2Plane(w_, h_, 0.0);
				sumCr_ = TdV2Plane(w_, h_, 0.0);
				weight_ = TdV2Plane(w_, h_, 0.0);
				cflStats_.assign(numCells, CflAccum{});
				cflCells_.assign(numCells, AnchorCflCell{});
			}

			const bool useMotion = (splatMotion != nullptr && splatMotion->IsWellFormed(cw, ch));
			if (splatMotion != nullptr && !useMotion && standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaAnchorCanvas malformed splatMotion");
			else if (splatMotion == nullptr && accumulatedFrames_ > 0 && standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "anchor splatMotion null; assuming Still");

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
						if (anchorYCleanFinite)
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

			if (anyNonFinite && standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaAnchorCanvas non-finite sample");
			if (anyBorderBilinear && standIns != nullptr)
				standIns->Engage(StandInKind::BorderBilinearFallback, "TdV2ChromaAnchorCanvas CfL luma border");

			if (splattedSamples > 0)
			{
				hasData_ = true;
				++accumulatedFrames_;
			}

			// Store first valid anchor clean luma as the reference-aligned P_Y source for H5 carried CfL.
			// (If subsequent near-I anchor frames arrive, retain the origin-aligned clean luma matching canvas coords.)
			if (anchorYCleanFinite && anchorCleanY_.Empty())
				anchorCleanY_ = *anchorCleanY;

			// Refit per-cell OLS CfL (aCb, bCb, aCr, bCr) with derived half-LSB variance ridge floor (Rule 1).
			if (anchorYCleanFinite)
			{
				const double depthScale = DepthScale(sourceBits);
				// Exact half-LSB quantization variance: (0.5 * depthScale)^2 = (depthScale * depthScale) / 4.0.
				const double halfLsbVar = (depthScale * depthScale) / 4.0;
				const double ridgeFloor = (std::max)(halfLsbVar, kEngineNumFloor);
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
		// Populated pixels keep exact (sum / weight); any unpopulated gap pixels (e.g., before shifted
		// sub-pixel frames fill odd 4:2:0 columns) are smoothly interpolated from populated neighbors so
		// bicubic sampling never sees 0.0-holes while accumulated sub-pixel sites stay exact.
		bool MaterializeCanvas(TdV2Plane& outCb, TdV2Plane& outCr) const
		{
			if (!hasData_ || w_ <= 0 || h_ <= 0 || active_.Empty())
				return false;
			outCb = TdV2Plane(w_, h_, 0.0);
			outCr = TdV2Plane(w_, h_, 0.0);
			std::vector<uint8_t> populated(static_cast<size_t>(w_) * static_cast<size_t>(h_), 0u);

			size_t gapCount = 0;
			for (int y = active_.y0; y < active_.y1; ++y)
			{
				for (int x = active_.x0; x < active_.x1; ++x)
				{
					const double wt = weight_.at(x, y);
					if (wt > kEngineNumFloor)
					{
						const double invW = 1.0 / wt;
						outCb.at(x, y) = sumCb_.at(x, y) * invW;
						outCr.at(x, y) = sumCr_.at(x, y) * invW;
						populated[static_cast<size_t>(y) * w_ + x] = 1u;
					}
					else
					{
						++gapCount;
					}
				}
			}

			if (gapCount == 0)
				return true;

			// Pass 1: Horizontal linear gap-fill along each active row between nearest populated columns.
			for (int y = active_.y0; y < active_.y1; ++y)
			{
				int lastPop = -1;
				for (int x = active_.x0; x < active_.x1; ++x)
				{
					if (populated[static_cast<size_t>(y) * w_ + x] == 0u)
						continue;
					if (lastPop < 0)
					{
						for (int gx = active_.x0; gx < x; ++gx)
						{
							outCb.at(gx, y) = outCb.at(x, y);
							outCr.at(gx, y) = outCr.at(x, y);
							populated[static_cast<size_t>(y) * w_ + gx] = 1u;
						}
					}
					else if (x > lastPop + 1)
					{
						const double span = static_cast<double>(x - lastPop);
						const double cb0 = outCb.at(lastPop, y), cb1 = outCb.at(x, y);
						const double cr0 = outCr.at(lastPop, y), cr1 = outCr.at(x, y);
						for (int gx = lastPop + 1; gx < x; ++gx)
						{
							const double t = static_cast<double>(gx - lastPop) / span;
							outCb.at(gx, y) = RampMath::Mix(cb0, cb1, t);
							outCr.at(gx, y) = RampMath::Mix(cr0, cr1, t);
							populated[static_cast<size_t>(y) * w_ + gx] = 1u;
						}
					}
					lastPop = x;
				}
				if (lastPop >= 0)
				{
					for (int gx = lastPop + 1; gx < active_.x1; ++gx)
					{
						outCb.at(gx, y) = outCb.at(lastPop, y);
						outCr.at(gx, y) = outCr.at(lastPop, y);
						populated[static_cast<size_t>(y) * w_ + gx] = 1u;
					}
				}
			}

			// Pass 2: Vertical linear gap-fill along each active column for any empty rows (e.g. odd rows on 1-frame Type2 4:2:0).
			for (int x = active_.x0; x < active_.x1; ++x)
			{
				int lastPop = -1;
				for (int y = active_.y0; y < active_.y1; ++y)
				{
					if (populated[static_cast<size_t>(y) * w_ + x] == 0u)
						continue;
					if (lastPop < 0)
					{
						for (int gy = active_.y0; gy < y; ++gy)
						{
							outCb.at(x, gy) = outCb.at(x, y);
							outCr.at(x, gy) = outCr.at(x, y);
							populated[static_cast<size_t>(gy) * w_ + x] = 1u;
						}
					}
					else if (y > lastPop + 1)
					{
						const double span = static_cast<double>(y - lastPop);
						const double cb0 = outCb.at(x, lastPop), cb1 = outCb.at(x, y);
						const double cr0 = outCr.at(x, lastPop), cr1 = outCr.at(x, y);
						for (int gy = lastPop + 1; gy < y; ++gy)
						{
							const double t = static_cast<double>(gy - lastPop) / span;
							outCb.at(x, gy) = RampMath::Mix(cb0, cb1, t);
							outCr.at(x, gy) = RampMath::Mix(cr0, cr1, t);
							populated[static_cast<size_t>(gy) * w_ + x] = 1u;
						}
					}
					lastPop = y;
				}
				if (lastPop >= 0)
				{
					for (int gy = lastPop + 1; gy < active_.y1; ++gy)
					{
						outCb.at(x, gy) = outCb.at(x, lastPop);
						outCr.at(x, gy) = outCr.at(x, lastPop);
						populated[static_cast<size_t>(gy) * w_ + x] = 1u;
					}
				}
			}
			return true;
		}

		// Sample the accumulated 4:4:4 canvas at coded-grid coordinates mapped to (x_luma(cx) + vx, y_luma(cy) + vy)
		// strictly inside `lumaActive`. `lumaMotion` is the Current -> Anchor luma motion field (null => Still 0,0).
		bool SampleAtCodedGrid(
			ChromaSubsampling codedSub,
			const SitingSpec& siting,
			const ActiveRect& lumaActive,
			const CellMotionField* lumaMotion,
			int cellPx,
			TdV2Plane& outCb,
			TdV2Plane& outCr,
			std::vector<uint8_t>& outValid,
			StandInRegistry* standIns = nullptr) const
		{
			if (!hasData_ || w_ <= 0 || h_ <= 0 || cellPx <= 0)
				return false;

			TdV2Plane canvasCb, canvasCr;
			if (!MaterializeCanvas(canvasCb, canvasCr))
				return false;

			const ActiveRect ar = lumaActive.Empty() ? ActiveRect::Full(w_, h_) : lumaActive.ClampedTo(w_, h_);
			const int cSubX = SubsampleX(codedSub);
			const int cSubY = SubsampleY(codedSub);
			const int cW = ChromaDetail::SubsampledDim(w_, cSubX);
			const int cH = ChromaDetail::SubsampledDim(h_, cSubY);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(ar, w_, h_, cSubX, cSubY, cW, cH);

			outCb = TdV2Plane(cW, cH, 0.0);
			outCr = TdV2Plane(cW, cH, 0.0);
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

			for (int cy = cActive.y0; cy < cActive.y1; ++cy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(cy), cSubY);
				const int mcy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ly)) / cellPx));
				for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(cx), cSubX);
					const int mcx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(lx)) / cellPx));
					const size_t cIdx = static_cast<size_t>(mcy) * cw + mcx;

					double vx = 0.0, vy = 0.0;
					if (useMotion)
					{
						if (lumaMotion->valid[cIdx] == 0u)
							continue;
						vx = lumaMotion->vx[cIdx];
						vy = lumaMotion->vy[cIdx];
						if (!std::isfinite(vx) || !std::isfinite(vy))
						{
							anyNonFinite = true;
							continue;
						}
					}

					const double sxLocal = ((lx + vx) - originOffX_) - static_cast<double>(ar.x0);
					const double syLocal = ((ly + vy) - originOffY_) - static_cast<double>(ar.y0);
					double cbVal = 0.0, crVal = 0.0;
					bool bCb = false, bCr = false;
					if (ChromaDetail::SampleSitedActive(cbView, sxLocal, syLocal, cbVal, bCb, /*allowEdgeClampOutside=*/false) &&
					    ChromaDetail::SampleSitedActive(crView, sxLocal, syLocal, crVal, bCr, /*allowEdgeClampOutside=*/false))
					{
						outCb.at(cx, cy) = cbVal;
						outCr.at(cx, cy) = crVal;
						outValid[static_cast<size_t>(cy) * cW + cx] = 1u;
						if (bCb || bCr)
							anyBorder = true;
					}
					else
					{
						// Fill edge-clamped sample to prevent 0.0-hole ringing while keeping outValid == 0.
						outCb.at(cx, cy) = ChromaDetail::SampleBilinearClampedLocal(cbView, sxLocal, syLocal);
						outCr.at(cx, cy) = ChromaDetail::SampleBilinearClampedLocal(crView, sxLocal, syLocal);
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
		bool PredictCarriedCflAtCodedGrid(
			ChromaSubsampling codedSub,
			const SitingSpec& siting,
			const ActiveRect& lumaActive,
			const CellMotionField* lumaMotion,
			int cellPx,
			TdV2Plane& outCb,
			TdV2Plane& outCr,
			std::vector<uint8_t>& outValid,
			StandInRegistry* standIns = nullptr) const
		{
			if (!hasCfl_ || anchorCleanY_.Empty() || w_ <= 0 || h_ <= 0 || cellPx <= 0)
				return false;

			const ActiveRect ar = lumaActive.Empty() ? ActiveRect::Full(w_, h_) : lumaActive.ClampedTo(w_, h_);
			const int cSubX = SubsampleX(codedSub);
			const int cSubY = SubsampleY(codedSub);
			const int cW = ChromaDetail::SubsampledDim(w_, cSubX);
			const int cH = ChromaDetail::SubsampledDim(h_, cSubY);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(ar, w_, h_, cSubX, cSubY, cW, cH);

			outCb = TdV2Plane(cW, cH, 0.0);
			outCr = TdV2Plane(cW, cH, 0.0);
			outValid.assign(static_cast<size_t>(cW) * static_cast<size_t>(cH), 0u);
			if (ar.Empty() || cActive.Empty())
				return false;

			const int cw = (w_ + cellPx - 1) / cellPx;
			const int ch = (h_ + cellPx - 1) / cellPx;
			const bool useMotion = (lumaMotion != nullptr && lumaMotion->IsWellFormed(cw, ch));
			if (lumaMotion != nullptr && !useMotion && standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaAnchorCanvas malformed CfL motion");

			ActiveSubPlaneView yView{ &anchorCleanY_, ar.x0, ar.y0, ar.Width(), ar.Height() };
			bool anyBorder = false;
			bool anyNonFinite = false;

			for (int cy = cActive.y0; cy < cActive.y1; ++cy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(cy), cSubY);
				const int mcy = (std::max)(0, (std::min)(ch - 1, static_cast<int>(std::floor(ly)) / cellPx));
				for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(cx), cSubX);
					const int mcx = (std::max)(0, (std::min)(cw - 1, static_cast<int>(std::floor(lx)) / cellPx));
					const size_t cIdx = static_cast<size_t>(mcy) * cw + mcx;

					double vx = 0.0, vy = 0.0;
					if (useMotion)
					{
						if (lumaMotion->valid[cIdx] == 0u)
							continue;
						vx = lumaMotion->vx[cIdx];
						vy = lumaMotion->vy[cIdx];
						if (!std::isfinite(vx) || !std::isfinite(vy))
						{
							anyNonFinite = true;
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
						continue;

					// Allow exact <= 0.5 px siting margin on subsampled axes at active edges (clamped strictly inside ar,
					// never crossing bar boundaries) so odd-dimension last coded chroma row/col is preserved.
					const double maxLocalX = static_cast<double>(ar.Width() - 1);
					const double maxLocalY = static_cast<double>(ar.Height() - 1);
					const double marginX = (cSubX > 1) ? 0.5 : 0.0;
					const double marginY = (cSubY > 1) ? 0.5 : 0.0;

					double pyVal = 0.0;
					bool borderBilin = false;
					bool sampledOk = ChromaDetail::SampleSitedActive(yView, sxLocal, syLocal, pyVal, borderBilin, /*allowEdgeClampOutside=*/false);
					if (!sampledOk &&
					    sxLocal >= -marginX && sxLocal <= maxLocalX + marginX &&
					    syLocal >= -marginY && syLocal <= maxLocalY + marginY)
					{
						const double sxClamped = (std::max)(0.0, (std::min)(maxLocalX, sxLocal));
						const double syClamped = (std::max)(0.0, (std::min)(maxLocalY, syLocal));
						sampledOk = ChromaDetail::SampleSitedActive(yView, sxClamped, syClamped, pyVal, borderBilin, /*allowEdgeClampOutside=*/true);
					}
					if (sampledOk)
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
						}
					}
					else
					{
						const double edgePy = ChromaDetail::SampleBilinearClampedLocal(yView, sxLocal, syLocal);
						outCb.at(cx, cy) = cfl.aCb * edgePy + cfl.bCb;
						outCr.at(cx, cy) = cfl.aCr * edgePy + cfl.bCr;
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
		ActiveRect active_{};
		double originOffX_ = 0.0;
		double originOffY_ = 0.0;
		TdV2Plane sumCb_;
		TdV2Plane sumCr_;
		TdV2Plane weight_;
		TdV2Plane anchorCleanY_;
		std::vector<CflAccum> cflStats_;
		std::vector<AnchorCflCell> cflCells_;
		size_t accumulatedFrames_ = 0;
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

		// Coding step prior (P16): when false, derives 1-LSB default with visible StandInKind::StepPriorDefault.
		bool hasStepPrior = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool transformIdentified8x8 = false;

		bool isCutOrSpanInvalid = false;              // Resets 4:4:4 anchor canvas cleanly
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
		size_t nonFiniteCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
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

		void SetConfig(const Config& cfg) { cfg_ = cfg; }
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

			// Mode::Off -> zero work, bit-exact passthrough!
			if (!cfg_.IsEnabled())
				return res;

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

			// Coding step prior (shared by both Identified and Unidentified branches).
			double effStepDelta = in.estimatedStepDelta;
			double effZeroFrac = in.zeroCoeffFraction;
			if (!in.hasStepPrior || !std::isfinite(effStepDelta) || effStepDelta <= 0.0 || !std::isfinite(effZeroFrac))
			{
				effStepDelta = DepthScale(cfg_.sourceBits);
				effZeroFrac = 0.0;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::StepPriorDefault);
			}
			const RoundingCellStats a2Stats = ComputeRoundingCellStats(effStepDelta, in.deadZonePhi, effZeroFrac);

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
				}
				else
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::P10gGrainSeparator, "chroma coarse: no upstream chroma split");
				}

				const double sigmaX = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubX);
				const double sigmaY = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(dSubY);
				double coarseGain = 1.0;
				const TdV2Plane curCoarseCb = ChromaDetail::BlurActiveAnisotropic(curCleanCb, delivActive, sigmaX, sigmaY, coarseGain);
				const TdV2Plane curCoarseCr = ChromaDetail::BlurActiveAnisotropic(curCleanCr, delivActive, sigmaX, sigmaY, coarseGain);

				struct CoarseRef
				{
					TdV2Plane coarseCb;
					TdV2Plane coarseCr;
					std::vector<uint8_t> validPx;
					MotionGateResult motionGate;
					bool isExactCopy = false;
				};
				std::vector<CoarseRef> cRefs;
				cRefs.reserve(2);

				auto tryAddCoarseRef = [&](const ChromaReferenceInput& rIn)
				{
					if (rIn.cleanCb == nullptr || rIn.cleanCr == nullptr ||
					    rIn.cleanCb->w != dW || rIn.cleanCb->h != dH ||
					    rIn.cleanCr->w != dW || rIn.cleanCr->h != dH)
						return;

					CellMotionField fallbackMf;
					const CellMotionField* useMf = rIn.lumaMotion;
					if (useMf == nullptr)
					{
						++res.motionStandInRefs;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::MotionFieldStandIn, "chroma coarse null motion; assuming Still");
						fallbackMf = CellMotionField::Uniform(res.cellsW, res.cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
						useMf = &fallbackMf;
					}
					else if (!useMf->IsWellFormed(res.cellsW, res.cellsH))
					{
						++res.motionStandInRefs;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::MotionFieldStandIn, "chroma coarse mismatched motion; ref rejected");
						return;
					}

					CoarseRef cr;
					cr.isExactCopy = rIn.isExactCopyOfCurrent;
					TdV2Plane warpedCb, warpedCr;
					std::vector<uint8_t> validCb, validCr;
					const WarpDiagnostics dCb = ChromaDetail::WarpChromaPlaneWithLumaMotion(
						*rIn.cleanCb, curCleanCb, delivActive, dSubX, dSubY, in.siting,
						useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, warpedCb, validCb, standIns);
					const WarpDiagnostics dCr = ChromaDetail::WarpChromaPlaneWithLumaMotion(
						*rIn.cleanCr, curCleanCr, delivActive, dSubX, dSubY, in.siting,
						useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, warpedCr, validCr, standIns);
					res.borderBilinearPixels += dCb.borderBilinearPixels + dCr.borderBilinearPixels;
					if (dCb.nonFinitePixels > 0 || dCr.nonFinitePixels > 0)
						++res.nonFiniteCells;
					cr.validPx.resize(validCb.size(), 0u);
					for (size_t i = 0; i < validCb.size(); ++i)
						cr.validPx[i] = (validCb[i] != 0u && validCr[i] != 0u) ? 1u : 0u;
					double dummyGain = 1.0;
					cr.coarseCb = ChromaDetail::BlurActiveAnisotropic(warpedCb, delivActive, sigmaX, sigmaY, dummyGain);
					cr.coarseCr = ChromaDetail::BlurActiveAnisotropic(warpedCr, delivActive, sigmaX, sigmaY, dummyGain);
					cr.motionGate = EvaluateCameraRelativeMotionGate(*useMf, standIns);
					res.nonFiniteCells += cr.motionGate.nonFiniteCells;
					cRefs.push_back(std::move(cr));
				};

				tryAddCoarseRef(in.prevPeak);
				tryAddCoarseRef(in.nextPeak);
				res.activeRefCount = cRefs.size();
				if (cRefs.empty())
					return res;

				auto fuseCoarseSinglePlane = [&](
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

					for (int cy = 0; cy < res.cellsH; ++cy)
					{
						for (int cx = 0; cx < res.cellsW; ++cx)
						{
							const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
							double cellGrainVar = 0.0;
							if (!retainedGrainVar.empty())
							{
								const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
								const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
								const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
								if (gIdx < retainedGrainVar.size())
								{
									const double gv = retainedGrainVar[gIdx];
									if (std::isfinite(gv) && gv >= 0.0)
										cellGrainVar = gv;
									else
									{
										++res.nonFiniteCells;
										if (standIns != nullptr)
											standIns->Engage(StandInKind::NonFiniteClosure, "chroma coarse non-finite retainedGrainVar");
									}
								}
							}

							MomentAccumulatorFp32 acc(K);
							const int py0 = (std::max)(delivActive.y0, ((cy - poolR) * cellPx) / dSubY);
							const int py1 = (std::min)(delivActive.y1, ((cy + poolR + 1) * cellPx + dSubY - 1) / dSubY);
							const int px0 = (std::max)(delivActive.x0, ((cx - poolR) * cellPx) / dSubX);
							const int px1 = (std::min)(delivActive.x1, ((cx + poolR + 1) * cellPx + dSubX - 1) / dSubX);

							for (int y = py0; y < py1; ++y)
							{
								const double ly = in.siting.CodedToLumaY(static_cast<double>(y), dSubY);
								const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
								for (int x = px0; x < px1; ++x)
								{
									const double lx = in.siting.CodedToLumaX(static_cast<double>(x), dSubX);
									const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
									const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
									const size_t pIdx = static_cast<size_t>(y) * dW + x;

									bool allValid = true;
									float diffs[kMaxBlendRefs] = {};
									const double curV = curCoarse.at(x, y);
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
										acc.AddSample(diffs);
								}
							}

							if (acc.sampleCount == 0)
								continue;

							double C[kMaxBlendRefs][kMaxBlendRefs] = {};
							acc.SecondMoment(C);
							for (int k = 0; k < K; ++k)
								cellDiagC[k][cIdx] = C[k][k];

							double cVec[kMaxBlendRefs] = {};
							for (int k = 0; k < K; ++k)
							{
								// Unit-sum low-pass DC transfer gain = (sum G_sigma)^2 = 1.0 (Rule 1):
								// Spatially coherent low-frequency chroma banding / macroblock DC quantization passes
								// through G(sigma) with DC gain 1.0, whereas spatially uncorrelated white grain passes
								// with ||G(sigma)||_2^2 = coarseGain (used strictly in EstimateC_A1 below).
								const double cA2 = EstimateC_A2(a2Stats, cRefs[k].isExactCopy);
								double cA1 = cA2;
								if (K >= 2)
								{
									const int partner = (k == 0) ? 1 : 0;
									cA1 = EstimateC_A1(C[k][partner], C[k][k], C[partner][partner], cellGrainVar, coarseGain);
								}
								else if (cfg_.estimator != VarianceEstimator::A2_RoundingCells)
								{
									++res.singleRefA2Cells;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
								}
								cVec[k] = SelectBlindC(cfg_.estimator, cA1, cA2);
								if (cRefs[k].isExactCopy)
									cVec[k] = 0.0;
							}

							const BlendSolveResult solved = SolveConvexWeights(C, cVec, K, kDefaultTraceRegScale, standIns);
							if (solved.nonFiniteClosed) { ++res.nonFiniteCells; continue; }
							if (solved.singular) { ++res.singularCells; continue; }

							for (int k = 0; k < K; ++k)
								cellW[k][cIdx] = solved.weights[k] * cRefs[k].motionGate.gateL[cIdx];
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

							const double curV = curCoarse.at(x, y);
							double delta = 0.0;
							for (int k = 0; k < K; ++k)
							{
								if (cRefs[k].validPx[pIdx] == 0u || cellW[k][cIdx] <= 0.0)
									continue;
								const double wSpat = SpatialBilinearWeight(cellW[k].data(), res.cellsW, res.cellsH, lx, ly, cellPx);
								if (wSpat <= 0.0)
									continue;
								const TdV2Plane& refP = isCb ? cRefs[k].coarseCb : cRefs[k].coarseCr;
								const double diff = curV - refP.at(x, y);
								const double taper = HuberPixelTaper(diff, cellDiagC[k][cIdx]);
								delta -= (wSpat * taper) * diff;
							}
							if (std::fabs(delta) > 0.0)
								delta = ApplyTransformUnknownBound(delta, bandVar, standIns);
							outDelta.at(x, y) = (delta == 0.0) ? 0.0 : delta;
						}
					}
				};

				fuseCoarseSinglePlane(curCoarseCb, true, in.retainedGrainVarCb, res.deliveredDeltaCb);
				fuseCoarseSinglePlane(curCoarseCr, false, in.retainedGrainVarCr, res.deliveredDeltaCr);
				res.codedDeltaCb = res.deliveredDeltaCb;
				res.codedDeltaCr = res.deliveredDeltaCr;

				if (cfg_.ModifiesPixels())
					ApplyDeliveredDelta(in, delivActive, res.deliveredDeltaCb, res.deliveredDeltaCr, res, standIns);
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
			}
			else
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::P10gGrainSeparator, "chroma: no upstream P10g chroma split supplied");
				codedCleanCb = codedRecCb;
				codedCleanCr = codedRecCr;
			}

			// Helper to pick grain-free anchor luma for anchor ingestion (NEVER feed grainy luma when clean split active!).
			auto resolveAnchorCleanY = [&]() -> const TdV2Plane*
			{
				if (!in.anchorCleanY.Empty() && in.anchorCleanY.w == lumaW && in.anchorCleanY.h == lumaH)
					return &in.anchorCleanY;
				if (!in.hasCleanSplit)
				{
					if (!in.receivedY.Empty() && in.receivedY.w == lumaW && in.receivedY.h == lumaH)
						return &in.receivedY;
					if (isRgb && !rgbDerivedY.Empty())
						return &rgbDerivedY;
				}
				return nullptr;
			};

			// On cut / span-invalid frames, never blend across the cut; seed new shot's anchor canvas if isAnchorFrame.
			if (in.isCutOrSpanInvalid)
			{
				if (in.isAnchorFrame)
				{
					anchorCanvas_.AccumulateAnchorFrame(
						codedCleanCb, codedCleanCr, resolveAnchorCleanY(),
						lumaW, lumaH, in.codedSubsampling, in.siting, lumaActive,
						in.hasAnchorSplatMotion ? &in.anchorSplatMotion : nullptr,
						cellPx, cfg_.sourceBits, standIns);
				}
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
				MotionGateResult motionGate;
				bool isExactCopy = false;
				bool isCfl = false;
				bool isAnchorDerived = false;
			};
			std::vector<WarpedChromaRef> refs;
			refs.reserve(kMaxBlendRefs);

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

				CellMotionField fallbackMf;
				const CellMotionField* useMf = rIn.lumaMotion;
				if (useMf == nullptr)
				{
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "chroma null motion field; assuming Still");
					fallbackMf = CellMotionField::Uniform(res.cellsW, res.cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					useMf = &fallbackMf;
				}
				else if (!useMf->IsWellFormed(res.cellsW, res.cellsH))
				{
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "chroma mismatched motion field; ref rejected");
					return;
				}

				WarpedChromaRef wr;
				wr.isExactCopy = rIn.isExactCopyOfCurrent;
				std::vector<uint8_t> validCb, validCr;
				const WarpDiagnostics dCb = ChromaDetail::WarpChromaPlaneWithLumaMotion(
					*refCb, codedCleanCb, codedActive, cSubX, cSubY, in.siting,
					useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, wr.cleanCb, validCb, standIns);
				const WarpDiagnostics dCr = ChromaDetail::WarpChromaPlaneWithLumaMotion(
					*refCr, codedCleanCr, codedActive, cSubX, cSubY, in.siting,
					useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, wr.cleanCr, validCr, standIns);
				res.borderBilinearPixels += dCb.borderBilinearPixels + dCr.borderBilinearPixels;
				if (dCb.nonFinitePixels > 0 || dCr.nonFinitePixels > 0)
					++res.nonFiniteCells;
				wr.validPx.resize(validCb.size(), 0u);
				for (size_t i = 0; i < validCb.size(); ++i)
					wr.validPx[i] = (validCb[i] != 0u && validCr[i] != 0u) ? 1u : 0u;
				wr.bandsCb = DecomposeActiveBands(wr.cleanCb, codedActive, standIns);
				wr.bandsCr = DecomposeActiveBands(wr.cleanCr, codedActive, standIns);
				wr.motionGate = EvaluateCameraRelativeMotionGate(*useMf, standIns);
				res.nonFiniteCells += wr.motionGate.nonFiniteCells;
				refs.push_back(std::move(wr));
			};

			tryAddPeakRef(in.prevPeak);
			tryAddPeakRef(in.nextPeak);

			// Anchor read motion field for 4:4:4 canvas and carried CfL (H5).
			CellMotionField anchorReadFallback;
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
			else if (anchorCanvas_.HasData() && (in.enableAnchorCanvasRef || in.enableCarriedCfl))
			{
				++res.motionStandInRefs;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::MotionFieldStandIn, "chroma anchorReadMotion missing; assuming Still");
				anchorReadFallback = CellMotionField::Uniform(res.cellsW, res.cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				anchorReadMf = &anchorReadFallback;
			}

			// 4:4:4 Anchor Memory Canvas reference.
			if (!anchorMotionRejected && in.enableAnchorCanvasRef && anchorCanvas_.HasData() &&
			    refs.size() < static_cast<size_t>(kMaxBlendRefs))
			{
				WarpedChromaRef wr;
				wr.isAnchorDerived = true;
				if (anchorCanvas_.SampleAtCodedGrid(
						in.codedSubsampling, in.siting, lumaActive, anchorReadMf, cellPx,
						wr.cleanCb, wr.cleanCr, wr.validPx, standIns))
				{
					wr.bandsCb = DecomposeActiveBands(wr.cleanCb, codedActive, standIns);
					wr.bandsCr = DecomposeActiveBands(wr.cleanCr, codedActive, standIns);
					wr.motionGate = EvaluateCameraRelativeMotionGate(*anchorReadMf, standIns);
					res.nonFiniteCells += wr.motionGate.nonFiniteCells;
					refs.push_back(std::move(wr));
					res.anchorCanvasUsed = true;
				}
			}

			// Carried CfL (H5) reference: strictly applied to P_Y = Warp(anchorCleanY), NEVER current luma!
			int cflRefIdx = -1;
			if (!anchorMotionRejected && in.enableCarriedCfl && anchorCanvas_.HasCfl() &&
			    refs.size() < static_cast<size_t>(kMaxBlendRefs))
			{
				WarpedChromaRef wr;
				wr.isCfl = true;
				wr.isAnchorDerived = true;
				if (anchorCanvas_.PredictCarriedCflAtCodedGrid(
						in.codedSubsampling, in.siting, lumaActive, anchorReadMf, cellPx,
						wr.cleanCb, wr.cleanCr, wr.validPx, standIns))
				{
					wr.bandsCb = DecomposeActiveBands(wr.cleanCb, codedActive, standIns);
					wr.bandsCr = DecomposeActiveBands(wr.cleanCr, codedActive, standIns);
					wr.motionGate = EvaluateCameraRelativeMotionGate(*anchorReadMf, standIns);
					res.nonFiniteCells += wr.motionGate.nonFiniteCells;
					cflRefIdx = static_cast<int>(refs.size());
					refs.push_back(std::move(wr));
					res.carriedCflUsed = true;
				}
			}

			res.activeRefCount = refs.size();
			if (refs.empty())
			{
				// Still ingest anchor frame into 4:4:4 canvas if this is an anchor frame!
				if (in.isAnchorFrame)
				{
					anchorCanvas_.AccumulateAnchorFrame(
						codedCleanCb, codedCleanCr, resolveAnchorCleanY(),
						lumaW, lumaH, in.codedSubsampling, in.siting, lumaActive,
						in.hasAnchorSplatMotion ? &in.anchorSplatMotion : nullptr,
						cellPx, cfg_.sourceBits, standIns);
				}
				return res;
			}

			// B5: Per-cell Carried CfL disagreement check (kHuberKneeSq = 9.0 * max(cellRefDiffVar, stepTotalVar)).
			// If chroma residual disagrees with luma-derived CfL prediction on a cell, reject CfL on that cell visibly!
			std::vector<uint8_t> cflRejectCb(numCells, 0u);
			std::vector<uint8_t> cflRejectCr(numCells, 0u);
			const double stepVar = a2Stats.valid ? a2Stats.totalVar : 0.0;

			if (cflRefIdx >= 0)
			{
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
							++res.cflFallbackCells;
							if (standIns != nullptr)
								standIns->Engage(StandInKind::ChromaCflFallback);
						}
					}
				}
			}

			// B6: Per-band fusion on coded Cb and coded Cr independently using shared TdV2BlendMath + Layer-4 TdGuardPort.
			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double bandTransfer[3] = { gains.low, gains.mid, gains.fine };

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

				for (int cy = 0; cy < res.cellsH; ++cy)
				{
					for (int cx = 0; cx < res.cellsW; ++cx)
					{
						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;

						double cellGrainVar = 0.0;
						if (!retainedGrainVar.empty())
						{
							const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
							const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
							const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
							if (gIdx < retainedGrainVar.size())
							{
								const double gv = retainedGrainVar[gIdx];
								if (std::isfinite(gv) && gv >= 0.0)
									cellGrainVar = gv;
								else
								{
									++res.nonFiniteCells;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::NonFiniteClosure, "chroma non-finite retainedGrainVar");
								}
							}
						}
						cellRetainedGrain[cIdx] = cellGrainVar;

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
							continue;

						const int py0 = (std::max)(codedActive.y0, ((cy - poolR) * cellPx) / cSubY);
						const int py1 = (std::min)(codedActive.y1, ((cy + poolR + 1) * cellPx + cSubY - 1) / cSubY);
						const int px0 = (std::max)(codedActive.x0, ((cx - poolR) * cellPx) / cSubX);
						const int px1 = (std::min)(codedActive.x1, ((cx + poolR + 1) * cellPx + cSubX - 1) / cSubX);

						for (int b = 0; b < 3; ++b)
						{
							const TdV2Plane& curB = bandPlaneOf(curBands, b);
							MomentAccumulatorFp32 acc(activeK);

							for (int y = py0; y < py1; ++y)
							{
								const double ly = in.siting.CodedToLumaY(static_cast<double>(y), cSubY);
								const int pcy = (std::max)(0, (std::min)(res.cellsH - 1, static_cast<int>(std::floor(ly)) / cellPx));
								for (int x = px0; x < px1; ++x)
								{
									const double lx = in.siting.CodedToLumaX(static_cast<double>(x), cSubX);
									const int pcx = (std::max)(0, (std::min)(res.cellsW - 1, static_cast<int>(std::floor(lx)) / cellPx));
									const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
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
										acc.AddSample(diffs);
								}
							}

							if (acc.sampleCount == 0)
								continue;

							double C[kMaxBlendRefs][kMaxBlendRefs] = {};
							acc.SecondMoment(C);
							for (int ai = 0; ai < activeK; ++ai)
								cellDiagC[b][activeIdx[ai]][cIdx] = C[ai][ai];

							double cVec[kMaxBlendRefs] = {};
							for (int ai = 0; ai < activeK; ++ai)
							{
								const int k = activeIdx[ai];
								const double cA2 = EstimateC_A2(a2Stats, refs[static_cast<size_t>(k)].isExactCopy) * bandTransfer[b];
								double cA1 = cA2;
								if (activeK >= 2)
								{
									// Prefer an independent partner ref: non-anchor peak ref (reverse scan prefers future peak),
									// or opposite isAnchorDerived class so anchor canvas & carried CfL never pair with each other
									// when an independent peak reference is available.
									int partner = (ai == 0) ? 1 : 0;
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
										if (selfAnchor != candAnchor)
											partner = cand;
									}
									cA1 = EstimateC_A1(C[ai][partner], C[ai][ai], C[partner][partner],
									                   cellGrainVar, bandTransfer[b]);
								}
								else if (cfg_.estimator != VarianceEstimator::A2_RoundingCells)
								{
									++res.singleRefA2Cells;
									if (standIns != nullptr)
										standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
								}
								cVec[ai] = SelectBlindC(cfg_.estimator, cA1, cA2);
								if (refs[static_cast<size_t>(k)].isExactCopy)
									cVec[ai] = 0.0;
							}

							const BlendSolveResult solved = SolveConvexWeights(C, cVec, activeK, kDefaultTraceRegScale, standIns);
							if (solved.nonFiniteClosed) { ++res.nonFiniteCells; continue; }
							if (solved.singular) { ++res.singularCells; continue; }

							for (int ai = 0; ai < activeK; ++ai)
							{
								const int k = activeIdx[ai];
								const WarpedChromaRef& wr = refs[static_cast<size_t>(k)];
								const double mGate = (b == 0) ? wr.motionGate.gateL[cIdx] : wr.motionGate.gateMH[cIdx];
								cellW[b][k][cIdx] = solved.weights[ai] * mGate;
							}
						}
					}
				}

				// Codec-frame-grid-aligned interior 8x8 DCT bounds on coded chroma plane (if transformIdentified8x8).
				const int bxStart = ((codedActive.x0 + 7) / 8) * 8;
				const int byStart = ((codedActive.y0 + 7) / 8) * 8;
				const int bxEnd = (codedActive.x1 / 8) * 8;
				const int byEnd = (codedActive.y1 / 8) * 8;

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

						const bool inFull8x8Block = in.transformIdentified8x8 && (bxStart < bxEnd) && (byStart < byEnd) &&
						                            (x >= bxStart) && (x < bxEnd) && (y >= byStart) && (y < byEnd);

						double totalDelta = 0.0;
						for (int b = 0; b < 3; ++b)
						{
							const double curV = bandPlaneOf(curBands, b).at(x, y);
							double bandDelta = 0.0;
							for (int k = 0; k < K; ++k)
							{
								if (refs[static_cast<size_t>(k)].validPx[pIdx] == 0u || cellW[b][k][cIdx] <= 0.0)
									continue;
								const double wSpat = SpatialBilinearWeight(cellW[b][k].data(), res.cellsW, res.cellsH, lx, ly, cellPx);
								if (wSpat <= 0.0)
									continue;
								const ThreeBandPlanes& rBands = isCb ? refs[static_cast<size_t>(k)].bandsCb : refs[static_cast<size_t>(k)].bandsCr;
								const double diff = curV - bandPlaneOf(rBands, b).at(x, y);
								const double taper = HuberPixelTaper(diff, cellDiagC[b][k][cIdx]);
								bandDelta -= (wSpat * taper) * diff;
							}
							if (!inFull8x8Block)
							{
								const double bandVar = a2Stats.valid ? (a2Stats.totalVar * bandTransfer[b]) : 0.0;
								if (std::fabs(bandDelta) > 0.0)
									bandDelta = ApplyTransformUnknownBound(bandDelta, bandVar, standIns);
							}
							totalDelta += bandDelta;
						}
						outCodedDelta.at(x, y) = (totalDelta == 0.0) ? 0.0 : totalDelta;
					}
				}

				// Identified 8x8 DCT STRICT/CELL POCS projection on coded chroma plane (codec-grid-aligned).
				if (in.transformIdentified8x8 && effStepDelta > 0.0 && (bxStart < bxEnd) && (byStart < byEnd))
				{
					const TdV2Plane& predClean = isCb ? refs[0].cleanCb : refs[0].cleanCr;
					const int iters = (std::max)(1, (std::min)(2, cfg_.cellProjIterations));
					for (int it = 0; it < iters; ++it)
					{
						for (int by = byStart; by < byEnd; by += 8)
						{
							for (int bx = bxStart; bx < bxEnd; bx += 8)
							{
								// Skip untouched / vetoed zero-delta 8x8 blocks so FP64 DCT round-trip stays bit-exact.
								bool anyNonZeroDelta = false;
								for (int j = 0; j < 8 && !anyNonZeroDelta; ++j)
									for (int i = 0; i < 8; ++i)
										if (outCodedDelta.at(bx + i, by + j) != 0.0) { anyNonZeroDelta = true; break; }
								if (!anyNonZeroDelta)
									continue;

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
								ProjectResidual8x8Dct(sentRes, blendRes, effStepDelta, in.deadZonePhi, cfg_.residualMode, projRes);
								for (int j = 0; j < 8; ++j)
								{
									for (int i = 0; i < 8; ++i)
									{
										const double cVal = codedClean.at(bx + i, by + j);
										const double pVal = predClean.at(bx + i, by + j);
										outCodedDelta.at(bx + i, by + j) = (pVal + projRes[j * 8 + i]) - cVal;
									}
								}
							}
						}
						for (int y = byStart; y < byEnd; ++y)
						{
							for (int x = bxStart; x < bxEnd; ++x)
							{
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

				// Layer 4: Shared Output Grain Guard (TdGuardPort) on output ONLY (never touching anchorCanvas_!).
				TdV2Plane candOut = codedRec;
				for (int y = codedActive.y0; y < codedActive.y1; ++y)
					for (int x = codedActive.x0; x < codedActive.x1; ++x)
						candOut.at(x, y) = codedClean.at(x, y) + outCodedDelta.at(x, y) + ownGrain.at(x, y);

				const ThreeBandPlanes inFullBands = DecomposeActiveBands(codedRec, codedActive, nullptr);
				const ThreeBandPlanes outFullBands = DecomposeActiveBands(candOut, codedActive, nullptr);
				const ThreeBandPlanes ownGrainBands = in.hasCleanSplit
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
						double ownGrainE = 0.0, ownGrainFineE = 0.0, inFineE = 0.0, outFineE = 0.0;
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
								if (in.hasCleanSplit)
								{
									const double ogf = ownGrainBands.H.at(x, y);
									ownGrainFineE += ogf * ogf;
								}
								inFineE += inf * inf;
								outFineE += outf * outf;
								++cnt;
							}
						}
						if (cnt == 0) continue;
						const double invC = 1.0 / static_cast<double>(cnt);
						if (ownGrainE * invC <= kEngineNumFloor && cellRetainedGrain[cIdx] <= kEngineNumFloor)
							continue;

						const double leakedFineE = cellRetainedGrain[cIdx] * gains.fine;
						const double targetFineE = in.hasCleanSplit
							? (ownGrainFineE * invC + leakedFineE)
							: (leakedFineE > kEngineNumFloor ? (std::min)(inFineE * invC, leakedFineE) : (inFineE * invC));

						const auto gDec = TdGuardPort::Evaluate(targetFineE, outFineE * invC, standIns);
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

			// B8: Apply delivered signed delta strictly inside ActiveRect (Mode::On only; Shadow stays passthrough).
			if (cfg_.ModifiesPixels())
				ApplyDeliveredDelta(in, delivActive, res.deliveredDeltaCb, res.deliveredDeltaCr, res, standIns);

			// B9: If this frame is an anchor frame, accumulate its grain-free coded chroma + clean luma into
			// the 4:4:4 anchor memory canvas (strictly grain-free, NEVER contaminated by Layer-4 output add-back!).
			if (in.isAnchorFrame)
			{
				anchorCanvas_.AccumulateAnchorFrame(
					codedCleanCb, codedCleanCr, resolveAnchorCleanY(),
					lumaW, lumaH, in.codedSubsampling, in.siting, lumaActive,
					in.hasAnchorSplatMotion ? &in.anchorSplatMotion : nullptr,
					cellPx, cfg_.sourceBits, standIns);
			}

			return res;
		}

	private:
		static void ApplyDeliveredDelta(
			const ChromaProcessInput& in,
			const ActiveRect& delivActive,
			const TdV2Plane& dCbPlane,
			const TdV2Plane& dCrPlane,
			ChromaProcessResult& res,
			StandInRegistry* standIns)
		{
			if (in.deliveredFormat == DeliveredChromaFormat::Rgb444)
			{
				bool anyRgbApplyFailed = false;
				for (int y = delivActive.y0; y < delivActive.y1; ++y)
				{
					for (int x = delivActive.x0; x < delivActive.x1; ++x)
					{
						if (!in.matrix.ApplySignedChromaDeltaToRgb(
								in.receivedR.at(x, y), in.receivedG.at(x, y), in.receivedB.at(x, y),
								dCbPlane.at(x, y), dCrPlane.at(x, y),
								res.outputR.at(x, y), res.outputG.at(x, y), res.outputB.at(x, y),
								standIns))
						{
							anyRgbApplyFailed = true;
						}
					}
				}
				if (anyRgbApplyFailed)
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
