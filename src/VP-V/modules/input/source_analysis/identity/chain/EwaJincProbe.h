// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// EwaJincProbe (SOTA #5): deterministic 1-D analytical kernel weight derivatives (for 1-step Gauss-Newton
// sub-sample phase refinement) and exact 2-D radial EWA-Lanczos (Jinc3) small-patch descale witness.
//
// Part 1: Analytical 1-D Kernel Derivatives
//   - SincDerivative(x), KernelDerivative(k, x), NaturalSplineWeightDerivatives(r, t, dw),
//     and TapWeightDerivatives(kernel, t, w, dw) compute exact analytical derivatives wrt fractional
//     latent position t (equivalently wrt sub-sample phase, since p = (i + 0.5)*n/N - 0.5 + phase).
//
// Part 2: Exact Bessel J_1, Radial Jinc, and 2-D Radial EWA-Jinc3 Patch Witness
//   - BesselJ1(x) uses Miller's backward recurrence normalized by the exact Neumann identity
//     J_0(x) + 2 * sum_{m>=1} J_{2m}(x) = 1 (zero fitted polynomial tables), with exact Hankel
//     asymptotic guard for |x| > 1000 so loop work stays bounded at full IEEE-754 precision.
//   - EwaJinc3Weight(r) = Jinc(r) * Jinc(r * kJincZero1 / kJincZero3) for |r| < kJincZero3 (exact
//     mpv / libplacebo ewa_lanczos a=3 radial filter).
//   - EvaluatePatch solves 2-D least squares min_z ||y - U z||^2 on a small 2-D patch using a sparse
//     CSR representation of U and dense Cholesky on (U^T U + ridge * maxDiag * I) across EWA-Jinc3,
//     2-D separable Lanczos3 (Kernel 5), and 2-D separable Bicubic 0/0.5 (Kernel 1) on the identical
//     inner box [innerMargin .. patchW - innerMargin) x [innerMargin .. patchH - innerMargin).
//   - Exact inner-box degrees of freedom dof = mean_{p in I}(1 - ||L^{-1} u_p||^2) eliminates
//     perimeter-halo bias between radial and separable kernels on small 2-D patches.
//
// Rule 1: Every constant is derived from exact Bessel/spline math, existing Chain contract constants,
//         or documented numerical singularity guards.
// Rule 2: Zero static/thread_local mutable caches; all buffers live in caller-owned EwaJincWorkspace.
// Rule 4: Zero silent fallbacks; every disabled/abstained/degraded outcome sets explicit verdict & reason.
// Default-OFF contract: EwaJincSettings::enabled defaults to false.
// Pure standard-library C++14, header-only, deterministic, no Windows headers, no clocks, no threads.

#include "KernelBank.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#pragma fp_contract(off)

namespace SourceIdentity
{
	namespace Chain
	{
		// Exact zeros of Jinc(r) = 2 * J_1(pi * r) / (pi * r):
		//   kJincZero1 = j_{1,1} / pi = 3.8317059702075123 / pi  (first positive zero of J_1(pi * r))
		//   kJincZero3 = j_{1,3} / pi = 10.1734681350627221 / pi (third positive zero of J_1(pi * r), 3-lobe EWA support)
		constexpr double kJincZero1 = 1.2196698912665045;
		constexpr double kJincZero3 = 3.2383154841662362;

		// KernelBank.h indices of the two separable comparator kernels used by EvaluatePatch.
		constexpr int kEwaSeparableLanczos3 = 5;   // KernelBank.h:65 ("lanczos3")
		constexpr int kEwaSeparableBicubic = 1;    // KernelBank.h:61 ("bicubic_0_0.5")

		// =========================================================================
		// Part 1: Exact Analytical 1-D Kernel Weight Derivatives
		// =========================================================================

		// Exact analytical derivative d/dx [sin(pi * x) / (pi * x)].
		// Maclaurin expansion: -(pi^2 / 3) * x * (1 - (pi*x)^2 / 10 + (pi*x)^4 / 280 - (pi*x)^6 / 15120 + ...).
		// For |x| < 1e-3, the truncated (pi*x)^6 / 15120 term is < 6.4e-20 < DBL_EPSILON, avoiding catastrophic
		// cancellation in (px * cos(px) - sin(px)) near x = 0 while staying exact to full IEEE-754 precision.
		inline double SincDerivative(double x)
		{
			if (x == 0.0)
				return 0.0;
			const double ax = x < 0.0 ? -x : x;
			const double px = kPi * x;
			if (ax < 1e-3)
			{
				const double px2 = px * px;
				return -(kPi * kPi / 3.0) * x * (1.0 - px2 / 10.0 + (px2 * px2) / 280.0);
			}
			return kPi * (px * std::cos(px) - std::sin(px)) / (px * px);
		}

		// Exact analytical derivative dk(x)/dx for every non-spline kernel in KernelBank.h.
		// All kernels in KernelBank.h are even, so dk(x)/dx is odd and vanishes at x == 0 (symmetric convention for bilinear).
		inline double KernelDerivative(const KernelInfo& k, double x)
		{
			if (x == 0.0)
				return 0.0;
			const double ax = x < 0.0 ? -x : x;
			const double sgn = x > 0.0 ? 1.0 : -1.0;
			switch (k.type)
			{
			case 0:
				return ax < 1.0 ? -sgn : 0.0;
			case 1:
			{
				const double B = k.a;
				const double C = k.b;
				if (ax < 1.0)
				{
					const double dfdAx = ((36.0 - 27.0 * B - 18.0 * C) * ax * ax
						+ (-36.0 + 24.0 * B + 12.0 * C) * ax) / 6.0;
					return sgn * dfdAx;
				}
				if (ax < 2.0)
				{
					const double dfdAx = ((-3.0 * B - 18.0 * C) * ax * ax
						+ (12.0 * B + 60.0 * C) * ax
						+ (-12.0 * B - 48.0 * C)) / 6.0;
					return sgn * dfdAx;
				}
				return 0.0;
			}
			case 2:
			{
				if (k.a <= 0.0 || ax >= k.a)
					return 0.0;
				const double invA = 1.0 / k.a;
				const double xa = x * invA;
				return SincDerivative(x) * Sinc(xa) + Sinc(x) * SincDerivative(xa) * invA;
			}
			default:
				break;
			}
			return 0.0;
		}

