// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Small Fast Objects & Trajectory-Type Study: pure POD types, derived physics/statistical
// constants, exact chi-squared utilities, compact mask helpers, and diagnostic formatting.
// Pure C++14/17, zero Windows headers, zero static/thread_local mutable state.
//
// Coordinate & unit conventions:
//  * CameraModel::Params homographies operate in picture-centred source px (see CameraMotion.h).
//  * BlobCandidate::cx/cy, FastObjectTrack::cx/cy, maskBox, and Capsule use top-left frame
//    source px ([0, srcW) x [0, srcH)).
//  * FastObjectTrack::vx/vy is the exact frame t -> t+1 pair displacement in source px.
//  * FastObjectTrack::velXPerSec/velYPerSec and ax/ay are physical velocity (source px/s) and
//    acceleration (source px/s^2) in frame t's stabilized plane using PairTiming::Seconds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../../../../core/AheadCeiling.h"
#include "../global_model/CameraMotion.h"

namespace FastObjects
{
	// -------------------------------------------------------------------------
	// Structural & Capacity Constants (aligned with VP-V shared products)
	// -------------------------------------------------------------------------
	constexpr int kCellSizeHalfPx = 4;                         // 4x4 half-luma tile = 8x8 source px
	constexpr uint32_t kMaxActiveCells = 256;                  // 256 x 8 B = 2048 B GPU->CPU staging cap
	constexpr uint32_t kMaxBlobs = 32;                         // 32 x 64 B = 2048 B blob cap
	constexpr uint32_t kMaxTracksDefault = 8;                  // Default active track cap
	constexpr uint32_t kMaxTracksHardCap = 32;                 // Fixed array capacity (zero heap alloc)
	constexpr int kMaskDim = 16;                               // 16x16 compact binary support mask (256 bits)
	constexpr int kWindowAheadMax = static_cast<int>(VpSkeleton::kAheadCeiling); // 6 frames ahead
	constexpr int kWindowBackMax = 6;                          // 6 frames back (13-frame ring)
	constexpr int kWindowRingCapacity = kWindowBackMax + kWindowAheadMax + 1;
	constexpr int kHotspotGridW = 64;                          // 16:9 coarse clutter-hotspot map
	constexpr int kHotspotGridH = 36;
	constexpr int kHotspotGridCells = kHotspotGridW * kHotspotGridH;
	constexpr uint32_t kDormantIdleFrames = static_cast<uint32_t>(VpSkeleton::kAheadCeiling); // 6 quiet frames

	// -------------------------------------------------------------------------
	// Physics & Statistical Constants (Rule 1: every constant derived)
	// -------------------------------------------------------------------------
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kSqrt2 = 1.41421356237309504880;
	constexpr double kInvSqrt12 = 0.28867513459481288225;      // Uniform quantization sigma = delta / sqrt(12)
	constexpr double kMadToSigma = 1.4826022185056020;         // 1 / Phi^-1(0.75), matches RobustStats::kMadToSigma
	constexpr double kSeedSigma = 4.0;                         // 4-sigma Bonferroni bound (~10^6 half-res px)
	constexpr double kGrowSigma = 3.0;                         // VP-V universal 3-sigma contract
	constexpr double kChi2Dof2_99 = 9.2103403719761836;        // Exact -2 * ln(0.01) for chi^2(2, 0.99)
	constexpr double kElongationRatioMin = 1.1547005383792515; // sqrt(4/3): exact L_est > r_est boundary
	constexpr double kMinBlobAreaHalfPx = 4.0;                 // 2x2 Nyquist sampling limit at half-res
	constexpr double kMaxMinorAxisFrac = 0.03;                 // 3% of active picture height (Plan 73 s2.2)
	constexpr double kMaxSpeedWidthFracPerSec = 2.0;           // 2.0 * W_pic px/s (Plan 73 s2.3)
	constexpr double kWaterProbThreshold = 0.5;                // CE-WATER exclusion threshold (DynamicTexture.h)
	constexpr double kCoastMaxSec = 0.200;                     // 200 ms occlusion coasting ceiling
	constexpr double kHotspotDecaySec = 2.0;                   // 2.0 s exponential decay for clutter hotspots
	constexpr double kManeuverAccelHeightFracPerSec2 = 1.0;    // 1.0 * H_pic px/s^2 broadcast gravity scale
	constexpr float kDefaultMinConfidence = 0.25f;             // Elementary 1/4 floor (> g_NIS(chi2(2,0.99))=0.165)

