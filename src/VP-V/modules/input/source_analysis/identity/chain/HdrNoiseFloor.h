// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// HdrNoiseFloor (SID HDR Phase): SMPTE ST 2084 (PQ) & ARIB STD-B67 (HLG) EOTF/OETF, exact analytical
// derivatives, heteroscedastic linear-light quantization noise propagation (10-bit & 12-bit), and
// Gauss-Markov weighted-least-squares (BLUE) noise-whitened descale verification.
//
// Motivation:
//   - Linearizing PQ/HLG video before descale verification restores linear shift-invariant upscale physics,
//     but the nonlinear EOTF maps uniform code-space quantization noise sigma_E' = step / sqrt(12) into
//     strongly heteroscedastic linear-light noise:
//       sigma_Y(E') = (dY / dE') * sigma_E'.
//   - Across PQ's 0..10000 nit range, dY/dE' spans > 4 orders of magnitude (variance > 10^8x), so naive
//     unweighted linear MSE is completely swamped by specular highlights and blind to dark-toe / midtone
//     upscaling. Solving the exact Gauss-Markov WLS normal equations (U^T W U) z = U^T W y with diagonal
//     weights W_ii = 1 / sigma_Y(i)^2 (using Jacobi diagonal equilibration for conditioning across 10^12+
//     weight ratios) yields the Minimum Variance Unbiased Estimator (BLUE) and unit-variance whitened
//     residuals z_i = (y_i - (U z)_i) / sigma_Y(i) with E[mean(z_i^2) / dof] = 1.0 under pure quantization
//     noise at any luminance profile.
//
// Rule 1 (Every constant derived from published standards, quantization physics, or existing contracts):
//   - SMPTE ST 2084 / ITU-R BT.2100 Table 4 exact rationals:
//       m1 = 2610 / 16384, m2 = (2523 / 4096) * 128,
//       c1 = 3424 / 4096, c2 = (2413 / 4096) * 32, c3 = (2392 / 4096) * 32.
//   - ARIB STD-B67 / ITU-R BT.2100 Table 5 exact HLG constants:
//       a = 0.17883277, b = 1 - 4*a, c = 0.5 - a * ln(4*a) (exact C0 & C1 continuity at E' = 0.5).
//   - ITU-R BT.2100 Table 9 quantization step in normalized [0, 1] signal:
//       limited range: 1 / (219 * 2^(bitDepth - 8))  (10-bit: 1/876; 12-bit: 1/3504, 4x finer),
//       full range:    1 / (2^bitDepth - 1)          (10-bit: 1/1023; 12-bit: 1/4095).
//   - Uniform quantization SD: sigma_E' = step / sqrt(12)  (variance = step^2 / 12).
//   - Dark-toe derivative floor: derivMin = EOTF(step) / step — the exact finite-difference luminance
//     slope across the very first quantization interval [0, 1 LSB] above black (strictly > 0 for all
//     bit depths in [8, 16], zero invented constants).
//   - Contract settings:
//       floorGuardDivisor = 4.0  (ChainEdgeStats.h:114: eps = 1/4 of floor, 2.77 nats dynamic range),
//       trimFraction      = 0.2  (DescaleVerifier.h:87 / ChainEvidence.h:97: 20% trimmed mean),
//       refFraction1/2    = 0.04 / 0.08 (DescaleVerifier.h:85-86: baseline reference sizes),
//       minLatent         = 16, maxLatentRatio = 0.985, minDof = 0.02 (DescaleVerifier.h:89-90, 496).
//   - Strictly numerical singularity guards: 1e-12 / 1e-9 / 1e-6 relative diagonal ridge and 1e-300
//     Cholesky pivot guard (matching KernelBank.h:335-346, 534).
//
// Pure standard-library C++14, header-only, deterministic, zero Windows headers, zero clocks/threads,
// zero static/thread_local mutable caches (Rule 2: all scratch state lives in caller-owned HdrNoiseWorkspace),
// zero silent fallbacks (Rule 4: explicit HdrNoiseReason + darkToeClampedSamples counter), default OFF.