		// Internal helper: evaluates natural cubic spline basis weights (if w != nullptr) and their exact
		// derivatives wrt t (if dw != nullptr) in a single tridiagonal solve per tap.
		// From KernelBank.h:164-165:
		//   w[s](t)  = (1 - t)*y[j] + t*y[j+1] - (t*(1-t)/6) * ((2 - t)*M[j] + (1 + t)*M[j+1])
		// Differentiating wrt t (with d/dt[t*(1-t)*(2-t)] = 2 - 6t + 3t^2, d/dt[t*(1-t)*(1+t)] = 1 - 3t^2):
		//   dw[s](t) = (y[j+1] - y[j]) - ((2 - 6t + 3t^2)*M[j] + (1 - 3t^2)*M[j+1]) / 6.
		inline void NaturalSplineWeightsAndDerivatives(int r, double t, double* w, double* dw)
		{
			if (r < 2 || r > 4)
				return;
			const int n = 2 * r;   // 4, 6, or 8 nodes
			const int m = n - 2;   // interior second derivatives
			const int j = r - 1;
			const double t2 = t * t;
			const double cM0 = 2.0 - 6.0 * t + 3.0 * t2;
			const double cM1 = 1.0 - 3.0 * t2;
			for (int s = 0; s < n; ++s)
			{
				double y[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
				double M[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
				y[s] = 1.0;
				double cp[8];
				double dp[8];
				for (int k = 0; k < m; ++k)
				{
					const int idx = k + 1;
					const double rhs = 6.0 * (y[idx - 1] - 2.0 * y[idx] + y[idx + 1]);
					if (k == 0)
					{
						cp[0] = 1.0 / 4.0;
						dp[0] = rhs / 4.0;
					}
					else
					{
						const double den = 4.0 - cp[k - 1];
						cp[k] = 1.0 / den;
						dp[k] = (rhs - dp[k - 1]) / den;
					}
				}
				double sol[8];
				sol[m - 1] = dp[m - 1];
				for (int k = m - 2; k >= 0; --k)
					sol[k] = dp[k] - cp[k] * sol[k + 1];
				for (int k = 0; k < m; ++k)
					M[k + 1] = sol[k];
				if (w != nullptr)
				{
					w[s] = (1.0 - t) * y[j] + t * y[j + 1]
						- (t * (1.0 - t) / 6.0) * ((2.0 - t) * M[j] + (1.0 + t) * M[j + 1]);
				}
				if (dw != nullptr)
				{
					dw[s] = (y[j + 1] - y[j]) - (cM0 * M[j] + cM1 * M[j + 1]) / 6.0;
				}
			}
		}

		// Exact analytical derivative wrt t of NaturalSplineWeights(r, t, w) (KernelBank.h:129-167).
		inline void NaturalSplineWeightDerivatives(int r, double t, double* dw)
		{
			if (dw == nullptr)
				return;
			NaturalSplineWeightsAndDerivatives(r, t, nullptr, dw);
		}

		// Computes normalized tap weights w[0..2r-1] AND their exact quotient-rule derivatives dw[0..2r-1]
		// wrt fractional latent position t (equivalently wrt sub-sample phase, since dp/dphase = 1):
		//   w[s]  = u[s] / sumK
		//   dw[s] = (du[s] - w[s] * sumDk) / sumK.
		inline void TapWeightDerivatives(int kernel, double t, double* w, double* dw)
		{
			if (w == nullptr || dw == nullptr)
				return;
			const KernelInfo& k = Kernel(kernel);
			const int r = k.radius;
			const int taps = 2 * r;
			if (k.type == 3)
			{
				NaturalSplineWeightsAndDerivatives(r, t, w, dw);
			}
			else
			{
				for (int s = 0; s < taps; ++s)
				{
					const double xs = t - static_cast<double>(s - (r - 1));
					w[s] = KernelValue(k, xs);
					dw[s] = KernelDerivative(k, xs);
				}
			}
			double sumK = 0.0;
			double sumDk = 0.0;
			for (int s = 0; s < taps; ++s)
			{
				sumK += w[s];
				sumDk += dw[s];
			}
			// Singularity guard 1e-12 matches KernelBank.h:183.
			if (sumK > 1e-12 || sumK < -1e-12)
			{
				for (int s = 0; s < taps; ++s)
				{
					const double wn = w[s] / sumK;
					w[s] = wn;
					dw[s] = (dw[s] - wn * sumDk) / sumK;
				}
			}
			else
			{
				for (int s = 0; s < taps; ++s)
					dw[s] = 0.0;
			}
		}

		// =========================================================================
		// Part 2: Exact Deterministic Bessel J_1, Radial Jinc, and 2-D EWA-Jinc3
		// =========================================================================

		// Exact cylindrical Bessel function of the first kind J_1(x).
		//   - Odd symmetry: J_1(-x) = -J_1(x).
		//   - Small |x| < 1e-8: Maclaurin (x/2)*(1 - x^2/8) (remainder x^4/192 < 5.3e-35 << DBL_EPSILON).
		//   - 1e-8 <= |x| <= 1000: Miller's backward recurrence from even K = max(44, 2*(ceil(|x|) + 18))
		//     normalized by exact Neumann identity J_0(x) + 2 * sum_{m=1}^{K/2} J_{2m}(x) = 1.
		//     Intermediate values > 1e100 are rescaled by 1e-100 to prevent IEEE-754 overflow.
		//   - |x| > 1000: exact Hankel asymptotic expansion (Abramowitz & Stegun 9.2.5-9.2.10, nu = 1;
		//     truncated term < 1e-17 < DBL_EPSILON) so loop count stays bounded for arbitrary finite inputs.
		inline double BesselJ1(double x)
		{
			if (!std::isfinite(x) || x == 0.0)
				return 0.0;
			const double sgn = x < 0.0 ? -1.0 : 1.0;
			const double ax = x < 0.0 ? -x : x;
			if (ax < 1e-8)
				return 0.5 * x * (1.0 - (x * x) / 8.0);
			if (ax > 1000.0)
			{
				const double invX = 1.0 / ax;
				const double invX2 = invX * invX;
				const double p1 = 1.0 + (15.0 / 128.0) * invX2 - (4725.0 / 32768.0) * invX2 * invX2;
				const double q1 = (3.0 / 8.0) * invX - (105.0 / 1024.0) * invX * invX2;
				const double theta = ax - 0.75 * kPi;
				return sgn * std::sqrt(2.0 / (kPi * ax)) * (p1 * std::cos(theta) - q1 * std::sin(theta));
			}

			int halfK = static_cast<int>(std::ceil(ax)) + 18;
			if (halfK < 22)
				halfK = 22;   // K >= 44 guarantees >27 digits for |x| <= 16 and >16 digits for all |x| <= 1000
			const int K = 2 * halfK;
			const double twoOverX = 2.0 / ax;

			double fNext = 0.0;   // f[K + 1]
			double fCurr = 1.0;   // f[K] (K is even)
			double evenSum = fCurr;
			double f1 = 0.0;
			double f0 = 0.0;

			for (int n = K; n >= 1; --n)
			{
				const double fPrev = static_cast<double>(n) * twoOverX * fCurr - fNext;
				fNext = fCurr;
				fCurr = fPrev;
				const int idx = n - 1;
				if (idx == 1)
					f1 = fCurr;
				else if (idx == 0)
					f0 = fCurr;
				else if ((idx & 1) == 0)
					evenSum += fCurr;

				if (std::fabs(fCurr) > 1e100)
				{
					const double invScale = 1e-100;
					fNext *= invScale;
					fCurr *= invScale;
					evenSum *= invScale;
					f1 *= invScale;
					f0 *= invScale;
				}
			}

			const double norm = f0 + 2.0 * evenSum;
			if (norm == 0.0 || !std::isfinite(norm))
				return 0.0;
			return sgn * (f1 / norm);
		}

		// Normalized radial Jinc(r) = 2 * J_1(pi * |r|) / (pi * |r|), with Jinc(0) = 1.
		// For |r| < 1e-8: Maclaurin 1 - (pi * r)^2 / 8 (remainder (pi*r)^4/192 < 5.1e-33 << DBL_EPSILON).
		inline double Jinc(double r)
		{
			if (!std::isfinite(r))
				return 0.0;
			const double ar = r < 0.0 ? -r : r;
			const double u = kPi * ar;
			if (ar < 1e-8)
				return 1.0 - (u * u) / 8.0;
			return (2.0 * BesselJ1(u)) / u;
		}

		// Exact 3-lobe radial EWA-Lanczos (mpv / libplacebo ewa_lanczos, a = 3) filter weight:
		//   w(r) = Jinc(|r|) * Jinc(|r| * kJincZero1 / kJincZero3)  for |r| < kJincZero3, else 0.
		inline double EwaJinc3Weight(double r)
		{
			const double ar = r < 0.0 ? -r : r;
			if (!(ar < kJincZero3))
				return 0.0;
			return Jinc(ar) * Jinc(ar * (kJincZero1 / kJincZero3));
		}

		// =========================================================================
		// Enums, Settings, Workspace, and Result Types (Rules 1, 2, 4)
		// =========================================================================

		enum class EwaProbeVerdict : int
		{
			Unknown = 0,
			EwaJinc = 1,
			Separable = 2,
			NativeOrUnsure = 3,
			Abstain = 4
		};

		inline const char* EwaProbeVerdictName(EwaProbeVerdict v)
		{
			switch (v)
			{
			case EwaProbeVerdict::Unknown: return "unknown";
			case EwaProbeVerdict::EwaJinc: return "ewa_jinc";
			case EwaProbeVerdict::Separable: return "separable";
			case EwaProbeVerdict::NativeOrUnsure: return "native_or_unsure";
			case EwaProbeVerdict::Abstain: return "abstain";
			}
			return "unknown";
		}

		enum class EwaProbeReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			PatchTooSmall,
			LowPatchDetail,
			SingularNormalMatrix,
			EwaJincConfirmed,
			SeparablePreferred,
			NoUpscaleDip,
			AmbiguousKernelMargin
		};

		inline const char* EwaProbeReasonText(EwaProbeReason r)
		{
			switch (r)
			{
			case EwaProbeReason::Ok: return "ok";
			case EwaProbeReason::Disabled: return "ewa jinc probe disabled";
			case EwaProbeReason::InvalidInput: return "invalid input";
			case EwaProbeReason::PatchTooSmall: return "abstain (patch too small or underdetermined)";
			case EwaProbeReason::LowPatchDetail: return "abstain (low patch detail)";
			case EwaProbeReason::SingularNormalMatrix: return "abstain (singular 2d normal matrix)";
			case EwaProbeReason::EwaJincConfirmed: return "ewa jinc3 confirmed over separable";
			case EwaProbeReason::SeparablePreferred: return "separable kernel preferred over ewa jinc3";
			case EwaProbeReason::NoUpscaleDip: return "no upscale dip (native or unsure)";
			case EwaProbeReason::AmbiguousKernelMargin: return "ambiguous ewa vs separable kernel margin";
			}
			return "unknown";
		}

		enum class Operator2DMode : int
		{
			EwaJinc3 = 0,
			SeparableKernel = 1
		};

		struct EwaJincSettings
		{
			// Default-OFF contract: preserves 100% bit-exactness of existing pipeline when OFF.
			bool enabled = false;

			// Minimum 2-D output patch edge length (24 x 24 = 576 output pixels): overdetermined
			// (active latent nodes M < 576) for EWA-Jinc3 (radius 3.238) across upscale ratios <= 2/3.
			int patchSize = 24;

			// Inner evaluation margin (ceil(kJincZero3) = 4 px): evaluates residual on [4 .. patchW - 4)
			// x [4 .. patchH - 4) (= 16 x 16 = 256 inner pixels for 24x24), clearing boundary halo effects.
			int innerMargin = 4;

			// Derived: 3 * EvidenceSettings::majorityDip (-0.03, ChainEvidence.h:92) = -0.09 ln units.
			// Requires a clear 2-D upscale dip (~8.6% RMS reduction) so native 4K never false-verifies.
			double minUpscaleDip = -0.09;

			// Derived: ReadingSettings::kernelMarginScale (0.05 ln units, ChainReading.h:48).
			// Requires ~5% lower residual to prefer EWA-Jinc3 over separable kernels (or vice versa).
			double minKernelMargin = 0.05;

			// Derived: EvidenceSettings::lineGateFactor (1.5, ChainEvidence.h:87).
			double lineGateFactor = 1.5;

			// Derived: VerifierSettings::epsCodes (0.02 in 10-bit code units, DescaleVerifier.h:88).
			double epsCodes = 0.02;

			// Derived: VerifierSettings::refFraction2 (0.08, DescaleVerifier.h:86).
			double refFraction = 0.08;
		};

		// Caller-owned bounded reusable workspace for 2-D sparse operator build and Cholesky solve.
		// Rule 2: Zero static or thread_local mutable caches anywhere in this unit.
		struct EwaJincWorkspace
		{
			int patchW = 0;
			int patchH = 0;
			int latW = 0;
			int latH = 0;
			int nodes = 0;            // active latent unknowns M after dead-corner compaction
			int rawNodes = 0;         // bounding-box latent nodes latW * latH
			int cxMin = 0;
			int cyMin = 0;
			double ridgeUsed = 0.0;   // relative ridge multiplier that factored cleanly (1e-12, 1e-9, or 1e-6)
			bool operatorOk = false;

			// Sparse CSR representation of the (patchW * patchH) x nodes 2-D upscale operator U.
			std::vector<int> rowStart;     // size = P + 1
			std::vector<int> rowCount;     // size = P
			std::vector<int> colIdx;       // active latent node index for each non-zero (strictly ascending per row)
			std::vector<double> val;       // normalized tap weight for each non-zero

			// Dense lower-triangle normal matrix A = U^T U, Cholesky factor L (reciprocal diagonal stored),
			// right-hand side b = U^T y, latent solution z, and reusable scratch buffers.
			std::vector<double> U;         // scratch buffer
			std::vector<double> A;         // M x M lower triangle (row-major)
			std::vector<double> L;         // M x M lower triangular Cholesky factor (diagonal holds 1 / L(i, i))
			std::vector<double> b;         // length M: U^T y
			std::vector<double> z;         // length M: solved latent samples
			std::vector<double> levWork;   // length M: scratch for exact inner-box leverage L^{-1} u_p
			std::vector<int> nodeMap;      // length latW * latH: bounding-box node -> active node index (-1 if unused)

			void Reset()
			{
				patchW = 0;
				patchH = 0;
				latW = 0;
				latH = 0;
				nodes = 0;
				rawNodes = 0;
				cxMin = 0;
				cyMin = 0;
				ridgeUsed = 0.0;
				operatorOk = false;
				rowStart.clear();
				rowCount.clear();
				colIdx.clear();
				val.clear();
				U.clear();
				A.clear();
				L.clear();
				b.clear();
				z.clear();
				levWork.clear();
				nodeMap.clear();
			}
		};

		struct EwaProbeResult
		{
			bool valid = false;
			EwaProbeVerdict verdict = EwaProbeVerdict::Unknown;
			EwaProbeReason reason = EwaProbeReason::Disabled;
			int bestSeparableKernel = -1;     // 5 = Lanczos3, 1 = Bicubic_0_0.5
			double statEwa = 0.0;             // ln(s_EWA / s_base)
			double statSepLanczos3 = 0.0;     // ln(s_SepL3 / s_base)
			double statSepBicubic = 0.0;      // ln(s_SepBC / s_base)
			double ewaMarginOverSeparable = 0.0;   // min(statSepL3, statSepBC) - statEwa (>0: EWA beats separable)
			double baselineRms = 0.0;
		};

		// =========================================================================
		// 2-D Sparse CSR Operator Builder & Dense Cholesky Normal-Equation Solver
		// =========================================================================

		// Builds the sparse CSR 2-D upscale operator U (un-clamped exact support nodes covering all
		// latent samples touched by [originX .. originX + patchW) x [originY .. originY + patchH),
		// compacted to active nodes only), assembles A = U^T U, and computes dense lower-triangular
		// Cholesky factor L of (A + relRidge * maxDiag * I) trying relRidge = 1e-12, 1e-9, 1e-6
		// (matching KernelBank.h:335-347).
		inline bool BuildEwaOrSeparable2DOperator(
			int patchW, int patchH,
			int originX, int originY,
			int Nx, int nx,
			int Ny, int ny,
			double phaseX, double phaseY,
			Operator2DMode mode,
			int separableKernel,
			EwaJincWorkspace& ws,
			EwaProbeReason* failReason = nullptr)
		{
			ws.operatorOk = false;
			ws.nodes = 0;
			ws.ridgeUsed = 0.0;

			if (patchW <= 0 || patchH <= 0 || Nx <= 0 || Ny <= 0 || nx <= 1 || ny <= 1 || nx >= Nx || ny >= Ny
				|| !std::isfinite(phaseX) || !std::isfinite(phaseY))
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::InvalidInput;
				return false;
			}
			if (mode == Operator2DMode::SeparableKernel && (separableKernel < 0 || separableKernel >= kKernelCount))
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::InvalidInput;
				return false;
			}