	// -------------------------------------------------------------------------
	// Exact Chi-Squared Statistical Helpers (Rule 1: zero hand-fitted tables)
	// -------------------------------------------------------------------------

	// Exact survival function S_3(x) = P(chi^2(3) > x) = erfc(sqrt(x/2)) + sqrt(2x/pi) * exp(-x/2).
	inline double Chi2Dof3Survival(double x)
	{
		if (x <= 0.0)
			return 1.0;
		const double halfX = 0.5 * x;
		return std::erfc(std::sqrt(halfX)) + std::sqrt((2.0 * x) / kPi) * std::exp(-halfX);
	}

	// Exact two-sided bivariate standard normal same-sign exceedance tail for correlation rho = 1/2:
	//   P_FA(k; rho = 1/2) = 2 * P(Z_1 > k, Z_2 > k | rho = 1/2)
	// Using the exact latent-factor decomposition Z_i = (U + V_i) / sqrt(2) with independent standard normals:
	//   P(Z_1 > k, Z_2 > k) = integral phi(u) * [0.5 * erfc((sqrt(2)*k - u) / sqrt(2))]^2 du.
	// Evaluated by 64-point deterministic midpoint quadrature over u in [-6, 12] (exact to < 1e-10 for k in [0, 8]).
	// Matches Spec sA.3: P_FA(4.0; 0.5) = 1.32e-5, P_FA(3.0; 0.5) = 4.61e-4.
	inline double BivariateSameSignTailRhoHalf(double k)
	{
		if (!(k > 0.0))
			return 0.5;
		constexpr int kSteps = 64;
		const double uMin = -6.0;
		const double uMax = std::fmax(12.0, kSqrt2 * k + 6.0);
		const double du = (uMax - uMin) / static_cast<double>(kSteps);
		const double invSqrt2Pi = 1.0 / std::sqrt(2.0 * kPi);
		const double invSqrt2 = 1.0 / kSqrt2;
		double integral = 0.0;
		for (int i = 0; i < kSteps; ++i)
		{
			const double u = uMin + (static_cast<double>(i) + 0.5) * du;
			const double phi = invSqrt2Pi * std::exp(-0.5 * u * u);
			const double tail1 = 0.5 * std::erfc((kSqrt2 * k - u) * invSqrt2);
			integral += phi * tail1 * tail1;
		}
		return 2.0 * integral * du;
	}

	// Exact chi^2(3) threshold whose upper tail matches the bivariate same-sign residual tail P_FA(k; rho = 0.5).
	inline double Chi2Dof3ThresholdForSigma(double k)
	{
		if (!(k > 0.0))
			return 0.0;
		const double target = BivariateSameSignTailRhoHalf(k);
		double x = k * k + 4.0;
		for (int iter = 0; iter < 16; ++iter)
		{
			const double s = Chi2Dof3Survival(x);
			const double pdf = std::sqrt(x / (2.0 * kPi)) * std::exp(-0.5 * x);
			if (!(pdf > 1e-300))
				break;
			const double dx = (s - target) / pdf;
			x += dx;
			if (x < 1e-9)
				x = 1e-9;
			if (std::fabs(dx) <= 1e-12 * std::fmax(1.0, x))
				break;
		}
		return x;
	}

	// Exact regularized upper incomplete gamma Q(dof/2, x/2) = P(chi^2(dof) > x) for positive integer dof.
	inline double Chi2SurvivalIntDof(int dof, double x)
	{
		if (dof <= 0 || x <= 0.0)
			return 1.0;
		const double halfX = 0.5 * x;
		if ((dof & 1) == 0)
		{
			// Even dof = 2m: Q(m, t) = exp(-t) * sum_{k=0}^{m-1} t^k / k!
			const int m = dof / 2;
			double term = 1.0;
			double sum = 1.0;
			for (int k = 1; k < m; ++k)
			{
				term *= halfX / static_cast<double>(k);
				sum += term;
			}
			return std::exp(-halfX) * sum;
		}
		// Odd dof = 2m + 1: Q(m + 1/2, t) = erfc(sqrt(t)) + exp(-t) * sqrt(t/pi) * sum_{k=0}^{m-1} t^k / (1/2)_{k+1}
		const int m = dof / 2;
		double sum = 0.0;
		if (m > 0)
		{
			double term = 2.0; // 1 / (1/2)
			sum = term;
			for (int k = 1; k < m; ++k)
			{
				term *= halfX / (static_cast<double>(k) + 0.5);
				sum += term;
			}
		}
		return std::erfc(std::sqrt(halfX)) + std::exp(-halfX) * std::sqrt(halfX / kPi) * sum;
	}

