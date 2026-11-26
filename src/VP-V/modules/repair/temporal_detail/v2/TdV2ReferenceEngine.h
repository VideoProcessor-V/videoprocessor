// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S3 (2026-11-23_TD-consolidated-plan.md s1.1-s1.6, s3.2):
// Full CPU Reference Fusion Engine with explicit 4-layer grain preservation:
//   - Dual-source grain-free fusion: prev peak + next peak + recurrent carry-forward in one w* = C^-1 c solve
//   - Shared bicubic resampling via VpComposed::SampleBicubic strictly inside ActiveRect (hard bar clamping,
//     edge-extended warp buffer with explicit out-of-bounds mask so zero-holes never ring across 18 px)
//   - Shared L/M/H decomposition via U2 VpUnit::BandFromBase (exact derived 2D transfer gains)
//   - Camera-relative fast-object motion gate (|v - v_cam| + 3x3 Karis max-velocity silhouette dilation +
//     Overlay / DynamicTexture zero-weight masks + zero cross-boundary weight bleed)
//   - Physics-derived frequency-split focus guard (analytical EdgeFocusRatio from BandFromBase sigmas;
//     protects focal-plane shifts on all bands; never self-vetoes W1 banded sky; Q4 optional L-band bokeh mode)
//   - Explicit 4-layer grain preservation (L1 separation, L2 A1 grain subtraction, L3 convex+cell bounds,
//     L4 TdGuardPort output guard modifying candOut ONLY — carryClean_ stays 100% grain-free)
//   - Populates U19 TdMapsView::Block per 16x16 cell
//
// Every constant is derived from signal physics or existing VP-V contracts (Rule 1).
// Every stand-in / fallback is visibly recorded in StandInRegistry and result counters (Rule 4).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/Ramp.h>
#include <VP-V/modules/motion/estimation/composed/WarpSampler.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/shared/TdMapsView.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2FrameRole.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TdV2
{
	// ---- Rule-1 Derived Motion & Focus Gate Physics ----

	// Quarter-pel sub-pixel tracking ceiling (P3r / CellTrust): object speed <= 0.25 px/frame receives full weight 1.0.
	constexpr double kStillObjectSpeedPx = 0.25;

	// Fast-object motion cutoff: 2 * kSigmaFine (= 2.0 px/frame), where 1-frame object displacement spans the fine filter support.
	constexpr double kFastObjectCutoffPx = 2.0 * VpUnit::BandFromBase::kSigmaFine;

	// Strictly numerical denominator floor.
	constexpr double kEngineNumFloor = 1e-9;

	// Analytical 1D step-edge Fourier energy passing through difference-of-Gaussians G(a) - G(b):
	//   integral |exp(-0.5 a^2 w^2) - exp(-0.5 b^2 w^2)|^2 / w^2 dw  proportional to  2 * sqrt((a^2 + b^2) / 2) - a - b.
	inline double StepEdgeBandEnergy(double sigmaA, double sigmaB)
	{
		const double e = 2.0 * std::sqrt(0.5 * (sigmaA * sigmaA + sigmaB * sigmaB)) - sigmaA - sigmaB;
		return e > 0.0 ? e : 0.0;
	}

	// Analytical focus ratio F(sigmaPsf) = E2(sigmaPsf) / (E2(sigmaPsf) + E3(sigmaPsf)) of an ideal step edge
	// blurred by optical PSF sigmaPsf and measured through U2 BandFromBase's exact sigmas (1.0, 2.5, 6.0):
	//   - Sharp edge (sigmaPsf = 0.0):       F ~= 0.3078
	//   - Defocused (sigmaPsf = kSigmaBase): F ~= 0.1170
	//   - Focus-shift drop (sharp -> defocus): F(0) - F(kSigmaBase) ~= 0.1908
	inline double AnalyticalEdgeFocusRatio(double sigmaPsf)
	{
		const double a2 = std::hypot(VpUnit::BandFromBase::kSigmaFine, sigmaPsf);
		const double b2 = std::hypot(VpUnit::BandFromBase::kSigmaBase, sigmaPsf);
		const double b3 = std::hypot(VpUnit::BandFromBase::kSigmaLow,  sigmaPsf);
		const double e2 = StepEdgeBandEnergy(a2, b2);
		const double e3 = StepEdgeBandEnergy(b2, b3);
		const double tot = e2 + e3;
		return tot > kEngineNumFloor ? (e2 / tot) : 0.0;
	}

	// ---- Lightweight Deterministic Plane (compatible with VpComposed::SampleBicubic) ----
	struct TdV2Plane
	{
		int w = 0;
		int h = 0;
		std::vector<double> v;

		TdV2Plane() = default;
		TdV2Plane(int width, int height, double fill = 0.0)
			: w(width), h(height), v(static_cast<size_t>(width) * static_cast<size_t>(height), fill)
		{
		}

		bool Empty() const { return w <= 0 || h <= 0 || v.size() != static_cast<size_t>(w) * static_cast<size_t>(h); }
		double& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
	};

	// Active picture rectangle [x0, x1) x [y0, y1). Bar pixels outside are bit-exact copied and never crossed by warps.
	struct ActiveRect
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

		static ActiveRect Full(int w, int h) { return { 0, 0, w, h }; }

		ActiveRect ClampedTo(int w, int h) const
		{
			ActiveRect r;
			r.x0 = (std::max)(0, (std::min)(w, x0));
			r.y0 = (std::max)(0, (std::min)(h, y0));
			r.x1 = (std::max)(r.x0, (std::min)(w, x1));
			r.y1 = (std::max)(r.y0, (std::min)(h, y1));
			return r;
		}

		int Width() const { return x1 - x0; }
		int Height() const { return y1 - y0; }
		bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
		bool Empty() const { return x1 <= x0 || y1 <= y0; }
	};

	// Sub-plane view over ActiveRect so VpComposed::SampleBicubic enforces hard bar boundaries automatically
	// (all 16 bicubic taps must lie strictly inside [x0, x1) x [y0, y1)).
	struct ActiveSubPlaneView
	{
		const TdV2Plane* src = nullptr;
		int ox = 0, oy = 0;
		int w = 0, h = 0;

		double at(int x, int y) const { return src->at(ox + x, oy + y); }
	};

	struct WarpDiagnostics
	{
		size_t borderBilinearPixels = 0;
		size_t nonFinitePixels = 0;
	};

	// Warp `refClean` into current frame coordinates using shared VpComposed::SampleBicubic inside `active`.
	// ---- Motion Classification & Camera-Relative Fast-Object Motion Gate (plan s1.3) ----
	enum class RegionMotionClass : uint8_t
	{
		Still = 0,
		Camera = 1,
		Object = 2,
		Overlay = 3,
		DynamicTexture = 4
	};

	// Out-of-bounds landings keep `outValid = 0` (zero blend/moment weight) while filling `outWarped` with the
	// edge-clamped active-border sample (NEVER `0.0`), preventing 18-px Gaussian ringing near letterbox bars.
	// Any non-finite pixel in `refClean` is detected visibly, excluded (`outValid = 0`), and filled from `fallbackClean`
	// so `DecomposeActiveBands` never propagates NaN/Inf.
	inline WarpDiagnostics WarpCleanReference(
		const TdV2Plane& refClean,
		const TdV2Plane& fallbackClean,
		const ActiveRect& active,
		const std::vector<double>& cellVx,
		const std::vector<double>& cellVy,
		int cellsW,
		int cellsH,
		int cellPx,
		TdV2Plane& outWarped,
		std::vector<uint8_t>& outValid,
		StandInRegistry* standIns = nullptr,
		const std::vector<uint8_t>* cellValid = nullptr,
		const std::vector<RegionMotionClass>* cellCls = nullptr,
		const Homography3x3* cameraH = nullptr,
		std::vector<uint8_t>* outNonFiniteCells = nullptr,
		bool enableSmoothWarpVectors = true)
	{
		WarpDiagnostics diag;
		const int w = refClean.w;
		const int h = refClean.h;
		outWarped = fallbackClean;
		outValid.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);

		const ActiveRect ar = active.ClampedTo(w, h);
		const size_t expectedCells = (cellsW > 0 && cellsH > 0)
			? static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH)
			: 0u;
		if (ar.Empty() || expectedCells == 0u || cellPx <= 0 ||
		    cellVx.size() != expectedCells || cellVy.size() != expectedCells)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "WarpCleanReference invalid motion dimensions");
			return diag;
		}

		// Sanitize any non-finite pixels inside refClean's active rect before bicubic sampling.
		TdV2Plane safeRef = refClean;
		std::vector<uint8_t> refFinite(static_cast<size_t>(w) * static_cast<size_t>(h), 1u);
		for (int y = ar.y0; y < ar.y1; ++y)
		{
			for (int x = ar.x0; x < ar.x1; ++x)
			{
				if (!std::isfinite(safeRef.at(x, y)))
				{
					safeRef.at(x, y) = fallbackClean.at(x, y);
					refFinite[static_cast<size_t>(y) * w + x] = 0u;
					++diag.nonFinitePixels;
				}
			}
		}
		if (diag.nonFinitePixels > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "WarpCleanReference non-finite ref pixel");

		ActiveSubPlaneView view{ &safeRef, ar.x0, ar.y0, ar.Width(), ar.Height() };

		auto sampleBilinearClamped = [&](double sxLocal, double syLocal) -> double
		{
			const double cx = (std::max)(0.0, (std::min)(static_cast<double>(ar.Width() - 1), sxLocal));
			const double cy = (std::max)(0.0, (std::min)(static_cast<double>(ar.Height() - 1), syLocal));
			const int ix0 = static_cast<int>(std::floor(cx));
			const int iy0 = static_cast<int>(std::floor(cy));
			const int ix1 = (std::min)(ar.Width() - 1, ix0 + 1);
			const int iy1 = (std::min)(ar.Height() - 1, iy0 + 1);
			const double fx = cx - ix0;
			const double fy = cy - iy0;
			const double v00 = view.at(ix0, iy0);
			const double v10 = view.at(ix1, iy0);
			const double v01 = view.at(ix0, iy1);
			const double v11 = view.at(ix1, iy1);
			return (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy;
		};

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

		for (int y = ar.y0; y < ar.y1; ++y)
		{
			const int cy = (std::min)(cellsH - 1, y / cellPx);
			for (int x = ar.x0; x < ar.x1; ++x)
			{
				const int cx = (std::min)(cellsW - 1, x / cellPx);
				const size_t cIdx = static_cast<size_t>(cy) * cellsW + cx;
				if (cellValid != nullptr && cIdx < cellValid->size() && (*cellValid)[cIdx] == 0u)
				{
					// Invalid cell motion vector (mf.valid == 0): do NOT warp with invalid MV;
					// leave fallbackClean (curClean) in outWarped with outValid = 0.
					continue;
				}
				double vx = cellVx[cIdx];
				double vy = cellVy[cIdx];
				const bool useCamH = (cameraH != nullptr && cameraH->valid &&
				                      cellCls != nullptr && cIdx < cellCls->size() &&
				                      (*cellCls)[cIdx] == RegionMotionClass::Camera);
				double sxLocal = 0.0;
				double syLocal = 0.0;
				if (useCamH)
				{
					double hsx = 0.0, hsy = 0.0;
					if (!cameraH->Project(static_cast<double>(x), static_cast<double>(y), hsx, hsy))
					{
						++diag.nonFinitePixels;
						if (outNonFiniteCells != nullptr && cIdx < outNonFiniteCells->size())
							(*outNonFiniteCells)[cIdx] = 1u;
						continue;
					}
					sxLocal = hsx - static_cast<double>(ar.x0);
					syLocal = hsy - static_cast<double>(ar.y0);
				}
				else
				{
					if (!std::isfinite(vx) || !std::isfinite(vy))
					{
						++diag.nonFinitePixels;
						if (outNonFiniteCells != nullptr && cIdx < outNonFiniteCells->size())
							(*outNonFiniteCells)[cIdx] = 1u;
						// Leave fallbackClean value in outWarped (no zero-hole) with outValid = 0.
						continue;
					}
					// Plan 6.1 / Fable L-1: Bilinearly interpolate 16-px cell vectors between cell centers when enabled,
					// strictly skipping invalid or non-finite neighbor cells so bad vectors never bleed into valid neighbors.
					if (enableSmoothWarpVectors && cellPx > 0)
					{
						const double gx = (static_cast<double>(x) + 0.5) / static_cast<double>(cellPx) - 0.5;
						const double gy = (static_cast<double>(y) + 0.5) / static_cast<double>(cellPx) - 0.5;
						const int x0 = (std::max)(0, (std::min)(cellsW - 1, static_cast<int>(std::floor(gx))));
						const int y0 = (std::max)(0, (std::min)(cellsH - 1, static_cast<int>(std::floor(gy))));
						const int x1 = (std::min)(cellsW - 1, x0 + 1);
						const int y1 = (std::min)(cellsH - 1, y0 + 1);
						const double tx = RampMath::Clamp01(gx - static_cast<double>(x0));
						const double ty = RampMath::Clamp01(gy - static_cast<double>(y0));
						const size_t cIds[4] = {
							static_cast<size_t>(y0) * cellsW + x0,
							static_cast<size_t>(y0) * cellsW + x1,
							static_cast<size_t>(y1) * cellsW + x0,
							static_cast<size_t>(y1) * cellsW + x1
						};
						const double cwts[4] = {
							(1.0 - tx) * (1.0 - ty),
							tx * (1.0 - ty),
							(1.0 - tx) * ty,
							tx * ty
						};
						double wSum = 0.0, accVx = 0.0, accVy = 0.0;
						for (int q = 0; q < 4; ++q)
						{
							const size_t qi = cIds[q];
							if (cellValid != nullptr && qi < cellValid->size() && (*cellValid)[qi] == 0u)
								continue;
							const double qvx = cellVx[qi];
							const double qvy = cellVy[qi];
							if (!std::isfinite(qvx) || !std::isfinite(qvy))
								continue;
							wSum += cwts[q];
							accVx += cwts[q] * qvx;
							accVy += cwts[q] * qvy;
						}
						if (wSum > kEngineNumFloor)
						{
							vx = accVx / wSum;
							vy = accVy / wSum;
						}
					}
					sxLocal = static_cast<double>(x - ar.x0) + vx;
					syLocal = static_cast<double>(y - ar.y0) + vy;
				}
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
					// Active-rect 1-2 px border bilinear fallback (never crossing bar edges; visibly counted).
					const double bVal = sampleBilinearClamped(sxLocal, syLocal);
					const int ix0 = static_cast<int>(std::floor(sxLocal));
					const int iy0 = static_cast<int>(std::floor(syLocal));
					outWarped.at(x, y) = bVal;
					++diag.borderBilinearPixels;
					if (std::isfinite(bVal) && !footprintHasNonFinite(ix0, iy0, ix0 + 1, iy0 + 1))
						outValid[static_cast<size_t>(y) * w + x] = 1u;
				}
				else
				{
					// Out-of-bounds landing: fill with edge-clamped active border sample (ZERO 0.0-hole ringing!)
					// while keeping outValid == 0 so this pixel never enters moments or blending.
					outWarped.at(x, y) = sampleBilinearClamped(sxLocal, syLocal);
				}
			}
		}

		if (diag.borderBilinearPixels > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::BorderBilinearFallback);
		if (diag.nonFinitePixels > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "WarpCleanReference non-finite motion or ref");
		return diag;
	}

	// ---- U2 VpUnit::BandFromBase Decomposition (L, M, H, D3 strictly inside ActiveRect) ----
	struct ThreeBandPlanes
	{
		TdV2Plane L;   // G(2.5) clean
		TdV2Plane M;   // G(1.0) clean - L
		TdV2Plane H;   // clean - G(1.0) clean
		TdV2Plane D3;  // L - G(6.0) clean (for focus E2/(E2+E3) stand-in)
	};

	inline ThreeBandPlanes DecomposeActiveBands(const TdV2Plane& clean, const ActiveRect& active, StandInRegistry* standIns = nullptr)
	{
		if (standIns != nullptr)
			standIns->Engage(StandInKind::BandMemoW2);

		ThreeBandPlanes out;
		out.L = TdV2Plane(clean.w, clean.h, 0.0);
		out.M = TdV2Plane(clean.w, clean.h, 0.0);
		out.H = TdV2Plane(clean.w, clean.h, 0.0);
		out.D3 = TdV2Plane(clean.w, clean.h, 0.0);

		const ActiveRect ar = active.ClampedTo(clean.w, clean.h);
		if (ar.Empty())
			return out;

		const int aw = ar.Width();
		const int ah = ar.Height();
		std::vector<double> sub(static_cast<size_t>(aw) * static_cast<size_t>(ah), 0.0);
		for (int y = 0; y < ah; ++y)
			for (int x = 0; x < aw; ++x)
				sub[static_cast<size_t>(y) * aw + x] = clean.at(ar.x0 + x, ar.y0 + y);

		const auto tapsFine = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
		const auto tapsBase = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaBase, 3.0, true, 0.0);
		const auto tapsLow  = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaLow,  3.0, true, 0.0);

		std::vector<double> gFine, gBase, gLow;
		VpUnit::BandFromBase::BlurTaps(sub, gFine, aw, ah, tapsFine, VpUnit::BandFromBase::EdgeMode::Replicate);
		VpUnit::BandFromBase::BlurTaps(sub, gBase, aw, ah, tapsBase, VpUnit::BandFromBase::EdgeMode::Replicate);
		VpUnit::BandFromBase::BlurTaps(sub, gLow,  aw, ah, tapsLow,  VpUnit::BandFromBase::EdgeMode::Replicate);

		for (int y = 0; y < ah; ++y)
		{
			for (int x = 0; x < aw; ++x)
			{
				const size_t si = static_cast<size_t>(y) * aw + x;
				const int gx = ar.x0 + x;
				const int gy = ar.y0 + y;
				const double lVal = gBase[si];
				const double mVal = gFine[si] - lVal;
				const double hVal = sub[si] - gFine[si];
				out.L.at(gx, gy) = lVal;
				out.M.at(gx, gy) = mVal;
				out.H.at(gx, gy) = hVal;
				out.D3.at(gx, gy) = lVal - gLow[si];
			}
		}
		return out;
	}

	struct CellMotionField
	{
		int cellsW = 0;
		int cellsH = 0;
		std::vector<double> vx;
		std::vector<double> vy;
		std::vector<double> camVx;
		std::vector<double> camVy;
		std::vector<RegionMotionClass> cls;
		std::vector<uint8_t> valid;
		std::vector<double> confidence;       // Optional P3r continuous per-cell motion confidence in [0, 1] (empty => 1.0 everywhere)
		std::vector<double> fbErrorPx;        // Optional forward/backward disagreement in px (6.2 / M-3: > 1.0 px => hard weight 0)

		bool IsWellFormed(int expectedCW, int expectedCH) const
		{
			if (expectedCW <= 0 || expectedCH <= 0 || cellsW != expectedCW || cellsH != expectedCH)
				return false;
			const size_t n = static_cast<size_t>(expectedCW) * static_cast<size_t>(expectedCH);
			return vx.size() == n && vy.size() == n &&
			       camVx.size() == n && camVy.size() == n &&
			       cls.size() == n && valid.size() == n &&
			       (confidence.empty() || confidence.size() == n) &&
			       (fbErrorPx.empty() || fbErrorPx.size() == n);
		}

		static CellMotionField Uniform(int cw, int ch, double mvx, double mvy,
		                               double cvx, double cvy, RegionMotionClass c = RegionMotionClass::Camera)
		{
			CellMotionField f;
			f.cellsW = cw;
			f.cellsH = ch;
			const size_t n = static_cast<size_t>(cw) * static_cast<size_t>(ch);
			f.vx.assign(n, mvx);
			f.vy.assign(n, mvy);
			f.camVx.assign(n, cvx);
			f.camVy.assign(n, cvy);
			f.cls.assign(n, c);
			f.valid.assign(n, 1u);
			return f;
		}
	};

	struct MotionGateResult
	{
		std::vector<double> gateL;    // 0 on Overlay/invalid; speed-gated on fast Object cells; 1 on Camera/Still
		std::vector<double> gateMH;   // Camera-relative smooth speed fade + 3x3 silhouette dilation, 0 on Overlay/DynamicTexture
		std::vector<uint8_t> nonFiniteCellMask;
		size_t nonFiniteCells = 0;
	};

	// Compute camera-relative object speed gate with Karis 3x3 max-velocity silhouette dilation
	// and Plan 6.2 / Fable M-3 hard > 1.0 px forward/backward occlusion gate (continuous confidence tapers below 1.0 px).
	inline MotionGateResult EvaluateCameraRelativeMotionGate(
		const CellMotionField& mf,
		StandInRegistry* standIns = nullptr,
		bool enableHardFbOcclusionGate = true)
	{
		MotionGateResult out;
		if (!mf.IsWellFormed(mf.cellsW, mf.cellsH))
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::MotionFieldStandIn, "EvaluateCameraRelativeMotionGate malformed field");
			return out;
		}
		const size_t n = static_cast<size_t>(mf.cellsW) * static_cast<size_t>(mf.cellsH);
		out.gateL.assign(n, 0.0);
		out.gateMH.assign(n, 0.0);
		out.nonFiniteCellMask.assign(n, 0u);

		std::vector<double> relSpeed(n, 0.0);
		std::vector<double> cellConf(n, 1.0);
		std::vector<uint8_t> fbOccluded(n, 0u);
		std::vector<uint8_t> finiteCell(n, 0u);
		const bool hasConf = !mf.confidence.empty();
		const bool hasFbErr = !mf.fbErrorPx.empty();
		for (size_t i = 0; i < n; ++i)
		{
			if (!mf.valid[i])
				continue;
			if (!std::isfinite(mf.vx[i]) || !std::isfinite(mf.vy[i]) ||
			    !std::isfinite(mf.camVx[i]) || !std::isfinite(mf.camVy[i]))
			{
				out.nonFiniteCellMask[i] = 1u;
				++out.nonFiniteCells;
				continue;
			}
			if (hasConf)
			{
				const double rawConf = mf.confidence[i];
				if (!std::isfinite(rawConf))
				{
					out.nonFiniteCellMask[i] = 1u;
					++out.nonFiniteCells;
					continue;
				}
				cellConf[i] = RampMath::Clamp01(rawConf);
			}
			if (hasFbErr)
			{
				const double rawFb = mf.fbErrorPx[i];
				if (!std::isfinite(rawFb) || rawFb < 0.0)
				{
					out.nonFiniteCellMask[i] = 1u;
					++out.nonFiniteCells;
					continue;
				}
				if (enableHardFbOcclusionGate && rawFb > kHardFbOcclusionPx)
				{
					fbOccluded[i] = 1u;
				}
			}
			finiteCell[i] = 1u;
			const double dvx = mf.vx[i] - mf.camVx[i];
			const double dvy = mf.vy[i] - mf.camVy[i];
			relSpeed[i] = std::sqrt(dvx * dvx + dvy * dvy);
		}
		if (out.nonFiniteCells > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "EvaluateCameraRelativeMotionGate non-finite vector, confidence, or fbErrorPx");

		for (int cy = 0; cy < mf.cellsH; ++cy)
		{
			for (int cx = 0; cx < mf.cellsW; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy) * mf.cellsW + cx;
				if (!finiteCell[idx])
					continue;
				const RegionMotionClass c = mf.cls[idx];
				if (c == RegionMotionClass::Overlay || fbOccluded[idx] != 0u)
				{
					// Hard drop on overlays/subtitles and > 1.0 px forward/backward occlusion cells across ALL bands.
					out.gateL[idx] = 0.0;
					out.gateMH[idx] = 0.0;
					continue;
				}

				// 3x3 max-velocity silhouette dilation (Karis SIGGRAPH 2014):
				// Only dilate non-Overlay fast object speeds so moving foreground edges claim disocclusion boundaries tightly.
				double dilatedSpeed = relSpeed[idx];
				for (int dy = -1; dy <= 1; ++dy)
				{
					const int ny = cy + dy;
					if (ny < 0 || ny >= mf.cellsH) continue;
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = cx + dx;
						if (nx < 0 || nx >= mf.cellsW) continue;
						const size_t nIdx = static_cast<size_t>(ny) * mf.cellsW + nx;
						if (finiteCell[nIdx] && mf.cls[nIdx] != RegionMotionClass::Overlay)
						{
							if (relSpeed[nIdx] > dilatedSpeed)
								dilatedSpeed = relSpeed[nIdx];
						}
					}
				}

				// Smooth fade via U25 RampMath::Smoothstep: 1.0 at <= 0.25 px, 0.0 at >= 2.0 px.
				const double speedGate = 1.0 - RampMath::Smoothstep(kStillObjectSpeedPx, kFastObjectCutoffPx, dilatedSpeed);

				// Gate L-band by speed on fast moving Object cells too (prevents coarse-band ghosting on fast movers),
				// while Still / Camera / DynamicTexture keep L-band at 1.0 (camera pans are never gated off).
				// P3r continuous per-cell confidence feathers both L and M/H reliability smoothly in [0, 1] (default 1.0).
				const double conf = cellConf[idx];
				out.gateL[idx] = ((c == RegionMotionClass::Object) ? speedGate : 1.0) * conf;

				if (c == RegionMotionClass::DynamicTexture)
				{
					// Hard drop on chaotic textures (water/rain/foliage/fire) for M/H.
					out.gateMH[idx] = 0.0;
				}
				else
				{
					out.gateMH[idx] = speedGate * conf;
				}
			}
		}
		return out;
	}

	// ---- Frequency-Split Focus Guard (Owner Focus Rule, plan s1.4 L135-L143) ----
	struct FocusCellInfo
	{
		double focusRatio = 1.0;        // E2 / (E2 + E3)
		bool isDefocused = false;       // Diagnostic static low-focus indicator (does NOT self-veto curved gradients or ramps)
		bool isExplicitBokeh = false;   // Upstream kFocusMap (id 32) matched-focus optical bokeh -> gated by cfg_.bokehMode
		bool focusChange = false;       // Genuine focal-plane shift along motion (or explicitFocusChangeCells) -> hard veto ALL bands
	};

	// Compute physics-derived E2/(E2+E3) focus stand-in map using AnalyticalEdgeFocusRatio(sigmaPsf):
	//   - structureFloor: 0-dB SNR boundary (2 * gainMD3 * max(1/12, stepTotalVar)) derived strictly from
	//     1-LSB uniform quantization noise variance (1/12) and BandFromBase's exact 2D (M + D3) transfer gain.
	//   - Focus change (rack focus): requires a genuine drop in focus ratio along aligned motion
	//     (refEnergy > structureFloor, curM2 < refM2 by Parseval, curFDebiased < fSharp, and focus drop > F(0) - F(kSigmaBase))
	//     or explicit focus-change flag, so static smooth curved skies & ramps are NEVER falsely vetoed.
	//   - Matched-focus optical bokeh (`isExplicitBokeh`): supplied strictly by upstream kFocusMap id 32
	//     (`explicitDefocusedCells`, Plan s1.4 L140) and governed by `cfg_.bokehMode`.
	inline std::vector<FocusCellInfo> EvaluateFocusGuardStandIn(
		const ThreeBandPlanes& curBands,
		const std::vector<ThreeBandPlanes>& refBands,
		const ActiveRect& active,
		int cellsW,
		int cellsH,
		int cellPx,
		int sourceBits,
		double stepTotalVar,
		const std::vector<uint8_t>* explicitFocusChangeCells,
		StandInRegistry* standIns,
		const std::vector<uint8_t>* explicitDefocusedCells = nullptr,
		const std::vector<double>* cellStepScales = nullptr)
	{
		(void)sourceBits;
		if (standIns != nullptr)
			standIns->Engage(StandInKind::FocusMapK32);

		const size_t numCells = static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH);
		std::vector<FocusCellInfo> info(numCells);
		// Map full-spectrum noise variance into the exact (M + D3) band-energy domain via BandFromBase's
		// 2D white-noise transfer gain gainMD3 = ||M||_2^2 + ||D3||_2^2 (~= 0.05589), floored by the exact
		// 1-LSB uniform quantization noise variance (1.0^2 / 12 = 1/12 code^2), at the exact 0-dB SNR boundary (2 * N).
		const BandWhiteNoiseGains focusGains = ComputeBandWhiteNoiseGains();
		const double gainMD3 = focusGains.mid + focusGains.d3;
		const double baseQuantVar = (kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0;
		const double stepNoiseVar = (std::max)(0.0, stepTotalVar);
		const double effNoiseVar = (std::max)(baseQuantVar, stepNoiseVar);
		const double structureFloor = 2.0 * gainMD3 * effNoiseVar;
		const double fSharp = AnalyticalEdgeFocusRatio(0.0);
		const double fDefocus = AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase);
		const double fShiftDelta = fSharp - fDefocus;
		// Exact M-band passband supremum ||H_M||_inf (matches a2BandPeakSq[1] and ApplyTransformUnknownBound 1:1).
		const double s1SqF = VpUnit::BandFromBase::kSigmaFine * VpUnit::BandFromBase::kSigmaFine;
		const double s2SqF = VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase;
		const double wStarSqF = (2.0 * std::log(VpUnit::BandFromBase::kSigmaBase / VpUnit::BandFromBase::kSigmaFine)) / (s2SqF - s1SqF);
		const double hmPeakF = std::exp(-(s1SqF * wStarSqF) / 2.0) - std::exp(-(s2SqF * wStarSqF) / 2.0);

		const ActiveRect ar = active.ClampedTo(curBands.M.w, curBands.M.h);

		auto cellFocusRatio = [&](const ThreeBandPlanes& b, int cx, int cy, double& outTotalEnergy, double& outM2) -> double
		{
			const int x0 = (std::max)(ar.x0, cx * cellPx);
			const int y0 = (std::max)(ar.y0, cy * cellPx);
			const int x1 = (std::min)(ar.x1, (cx + 1) * cellPx);
			const int y1 = (std::min)(ar.y1, (cy + 1) * cellPx);
			double e2 = 0.0, e3 = 0.0;
			size_t cnt = 0;
			for (int y = y0; y < y1; ++y)
			{
				for (int x = x0; x < x1; ++x)
				{
					const double m = b.M.at(x, y);
					const double d3 = b.D3.at(x, y);
					e2 += m * m;
					e3 += d3 * d3;
					++cnt;
				}
			}
			if (cnt == 0)
			{
				outTotalEnergy = 0.0;
				outM2 = 0.0;
				return 1.0;
			}
			const double invC = 1.0 / static_cast<double>(cnt);
			const double m2 = e2 * invC;
			const double m3 = e3 * invC;
			outM2 = m2;
			outTotalEnergy = m2 + m3;
			return outTotalEnergy > kEngineNumFloor ? (m2 / (outTotalEnergy + kEngineNumFloor)) : 1.0;
		};

		for (int cy = 0; cy < cellsH; ++cy)
		{
			for (int cx = 0; cx < cellsW; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy) * cellsW + cx;
				double curEnergy = 0.0, curM2 = 0.0;
				const double curF = cellFocusRatio(curBands, cx, cy, curEnergy, curM2);
				info[idx].focusRatio = curF;

				// Per-cell AQ/PQ step scaling of coding noise variance & structure floor (homogeneous degree 2 in stepDelta).
				double cellScale = 1.0;
				if (cellStepScales != nullptr && idx < cellStepScales->size())
				{
					const double sv = (*cellStepScales)[idx];
					if (std::isfinite(sv) && sv > 0.0)
						cellScale = sv < kCellStepScaleMin ? kCellStepScaleMin : (sv > kCellStepScaleMax ? kCellStepScaleMax : sv);
				}
				const double cellStepScaleSq = cellScale * cellScale;
				const double cellStepNoiseVar = stepNoiseVar * cellStepScaleSq;
				const double cellStructureFloor = structureFloor * cellStepScaleSq;
				// Exact 3-sigma M-band Parseval passband bound (decoupled from TD-U3 pixelBoundKappa):
				// any M-band RMS drop <= 3 * sqrt(cellStepNoiseVar) * ||H_M||_inf can arise from lawful B-frame quantization!
				const double quantMidRmsBound = std::sqrt(kHuberKneeSq) * std::sqrt(cellStepNoiseVar) * hmPeakF;

				// Noise-debiased focus ratio for optical bokeh / rack-focus detection:
				// White coding noise passes ~6.5x more variance into M (gains.mid) than D3 (gains.d3);
				// subtracting exact BandFromBase white-noise variance restores the true optical focus ratio.
				const double curM3 = (std::max)(0.0, curEnergy - curM2);
				const double cleanM2 = (std::max)(0.0, curM2 - cellStepNoiseVar * focusGains.mid);
				const double cleanM3 = (std::max)(0.0, curM3 - cellStepNoiseVar * focusGains.d3);
				const double curFDebiased = (cleanM2 + cleanM3 > kEngineNumFloor)
					? (cleanM2 / (cleanM2 + cleanM3 + kEngineNumFloor))
					: curF;

				if (curEnergy > cellStructureFloor && curFDebiased <= fDefocus)
					info[idx].isDefocused = true;

				if (explicitDefocusedCells != nullptr && idx < explicitDefocusedCells->size() && (*explicitDefocusedCells)[idx] != 0)
					info[idx].isExplicitBokeh = true;

				if (explicitFocusChangeCells != nullptr && idx < explicitFocusChangeCells->size() && (*explicitFocusChangeCells)[idx] != 0)
				{
					info[idx].focusChange = true;
				}
				else
				{
					// Detect genuine focal-plane shift (rack focus) along motion respecting 2D Parseval's theorem:
					// Strict conjunction on noise-debiased focus ratios + 3-sigma 2D Parseval M-band RMS bound
					// so static curved skies (refF ~= curF) and B-frame quantization wipes NEVER trigger false vetoes!
					for (const auto& rb : refBands)
					{
						double refEnergy = 0.0, refM2 = 0.0;
						const double refF = cellFocusRatio(rb, cx, cy, refEnergy, refM2);
						const double refM3 = (std::max)(0.0, refEnergy - refM2);
						const double cleanRefM2 = (std::max)(0.0, refM2 - baseQuantVar * focusGains.mid);
						const double cleanRefM3 = (std::max)(0.0, refM3 - baseQuantVar * focusGains.d3);
						const double refFDebiased = (cleanRefM2 + cleanRefM3 > kEngineNumFloor)
							? (cleanRefM2 / (cleanRefM2 + cleanRefM3 + kEngineNumFloor))
							: refF;
						if (refEnergy > cellStructureFloor && curM2 < refM2 && curFDebiased < fSharp &&
						    (refFDebiased - curFDebiased) > fShiftDelta &&
						    (std::sqrt(refM2) - std::sqrt(curM2)) > quantMidRmsBound)
						{
							info[idx].focusChange = true;
							break;
						}
					}
				}
			}
		}
		return info;
	}

	// ---- Reference Frame Descriptor for Dual-Source Fusion ----
	struct ReferenceInput
	{
		const TdV2Plane* cleanPlane = nullptr;     // Grain-free reference plane
		const CellMotionField* motion = nullptr;   // Current -> Reference motion field
		bool isExactCopyOfCurrent = false;
		Homography3x3 cameraHomography;            // Optional P7 3x3 camera homography (used strictly on RegionMotionClass::Camera cells when valid)
	};

	// ---- Per-Frame Input & Output Structures ----
	struct FrameProcessInput
	{
		uint64_t seq = 0;
		TdV2Plane receivedY;                  // Received frame Y = clean + ownGrain
		bool hasCleanSplit = false;           // True when upstream P10/P10g supplied `cleanY`
		TdV2Plane cleanY;                     // Grain-free X_clean (Layer 1 separation)
		std::vector<double> retainedGrainVar; // Per-cell mean(f'^2) * N_b from P10g (Layer 2 A1 correction)
		ActiveRect activeRect;                // Trusted active picture rect (0-sized => full plane)

		ReferenceInput prevPeak;              // y_1: nearest previous peak
		ReferenceInput nextPeak;              // y_2: nearest future peak (optional when E=0 or cut ahead)
		ReferenceInput prevPeak2;             // Optional 2nd past peak for one-sided A1 partner when nextPeak absent
		CellMotionField carryMotion;          // Current -> Previous finalized clean carry motion (used when carry valid)
		bool hasCarryMotion = false;

		// Explicit coding step prior from P16 (when hasStepPrior == false, engine derives 1-LSB default & engages StandInKind::StepPriorDefault).
		bool hasStepPrior = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool transformIdentified8x8 = false;  // True only when codec transform (e.g. AVC 8x8) is identified
		const TdV2Plane* codecModelPb = nullptr; // Field B CodecModel prediction P_B (null when transformIdentified8x8 engages CodecModelPbStandIn)

		bool isCutOrSpanInvalid = false;      // Resets recurrent carry state immediately
		double fadeKeep = 1.0;                // Upstream U7 gradual retention keep in [0, 1] (default 1.0 = bit-identical; <=0 or NaN resets carry)
		const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
		const std::vector<uint8_t>* explicitDefocusedCells = nullptr; // Upstream kFocusMap (id 32) matched-focus optical bokeh mask (Plan s1.4 L140)
		const std::vector<uint8_t>* faceMask = nullptr; // Pass-1 per-cell (cellsW * cellsH) face/non-rigid mask (non-zero => delta == 0)
		std::vector<double> cellStepScale;    // Optional per-cell AQ/PQ step scale (cellsW * cellsH; empty => 1.0 neutral everywhere)
		PsdShape psdShape;                    // Optional P10g grain PSD shape multipliers (default 1.0 = white-noise identity)
		QualityPattern::FrameRole frameRole = QualityPattern::FrameRole::Unknown; // Optional frame role for A2e / 2-frame stats pool

		// TD-U5 (H-4 / Owner D2): Anchored carry & peak drift measurement inputs.
		bool isConfirmedPeak = false;          // True on confirmed quality peaks: measures real kDrift and re-anchors carry
		CellMotionField anchorComposedMotion;  // Single-resample composed vector field current -> last confirmed peak anchor
		bool hasAnchorComposedMotion = false;
		CellMotionField directPeakToPeakMotion;// Optional direct vector field current peak -> previous peak (for exact kDrift)
		bool hasDirectPeakToPeakMotion = false;
	};

	struct FrameProcessResult
	{
		TdV2Plane outputY;                    // Final output Y_out (bit-identical to receivedY when Off/Shadow)
		TdV2Plane outputClean;                // Reconstructed grain-free clean + delta (for diagnostics / carry; NEVER carries Layer-4 grain add-back)
		std::vector<TdMapsView::Block> tdMaps;// U19 per-16x16-cell published td.maps
		int cellsW = 0;
		int cellsH = 0;
		size_t activeRefCount = 0;            // Number of references fused (peaks + carry)
		size_t guardViolationCells = 0;       // Layer-4 TdGuardPort interventions
		size_t focusVetoCells = 0;            // Focus-change cells vetoed (M/H)
		size_t nonFiniteCells = 0;            // Unique cells closed to zero on non-finite input
		size_t singularCells = 0;             // Unique cells closed to zero on singular covariance solve
		size_t singleRefA2Cells = 0;          // Unique cells where A1 fell back to A2 due to missing independent partner
		size_t motionStandInRefs = 0;         // References with null/mismatched motion field visibly handled
		size_t missingMotionSkippedRefs = 0;  // References skipped per L4 because motion field was null (zero whole-frame Still blend)
		size_t faceMaskedCells = 0;           // Pass-1 face/non-rigid masked cells forced to exact delta == 0
		size_t codecModelPbStandInCells = 0;  // Unique cells where STRICT/CELL fell back visibly when Field B P_B absent
		size_t a2InvalidCells = 0;            // Cells closed to zero because A2 step/deadZonePhi stats were invalid
		size_t borderBilinearPixels = 0;      // Active-rect 1-2 px border bilinear fallback pixel count
		bool carryUsed = false;
		bool carryReset = false;
		bool anchoredCarryUsed = false;             // TD-U5: true when single-resample anchored carry served as a ref this frame
		size_t anchoredCarryHeldOutRejectCells = 0; // TD-U5: cells rejected by per-cell held-out structure check (marked for re-seed)
		size_t driftBarredCells = 0;                // TD-U5: cells barred from carry for next span because measured kDrift > kAnchorDriftBarPx
		bool driftReducedSearchUsed = false;        // TD-U5: true when seeded local block search measured kDrift ("drift: reduced")
		EmpiricalOwnErrorPrior empiricalOwnError;   // TD-U1 A2e per-band x activity-class own-error prior U_j(class)
		AlignedSharpnessSummary alignedSharpness;   // TD-U1 motion-aligned relative sharpness r_t (abstaining focus cells)
		// Plan 6.6 / CHR-HIGH-3 / RED-BUG-5: Exported post-gate luma cell weights [band][ref][cell] + explicit per-slot RefRole
		// aligned directly with ChromaProcessInput::lumaCellWeights so callers pass `&lumaRes.lumaCellWeights` cleanly.
		std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> lumaCellWeights;
		RefRole lumaRefRoles[kMaxBlendRefs] = {};
		int lumaActiveRefCount = 0;
	};

	// ---- Corner-Tapered Spatial-Only Bilinear Weight Interpolation (Zero New Constants) ----
	// Applies the 3-sigma Huber pixel taper psi(d^2 / max(C_q, eps_num)) per bilinear corner q in {00, 10, 01, 11}
	// BEFORE bilinearly combining the 4 corner weights:
	//   w_eff(p) = sum_q alpha_q(p) * w_q * HuberPixelTaper(diff, C_q).
	// On uniform regions (C_00 == C_10 == C_01 == C_11) this is algebraically identical to SpatialBilinearWeight * HuberPixelTaper;
	// across a micro-motion cell boundary (e.g. poolR == 0 in Preset::RealTime), a low-variance neighbor's weight w_nbr is
	// tapered by its OWN variance C_nbr so shifted pixels (d^2 >> 9 * C_nbr) immediately reject w_nbr without blocky seams.
	inline double SpatialBilinearHuberWeight(
		const double* cellWeights,
		const double* cellDiagC,
		int cellsW,
		int cellsH,
		double pxX,
		double pxY,
		int cellPx,
		double diff)
	{
		if (cellWeights == nullptr || cellDiagC == nullptr || cellsW <= 0 || cellsH <= 0 || cellPx <= 0 ||
		    !std::isfinite(pxX) || !std::isfinite(pxY) || !std::isfinite(diff))
		{
			return 0.0;
		}
		const double gx = (pxX + 0.5) / static_cast<double>(cellPx) - 0.5;
		const double gy = (pxY + 0.5) / static_cast<double>(cellPx) - 0.5;
		const int x0 = (std::max)(0, (std::min)(cellsW - 1, static_cast<int>(std::floor(gx))));
		const int y0 = (std::max)(0, (std::min)(cellsH - 1, static_cast<int>(std::floor(gy))));
		const int x1 = (std::min)(cellsW - 1, x0 + 1);
		const int y1 = (std::min)(cellsH - 1, y0 + 1);
		const double tx = RampMath::Clamp01(gx - static_cast<double>(x0));
		const double ty = RampMath::Clamp01(gy - static_cast<double>(y0));

		const size_t i00 = static_cast<size_t>(y0) * cellsW + x0;
		const size_t i10 = static_cast<size_t>(y0) * cellsW + x1;
		const size_t i01 = static_cast<size_t>(y1) * cellsW + x0;
		const size_t i11 = static_cast<size_t>(y1) * cellsW + x1;

		const double w00 = cellWeights[i00];
		const double w10 = cellWeights[i10];
		const double w01 = cellWeights[i01];
		const double w11 = cellWeights[i11];
		const double d00 = cellDiagC[i00];
		const double d10 = cellDiagC[i10];
		const double d01 = cellDiagC[i01];
		const double d11 = cellDiagC[i11];
		if (!std::isfinite(w00) || !std::isfinite(w10) || !std::isfinite(w01) || !std::isfinite(w11) ||
		    !std::isfinite(d00) || !std::isfinite(d10) || !std::isfinite(d01) || !std::isfinite(d11))
		{
			return 0.0;
		}

		const double t00 = w00 * HuberPixelTaper(diff, d00);
		const double t10 = w10 * HuberPixelTaper(diff, d10);
		const double t01 = w01 * HuberPixelTaper(diff, d01);
		const double t11 = w11 * HuberPixelTaper(diff, d11);
		const double top = t00 + (t10 - t00) * tx;
		const double bot = t01 + (t11 - t01) * tx;
		return top + (bot - top) * ty;
	}

	// ---- Clean-Luma-Guided Edge-Aware Weight Upsampling (SOTA #4 / SC-04, Rule 1) ----
	// Modulates the 4 spatial bilinear corner weights s_q by a luminance-range affinity
	//   g_q = exp(-(cleanY(p) - cellMeanY(q))^2 / (2 * sigma_L(q)^2))
	// where sigma_L(q) = max(cellStdY(q), noiseSigmaFloor, sqrt(1/12)) is derived directly from local cell
	// luminance std-dev and quantization noise floor (zero invented constants), snapping 16-px cell weights
	// tightly to sharp object silhouettes even below the 3-sigma Huber knee. Falls back cleanly to pure spatial
	// weights s_q when sum_q (s_q * g_q) <= kEngineNumFloor. Identical to bilinear on uniform luminance.
	inline double SpatialGuidedBilinearWeight(
		const double* cellWeights,
		const double* cellDiagC,
		const double* cellMeanY,
		const double* cellStdY,
		int cellsW,
		int cellsH,
		double pxX,
		double pxY,
		int cellPx,
		double diff,
		double cleanY,
		double quantSigmaFloor,
		bool enableHuber)
	{
		if (cellWeights == nullptr || cellMeanY == nullptr || cellStdY == nullptr ||
		    (enableHuber && cellDiagC == nullptr) ||
		    cellsW <= 0 || cellsH <= 0 || cellPx <= 0 ||
		    !std::isfinite(pxX) || !std::isfinite(pxY) || !std::isfinite(diff) || !std::isfinite(cleanY))
		{
			return 0.0;
		}
		const double gx = (pxX + 0.5) / static_cast<double>(cellPx) - 0.5;
		const double gy = (pxY + 0.5) / static_cast<double>(cellPx) - 0.5;
		const int x0 = (std::max)(0, (std::min)(cellsW - 1, static_cast<int>(std::floor(gx))));
		const int y0 = (std::max)(0, (std::min)(cellsH - 1, static_cast<int>(std::floor(gy))));
		const int x1 = (std::min)(cellsW - 1, x0 + 1);
		const int y1 = (std::min)(cellsH - 1, y0 + 1);
		const double tx = RampMath::Clamp01(gx - static_cast<double>(x0));
		const double ty = RampMath::Clamp01(gy - static_cast<double>(y0));

		const size_t i00 = static_cast<size_t>(y0) * cellsW + x0;
		const size_t i10 = static_cast<size_t>(y0) * cellsW + x1;
		const size_t i01 = static_cast<size_t>(y1) * cellsW + x0;
		const size_t i11 = static_cast<size_t>(y1) * cellsW + x1;

		const double w00 = cellWeights[i00], w10 = cellWeights[i10], w01 = cellWeights[i01], w11 = cellWeights[i11];
		if (!std::isfinite(w00) || !std::isfinite(w10) || !std::isfinite(w01) || !std::isfinite(w11))
			return 0.0;

		double t00 = w00, t10 = w10, t01 = w01, t11 = w11;
		if (enableHuber)
		{
			const double d00 = cellDiagC[i00], d10 = cellDiagC[i10], d01 = cellDiagC[i01], d11 = cellDiagC[i11];
			if (!std::isfinite(d00) || !std::isfinite(d10) || !std::isfinite(d01) || !std::isfinite(d11))
				return 0.0;
			t00 *= HuberPixelTaper(diff, d00);
			t10 *= HuberPixelTaper(diff, d10);
			t01 *= HuberPixelTaper(diff, d01);
			t11 *= HuberPixelTaper(diff, d11);
		}

		const double s00 = (1.0 - tx) * (1.0 - ty);
		const double s10 = tx * (1.0 - ty);
		const double s01 = (1.0 - tx) * ty;
		const double s11 = tx * ty;

		const double baseQuantSigma = std::sqrt((kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0);
		const double effQuantFloor = (std::isfinite(quantSigmaFloor) && quantSigmaFloor > baseQuantSigma)
			? quantSigmaFloor
			: baseQuantSigma;

		auto rangeAffinity = [&](size_t idx) -> double
		{
			const double m = cellMeanY[idx];
			const double sd = cellStdY[idx];
			if (!std::isfinite(m) || !std::isfinite(sd))
				return 1.0;
			const double sigL = sd > effQuantFloor ? sd : effQuantFloor;
			const double dL = cleanY - m;
			return std::exp(-(dL * dL) / (2.0 * sigL * sigL));
		};

		const double sw00 = s00 * rangeAffinity(i00);
		const double sw10 = s10 * rangeAffinity(i10);
		const double sw01 = s01 * rangeAffinity(i01);
		const double sw11 = s11 * rangeAffinity(i11);
		const double swSum = sw00 + sw10 + sw01 + sw11;
		if (swSum > kEngineNumFloor)
		{
			const double invSum = 1.0 / swSum;
			return (sw00 * t00 + sw10 * t10 + sw01 * t01 + sw11 * t11) * invSum;
		}
		return s00 * t00 + s10 * t10 + s01 * t01 + s11 * t11;
	}

	// ---- Full CPU Reference Fusion Engine (Stage TD-S3) ----
	class TdV2ReferenceEngine
	{
	public:
		explicit TdV2ReferenceEngine(const Config& cfg = Config{})
			: cfg_(cfg)
		{
		}

		void SetConfig(const Config& cfg)
		{
			if (cfg.mode == Mode::Off && hasCarry_)
				ResetCarry();
			cfg_ = cfg;
		}
		const Config& GetConfig() const { return cfg_; }

		void ResetCarry()
		{
			hasCarry_ = false;
			carryClean_ = TdV2Plane();
			hasAnchor_ = false;
			anchorClean_ = TdV2Plane();
			anchorAgeFrames_ = 0;
			anchorBarredCells_.clear();
			anchorReseedCells_.clear();
			hasLastSeq_ = false;
			lastSeq_ = 0;
			elapsedSec_ = 0.0;
			a2eHistory_.Reset();
			for (auto& slot : sameRoleStats_)
				slot.Reset();
		}

		bool HasCarry() const { return hasCarry_; }
		const TdV2Plane& CarryClean() const { return carryClean_; }
		bool HasAnchor() const { return hasAnchor_; }
		int AnchorAgeFrames() const { return anchorAgeFrames_; }
		const TdV2Plane& AnchorClean() const { return anchorClean_; }
		const std::vector<uint8_t>& AnchorBarredCells() const { return anchorBarredCells_; }
		const std::vector<uint8_t>& AnchorReseedCells() const { return anchorReseedCells_; }

		FrameProcessResult ProcessFrame(const FrameProcessInput& in, StandInRegistry* standIns = nullptr)
		{
			FrameProcessResult res;
			const int w = in.receivedY.w;
			const int h = in.receivedY.h;
			res.outputY = in.receivedY; // Default passthrough: bit-exact when Mode::Off or Mode::Shadow!
			if (w <= 0 || h <= 0 || in.receivedY.Empty())
				return res;

			const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
			const int grainCellPx = cfg_.grainCellPx > 0 ? cfg_.grainCellPx : cellPx;
			res.cellsW = (w + cellPx - 1) / cellPx;
			res.cellsH = (h + cellPx - 1) / cellPx;
			const size_t numCells = static_cast<size_t>(res.cellsW) * static_cast<size_t>(res.cellsH);
			res.tdMaps.resize(numCells);

			// Mode::Off -> reset any stale carry so Off->On never blends stale history, then byte-identical passthrough.
			if (!cfg_.IsEnabled())
			{
				if (hasCarry_)
				{
					ResetCarry();
					res.carryReset = true;
				}
				return res;
			}

			// Upfront sourceBits and psdShape validation (Rule 4: invalid inputs close cleanly and visibly).
			if (!(PeakCodeForBits(cfg_.sourceBits) > 0.0) || !in.psdShape.IsValid())
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure,
						!(PeakCodeForBits(cfg_.sourceBits) > 0.0)
							? "TdV2ReferenceEngine invalid sourceBits"
							: "TdV2ReferenceEngine invalid/non-finite psdShape");
				ResetCarry();
				res.carryReset = true;
				res.nonFiniteCells = numCells;
				return res;
			}

			// Visible telemetry when long-reach I-anchor is enabled in config without a long-pair producer.
			if (cfg_.enableLongAnchor && standIns != nullptr)
				standIns->Engage(StandInKind::LongPairDrift, "enableLongAnchor requested; 1-session long pair pending");

			// Resolve U7 gradual fadeKeep in [0, 1] (non-finite fails closed to 0.0 + NonFiniteClosure).
			bool fadeNonFinite = false;
			const double effFadeKeep = ResolveFadeKeep(in.fadeKeep, fadeNonFinite);
			if (fadeNonFinite)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ReferenceEngine non-finite fadeKeep; failing closed to 0.0");
				res.nonFiniteCells = numCells;
			}
			const bool effectiveCut = in.isCutOrSpanInvalid || (effFadeKeep <= 0.0);

			// Cut / invalid span, fadeKeep <= 0, resolution change, seek, or non-consecutive frame sequence resets carry & anchor cleanly.
			const bool seqDiscontinuous = (hasCarry_ || hasAnchor_) && hasLastSeq_ &&
				!(in.seq == 0 && lastSeq_ == 0) &&
				(in.seq != lastSeq_ + 1);
			const bool dimChanged = (hasCarry_ && (carryClean_.w != w || carryClean_.h != h)) ||
			                        (hasAnchor_ && (anchorClean_.w != w || anchorClean_.h != h));
			if (effectiveCut || seqDiscontinuous || dimChanged)
			{
				ResetCarry();
				res.carryReset = true;
			}

			// Validate receivedY finite; any non-finite input closes the frame cleanly with visible telemetry.
			for (double val : in.receivedY.v)
			{
				if (!std::isfinite(val))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ReferenceEngine non-finite receivedY");
					ResetCarry();
					res.carryReset = true;
					res.nonFiniteCells = numCells;
					return res;
				}
			}

			const ActiveRect active = in.activeRect.Empty()
				? ActiveRect::Full(w, h)
				: in.activeRect.ClampedTo(w, h);

			// Layer 1: Grain-free clean & own-grain separation (consistent `splitActive` across Layer 1 & Layer 4).
			const bool splitActive = in.hasCleanSplit && !in.cleanY.Empty() && in.cleanY.w == w && in.cleanY.h == h;
			TdV2Plane curClean(w, h, 0.0);
			TdV2Plane ownGrain(w, h, 0.0);
			if (splitActive)
			{
				for (size_t i = 0; i < in.receivedY.v.size(); ++i)
				{
					const double cv = in.cleanY.v[i];
					if (!std::isfinite(cv))
					{
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ReferenceEngine non-finite cleanY");
						ResetCarry();
						res.carryReset = true;
						res.nonFiniteCells = numCells;
						return res;
					}
					curClean.v[i] = cv;
					ownGrain.v[i] = in.receivedY.v[i] - cv; // Exact identity: receivedY = curClean + ownGrain
				}
			}
			else
			{
				// Visible P10g stand-in: without valid upstream grain split, operate with zero own-grain extraction
				// and run Layer-4 TdGuardPort unconditionally against inFineE.
				if (standIns != nullptr)
					standIns->Engage(StandInKind::P10gGrainSeparator, "no upstream P10g split supplied");
				curClean = in.receivedY;
			}
			res.outputClean = curClean;

			// Assemble available grain-free reference specifications (prevPeak, nextPeak, optional prevPeak2, recurrent carry)
			// and validate coding step prior BEFORE band-splitting or warping references (exact 1:1 parity with TdV2Gpu.h:
			// zero wasted work and borderBilinearPixels == 0 when refs are empty or !a2Stats.valid).
			struct RefSpec
			{
				const TdV2Plane* cleanPlane = nullptr;
				const CellMotionField* motion = nullptr;
				bool isExactCopy = false;
				RefRole role = RefRole::PrevPeak;
				Homography3x3 cameraHomography;
			};
			std::vector<RefSpec> refSpecs;
			refSpecs.reserve(kMaxBlendRefs);

			auto tryAddRefSpec = [&](const TdV2Plane* refClean, const CellMotionField* mf, bool exactCopy, RefRole role,
			                         const Homography3x3& camH = Homography3x3{}) -> bool
			{
				if (refClean == nullptr || refClean->Empty() || refClean->w != w || refClean->h != h)
					return false;
				if (static_cast<int>(refSpecs.size()) >= kMaxBlendRefs)
					return false;
				if (mf == nullptr)
				{
					// Plan s1.2 L108 (L4) & H9: Missing motion field MUST NEVER whole-frame blend as "Still".
					// Skip the reference visibly so moving content suffers ZERO ghosting.
					++res.motionStandInRefs;
					++res.missingMotionSkippedRefs;
					if (standIns != nullptr)
					{
						standIns->Engage(StandInKind::MotionFieldStandIn, "null motion field; ref skipped per L4");
						standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "L4 missing motion field: ref skipped (no whole-frame Still blend)");
					}
					return false;
				}
				if (!mf->IsWellFormed(res.cellsW, res.cellsH))
				{
					// Dimension-mismatched / malformed motion field: reject reference visibly (zero OOB crash!).
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "dimension-mismatched motion field; ref rejected");
					return false;
				}
				refSpecs.push_back(RefSpec{ refClean, mf, exactCopy, role, camH });
				return true;
			};

			int anchoredCarryRefIdx = -1;
			const int maxAnchorAge = cfg_.maxAnchorAgeFrames > 0 ? cfg_.maxAnchorAgeFrames : kContractBackRefs;
			if (hasAnchor_ && anchorAgeFrames_ > maxAnchorAge)
			{
				hasAnchor_ = false;
				anchorClean_ = TdV2Plane();
				anchorAgeFrames_ = 0;
				anchorBarredCells_.clear();
				anchorReseedCells_.clear();
			}

			if (!effectiveCut)
			{
				if (in.prevPeak.cleanPlane != nullptr)
					tryAddRefSpec(in.prevPeak.cleanPlane, in.prevPeak.motion, in.prevPeak.isExactCopyOfCurrent, RefRole::PrevPeak, in.prevPeak.cameraHomography);
				if (in.nextPeak.cleanPlane != nullptr)
					tryAddRefSpec(in.nextPeak.cleanPlane, in.nextPeak.motion, in.nextPeak.isExactCopyOfCurrent, RefRole::NextPeak, in.nextPeak.cameraHomography);
				if (in.prevPeak2.cleanPlane != nullptr)
					tryAddRefSpec(in.prevPeak2.cleanPlane, in.prevPeak2.motion, in.prevPeak2.isExactCopyOfCurrent, RefRole::PrevPeak2, in.prevPeak2.cameraHomography);
				// TD-U5 (H-4 / Owner D2 / UPG-HIGH-1 / RED-BUG-7a): Single-resample anchored carry from last confirmed peak
				// along composed vector, treated as a second past peak (PrevPeak2) for A1 (NEVER chained resample).
				// RED-BUG-7a: Detect same-GOP duplicate peak across the ENTIRE GOP span (age >= 1, not just age <= 1!)
				// by strided + exact plane equality so P_0 is NEVER self-instrumented against itself on valleys t=2..7!
				auto planesMatchExact = [](const TdV2Plane& a, const TdV2Plane& b) -> bool
				{
					if (a.w != b.w || a.h != b.h || a.Empty())
						return false;
					if (&a == &b)
						return true;
					const size_t n = a.v.size();
					const size_t stride = (std::max<size_t>)(1u, n / 64u);
					for (size_t i = 0; i < n; i += stride)
					{
						if (!(std::fabs(a.v[i] - b.v[i]) <= 1e-6))
							return false;
					}
					for (size_t i = 0; i < n; ++i)
					{
						if (!(std::fabs(a.v[i] - b.v[i]) <= 1e-6))
							return false;
					}
					return true;
				};
				const bool anchorDuplicatesPrevPeak =
					(in.prevPeak.cleanPlane != nullptr) &&
					(anchorAgeFrames_ <= 1 || planesMatchExact(anchorClean_, *in.prevPeak.cleanPlane));
				const bool anchorDuplicatesPrevPeak2 =
					(in.prevPeak2.cleanPlane != nullptr) &&
					planesMatchExact(anchorClean_, *in.prevPeak2.cleanPlane);
				if (cfg_.enableAnchoredCarry && hasAnchor_ && !anchorDuplicatesPrevPeak && !anchorDuplicatesPrevPeak2 &&
				    anchorClean_.w == w && anchorClean_.h == h &&
				    anchorAgeFrames_ >= 1 && anchorAgeFrames_ <= maxAnchorAge)
				{
					const CellMotionField* ancMf = in.hasAnchorComposedMotion ? &in.anchorComposedMotion : nullptr;
					if (tryAddRefSpec(&anchorClean_, ancMf, false, RefRole::PrevPeak2))
					{
						anchoredCarryRefIdx = static_cast<int>(refSpecs.size()) - 1;
						res.anchoredCarryUsed = true;
						res.carryUsed = true;
					}
				}
				else if (cfg_.enableCarryForward && hasCarry_ && carryClean_.w == w && carryClean_.h == h)
				{
					if (tryAddRefSpec(&carryClean_, in.hasCarryMotion ? &in.carryMotion : nullptr, false, RefRole::Carry))
						res.carryUsed = true;
				}
			}

			res.activeRefCount = refSpecs.size();
			if (refSpecs.empty())
			{
				// First frame / cut frame / all refs skipped: seed carry (and anchor if confirmed peak); output stays receivedY.
				carryClean_ = curClean;
				hasCarry_ = true;
				if (in.isConfirmedPeak)
				{
					anchorClean_ = curClean;
					hasAnchor_ = true;
					anchorAgeFrames_ = 1;
					anchorBarredCells_.assign(numCells, 0u);
					anchorReseedCells_.assign(numCells, 0u);
				}
				else if (hasAnchor_)
				{
					++anchorAgeFrames_;
				}
				hasLastSeq_ = true;
				lastSeq_ = in.seq;
				return res;
			}

			// Coding step prior:
			// - When hasStepPrior == false: true 1-LSB uniform output rounding (delta = 1.0 code, phi = 0.5, pi0 = 0),
			//   visibly tagged via StandInKind::StepPriorDefault.
			// - When hasStepPrior == true: validate finite positive stepDelta, deadZonePhi strictly in (0, 1),
			//   and zeroCoeffFraction in [0, 1]; invalid A2 stats close delta & validity map to 0 visibly!
			double effStepDelta = in.estimatedStepDelta;
			double effPhi = in.deadZonePhi;
			double effZeroFrac = in.zeroCoeffFraction;
			bool a2InputNonFinite = !std::isfinite(effPhi) || !std::isfinite(effZeroFrac);
			if (!in.hasStepPrior)
			{
				if (!a2InputNonFinite)
				{
					effStepDelta = kDefaultStepPriorLsb; // True 1-LSB quantization step in native integer code units
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
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ReferenceEngine non-finite A2 step/phi stats");
					standIns->Engage(StandInKind::A2StatsInvalid, "invalid stepDelta/deadZonePhi/zeroFrac; delta closed to 0");
				}
				res.a2InvalidCells = numCells;
				if (a2InputNonFinite)
					res.nonFiniteCells = numCells;
				for (size_t ci = 0; ci < numCells; ++ci)
				{
					res.tdMaps[ci].f[TdMapsView::kValidity] = 0.0f;
					res.tdMaps[ci].f[TdMapsView::kBandMask] = 0.0f;
					res.tdMaps[ci].f[TdMapsView::kLightGain] = 1.0f;
				}
				carryClean_ = curClean;
				hasCarry_ = true;
				hasLastSeq_ = true;
				lastSeq_ = in.seq;
				return res;
			}

			// Decompose current frame's grain-free clean into L, M, H, D3 via U2 BandFromBase.
			const ThreeBandPlanes curBands = DecomposeActiveBands(curClean, active, standIns);

			struct WarpedRef
			{
				TdV2Plane clean;
				ThreeBandPlanes bands;
				std::vector<uint8_t> validPx;
				MotionGateResult motionGate;
				std::vector<double> motionVx;
				std::vector<double> motionVy;
				bool isExactCopy = false;
				RefRole role = RefRole::PrevPeak;
			};
			std::vector<WarpedRef> refs;
			refs.reserve(refSpecs.size());
			std::vector<uint8_t> nonFiniteCellFlag(numCells, 0u);

			for (const RefSpec& spec : refSpecs)
			{
				WarpedRef wr;
				wr.isExactCopy = spec.isExactCopy;
				wr.role = spec.role;
				wr.motionVx = spec.motion->vx;
				wr.motionVy = spec.motion->vy;
				const WarpDiagnostics wDiag = WarpCleanReference(
					*spec.cleanPlane, curClean, active, spec.motion->vx, spec.motion->vy,
					res.cellsW, res.cellsH, cellPx, wr.clean, wr.validPx, standIns,
					&spec.motion->valid, &spec.motion->cls, &spec.cameraHomography, &nonFiniteCellFlag,
					cfg_.enableSmoothWarpVectors);
				res.borderBilinearPixels += wDiag.borderBilinearPixels;
				if (wDiag.nonFinitePixels > 0)
				{
					for (int y = active.y0; y < active.y1; ++y)
					{
						const int cy = (std::min)(res.cellsH - 1, y / cellPx);
						for (int x = active.x0; x < active.x1; ++x)
						{
							if (!std::isfinite(spec.cleanPlane->at(x, y)))
							{
								const int cx = (std::min)(res.cellsW - 1, x / cellPx);
								nonFiniteCellFlag[static_cast<size_t>(cy) * res.cellsW + cx] = 1u;
							}
						}
					}
				}
				wr.bands = DecomposeActiveBands(wr.clean, active, standIns);
				wr.motionGate = EvaluateCameraRelativeMotionGate(*spec.motion, standIns, cfg_.enableHardFbOcclusionGate);
				if (wr.motionGate.nonFiniteCells > 0)
				{
					for (size_t ci = 0; ci < numCells && ci < wr.motionGate.nonFiniteCellMask.size(); ++ci)
					{
						if (wr.motionGate.nonFiniteCellMask[ci] != 0u)
							nonFiniteCellFlag[ci] = 1u;
					}
				}
				refs.push_back(std::move(wr));
			}

			// Exact derived 2D separable white-noise transfer gains from U2 BandFromBase kernel taps (HIGH-1 + TD-U3/H-1:
			// exact band shares ||H_b||_2^2; the old filter-peak 1.0 full-variance amplification is removed).
			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double bandTransfer[3] = { gains.low, gains.mid, gains.fine };

			// Resolve per-cell AQ/PQ step scale upfront so FocusGuard, A2, pixel bounds, and 8x8 POCS share identical scales.
			// TD-U3 (M-6): PQ perceptual factor may ONLY tighten (<= 1.0); AQ widening (> 1.0) is gated behind cfg_.enableAqStepScaling (default false).
			const double maxAllowedStepScale = cfg_.enableAqStepScaling ? kCellStepScaleMax : 1.0;
			std::vector<double> cellStepScales(numCells, 1.0);
			if (!in.cellStepScale.empty())
			{
				for (size_t ci = 0; ci < numCells && ci < in.cellStepScale.size(); ++ci)
				{
					const double sv = in.cellStepScale[ci];
					if (std::isfinite(sv) && sv > 0.0)
					{
						cellStepScales[ci] = sv < kCellStepScaleMin ? kCellStepScaleMin : (sv > maxAllowedStepScale ? maxAllowedStepScale : sv);
					}
					else
					{
						cellStepScales[ci] = 0.0;
						nonFiniteCellFlag[ci] = 1u;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "non-finite or non-positive cellStepScale");
					}
				}
			}

			// ---- TD-U5 (H-4 / Owner D2): Per-Cell Held-Out Structure Check on Anchored Carry & Real kDrift Audit ----
			// 1) Held-out structure check on anchored carry BEFORE use:
			//    - Mid-band normalized correlation (NCC) between warped anchor M-band and received clean M-band
			//      (strictly invariant to amplitude scaling, so mid-band detail loss alone NEVER fails it).
			//    - Coarse-band mean difference within the U3 limit |mean(L_anc) - mean(L_cur)| <= kappa * sqrt(U_L).
			//    - Failing cells get carry weight 0 and are marked in anchorReseedCells_ for re-seed at the next peak.
			const double depthScaleDrift = DepthScale(cfg_.sourceBits);
			const double baseQuantVarDrift = (depthScaleDrift * depthScaleDrift) / 12.0;
			const double midNoiseFloorHeldOut = 2.0 * gains.mid * baseQuantVarDrift;
			const double effKappaHeldOut = ClampPixelBoundKappa(cfg_.pixelBoundKappa);
			if (anchoredCarryRefIdx >= 0 && anchoredCarryRefIdx < static_cast<int>(refs.size()))
			{
				WarpedRef& ancRef = refs[static_cast<size_t>(anchoredCarryRefIdx)];
				if (anchorReseedCells_.size() != numCells)
					anchorReseedCells_.assign(numCells, 0u);
				for (int cy = 0; cy < res.cellsH; ++cy)
				{
					const int y0 = (std::max)(active.y0, cy * cellPx);
					const int y1 = (std::min)(active.y1, (cy + 1) * cellPx);
					if (y1 <= y0) continue;
					for (int cx = 0; cx < res.cellsW; ++cx)
					{
						const int x0 = (std::max)(active.x0, cx * cellPx);
						const int x1 = (std::min)(active.x1, (cx + 1) * cellPx);
						if (x1 <= x0) continue;
						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;

						// Barred from previous peak's drift audit (> kAnchorDriftBarPx) or prior held-out failure
						// awaiting next confirmed peak re-seed (UPG-HIGH-2): force anchor weight strictly 0.
						if ((cIdx < anchorBarredCells_.size() && anchorBarredCells_[cIdx] != 0u) ||
						    (cIdx < anchorReseedCells_.size() && anchorReseedCells_[cIdx] != 0u))
						{
							ancRef.motionGate.gateL[cIdx] = 0.0;
							ancRef.motionGate.gateMH[cIdx] = 0.0;
							if (cIdx < anchorReseedCells_.size() && anchorReseedCells_[cIdx] != 0u)
								++res.anchoredCarryHeldOutRejectCells;
							continue;
						}
						if (ancRef.motionGate.gateL[cIdx] <= 0.0 && ancRef.motionGate.gateMH[cIdx] <= 0.0)
							continue;

						double sumCurM = 0.0, sumAncM = 0.0, sumCurL = 0.0, sumAncL = 0.0;
						size_t cnt = 0;
						for (int y = y0; y < y1; ++y)
						{
							for (int x = x0; x < x1; ++x)
							{
								const size_t pIdx = static_cast<size_t>(y) * w + x;
								if (ancRef.validPx[pIdx] == 0u)
									continue;
								sumCurM += curBands.M.at(x, y);
								sumAncM += ancRef.bands.M.at(x, y);
								sumCurL += curBands.L.at(x, y);
								sumAncL += ancRef.bands.L.at(x, y);
								++cnt;
							}
						}
						if (cnt == 0)
						{
							ancRef.motionGate.gateL[cIdx] = 0.0;
							ancRef.motionGate.gateMH[cIdx] = 0.0;
							anchorReseedCells_[cIdx] = 1u;
							++res.anchoredCarryHeldOutRejectCells;
							continue;
						}
						const double invN = 1.0 / static_cast<double>(cnt);
						const double muCurM = sumCurM * invN;
						const double muAncM = sumAncM * invN;
						const double muCurL = sumCurL * invN;
						const double muAncL = sumAncL * invN;

						double varCurM = 0.0, varAncM = 0.0, covM = 0.0;
						for (int y = y0; y < y1; ++y)
						{
							for (int x = x0; x < x1; ++x)
							{
								const size_t pIdx = static_cast<size_t>(y) * w + x;
								if (ancRef.validPx[pIdx] == 0u)
									continue;
								const double dc = curBands.M.at(x, y) - muCurM;
								const double da = ancRef.bands.M.at(x, y) - muAncM;
								varCurM += dc * dc;
								varAncM += da * da;
								covM += dc * da;
							}
						}
						varCurM *= invN;
						varAncM *= invN;
						covM *= invN;

						bool structOk = true;
						if (varCurM > midNoiseFloorHeldOut && varAncM > midNoiseFloorHeldOut)
						{
							const double ncc = covM / std::sqrt(varCurM * varAncM);
							// Derived 3-sigma zero-correlation significance floor: 3 / sqrt(N_eff,M), capped at 0.5.
							const double nEffM = EffectiveSamplesForBand(1, static_cast<double>(cnt));
							const double nccMin = (std::min)(0.5, std::sqrt(kHuberKneeSq) / std::sqrt(nEffM));
							if (!std::isfinite(ncc) || ncc < nccMin)
								structOk = false;
						}
						const double cScale = cellStepScales[cIdx];
						const double coarseVar = (std::max)(a2Stats.totalVar * cScale * cScale, baseQuantVarDrift) * gains.low;
						const double coarseLimit = effKappaHeldOut * std::sqrt(coarseVar);
						if (std::fabs(muAncL - muCurL) > coarseLimit)
							structOk = false;

						if (!structOk)
						{
							ancRef.motionGate.gateL[cIdx] = 0.0;
							ancRef.motionGate.gateMH[cIdx] = 0.0;
							anchorReseedCells_[cIdx] = 1u;
							++res.anchoredCarryHeldOutRejectCells;
						}
					}
				}
			}

			// 2) Real kDrift measurement at every confirmed peak (Plan Phase 3 / Fable H-4 / 99 s2.5 fallback a):
			//    Compares direct peak-to-peak vector against composed trail vector (or falls back visibly to a seeded
			//    +/- 2 px local block search with "drift: reduced" tag). Written into td.maps kDrift; drifted cells
			//    (> kHardFbOcclusionPx = 1.0 px) are barred from carry for the next span!
			std::vector<float> cellDriftPx(numCells, 0.0f);
			std::vector<uint8_t> nextSpanBarredCells(numCells, 0u);
			if (in.isConfirmedPeak && !effectiveCut && in.hasAnchorComposedMotion &&
			    in.anchorComposedMotion.IsWellFormed(res.cellsW, res.cellsH))
			{
				if (in.hasDirectPeakToPeakMotion && in.directPeakToPeakMotion.IsWellFormed(res.cellsW, res.cellsH))
				{
					for (size_t ci = 0; ci < numCells; ++ci)
					{
						if (in.anchorComposedMotion.valid[ci] == 0u || in.directPeakToPeakMotion.valid[ci] == 0u)
							continue;
						const double dvx = in.directPeakToPeakMotion.vx[ci] - in.anchorComposedMotion.vx[ci];
						const double dvy = in.directPeakToPeakMotion.vy[ci] - in.anchorComposedMotion.vy[ci];
						if (!std::isfinite(dvx) || !std::isfinite(dvy))
							continue;
						const double drift = std::hypot(dvx, dvy);
						cellDriftPx[ci] = static_cast<float>(drift);
						if (drift > kAnchorDriftBarPx)
						{
							nextSpanBarredCells[ci] = 1u;
							++res.driftBarredCells;
						}
					}
				}
				else if (hasAnchor_ && anchorClean_.w == w && anchorClean_.h == h)
				{
					// Seeded local block search (+/- 2 px around exact floating-point composed trail + quadratic sub-pixel peak)
					// with visible tag. Samples anchorClean_ bilinearly at exact (x + baseVx + dx, y + baseVy + dy) so sub-pixel
					// composed bias (e.g. 8 * 0.05 = 0.40 px) is NEVER erased by integer rounding (UPG-HIGH-2).
					res.driftReducedSearchUsed = true;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::LongPairDrift, "drift: reduced (seeded local block search around composed trail)");
					const int kSearchRad = 2;
					for (int cy = 0; cy < res.cellsH; ++cy)
					{
						const int y0 = (std::max)(active.y0, cy * cellPx);
						const int y1 = (std::min)(active.y1, (cy + 1) * cellPx);
						if (y1 <= y0) continue;
						for (int cx = 0; cx < res.cellsW; ++cx)
						{
							const int x0 = (std::max)(active.x0, cx * cellPx);
							const int x1 = (std::min)(active.x1, (cx + 1) * cellPx);
							if (x1 <= x0) continue;
							const size_t ci = static_cast<size_t>(cy) * res.cellsW + cx;
							if (in.anchorComposedMotion.valid[ci] == 0u)
								continue;
							const double baseVx = in.anchorComposedMotion.vx[ci];
							const double baseVy = in.anchorComposedMotion.vy[ci];
							if (!std::isfinite(baseVx) || !std::isfinite(baseVy))
								continue;

							double absDiffGrid[5][5] = {};
							int bestDx = 0, bestDy = 0;
							double bestDiff = std::numeric_limits<double>::infinity();
							for (int dy = -kSearchRad; dy <= kSearchRad; ++dy)
							{
								for (int dx = -kSearchRad; dx <= kSearchRad; ++dx)
								{
									double absDiffSum = 0.0;
									size_t cnt = 0;
									for (int y = y0; y < y1; y += 2)
									{
										const double sy = static_cast<double>(y) + baseVy + static_cast<double>(dy);
										if (sy < static_cast<double>(active.y0) || sy > static_cast<double>(active.y1 - 1)) continue;
										const int iy0 = static_cast<int>(std::floor(sy));
										const int iy1 = (std::min)(active.y1 - 1, iy0 + 1);
										const double fy = sy - static_cast<double>(iy0);
										for (int x = x0; x < x1; x += 2)
										{
											const double sx = static_cast<double>(x) + baseVx + static_cast<double>(dx);
											if (sx < static_cast<double>(active.x0) || sx > static_cast<double>(active.x1 - 1)) continue;
											const int ix0 = static_cast<int>(std::floor(sx));
											const int ix1 = (std::min)(active.x1 - 1, ix0 + 1);
											const double fx = sx - static_cast<double>(ix0);
											const double a00 = anchorClean_.at(ix0, iy0);
											const double a10 = anchorClean_.at(ix1, iy0);
											const double a01 = anchorClean_.at(ix0, iy1);
											const double a11 = anchorClean_.at(ix1, iy1);
											const double aVal = (1.0 - fy) * ((1.0 - fx) * a00 + fx * a10) +
											                    fy * ((1.0 - fx) * a01 + fx * a11);
											absDiffSum += std::fabs(curClean.at(x, y) - aVal);
											++cnt;
										}
									}
									const double meanDiff = (cnt > 0) ? (absDiffSum / static_cast<double>(cnt)) : std::numeric_limits<double>::infinity();
									absDiffGrid[dy + kSearchRad][dx + kSearchRad] = meanDiff;
									if (meanDiff < bestDiff)
									{
										bestDiff = meanDiff;
										bestDx = dx;
										bestDy = dy;
									}
								}
							}
							double subDx = static_cast<double>(bestDx);
							double subDy = static_cast<double>(bestDy);
							if (bestDx > -kSearchRad && bestDx < kSearchRad)
							{
								const double cL = absDiffGrid[bestDy + kSearchRad][bestDx + kSearchRad - 1];
								const double c0 = absDiffGrid[bestDy + kSearchRad][bestDx + kSearchRad];
								const double cR = absDiffGrid[bestDy + kSearchRad][bestDx + kSearchRad + 1];
								const double den = cL - 2.0 * c0 + cR;
								if (std::isfinite(den) && den > kEngineNumFloor)
									subDx += (std::max)(-0.5, (std::min)(0.5, 0.5 * (cL - cR) / den));
							}
							if (bestDy > -kSearchRad && bestDy < kSearchRad)
							{
								const double cU = absDiffGrid[bestDy + kSearchRad - 1][bestDx + kSearchRad];
								const double c0 = absDiffGrid[bestDy + kSearchRad][bestDx + kSearchRad];
								const double cD = absDiffGrid[bestDy + kSearchRad + 1][bestDx + kSearchRad];
								const double den = cU - 2.0 * c0 + cD;
								if (std::isfinite(den) && den > kEngineNumFloor)
									subDy += (std::max)(-0.5, (std::min)(0.5, 0.5 * (cU - cD) / den));
							}
							const double drift = std::hypot(subDx, subDy);
							cellDriftPx[ci] = static_cast<float>(drift);
							if (drift > kAnchorDriftBarPx)
							{
								nextSpanBarredCells[ci] = 1u;
								++res.driftBarredCells;
							}
						}
					}
				}
			}

			// Evaluate physics-derived frequency-split focus guard across all aligned references.
			std::vector<ThreeBandPlanes> refBandList;
			refBandList.reserve(refs.size());
			for (const auto& wr : refs)
				refBandList.push_back(wr.bands);
			const auto focusCells = EvaluateFocusGuardStandIn(
				curBands, refBandList, active, res.cellsW, res.cellsH, cellPx, cfg_.sourceBits,
				a2Stats.valid ? a2Stats.totalVar : 0.0,
				in.explicitFocusChangeCells, standIns, in.explicitDefocusedCells, &cellStepScales);

			const int K = static_cast<int>(refs.size());
			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> cellW;
			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> cellDiagC;
			std::array<std::vector<double>, 3> cellOwnErrU;
			for (int b = 0; b < 3; ++b)
			{
				cellOwnErrU[b].assign(numCells, 0.0);
				for (int k = 0; k < K; ++k)
				{
					cellW[b][k].assign(numCells, 0.0);
					cellDiagC[b][k].assign(numCells, 0.0);
				}
			}

			auto bandPlaneOf = [](const ThreeBandPlanes& bp, int b) -> const TdV2Plane&
			{
				return (b == 0) ? bp.L : ((b == 1) ? bp.M : bp.H);
			};

			const int gW = (w + grainCellPx - 1) / grainCellPx;
			std::vector<double> cellRetainedGrain(numCells, 0.0);
			std::vector<double> cellMeanCleanY(numCells, 0.0);
			std::vector<double> cellStdCleanY(numCells, 0.0);

			const bool faceMaskSizeMismatch = (in.faceMask != nullptr && !in.faceMask->empty() && in.faceMask->size() != numCells);
			if (faceMaskSizeMismatch && standIns != nullptr)
			{
				standIns->Engage(
					StandInKind::FaceMaskSizeMismatch,
					in.faceMask->size() < numCells
						? "TdV2ReferenceEngine faceMask undersized vs cell grid; masking trailing cells fail-closed"
						: "TdV2ReferenceEngine faceMask oversized vs cell grid");
			}
			auto isFaceMaskedCell = [&](size_t cellIdx) -> bool
			{
				if (in.faceMask == nullptr || in.faceMask->empty())
					return false;
				if (cellIdx >= in.faceMask->size())
					return true;
				return (*in.faceMask)[cellIdx] != 0u;
			};

			// Role-based independent A1 instrument partner lookup (NEVER allows Carry <-> PrevPeak/PrevPeak2/NextPeak
			// cross-instrumentation since Carry shares recursive past error; plan s1.1 L70 & MEDIUM audit).
			auto findIndependentA1Partner = [&](int k, size_t cIdx, int b) -> int
			{
				const RefRole myRole = refs[k].role;
				int primary = -1;
				int secondary = -1;
				for (int j = 0; j < K; ++j)
				{
					if (j == k) continue;
					// RED-BUG-7b: Exclude any ref with motionGate <= 0 on this cell/band (drift-barred, held-out-rejected, or occluded).
					const double jGate = (b == 0) ? refs[j].motionGate.gateL[cIdx] : refs[j].motionGate.gateMH[cIdx];
					if (!(jGate > 0.0)) continue;
					const RefRole other = refs[j].role;
					if (other == RefRole::Carry)
						continue; // Carry is NEVER a valid A1 instrument for any reference!
					if (myRole == RefRole::PrevPeak)
					{
						if (other == RefRole::NextPeak) primary = j;
						else if (other == RefRole::PrevPeak2) secondary = j;
					}
					else if (myRole == RefRole::NextPeak)
					{
						if (other == RefRole::PrevPeak) primary = j;
						else if (other == RefRole::PrevPeak2) secondary = j;
					}
					else if (myRole == RefRole::PrevPeak2)
					{
						if (other == RefRole::NextPeak) primary = j;
						else if (other == RefRole::PrevPeak) secondary = j;
					}
					else if (myRole == RefRole::Carry)
					{
						// Future peak is independent of past recurrent carry.
						if (other == RefRole::NextPeak) primary = j;
					}
				}
				return primary >= 0 ? primary : secondary;
			};

			// Single-source-of-truth Owner Focus Rule & BokehMode band veto (Plan s1.4 L135-L143 & H8):
			// - Genuine focal-plane shift (focusChange == true): hard-vetoes ALL bands (L, M, H).
			// - Upstream kFocusMap matched-focus optical bokeh (!focusChange && isExplicitBokeh): hard-vetoes M & H always;
			//   L band is allowed ONLY under BokehMode::LowBandCellOnly and vetoed under BokehMode::Untouched.
			// - Sharp scenes, smooth curved skies, & 12-bit smooth ramps (!focusChange && !isExplicitBokeh): all bands allowed.
			auto isFocusVetoedBand = [&](size_t cellIndex, int bandIdx) -> bool
			{
				const FocusCellInfo& fi = focusCells[cellIndex];
				if (fi.focusChange)
					return true;
				if (fi.isExplicitBokeh)
				{
					if (bandIdx >= 1)
						return true;
					return cfg_.bokehMode != BokehMode::LowBandCellOnly;
				}
				return false;
			};

			// ---- Stage 2A: Per-cell spatial stats, aligned sharpness energies, pooled FP32 moments & raw grain-corrected c_IV ----
			struct CellBandMoments
			{
				double C[kMaxBlendRefs][kMaxBlendRefs] = {};
				double cIvRaw[kMaxBlendRefs] = {};      // Grain-corrected unbounded c_IV (before [0, min(C_pp, C_nn)] clip)
				double cIvClipped[kMaxBlendRefs] = {};  // Clipped c_A1 in [0, min(C_pp, C_nn)]
				double cIvSeSq[kMaxBlendRefs] = {};     // Wishart sampling variance se^2 = (C_pp C_nn + C_pn^2) / N_eff
				double cIvUpperCap[kMaxBlendRefs] = {}; // Physical upper cap min(C_pp, C_nn)
				bool hasIndependentPartner[kMaxBlendRefs] = {};
				size_t sampleCount = 0;
			};

			const double depthScale = DepthScale(cfg_.sourceBits);
			const double baseQuantVar = (depthScale * depthScale) / 12.0;
			const double activityTexFloor = 2.0 * (gains.mid + gains.fine) * baseQuantVar;

			std::vector<double> cellSpatialDetailRms(numCells, 0.0);
			std::vector<double> cellOwnGrainVarArr(numCells, 0.0);
			std::vector<uint8_t> cellActivityClass(numCells, 0u); // 0 = flat, 1 = textured
			std::vector<double> cellCurMidE(numCells, 0.0);
			std::vector<double> cellCurFineE(numCells, 0.0);
			std::vector<double> cellPrevMidE(numCells, 0.0);
			std::vector<double> cellPrevFineE(numCells, 0.0);
			std::vector<uint8_t> cellFocusFlagged(numCells, 0u);
			std::array<std::vector<CellBandMoments>, 3> cellMoments;
			for (int b = 0; b < 3; ++b)
				cellMoments[b].resize(numCells);

			int prevRefIdx = -1;
			for (int k = 0; k < K; ++k)
			{
				if (refs[k].role == RefRole::PrevPeak)
				{
					prevRefIdx = k;
					break;
				}
			}

			std::array<std::array<std::vector<double>, kA2eActivityClasses>, 3> a2eSamples;

			for (int cy = 0; cy < res.cellsH; ++cy)
			{
				for (int cx = 0; cx < res.cellsW; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					const FocusCellInfo& fInfo = focusCells[cIdx];
					if (fInfo.focusChange || fInfo.isExplicitBokeh)
					{
						++res.focusVetoCells;
						cellFocusFlagged[cIdx] = 1u;
					}

					// Map TD cell to P10/P10g retained grain variance cell (dynamic P10-D6 mapping).
					double cellRetainedGrainVar = 0.0;
					if (!in.retainedGrainVar.empty())
					{
						const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
						const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
						const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
						if (gIdx < in.retainedGrainVar.size())
						{
							const double gv = in.retainedGrainVar[gIdx];
							if (std::isfinite(gv) && gv >= 0.0)
							{
								cellRetainedGrainVar = gv;
							}
							else
							{
								nonFiniteCellFlag[cIdx] = 1u;
								if (standIns != nullptr)
									standIns->Engage(StandInKind::NonFiniteClosure, "non-finite retainedGrainVar");
							}
						}
						else if (standIns != nullptr)
						{
							standIns->Engage(StandInKind::GrainPriorOutOfRange, "TdV2ReferenceEngine retainedGrainVar index out of range");
						}
					}
					cellRetainedGrain[cIdx] = cellRetainedGrainVar;

					// Compute spatial mid+fine detail energy, own-grain variance, and clean-luma mean/std.
					double spatialMidEnergy = 0.0;
					double spatialFineEnergy = 0.0;
					double prevMidEnergy = 0.0;
					double prevFineEnergy = 0.0;
					double cellOwnGrainEnergy = 0.0;
					double cleanYMean = 0.0;
					double cleanYM2 = 0.0;
					size_t spatialPxCnt = 0;
					{
						const int sy0 = (std::max)(active.y0, cy * cellPx);
						const int sy1 = (std::min)(active.y1, (cy + 1) * cellPx);
						const int sx0 = (std::max)(active.x0, cx * cellPx);
						const int sx1 = (std::min)(active.x1, (cx + 1) * cellPx);
						for (int y = sy0; y < sy1; ++y)
						{
							for (int x = sx0; x < sx1; ++x)
							{
								const double mv = curBands.M.at(x, y);
								const double hv = curBands.H.at(x, y);
								spatialMidEnergy += mv * mv;
								spatialFineEnergy += hv * hv;
								if (prevRefIdx >= 0)
								{
									const double pmv = refs[prevRefIdx].bands.M.at(x, y);
									const double phv = refs[prevRefIdx].bands.H.at(x, y);
									prevMidEnergy += pmv * pmv;
									prevFineEnergy += phv * phv;
								}
								if (splitActive)
								{
									const double og = ownGrain.at(x, y);
									cellOwnGrainEnergy += og * og;
								}
								++spatialPxCnt;
								const double cyv = curClean.at(x, y);
								const double dY = cyv - cleanYMean;
								cleanYMean += dY / static_cast<double>(spatialPxCnt);
								cleanYM2 += dY * (cyv - cleanYMean);
							}
						}
					}
					const double invSpatialCnt = spatialPxCnt > 0 ? (1.0 / static_cast<double>(spatialPxCnt)) : 0.0;
					cellMeanCleanY[cIdx] = cleanYMean;
					cellStdCleanY[cIdx] = spatialPxCnt > 0
						? std::sqrt((std::max)(0.0, cleanYM2 * invSpatialCnt))
						: 0.0;
					const double curMVar = spatialMidEnergy * invSpatialCnt;
					const double curHVar = spatialFineEnergy * invSpatialCnt;
					const double spatialDetailRms = std::sqrt((std::max)(0.0, curHVar));
					cellSpatialDetailRms[cIdx] = spatialDetailRms;
					cellCurMidE[cIdx] = curMVar;
					cellCurFineE[cIdx] = curHVar;
					cellPrevMidE[cIdx] = prevMidEnergy * invSpatialCnt;
					cellPrevFineE[cIdx] = prevFineEnergy * invSpatialCnt;
					cellOwnGrainVarArr[cIdx] = (splitActive && spatialPxCnt > 0)
						? (cellOwnGrainEnergy * invSpatialCnt)
						: 0.0;

					const double cleanTexMH = (std::max)(0.0, curMVar - cellRetainedGrainVar * gains.mid * in.psdShape.ForBand(1)) +
					                          (std::max)(0.0, curHVar - cellRetainedGrainVar * gains.fine * in.psdShape.ForBand(2));
					const uint8_t actClass = (cleanTexMH > activityTexFloor) ? 1u : 0u;
					cellActivityClass[cIdx] = actClass;

					if (isFaceMaskedCell(cIdx))
					{
						++res.faceMaskedCells;
						const double cellStepScaleVal = cellStepScales[cIdx];
						const double cellStepScaleSq = cellStepScaleVal * cellStepScaleVal;
						TdMapsView::Block& mBlock = res.tdMaps[cIdx];
						mBlock.f[TdMapsView::kValidity] = 0.0f;
						mBlock.f[TdMapsView::kTrailConfidence0] = 0.0f;
						mBlock.f[TdMapsView::kTrailConfidence1] = 0.0f;
						mBlock.f[TdMapsView::kResidual] = 0.0f;
						mBlock.f[TdMapsView::kRealDetail] = static_cast<float>(spatialDetailRms);
						mBlock.f[TdMapsView::kStability] = 0.0f;
						mBlock.f[TdMapsView::kOwnGrain] = static_cast<float>(std::sqrt((std::max)(0.0, cellRetainedGrainVar)));
						mBlock.f[TdMapsView::kCodingNoise] = static_cast<float>(std::sqrt((std::max)(0.0, a2Stats.totalVar * cellStepScaleSq)));
						mBlock.f[TdMapsView::kLightGain] = 1.0f;
						mBlock.f[TdMapsView::kLightOffset] = 0.0f;
						mBlock.f[TdMapsView::kDrift] = 0.0f;
						mBlock.f[TdMapsView::kBandMask] = 0.0f;
						continue;
					}

					for (int b = 0; b < 3; ++b)
					{
						const int bandPoolR = cfg_.PoolRadiusForBand(b);
						const TdV2Plane& curB = bandPlaneOf(curBands, b);
						double muWelford[kMaxBlendRefs] = {};
						double scatterWelford[kMaxBlendRefs][kMaxBlendRefs] = {};
						size_t sampleCount = 0;
						double ownSumSq[kMaxBlendRefs] = {};
						size_t ownSampleCount = 0;

						const int py0 = (std::max)(active.y0, (cy - bandPoolR) * cellPx);
						const int py1 = (std::min)(active.y1, (cy + bandPoolR + 1) * cellPx);
						const int px0 = (std::max)(active.x0, (cx - bandPoolR) * cellPx);
						const int px1 = (std::min)(active.x1, (cx + bandPoolR + 1) * cellPx);

						// RED-BUG-7b: Only require validPx & gate > 0 for refs active on the center cell cIdx,
						// so a drift-barred / held-out-rejected anchor (or occluded ref) NEVER zeroes out valid peers!
						bool cellRefActive[kMaxBlendRefs] = {};
						bool anyCellRefActive = false;
						int centerMask = 0;
						for (int k = 0; k < K; ++k)
						{
							const double cGate = (b == 0) ? refs[k].motionGate.gateL[cIdx] : refs[k].motionGate.gateMH[cIdx];
							cellRefActive[k] = (cGate > 0.0);
							if (cellRefActive[k])
							{
								anyCellRefActive = true;
								centerMask |= (1 << k);
							}
						}
						if (!anyCellRefActive)
							continue;

						for (int y = py0; y < py1; ++y)
						{
							const int pcy = (std::min)(res.cellsH - 1, y / cellPx);
							for (int x = px0; x < px1; ++x)
							{
								const int pcx = (std::min)(res.cellsW - 1, x / cellPx);
								const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
								if (isFaceMaskedCell(pCell) || isFocusVetoedBand(pCell, b))
									continue;

								int nbrMask = 0;
								for (int k = 0; k < K; ++k)
								{
									const double pGate = (b == 0) ? refs[k].motionGate.gateL[pCell] : refs[k].motionGate.gateMH[pCell];
									if (pGate > 0.0)
										nbrMask |= (1 << k);
								}
								if ((nbrMask & centerMask) != centerMask)
									continue;

								const size_t pIdx = static_cast<size_t>(y) * w + x;
								bool allValid = true;
								double diffs[kMaxBlendRefs] = {};
								const double curV = curB.at(x, y);
								for (int k = 0; k < K; ++k)
								{
									if ((nbrMask & (1 << k)) == 0)
									{
										diffs[k] = 0.0;
										continue;
									}
									if (refs[k].validPx[pIdx] == 0u)
									{
										allValid = false;
										break;
									}
									const double dVal = curV - bandPlaneOf(refs[k].bands, b).at(x, y);
									if (!std::isfinite(dVal))
									{
										allValid = false;
										break;
									}
									diffs[k] = cellRefActive[k] ? dVal : 0.0;
								}
								if (allValid)
								{
									++sampleCount;
									const double invN = 1.0 / static_cast<double>(sampleCount);
									double deltaPrev[kMaxBlendRefs] = {};
									for (int i = 0; i < K; ++i)
									{
										deltaPrev[i] = diffs[i] - muWelford[i];
										muWelford[i] += deltaPrev[i] * invN;
									}
									for (int i = 0; i < K; ++i)
									{
										for (int j = i; j < K; ++j)
											scatterWelford[i][j] += deltaPrev[i] * (diffs[j] - muWelford[j]);
									}
									if (pCell == cIdx)
									{
										++ownSampleCount;
										for (int i = 0; i < K; ++i)
											ownSumSq[i] += diffs[i] * diffs[i];
									}
								}
							}
						}

						if (sampleCount == 0)
							continue;

						CellBandMoments& cm = cellMoments[b][cIdx];
						cm.sampleCount = sampleCount;
						const double invSampleN = 1.0 / static_cast<double>(sampleCount);
						for (int i = 0; i < K; ++i)
						{
							for (int j = i; j < K; ++j)
							{
								const double m2 = scatterWelford[i][j] * invSampleN + muWelford[i] * muWelford[j];
								const double val = (i == j && m2 < 0.0) ? 0.0 : m2;
								cm.C[i][j] = val;
								cm.C[j][i] = val;
							}
						}

						for (int k = 0; k < K; ++k)
						{
							const double ownDiag = (ownSampleCount > 0)
								? (ownSumSq[k] / static_cast<double>(ownSampleCount))
								: cm.C[k][k];
							cellDiagC[b][k][cIdx] = (std::min)(ownDiag, cm.C[k][k]);
						}

						// Precompute grain-corrected three-cornered-hat c_IV per ref for A2e frame prior pooling.
						// PROD-ALGO-BUG-10: Derived collinearity floor rejects exact/near-duplicate copies (|R0-R1| < 0.05 LSB)
						// without falsely rejecting genuinely independent clean peaks on heavily damaged B-frames!
						const double collinearFloor = 0.01 * baseQuantVar * bandTransfer[b];
						auto refsCollinear = [collinearFloor](double ckk, double cjj, double ckj) -> bool
						{
							const double mx = (std::max)(ckk, cjj);
							return (mx > kEngineNumFloor) && ((ckk + cjj - 2.0 * ckj) <= (collinearFloor + 1e-6 * mx));
						};
						const double psdScale = in.psdShape.ForBand(b);
						bool cellContributedA2e = false;
						for (int k = 0; k < K; ++k)
						{
							if (!cellRefActive[k])
								continue;
							const int partner = findIndependentA1Partner(k, cIdx, b);
							if (partner >= 0 && !refsCollinear(cm.C[k][k], cm.C[partner][partner], cm.C[k][partner]))
							{
								cm.hasIndependentPartner[k] = true;
								double subpixelBias = 0.0;
								if (cfg_.enableSubpixelHAttenuationDebias && b == 2 &&
								    cIdx < refs[k].motionVx.size() && cIdx < refs[partner].motionVx.size())
								{
									subpixelBias = ComputeSubpixelHAttenuationBias(
										refs[k].motionVx[cIdx], refs[k].motionVy[cIdx],
										refs[partner].motionVx[cIdx], refs[partner].motionVy[cIdx],
										curHVar, cellRetainedGrainVar * bandTransfer[2] * psdScale);
								}
								const double grainTerm = (std::max)(0.0, cellRetainedGrainVar) * bandTransfer[b] * (std::max)(0.0, psdScale);
								const double rawIv = cm.C[k][partner] - grainTerm - (std::max)(0.0, subpixelBias);
								const double nEff = EffectiveSamplesForBand(b, static_cast<double>(cm.sampleCount));
								cm.cIvRaw[k] = std::isfinite(rawIv) ? rawIv : 0.0;
								cm.cIvUpperCap[k] = (std::max)(0.0, (std::min)(cm.C[k][k], cm.C[partner][partner]));
								cm.cIvSeSq[k] = SamplingVarianceOfCrossMoment(cm.C[k][k], cm.C[partner][partner], cm.C[k][partner], nEff);
								cm.cIvClipped[k] = EstimateC_A1(cm.C[k][partner], cm.C[k][k], cm.C[partner][partner],
								                                cellRetainedGrainVar, bandTransfer[b], 1.0, psdScale, subpixelBias);
								if (!cellContributedA2e && !refs[k].isExactCopy && !isFocusVetoedBand(cIdx, b))
								{
									const double mGate = (b == 0) ? refs[k].motionGate.gateL[cIdx] : refs[k].motionGate.gateMH[cIdx];
									if (mGate > 0.0 && std::isfinite(cm.cIvClipped[k]))
									{
										a2eSamples[b][actClass].push_back(cm.cIvClipped[k]);
										cellContributedA2e = true;
									}
								}
							}
						}
					}
				}
			}

			// ---- TD-U4 (Owner D4(b) / UPG-BLKR-1 / UPG-MED-2): Optional 2-Frame Same-Role Statistics-Only Pooling (DEFAULT OFF) ----
			// Pools ONLY cell statistics (C, c_raw - NEVER pixels or weights!) across at most 2 recent frames of the same role:
			//   Peak -> 0, Reference -> 1, NonReference -> 2; skips Unknown & ExactCopy (-1) so pulldown repeats never poison stats!
			// Requires matching RefRole per slot, source-seconds age <= kA2eHistoryHorizonSec, and low inter-frame cell motion
			// (|v| < 0.5 * cellPx) so fast-moving cells never mix unrelated spatial content.
			const double frameHz = (std::isfinite(cfg_.sourceHz) && cfg_.sourceHz > 0.0) ? cfg_.sourceHz : kDefaultSourceHz;
			elapsedSec_ += (1.0 / frameHz);
			auto roleToPoolIdx = [](QualityPattern::FrameRole r) -> int
			{
				switch (r)
				{
				case QualityPattern::FrameRole::Peak:         return 0;
				case QualityPattern::FrameRole::Reference:    return 1;
				case QualityPattern::FrameRole::NonReference: return 2;
				case QualityPattern::FrameRole::Unknown:
				case QualityPattern::FrameRole::ExactCopy:
				default:                                      return -1;
				}
			};
			const int roleIdx = roleToPoolIdx(in.frameRole);
			if (effectiveCut || effFadeKeep < 1.0)
			{
				for (auto& slot : sameRoleStats_)
					slot.Reset();
			}
			else if (cfg_.enableTwoFrameSameRoleStatsPool && roleIdx >= 0)
			{
				SameRoleStatsSlot& prevSlot = sameRoleStats_[static_cast<size_t>(roleIdx)];
				// Capture unmerged snapshot of current frame's cell stats BEFORE merging with prevSlot.
				SameRoleStatsSlot curSnapshot;
				curSnapshot.valid = true;
				curSnapshot.cellsW = res.cellsW;
				curSnapshot.cellsH = res.cellsH;
				curSnapshot.refCount = K;
				curSnapshot.timeSec = elapsedSec_;
				for (int i = 0; i < K; ++i)
					curSnapshot.refRoles[i] = refSpecs[static_cast<size_t>(i)].role;
				for (int b = 0; b < 3; ++b)
				{
					curSnapshot.bands[b].resize(numCells);
					for (size_t ci = 0; ci < numCells; ++ci)
					{
						const CellBandMoments& src = cellMoments[b][ci];
						SameRoleCellStat& dst = curSnapshot.bands[b][ci];
						dst.sampleCount = src.sampleCount;
						for (int i = 0; i < K; ++i)
						{
							dst.cIvRaw[i] = src.cIvRaw[i];
							dst.hasPartner[i] = src.hasIndependentPartner[i];
							for (int j = 0; j < K; ++j)
								dst.C[i][j] = src.C[i][j];
						}
					}
				}

				bool rolesMatch = prevSlot.valid && prevSlot.cellsW == res.cellsW &&
				                  prevSlot.cellsH == res.cellsH && prevSlot.refCount == K &&
				                  (elapsedSec_ - prevSlot.timeSec) <= kA2eHistoryHorizonSec;
				if (rolesMatch)
				{
					for (int i = 0; i < K; ++i)
					{
						if (prevSlot.refRoles[i] != refSpecs[static_cast<size_t>(i)].role)
						{
							rolesMatch = false;
							break;
						}
					}
				}

				if (rolesMatch)
				{
					const double maxStaticSpeedPx = 0.5 * static_cast<double>(cellPx);
					for (int b = 0; b < 3; ++b)
					{
						a2eSamples[b][0].clear();
						a2eSamples[b][1].clear();
						for (size_t ci = 0; ci < numCells; ++ci)
						{
							CellBandMoments& cm = cellMoments[b][ci];
							const SameRoleCellStat& pm = prevSlot.bands[b][ci];
							// Gate 2-frame static-coordinate merge strictly to low-motion cells (|v| < 0.5 * cellPx).
							bool lowMotionCell = true;
							for (int i = 0; i < K; ++i)
							{
								if (ci < refs[i].motionVx.size() && ci < refs[i].motionVy.size())
								{
									const double spd = std::hypot(refs[i].motionVx[ci], refs[i].motionVy[ci]);
									if (!std::isfinite(spd) || spd >= maxStaticSpeedPx)
									{
										lowMotionCell = false;
										break;
									}
								}
							}
							if (cm.sampleCount == 0 || pm.sampleCount == 0 || !lowMotionCell)
							{
								// Still preserve current cell's A2e observation even when not merged.
								if (cm.sampleCount > 0)
								{
									for (int k = 0; k < K; ++k)
									{
										if (cm.hasIndependentPartner[k] && !refs[k].isExactCopy && !isFocusVetoedBand(ci, b))
										{
											const double mGate = (b == 0) ? refs[k].motionGate.gateL[ci] : refs[k].motionGate.gateMH[ci];
											if (mGate > 0.0 && std::isfinite(cm.cIvClipped[k]))
											{
												a2eSamples[b][cellActivityClass[ci]].push_back(cm.cIvClipped[k]);
												break;
											}
										}
									}
								}
								continue;
							}
							const double nCur = static_cast<double>(cm.sampleCount);
							const double nPrev = static_cast<double>(pm.sampleCount);
							const double nTot = nCur + nPrev;
							const double wCur = nCur / nTot;
							const double wPrev = nPrev / nTot;
							for (int i = 0; i < K; ++i)
							{
								for (int j = 0; j < K; ++j)
									cm.C[i][j] = wCur * cm.C[i][j] + wPrev * pm.C[i][j];
							}
							for (int k = 0; k < K; ++k)
								cellDiagC[b][k][ci] = (std::min)(cellDiagC[b][k][ci], cm.C[k][k]);
							cm.sampleCount = cm.sampleCount + pm.sampleCount;
							const double nEffMerged = EffectiveSamplesForBand(b, static_cast<double>(cm.sampleCount));
							bool cellContributed = false;
							for (int k = 0; k < K; ++k)
							{
								const int partner = findIndependentA1Partner(k, ci, b);
								if (partner >= 0 && cm.hasIndependentPartner[k] && pm.hasPartner[k])
								{
									cm.cIvRaw[k] = wCur * cm.cIvRaw[k] + wPrev * pm.cIvRaw[k];
									cm.cIvUpperCap[k] = (std::max)(0.0, (std::min)(cm.C[k][k], cm.C[partner][partner]));
									cm.cIvSeSq[k] = SamplingVarianceOfCrossMoment(cm.C[k][k], cm.C[partner][partner], cm.C[k][partner], nEffMerged);
									const double clipped = cm.cIvRaw[k] <= 0.0 ? 0.0 : (cm.cIvRaw[k] < cm.cIvUpperCap[k] ? cm.cIvRaw[k] : cm.cIvUpperCap[k]);
									cm.cIvClipped[k] = clipped;
									if (!cellContributed && !refs[k].isExactCopy && !isFocusVetoedBand(ci, b))
									{
										const double mGate = (b == 0) ? refs[k].motionGate.gateL[ci] : refs[k].motionGate.gateMH[ci];
										if (mGate > 0.0 && std::isfinite(clipped))
										{
											a2eSamples[b][cellActivityClass[ci]].push_back(clipped);
											cellContributed = true;
										}
									}
								}
							}
						}
					}
				}
				prevSlot = std::move(curSnapshot);
			}

			// ---- Frame-Level TD-U1 & TD-U4 Reductions: Aligned Sharpness r_t, A2e Prior U_j(class), & EB Spread tau^2 ----
			if (prevRefIdx >= 0 && prevRefIdx < static_cast<int>(refSpecs.size()) && refSpecs[prevRefIdx].motion != nullptr)
			{
				const CellMotionField& pmf = *refSpecs[prevRefIdx].motion;
				res.alignedSharpness = MeasureAlignedSharpnessFromCellEnergies(
					cellCurMidE, cellCurFineE, cellPrevMidE, cellPrevFineE,
					pmf.vx, pmf.vy, pmf.valid, &cellFocusFlagged,
					&cellRetainedGrain, cfg_.sourceBits,
					refs[prevRefIdx].isExactCopy ? 1.0 : 0.0, 0.0,
					nullptr, nullptr, nullptr, nullptr, standIns);
			}

			{
				EmpiricalOwnErrorPrior rawA2e;
				bool anyClassValid = false;
				for (int b = 0; b < 3; ++b)
				{
					for (int ac = 0; ac < kA2eActivityClasses; ++ac)
					{
						if (!a2eSamples[b][ac].empty())
						{
							rawA2e.U[b][ac] = (std::max)(0.0, RobustStats::Median(a2eSamples[b][ac], RobustStats::MedianRule::MeanOfMiddle));
							rawA2e.cellCount[b][ac] = static_cast<uint32_t>(a2eSamples[b][ac].size());
							anyClassValid = true;
						}
					}
					// If one activity class had 0 cells (e.g. uniform frame), fall back cleanly to the other populated class.
					if (rawA2e.cellCount[b][0] == 0 && rawA2e.cellCount[b][1] > 0)
						rawA2e.U[b][0] = rawA2e.U[b][1];
					else if (rawA2e.cellCount[b][1] == 0 && rawA2e.cellCount[b][0] > 0)
						rawA2e.U[b][1] = rawA2e.U[b][0];
				}
				rawA2e.valid = anyClassValid;
				const bool freezeA2e = effectiveCut || (effFadeKeep < 1.0);
				res.empiricalOwnError = a2eHistory_.ApplyCapAndRecord(
					rawA2e, in.frameRole, cfg_.sourceHz, freezeA2e, effectiveCut);
			}

			// TD-U4 (H-3): Cross-cell spread tau^2 = max(0, Var(c_raw) - mean(se^2)) per band x activity class.
			double ebTauSq[3][kA2eActivityClasses] = {};
			if (cfg_.enableEmpiricalBayesShrinkage && res.empiricalOwnError.valid)
			{
				for (int b = 0; b < 3; ++b)
				{
					double meanRaw[kA2eActivityClasses] = {};
					double m2Raw[kA2eActivityClasses] = {};
					double sumSeSq[kA2eActivityClasses] = {};
					size_t cnt[kA2eActivityClasses] = {};
					for (size_t ci = 0; ci < numCells; ++ci)
					{
						if (isFaceMaskedCell(ci) || isFocusVetoedBand(ci, b))
							continue;
						const CellBandMoments& cm = cellMoments[b][ci];
						if (cm.sampleCount == 0)
							continue;
						const int ac = static_cast<int>(cellActivityClass[ci]);
						for (int k = 0; k < K; ++k)
						{
							if (!cm.hasIndependentPartner[k] || refs[k].isExactCopy)
								continue;
							const double mGate = (b == 0) ? refs[k].motionGate.gateL[ci] : refs[k].motionGate.gateMH[ci];
							if (!(mGate > 0.0) || !std::isfinite(cm.cIvRaw[k]) || !std::isfinite(cm.cIvSeSq[k]))
								continue;
							++cnt[ac];
							const double d = cm.cIvRaw[k] - meanRaw[ac];
							meanRaw[ac] += d / static_cast<double>(cnt[ac]);
							m2Raw[ac] += d * (cm.cIvRaw[k] - meanRaw[ac]);
							sumSeSq[ac] += cm.cIvSeSq[k];
							break; // One unbiased c_IV observation per active cell
						}
					}
					for (int ac = 0; ac < kA2eActivityClasses; ++ac)
					{
						if (cnt[ac] >= 2)
						{
							const double varRaw = m2Raw[ac] / static_cast<double>(cnt[ac] - 1);
							const double meanSe = sumSeSq[ac] / static_cast<double>(cnt[ac]);
							ebTauSq[b][ac] = (std::max)(0.0, varRaw - meanSe);
						}
					}
					if (cnt[0] < 2 && cnt[1] >= 2)
						ebTauSq[b][0] = ebTauSq[b][1];
					else if (cnt[1] < 2 && cnt[0] >= 2)
						ebTauSq[b][1] = ebTauSq[b][0];
				}
			}

			// ---- Stage 2B: Per-cell blind c selection, constrained convex QP solve, motion/focus/grain/fade gates ----
			for (int cy = 0; cy < res.cellsH; ++cy)
			{
				for (int cx = 0; cx < res.cellsW; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					if (isFaceMaskedCell(cIdx))
						continue;

					const double cellRetainedGrainVar = cellRetainedGrain[cIdx];
					const double spatialDetailRms = cellSpatialDetailRms[cIdx];
					const double curHVar = spatialDetailRms * spatialDetailRms;
					const double cellOwnGrainVar = cellOwnGrainVarArr[cIdx];
					const double cellStepScaleVal = cellStepScales[cIdx];
					const double cellStepScaleSq = cellStepScaleVal * cellStepScaleVal;
					const int actClass = static_cast<int>(cellActivityClass[cIdx]);

					double cellWeightSumAll = 0.0;
					double cellResEnergy = 0.0;
					unsigned activeBandBits = 0;
					bool cellUsedSingleRefA2 = false;
					bool cellHadSingular = false;

					for (int b = 0; b < 3; ++b)
					{
						const CellBandMoments& cm = cellMoments[b][cIdx];
						if (cm.sampleCount == 0)
							continue;

						const double (&C)[kMaxBlendRefs][kMaxBlendRefs] = cm.C;
						// RED-BUG-7b: Determine per-ref active mask (motionGate > 0) BEFORE computing cVec / solving QP
						// so drift-barred / held-out-rejected anchors or occluded refs NEVER crush peer cA1 or steal simplex budget!
						bool refActive[kMaxBlendRefs] = {};
						int subMap[kMaxBlendRefs] = {};
						int subK = 0;
						for (int k = 0; k < K; ++k)
						{
							const double mGate = (b == 0) ? refs[k].motionGate.gateL[cIdx] : refs[k].motionGate.gateMH[cIdx];
							if (mGate > 0.0)
							{
								refActive[k] = true;
								subMap[subK++] = k;
							}
						}
						if (subK == 0)
							continue;

						double cVec[kMaxBlendRefs] = {};
						for (int k = 0; k < K; ++k)
						{
							if (!refActive[k])
							{
								cVec[k] = 0.0;
								continue;
							}
							// A2 prior for ref k on band b (TD-U3 / H-1 / UPG-HIGH-5):
							// - On unidentified transforms when A2e empirical prior is active and no explicit codec step was supplied:
							//   * Single-ref (no independent A1 partner): use conservative class median U_j(class) * cellStepScaleSq.
							//   * Multi-ref (independent A1 partner present): allow upper envelope (U_j + 3*tau) * cellStepScaleSq
							//     so A3 = min(EB-shrunk cA1, cA2) repairs worst-5% damaged cells rather than clipping them to median!
							const bool useA2ePrior = cfg_.enableA2eEmpiricalPrior && !in.transformIdentified8x8 &&
							                         !in.hasStepPrior && res.empiricalOwnError.valid;
							double rawA2 = 0.0;
							if (useA2ePrior)
							{
								if (!refs[k].isExactCopy)
								{
									const double priorU = res.empiricalOwnError.ForBandClass(b, actClass);
									const double upperEnvelope = cm.hasIndependentPartner[k]
										? (priorU + std::sqrt(kHuberKneeSq * (std::max)(0.0, ebTauSq[b][actClass])))
										: priorU;
									rawA2 = upperEnvelope * cellStepScaleSq;
								}
							}
							else
							{
								rawA2 = EstimateC_A2(a2Stats, refs[k].isExactCopy) * cellStepScaleSq * bandTransfer[b];
							}
							const double cA2 = (std::min)(rawA2, C[k][k]);

							const double psdScale = in.psdShape.ForBand(b);
							double cA1 = cA2;
							if (cm.hasIndependentPartner[k])
							{
								if (cfg_.enableEmpiricalBayesShrinkage && !refs[k].isExactCopy && res.empiricalOwnError.valid)
								{
									const double priorU = res.empiricalOwnError.ForBandClass(b, actClass);
									cA1 = EmpiricalBayesShrinkC(
										cm.cIvRaw[k], cm.cIvSeSq[k], priorU, ebTauSq[b][actClass], cm.cIvUpperCap[k]);
								}
								else
								{
									cA1 = cm.cIvClipped[k];
								}
							}
							else if (cfg_.estimator != VarianceEstimator::A2_RoundingCells)
							{
								cellUsedSingleRefA2 = true;
								for (int j = 0; j < K; ++j)
								{
									if (j == k || refs[j].isExactCopy || !refActive[j])
										continue;
									const double mxKJ = (std::max)(C[k][k], C[j][j]);
									const double collinearFloorB = 0.01 * baseQuantVar * bandTransfer[b];
									if (mxKJ > kEngineNumFloor && (C[k][k] + C[j][j] - 2.0 * C[k][j]) <= (collinearFloorB + 1e-6 * mxKJ))
										continue;
									double capBias = 0.0;
									if (cfg_.enableSubpixelHAttenuationDebias && b == 2 &&
									    cIdx < refs[k].motionVx.size() && cIdx < refs[j].motionVx.size())
									{
										capBias = ComputeSubpixelHAttenuationBias(
											refs[k].motionVx[cIdx], refs[k].motionVy[cIdx],
											refs[j].motionVx[cIdx], refs[j].motionVy[cIdx],
											curHVar, cellRetainedGrainVar * bandTransfer[2] * psdScale);
									}
									const double cap = EstimateC_A1(C[k][j], C[k][k], C[j][j],
									                                cellRetainedGrainVar, bandTransfer[b], 1.0, psdScale, capBias);
									if (cap < cA1)
										cA1 = cap;
								}
							}

							cVec[k] = SelectBlindC(cfg_.estimator, cA1, cA2);
							if (refs[k].isExactCopy)
								cVec[k] = 0.0;
						}

						// TD-U3 (H-1): Per-block own-error variance U_j(block) for the honest transform-unknown statistical limit.
						// Uses the cell's own selected/shrunk blind own-error estimate when empirical A2e is active, else exact bandTransfer[b].
						double blockOwnU = a2Stats.totalVar * cellStepScaleSq * bandTransfer[b];
						if (cfg_.enableA2eEmpiricalPrior && !in.transformIdentified8x8 && res.empiricalOwnError.valid)
						{
							double sumC = 0.0;
							int nC = 0;
							for (int k = 0; k < K; ++k)
							{
								if (refActive[k] && !refs[k].isExactCopy)
								{
									sumC += (cVec[k] > 0.0 ? cVec[k] : 0.0);
									++nC;
								}
							}
							blockOwnU = (nC > 0) ? (sumC / static_cast<double>(nC)) : 0.0;
						}
						cellOwnErrU[b][cIdx] = blockOwnU;

						// Compact active subproblem before SolveConvexWeights so barred/occluded refs never distort C^-1 or simplex!
						double subC[kMaxBlendRefs][kMaxBlendRefs] = {};
						double subCVec[kMaxBlendRefs] = {};
						for (int i = 0; i < subK; ++i)
						{
							subCVec[i] = cVec[subMap[i]];
							for (int j = 0; j < subK; ++j)
								subC[i][j] = C[subMap[i]][subMap[j]];
						}
						const BlendSolveResult subSolved = SolveConvexWeights(subC, subCVec, subK, kDefaultTraceRegScale, standIns, cfg_.solverMode);
						BlendSolveResult solved{};
						solved.nonFiniteClosed = subSolved.nonFiniteClosed;
						solved.singular = subSolved.singular;
						if (!solved.nonFiniteClosed && !solved.singular)
						{
							for (int i = 0; i < subK; ++i)
								solved.weights[subMap[i]] = subSolved.weights[i];
						}
						if (solved.nonFiniteClosed)
						{
							nonFiniteCellFlag[cIdx] = 1u;
							continue;
						}
						if (solved.singular)
						{
							cellHadSingular = true;
							continue;
						}

						// Apply Owner Focus Rule & BokehMode (Plan s1.4 L135-L143 & H8).
						const double focusGate = isFocusVetoedBand(cIdx, b) ? 0.0 : 1.0;

						double bandWSum = 0.0;
						for (int k = 0; k < K; ++k)
						{
							const double mGate = (b == 0) ? refs[k].motionGate.gateL[cIdx] : refs[k].motionGate.gateMH[cIdx];
							const double wFinal = solved.weights[k] * mGate * focusGate;
							cellW[b][k][cIdx] = wFinal;
							bandWSum += wFinal;
						}
						if (bandWSum > kEngineNumFloor)
							activeBandBits |= (1u << static_cast<unsigned>(b));
						cellWeightSumAll += bandWSum;
						cellResEnergy += C[0][0];
					}

					if (cellUsedSingleRefA2)
					{
						++res.singleRefA2Cells;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
					}
					if (cellHadSingular)
						++res.singularCells;

					// Layer 2/3 grain-leak transfer budget (refs/111 s7.3 & TdGuardPort::kMinVarianceRetention = 0.98^2):
					// Active when Layer 2 grain-prior protection is enabled (estimator != A2_RoundingCells) and either:
					//   (a) upstream P10g split separated the majority of grain into ownGrain with a minor leaked fraction
					//       inside curClean (splitActive && cellOwnGrainVar > cellRetainedGrainVar > 0, 0 < alphaLeak < 0.5), OR
					//   (b) raw unsplit path (!splitActive && cellRetainedGrainVar > 0), where 100% of curClean grain is
					//       own grain (alphaLeak = 1.0).
					// (When splitActive && cellRetainedGrainVar >= cellOwnGrainVar, Layer 2 A1 + Layer 4 TdGuardPort govern.)
					// Blending band b with total weight W_b reduces betaOwn by exactly alphaLeak * bandOrigin[b] * W_b
					// (where bandOrigin[b] = h_b[0,0] is BandFromBase's exact 2D white-noise origin projection).
					// Allocate the quadrature-split budget (1 - sqrt(kMinVarianceRetention)) / sqrt(2) strictly coarse-to-fine (L -> M -> H)
					// — splitting the 2% retention tolerance equally in variance between deterministic band attenuation and
					// finite-sample / 1x1-cell pooling variance — so L/M compression repair runs strongly while grain stays protected.
					const bool minorSplitLeak = splitActive && (cellOwnGrainVar > cellRetainedGrainVar);
					const bool rawUnsplitGrain = !splitActive;
					if (cfg_.estimator != VarianceEstimator::A2_RoundingCells &&
					    cellRetainedGrainVar > kEngineNumFloor && (minorSplitLeak || rawUnsplitGrain))
					{
						double alphaLeak = 1.0;
						if (minorSplitLeak)
						{
							const double sigmaLeak = std::sqrt(cellRetainedGrainVar);
							const double sigmaOwn = std::sqrt(cellOwnGrainVar);
							const double sigmaTotal = sigmaOwn + sigmaLeak;
							alphaLeak = (sigmaTotal > kEngineNumFloor) ? (sigmaLeak / sigmaTotal) : 0.0;
						}
						if (alphaLeak > kEngineNumFloor)
						{
							const double bandOrigin[3] = { gains.originLow, gains.originMid, gains.originFine };
							double remGrainBudget = (1.0 - std::sqrt(TdGuardPort::kMinVarianceRetention)) / std::sqrt(2.0);
							activeBandBits = 0u;
							cellWeightSumAll = 0.0;
							for (int b = 0; b < 3; ++b)
							{
								double bandWSum = 0.0;
								for (int k = 0; k < K; ++k)
									bandWSum += cellW[b][k][cIdx];
								const double bandLossPerUnitW = alphaLeak * bandOrigin[b];
								if (bandLossPerUnitW > kEngineNumFloor && bandWSum > kEngineNumFloor)
								{
									const double maxBandW = remGrainBudget / bandLossPerUnitW;
									if (bandWSum >= maxBandW)
									{
										const double scale = (maxBandW > 0.0) ? (maxBandW / bandWSum) : 0.0;
										for (int k = 0; k < K; ++k)
											cellW[b][k][cIdx] *= scale;
										bandWSum = (maxBandW > 0.0) ? maxBandW : 0.0;
										remGrainBudget = 0.0;
									}
									else
									{
										remGrainBudget = (std::max)(0.0, remGrainBudget - bandLossPerUnitW * bandWSum);
									}
								}
								if (bandWSum > kEngineNumFloor)
									activeBandBits |= (1u << static_cast<unsigned>(b));
								cellWeightSumAll += bandWSum;
							}
						}
					}

					// SOTA-7 (U7 gradual fadeKeep): taper recurrent Carry weight strictly AFTER grain-budget scaling
					// so grain-budget normalization never cancels fadeKeep (fadeKeep == 1.0 -> bit-identical).
					if (effFadeKeep < 1.0)
					{
						activeBandBits = 0u;
						cellWeightSumAll = 0.0;
						for (int b = 0; b < 3; ++b)
						{
							double bandWSum = 0.0;
							for (int k = 0; k < K; ++k)
							{
								if (refs[k].role == RefRole::Carry)
									cellW[b][k][cIdx] *= effFadeKeep;
								bandWSum += cellW[b][k][cIdx];
							}
							if (bandWSum > kEngineNumFloor)
								activeBandBits |= (1u << static_cast<unsigned>(b));
							cellWeightSumAll += bandWSum;
						}
					}

					// Sum ALL active references (including k=2 carry) for mid-band trail confidence (MEDIUM-3).
					double midWeightSum = 0.0;
					for (int k = 0; k < K; ++k)
						midWeightSum += cellW[1][k][cIdx];

					// Populate U19 TdMapsView::Block (all 12 named fields).
					TdMapsView::Block& mBlock = res.tdMaps[cIdx];
					mBlock.f[TdMapsView::kValidity] = (activeBandBits != 0u) ? 1.0f : 0.0f;
					mBlock.f[TdMapsView::kTrailConfidence0] = static_cast<float>(RampMath::Clamp01(cellWeightSumAll / 3.0));
					mBlock.f[TdMapsView::kTrailConfidence1] = static_cast<float>(RampMath::Clamp01(midWeightSum));
					mBlock.f[TdMapsView::kResidual] = static_cast<float>(cellResEnergy / 3.0);
					mBlock.f[TdMapsView::kRealDetail] = static_cast<float>(spatialDetailRms);
					mBlock.f[TdMapsView::kStability] = static_cast<float>(RampMath::Clamp01(cellWeightSumAll / 3.0));
					mBlock.f[TdMapsView::kOwnGrain] = static_cast<float>(std::sqrt((std::max)(0.0, cellRetainedGrainVar)));
					mBlock.f[TdMapsView::kCodingNoise] = static_cast<float>(std::sqrt((std::max)(0.0, a2Stats.totalVar * cellStepScaleSq)));
					mBlock.f[TdMapsView::kLightGain] = 1.0f;
					mBlock.f[TdMapsView::kLightOffset] = 0.0f;
					mBlock.f[TdMapsView::kDrift] = cellDriftPx[cIdx];
					mBlock.f[TdMapsView::kBandMask] = static_cast<float>(activeBandBits);
				}
			}

			if (res.faceMaskedCells > 0 && standIns != nullptr)
				standIns->Engage(StandInKind::FaceNonRigidMask);

			// Plan 6.6 / CHR-HIGH-3: Export final post-gate luma cell weights & explicit RefRoles for chroma reuse_luma.
			res.lumaActiveRefCount = K;
			for (int k = 0; k < K; ++k)
			{
				res.lumaRefRoles[k] = refs[static_cast<size_t>(k)].role;
				for (int b = 0; b < 3; ++b)
					res.lumaCellWeights[b][k] = cellW[b][k];
			}

			// Per-pixel spatial-only interpolation (ZERO bleed across vetoed cell boundaries — HIGH-5),
			// toggleable 3-sigma per-corner Huber taper (cfg_.enablePerCornerHuber), and honest statistical limit (TD-U3 / D3).
			// TD-U3 (H-1): 8x8 DCT STRICT/CELL POCS runs ONLY when BOTH transformIdentified8x8 AND a real encoder prediction
			// `codecModelPb` are present (warped-peak substitute refs[0].clean is removed; absent P_B falls back visibly
			// to the statistical per-band + joint cross-band limit).
			TdV2Plane deltaPlane(w, h, 0.0);
			const int bxStart = ((active.x0 + 7) / 8) * 8;
			const int byStart = ((active.y0 + 7) / 8) * 8;
			const int bxEnd = (active.x1 / 8) * 8;
			const int byEnd = (active.y1 / 8) * 8;
			const double effKappa = ClampPixelBoundKappa(cfg_.pixelBoundKappa);
			const bool hasValidCodecModelPb = (in.codecModelPb != nullptr && !in.codecModelPb->Empty() &&
			                                   in.codecModelPb->w == w && in.codecModelPb->h == h);
			const bool canRun8x8Pocs = in.transformIdentified8x8 && hasValidCodecModelPb && effStepDelta > 0.0 &&
			                           (bxStart < bxEnd) && (byStart < byEnd);
			if (in.transformIdentified8x8 && !hasValidCodecModelPb)
			{
				if (standIns != nullptr)
				{
					standIns->Engage(StandInKind::CodecModelPbStandIn,
						in.codecModelPb == nullptr
							? "CodecModel Field B P_B absent; falling back to statistical limit (no warped-peak substitute)"
							: "codecModelPb dimension mismatch; falling back to statistical limit (no warped-peak substitute)");
				}
				res.codecModelPbStandInCells = numCells;
			}

			for (int y = active.y0; y < active.y1; ++y)
			{
				const int cy = (std::min)(res.cellsH - 1, y / cellPx);
				for (int x = active.x0; x < active.x1; ++x)
				{
					const int cx = (std::min)(res.cellsW - 1, x / cellPx);
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					const size_t pIdx = static_cast<size_t>(y) * w + x;

					// Pass-1 face / non-rigid mask: exact delta == 0.0 for every pixel inside a masked cell.
					if (isFaceMaskedCell(cIdx))
					{
						deltaPlane.at(x, y) = 0.0;
						continue;
					}

					// Pixels outside valid 8x8 DCT POCS (transform unknown, missing real P_B, or non-8-aligned strips)
					// receive the honest statistical per-band + joint cross-band limit (TD-U3 / H-1 / Owner D3).
					const bool inFull8x8Block = canRun8x8Pocs &&
					                            (x >= bxStart) && (x < bxEnd) && (y >= byStart) && (y < byEnd);

					double totalDelta = 0.0;
					double sumBandVar = 0.0;
					for (int b = 0; b < 3; ++b)
					{
						const TdV2Plane& curBandPlane = bandPlaneOf(curBands, b);
						const double curV = curBandPlane.at(x, y);

						// SOTA #5: Optional 3x3 grain-free neighborhood 3-sigma C^1 soft clamp on M/H bands (b >= 1).
						double nbrMean = curV;
						double nbrSigma = 0.0;
						const bool applyNbrClamp = cfg_.enableNeighborhoodSoftClamp && (b >= 1);
						if (applyNbrClamp)
						{
							double sumS = 0.0;
							double sumSq = 0.0;
							const int xMaxA = active.x1 - 1;
							const int yMaxA = active.y1 - 1;
							for (int dy = -1; dy <= 1; ++dy)
							{
								const int sy = (std::max)(active.y0, (std::min)(yMaxA, y + dy));
								for (int dx = -1; dx <= 1; ++dx)
								{
									const int sx = (std::max)(active.x0, (std::min)(xMaxA, x + dx));
									const double s = curBandPlane.at(sx, sy) - curV;
									sumS += s;
									sumSq += s * s;
								}
							}
							const double meanS = sumS / 9.0;
							nbrMean = curV + meanS;
							nbrSigma = std::sqrt((std::max)(0.0, (sumSq / 9.0) - meanS * meanS));
						}

						double bandDelta = 0.0;
						for (int k = 0; k < K; ++k)
						{
							if (refs[k].validPx[pIdx] == 0u)
								continue;
							// Hard veto boundary guard (HIGH-5): if the pixel's own cell has weight 0
							// (Overlay, DynamicTexture, focusChange, faceMask, exact copy, or invalid motion),
							// force weight strictly 0.0 so adjacent active cells NEVER bleed across the boundary!
							if (cellW[b][k][cIdx] <= 0.0)
								continue;

							// Spatial-only interpolation (ZERO temporal weight ramps):
							// - Optional SOTA #4 clean-luma-guided edge-aware bilinear (cfg_.enableEdgeAwareWeightUpsample)
							// - Default spatial bilinear with optional per-corner 3-sigma Huber taper (cfg_.enablePerCornerHuber).
							double refV = bandPlaneOf(refs[k].bands, b).at(x, y);
							if (applyNbrClamp)
								refV = SoftClampToNeighborhood3Sigma(refV, nbrMean, nbrSigma);
							const double diff = curV - refV;
							double wEff = 0.0;
							if (cfg_.enableEdgeAwareWeightUpsample)
							{
								const double pxStepScale = cellStepScales[cIdx];
								const double pxQuantSigma = std::sqrt((std::max)(0.0, a2Stats.totalVar * pxStepScale * pxStepScale));
								wEff = SpatialGuidedBilinearWeight(
									cellW[b][k].data(), cellDiagC[b][k].data(),
									cellMeanCleanY.data(), cellStdCleanY.data(),
									res.cellsW, res.cellsH,
									static_cast<double>(x), static_cast<double>(y), cellPx,
									diff, curClean.at(x, y), pxQuantSigma, cfg_.enablePerCornerHuber);
							}
							else if (cfg_.enablePerCornerHuber)
							{
								wEff = SpatialBilinearHuberWeight(
									cellW[b][k].data(), cellDiagC[b][k].data(), res.cellsW, res.cellsH,
									static_cast<double>(x), static_cast<double>(y), cellPx, diff);
							}
							else
							{
								wEff = SpatialBilinearWeight(
									cellW[b][k].data(), res.cellsW, res.cellsH,
									static_cast<double>(x), static_cast<double>(y), cellPx);
							}
							if (wEff <= 0.0)
								continue;
							bandDelta -= wEff * diff;
						}
						// TD-U3 (H-1 / Owner D3): Per-band statistical limit |Delta_j(p)| <= kappa * sqrt(U_j(block)).
						if (!inFull8x8Block)
						{
							const double bandVar = cellOwnErrU[b][cIdx];
							sumBandVar += bandVar;
							if (std::fabs(bandDelta) > 0.0)
								bandDelta = ApplyTransformUnknownBound(bandDelta, bandVar, standIns, effKappa);
						}
						totalDelta += bandDelta;
					}
					// TD-U3 (H-1): Joint cross-band limit |sum_j Delta_j(p)| <= kappa * sqrt(sum_j U_j(block)).
					if (!inFull8x8Block && cfg_.enableJointCrossBandBound && std::fabs(totalDelta) > 0.0)
						totalDelta = ApplyJointCrossBandBound(totalDelta, sumBandVar, standIns, effKappa);
					deltaPlane.at(x, y) = totalDelta;
				}
			}

			// Identified 8x8 DCT transform-domain STRICT/CELL projection strictly on the coded residual r = X - P_B (HIGH-6 & TD-U3),
			// ONLY when a real encoder prediction `codecModelPb` is supplied (never warped-peak substitute).
			if (canRun8x8Pocs)
			{
				const double peakCode = PeakCodeForBits(cfg_.sourceBits);
				const TdV2Plane& predClean = *in.codecModelPb;

				const int iters = (std::max)(1, (std::min)(2, cfg_.cellProjIterations));
				for (int it = 0; it < iters; ++it)
				{
					// Set 1: Transform-domain quantization-cell projection on residual r = X - P_B.
					for (int by = byStart; by < byEnd; by += 8)
					{
						for (int bx = bxStart; bx < bxEnd; bx += 8)
						{
							// Skip untouched / vetoed zero-delta 8x8 blocks so FP64 DCT round-trip stays bit-exact.
							bool anyNonZeroDelta = false;
							for (int j = 0; j < 8 && !anyNonZeroDelta; ++j)
								for (int i = 0; i < 8; ++i)
									if (deltaPlane.at(bx + i, by + j) != 0.0) { anyNonZeroDelta = true; break; }
							if (!anyNonZeroDelta)
								continue;

							double sentRes[64] = {}, blendRes[64] = {}, projRes[64] = {};
							bool blockNonFinite = false;
							for (int j = 0; j < 8; ++j)
							{
								for (int i = 0; i < 8; ++i)
								{
									const double cVal = curClean.at(bx + i, by + j);
									const double pVal = predClean.at(bx + i, by + j);
									if (!std::isfinite(pVal))
									{
										blockNonFinite = true;
										break;
									}
									sentRes[j * 8 + i] = cVal - pVal;
									blendRes[j * 8 + i] = (cVal + deltaPlane.at(bx + i, by + j)) - pVal;
								}
								if (blockNonFinite)
									break;
							}
							const int bcy = (std::min)(res.cellsH - 1, by / cellPx);
							const int bcx = (std::min)(res.cellsW - 1, bx / cellPx);
							const size_t bCellIdx = static_cast<size_t>(bcy) * res.cellsW + bcx;
							const double blockStepScale = cellStepScales[bCellIdx];
							if (blockNonFinite || !(blockStepScale > 0.0))
							{
								nonFiniteCellFlag[bCellIdx] = 1u;
								if (standIns != nullptr)
									standIns->Engage(StandInKind::NonFiniteClosure,
										blockNonFinite ? "TdV2ReferenceEngine non-finite codecModelPb" : "TdV2ReferenceEngine invalid cellStepScale");
								for (int j = 0; j < 8; ++j)
									for (int i = 0; i < 8; ++i)
										deltaPlane.at(bx + i, by + j) = 0.0;
								continue;
							}
							ProjectResidual8x8Dct(sentRes, blendRes, effStepDelta * blockStepScale, effPhi, cfg_.residualMode, projRes);
							for (int j = 0; j < 8; ++j)
							{
								for (int i = 0; i < 8; ++i)
								{
									const double cVal = curClean.at(bx + i, by + j);
									const double pVal = predClean.at(bx + i, by + j);
									deltaPlane.at(bx + i, by + j) = (pVal + projRes[j * 8 + i]) - cVal;
								}
							}
						}
					}

					// Set 2: Pixel-domain valid code-range projection [0, peakCode] (makes 2nd POCS iteration genuine).
					// Untouched zero-delta pixels stay untouched.
					if (peakCode > 0.0)
					{
						for (int by = byStart; by < byEnd; by += 8)
						{
							for (int bx = bxStart; bx < bxEnd; bx += 8)
							{
								for (int j = 0; j < 8; ++j)
								{
									for (int i = 0; i < 8; ++i)
									{
										const double dVal = deltaPlane.at(bx + i, by + j);
										if (dVal == 0.0)
											continue;
										const double cVal = curClean.at(bx + i, by + j);
										const double clamped = (std::max)(0.0, (std::min)(peakCode, cVal + dVal));
										deltaPlane.at(bx + i, by + j) = clamped - cVal;
									}
								}
							}
						}
					}
				}
			}

			// Enforce exact 0.0 delta on every pixel inside Pass-1 face-masked cells even if an 8x8 DCT block straddled a non-8-aligned cell.
			if (res.faceMaskedCells > 0)
			{
				for (int y = active.y0; y < active.y1; ++y)
				{
					const int cy = (std::min)(res.cellsH - 1, y / cellPx);
					for (int x = active.x0; x < active.x1; ++x)
					{
						const int cx = (std::min)(res.cellsW - 1, x / cellPx);
						if (isFaceMaskedCell(static_cast<size_t>(cy) * res.cellsW + cx))
							deltaPlane.at(x, y) = 0.0;
					}
				}
			}

			// Build candidate recomposed output (clean + delta) + ownGrain strictly inside ActiveRect.
			// When d == 0.0, keep candOut bit-identical to in.receivedY (zero FP rounding drift).
			TdV2Plane candClean = curClean;
			TdV2Plane candOut = in.receivedY;
			for (int y = active.y0; y < active.y1; ++y)
			{
				for (int x = active.x0; x < active.x1; ++x)
				{
					const double d = deltaPlane.at(x, y);
					if (d != 0.0)
					{
						candClean.at(x, y) = curClean.at(x, y) + d;
						// Exact grain preservation: ownGrain = receivedY - curClean is NEVER modified or averaged!
						candOut.at(x, y) = candClean.at(x, y) + ownGrain.at(x, y);
					}
				}
			}

			// Layer 4: Shared Output Grain Guard (TdGuardPort stand-in for W4c GrainSplit::Guard).
			// - When splitActive == true: guards any cell where ownGrain or retainedGrainVar is present.
			// - When !splitActive (H2-luma fix): ALWAYS evaluates TdGuardPort with target = inFineE (never skips when
			//   ownGrain == 0 && retainedGrainVar == 0).
			// - Face-masked cells stay bit-exact untouched (`candOut == in.receivedY`, `delta == 0`).
			// Adds missing fine variance strictly to `candOut` ONLY — NEVER touching `candClean` / `carryClean_`
			// so own grain NEVER enters frame t+1's recurrent carry as foreign grain (BLOCKER-2)!
			const ThreeBandPlanes inFullBands = DecomposeActiveBands(in.receivedY, active, nullptr);
			const ThreeBandPlanes outFullBands = DecomposeActiveBands(candOut, active, nullptr);
			const ThreeBandPlanes ownGrainBands = splitActive
				? DecomposeActiveBands(ownGrain, active, nullptr)
				: ThreeBandPlanes{};

			std::vector<double> cellAddBackFrac(numCells, 0.0);
			for (int cy = 0; cy < res.cellsH; ++cy)
			{
				const int y0 = (std::max)(active.y0, cy * cellPx);
				const int y1 = (std::min)(active.y1, (cy + 1) * cellPx);
				if (y1 <= y0) continue;
				for (int cx = 0; cx < res.cellsW; ++cx)
				{
					const int x0 = (std::max)(active.x0, cx * cellPx);
					const int x1 = (std::min)(active.x1, (cx + 1) * cellPx);
					if (x1 <= x0) continue;

					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					if (isFaceMaskedCell(cIdx))
						continue;

					double ownGrainEnergy = 0.0;
					double ownGrainFineE = 0.0;
					double inFineE = 0.0, outFineE = 0.0;
					double inCovOwn = 0.0, outCovOwn = 0.0;
					size_t cnt = 0;
					for (int y = y0; y < y1; ++y)
					{
						for (int x = x0; x < x1; ++x)
						{
							const double og = ownGrain.at(x, y);
							const double inf = inFullBands.H.at(x, y);
							const double outf = outFullBands.H.at(x, y);
							ownGrainEnergy += og * og;
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
					if (splitActive && ownGrainEnergy * invC <= kEngineNumFloor && cellRetainedGrain[cIdx] <= kEngineNumFloor)
						continue;

					const double leakedGrainFineE = cellRetainedGrain[cIdx] * gains.fine;
					double targetGrainFineE = inFineE * invC;
					if (splitActive)
					{
						// ownGrain and leaked own grain inside curClean are 100% positively correlated fractions
						// of the same grain field G, so their fine-band std-devs add linearly (not in quadrature).
						const double targetStd = std::sqrt(ownGrainFineE * invC) + std::sqrt(leakedGrainFineE);
						targetGrainFineE = (std::min)(inFineE * invC, targetStd * targetStd);
					}

					// Texture-immune own-grain fine-band covariance check when leaked grain is reported:
					// fine scene texture cancels out of Cov(out_H, ownGrain_H) / Cov(in_H, ownGrain_H),
					// preventing textured cells from hiding lost leaked grain from TdGuardPort.
					double effOutFineE = outFineE * invC;
					if (splitActive && ownGrainFineE * invC > kEngineNumFloor && cellRetainedGrain[cIdx] > kEngineNumFloor &&
					    inCovOwn > kEngineNumFloor)
					{
						const double covRatio = outCovOwn > 0.0 ? (outCovOwn / inCovOwn) : 0.0;
						const double covImpliedOutE = targetGrainFineE * covRatio * covRatio;
						if (covImpliedOutE < effOutFineE)
							effOutFineE = covImpliedOutE;
					}

					const auto gDec = TdGuardPort::Evaluate(targetGrainFineE, effOutFineE, standIns);
					if (gDec.violated && gDec.addBackFraction > 0.0)
					{
						++res.guardViolationCells;
						cellAddBackFrac[cIdx] = gDec.addBackFraction;
						if (!cfg_.enableSmoothLayer4Guard)
						{
							for (int y = y0; y < y1; ++y)
							{
								for (int x = x0; x < x1; ++x)
								{
									const double addBack = gDec.addBackFraction * (inFullBands.H.at(x, y) - outFullBands.H.at(x, y));
									candOut.at(x, y) += addBack;
									// BLOCKER-2 invariant: candClean is NOT modified here, keeping carryClean_ 100% grain-free!
								}
							}
						}
					}
				}
			}

			// Fable L-3: Spatially bilinearly interpolated Layer-4 grain-guard add-back (removes 16-px block seams
			// while keeping Pass-1 face-masked cells strictly untouched and candClean 100% grain-free).
			if (cfg_.enableSmoothLayer4Guard && res.guardViolationCells > 0)
			{
				for (int y = active.y0; y < active.y1; ++y)
				{
					const int cy = (std::min)(res.cellsH - 1, y / cellPx);
					for (int x = active.x0; x < active.x1; ++x)
					{
						const int cx = (std::min)(res.cellsW - 1, x / cellPx);
						const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
						if (isFaceMaskedCell(cIdx))
							continue;
						const double frac = SpatialBilinearWeight(
							cellAddBackFrac.data(), res.cellsW, res.cellsH,
							static_cast<double>(x), static_cast<double>(y), cellPx);
						if (frac > 0.0)
							candOut.at(x, y) += frac * (inFullBands.H.at(x, y) - outFullBands.H.at(x, y));
					}
				}
			}

			// Tally harmonized unique-cell non-finite counter.
			for (uint8_t nf : nonFiniteCellFlag)
				if (nf != 0u) ++res.nonFiniteCells;

			// Update legacy recurrent carry-forward buffer strictly with the grain-free estimate `candClean`.
			if (effFadeKeep < 1.0)
			{
				carryClean_ = TdV2Plane(w, h, 0.0);
				const double invKeep = 1.0 - effFadeKeep;
				for (size_t i = 0; i < candClean.v.size(); ++i)
					carryClean_.v[i] = effFadeKeep * candClean.v[i] + invKeep * curClean.v[i];
			}
			else
			{
				carryClean_ = candClean;
			}
			hasCarry_ = true;

			// TD-U5 (H-4 / Owner D2): Re-anchor single-resample carry strictly at confirmed peaks (grain-free candClean only!),
			// installing the next-span drift bar mask and clearing held-out re-seed flags.
			if (in.isConfirmedPeak && !effectiveCut)
			{
				anchorClean_ = candClean;
				hasAnchor_ = true;
				anchorAgeFrames_ = 1;
				anchorBarredCells_ = std::move(nextSpanBarredCells);
				anchorReseedCells_.assign(numCells, 0u);
			}
			else if (hasAnchor_)
			{
				++anchorAgeFrames_;
			}

			hasLastSeq_ = true;
			lastSeq_ = in.seq;
			res.outputClean = candClean;

			// Shadow mode computes maps & stats without modifying output pixels; Mode::On writes outputY.
			if (cfg_.ModifiesPixels())
			{
				res.outputY = candOut;
			}
			return res;
		}

	private:
		struct SameRoleCellStat
		{
			double C[kMaxBlendRefs][kMaxBlendRefs] = {};
			double cIvRaw[kMaxBlendRefs] = {};
			bool hasPartner[kMaxBlendRefs] = {};
			size_t sampleCount = 0;
		};
		struct SameRoleStatsSlot
		{
			bool valid = false;
			int cellsW = 0;
			int cellsH = 0;
			int refCount = 0;
			double timeSec = 0.0;
			RefRole refRoles[kMaxBlendRefs] = {};
			std::array<std::vector<SameRoleCellStat>, 3> bands;
			void Reset()
			{
				valid = false;
				cellsW = 0;
				cellsH = 0;
				refCount = 0;
				timeSec = 0.0;
				for (int i = 0; i < kMaxBlendRefs; ++i)
					refRoles[i] = RefRole::PrevPeak;
				for (auto& v : bands)
					v.clear();
			}
		};

		Config cfg_;
		bool hasCarry_ = false;
		TdV2Plane carryClean_;
		bool hasAnchor_ = false;
		TdV2Plane anchorClean_;
		int anchorAgeFrames_ = 0;
		std::vector<uint8_t> anchorBarredCells_;
		std::vector<uint8_t> anchorReseedCells_;
		bool hasLastSeq_ = false;
		uint64_t lastSeq_ = 0;
		double elapsedSec_ = 0.0;
		A2ePriorHistory a2eHistory_;
		std::array<SameRoleStatsSlot, 3> sameRoleStats_;
	};
}