			const double rx = static_cast<double>(nx) / static_cast<double>(Nx);
			const double ry = static_cast<double>(ny) / static_cast<double>(Ny);
			const double px0 = (static_cast<double>(originX) + 0.5) * rx - 0.5 + phaseX;
			const double pxLast = (static_cast<double>(originX + patchW - 1) + 0.5) * rx - 0.5 + phaseX;
			const double py0 = (static_cast<double>(originY) + 0.5) * ry - 0.5 + phaseY;
			const double pyLast = (static_cast<double>(originY + patchH - 1) + 0.5) * ry - 0.5 + phaseY;

			int cxMin = 0;
			int cxMax = 0;
			int cyMin = 0;
			int cyMax = 0;
			if (mode == Operator2DMode::EwaJinc3)
			{
				cxMin = static_cast<int>(std::floor(px0 - kJincZero3)) + 1;
				cxMax = static_cast<int>(std::ceil(pxLast + kJincZero3)) - 1;
				cyMin = static_cast<int>(std::floor(py0 - kJincZero3)) + 1;
				cyMax = static_cast<int>(std::ceil(pyLast + kJincZero3)) - 1;
			}
			else
			{
				const KernelInfo& ki = Kernel(separableKernel);
				const int rad = ki.radius;
				const int taps = 2 * rad;
				cxMin = static_cast<int>(std::floor(px0)) - rad + 1;
				cxMax = static_cast<int>(std::floor(pxLast)) - rad + taps;
				cyMin = static_cast<int>(std::floor(py0)) - rad + 1;
				cyMax = static_cast<int>(std::floor(pyLast)) - rad + taps;
			}