	// Exact chi^2(dof) PDF at x > 0 for positive integer dof.
	inline double Chi2PdfIntDof(int dof, double x)
	{
		if (dof <= 0 || !(x > 0.0))
			return 0.0;
		const double a = 0.5 * static_cast<double>(dof);
		const double logPdf = (a - 1.0) * std::log(x) - 0.5 * x - a * std::log(2.0) - std::lgamma(a);
		return std::exp(logPdf);
	}

	// Exact 99% critical value chi^2(dof, 0.99) (alpha = 0.01) for any integer dof >= 1.
	inline double Chi2Critical99(int dof)
	{
		if (dof <= 0)
			return 0.0;
		if (dof == 1)
			return 6.6348966010212145; // (Phi^-1(0.995))^2
		if (dof == 2)
			return kChi2Dof2_99;       // -2 * ln(0.01)
		// Wilson-Hilferty initial guess with z_0.99 = 2.3263478740408408, refined by Newton.
		constexpr double kZ99 = 2.3263478740408408;
		const double nu = static_cast<double>(dof);
		const double c = 1.0 - 2.0 / (9.0 * nu) + kZ99 * std::sqrt(2.0 / (9.0 * nu));
		double x = (c > 0.1) ? (nu * c * c * c) : (nu + 3.0 * std::sqrt(2.0 * nu));
		for (int iter = 0; iter < 12; ++iter)
		{
			const double s = Chi2SurvivalIntDof(dof, x);
			const double pdf = Chi2PdfIntDof(dof, x);
			if (!(pdf > 1e-300))
				break;
			const double dx = (s - 0.01) / pdf;
			x += dx;
			if (x < 1e-9)
				x = 1e-9;
			if (std::fabs(dx) <= 1e-11 * std::fmax(1.0, x))
				break;
		}
		return x;
	}

	// -------------------------------------------------------------------------
	// Core Enums & Bitmasks
	// -------------------------------------------------------------------------
	enum class TrajectoryType : uint8_t
	{
		Unknown = 0,
		LinearGlide = 1,
		Ballistic = 2,
		Impulse = 3,
		Clutter = 4
	};

	enum class ValidityState : uint8_t
	{
		Invalid = 0,
		Uncertain = 1,
		Coasting = 2,
		Valid = 3
	};

	enum class SchedulerMode : uint8_t
	{
		Off = 0,
		Dormant = 1,
		ActiveT1 = 2,
		ActiveT2 = 3,
		ShedBudget = 4
	};

	namespace AbstainReason
	{
		constexpr uint32_t None = 0u;
		constexpr uint32_t Disabled = 1u << 0;
		constexpr uint32_t OriginalPathway = 1u << 1;
		constexpr uint32_t NoUsableCamera = 1u << 2;
		constexpr uint32_t SceneDiscontinuity = 1u << 3;
		constexpr uint32_t ClutterOverflow = 1u << 4;
		constexpr uint32_t CausalNoLookAhead = 1u << 5;
		constexpr uint32_t BudgetShed = 1u << 6;
	}

	namespace TrackFlags
	{
		constexpr uint32_t None = 0u;
		constexpr uint32_t kCentroidVector = 1u << 0;    // Tiny/textureless fallback from ZNCC to trajectory centroid
		constexpr uint32_t kImpulsePair = 1u << 1;       // Pair (t, t+1) brackets an Impulse vertex
		constexpr uint32_t kBlurStretched = 1u << 2;     // L_est > r_est (motion-blur elongated)
		constexpr uint32_t kDepthPerspective = 1u << 3;  // 2.5D apparent-radius perspective model selected
		constexpr uint32_t kCausalMode = 1u << 4;        // Tracked without future look-ahead
		constexpr uint32_t kChromaAssisted = 1u << 5;    // Chroma residual contributed to detection/association
		constexpr uint32_t kCrossingAmbiguity = 1u << 6; // Gate overlap < 2 sigma separation (forced Uncertain)
		constexpr uint32_t kSessionPriorMatch = 1u << 7; // Softly prioritized by session ball appearance prior
		constexpr uint32_t kIsoLumaChroma = 1u << 8;     // Isoluminant chroma-driven track (skips luma-only ZNCC)
	}

