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
		StandInRegistry* standIns = nullptr)
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
				const double vx = cellVx[cIdx];
				const double vy = cellVy[cIdx];
				if (!std::isfinite(vx) || !std::isfinite(vy))
				{
					++diag.nonFinitePixels;
					// Leave fallbackClean value in outWarped (no zero-hole) with outValid = 0.
					continue;
				}

				const double sxLocal = static_cast<double>(x - ar.x0) + vx;
				const double syLocal = static_cast<double>(y - ar.y0) + vy;
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

	// ---- Motion Classification & Camera-Relative Fast-Object Motion Gate (plan s1.3) ----
	enum class RegionMotionClass : uint8_t
	{
		Still = 0,
		Camera = 1,
		Object = 2,
		Overlay = 3,
		DynamicTexture = 4
	};

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

		bool IsWellFormed(int expectedCW, int expectedCH) const
		{
			if (expectedCW <= 0 || expectedCH <= 0 || cellsW != expectedCW || cellsH != expectedCH)
				return false;
			const size_t n = static_cast<size_t>(expectedCW) * static_cast<size_t>(expectedCH);
			return vx.size() == n && vy.size() == n &&
			       camVx.size() == n && camVy.size() == n &&
			       cls.size() == n && valid.size() == n;
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
		size_t nonFiniteCells = 0;
	};

	// Compute camera-relative object speed gate with Karis 3x3 max-velocity silhouette dilation.
	inline MotionGateResult EvaluateCameraRelativeMotionGate(const CellMotionField& mf, StandInRegistry* standIns = nullptr)
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

		std::vector<double> relSpeed(n, 0.0);
		std::vector<uint8_t> finiteCell(n, 0u);
		for (size_t i = 0; i < n; ++i)
		{
			if (!mf.valid[i])
			{
				relSpeed[i] = kFastObjectCutoffPx * 2.0;
				continue;
			}
			if (!std::isfinite(mf.vx[i]) || !std::isfinite(mf.vy[i]) ||
			    !std::isfinite(mf.camVx[i]) || !std::isfinite(mf.camVy[i]))
			{
				relSpeed[i] = kFastObjectCutoffPx * 2.0;
				++out.nonFiniteCells;
				continue;
			}
			finiteCell[i] = 1u;
			const double dvx = mf.vx[i] - mf.camVx[i];
			const double dvy = mf.vy[i] - mf.camVy[i];
			relSpeed[i] = std::sqrt(dvx * dvx + dvy * dvy);
		}
		if (out.nonFiniteCells > 0 && standIns != nullptr)
			standIns->Engage(StandInKind::NonFiniteClosure, "EvaluateCameraRelativeMotionGate non-finite vector");

		for (int cy = 0; cy < mf.cellsH; ++cy)
		{
			for (int cx = 0; cx < mf.cellsW; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy) * mf.cellsW + cx;
				if (!finiteCell[idx])
					continue;
				const RegionMotionClass c = mf.cls[idx];
				if (c == RegionMotionClass::Overlay)
				{
					// Hard drop on overlays/subtitles across ALL bands (prevents ghost letters).
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
				out.gateL[idx] = (c == RegionMotionClass::Object) ? speedGate : 1.0;

				if (c == RegionMotionClass::DynamicTexture)
				{
					// Hard drop on chaotic textures (water/rain/foliage/fire) for M/H.
					out.gateMH[idx] = 0.0;
				}
				else
				{
					out.gateMH[idx] = speedGate;
				}
			}
		}
		return out;
	}

	// ---- Frequency-Split Focus Guard (Owner Focus Rule + Q4 Bokeh Study Knob) ----
	struct FocusCellInfo
	{
		double focusRatio = 1.0;    // E2 / (E2 + E3)
		bool isDefocused = false;
		bool focusChange = false;   // Focal-plane shift detected -> hard veto ALL bands (M/H always; L too per MEDIUM-2)
	};

	// Compute physics-derived E2/(E2+E3) focus stand-in map using AnalyticalEdgeFocusRatio(sigmaPsf):
	//   - Defocused cell: curEnergy > structureFloor (above 2-sigma quantization step variance so W1 banded sky
	//     is NEVER self-vetoed!) AND curF <= AnalyticalEdgeFocusRatio(kSigmaBase).
	//   - Focus change (rack focus): refEnergy > structureFloor AND (refF - curF) > (F(0) - F(kSigmaBase)).
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
		StandInRegistry* standIns)
	{
		if (standIns != nullptr)
			standIns->Engage(StandInKind::FocusMapK32);

		const size_t numCells = static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH);
		std::vector<FocusCellInfo> info(numCells);
		const double depthScale = DepthScale(sourceBits);
		// Map full-spectrum stepTotalVar into the exact (M + D3) band-energy domain via BandFromBase's
		// 2D white-noise transfer gain gainMD3 = ||M||_2^2 + ||D3||_2^2 (~= 0.05589), and apply the exact
		// 0-dB SNR cell-energy boundary (S >= N <=> S + N >= 2 * N) so genuine defocused bokeh is detected
		// while smooth quantization-noise-dominated W1 banded sky stays unvetoed!
		const BandWhiteNoiseGains focusGains = ComputeBandWhiteNoiseGains();
		const double gainMD3 = focusGains.mid + focusGains.d3;
		const double stepNoiseVar = (std::max)(0.0, stepTotalVar);
		const double structureFloor = (std::max)(depthScale * depthScale, 2.0 * gainMD3 * stepNoiseVar);
		const double fSharp = AnalyticalEdgeFocusRatio(0.0);
		const double fDefocus = AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase);
		const double fShiftDelta = fSharp - fDefocus;

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

				// Noise-debiased focus ratio for optical bokeh detection:
				// White coding noise passes ~6.5x more variance into M (gains.mid) than D3 (gains.d3),
				// biasing raw curF upward on noisy valley bokeh; subtracting the exact BandFromBase
				// white-noise variance restores the true optical defocus ratio (zero new constants).
				const double curM3 = (std::max)(0.0, curEnergy - curM2);
				const double cleanM2 = (std::max)(0.0, curM2 - stepNoiseVar * focusGains.mid);
				const double cleanM3 = (std::max)(0.0, curM3 - stepNoiseVar * focusGains.d3);
				const double curFDebiased = cleanM2 / (cleanM2 + cleanM3 + kEngineNumFloor);

				if (curEnergy > structureFloor && curFDebiased <= fDefocus)
					info[idx].isDefocused = true;

				if (explicitFocusChangeCells != nullptr && idx < explicitFocusChangeCells->size() && (*explicitFocusChangeCells)[idx] != 0)
				{
					info[idx].focusChange = true;
				}
				else
				{
					// Detect focal-plane shift (rack focus) respecting Parseval's theorem:
					// Optical defocus (|H(w)| <= 1) strictly reduces mid-band energy (curM2 < refM2) and pulls
					// curF < fSharp, so bright adjacent overlays / appearance changes (where curM2 >= refM2)
					// never trigger false focusChange vetoes on normal neighbor cells.
					for (const auto& rb : refBands)
					{
						double refEnergy = 0.0, refM2 = 0.0;
						const double refF = cellFocusRatio(rb, cx, cy, refEnergy, refM2);
						if (refEnergy > structureFloor && curM2 < refM2 && curF < fSharp && (refF - curF) > fShiftDelta)
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
		CellMotionField carryMotion;          // Current -> Previous finalized clean carry motion (used when carry valid)
		bool hasCarryMotion = false;

		// Explicit coding step prior from P16 (when hasStepPrior == false, engine derives 1-LSB default & engages StandInKind::StepPriorDefault).
		bool hasStepPrior = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool transformIdentified8x8 = false;  // True only when codec transform (e.g. AVC 8x8) is identified

		bool isCutOrSpanInvalid = false;      // Resets recurrent carry state immediately
		const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
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
		size_t focusVetoCells = 0;            // Focus-change cells vetoed
		size_t nonFiniteCells = 0;            // Cells closed to zero on non-finite input
		size_t singularCells = 0;             // Cells closed to zero on singular covariance solve
		size_t singleRefA2Cells = 0;          // Cells where K==1 fell back from A1 to A2 visibly
		size_t motionStandInRefs = 0;         // References using null/mismatched motion stand-in visibly
		size_t borderBilinearPixels = 0;      // Active-rect 1-2 px border bilinear fallback pixel count
		bool carryUsed = false;
		bool carryReset = false;
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

	// ---- Full CPU Reference Fusion Engine (Stage TD-S3) ----
	class TdV2ReferenceEngine
	{
	public:
		explicit TdV2ReferenceEngine(const Config& cfg = Config{})
			: cfg_(cfg)
		{
		}

		void SetConfig(const Config& cfg) { cfg_ = cfg; }
		const Config& GetConfig() const { return cfg_; }

		void ResetCarry()
		{
			hasCarry_ = false;
			carryClean_ = TdV2Plane();
		}

		bool HasCarry() const { return hasCarry_; }
		const TdV2Plane& CarryClean() const { return carryClean_; }

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

			// Mode::Off -> zero work, byte-identical passthrough.
			if (!cfg_.IsEnabled())
				return res;

			// Upfront sourceBits validation (Rule 4: invalid bit depth closes cleanly and visibly).
			if (!(PeakCodeForBits(cfg_.sourceBits) > 0.0))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ReferenceEngine invalid sourceBits");
				ResetCarry();
				res.carryReset = true;
				res.nonFiniteCells = numCells;
				return res;
			}

			// Visible telemetry when long-reach I-anchor is enabled in config without a long-pair producer.
			if (cfg_.enableLongAnchor && standIns != nullptr)
				standIns->Engage(StandInKind::LongPairDrift, "enableLongAnchor requested; 1-session long pair pending");

			// Cut / invalid span or resolution change resets recurrent carry cleanly (zero ghosting).
			if (in.isCutOrSpanInvalid || (hasCarry_ && (carryClean_.w != w || carryClean_.h != h)))
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

			// Layer 1: Grain-free clean & own-grain separation.
			TdV2Plane curClean(w, h, 0.0);
			TdV2Plane ownGrain(w, h, 0.0);
			if (in.hasCleanSplit && !in.cleanY.Empty() && in.cleanY.w == w && in.cleanY.h == h)
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
				// Visible P10g stand-in: without upstream grain split, operate with zero own-grain extraction.
				if (standIns != nullptr)
					standIns->Engage(StandInKind::P10gGrainSeparator, "no upstream P10g split supplied");
				curClean = in.receivedY;
			}
			res.outputClean = curClean;

			// Decompose current frame's grain-free clean into L, M, H, D3 via U2 BandFromBase.
			const ThreeBandPlanes curBands = DecomposeActiveBands(curClean, active, standIns);

			// Assemble available aligned grain-free references (up to 3: prevPeak, nextPeak, recurrent carry).
			struct WarpedRef
			{
				TdV2Plane clean;
				ThreeBandPlanes bands;
				std::vector<uint8_t> validPx;
				MotionGateResult motionGate;
				bool isExactCopy = false;
				bool isCarry = false;
			};
			std::vector<WarpedRef> refs;
			refs.reserve(3);

			auto tryAddRef = [&](const TdV2Plane* refClean, const CellMotionField* mf, bool exactCopy, bool isCarry) -> bool
			{
				if (refClean == nullptr || refClean->Empty() || refClean->w != w || refClean->h != h)
					return false;
				CellMotionField fallbackMf;
				const CellMotionField* useMf = mf;
				if (useMf == nullptr)
				{
					// Null motion field: visibly record stand-in assuming Still scene.
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "null motion field; assuming Still");
					fallbackMf = CellMotionField::Uniform(res.cellsW, res.cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					useMf = &fallbackMf;
				}
				else if (!useMf->IsWellFormed(res.cellsW, res.cellsH))
				{
					// Dimension-mismatched / malformed motion field: reject reference visibly (zero OOB crash!).
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "dimension-mismatched motion field; ref rejected");
					return false;
				}

				WarpedRef wr;
				wr.isExactCopy = exactCopy;
				wr.isCarry = isCarry;
				const WarpDiagnostics wDiag = WarpCleanReference(
					*refClean, curClean, active, useMf->vx, useMf->vy,
					res.cellsW, res.cellsH, cellPx, wr.clean, wr.validPx, standIns);
				res.borderBilinearPixels += wDiag.borderBilinearPixels;
				if (wDiag.nonFinitePixels > 0)
					++res.nonFiniteCells;
				wr.bands = DecomposeActiveBands(wr.clean, active, standIns);
				wr.motionGate = EvaluateCameraRelativeMotionGate(*useMf, standIns);
				res.nonFiniteCells += wr.motionGate.nonFiniteCells;
				refs.push_back(std::move(wr));
				return true;
			};

			if (!in.isCutOrSpanInvalid)
			{
				if (in.prevPeak.cleanPlane != nullptr)
					tryAddRef(in.prevPeak.cleanPlane, in.prevPeak.motion, in.prevPeak.isExactCopyOfCurrent, false);
				if (in.nextPeak.cleanPlane != nullptr)
					tryAddRef(in.nextPeak.cleanPlane, in.nextPeak.motion, in.nextPeak.isExactCopyOfCurrent, false);
				if (cfg_.enableCarryForward && hasCarry_ && carryClean_.w == w && carryClean_.h == h)
				{
					if (tryAddRef(&carryClean_, in.hasCarryMotion ? &in.carryMotion : nullptr, false, true))
						res.carryUsed = true;
				}
			}

			res.activeRefCount = refs.size();
			if (refs.empty())
			{
				// First frame / cut frame: seed recurrent carry with current clean plane; output stays receivedY.
				carryClean_ = curClean;
				hasCarry_ = true;
				return res;
			}

			// Coding step prior: use caller's P16 prior when supplied, else derived 1-LSB default visibly tagged.
			double effStepDelta = in.estimatedStepDelta;
			double effZeroFrac = in.zeroCoeffFraction;
			if (!in.hasStepPrior || !std::isfinite(effStepDelta) || effStepDelta <= 0.0 || !std::isfinite(effZeroFrac))
			{
				effStepDelta = DepthScale(cfg_.sourceBits); // Exact 1-LSB quantization step at sourceBits
				effZeroFrac = 0.0;                          // Uniform rounding cell without invented pi0
				if (standIns != nullptr)
					standIns->Engage(StandInKind::StepPriorDefault);
			}
			const RoundingCellStats a2Stats = ComputeRoundingCellStats(effStepDelta, in.deadZonePhi, effZeroFrac);

			// Exact derived 2D separable white-noise transfer gains from U2 BandFromBase kernel taps (HIGH-1: zero magic/over-subtraction).
			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double bandTransfer[3] = { gains.low, gains.mid, gains.fine };

			// Evaluate physics-derived frequency-split focus guard across all aligned references.
			std::vector<ThreeBandPlanes> refBandList;
			refBandList.reserve(refs.size());
			for (const auto& wr : refs)
				refBandList.push_back(wr.bands);
			const auto focusCells = EvaluateFocusGuardStandIn(
				curBands, refBandList, active, res.cellsW, res.cellsH, cellPx, cfg_.sourceBits,
				a2Stats.valid ? a2Stats.totalVar : 0.0,
				in.explicitFocusChangeCells, standIns);

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

			auto bandPlaneOf = [](const ThreeBandPlanes& bp, int b) -> const TdV2Plane&
			{
				return (b == 0) ? bp.L : ((b == 1) ? bp.M : bp.H);
			};

			const int poolR = cfg_.momentPoolRadiusCells > 0 ? cfg_.momentPoolRadiusCells : 0;
			const int gW = (w + grainCellPx - 1) / grainCellPx;
			std::vector<double> cellRetainedGrain(numCells, 0.0);

			// Per-cell moment pooling (FP32 centered), blind c estimation (A1/A2/A3), convex w* = C^-1 c, motion & focus gates.
			for (int cy = 0; cy < res.cellsH; ++cy)
			{
				for (int cx = 0; cx < res.cellsW; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					const FocusCellInfo& fInfo = focusCells[cIdx];
					if (fInfo.focusChange)
						++res.focusVetoCells;

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
								++res.nonFiniteCells;
								if (standIns != nullptr)
									standIns->Engage(StandInKind::NonFiniteClosure, "non-finite retainedGrainVar");
							}
						}
					}
					cellRetainedGrain[cIdx] = cellRetainedGrainVar;

					// Compute spatial fine-band detail RMS for TdMapsView::kRealDetail (MEDIUM-3: spatial detail, NOT temporal noise).
					double spatialFineEnergy = 0.0;
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
								const double hv = curBands.H.at(x, y);
								spatialFineEnergy += hv * hv;
								++spatialPxCnt;
							}
						}
					}
					const double spatialDetailRms = spatialPxCnt > 0
						? std::sqrt(spatialFineEnergy / static_cast<double>(spatialPxCnt))
						: 0.0;

					double cellWeightSumAll = 0.0;
					double cellResEnergy = 0.0;
					unsigned activeBandBits = 0;

					for (int b = 0; b < 3; ++b)
					{
						const TdV2Plane& curB = bandPlaneOf(curBands, b);
						// Exact Welford online mean-centered second-moment accumulator (zero first-pixel-anchor
						// catastrophic cancellation; matches GPU Chan-Pebay parallel merge to FP32 precision).
						double muWelford[kMaxBlendRefs] = {};
						double scatterWelford[kMaxBlendRefs][kMaxBlendRefs] = {};
						size_t sampleCount = 0;

						const int py0 = (std::max)(active.y0, (cy - poolR) * cellPx);
						const int py1 = (std::min)(active.y1, (cy + poolR + 1) * cellPx);
						const int px0 = (std::max)(active.x0, (cx - poolR) * cellPx);
						const int px1 = (std::min)(active.x1, (cx + poolR + 1) * cellPx);

						for (int y = py0; y < py1; ++y)
						{
							const int pcy = (std::min)(res.cellsH - 1, y / cellPx);
							for (int x = px0; x < px1; ++x)
							{
								const int pcx = (std::min)(res.cellsW - 1, x / cellPx);
								const size_t pCell = static_cast<size_t>(pcy) * res.cellsW + pcx;
								// Exclude pixels owned by hard-vetoed cells (focusChange, Overlay, DynamicTexture,
								// fast Object, invalid motion) from 3x3 covariance pooling so vetoed regions never
								// poison neighboring normal cells' second moments (eliminates 48x48 dead-zone halo).
								if (focusCells[pCell].focusChange)
									continue;

								const size_t pIdx = static_cast<size_t>(y) * w + x;
								bool allValid = true;
								double diffs[kMaxBlendRefs] = {};
								const double curV = curB.at(x, y);
								for (int k = 0; k < K; ++k)
								{
									const double pGate = (b == 0) ? refs[k].motionGate.gateL[pCell] : refs[k].motionGate.gateMH[pCell];
									if (refs[k].validPx[pIdx] == 0u || pGate <= 0.0)
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
									diffs[k] = dVal;
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
								}
							}
						}

						if (sampleCount == 0)
							continue;

						double C[kMaxBlendRefs][kMaxBlendRefs] = {};
						const double invSampleN = 1.0 / static_cast<double>(sampleCount);
						for (int i = 0; i < K; ++i)
						{
							for (int j = i; j < K; ++j)
							{
								const double m2 = scatterWelford[i][j] * invSampleN + muWelford[i] * muWelford[j];
								const double val = (i == j && m2 < 0.0) ? 0.0 : m2;
								C[i][j] = val;
								C[j][i] = val;
							}
						}
						for (int k = 0; k < K; ++k)
							cellDiagC[b][k][cIdx] = C[k][k];

						double cVec[kMaxBlendRefs] = {};
						for (int k = 0; k < K; ++k)
						{
							// A2 rounding-cell prior for ref k on band b using exact BandFromBase transfer gain.
							const double cA2 = EstimateC_A2(a2Stats, refs[k].isExactCopy) * bandTransfer[b];

							// A1 grain-corrected Three-Cornered Hat using an independent partner instrument l != k:
							//   - k == 0 (prevPeak): partner = 1 (nextPeak)
							//   - k == 1 (nextPeak): partner = 0 (prevPeak)
							//   - k == 2 (carry):    partner = 1 (nextPeak, independent of past carry! — MEDIUM-1)
							double cA1 = cA2;
							if (K >= 2)
							{
								const int partner = (k == 0 || k == 2) ? 1 : 0;
								cA1 = EstimateC_A1(C[k][partner], C[k][k], C[partner][partner],
								                   cellRetainedGrainVar, bandTransfer[b]);
							}
							else if (cfg_.estimator != VarianceEstimator::A2_RoundingCells)
							{
								// Single-reference K == 1 fallback from A1 to A2 is recorded visibly (HIGH-7).
								++res.singleRefA2Cells;
								if (standIns != nullptr)
									standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
							}

							cVec[k] = SelectBlindC(cfg_.estimator, cA1, cA2);
							// Exact copy of reference k always gets c_k = 0 (refs/111 s2.2 fact 3).
							if (refs[k].isExactCopy)
								cVec[k] = 0.0;
						}

						const BlendSolveResult solved = SolveConvexWeights(C, cVec, K, kDefaultTraceRegScale, standIns);
						if (solved.nonFiniteClosed)
						{
							++res.nonFiniteCells;
							continue;
						}
						if (solved.singular)
						{
							++res.singularCells;
							continue;
						}

						// Apply Focus Rule (plan s1.4, Q4, MEDIUM-2):
						// - Unmatched focal-plane shift (focusChange) ALWAYS vetoes ALL bands (L, M, H), even under LowBandCellOnly!
						// - Matched-focus bokeh (isDefocused && !focusChange) vetoes M/H always; L allowed ONLY under LowBandCellOnly.
						double focusGate = 1.0;
						if (fInfo.focusChange)
						{
							focusGate = 0.0;
						}
						else if (fInfo.isDefocused)
						{
							focusGate = (b == 0 && cfg_.bokehMode == BokehMode::LowBandCellOnly) ? 1.0 : 0.0;
						}

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
					mBlock.f[TdMapsView::kCodingNoise] = static_cast<float>(std::sqrt((std::max)(0.0, a2Stats.totalVar)));
					mBlock.f[TdMapsView::kLightGain] = 1.0f;
					mBlock.f[TdMapsView::kLightOffset] = 0.0f;
					mBlock.f[TdMapsView::kDrift] = 0.0f;
					mBlock.f[TdMapsView::kBandMask] = static_cast<float>(activeBandBits);
				}
			}

			// Per-pixel spatial-only interpolation (ZERO bleed across vetoed cell boundaries — HIGH-5),
			// 3-sigma Huber taper, and pixel-domain bound when transform is unknown.
			// Codec-frame-grid-aligned interior full 8x8 bounds so non-8-aligned letterbox/pillarbox bars
			// never misalign the codec's 8x8 grid and leading/trailing partial strips receive ApplyTransformUnknownBound.
			TdV2Plane deltaPlane(w, h, 0.0);
			const int bxStart = ((active.x0 + 7) / 8) * 8;
			const int byStart = ((active.y0 + 7) / 8) * 8;
			const int bxEnd = (active.x1 / 8) * 8;
			const int byEnd = (active.y1 / 8) * 8;

			for (int y = active.y0; y < active.y1; ++y)
			{
				const int cy = (std::min)(res.cellsH - 1, y / cellPx);
				for (int x = active.x0; x < active.x1; ++x)
				{
					const int cx = (std::min)(res.cellsW - 1, x / cellPx);
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					const size_t pIdx = static_cast<size_t>(y) * w + x;

					// Pixels in leading/trailing non-8-aligned partial strips cannot enter an interior 8x8 DCT block,
					// so they ALWAYS receive the derived pixel-domain band bound (HIGH-6).
					const bool inFull8x8Block = in.transformIdentified8x8 && (bxStart < bxEnd) && (byStart < byEnd) &&
					                            (x >= bxStart) && (x < bxEnd) && (y >= byStart) && (y < byEnd);

					double totalDelta = 0.0;
					for (int b = 0; b < 3; ++b)
					{
						const double curV = bandPlaneOf(curBands, b).at(x, y);
						double bandDelta = 0.0;
						for (int k = 0; k < K; ++k)
						{
							if (refs[k].validPx[pIdx] == 0u)
								continue;
							// Hard veto boundary guard (HIGH-5): if the pixel's own cell has weight 0
							// (Overlay, DynamicTexture, focusChange, defocused, exact copy, or invalid motion),
							// force weight strictly 0.0 so adjacent active cells NEVER bleed across the boundary!
							if (cellW[b][k][cIdx] <= 0.0)
								continue;

							// Corner-tapered spatial-only interpolation (ZERO temporal weight ramps):
							// applies 3-sigma Huber pixel taper per bilinear corner q using that corner's OWN variance C_q
							// before combining, protecting sub-block micro-motion boundaries even when poolR == 0.
							const double diff = curV - bandPlaneOf(refs[k].bands, b).at(x, y);
							const double wEff = SpatialBilinearHuberWeight(
								cellW[b][k].data(), cellDiagC[b][k].data(), res.cellsW, res.cellsH,
								static_cast<double>(x), static_cast<double>(y), cellPx, diff);
							if (wEff <= 0.0)
								continue;
							bandDelta -= wEff * diff;
						}
						// Apply derived per-band quantization-cell bound when transform is unknown OR in remainder strip.
						if (!inFull8x8Block)
						{
							const double bandVar = a2Stats.valid ? (a2Stats.totalVar * bandTransfer[b]) : 0.0;
							if (std::fabs(bandDelta) > 0.0)
								bandDelta = ApplyTransformUnknownBound(bandDelta, bandVar, standIns);
						}
						totalDelta += bandDelta;
					}
					deltaPlane.at(x, y) = totalDelta;
				}
			}

			// Identified 8x8 DCT transform-domain STRICT/CELL projection strictly on the coded residual r = X - P_B (HIGH-6),
			// over codec-frame-grid-aligned interior blocks [bxStart, bxEnd) x [byStart, byEnd),
			// alternating with the pixel-domain code range [0, PeakCodeForBits] for genuine 2-set POCS!
			if (in.transformIdentified8x8 && effStepDelta > 0.0 && (bxStart < bxEnd) && (byStart < byEnd))
			{
				const double peakCode = PeakCodeForBits(cfg_.sourceBits);
				const TdV2Plane& predClean = refs[0].clean; // P_B primary reference prediction
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
							for (int j = 0; j < 8; ++j)
							{
								for (int i = 0; i < 8; ++i)
								{
									const double cVal = curClean.at(bx + i, by + j);
									const double pVal = predClean.at(bx + i, by + j);
									sentRes[j * 8 + i] = cVal - pVal;
									blendRes[j * 8 + i] = (cVal + deltaPlane.at(bx + i, by + j)) - pVal;
								}
							}
							ProjectResidual8x8Dct(sentRes, blendRes, effStepDelta, in.deadZonePhi, cfg_.residualMode, projRes);
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
					// Untouched zero-delta pixels stay bit-exact (preserving out-of-range BTB/WTW untouched pixels).
					if (peakCode > 0.0)
					{
						for (int y = byStart; y < byEnd; ++y)
						{
							for (int x = bxStart; x < bxEnd; ++x)
							{
								const double dVal = deltaPlane.at(x, y);
								if (dVal == 0.0)
									continue;
								const double cVal = curClean.at(x, y);
								const double clamped = (std::max)(0.0, (std::min)(peakCode, cVal + dVal));
								deltaPlane.at(x, y) = clamped - cVal;
							}
						}
					}
				}
			}

			// Build candidate recomposed output (clean + delta) + ownGrain strictly inside ActiveRect.
			TdV2Plane candClean = curClean;
			TdV2Plane candOut = in.receivedY;
			for (int y = active.y0; y < active.y1; ++y)
			{
				for (int x = active.x0; x < active.x1; ++x)
				{
					const double d = deltaPlane.at(x, y);
					candClean.at(x, y) = curClean.at(x, y) + d;
					// Exact grain preservation: ownGrain = receivedY - curClean is NEVER modified or averaged!
					candOut.at(x, y) = candClean.at(x, y) + ownGrain.at(x, y);
				}
			}

			// Layer 4: Shared Output Grain Guard (TdGuardPort stand-in for W4c GrainSplit::Guard).
			// Guards any cell where grain is present (ownGrain OR leaked retainedGrainVar) by comparing output fine-band
			// energy against the cell's expected grain fine-band floor (never falsely undoing clean coding-noise repair!).
			// Adds missing fine variance strictly to `candOut` ONLY — NEVER touching `candClean` / `carryClean_`
			// so own grain NEVER enters frame t+1's recurrent carry as foreign grain (BLOCKER-2)!
			const ThreeBandPlanes inFullBands = DecomposeActiveBands(in.receivedY, active, nullptr);
			const ThreeBandPlanes outFullBands = DecomposeActiveBands(candOut, active, nullptr);
			const ThreeBandPlanes ownGrainBands = in.hasCleanSplit
				? DecomposeActiveBands(ownGrain, active, nullptr)
				: ThreeBandPlanes{};

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
					double ownGrainEnergy = 0.0;
					double ownGrainFineE = 0.0;
					double inFineE = 0.0, outFineE = 0.0;
					size_t cnt = 0;
					for (int y = y0; y < y1; ++y)
					{
						for (int x = x0; x < x1; ++x)
						{
							const double og = ownGrain.at(x, y);
							const double inf = inFullBands.H.at(x, y);
							const double outf = outFullBands.H.at(x, y);
							ownGrainEnergy += og * og;
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
					if (ownGrainEnergy * invC <= kEngineNumFloor && cellRetainedGrain[cIdx] <= kEngineNumFloor)
						continue;

					const double leakedGrainFineE = cellRetainedGrain[cIdx] * gains.fine;
					const double targetGrainFineE = in.hasCleanSplit
						? (ownGrainFineE * invC + leakedGrainFineE)
						: (leakedGrainFineE > kEngineNumFloor ? (std::min)(inFineE * invC, leakedGrainFineE) : (inFineE * invC));

					const auto gDec = TdGuardPort::Evaluate(targetGrainFineE, outFineE * invC, standIns);
					if (gDec.violated && gDec.addBackFraction > 0.0)
					{
						++res.guardViolationCells;
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

			// Update recurrent carry-forward buffer strictly with the grain-free estimate `candClean`.
			carryClean_ = candClean;
			hasCarry_ = true;
			res.outputClean = candClean;

			// Shadow mode computes maps & stats without modifying output pixels; Mode::On writes outputY.
			if (cfg_.ModifiesPixels())
			{
				res.outputY = candOut;
			}
			return res;
		}

	private:
		Config cfg_;
		bool hasCarry_ = false;
		TdV2Plane carryClean_;
	};
}
