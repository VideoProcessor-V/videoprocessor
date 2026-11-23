// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S3 (2026-11-23_TD-consolidated-plan.md s1.1-s1.6, s3.2):
// Full CPU Reference Fusion Engine with explicit 4-layer grain preservation:
//   - Dual-source grain-free fusion: prev peak + next peak + recurrent carry-forward in one w* = C^-1 c solve
//   - Shared bicubic resampling via VpComposed::SampleBicubic strictly inside ActiveRect (bar edges hard-clamped)
//   - Shared L/M/H decomposition via U2 VpUnit::BandFromBase (zero private Gaussian copies)
//   - Camera-relative fast-object motion gate (|v - v_cam| + 3x3 Karis max-velocity silhouette dilation +
//     Overlay / DynamicTexture zero-weight masks; camera pans are NEVER gated off)
//   - Frequency-split focus guard (protects focal-plane shifts on M/H; Q4 optional L-band bokeh mode)
//   - Explicit 4-layer grain preservation (L1 separation, L2 A1 grain subtraction, L3 convex+cell bounds,
//     L4 TdGuardPort output guard; current frame's own grain G_t is re-injected 1:1 untouched)
//   - Populates U19 TdMapsView::Block per 16x16 cell
//
// Every constant is derived from signal physics or existing VP-V contracts (Rule 1).
// Every stand-in / fallback is visibly recorded in StandInRegistry (Rule 4).
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
	// ---- Rule-1 Derived Motion & Focus Gate Constants ----

	// Quarter-pel sub-pixel tracking ceiling (P3r / CellTrust): object speed <= 0.25 px/frame receives full weight 1.0.
	constexpr double kStillObjectSpeedPx = 0.25;

	// Fast-object motion cutoff: 2 * kSigmaFine (= 2.0 px/frame), where 1-frame object displacement spans the fine filter support.
	constexpr double kFastObjectCutoffPx = 2.0 * VpUnit::BandFromBase::kSigmaFine;

	// Focus ratio F = E2 / (E2 + E3): a focal-plane shift where reference sharpness ratio exceeds current frame's
	// sharpness ratio by more than 1/8 (= 0.125, one octant of the [0, 1] band-energy balance) vetoes M/H borrowing.
	constexpr double kFocusShiftDelta = 0.125;

	// Defocus detection floor: when E2 / (E2 + E3) <= 0.25 (mid-band holds <= 1/4 of mid+low structure energy)
	// on a structured cell, the cell is classified as defocused (bokeh).
	constexpr double kDefocusRatioCeiling = 0.25;

	// Minimum mid+low cell energy (codes^2 at 10-bit, scaled by DepthScale^2) to judge focus vs flat region:
	// equals 1.0 code^2 (unit quantization step variance floor).
	constexpr double kFocusMinEnergy10Bit = 1.0;

	// Strictly numerical denominator floor.
	constexpr double kEngineNumFloor = 1e-9;

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

	// Warp `refClean` into current frame coordinates using shared VpComposed::SampleBicubic inside `active`.
	// Motion `(vx, vy)` gives the displacement from current pixel `(x, y)` to reference pixel `(x + vx, y + vy)`.
	// Returns a validity mask (1 = sampled strictly inside active rect, 0 = out of footprint / non-finite).
	inline void WarpCleanReference(
		const TdV2Plane& refClean,
		const ActiveRect& active,
		const std::vector<double>& cellVx,
		const std::vector<double>& cellVy,
		int cellsW,
		int cellsH,
		int cellPx,
		TdV2Plane& outWarped,
		std::vector<uint8_t>& outValid)
	{
		const int w = refClean.w;
		const int h = refClean.h;
		outWarped = TdV2Plane(w, h, 0.0);
		outValid.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);

		const ActiveRect ar = active.ClampedTo(w, h);
		if (ar.Empty() || cellsW <= 0 || cellsH <= 0 || cellPx <= 0)
			return;

		ActiveSubPlaneView view{ &refClean, ar.x0, ar.y0, ar.Width(), ar.Height() };

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
					continue;

				const double sxLocal = static_cast<double>(x - ar.x0) + vx;
				const double syLocal = static_cast<double>(y - ar.y0) + vy;
				double val = 0.0;
				if (VpComposed::SampleBicubic(view, sxLocal, syLocal, val) && std::isfinite(val))
				{
					outWarped.at(x, y) = val;
					outValid[static_cast<size_t>(y) * w + x] = 1u;
				}
				else if (sxLocal >= 0.0 && syLocal >= 0.0 &&
				         sxLocal <= static_cast<double>(ar.Width() - 1) &&
				         syLocal <= static_cast<double>(ar.Height() - 1))
				{
					// Active-rect 1-2 px border fallback: bilinear inside active rect (never crossing bar edges).
					const int ix0 = static_cast<int>(std::floor(sxLocal));
					const int iy0 = static_cast<int>(std::floor(syLocal));
					const int ix1 = (std::min)(ar.Width() - 1, ix0 + 1);
					const int iy1 = (std::min)(ar.Height() - 1, iy0 + 1);
					const double fx = sxLocal - ix0;
					const double fy = syLocal - iy0;
					const double v00 = view.at(ix0, iy0);
					const double v10 = view.at(ix1, iy0);
					const double v01 = view.at(ix0, iy1);
					const double v11 = view.at(ix1, iy1);
					const double bVal = (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy;
					if (std::isfinite(bVal))
					{
						outWarped.at(x, y) = bVal;
						outValid[static_cast<size_t>(y) * w + x] = 1u;
					}
				}
			}
		}
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
		std::vector<double> gateL;    // 0 on Overlay/invalid, 1 otherwise
		std::vector<double> gateMH;   // Camera-relative smooth speed fade + 3x3 silhouette dilation, 0 on Overlay/DynamicTexture
	};

	// Compute camera-relative object speed gate with Karis 3x3 max-velocity silhouette dilation.
	inline MotionGateResult EvaluateCameraRelativeMotionGate(const CellMotionField& mf)
	{
		MotionGateResult out;
		const size_t n = static_cast<size_t>(mf.cellsW) * static_cast<size_t>(mf.cellsH);
		out.gateL.assign(n, 0.0);
		out.gateMH.assign(n, 0.0);
		if (mf.cellsW <= 0 || mf.cellsH <= 0 || mf.vx.size() != n || mf.vy.size() != n)
			return out;

		std::vector<double> relSpeed(n, 0.0);
		for (size_t i = 0; i < n; ++i)
		{
			if (!mf.valid[i] || !std::isfinite(mf.vx[i]) || !std::isfinite(mf.vy[i]) ||
			    !std::isfinite(mf.camVx[i]) || !std::isfinite(mf.camVy[i]))
			{
				relSpeed[i] = kFastObjectCutoffPx * 2.0;
				continue;
			}
			const double dvx = mf.vx[i] - mf.camVx[i];
			const double dvy = mf.vy[i] - mf.camVy[i];
			relSpeed[i] = std::sqrt(dvx * dvx + dvy * dvy);
		}

		for (int cy = 0; cy < mf.cellsH; ++cy)
		{
			for (int cx = 0; cx < mf.cellsW; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy) * mf.cellsW + cx;
				if (!mf.valid[idx] || !std::isfinite(mf.vx[idx]) || !std::isfinite(mf.vy[idx]))
					continue;
				const RegionMotionClass c = mf.cls[idx];
				if (c == RegionMotionClass::Overlay)
				{
					// Hard drop on overlays/subtitles across ALL bands (prevents ghost letters).
					out.gateL[idx] = 0.0;
					out.gateMH[idx] = 0.0;
					continue;
				}
				out.gateL[idx] = 1.0;
				if (c == RegionMotionClass::DynamicTexture)
				{
					// Hard drop on chaotic textures (water/rain/foliage/fire) for M/H.
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
						if (mf.valid[nIdx] && mf.cls[nIdx] != RegionMotionClass::Overlay)
						{
							if (relSpeed[nIdx] > dilatedSpeed)
								dilatedSpeed = relSpeed[nIdx];
						}
					}
				}

				// Smooth fade via U25 RampMath::Smoothstep: 1.0 at <= 0.25 px, 0.0 at >= 2.0 px.
				out.gateMH[idx] = 1.0 - RampMath::Smoothstep(kStillObjectSpeedPx, kFastObjectCutoffPx, dilatedSpeed);
			}
		}
		return out;
	}

	// ---- Frequency-Split Focus Guard (Owner Focus Rule + Q4 Bokeh Study Knob) ----
	struct FocusCellInfo
	{
		double focusRatio = 1.0;    // E2 / (E2 + E3)
		bool isDefocused = false;
		bool focusChange = false;   // Focal-plane shift detected -> hard veto M/H
	};

	// Compute E2/(E2+E3) focus stand-in map and detect focal-plane shifts against warped references.
	inline std::vector<FocusCellInfo> EvaluateFocusGuardStandIn(
		const ThreeBandPlanes& curBands,
		const std::vector<ThreeBandPlanes>& refBands,
		const ActiveRect& active,
		int cellsW,
		int cellsH,
		int cellPx,
		int sourceBits,
		const std::vector<uint8_t>* explicitFocusChangeCells,
		StandInRegistry* standIns)
	{
		if (standIns != nullptr)
			standIns->Engage(StandInKind::FocusMapK32);

		const size_t numCells = static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH);
		std::vector<FocusCellInfo> info(numCells);
		const double depthScale = DepthScale(sourceBits);
		const double energyFloor = kFocusMinEnergy10Bit * depthScale * depthScale;
		const ActiveRect ar = active.ClampedTo(curBands.M.w, curBands.M.h);

		auto cellFocusRatio = [&](const ThreeBandPlanes& b, int cx, int cy, double& outTotalEnergy) -> double
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
				return 1.0;
			}
			const double invC = 1.0 / static_cast<double>(cnt);
			const double m2 = e2 * invC;
			const double m3 = e3 * invC;
			outTotalEnergy = m2 + m3;
			return outTotalEnergy > kEngineNumFloor ? (m2 / (outTotalEnergy + kEngineNumFloor)) : 1.0;
		};

		for (int cy = 0; cy < cellsH; ++cy)
		{
			for (int cx = 0; cx < cellsW; ++cx)
			{
				const size_t idx = static_cast<size_t>(cy) * cellsW + cx;
				double curEnergy = 0.0;
				const double curF = cellFocusRatio(curBands, cx, cy, curEnergy);
				info[idx].focusRatio = curF;

				if (curEnergy >= energyFloor && curF <= kDefocusRatioCeiling)
					info[idx].isDefocused = true;

				if (explicitFocusChangeCells != nullptr && idx < explicitFocusChangeCells->size() && (*explicitFocusChangeCells)[idx] != 0)
				{
					info[idx].focusChange = true;
				}
				else
				{
					// Detect focal-plane shift (rack focus): if any reference is meaningfully sharper than current cell
					// while current cell is defocused or low-ratio, hard-veto M/H so sharper detail is never imported.
					for (const auto& rb : refBands)
					{
						double refEnergy = 0.0;
						const double refF = cellFocusRatio(rb, cx, cy, refEnergy);
						if (refEnergy >= energyFloor && (refF - curF) > kFocusShiftDelta)
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

		// Coding step prior for A2 & transform-unknown bound (from P16 stand-in or upstream P16).
		double estimatedStepDelta = 4.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.5;
		bool transformIdentified8x8 = false;  // True only when codec transform (e.g. AVC 8x8) is identified

		bool isCutOrSpanInvalid = false;      // Resets recurrent carry state immediately
		const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
	};

	struct FrameProcessResult
	{
		TdV2Plane outputY;                    // Final output Y_out (bit-identical to receivedY when Off/Shadow)
		TdV2Plane outputClean;                // Reconstructed grain-free clean + delta (for diagnostics / carry)
		std::vector<TdMapsView::Block> tdMaps;// U19 per-16x16-cell published td.maps
		int cellsW = 0;
		int cellsH = 0;
		size_t activeRefCount = 0;            // Number of references fused (peaks + carry)
		size_t guardViolationCells = 0;       // Layer-4 TdGuardPort interventions
		size_t focusVetoCells = 0;            // Focus-change cells vetoed on M/H
		size_t nonFiniteCells = 0;            // Cells closed to zero on non-finite input
		bool carryUsed = false;
		bool carryReset = false;
	};

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

		FrameProcessResult ProcessFrame(const FrameProcessInput& in, StandInRegistry* standIns = nullptr)
		{
			FrameProcessResult res;
			const int w = in.receivedY.w;
			const int h = in.receivedY.h;
			res.outputY = in.receivedY; // Default passthrough: bit-exact when Mode::Off or Mode::Shadow!
			if (w <= 0 || h <= 0 || in.receivedY.Empty())
				return res;

			const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
			res.cellsW = (w + cellPx - 1) / cellPx;
			res.cellsH = (h + cellPx - 1) / cellPx;
			const size_t numCells = static_cast<size_t>(res.cellsW) * static_cast<size_t>(res.cellsH);
			res.tdMaps.resize(numCells);

			// Mode::Off -> zero work, byte-identical passthrough.
			if (!cfg_.IsEnabled())
				return res;

			// Cut / invalid span resets recurrent carry cleanly (zero ghosting across cuts).
			if (in.isCutOrSpanInvalid)
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

			auto tryAddRef = [&](const TdV2Plane* refClean, const CellMotionField* mf, bool exactCopy, bool isCarry)
			{
				if (refClean == nullptr || refClean->Empty() || refClean->w != w || refClean->h != h)
					return;
				CellMotionField fallbackMf;
				const CellMotionField* useMf = mf;
				if (useMf == nullptr || useMf->cellsW != res.cellsW || useMf->cellsH != res.cellsH)
				{
					fallbackMf = CellMotionField::Uniform(res.cellsW, res.cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					useMf = &fallbackMf;
				}
				WarpedRef wr;
				wr.isExactCopy = exactCopy;
				wr.isCarry = isCarry;
				WarpCleanReference(*refClean, active, useMf->vx, useMf->vy, res.cellsW, res.cellsH, cellPx, wr.clean, wr.validPx);
				wr.bands = DecomposeActiveBands(wr.clean, active, standIns);
				wr.motionGate = EvaluateCameraRelativeMotionGate(*useMf);
				refs.push_back(std::move(wr));
			};

			if (!in.isCutOrSpanInvalid)
			{
				if (in.prevPeak.cleanPlane != nullptr)
					tryAddRef(in.prevPeak.cleanPlane, in.prevPeak.motion, in.prevPeak.isExactCopyOfCurrent, false);
				if (in.nextPeak.cleanPlane != nullptr)
					tryAddRef(in.nextPeak.cleanPlane, in.nextPeak.motion, in.nextPeak.isExactCopyOfCurrent, false);
				if (cfg_.enableCarryForward && hasCarry_ && carryClean_.w == w && carryClean_.h == h)
				{
					tryAddRef(&carryClean_, in.hasCarryMotion ? &in.carryMotion : nullptr, false, true);
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

			// Evaluate frequency-split focus guard across all aligned references.
			std::vector<ThreeBandPlanes> refBandList;
			refBandList.reserve(refs.size());
			for (const auto& wr : refs)
				refBandList.push_back(wr.bands);
			const auto focusCells = EvaluateFocusGuardStandIn(
				curBands, refBandList, active, res.cellsW, res.cellsH, cellPx, cfg_.sourceBits,
				in.explicitFocusChangeCells, standIns);

			// Compute A2 rounding-cell stats from P16 step prior.
			const RoundingCellStats a2Stats = ComputeRoundingCellStats(
				in.estimatedStepDelta, in.deadZonePhi, in.zeroCoeffFraction);
			const double bandShares[3] = { kBandGrainShareLow, kBandGrainShareMid, kBandGrainShareFine };
			// For A2 pixel-domain variance split across L/M/H, distribute total rounding variance cleanly:
			// low band holds quantization step DC/ramp rounding (1/3), mid+fine split remaining 2/3 by filter energy shares.
			const double a2BandFrac[3] = { 1.0 / 3.0, (2.0 / 3.0) * kBandGrainShareMid, (2.0 / 3.0) * kBandGrainShareFine };

			const int K = static_cast<int>(refs.size());
			// Per-band, per-ref block weights and diagonal variance C_kk (for 3-sigma Huber pixel taper).
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
						const int gcx = MapCellToGrainGrid(cx, cellPx, cfg_.grainCellPx);
						const int gcy = MapCellToGrainGrid(cy, cellPx, cfg_.grainCellPx);
						const int gW = (w + cfg_.grainCellPx - 1) / cfg_.grainCellPx;
						const size_t gIdx = static_cast<size_t>(gcy) * gW + gcx;
						if (gIdx < in.retainedGrainVar.size() && std::isfinite(in.retainedGrainVar[gIdx]))
							cellRetainedGrainVar = (std::max)(0.0, in.retainedGrainVar[gIdx]);
					}

					double cellWeightSumAll = 0.0;
					double cellResEnergy = 0.0;
					double cellRealDetail = 0.0;
					unsigned activeBandBits = 0;

					for (int b = 0; b < 3; ++b)
					{
						const TdV2Plane& curB = bandPlaneOf(curBands, b);
						MomentAccumulatorFp32 acc(K);

						const int py0 = (std::max)(active.y0, (cy - poolR) * cellPx);
						const int py1 = (std::min)(active.y1, (cy + poolR + 1) * cellPx);
						const int px0 = (std::max)(active.x0, (cx - poolR) * cellPx);
						const int px1 = (std::min)(active.x1, (cx + poolR + 1) * cellPx);

						for (int y = py0; y < py1; ++y)
						{
							for (int x = px0; x < px1; ++x)
							{
								const size_t pIdx = static_cast<size_t>(y) * w + x;
								bool allValid = true;
								float diffs[kMaxBlendRefs] = {};
								const double curV = curB.at(x, y);
								for (int k = 0; k < K; ++k)
								{
									if (refs[k].validPx[pIdx] == 0u)
									{
										allValid = false;
										break;
									}
									diffs[k] = static_cast<float>(curV - bandPlaneOf(refs[k].bands, b).at(x, y));
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
							cellDiagC[b][k][cIdx] = C[k][k];

						double cVec[kMaxBlendRefs] = {};
						for (int k = 0; k < K; ++k)
						{
							// A2 rounding-cell prior for ref k on band b.
							const double cA2 = EstimateC_A2(a2Stats, refs[k].isExactCopy) * a2BandFrac[b];

							// A1 grain-corrected Three-Cornered Hat using the best available partner reference l != k.
							double cA1 = cA2; // Single-reference fallback when no second instrument exists.
							if (K >= 2)
							{
								const int partner = (k == 0) ? 1 : 0;
								cA1 = EstimateC_A1(C[k][partner], C[k][k], C[partner][partner],
								                   cellRetainedGrainVar, bandShares[b]);
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

						// Apply Focus Rule (plan s1.4 & Q4) and Camera-Relative Motion Gate (plan s1.3).
						double focusGate = 1.0;
						if (b == 0)
						{
							// L band: untouched in bokeh unless Q4 LowBandCellOnly is enabled.
							if (fInfo.isDefocused && !fInfo.focusChange && cfg_.bokehMode == BokehMode::Untouched)
								focusGate = 0.0;
							if (fInfo.focusChange && cfg_.bokehMode == BokehMode::Untouched)
								focusGate = 0.0;
						}
						else
						{
							// M and H bands: hard-veto on focal-plane shift OR defocused bokeh!
							if (fInfo.focusChange || fInfo.isDefocused)
								focusGate = 0.0;
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
						if (b == 2)
							cellRealDetail = C[0][0];
						cellResEnergy += C[0][0];
					}

					// Populate U19 TdMapsView::Block (all 12 named fields).
					TdMapsView::Block& mBlock = res.tdMaps[cIdx];
					mBlock.f[TdMapsView::kValidity] = (activeBandBits != 0u) ? 1.0f : 0.0f;
					mBlock.f[TdMapsView::kTrailConfidence0] = static_cast<float>(RampMath::Clamp01(cellWeightSumAll / 3.0));
					mBlock.f[TdMapsView::kTrailConfidence1] = static_cast<float>(K >= 2 ? RampMath::Clamp01(cellW[1][0][cIdx] + cellW[1][1][cIdx]) : cellW[1][0][cIdx]);
					mBlock.f[TdMapsView::kResidual] = static_cast<float>(cellResEnergy / 3.0);
					mBlock.f[TdMapsView::kRealDetail] = static_cast<float>(std::sqrt((std::max)(0.0, cellRealDetail)));
					mBlock.f[TdMapsView::kStability] = static_cast<float>(RampMath::Clamp01(cellWeightSumAll / 3.0));
					mBlock.f[TdMapsView::kOwnGrain] = static_cast<float>(std::sqrt((std::max)(0.0, cellRetainedGrainVar)));
					mBlock.f[TdMapsView::kCodingNoise] = static_cast<float>(std::sqrt((std::max)(0.0, a2Stats.totalVar)));
					mBlock.f[TdMapsView::kLightGain] = 1.0f;
					mBlock.f[TdMapsView::kLightOffset] = 0.0f;
					mBlock.f[TdMapsView::kDrift] = 0.0f;
					mBlock.f[TdMapsView::kBandMask] = static_cast<float>(activeBandBits);
				}
			}

			// Per-pixel spatial-only interpolation, 3-sigma Huber taper, STRICT/CELL or pixel-domain bound, and clean delta.
			TdV2Plane rawDeltaPlane(w, h, 0.0);
			TdV2Plane deltaPlane(w, h, 0.0);
			for (int y = active.y0; y < active.y1; ++y)
			{
				const int cy = (std::min)(res.cellsH - 1, y / cellPx);
				for (int x = active.x0; x < active.x1; ++x)
				{
					const int cx = (std::min)(res.cellsW - 1, x / cellPx);
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					const size_t pIdx = static_cast<size_t>(y) * w + x;

					double totalDelta = 0.0;
					for (int b = 0; b < 3; ++b)
					{
						const double curV = bandPlaneOf(curBands, b).at(x, y);
						double bandDelta = 0.0;
						for (int k = 0; k < K; ++k)
						{
							if (refs[k].validPx[pIdx] == 0u)
								continue;
							// Spatial-only interpolation (ZERO temporal weight ramps).
							const double wSpat = SpatialBilinearWeight(
								cellW[b][k].data(), res.cellsW, res.cellsH,
								static_cast<double>(x), static_cast<double>(y), cellPx);
							if (wSpat <= 0.0)
								continue;
							const double diff = curV - bandPlaneOf(refs[k].bands, b).at(x, y);
							// 3-sigma Huber pixel taper protects sharp corners & sub-block disocclusions.
							const double taper = HuberPixelTaper(diff, cellDiagC[b][k][cIdx]);
							bandDelta -= (wSpat * taper) * diff;
						}
						// Apply per-band quantization-cell bound when codec transform is unknown.
						if (!in.transformIdentified8x8)
						{
							const double bandVar = a2Stats.valid ? (a2Stats.totalVar * a2BandFrac[b]) : 0.0;
							if (std::fabs(bandDelta) > 0.0)
								bandDelta = ApplyTransformUnknownBound(bandDelta, bandVar, standIns);
						}
						totalDelta += bandDelta;
					}
					rawDeltaPlane.at(x, y) = totalDelta;
					deltaPlane.at(x, y) = totalDelta;
				}
			}

			// Optional 8x8 DCT transform-domain STRICT/CELL projection when transform is identified (AVC/MPEG).
			if (in.transformIdentified8x8 && in.estimatedStepDelta > 0.0)
			{
				const int iters = (std::max)(1, (std::min)(2, cfg_.cellProjIterations));
				for (int it = 0; it < iters; ++it)
				{
					for (int by = active.y0; by + 8 <= active.y1; by += 8)
					{
						for (int bx = active.x0; bx + 8 <= active.x1; bx += 8)
						{
							double sentRes[64] = {}, blendRes[64] = {}, projRes[64] = {};
							for (int j = 0; j < 8; ++j)
							{
								for (int i = 0; i < 8; ++i)
								{
									const double c = curClean.at(bx + i, by + j);
									sentRes[j * 8 + i] = c;
									blendRes[j * 8 + i] = c + deltaPlane.at(bx + i, by + j);
								}
							}
							ProjectResidual8x8Dct(sentRes, blendRes, in.estimatedStepDelta, in.deadZonePhi, cfg_.residualMode, projRes);
							for (int j = 0; j < 8; ++j)
								for (int i = 0; i < 8; ++i)
									deltaPlane.at(bx + i, by + j) = projRes[j * 8 + i] - curClean.at(bx + i, by + j);
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
			// Checks per-cell fine-band variance on recomposed output vs received input when own grain is present;
			// if any cell's fine variance dropped > 2% RMS, adds back the exact missing fraction on the fine band.
			const ThreeBandPlanes inFullBands = DecomposeActiveBands(in.receivedY, active, nullptr);
			const ThreeBandPlanes outFullBands = DecomposeActiveBands(candOut, active, nullptr);

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

					// Only guard cells where own grain is actually present (so pure noiseless coding repair isn't rolled back).
					double ownGrainEnergy = 0.0;
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
							inFineE += inf * inf;
							outFineE += outf * outf;
							++cnt;
						}
					}
					if (cnt == 0) continue;
					const double invC = 1.0 / static_cast<double>(cnt);
					if (ownGrainEnergy * invC <= kEngineNumFloor)
						continue;

					const auto gDec = TdGuardPort::Evaluate(inFineE * invC, outFineE * invC, standIns);
					if (gDec.violated && gDec.addBackFraction > 0.0)
					{
						++res.guardViolationCells;
						for (int y = y0; y < y1; ++y)
						{
							for (int x = x0; x < x1; ++x)
							{
								const double addBack = gDec.addBackFraction * (inFullBands.H.at(x, y) - outFullBands.H.at(x, y));
								candOut.at(x, y) += addBack;
								candClean.at(x, y) += addBack;
							}
						}
					}
				}
			}

			// Update recurrent carry-forward buffer with the finalized grain-free estimate.
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