	// -------------------------------------------------------------------------
	// Compact 16x16 Support Mask & Swept Capsule Helpers
	// -------------------------------------------------------------------------
	struct Capsule
	{
		float x0 = 0.0f;
		float y0 = 0.0f;
		float x1 = 0.0f;
		float y1 = 0.0f;
		float radiusPx = 0.0f;
	};

	inline void MaskClear(uint64_t bits[4])
	{
		bits[0] = bits[1] = bits[2] = bits[3] = 0ull;
	}

	inline void MaskSetBit(uint64_t bits[4], int mx, int my)
	{
		if (mx < 0 || mx >= kMaskDim || my < 0 || my >= kMaskDim)
			return;
		const int idx = my * kMaskDim + mx;
		bits[idx >> 6] |= (1ull << (idx & 63));
	}

	inline bool MaskGetBit(const uint64_t bits[4], int mx, int my)
	{
		if (mx < 0 || mx >= kMaskDim || my < 0 || my >= kMaskDim)
			return false;
		const int idx = my * kMaskDim + mx;
		return (bits[idx >> 6] & (1ull << (idx & 63))) != 0ull;
	}

	inline bool MaskContainsPoint(const CameraModel::Rect& box, const uint64_t bits[4], bool hasMaskBits, float x, float y)
	{
		if (box.Empty())
			return false;
		if (x < static_cast<float>(box.left) || x >= static_cast<float>(box.right) ||
			y < static_cast<float>(box.top) || y >= static_cast<float>(box.bottom))
			return false;
		if (!hasMaskBits)
			return true;
		const float fx = (x - static_cast<float>(box.left)) / static_cast<float>(box.Width());
		const float fy = (y - static_cast<float>(box.top)) / static_cast<float>(box.Height());
		int mx = static_cast<int>(std::floor(fx * static_cast<float>(kMaskDim)));
		int my = static_cast<int>(std::floor(fy * static_cast<float>(kMaskDim)));
		mx = std::min(std::max(mx, 0), kMaskDim - 1);
		my = std::min(std::max(my, 0), kMaskDim - 1);
		return MaskGetBit(bits, mx, my);
	}

	inline float DistanceSqToSegment(float px, float py, float x0, float y0, float x1, float y1)
	{
		const float vx = x1 - x0;
		const float vy = y1 - y0;
		const float wx = px - x0;
		const float wy = py - y0;
		const float len2 = vx * vx + vy * vy;
		float t = 0.0f;
		if (len2 > 1e-12f)
		{
			t = (wx * vx + wy * vy) / len2;
			if (t < 0.0f) t = 0.0f;
			else if (t > 1.0f) t = 1.0f;
		}
		const float dx = px - (x0 + t * vx);
		const float dy = py - (y0 + t * vy);
		return dx * dx + dy * dy;
	}

	inline bool CapsuleContains(const Capsule& cap, float x, float y, float extraRadiusPx = 0.0f)
	{
		const float r = cap.radiusPx + extraRadiusPx;
		if (!(r > 0.0f))
			return false;
		return DistanceSqToSegment(x, y, cap.x0, cap.y0, cap.x1, cap.y1) <= r * r;
	}

	// -------------------------------------------------------------------------
	// Self-Contained 3x3 Homography Helpers on CameraModel::Params
	// (Matches PixelResidualReference.cpp pattern: zero link dependency on GlobalModelGeometry.cpp)
	// -------------------------------------------------------------------------
	inline CameraModel::Params IdentityHomography()
	{
		CameraModel::Params p;
		p.type = CameraModel::ModelType::Homography;
		return p;
	}