			const int latW = cxMax - cxMin + 1;
			const int latH = cyMax - cyMin + 1;
			if (latW <= 0 || latH <= 0)
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::InvalidInput;
				return false;
			}

			const int64_t P64 = static_cast<int64_t>(patchW) * static_cast<int64_t>(patchH);
			const int64_t rawNodes64 = static_cast<int64_t>(latW) * static_cast<int64_t>(latH);
			// Practical guard: dense Cholesky matrix M x M stays bounded (M <= 4096 => <= 128 MB).
			if (P64 <= 0 || rawNodes64 <= 0 || P64 > 65536 || rawNodes64 > 4096)
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::InvalidInput;
				return false;
			}

			const int P = static_cast<int>(P64);
			const int rawNodes = static_cast<int>(rawNodes64);
			ws.patchW = patchW;
			ws.patchH = patchH;
			ws.latW = latW;
			ws.latH = latH;
			ws.rawNodes = rawNodes;
			ws.cxMin = cxMin;
			ws.cyMin = cyMin;

			ws.rowStart.resize(static_cast<size_t>(P) + 1u);
			ws.rowCount.resize(static_cast<size_t>(P));
			ws.colIdx.clear();
			ws.val.clear();
			ws.nodeMap.assign(static_cast<size_t>(rawNodes), -1);

			const double r2Max = kJincZero3 * kJincZero3;
			for (int j = 0; j < patchH; ++j)
			{
				const double py = (static_cast<double>(originY + j) + 0.5) * ry - 0.5 + phaseY;
				for (int i = 0; i < patchW; ++i)
				{
					const int p = j * patchW + i;
					const size_t rowBegin = ws.colIdx.size();
					ws.rowStart[static_cast<size_t>(p)] = static_cast<int>(rowBegin);
					const double px = (static_cast<double>(originX + i) + 0.5) * rx - 0.5 + phaseX;

					if (mode == Operator2DMode::EwaJinc3)
					{
						const int ix0 = (std::max)(cxMin, static_cast<int>(std::floor(px - kJincZero3)) + 1);
						const int ix1 = (std::min)(cxMax, static_cast<int>(std::ceil(px + kJincZero3)) - 1);
						const int iy0 = (std::max)(cyMin, static_cast<int>(std::floor(py - kJincZero3)) + 1);
						const int iy1 = (std::min)(cyMax, static_cast<int>(std::ceil(py + kJincZero3)) - 1);
						double wSum = 0.0;
						for (int iy = iy0; iy <= iy1; ++iy)
						{
							const double dy = py - static_cast<double>(iy);
							const double dy2 = dy * dy;
							if (!(dy2 < r2Max))
								continue;
							const int rowOff = (iy - cyMin) * latW;
							for (int ix = ix0; ix <= ix1; ++ix)
							{
								const double dx = px - static_cast<double>(ix);
								const double r2 = dx * dx + dy2;
								if (!(r2 < r2Max))
									continue;
								const double w = EwaJinc3Weight(std::sqrt(r2));
								if (w == 0.0)
									continue;
								const int rawIdx = rowOff + (ix - cxMin);
								ws.colIdx.push_back(rawIdx);
								ws.val.push_back(w);
								ws.nodeMap[static_cast<size_t>(rawIdx)] = 1;
								wSum += w;
							}
						}
						if (!(wSum > 1e-12 || wSum < -1e-12))
						{
							if (failReason != nullptr)
								*failReason = EwaProbeReason::SingularNormalMatrix;
							return false;
						}
						const size_t rowEnd = ws.val.size();
						for (size_t e = rowBegin; e < rowEnd; ++e)
							ws.val[e] /= wSum;
					}
					else
					{
						const KernelInfo& ki = Kernel(separableKernel);
						const int rad = ki.radius;
						const int taps = 2 * rad;
						const double flx = std::floor(px);
						const double fly = std::floor(py);
						double wx[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
						double wy[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
						TapWeights(separableKernel, px - flx, wx);
						TapWeights(separableKernel, py - fly, wy);
						const int ixBase = static_cast<int>(flx) - rad + 1;
						const int iyBase = static_cast<int>(fly) - rad + 1;
						for (int sy = 0; sy < taps; ++sy)
						{
							const double wyVal = wy[sy];
							if (wyVal == 0.0)
								continue;
							const int iy = iyBase + sy;
							const int rowOff = (iy - cyMin) * latW;
							for (int sx = 0; sx < taps; ++sx)
							{
								const double wxVal = wx[sx];
								if (wxVal == 0.0)
									continue;
								const double w = wxVal * wyVal;
								if (w == 0.0)
									continue;
								const int ix = ixBase + sx;
								const int rawIdx = rowOff + (ix - cxMin);
								ws.colIdx.push_back(rawIdx);
								ws.val.push_back(w);
								ws.nodeMap[static_cast<size_t>(rawIdx)] = 1;
							}
						}
					}

					ws.rowCount[static_cast<size_t>(p)] = static_cast<int>(ws.colIdx.size() - rowBegin);
				}
			}
			ws.rowStart[static_cast<size_t>(P)] = static_cast<int>(ws.colIdx.size());

			// Compact active latent nodes in ascending row-major order (eliminates untouched circular corners).
			int activeNodes = 0;
			for (int k = 0; k < rawNodes; ++k)
			{
				if (ws.nodeMap[static_cast<size_t>(k)] >= 0)
					ws.nodeMap[static_cast<size_t>(k)] = activeNodes++;
			}
			ws.nodes = activeNodes;
			if (activeNodes <= 0 || activeNodes >= P)
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::PatchTooSmall;
				return false;
			}
			for (size_t e = 0; e < ws.colIdx.size(); ++e)
				ws.colIdx[e] = ws.nodeMap[static_cast<size_t>(ws.colIdx[e])];

			// Assemble lower triangle of symmetric normal matrix A = U^T U.
			const size_t M = static_cast<size_t>(activeNodes);
			ws.A.assign(M * M, 0.0);
			for (int p = 0; p < P; ++p)
			{
				const int r0 = ws.rowStart[static_cast<size_t>(p)];
				const int r1 = r0 + ws.rowCount[static_cast<size_t>(p)];
				for (int a = r0; a < r1; ++a)
				{
					const size_t ca = static_cast<size_t>(ws.colIdx[static_cast<size_t>(a)]);
					const double va = ws.val[static_cast<size_t>(a)];
					double* aRow = &ws.A[ca * M];
					for (int bIdx = r0; bIdx <= a; ++bIdx)
					{
						const size_t cb = static_cast<size_t>(ws.colIdx[static_cast<size_t>(bIdx)]);
						aRow[cb] += va * ws.val[static_cast<size_t>(bIdx)];
					}
				}
			}

			double maxDiag = 0.0;
			for (size_t i = 0; i < M; ++i)
			{
				const double d = ws.A[i * M + i];
				if (d > maxDiag)
					maxDiag = d;
			}
			if (!(maxDiag > 0.0))
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::SingularNormalMatrix;
				return false;
			}

			// Dense Cholesky factorization with relative ridge 1e-12, 1e-9, 1e-6 (KernelBank.h:335-347).
			ws.L.resize(M * M);
			double relRidge = 1e-12;
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				const double ridge = relRidge * maxDiag;
				for (size_t i = 0; i < M; ++i)
				{
					const double* srcRow = &ws.A[i * M];
					double* dstRow = &ws.L[i * M];
					for (size_t k = 0; k <= i; ++k)
						dstRow[k] = srcRow[k];
					dstRow[i] += ridge;
				}
				bool factored = true;
				for (size_t j = 0; j < M; ++j)
				{
					double* lj = &ws.L[j * M];
					double diagSum = lj[j];
					for (size_t k = 0; k < j; ++k)
						diagSum -= lj[k] * lj[k];
					// Pivot guard 1e-300 matches KernelBank.h:534.
					if (!(diagSum > 1e-300))
					{
						factored = false;
						break;
					}
					const double invDiag = 1.0 / std::sqrt(diagSum);
					lj[j] = invDiag;   // store reciprocal diagonal
					for (size_t i = j + 1; i < M; ++i)
					{
						double* li = &ws.L[i * M];
						double v = li[j];
						for (size_t k = 0; k < j; ++k)
							v -= li[k] * lj[k];
						li[j] = v * invDiag;
					}
				}
				if (factored)
				{
					ws.ridgeUsed = relRidge;
					ws.operatorOk = true;
					if (failReason != nullptr)
						*failReason = EwaProbeReason::Ok;
					return true;
				}
				relRidge *= 1000.0;
			}

			if (failReason != nullptr)
				*failReason = EwaProbeReason::SingularNormalMatrix;
			return false;
		}

		// Solves (U^T U + ridge * I) z = U^T y on the already-factored workspace operator and computes
		// the inner-box residual RMS normalized by the exact inner-box degrees of freedom:
		//   dof_inner = (1 / |I|) * sum_{p in I} (1 - ||L^{-1} u_p||_2^2).
		inline bool Solve2DPatchResidual(
			const double* patch, int stride,
			int innerMargin, double epsCodes,
			EwaJincWorkspace& ws,
			double& sOut, double& rawRmsOut, double& dofOut,
			EwaProbeReason* failReason = nullptr)
		{
			sOut = 0.0;
			rawRmsOut = 0.0;
			dofOut = 0.0;
			if (!ws.operatorOk || patch == nullptr || stride < ws.patchW || innerMargin < 0
				|| ws.patchW - 2 * innerMargin < 1 || ws.patchH - 2 * innerMargin < 1)
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::InvalidInput;
				return false;
			}

			const size_t M = static_cast<size_t>(ws.nodes);
			ws.b.assign(M, 0.0);
			for (int j = 0; j < ws.patchH; ++j)
			{
				const double* rowY = patch + static_cast<size_t>(j) * static_cast<size_t>(stride);
				for (int i = 0; i < ws.patchW; ++i)
				{
					const double yp = rowY[i];
					const int p = j * ws.patchW + i;
					const int r0 = ws.rowStart[static_cast<size_t>(p)];
					const int r1 = r0 + ws.rowCount[static_cast<size_t>(p)];
					for (int e = r0; e < r1; ++e)
						ws.b[static_cast<size_t>(ws.colIdx[static_cast<size_t>(e)])] += ws.val[static_cast<size_t>(e)] * yp;
				}
			}

			// Forward substitution: L v = b.
			ws.z.resize(M);
			for (size_t i = 0; i < M; ++i)
			{
				double v = ws.b[i];
				const double* li = &ws.L[i * M];
				for (size_t k = 0; k < i; ++k)
					v -= li[k] * ws.z[k];
				ws.z[i] = v * li[i];
			}

			// Backward substitution: L^T z = v.
			for (size_t ii = M; ii > 0; --ii)
			{
				const size_t i = ii - 1u;
				double v = ws.z[i];
				for (size_t k = i + 1u; k < M; ++k)
					v -= ws.L[k * M + i] * ws.z[k];
				ws.z[i] = v * ws.L[i * M + i];
			}

			// Evaluate residual and exact hat-matrix leverage over the inner evaluation box.
			ws.levWork.resize(M);
			double rss = 0.0;
			double dofSum = 0.0;
			int innerCount = 0;
			const int jEnd = ws.patchH - innerMargin;
			const int iEnd = ws.patchW - innerMargin;
			for (int j = innerMargin; j < jEnd; ++j)
			{
				const double* rowY = patch + static_cast<size_t>(j) * static_cast<size_t>(stride);
				for (int i = innerMargin; i < iEnd; ++i)
				{
					const int p = j * ws.patchW + i;
					const int r0 = ws.rowStart[static_cast<size_t>(p)];
					const int r1 = r0 + ws.rowCount[static_cast<size_t>(p)];
					double pred = 0.0;
					for (int e = r0; e < r1; ++e)
						pred += ws.val[static_cast<size_t>(e)] * ws.z[static_cast<size_t>(ws.colIdx[static_cast<size_t>(e)])];
					const double err = rowY[i] - pred;
					rss += err * err;

					// Exact leverage h_p = ||L^{-1} u_p||_2^2. Since u_p[k] = 0 for k < firstCol
					// and L is lower triangular, (L^{-1} u_p)[k] = 0 identically for k < firstCol.
					double hp = 0.0;
					if (r0 < r1)
					{
						const size_t firstCol = static_cast<size_t>(ws.colIdx[static_cast<size_t>(r0)]);
						for (size_t idx = firstCol; idx < M; ++idx)
							ws.levWork[idx] = 0.0;
						for (int e = r0; e < r1; ++e)
							ws.levWork[static_cast<size_t>(ws.colIdx[static_cast<size_t>(e)])] = ws.val[static_cast<size_t>(e)];
						for (size_t row = firstCol; row < M; ++row)
						{
							double v = ws.levWork[row];
							const double* lRow = &ws.L[row * M];
							for (size_t col = firstCol; col < row; ++col)
								v -= lRow[col] * ws.levWork[col];
							const double wv = v * lRow[row];
							ws.levWork[row] = wv;
							hp += wv * wv;
						}
					}
					dofSum += (1.0 - hp);
					++innerCount;
				}
			}

			if (innerCount <= 0)
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::PatchTooSmall;
				return false;
			}
			const double dof = dofSum / static_cast<double>(innerCount);
			// Minimum DOF guard 0.02 matches DescaleVerifier.h:496.
			if (!(dof >= 0.02))
			{
				if (failReason != nullptr)
					*failReason = EwaProbeReason::PatchTooSmall;
				return false;
			}
			const double ms = rss / static_cast<double>(innerCount);
			rawRmsOut = std::sqrt(ms);
			dofOut = dof;
			sOut = std::sqrt((ms + epsCodes * epsCodes) / dof);
			if (failReason != nullptr)
				*failReason = EwaProbeReason::Ok;
			return true;
		}

		// Internal helper: evaluates EWA-Jinc3, Separable Lanczos3, and Separable Bicubic(0, 0.5) at one (nx, ny).
		inline bool EvaluateTriple2D(
			const double* patch, int patchW, int patchH, int stride,
			int originX, int originY,
			int Nx, int nx, int Ny, int ny,
			double phaseX, double phaseY,
			const EwaJincSettings& cfg,
			EwaJincWorkspace& ws,
			double& sEwa, double& sSepL3, double& sSepBC,
			double* maxRawRms,
			EwaProbeReason& failReason)
		{
			double raw0 = 0.0;
			double raw1 = 0.0;
			double raw2 = 0.0;
			double dofTmp = 0.0;
			failReason = EwaProbeReason::Ok;

			if (!BuildEwaOrSeparable2DOperator(patchW, patchH, originX, originY, Nx, nx, Ny, ny,
				phaseX, phaseY, Operator2DMode::EwaJinc3, -1, ws, &failReason)
				|| !Solve2DPatchResidual(patch, stride, cfg.innerMargin, cfg.epsCodes, ws, sEwa, raw0, dofTmp, &failReason))
			{
				return false;
			}

			if (!BuildEwaOrSeparable2DOperator(patchW, patchH, originX, originY, Nx, nx, Ny, ny,
				phaseX, phaseY, Operator2DMode::SeparableKernel, kEwaSeparableLanczos3, ws, &failReason)
				|| !Solve2DPatchResidual(patch, stride, cfg.innerMargin, cfg.epsCodes, ws, sSepL3, raw1, dofTmp, &failReason))
			{
				return false;
			}

			if (!BuildEwaOrSeparable2DOperator(patchW, patchH, originX, originY, Nx, nx, Ny, ny,
				phaseX, phaseY, Operator2DMode::SeparableKernel, kEwaSeparableBicubic, ws, &failReason)
				|| !Solve2DPatchResidual(patch, stride, cfg.innerMargin, cfg.epsCodes, ws, sSepBC, raw2, dofTmp, &failReason))
			{
				return false;
			}

			if (maxRawRms != nullptr)
				*maxRawRms = (std::max)(raw0, (std::max)(raw1, raw2));
			return true;
		}

		// Evaluates whether a small 2-D patch (e.g. 24 x 24 in 10-bit code units) was upscaled from
		// latent (nx, ny) using radial EWA-Jinc3 vs separable Lanczos3 / Bicubic(0, 0.5), or shows
		// no upscale dip (native / unsure) or insufficient detail (abstain).
		inline EwaProbeResult EvaluatePatch(
			const double* patch, int patchW, int patchH, int stride,
			int originX, int originY,
			int Nx, int nx, int Ny, int ny,
			double phaseX, double phaseY,
			double floorCodes,
			const EwaJincSettings& cfg,
			EwaJincWorkspace& ws)
		{
			EwaProbeResult res;
			if (!cfg.enabled)
			{
				res.valid = false;
				res.verdict = EwaProbeVerdict::Unknown;
				res.reason = EwaProbeReason::Disabled;
				return res;
			}

			if (patch == nullptr || patchW <= 0 || patchH <= 0 || stride < patchW
				|| Nx <= 0 || Ny <= 0 || nx <= 1 || ny <= 1 || nx >= Nx || ny >= Ny
				|| originX < 0 || originY < 0 || originX + patchW > Nx || originY + patchH > Ny
				|| !std::isfinite(phaseX) || !std::isfinite(phaseY)
				|| !std::isfinite(floorCodes) || floorCodes < 0.0
				|| cfg.patchSize < 0 || cfg.innerMargin < 0
				|| !std::isfinite(cfg.epsCodes) || cfg.epsCodes < 0.0
				|| !std::isfinite(cfg.lineGateFactor) || cfg.lineGateFactor < 0.0
				|| !std::isfinite(cfg.minUpscaleDip)
				|| !std::isfinite(cfg.minKernelMargin) || cfg.minKernelMargin < 0.0
				|| !(cfg.refFraction > 0.0 && cfg.refFraction < 0.5))
			{
				res.valid = false;
				res.verdict = EwaProbeVerdict::Abstain;
				res.reason = EwaProbeReason::InvalidInput;
				return res;
			}

			// Verify finite patch samples before building any normal equations.
			for (int j = 0; j < patchH; ++j)
			{
				const double* rowY = patch + static_cast<size_t>(j) * static_cast<size_t>(stride);
				for (int i = 0; i < patchW; ++i)
				{
					if (!std::isfinite(rowY[i]))
					{
						res.valid = false;
						res.verdict = EwaProbeVerdict::Abstain;
						res.reason = EwaProbeReason::InvalidInput;
						return res;
					}
				}
			}

			// Require patch to meet configured minimum size and leave at least a 4 x 4 inner evaluation box.
			if (patchW < cfg.patchSize || patchH < cfg.patchSize
				|| patchW - 2 * cfg.innerMargin < 4 || patchH - 2 * cfg.innerMargin < 4)
			{
				res.valid = false;
				res.verdict = EwaProbeVerdict::Abstain;
				res.reason = EwaProbeReason::PatchTooSmall;
				return res;
			}

			// 1. Solve candidate (nx, ny) across EWA-Jinc3, Separable Lanczos3, and Separable Bicubic.
			double sEwa = 0.0;
			double sSepL3 = 0.0;
			double sSepBC = 0.0;
			EwaProbeReason failReason = EwaProbeReason::Ok;
			if (!EvaluateTriple2D(patch, patchW, patchH, stride, originX, originY,
				Nx, nx, Ny, ny, phaseX, phaseY, cfg, ws, sEwa, sSepL3, sSepBC, nullptr, failReason))
			{
				res.valid = false;
				res.verdict = EwaProbeVerdict::Abstain;
				res.reason = failReason;
				return res;
			}

			// 2. Solve off-grid reference baseline sizes (nx*(1 +- refFraction), ny*(1 +- refFraction)).
			// If either primary reference is out-of-bounds or underdetermined on a compact patch (e.g.
			// ratio 2/3 at +8% on 24x24), fall back to the midpoint offset 0.5 * refFraction (matching
			// VerifierSettings::refFraction1 / refFraction2 = 0.04 / 0.08 = 0.5) to maintain 2 references.
			const double refOffsets[4] = {
				-cfg.refFraction,
				cfg.refFraction,
				0.5 * cfg.refFraction,
				-0.5 * cfg.refFraction
			};
			int usedRefNx[4] = { 0, 0, 0, 0 };
			int usedRefNy[4] = { 0, 0, 0, 0 };
			double refBestS[4] = { 0.0, 0.0, 0.0, 0.0 };
			int refCount = 0;
			double refMaxRawRms = 0.0;
			EwaProbeReason lastRefFail = EwaProbeReason::PatchTooSmall;

			for (int rIdx = 0; rIdx < 4 && refCount < 2; ++rIdx)
			{
				const int rxN = RoundToInt(static_cast<double>(nx) * (1.0 + refOffsets[rIdx]));
				const int ryN = RoundToInt(static_cast<double>(ny) * (1.0 + refOffsets[rIdx]));
				if (rxN <= 1 || rxN >= Nx || ryN <= 1 || ryN >= Ny || (rxN == nx && ryN == ny))
					continue;
				bool dup = false;
				for (int prev = 0; prev < refCount; ++prev)
				{
					if (usedRefNx[prev] == rxN && usedRefNy[prev] == ryN)
					{
						dup = true;
						break;
					}
				}
				if (dup)
					continue;

				double rEwa = 0.0;
				double rL3 = 0.0;
				double rBC = 0.0;
				double rRaw = 0.0;
				EwaProbeReason rFail = EwaProbeReason::Ok;
				if (!EvaluateTriple2D(patch, patchW, patchH, stride, originX, originY,
					Nx, rxN, Ny, ryN, 0.0, 0.0, cfg, ws, rEwa, rL3, rBC, &rRaw, rFail))
				{
					lastRefFail = rFail;
					continue;
				}
				usedRefNx[refCount] = rxN;
				usedRefNy[refCount] = ryN;
				refBestS[refCount] = (std::min)(rEwa, (std::min)(rL3, rBC));
				if (rRaw > refMaxRawRms)
					refMaxRawRms = rRaw;
				++refCount;
			}

			if (refCount < 2)
			{
				res.valid = false;
				res.verdict = EwaProbeVerdict::Abstain;
				res.reason = lastRefFail;
				return res;
			}

			// Median of the 2 valid off-grid reference baselines.
			const double baselineRms = 0.5 * (refBestS[0] + refBestS[1]);
			res.baselineRms = baselineRms;

			// 3. Patch detail gate: abstain on flat or sub-floor patches.
			if (baselineRms < cfg.lineGateFactor * floorCodes || !(refMaxRawRms > cfg.epsCodes))
			{
				res.valid = false;
				res.verdict = EwaProbeVerdict::Abstain;
				res.reason = EwaProbeReason::LowPatchDetail;
				return res;
			}

			// 4. Compute log-ratio residuals against the shared baseline and classify verdict.
			const double baseSafe = (std::max)(baselineRms, 1e-12);
			res.statEwa = std::log((std::max)(sEwa, 1e-12) / baseSafe);
			res.statSepLanczos3 = std::log((std::max)(sSepL3, 1e-12) / baseSafe);
			res.statSepBicubic = std::log((std::max)(sSepBC, 1e-12) / baseSafe);

			const double bestSep = (std::min)(res.statSepLanczos3, res.statSepBicubic);
			res.bestSeparableKernel = (res.statSepLanczos3 <= res.statSepBicubic)
				? kEwaSeparableLanczos3
				: kEwaSeparableBicubic;
			res.ewaMarginOverSeparable = bestSep - res.statEwa;
			const double bestStat = (std::min)(res.statEwa, bestSep);
			res.valid = true;

			if (bestStat >= cfg.minUpscaleDip)
			{
				res.verdict = EwaProbeVerdict::NativeOrUnsure;
				res.reason = EwaProbeReason::NoUpscaleDip;
			}
			else if (res.statEwa <= bestSep - cfg.minKernelMargin)
			{
				res.verdict = EwaProbeVerdict::EwaJinc;
				res.reason = EwaProbeReason::EwaJincConfirmed;
			}
			else if (bestSep <= res.statEwa - cfg.minKernelMargin)
			{
				res.verdict = EwaProbeVerdict::Separable;
				res.reason = EwaProbeReason::SeparablePreferred;
			}
			else
			{
				res.verdict = EwaProbeVerdict::NativeOrUnsure;
				res.reason = EwaProbeReason::AmbiguousKernelMargin;
			}

			return res;
		}
	}
}
#pragma fp_contract(on)