#include "ChainCommon.h"
#include "KernelBank.h"
#include "../../../../../core/RobustStats.h"

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
		// ====================================================================================================
		// 1. Transfer Enum, Explicit Reason Codes & Default-OFF Settings
		// ====================================================================================================

		enum class HdrTransfer : int
		{
			Sdr = 0,
			Pq = 1,
			Hlg = 2
		};

		inline const char* HdrTransferName(HdrTransfer t)
		{
			switch (t)
			{
			case HdrTransfer::Sdr: return "sdr";
			case HdrTransfer::Pq: return "pq";
			case HdrTransfer::Hlg: return "hlg";
			default: return "unknown";
			}
		}

		enum class HdrNoiseReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			SdrPassthrough,
			DarkToeClamped,
			InsufficientSamples,
			SingularOperator
		};

		inline const char* HdrNoiseReasonText(HdrNoiseReason r)
		{
			switch (r)
			{
			case HdrNoiseReason::Ok: return "ok";
			case HdrNoiseReason::Disabled: return "hdr noise floor disabled";
			case HdrNoiseReason::InvalidInput: return "invalid input";
			case HdrNoiseReason::SdrPassthrough: return "sdr passthrough (no hdr transfer)";
			case HdrNoiseReason::DarkToeClamped: return "ok (dark-toe derivative clamped to 1-lsb slope)";
			case HdrNoiseReason::InsufficientSamples: return "insufficient samples or reference sizes";
			case HdrNoiseReason::SingularOperator: return "singular descale operator";
			default: return "unknown";
			}
		}

		struct HdrNoiseSettings
		{
			bool enabled = false;                 // Default OFF: preserves 100% W1 bit-exactness when disabled
			HdrTransfer transfer = HdrTransfer::Sdr;
			int bitDepth = 10;                    // Signal bit depth in [8, 16] (10-bit standard HDR, 12-bit mastering)
			bool limitedRange = true;             // ITU-R BT.2100 narrow range [64, 940] in 10-bit (vs full [0, 1023])
			bool linearizeForDescale = true;      // Linearize code-space lines via EOTF before descale verification
			double floorGuardDivisor = 4.0;       // Derived from ChainEdgeStats.h:114 (eps = 1 / 4 in whitened units)
			double trimFraction = 0.2;            // Derived from DescaleVerifier.h:87 (20% trimmed mean across lines)
			double refFraction1 = 0.04;           // Derived from DescaleVerifier.h:85 (reference sizes n * (1 +- 4%))
			double refFraction2 = 0.08;           // Derived from DescaleVerifier.h:86 (reference sizes n * (1 +- 8%))
		};

		// ====================================================================================================
		// 2. SMPTE ST 2084 (PQ) & ARIB STD-B67 (HLG) EOTF / OETF + Analytical Derivatives
		// ====================================================================================================

		// Exact rational constants from SMPTE ST 2084 / ITU-R BT.2100 Table 4.
		constexpr double kPqM1 = 2610.0 / 16384.0;
		constexpr double kPqM2 = (2523.0 / 4096.0) * 128.0;
		constexpr double kPqC1 = 3424.0 / 4096.0;
		constexpr double kPqC2 = (2413.0 / 4096.0) * 32.0;
		constexpr double kPqC3 = (2392.0 / 4096.0) * 32.0;

		// Exact ARIB STD-B67 / ITU-R BT.2100 Table 5 HLG constants (b = 1 - 4a; c = 0.5 - a*ln(4a)).
		constexpr double kHlgA = 0.17883277;
		constexpr double kHlgB = 1.0 - 4.0 * kHlgA;

		// Computes HLG offset c = 0.5 - a * ln(4a) at full IEEE-754 double precision so C0 and C1
		// continuity across the E' = 0.5 branch boundary holds to machine epsilon.
		inline double HlgC()
		{
			return 0.5 - kHlgA * std::log(4.0 * kHlgA);
		}

		// SMPTE ST 2084 EOTF: normalized nonlinear PQ signal E' in [0, 1] -> normalized linear luminance
		// Y in [0, 1] (where Y = 1.0 corresponds to 10000 cd/m^2).
		inline double PqEotf(double ep)
		{
			if (!std::isfinite(ep))
				return ep > 0.0 ? 1.0 : 0.0;
			const double e = ClampD(ep, 0.0, 1.0);
			if (e <= 0.0)
				return 0.0;
			if (e >= 1.0)
				return 1.0;
			const double p = std::pow(e, 1.0 / kPqM2);
			const double num = (std::max)(p - kPqC1, 0.0);
			const double den = kPqC2 - kPqC3 * p;
			if (!(num > 0.0) || !(den > 1e-12))
				return 0.0;
			return ClampD(std::pow(num / den, 1.0 / kPqM1), 0.0, 1.0);
		}

		// Inverse SMPTE ST 2084 (PQ OETF / EOTF^-1): normalized linear luminance Y in [0, 1] -> E' in [0, 1].
		inline double PqOetf(double y)
		{
			if (!std::isfinite(y))
				return y > 0.0 ? 1.0 : 0.0;
			const double yc = ClampD(y, 0.0, 1.0);
			if (yc <= 0.0)
				return 0.0;
			if (yc >= 1.0)
				return 1.0;
			const double ym1 = std::pow(yc, kPqM1);
			const double num = kPqC1 + kPqC2 * ym1;
			const double den = 1.0 + kPqC3 * ym1;
			if (!(den > 1e-12))
				return 0.0;
			return ClampD(std::pow(num / den, kPqM2), 0.0, 1.0);
		}

		// Exact analytical derivative dY / dE' of SMPTE ST 2084 EOTF.
		// Returns 0.0 at/below the dark-toe cutoff (E' <= 0 or p <= c1); callers apply the 1-LSB floor guard.
		inline double PqEotfDerivative(double ep)
		{
			if (!std::isfinite(ep))
				return 0.0;
			const double e = ClampD(ep, 0.0, 1.0);
			if (e <= 0.0)
				return 0.0;
			const double p = std::pow(e, 1.0 / kPqM2);
			const double num = (std::max)(p - kPqC1, 0.0);
			const double den = kPqC2 - kPqC3 * p;
			if (!(num > 0.0) || !(den > 1e-12))
				return 0.0;
			const double dNdD_dp = (kPqC2 - kPqC1 * kPqC3) / (den * den);
			const double dp_dep = (1.0 / kPqM2) * std::pow(e, 1.0 / kPqM2 - 1.0);
			const double dY_dep = (1.0 / kPqM1) * std::pow(num / den, 1.0 / kPqM1 - 1.0) * dNdD_dp * dp_dep;
			return (std::isfinite(dY_dep) && dY_dep > 0.0) ? dY_dep : 0.0;
		}

		// ARIB STD-B67 / ITU-R BT.2100 HLG inverse OETF: nonlinear signal E' in [0, 1] -> scene-linear E in [0, 1].
		inline double HlgInvOetf(double ep)
		{
			if (!std::isfinite(ep))
				return ep > 0.0 ? 1.0 : 0.0;
			const double e = ClampD(ep, 0.0, 1.0);
			if (e <= 0.5)
				return (e * e) / 3.0;
			const double c = HlgC();
			return ClampD((std::exp((e - c) / kHlgA) + kHlgB) / 12.0, 0.0, 1.0);
		}

		// ARIB STD-B67 / ITU-R BT.2100 HLG OETF: scene-linear E in [0, 1] -> nonlinear signal E' in [0, 1].
		inline double HlgOetf(double e)
		{
			if (!std::isfinite(e))
				return e > 0.0 ? 1.0 : 0.0;
			const double ec = ClampD(e, 0.0, 1.0);
			if (ec <= 0.0)
				return 0.0;
			if (ec <= 1.0 / 12.0)
				return std::sqrt(3.0 * ec);
			const double arg = 12.0 * ec - kHlgB;
			if (!(arg > 0.0))
				return 0.0;
			const double c = HlgC();
			return ClampD(kHlgA * std::log(arg) + c, 0.0, 1.0);
		}

		// Exact analytical derivative dE / dE' of ARIB STD-B67 HLG inverse OETF.
		// Continuous at E' = 0.5: both branches evaluate to 1/3.
		inline double HlgInvOetfDerivative(double ep)
		{
			if (!std::isfinite(ep))
				return 0.0;
			const double e = ClampD(ep, 0.0, 1.0);
			if (e <= 0.0)
				return 0.0;
			if (e <= 0.5)
				return (2.0 * e) / 3.0;
			const double c = HlgC();
			return std::exp((e - c) / kHlgA) / (12.0 * kHlgA);
		}

		// ====================================================================================================
		// 3. Code-Space <-> Normalized Signal & Heteroscedastic Local Quantization Variance
		// ====================================================================================================

		// Maps a 10-bit code value (LineSet units) to normalized nonlinear signal E' in [0, 1].
		// ITU-R BT.2100 narrow range: [64, 940] (span 876 = 219 * 4); full range: [0, 1023] (span 1023).
		inline double Code10ToNorm(double code10, bool limitedRange)
		{
			if (!std::isfinite(code10))
				return 0.0;
			if (limitedRange)
				return ClampD((code10 - 64.0) / 876.0, 0.0, 1.0);
			return ClampD(code10 / 1023.0, 0.0, 1.0);
		}

		// Maps normalized nonlinear signal E' in [0, 1] back to 10-bit code units (LineSet units).
		inline double NormToCode10(double ep, bool limitedRange)
		{
			if (!std::isfinite(ep))
				return limitedRange ? 64.0 : 0.0;
			const double e = ClampD(ep, 0.0, 1.0);
			if (limitedRange)
				return 64.0 + e * 876.0;
			return e * 1023.0;
		}

		// Normalized quantization step Delta_E' for a given signal bit depth in [8, 16].
		// Limited range: 1 / (219 * 2^(bd - 8)) (10-bit: 1/876; 12-bit: 1/3504, 4x finer).
		// Full range:    1 / (2^bd - 1)         (10-bit: 1/1023; 12-bit: 1/4095).
		inline double QuantStepNorm(int bitDepth, bool limitedRange)
		{
			const int bd = bitDepth < 8 ? 8 : (bitDepth > 16 ? 16 : bitDepth);
			if (limitedRange)
			{
				const double steps = 219.0 * static_cast<double>(1u << static_cast<unsigned>(bd - 8));
				return 1.0 / steps;
			}
			const double steps = static_cast<double>((1u << static_cast<unsigned>(bd)) - 1u);
			return 1.0 / steps;
		}

		// Local linear-light quantization noise SD: sigma_Y = (dY / dE') * (Delta_E' / sqrt(12)).
		// For SDR, returns the uniform normalized quantization SD Delta_E' / sqrt(12).
		// For PQ/HLG, clamps dY/dE' from below at the exact finite-difference slope of the first
		// quantization interval above black: derivMin = EOTF(Delta_E') / Delta_E' (Rule 1: zero invented
		// constants; Rule 4: sets *darkToeClamped = true whenever clamping occurs).
		inline double LocalQuantSigmaLinear(
			HdrTransfer transfer,
			double code10,
			int bitDepth,
			bool limitedRange,
			bool* darkToeClamped = nullptr)
		{
			if (darkToeClamped != nullptr)
				*darkToeClamped = false;
			const double step = QuantStepNorm(bitDepth, limitedRange);
			const double sigmaEp = step / std::sqrt(12.0);
			if (transfer == HdrTransfer::Sdr)
				return sigmaEp;
			if (transfer != HdrTransfer::Pq && transfer != HdrTransfer::Hlg)
				return 0.0;

			const double ep = Code10ToNorm(code10, limitedRange);
			const double derivRaw = (transfer == HdrTransfer::Pq)
				? PqEotfDerivative(ep)
				: HlgInvOetfDerivative(ep);
			const double firstStepLinear = (transfer == HdrTransfer::Pq)
				? PqEotf(step)
				: HlgInvOetf(step);
			const double derivMin = firstStepLinear / step;

			double deriv = derivRaw;
			if (!std::isfinite(deriv) || deriv < derivMin)
			{
				deriv = derivMin;
				if (darkToeClamped != nullptr)
					*darkToeClamped = true;
			}
			return deriv * sigmaEp;
		}

		// Dimensionless ratio of dY/dE' at `code10` relative to midtone E' = 0.5.
		// Values < 1 indicate dark-toe compression; values > 1 indicate highlight expansion; SDR returns 1.0.
		inline double PerceptualCodeStepRatio(HdrTransfer transfer, double code10, bool limitedRange)
		{
			if (transfer == HdrTransfer::Sdr)
				return 1.0;
			if (transfer != HdrTransfer::Pq && transfer != HdrTransfer::Hlg)
				return 0.0;
			const double ep = Code10ToNorm(code10, limitedRange);
			const double dCode = (transfer == HdrTransfer::Pq)
				? PqEotfDerivative(ep)
				: HlgInvOetfDerivative(ep);
			const double dMid = (transfer == HdrTransfer::Pq)
				? PqEotfDerivative(0.5)
				: HlgInvOetfDerivative(0.5);
			if (!(dMid > 0.0) || !std::isfinite(dCode))
				return 0.0;
			return dCode / dMid;
		}

		// ====================================================================================================
		// 4. Caller-Owned Workspace, Linearization & Heteroscedastic Gauss-Markov WLS Verification
		// ====================================================================================================

		// All mutable buffers live in this caller-owned workspace (Rule 2: zero static/thread_local caches).
		struct HdrNoiseWorkspace
		{
			LineSet linearLines;
			std::vector<double> localSigma;
			std::vector<double> workSolve;
			std::vector<double> refCols;
			std::vector<double> lineVals;

			// Bounded reusable tap memo (SOTA1-HIGH-2: zero per-call TapMemo heap allocations).
			TapMemo tapMemo{ 4096 };

			// Bounded WLS / diagnostic scratch buffers reused across calls without static state.
			std::vector<double> wlsTaps;
			std::vector<int> wlsCol;
			std::vector<double> wlsMat;
			std::vector<double> wlsChol;
			std::vector<double> wlsRhs;
			std::vector<double> wlsInvDiag;
			std::vector<double> baseLineW;
			std::vector<double> baseLineU;
			std::vector<double> refColsUnif;
			std::vector<double> lineValsUnif;
			std::vector<double> lineWhitenedRms;
			std::vector<double> lineSigmaRms;
			std::vector<double> refSegW;
			std::vector<double> candSegSumW;
			std::vector<double> bestLineVals;
			std::vector<double> bestLineRms;
			std::vector<double> bestSegSumW;
			std::vector<int> bestSegRowsW;

			void Reset()
			{
				linearLines.axis = 0;
				linearLines.axisLen = 0;
				linearLines.origin = 0;
				linearLines.length = 0;
				linearLines.count = 0;
				linearLines.data.clear();
				linearLines.cross.clear();
				localSigma.clear();
				workSolve.clear();
				refCols.clear();
				lineVals.clear();
				tapMemo = TapMemo(4096);
				wlsTaps.clear();
				wlsCol.clear();
				wlsMat.clear();
				wlsChol.clear();
				wlsRhs.clear();
				wlsInvDiag.clear();
				baseLineW.clear();
				baseLineU.clear();
				refColsUnif.clear();
				lineValsUnif.clear();
				lineWhitenedRms.clear();
				lineSigmaRms.clear();
				refSegW.clear();
				candSegSumW.clear();
				bestLineVals.clear();
				bestLineRms.clear();
				bestSegSumW.clear();
				bestSegRowsW.clear();
			}
		};

		// Converts every 10-bit code sample in `codeLs` to linear luminance Y via PqEotf / HlgInvOetf into
		// `ws.linearLines` and computes exact per-sample `ws.localSigma[j * L + i] = LocalQuantSigmaLinear(...)`.
		// SOTA1-HIGH-2: hoists quantization step, sigmaEp, and firstStepLinear (derivMin) once per LineSet
		// instead of recomputing EOTF(step) per pixel.
		inline bool LinearizeLineSet(
			const LineSet& codeLs,
			const HdrNoiseSettings& cfg,
			HdrNoiseWorkspace& ws,
			HdrNoiseReason& reason,
			int& darkToeClampedCount)
		{
			darkToeClampedCount = 0;
			if (!cfg.enabled)
			{
				reason = HdrNoiseReason::Disabled;
				return false;
			}
			if (cfg.transfer == HdrTransfer::Sdr)
			{
				reason = HdrNoiseReason::SdrPassthrough;
				return false;
			}
			if (cfg.transfer != HdrTransfer::Pq && cfg.transfer != HdrTransfer::Hlg)
			{
				reason = HdrNoiseReason::InvalidInput;
				return false;
			}
			if (codeLs.count <= 0 || codeLs.length <= 0 || codeLs.axisLen <= 0)
			{
				reason = HdrNoiseReason::InvalidInput;
				return false;
			}
			if (codeLs.data.size() / static_cast<size_t>(codeLs.length) < static_cast<size_t>(codeLs.count))
			{
				reason = HdrNoiseReason::InvalidInput;
				return false;
			}

			ws.linearLines.axis = codeLs.axis;
			ws.linearLines.axisLen = codeLs.axisLen;
			ws.linearLines.origin = codeLs.origin;
			ws.linearLines.Resize(codeLs.count, codeLs.length);
			if (codeLs.cross.size() >= static_cast<size_t>(codeLs.count))
			{
				for (int j = 0; j < codeLs.count; ++j)
					ws.linearLines.cross[static_cast<size_t>(j)] = codeLs.cross[static_cast<size_t>(j)];
			}
			const size_t lenSz = static_cast<size_t>(codeLs.length);
			ws.localSigma.assign(static_cast<size_t>(codeLs.count) * lenSz, 0.0);

			const bool isPq = (cfg.transfer == HdrTransfer::Pq);
			const double step = QuantStepNorm(cfg.bitDepth, cfg.limitedRange);
			const double sigmaEp = step / std::sqrt(12.0);
			const double firstStepLinear = isPq ? PqEotf(step) : HlgInvOetf(step);
			const double derivMin = firstStepLinear / step;
			const double hlgC = HlgC();

			for (int j = 0; j < codeLs.count; ++j)
			{
				const double* src = codeLs.Line(j);
				double* dst = ws.linearLines.Line(j);
				double* sigRow = ws.localSigma.data() + static_cast<size_t>(j) * lenSz;
				for (int i = 0; i < codeLs.length; ++i)
				{
					const double code = src[i];
					if (!std::isfinite(code))
					{
						reason = HdrNoiseReason::InvalidInput;
						return false;
					}
					const double ep = Code10ToNorm(code, cfg.limitedRange);
					double yVal = 0.0;
					double derivRaw = 0.0;
					if (isPq)
					{
						if (ep > 0.0)
						{
							const double p = std::pow(ep, 1.0 / kPqM2);
							const double num = (std::max)(p - kPqC1, 0.0);
							const double den = kPqC2 - kPqC3 * p;
							if (num > 0.0 && den > 1e-12)
							{
								const double ratio = num / den;
								const double powBase = std::pow(ratio, 1.0 / kPqM1 - 1.0);
								yVal = (ep >= 1.0) ? 1.0 : ClampD(powBase * ratio, 0.0, 1.0);
								const double dNdD_dp = (kPqC2 - kPqC1 * kPqC3) / (den * den);
								const double dp_dep = (1.0 / kPqM2) * (p / ep);
								const double dY = (1.0 / kPqM1) * powBase * dNdD_dp * dp_dep;
								derivRaw = (std::isfinite(dY) && dY > 0.0) ? dY : 0.0;
							}
							else if (ep >= 1.0)
							{
								yVal = 1.0;
							}
						}
					}
					else
					{
						if (ep <= 0.5)
						{
							yVal = (ep * ep) / 3.0;
							derivRaw = (ep > 0.0) ? ((2.0 * ep) / 3.0) : 0.0;
						}
						else
						{
							const double ex = std::exp((ep - hlgC) / kHlgA);
							yVal = ClampD((ex + kHlgB) / 12.0, 0.0, 1.0);
							derivRaw = ex / (12.0 * kHlgA);
						}
					}
					dst[i] = yVal;
					double deriv = derivRaw;
					if (!std::isfinite(deriv) || deriv < derivMin)
					{
						deriv = derivMin;
						++darkToeClampedCount;
					}
					sigRow[i] = deriv * sigmaEp;
				}
			}

			reason = darkToeClampedCount > 0 ? HdrNoiseReason::DarkToeClamped : HdrNoiseReason::Ok;
			return true;
		}

		struct HdrVerifyResult
		{
			bool valid = false;
			HdrNoiseReason reason = HdrNoiseReason::Disabled;
			int bestKernel = -1;
			double stat = 0.0;
			double whitenedRms = 0.0;
			double uniformLinearStat = 0.0;
			int darkToeClampedSamples = 0;
			int evaluatedLines = 0;
			// HIGH-4: per-line & per-256-px segment whitened Gauss-Markov WLS stats for ChainEvidence::Judge.
			int segCount = 0;
			std::vector<double> lineStat;          // per-line ln(levelW / baseLineW) at bestKernel
			std::vector<double> lineBaseWhitened;  // per-line whitened baseline level (dimensionless SNR units, 1.0 = floor)
			std::vector<double> lineRmsWhitened;   // per-line whitened RMS residual at bestKernel (SNR units)
			std::vector<double> segStat;           // evaluatedLines x segCount: 0.5 * ln(segLevelW / segBaseW)
			std::vector<double> segBaseWhitened;   // evaluatedLines x segCount: sqrt(segBaseW) (SNR units)
			std::vector<int> segRows;              // evaluatedLines x segCount: inner rows per segment
		};

		namespace Detail
		{
			struct WlsOperatorMeta
			{
				int taps = 0;
				int L = 0;
				int m = 0;
				int u = 0;
				int margin = 0;
				double dof = 1.0;
			};

			// Builds the exact same tap geometry U (centre-aligned, 2^-30 quantized position memo) as
			// KernelBank::DescaleOperator::Build, storing rows into caller-owned workspace vectors.
			inline bool BuildWlsTapGeometry(
				int N,
				int n,
				int kernel,
				int origin,
				int length,
				HdrNoiseWorkspace& ws,
				WlsOperatorMeta& meta)
			{
				if (N <= 0 || n <= 1 || n >= N || length <= 0)
					return false;
				const KernelInfo& ki = Kernel(kernel);
				const int r = ki.radius;
				const int taps = 2 * r;
				const int margin = OperatorMargin(N, n, 4); // identical inner rows across every kernel (KernelBank.h:288)
				if (length - 2 * margin < 32)
					return false;

				ws.wlsTaps.assign(static_cast<size_t>(length) * static_cast<size_t>(taps), 0.0);
				ws.wlsCol.assign(static_cast<size_t>(length), 0);

				int cmin = 0;
				int cmax = 0;
				const double ratio = static_cast<double>(n) / static_cast<double>(N);
				double tapScratch[8];
				for (int j = 0; j < length; ++j)
				{
					const double p = (static_cast<double>(origin + j) + 0.5) * ratio - 0.5;
					const double fl = std::floor(p);
					const int64_t key = static_cast<int64_t>(std::floor((p - fl) * 1073741824.0 + 0.5));
					double* row = &ws.wlsTaps[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const double* tw = ws.tapMemo.Get(kernel, key, tapScratch);
					for (int s = 0; s < taps; ++s)
						row[s] = tw[s];
					const int c = static_cast<int>(fl) - r + 1;
					ws.wlsCol[static_cast<size_t>(j)] = c;
					if (j == 0 || c < cmin)
						cmin = c;
					if (j == 0 || c > cmax)
						cmax = c;
				}
				const int m = cmax + taps - cmin;
				if (m <= 0)
					return false;
				for (int j = 0; j < length; ++j)
					ws.wlsCol[static_cast<size_t>(j)] -= cmin;

				meta.taps = taps;
				meta.L = length;
				meta.m = m;
				meta.u = taps - 1;
				meta.margin = margin;
				// Degrees of freedom: 1 - nodes/length, floored at 0.02 (DescaleVerifier.h:495-497).
				const double rawDof = 1.0 - static_cast<double>(m) / static_cast<double>(length);
				meta.dof = rawDof < 0.02 ? 0.02 : rawDof;
				return true;
			}

			// Banded Cholesky factorization of symmetric positive-definite m x m matrix with half-bandwidth u.
			// Storage matches KernelBank.h: chol[i * (u + 1) + d] = L(i, i - d), with 1 / L(i, i) on diagonal d = 0.
			inline bool FactorBandedCholesky(std::vector<double>& chol, int m, int u)
			{
				const size_t bw = static_cast<size_t>(u + 1);
				for (int j = 0; j < m; ++j)
				{
					double sumDiag = chol[static_cast<size_t>(j) * bw];
					const int k0 = j - u > 0 ? j - u : 0;
					for (int k = k0; k < j; ++k)
					{
						const double ljk = chol[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						sumDiag -= ljk * ljk;
					}
					if (!(sumDiag > 1e-300) || !std::isfinite(sumDiag))
						return false;
					const double inv = 1.0 / std::sqrt(sumDiag);
					chol[static_cast<size_t>(j) * bw] = inv;
					const int i1 = j + u < m - 1 ? j + u : m - 1;
					for (int i = j + 1; i <= i1; ++i)
					{
						double v = chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)];
						const int kk0 = i - u > 0 ? i - u : 0;
						for (int k = kk0; k < j; ++k)
						{
							v -= chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)]
								* chol[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						}
						chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)] = v * inv;
					}
				}
				return true;
			}

			// Solves the exact Gauss-Markov weighted least squares problem:
			//   min_z (y - U z)^T W (y - U z),  W = diag(1 / sigma_i^2)
			// using Jacobi diagonal equilibration D^{-1} (U^T W U) D^{-1} so dark-toe (W ~ 1e17) and
			// highlight (W ~ 1e5) nodes share unit diagonal conditioning before banded Cholesky.
			// Outputs the DOF-normalized whitened level `outLevel = sqrt((mean(z_i^2) + epsW^2) / dof)`
			// and `outRawRms = sqrt(mean(z_i^2) / dof)` over inner rows [margin, L - margin).
			inline bool SolveLineWls(
				const double* y,
				const double* sigma,
				const WlsOperatorMeta& meta,
				double epsW,
				HdrNoiseWorkspace& ws,
				double& outLevel,
				double& outRawRms,
				double* outSegSumW = nullptr,
				int* outSegRowsW = nullptr)
			{
				const int L = meta.L;
				const int m = meta.m;
				const int u = meta.u;
				const int taps = meta.taps;
				const size_t bw = static_cast<size_t>(u + 1);

				ws.wlsMat.assign(static_cast<size_t>(m) * bw, 0.0);
				ws.wlsRhs.assign(static_cast<size_t>(m), 0.0);

				for (int row = 0; row < L; ++row)
				{
					const double sig = sigma[row];
					if (!(sig > 0.0) || !std::isfinite(sig))
						return false;
					const double invSig = 1.0 / sig;
					const double wRow = invSig * invSig;
					if (!std::isfinite(wRow) || !(wRow > 0.0))
						return false;
					const double* tw = &ws.wlsTaps[static_cast<size_t>(row) * static_cast<size_t>(taps)];
					const int col0 = ws.wlsCol[static_cast<size_t>(row)];
					const double wy = wRow * y[row];
					for (int s0 = 0; s0 < taps; ++s0)
					{
						const double ws0 = tw[s0];
						ws.wlsRhs[static_cast<size_t>(col0 + s0)] += ws0 * wy;
						const double wRowS0 = wRow * ws0;
						for (int t0 = s0; t0 < taps; ++t0)
						{
							ws.wlsMat[static_cast<size_t>(col0 + t0) * bw + static_cast<size_t>(t0 - s0)]
								+= wRowS0 * tw[t0];
						}
					}
				}

				// Jacobi diagonal equilibration: A_tilde = D^{-1} A D^{-1} has exact unit diagonal.
				ws.wlsInvDiag.resize(static_cast<size_t>(m));
				for (int i = 0; i < m; ++i)
				{
					const double diag = ws.wlsMat[static_cast<size_t>(i) * bw];
					if (!(diag > 0.0) || !std::isfinite(diag))
						return false;
					ws.wlsInvDiag[static_cast<size_t>(i)] = 1.0 / std::sqrt(diag);
				}
				for (int i = 0; i < m; ++i)
				{
					const double di = ws.wlsInvDiag[static_cast<size_t>(i)];
					ws.wlsMat[static_cast<size_t>(i) * bw] = 1.0;
					const int dMax = i < u ? i : u;
					for (int d = 1; d <= dMax; ++d)
					{
						ws.wlsMat[static_cast<size_t>(i) * bw + static_cast<size_t>(d)]
							*= di * ws.wlsInvDiag[static_cast<size_t>(i - d)];
					}
					ws.wlsRhs[static_cast<size_t>(i)] *= di;
				}

				// Strictly numerical relative ridge schedule matching KernelBank.h:335-347 (1e-12, 1e-9, 1e-6).
				double ridge = 1e-12;
				bool factored = false;
				for (int attempt = 0; attempt < 3; ++attempt)
				{
					ws.wlsChol = ws.wlsMat;
					for (int i = 0; i < m; ++i)
						ws.wlsChol[static_cast<size_t>(i) * bw] += ridge;
					if (FactorBandedCholesky(ws.wlsChol, m, u))
					{
						factored = true;
						break;
					}
					ridge *= 1000.0;
				}
				if (!factored)
					return false;

				// Forward solve: L v = b_tilde.
				double* z = ws.wlsRhs.data();
				for (int i = 0; i < m; ++i)
				{
					double v = z[i];
					const int k0 = i - u > 0 ? i - u : 0;
					for (int k = k0; k < i; ++k)
						v -= ws.wlsChol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * z[k];
					z[i] = v * ws.wlsChol[static_cast<size_t>(i) * bw];
				}
				// Backward solve: L^T z_tilde = v.
				for (int i = m - 1; i >= 0; --i)
				{
					double v = z[i];
					const int k1 = i + u < m - 1 ? i + u : m - 1;
					for (int k = i + 1; k <= k1; ++k)
						v -= ws.wlsChol[static_cast<size_t>(k) * bw + static_cast<size_t>(k - i)] * z[k];
					z[i] = v * ws.wlsChol[static_cast<size_t>(i) * bw];
				}
				// Unscale latent solution: z = D^{-1} z_tilde.
				for (int i = 0; i < m; ++i)
				{
					z[i] *= ws.wlsInvDiag[static_cast<size_t>(i)];
					if (!std::isfinite(z[i]))
						return false;
				}

				// Exact whitened residual over inner rows [margin, L - margin).
				if (outSegSumW != nullptr && outSegRowsW != nullptr)
				{
					for (int s = 0; s < kMaxSegs; ++s)
					{
						outSegSumW[s] = 0.0;
						outSegRowsW[s] = 0;
					}
				}
				double rssW = 0.0;
				int innerRows = 0;
				for (int row = meta.margin; row < L - meta.margin; ++row)
				{
					const double* tw = &ws.wlsTaps[static_cast<size_t>(row) * static_cast<size_t>(taps)];
					const int col0 = ws.wlsCol[static_cast<size_t>(row)];
					double pred = 0.0;
					for (int s = 0; s < taps; ++s)
						pred += tw[s] * z[col0 + s];
					const double zErr = (y[row] - pred) / sigma[row];
					const double zErr2 = zErr * zErr;
					rssW += zErr2;
					++innerRows;
					if (outSegSumW != nullptr && outSegRowsW != nullptr)
					{
						int seg = row / kSegPx;
						if (seg >= kMaxSegs)
							seg = kMaxSegs - 1;
						outSegSumW[seg] += zErr2;
						++outSegRowsW[seg];
					}
				}
				if (innerRows <= 0 || !std::isfinite(rssW))
					return false;

				const double msW = rssW / static_cast<double>(innerRows);
				outRawRms = std::sqrt(msW / meta.dof);
				outLevel = std::sqrt((msW + epsW * epsW) / meta.dof);
				return std::isfinite(outLevel) && (outLevel > 0.0) && std::isfinite(outRawRms);
			}
		}

		// In-place Gauss-Markov WLS verifier (SOTA1-HIGH-1 & SOTA1-HIGH-2):
		//   - Populates caller-owned `res` in place (reusing vector capacity across calls);
		//   - Supports `alreadyLinearized = true` so callers like DescaleVerifier linearize `codeLs` ONCE per frame;
		//   - Supports `onlyKernel >= 0` (honoring VerifyOptions::onlyKernel and fast single-kernel integer screening);
		//   - Supports `computeUniformDiagnostic = false` to skip the discarded unweighted `unifOp` solves on hot paths.
		inline void VerifyLinearizedHeteroscedasticInPlace(
			const LineSet& codeLs,
			int candidateN,
			const HdrNoiseSettings& cfg,
			HdrNoiseWorkspace& ws,
			HdrVerifyResult& res,
			const int* lineIdx = nullptr,
			int lineCount = 0,
			bool computeUniformDiagnostic = true,
			int onlyKernel = -1,
			bool alreadyLinearized = false,
			int preClampedCount = 0)
		{
			res.valid = false;
			res.reason = HdrNoiseReason::Disabled;
			res.bestKernel = -1;
			res.stat = 0.0;
			res.whitenedRms = 0.0;
			res.uniformLinearStat = 0.0;
			res.darkToeClampedSamples = 0;
			res.evaluatedLines = 0;
			res.segCount = 0;
			res.lineStat.clear();
			res.lineBaseWhitened.clear();
			res.lineRmsWhitened.clear();
			res.segStat.clear();
			res.segBaseWhitened.clear();
			res.segRows.clear();

			if (!cfg.enabled)
			{
				res.reason = HdrNoiseReason::Disabled;
				return;
			}
			if (cfg.transfer == HdrTransfer::Sdr)
			{
				res.reason = HdrNoiseReason::SdrPassthrough;
				return;
			}
			if ((cfg.transfer != HdrTransfer::Pq && cfg.transfer != HdrTransfer::Hlg) ||
				!std::isfinite(cfg.floorGuardDivisor) || !(cfg.floorGuardDivisor > 0.0) ||
				!std::isfinite(cfg.trimFraction) || cfg.trimFraction < 0.0 || cfg.trimFraction >= 0.5 ||
				!std::isfinite(cfg.refFraction1) || !(cfg.refFraction1 > 0.0) || cfg.refFraction1 >= 0.5 ||
				!std::isfinite(cfg.refFraction2) || !(cfg.refFraction2 > 0.0) || cfg.refFraction2 >= 0.5)
			{
				res.reason = HdrNoiseReason::InvalidInput;
				return;
			}
			if (codeLs.count <= 0 || codeLs.length <= 0 || codeLs.axisLen <= 0 ||
				codeLs.origin < 0 || codeLs.length > codeLs.axisLen ||
				codeLs.origin > codeLs.axisLen - codeLs.length ||
				codeLs.data.size() / static_cast<size_t>(codeLs.length) < static_cast<size_t>(codeLs.count))
			{
				res.reason = HdrNoiseReason::InvalidInput;
				return;
			}

			// Latent bounds matching DescaleVerifier.h:89-90 (minLatent = 16, maxLatentRatio = 0.985).
			const int N = codeLs.axisLen;
			const int minLatent = 16;
			const int maxN = static_cast<int>(std::floor(0.985 * static_cast<double>(N)));
			if (candidateN < minLatent || candidateN > maxN || candidateN >= N)
			{
				res.reason = HdrNoiseReason::InvalidInput;
				return;
			}
			const int candMargin = OperatorMargin(N, candidateN, 4);
			if (codeLs.length - 2 * candMargin < 32)
			{
				res.reason = HdrNoiseReason::InsufficientSamples;
				return;
			}

			// Linearize into caller-owned workspace unless already linearized for this LineSet.
			int clampedCount = preClampedCount;
			if (!alreadyLinearized)
			{
				clampedCount = 0;
				if (cfg.linearizeForDescale)
				{
					HdrNoiseReason linReason = HdrNoiseReason::Ok;
					if (!LinearizeLineSet(codeLs, cfg, ws, linReason, clampedCount))
					{
						res.reason = linReason;
						return;
					}
				}
				else
				{
					ws.linearLines.axis = codeLs.axis;
					ws.linearLines.axisLen = codeLs.axisLen;
					ws.linearLines.origin = codeLs.origin;
					ws.linearLines.Resize(codeLs.count, codeLs.length);
					if (codeLs.cross.size() >= static_cast<size_t>(codeLs.count))
					{
						for (int j = 0; j < codeLs.count; ++j)
							ws.linearLines.cross[static_cast<size_t>(j)] = codeLs.cross[static_cast<size_t>(j)];
					}
					const double sigmaEp = QuantStepNorm(cfg.bitDepth, cfg.limitedRange) / std::sqrt(12.0);
					const size_t lenSz0 = static_cast<size_t>(codeLs.length);
					ws.localSigma.assign(static_cast<size_t>(codeLs.count) * lenSz0, sigmaEp);
					for (int j = 0; j < codeLs.count; ++j)
					{
						const double* src = codeLs.Line(j);
						double* dst = ws.linearLines.Line(j);
						for (int i = 0; i < codeLs.length; ++i)
						{
							if (!std::isfinite(src[i]))
							{
								res.reason = HdrNoiseReason::InvalidInput;
								return;
							}
							dst[i] = Code10ToNorm(src[i], cfg.limitedRange);
						}
					}
				}
			}

			// Build reference sizes n * (1 +- refFraction1/2) matching DescaleVerifier.h:543-556.
			int refs[4];
			int nref = 0;
			const double fr[2] = { cfg.refFraction1, cfg.refFraction2 };
			for (int a = 0; a < 2; ++a)
			{
				for (int sg = -1; sg <= 1; sg += 2)
				{
					const int nr = RoundToInt(static_cast<double>(candidateN) * (1.0 + static_cast<double>(sg) * fr[a]));
					if (nr < minLatent || nr > maxN || nr >= N || nr == candidateN)
						continue;
					if (ws.linearLines.length - 2 * OperatorMargin(N, nr, 4) < 32)
						continue;
					bool duplicate = false;
					for (int p = 0; p < nref; ++p)
					{
						if (refs[p] == nr)
						{
							duplicate = true;
							break;
						}
					}
					if (!duplicate)
						refs[nref++] = nr;
				}
			}
			if (nref < 2)
			{
				res.reason = HdrNoiseReason::InsufficientSamples;
				return;
			}

			const int totalLines = ws.linearLines.count;
			const int nl = (lineIdx != nullptr && lineCount > 0) ? lineCount : totalLines;
			if (nl <= 0)
			{
				res.reason = HdrNoiseReason::InvalidInput;
				return;
			}
			for (int j = 0; j < nl; ++j)
			{
				const int srcIdx = (lineIdx != nullptr && lineCount > 0) ? lineIdx[j] : j;
				if (srcIdx < 0 || srcIdx >= totalLines)
				{
					res.reason = HdrNoiseReason::InvalidInput;
					return;
				}
			}
			const int L = ws.linearLines.length;
			const size_t nlSz = static_cast<size_t>(nl);
			const size_t lenSz = static_cast<size_t>(L);
			const double epsW = 1.0 / cfg.floorGuardDivisor;
			const int segCount = (std::min)(kMaxSegs, (L + kSegPx - 1) / kSegPx);

			if (computeUniformDiagnostic)
			{
				// Per-line RMS linear quantization sigma (exact unweighted noise floor of line j, used to derive
				// epsU_j = sigmaRms_j / floorGuardDivisor so uniformLinearStat matches stat on homoscedastic lines).
				ws.lineSigmaRms.assign(nlSz, 0.0);
				for (int j = 0; j < nl; ++j)
				{
					const int srcIdx = (lineIdx != nullptr && lineCount > 0) ? lineIdx[j] : j;
					const double* sigRow = ws.localSigma.data() + static_cast<size_t>(srcIdx) * lenSz;
					double sumSig2 = 0.0;
					for (int i = 0; i < L; ++i)
						sumSig2 += sigRow[i] * sigRow[i];
					ws.lineSigmaRms[static_cast<size_t>(j)] = std::sqrt(sumSig2 / static_cast<double>(L));
				}
			}

			int evalKernels[kFamilyCount];
			int nEvalK = 0;
			if (onlyKernel >= 0 && onlyKernel < kKernelCount)
			{
				evalKernels[0] = onlyKernel;
				nEvalK = 1;
			}
			else
			{
				int nrep = 0;
				const int* reps = RepresentativeKernels(nrep);
				nEvalK = nrep;
				for (int q = 0; q < nrep; ++q)
					evalKernels[q] = reps[q];
			}

			// Reference baseline across nref reference sizes x evaluation kernels (lines + 256-px segments).
			ws.refCols.assign(nlSz * static_cast<size_t>(nref), 0.0);
			if (computeUniformDiagnostic)
				ws.refColsUnif.assign(nlSz * static_cast<size_t>(nref), 0.0);
			ws.refSegW.assign(static_cast<size_t>(nref) * nlSz * static_cast<size_t>(kMaxSegs), -1.0);

			double segSumTmp[kMaxSegs];
			int segRowsTmp[kMaxSegs];
			double segSumW[kMaxSegs];
			int segRowsW[kMaxSegs];
			DescaleOperator unifOp;

			for (int ri = 0; ri < nref; ++ri)
			{
				const int nr = refs[ri];
				for (int q = 0; q < nEvalK; ++q)
				{
					const int k = evalKernels[q];
					Detail::WlsOperatorMeta meta;
					if (!Detail::BuildWlsTapGeometry(N, nr, k, ws.linearLines.origin, L, ws, meta))
					{
						res.reason = HdrNoiseReason::InsufficientSamples;
						return;
					}
					double unifDof = 1.0;
					if (computeUniformDiagnostic)
					{
						if (!unifOp.Build(N, nr, k, 0.0, ws.linearLines.origin, L, &ws.tapMemo))
						{
							res.reason = HdrNoiseReason::SingularOperator;
							return;
						}
						const double unifRawDof = 1.0 - static_cast<double>(unifOp.Nodes()) / static_cast<double>(unifOp.Length());
						unifDof = unifRawDof < 0.02 ? 0.02 : unifRawDof;
					}

					for (int j = 0; j < nl; ++j)
					{
						const int srcIdx = (lineIdx != nullptr && lineCount > 0) ? lineIdx[j] : j;
						const double* yRow = ws.linearLines.Line(srcIdx);
						const double* sigRow = ws.localSigma.data() + static_cast<size_t>(srcIdx) * lenSz;
						double levelW = 0.0;
						double rawRmsW = 0.0;
						if (!Detail::SolveLineWls(yRow, sigRow, meta, epsW, ws, levelW, rawRmsW, segSumW, segRowsW))
						{
							res.reason = HdrNoiseReason::SingularOperator;
							return;
						}

						const size_t slot = static_cast<size_t>(j) * static_cast<size_t>(nref) + static_cast<size_t>(ri);
						if (q == 0 || levelW < ws.refCols[slot])
						{
							ws.refCols[slot] = levelW;
							const size_t segOff = (static_cast<size_t>(ri) * nlSz + static_cast<size_t>(j)) * static_cast<size_t>(kMaxSegs);
							for (int g = 0; g < segCount; ++g)
							{
								if (segRowsW[g] > 0)
									ws.refSegW[segOff + static_cast<size_t>(g)] = (segSumW[g] / static_cast<double>(segRowsW[g]) + epsW * epsW) / meta.dof;
							}
						}

						if (computeUniformDiagnostic)
						{
							double rssU = 0.0;
							int rowsU = 0;
							unifOp.Residual(yRow, ws.workSolve, rssU, rowsU, segSumTmp, segRowsTmp);
							if (rowsU <= 0 || !std::isfinite(rssU))
							{
								res.reason = HdrNoiseReason::SingularOperator;
								return;
							}
							const double epsU = ws.lineSigmaRms[static_cast<size_t>(j)] / cfg.floorGuardDivisor;
							const double msU = rssU / static_cast<double>(rowsU);
							const double levelU = std::sqrt((msU + epsU * epsU) / unifDof);
							if (!std::isfinite(levelU) || !(levelU > 0.0))
							{
								res.reason = HdrNoiseReason::SingularOperator;
								return;
							}
							if (q == 0 || levelU < ws.refColsUnif[slot])
								ws.refColsUnif[slot] = levelU;
						}
					}
				}
			}

			ws.baseLineW.assign(nlSz, 0.0);
			if (computeUniformDiagnostic)
				ws.baseLineU.assign(nlSz, 0.0);
			for (int j = 0; j < nl; ++j)
			{
				const size_t baseOff = static_cast<size_t>(j) * static_cast<size_t>(nref);
				ws.baseLineW[static_cast<size_t>(j)] = RobustStats::MedianOf(&ws.refCols[baseOff], nref);
				if (!(ws.baseLineW[static_cast<size_t>(j)] > 0.0))
				{
					res.reason = HdrNoiseReason::SingularOperator;
					return;
				}
				if (computeUniformDiagnostic)
				{
					ws.baseLineU[static_cast<size_t>(j)] = RobustStats::MedianOf(&ws.refColsUnif[baseOff], nref);
					if (!(ws.baseLineU[static_cast<size_t>(j)] > 0.0))
					{
						res.reason = HdrNoiseReason::SingularOperator;
						return;
					}
				}
			}

			// Evaluate candidateN across evaluation kernels against the fixed per-line baselines.
			ws.lineVals.assign(nlSz, 0.0);
			if (computeUniformDiagnostic)
				ws.lineValsUnif.assign(nlSz, 0.0);
			ws.lineWhitenedRms.assign(nlSz, 0.0);
			ws.candSegSumW.assign(nlSz * static_cast<size_t>(kMaxSegs), 0.0);
			ws.bestLineVals.assign(nlSz, 0.0);
			ws.bestLineRms.assign(nlSz, 0.0);
			ws.bestSegSumW.assign(nlSz * static_cast<size_t>(kMaxSegs), 0.0);
			ws.bestSegRowsW.assign(static_cast<size_t>(kMaxSegs), 0);

			bool haveBestW = false;
			int bestKernel = -1;
			double bestStatW = 0.0;
			double bestWhitenedRms = 0.0;
			double bestDofW = 1.0;
			bool haveBestU = false;
			double bestStatU = 0.0;

			for (int q = 0; q < nEvalK; ++q)
			{
				const int k = evalKernels[q];
				Detail::WlsOperatorMeta meta;
				if (!Detail::BuildWlsTapGeometry(N, candidateN, k, ws.linearLines.origin, L, ws, meta))
				{
					res.reason = HdrNoiseReason::InsufficientSamples;
					return;
				}
				double unifDof = 1.0;
				if (computeUniformDiagnostic)
				{
					if (!unifOp.Build(N, candidateN, k, 0.0, ws.linearLines.origin, L, &ws.tapMemo))
					{
						res.reason = HdrNoiseReason::SingularOperator;
						return;
					}
					const double unifRawDof = 1.0 - static_cast<double>(unifOp.Nodes()) / static_cast<double>(unifOp.Length());
					unifDof = unifRawDof < 0.02 ? 0.02 : unifRawDof;
				}

				int lastSegRowsW[kMaxSegs] = {};
				for (int j = 0; j < nl; ++j)
				{
					const int srcIdx = (lineIdx != nullptr && lineCount > 0) ? lineIdx[j] : j;
					const double* yRow = ws.linearLines.Line(srcIdx);
					const double* sigRow = ws.localSigma.data() + static_cast<size_t>(srcIdx) * lenSz;
					double levelW = 0.0;
					double rawRmsW = 0.0;
					if (!Detail::SolveLineWls(yRow, sigRow, meta, epsW, ws, levelW, rawRmsW, segSumW, segRowsW))
					{
						res.reason = HdrNoiseReason::SingularOperator;
						return;
					}
					ws.lineVals[static_cast<size_t>(j)] = std::log(levelW / ws.baseLineW[static_cast<size_t>(j)]);
					ws.lineWhitenedRms[static_cast<size_t>(j)] = rawRmsW;
					for (int g = 0; g < segCount; ++g)
					{
						ws.candSegSumW[static_cast<size_t>(j) * static_cast<size_t>(kMaxSegs) + static_cast<size_t>(g)] = segSumW[g];
						lastSegRowsW[g] = segRowsW[g];
					}

					if (computeUniformDiagnostic)
					{
						double rssU = 0.0;
						int rowsU = 0;
						unifOp.Residual(yRow, ws.workSolve, rssU, rowsU, segSumTmp, segRowsTmp);
						if (rowsU <= 0 || !std::isfinite(rssU))
						{
							res.reason = HdrNoiseReason::SingularOperator;
							return;
						}
						const double epsU = ws.lineSigmaRms[static_cast<size_t>(j)] / cfg.floorGuardDivisor;
						const double msU = rssU / static_cast<double>(rowsU);
						const double levelU = std::sqrt((msU + epsU * epsU) / unifDof);
						if (!std::isfinite(levelU) || !(levelU > 0.0))
						{
							res.reason = HdrNoiseReason::SingularOperator;
							return;
						}
						ws.lineValsUnif[static_cast<size_t>(j)] = std::log(levelU / ws.baseLineU[static_cast<size_t>(j)]);
					}
				}

				const double statW = RobustStats::TrimmedMeanOf(ws.lineVals.data(), nl, cfg.trimFraction);
				const double rmsW = RobustStats::TrimmedMeanOf(ws.lineWhitenedRms.data(), nl, cfg.trimFraction);
				if (!std::isfinite(statW) || !std::isfinite(rmsW))
				{
					res.reason = HdrNoiseReason::SingularOperator;
					return;
				}
				if (!haveBestW || statW < bestStatW)
				{
					haveBestW = true;
					bestStatW = statW;
					bestKernel = k;
					bestWhitenedRms = rmsW;
					bestDofW = meta.dof;
					ws.bestLineVals = ws.lineVals;
					ws.bestLineRms = ws.lineWhitenedRms;
					ws.bestSegSumW = ws.candSegSumW;
					for (int g = 0; g < segCount; ++g)
						ws.bestSegRowsW[static_cast<size_t>(g)] = lastSegRowsW[g];
				}
				if (computeUniformDiagnostic)
				{
					const double statU = RobustStats::TrimmedMeanOf(ws.lineValsUnif.data(), nl, cfg.trimFraction);
					if (!std::isfinite(statU))
					{
						res.reason = HdrNoiseReason::SingularOperator;
						return;
					}
					if (!haveBestU || statU < bestStatU)
					{
						haveBestU = true;
						bestStatU = statU;
					}
				}
			}

			res.valid = haveBestW;
			res.reason = clampedCount > 0 ? HdrNoiseReason::DarkToeClamped : HdrNoiseReason::Ok;
			res.bestKernel = bestKernel;
			res.stat = bestStatW;
			res.whitenedRms = bestWhitenedRms;
			res.uniformLinearStat = computeUniformDiagnostic ? bestStatU : 0.0;
			res.darkToeClampedSamples = clampedCount;
			res.evaluatedLines = nl;
			res.segCount = segCount;
			res.lineStat = ws.bestLineVals;
			res.lineBaseWhitened = ws.baseLineW;
			res.lineRmsWhitened = ws.bestLineRms;
			const size_t cells = nlSz * static_cast<size_t>(segCount);
			res.segStat.assign(cells, 0.0);
			res.segBaseWhitened.assign(cells, 0.0);
			res.segRows.assign(cells, 0);
			double refCol[4];
			for (int j = 0; j < nl; ++j)
			{
				for (int g = 0; g < segCount; ++g)
				{
					const size_t cell = static_cast<size_t>(j) * static_cast<size_t>(segCount) + static_cast<size_t>(g);
					const int sr = ws.bestSegRowsW[static_cast<size_t>(g)];
					res.segRows[cell] = sr;
					if (sr <= 0)
						continue;
					int nv = 0;
					for (int ri = 0; ri < nref; ++ri)
					{
						const double rv = ws.refSegW[(static_cast<size_t>(ri) * nlSz + static_cast<size_t>(j)) * static_cast<size_t>(kMaxSegs) + static_cast<size_t>(g)];
						if (rv > 0.0)
							refCol[nv++] = rv;
					}
					if (nv < 1)
						continue;
					const double baseMsW = RobustStats::MedianOf(refCol, nv);
					const double candSum = ws.bestSegSumW[static_cast<size_t>(j) * static_cast<size_t>(kMaxSegs) + static_cast<size_t>(g)];
					const double levelMsW = (candSum / static_cast<double>(sr) + epsW * epsW) / bestDofW;
					if (baseMsW > 0.0 && levelMsW > 0.0 && std::isfinite(baseMsW) && std::isfinite(levelMsW))
					{
						res.segStat[cell] = 0.5 * std::log(levelMsW / baseMsW);
						res.segBaseWhitened[cell] = std::sqrt(baseMsW);
					}
				}
			}
		}

		// Verifies a candidate latent size `candidateN` on HDR code-space lines using exact linearized
		// heteroscedastic Gauss-Markov WLS (noise-whitened residuals across dark toe, midtone, and highlights),
		// alongside the unweighted linear-MSE diagnostic `uniformLinearStat` on the exact same linearized lines.
		inline HdrVerifyResult VerifyLinearizedHeteroscedastic(
			const LineSet& codeLs,
			int candidateN,
			const HdrNoiseSettings& cfg,
			HdrNoiseWorkspace& ws,
			const int* lineIdx = nullptr,
			int lineCount = 0,
			bool computeUniformDiagnostic = true,
			int onlyKernel = -1)
		{
			HdrVerifyResult res;
			VerifyLinearizedHeteroscedasticInPlace(
				codeLs, candidateN, cfg, ws, res, lineIdx, lineCount,
				computeUniformDiagnostic, onlyKernel, false, 0);
			return res;
		}
	}
}

#pragma fp_contract(on)