	inline CameraModel::Params InvertHomography(const CameraModel::Params& p, bool* ok = nullptr)
	{
		const double* m = p.h;
		const double c00 = m[4] * m[8] - m[5] * m[7];
		const double c01 = m[5] * m[6] - m[3] * m[8];
		const double c02 = m[3] * m[7] - m[4] * m[6];
		const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
		double mag = 0.0;
		for (int i = 0; i < 9; ++i)
		{
			if (!CameraModel::IsFiniteBits(m[i]))
			{
				if (ok) *ok = false;
				return IdentityHomography();
			}
			mag = std::fmax(mag, std::fabs(m[i]));
		}
		if (!CameraModel::IsFiniteBits(det) || !(mag > 0.0) || std::fabs(det) <= 1e-12 * mag * mag * mag)
		{
			if (ok) *ok = false;
			return IdentityHomography();
		}
		const double id = 1.0 / det;
		CameraModel::Params r;
		r.type = p.type;
		r.h[0] = c00 * id;
		r.h[1] = (m[2] * m[7] - m[1] * m[8]) * id;
		r.h[2] = (m[1] * m[5] - m[2] * m[4]) * id;
		r.h[3] = c01 * id;
		r.h[4] = (m[0] * m[8] - m[2] * m[6]) * id;
		r.h[5] = (m[2] * m[3] - m[0] * m[5]) * id;
		r.h[6] = c02 * id;
		r.h[7] = (m[1] * m[6] - m[0] * m[7]) * id;
		r.h[8] = (m[0] * m[4] - m[1] * m[3]) * id;
		if (std::fabs(r.h[8]) > 1e-12)
		{
			const double s = 1.0 / r.h[8];
			for (int i = 0; i < 9; ++i)
				r.h[i] *= s;
		}
		for (int i = 0; i < 9; ++i)
		{
			if (!CameraModel::IsFiniteBits(r.h[i]))
			{
				if (ok) *ok = false;
				return IdentityHomography();
			}
		}
		if (ok) *ok = true;
		return r;
	}

	// Compose a o b (apply b first, then a).
	inline CameraModel::Params ComposeHomography(const CameraModel::Params& a, const CameraModel::Params& b, bool* ok = nullptr)
	{
		CameraModel::Params r;
		r.type = (static_cast<uint8_t>(a.type) >= static_cast<uint8_t>(b.type)) ? a.type : b.type;
		for (int row = 0; row < 3; ++row)
		{
			for (int col = 0; col < 3; ++col)
			{
				r.h[row * 3 + col] =
					a.h[row * 3 + 0] * b.h[0 * 3 + col] +
					a.h[row * 3 + 1] * b.h[1 * 3 + col] +
					a.h[row * 3 + 2] * b.h[2 * 3 + col];
			}
		}
		if (!CameraModel::IsFiniteBits(r.h[8]) || !(std::fabs(r.h[8]) > 1e-12))
		{
			if (ok) *ok = false;
			return r;
		}
		const double s = 1.0 / r.h[8];
		for (int i = 0; i < 9; ++i)
		{
			r.h[i] *= s;
			if (!CameraModel::IsFiniteBits(r.h[i]))
			{
				if (ok) *ok = false;
				return r;
			}
		}
		if (ok) *ok = true;
		return r;
	}

	// -------------------------------------------------------------------------
	// Data Products: BlobCandidate, FastObjectTrack, Diagnostics, TrackSet
	// -------------------------------------------------------------------------
	struct BlobCandidate
	{
		float cx = 0.0f;               // Sub-pixel centroid X (source px, top-left origin)
		float cy = 0.0f;               // Sub-pixel centroid Y (source px, top-left origin)
		float areaHalfPx = 0.0f;       // Component area in half-luma px
		float radiusPx = 0.0f;         // Deconvolved minor-axis radius r_est = 2*sqrt(lambda_2) (source px)
		float streakLenPx = 0.0f;      // Deconvolved streak length L_est = sqrt(12*max(0, l1-l2)) (source px)
		float streakAngleRad = 0.0f;   // Principal axis orientation in [-pi/2, pi/2]
		float flux = 0.0f;             // Conserved signed luma contrast flux sum(R_signed)
		int8_t polarity = 0;           // +1 bright-on-dark, -1 dark-on-bright, 0 isoluminant/unknown
		float peakSnr = 0.0f;          // Peak |z| inside blob
		float intSnr = 0.0f;           // Integrated SNR (luma or chroma-combined)
		float lumaIntSnr = 0.0f;       // Luma-only integrated SNR (for polarity reliability check)
		float cbContrast = 0.0f;       // Mean signed 3-frame Cb residual inside blob
		float crContrast = 0.0f;       // Mean signed 3-frame Cr residual inside blob
		float chromaFlux = 0.0f;       // Conserved chroma contrast flux sqrt(sumCb^2 + sumCr^2)
		float chromaSigma = 0.0f;      // Local chroma noise RMS inside blob
		float meanBgLuma = 0.0f;       // Mean background luma around blob (polarity-crossing check)
		bool hasChroma = false;
		bool overlapsOverlay = false;  // Touched P8 Label::Overlay
		bool overlapsWater = false;    // Touched CE-WATER waterProb >= 0.5
		bool nearLargeObject = false;  // Within 2*r + blockSize of a large P8 Label::Object (active strike check)
		CameraModel::Rect maskBox;     // Bounding box in source px (right/bottom exclusive)
		uint64_t maskBits[4] = { 0ull, 0ull, 0ull, 0ull };
		bool hasMaskBits = false;
	};

	struct FastObjectTrack
	{
		uint32_t id = 0;
		TrajectoryType type = TrajectoryType::Unknown;
		ValidityState state = ValidityState::Invalid;

		float cx = 0.0f;               // Position at frame t (source px)
		float cy = 0.0f;
		float vx = 0.0f;               // Exact frame t -> t+1 displacement (source px)
		float vy = 0.0f;
		float velXPerSec = 0.0f;       // Instantaneous velocity at frame t (source px/s)
		float velYPerSec = 0.0f;
		float ax = 0.0f;               // Physical acceleration (source px/s^2)
		float ay = 0.0f;
		float pairSeconds = 0.0f;      // Inter-frame duration dt for pair (t, t+1)

		bool hasImpulse = false;
		float impulseTau = 0.0f;       // Normalized phase in (0, 1) within pair [t, t+1]
		float impulseX = 0.0f;         // Bounce/strike vertex X (source px)
		float impulseY = 0.0f;         // Bounce/strike vertex Y (source px)

		float radiusPx = 0.0f;         // Current apparent radius r_est (source px)
		float radiusRate = 0.0f;       // dr/dt (source px/s) from 2.5D perspective fit
		float streakLenPx = 0.0f;      // Deconvolved motion-blur length L_est (source px)
		float streakAngleRad = 0.0f;   // Principal blur axis orientation (rad)
		float shutterEst = 0.0f;       // Online shutter fraction estimate tau_s in [0, 1]
		float blurAlignFactor = 1.0f;  // Motion-blur tangent alignment factor g_blur in (0, 1]
		float confidence = 0.0f;       // Derived confidence in [0, 1]
		float nis = 0.0f;              // Latest Normalized Innovation Squared (diagnostic)
		float bicMargin = 0.0f;        // Runner-up BIC minus winning BIC (diagnostic)
		uint32_t ageFrames = 0;        // Observed detections in window
		uint32_t coastFrames = 0;      // Consecutive coasted frames
		uint32_t flags = TrackFlags::None;

		CameraModel::Rect maskBox;
		uint64_t maskBits[4] = { 0ull, 0ull, 0ull, 0ull };
		bool hasMaskBits = false;
		Capsule disocclusionCapsule;
	};

	struct Diagnostics
	{
		uint32_t activeCells = 0;
		uint32_t blobCount = 0;
		uint32_t droppedBlobs = 0;
		uint32_t validTracks = 0;
		uint32_t uncertainTracks = 0;
		uint32_t coastingTracks = 0;
		uint32_t clutterRejected = 0;
		uint32_t ballisticTracks = 0;
		uint32_t linearGlideTracks = 0;
		uint32_t impulseTracks = 0;
		uint32_t hotspotSuppressedBlobs = 0;
		uint32_t centroidFallbackTracks = 0;
		uint32_t clutterOverflowCount = 0;
		uint32_t quietFrames = 0;
		bool clutterOverflow = false;
		bool causalMode = false;
		bool chromaUsed = false;
		bool scanSkippedDormant = false;
		float noiseSigmaEst = 0.0f;
	};

	struct FastObjectTrackSet
	{
		FastObjectTrack tracks[kMaxTracksHardCap];
		uint32_t count = 0;
		SchedulerMode mode = SchedulerMode::Off;
		uint32_t abstainReason = AbstainReason::Disabled;
		Diagnostics diag;

		void Clear(SchedulerMode newMode, uint32_t reason)
		{
			count = 0;
			mode = newMode;
			abstainReason = reason;
			diag = Diagnostics{};
		}

		// Human-readable status line for Ctrl+I / Ctrl+Alt+D / diagnostics (Rule 4).
		std::string FormatStatus() const
		{
			if ((abstainReason & AbstainReason::OriginalPathway) != 0u)
				return "fast_objects: off (original pathway)";
			if ((abstainReason & AbstainReason::Disabled) != 0u)
				return "fast_objects: off";
			if ((abstainReason & AbstainReason::BudgetShed) != 0u || mode == SchedulerMode::ShedBudget)
				return "fast_objects: off (budget)";
			if ((abstainReason & AbstainReason::ClutterOverflow) != 0u || diag.clutterOverflow)
			{
				char buf[96];
				std::snprintf(buf, sizeof(buf), "fast_objects: clutter overflow (%u cells)", diag.activeCells);
				return std::string(buf);
			}
			if ((abstainReason & AbstainReason::SceneDiscontinuity) != 0u)
				return "fast_objects: paused (scene cut/flash)";
			if ((abstainReason & AbstainReason::NoUsableCamera) != 0u)
				return "fast_objects: paused (camera model unusable)";
			if (mode == SchedulerMode::Off)
				return "fast_objects: off";
			if (mode == SchedulerMode::Dormant)
			{
				char buf[96];
				std::snprintf(buf, sizeof(buf), "fast_objects: dormant (quiet %uf%s)",
					diag.quietFrames, diag.causalMode ? ", causal" : "");
				return std::string(buf);
			}
			const char* tier = (mode == SchedulerMode::ActiveT2) ? "T2" : "T1";
			char buf[224];
			std::snprintf(buf, sizeof(buf),
				"fast_objects: %u tracks (%u valid, %u ballistic / %u glide / %u impulse), tier %s%s",
				count, diag.validTracks, diag.ballisticTracks, diag.linearGlideTracks, diag.impulseTracks,
				tier, diag.causalMode ? ", causal mode (no look-ahead)" : "");
			return std::string(buf);
		}
	};

	// -------------------------------------------------------------------------
	// Configuration (strictly default OFF; all defaults derived from physics)
	// -------------------------------------------------------------------------
	struct Config
	{
		bool enabled = false;                                      // STRICTLY DEFAULT OFF
		bool originalPathway = false;                              // Original == Bill invariant
		bool enableChromaResidual = true;                          // Half-res Cb/Cr 3-DOF chi^2 residual
		bool enableDormantScheduler = true;                        // Idle cadence drop after N_idle frames
		bool enableHotspotAutoMask = true;                         // 64x36 spatial clutter-hotspot auto-mask
		bool enableSessionPrior = true;                            // Soft session ball appearance prior
		bool enablePerspectiveDepth = true;                        // 2.5D apparent-radius perspective check
		bool enableMaskedRefine = true;                            // Stage B.5 masked pyramid ZNCC

		uint32_t maxTracks = kMaxTracksDefault;                    // Clamped to [1, kMaxTracksHardCap]
		uint32_t maxActiveCells = kMaxActiveCells;                 // Clutter overflow cap (256)
		uint32_t dormantIdleFrames = kDormantIdleFrames;           // 6 quiet frames

		double seedSigma = kSeedSigma;                             // 4.0 sigma
		double growSigma = kGrowSigma;                             // 3.0 sigma
		double lumaFullScale = 1.0;                                // 1.0 for normalized [0,1], 255.0 for 8-bit float
		int lumaBitDepth = 10;                                     // Bit depth for quantization step delta_code
		double quantizationStepOverride = 0.0;                     // > 0 overrides derived delta_code

		double minBlobAreaHalfPx = kMinBlobAreaHalfPx;             // 4 half-res px (2x2 Nyquist)
		double maxMinorAxisFrac = kMaxMinorAxisFrac;               // 0.03 * H_pic
		double maxSpeedWidthFracPerSec = kMaxSpeedWidthFracPerSec; // 2.0 * W_pic px/s
		double coastMaxSec = kCoastMaxSec;                         // 0.200 s
		double hotspotDecaySec = kHotspotDecaySec;                 // 2.0 s
		float minConfidence = kDefaultMinConfidence;               // 0.25

		double QuantizationSigma() const
		{
			double delta = quantizationStepOverride;
			if (!(delta > 0.0))
			{
				const int bits = (lumaBitDepth >= 8 && lumaBitDepth <= 16) ? lumaBitDepth : 10;
				const double levels = static_cast<double>((1u << bits) - 1u);
				delta = (lumaFullScale > 0.0 ? lumaFullScale : 1.0) / levels;
			}
			return delta * kInvSqrt12;
		}
	};
}
