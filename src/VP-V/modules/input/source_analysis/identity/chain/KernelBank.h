// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// KernelBank (SID-C1 unit 3a): the player upscale kernels the verifier knows, and the banded operator that
// "descales" a picture line to a candidate latent size and re-upscales it.
//
// Kernels (taps = 2 * radius around the sample position): bilinear; bicubic B/C = (0, 0.5), (0, 0.6),
// (1/3, 1/3); lanczos 2, 3, 4; spline 16, 36, 64. Splines are the natural cubic spline through 2 * radius
// nodes evaluated in the middle interval (that is exactly the Avisynth/zimg SplineXX family; the tests pin
// spline16/36 against their closed forms).
//
// Mapping (centre aligned): the latent position of output pixel i of an N-pixel axis upscaled from n samples is
//   p_i = (i + 0.5) * n / N - 0.5 + phase,
// tap s (0 .. 2r-1) sits at latent node floor(p_i) - r + 1 + s and carries weight kernel(p_i - node),
// the weights normalised to sum 1.
//
// DescaleOperator: for the output rows [origin, origin + L) of a line it builds the banded normal matrix
// A = U^T U (U = the L x m upscale operator, m = the latent nodes touched), factors it (banded Cholesky, a tiny
// relative ridge for numerical safety), and for a line y solves A z = U^T y and returns the residual
// y - U z summed over the INNER rows only (a margin of about 1.5 kernel supports at each end is ignored, as the
// edge nodes are poorly constrained there), plus its per-256-px segment sums.
#include "ChainCommon.h"

namespace SourceIdentity
{
	namespace Chain
	{
		constexpr int kKernelCount = 10;
		constexpr int kFamilyCount = 4;

		enum class Family : int { Bilinear = 0, Bicubic = 1, Lanczos = 2, Spline = 3 };

		struct KernelInfo
		{
			const char* name;
			Family family;
			int radius;       // taps = 2 * radius
			int type;         // 0 bilinear, 1 B/C cubic, 2 lanczos, 3 natural spline
			double a;         // B (cubic) or lobes (lanczos)
			double b;         // C (cubic)
		};

		inline const KernelInfo& Kernel(int i)
		{
			static const KernelInfo table[kKernelCount] = {
				{ "bilinear", Family::Bilinear, 1, 0, 0.0, 0.0 },
				{ "bicubic_0_0.5", Family::Bicubic, 2, 1, 0.0, 0.5 },
				{ "bicubic_0_0.6", Family::Bicubic, 2, 1, 0.0, 0.6 },
				{ "bicubic_1/3_1/3", Family::Bicubic, 2, 1, 1.0 / 3.0, 1.0 / 3.0 },
				{ "lanczos2", Family::Lanczos, 2, 2, 2.0, 0.0 },
				{ "lanczos3", Family::Lanczos, 3, 2, 3.0, 0.0 },
				{ "lanczos4", Family::Lanczos, 4, 2, 4.0, 0.0 },
				{ "spline16", Family::Spline, 2, 3, 0.0, 0.0 },
				{ "spline36", Family::Spline, 3, 3, 0.0, 0.0 },
				{ "spline64", Family::Spline, 4, 3, 0.0, 0.0 },
			};
			return table[i < 0 ? 0 : (i >= kKernelCount ? kKernelCount - 1 : i)];
		}

		inline const char* FamilyName(Family f)
		{
			switch (f)
			{
			case Family::Bilinear: return "bilinear";
			case Family::Bicubic: return "bicubic";
			case Family::Lanczos: return "lanczos";
			case Family::Spline: return "spline";
			}
			return "unknown";
		}

		// One kernel per family used by the coarse scan and the baseline: bilinear, bicubic (0, 0.5), lanczos3, spline36.
		inline const int* RepresentativeKernels(int& count)
		{
			static const int reps[kFamilyCount] = { 0, 1, 5, 8 };
			count = kFamilyCount;
			return reps;
		}

		inline double Sinc(double x)
		{
			if (x == 0.0)
				return 1.0;
			const double px = kPi * x;
			return std::sin(px) / px;
		}

		inline double KernelValue(const KernelInfo& k, double x)
		{
			const double ax = x < 0.0 ? -x : x;
			switch (k.type)
			{
			case 0:
				return ax < 1.0 ? 1.0 - ax : 0.0;
			case 1:
			{
				const double B = k.a;
				const double C = k.b;
				if (ax < 1.0)
					return ((12.0 - 9.0 * B - 6.0 * C) * ax * ax * ax + (-18.0 + 12.0 * B + 6.0 * C) * ax * ax + (6.0 - 2.0 * B)) / 6.0;
				if (ax < 2.0)
					return ((-B - 6.0 * C) * ax * ax * ax + (6.0 * B + 30.0 * C) * ax * ax + (-12.0 * B - 48.0 * C) * ax + (8.0 * B + 24.0 * C)) / 6.0;
				return 0.0;
			}
			case 2:
				return ax < k.a ? Sinc(ax) * Sinc(ax / k.a) : 0.0;
			default:
				break;
			}
			return 0.0;
		}

		// Weights of the natural cubic spline through the 2r nodes 0 .. 2r-1 at position r-1 + t (0 <= t < 1):
		// w[s] is the value at that position of the spline that is 1 at node s and 0 at the other nodes.
		inline void NaturalSplineWeights(int r, double t, double* w)
		{
			const int n = 2 * r;           // 4, 6 or 8 nodes
			const int m = n - 2;           // interior second derivatives
			const int j = r - 1;
			for (int s = 0; s < n; ++s)
			{
				double y[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
				double M[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
				y[s] = 1.0;
				// Tridiagonal system M[i-1] + 4 M[i] + M[i+1] = 6 (y[i-1] - 2 y[i] + y[i+1]), i = 1 .. n-2, M[0] = M[n-1] = 0.
				double cp[8];
				double dp[8];
				for (int k = 0; k < m; ++k)
				{
					const int i = k + 1;
					const double d = 6.0 * (y[i - 1] - 2.0 * y[i] + y[i + 1]);
					if (k == 0)
					{
						cp[0] = 1.0 / 4.0;
						dp[0] = d / 4.0;
					}
					else
					{
						const double den = 4.0 - cp[k - 1];
						cp[k] = 1.0 / den;
						dp[k] = (d - dp[k - 1]) / den;
					}
				}
				double x[8];
				x[m - 1] = dp[m - 1];
				for (int k = m - 2; k >= 0; --k)
					x[k] = dp[k] - cp[k] * x[k + 1];
				for (int k = 0; k < m; ++k)
					M[k + 1] = x[k];
				w[s] = (1.0 - t) * y[j] + t * y[j + 1]
					- (t * (1.0 - t) / 6.0) * ((2.0 - t) * M[j] + (1.0 + t) * M[j + 1]);
			}
		}

		// Normalised tap weights (2 * radius values, sum 1) of kernel `kernel` at fractional latent position t = p - floor(p).
		inline void TapWeights(int kernel, double t, double* w)
		{
			const KernelInfo& k = Kernel(kernel);
			const int r = k.radius;
			const int taps = 2 * r;
			if (k.type == 3)
				NaturalSplineWeights(r, t, w);
			else
				for (int s = 0; s < taps; ++s)
					w[s] = KernelValue(k, t - static_cast<double>(s - (r - 1)));
			double sum = 0.0;
			for (int s = 0; s < taps; ++s)
				sum += w[s];
			if (sum > 1e-12 || sum < -1e-12)
				for (int s = 0; s < taps; ++s)
					w[s] /= sum;
		}

		// Rows ignored at each end of a window for latent size n of an N-pixel axis, for a kernel of radius r
		// (1.5 kernel supports). The operator always uses r = 4 so every kernel is judged on the same rows.
		inline int OperatorMargin(int N, int n, int r)
		{
			const double support = 2.0 * static_cast<double>(r) * static_cast<double>(N) / static_cast<double>(n);
			const int m = static_cast<int>(std::ceil(1.5 * support));
			return m < 8 ? 8 : m;
		}

		class DescaleOperator
		{
		public:
			bool Ok() const { return ok_; }
			int Length() const { return L_; }
			int Margin() const { return margin_; }
			int Nodes() const { return m_; }

			// Output rows [origin, origin + length) of an N-pixel axis, latent size n, kernel, sub-sample phase.
			bool Build(int N, int n, int kernel, double phase, int origin, int length)
			{
				ok_ = false;
				if (N <= 0 || n <= 1 || n >= N || length <= 0)
					return false;
				const KernelInfo& ki = Kernel(kernel);
				r_ = ki.radius;
				taps_ = 2 * r_;
				L_ = length;
				margin_ = OperatorMargin(N, n, 4);   // the same inner rows for every kernel, so kernels compare like for like
				if (L_ - 2 * margin_ < 32)
					return false;
				w_.assign(static_cast<size_t>(L_) * static_cast<size_t>(taps_), 0.0);
				cl_.assign(static_cast<size_t>(L_), 0);
				int cmin = 0;
				int cmax = 0;
				const double ratio = static_cast<double>(n) / static_cast<double>(N);
				for (int j = 0; j < L_; ++j)
				{
					const double p = (static_cast<double>(origin + j) + 0.5) * ratio - 0.5 + phase;
					const double fl = std::floor(p);
					TapWeights(kernel, p - fl, &w_[static_cast<size_t>(j) * static_cast<size_t>(taps_)]);
					const int c = static_cast<int>(fl) - r_ + 1;
					cl_[static_cast<size_t>(j)] = c;
					if (j == 0 || c < cmin)
						cmin = c;
					if (j == 0 || c > cmax)
						cmax = c;
				}
				m_ = cmax + taps_ - cmin;
				for (int j = 0; j < L_; ++j)
					cl_[static_cast<size_t>(j)] -= cmin;
				u_ = taps_ - 1;
				const size_t bw = static_cast<size_t>(u_ + 1);
				std::vector<double> A(static_cast<size_t>(m_) * bw, 0.0);   // A(i, i-d) at [i * bw + d]
				for (int j = 0; j < L_; ++j)
				{
					const double* w = &w_[static_cast<size_t>(j) * static_cast<size_t>(taps_)];
					const int c = cl_[static_cast<size_t>(j)];
					for (int s0 = 0; s0 < taps_; ++s0)
						for (int t0 = s0; t0 < taps_; ++t0)
							A[static_cast<size_t>(c + t0) * bw + static_cast<size_t>(t0 - s0)] += w[s0] * w[t0];
				}
				double maxDiag = 0.0;
				for (int i = 0; i < m_; ++i)
					if (A[static_cast<size_t>(i) * bw] > maxDiag)
						maxDiag = A[static_cast<size_t>(i) * bw];
				if (!(maxDiag > 0.0))
					return false;
				double ridge = 1e-12 * maxDiag;
				for (int attempt = 0; attempt < 5; ++attempt)
				{
					chol_ = A;
					for (int i = 0; i < m_; ++i)
						chol_[static_cast<size_t>(i) * bw] += ridge;
					if (Factor())
					{
						ok_ = true;
						return true;
					}
					ridge *= 1000.0;
				}
				return false;
			}

			// y: L samples of the window. Returns the residual sum of squares over the inner rows and their count;
			// segSum/segRows (kMaxSegs entries each, zeroed here) get the per-256-px segment sums.
			// scratch is a caller-owned work vector (reused, never shrunk).
			void Residual(const double* y, std::vector<double>& scratch, double& rss, int& rows, double* segSum, int* segRows) const
			{
				const size_t bw = static_cast<size_t>(u_ + 1);
				scratch.assign(static_cast<size_t>(m_), 0.0);
				double* z = scratch.data();
				for (int j = 0; j < L_; ++j)
				{
					const double* w = &w_[static_cast<size_t>(j) * static_cast<size_t>(taps_)];
					const int c = cl_[static_cast<size_t>(j)];
					const double yj = y[j];
					for (int s = 0; s < taps_; ++s)
						z[c + s] += w[s] * yj;
				}
				// Forward: L v = b.
				for (int i = 0; i < m_; ++i)
				{
					double v = z[i];
					const int k0 = i - u_ > 0 ? i - u_ : 0;
					for (int k = k0; k < i; ++k)
						v -= chol_[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * z[k];
					z[i] = v / chol_[static_cast<size_t>(i) * bw];
				}
				// Backward: L^T x = v.
				for (int i = m_ - 1; i >= 0; --i)
				{
					double v = z[i];
					const int k1 = i + u_ < m_ - 1 ? i + u_ : m_ - 1;
					for (int k = i + 1; k <= k1; ++k)
						v -= chol_[static_cast<size_t>(k) * bw + static_cast<size_t>(k - i)] * z[k];
					z[i] = v / chol_[static_cast<size_t>(i) * bw];
				}
				for (int s = 0; s < kMaxSegs; ++s)
				{
					segSum[s] = 0.0;
					segRows[s] = 0;
				}
				rss = 0.0;
				rows = 0;
				for (int j = margin_; j < L_ - margin_; ++j)
				{
					const double* w = &w_[static_cast<size_t>(j) * static_cast<size_t>(taps_)];
					const int c = cl_[static_cast<size_t>(j)];
					double pred = 0.0;
					for (int s = 0; s < taps_; ++s)
						pred += w[s] * z[c + s];
					const double e = y[j] - pred;
					const double e2 = e * e;
					rss += e2;
					++rows;
					int seg = j / kSegPx;
					if (seg >= kMaxSegs)
						seg = kMaxSegs - 1;
					segSum[seg] += e2;
					++segRows[seg];
				}
			}

		private:
			bool Factor()
			{
				const size_t bw = static_cast<size_t>(u_ + 1);
				for (int j = 0; j < m_; ++j)
				{
					double s = chol_[static_cast<size_t>(j) * bw];
					const int k0 = j - u_ > 0 ? j - u_ : 0;
					for (int k = k0; k < j; ++k)
					{
						const double l = chol_[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						s -= l * l;
					}
					if (!(s > 1e-300))
						return false;
					const double d = std::sqrt(s);
					chol_[static_cast<size_t>(j) * bw] = d;
					const int i1 = j + u_ < m_ - 1 ? j + u_ : m_ - 1;
					for (int i = j + 1; i <= i1; ++i)
					{
						double v = chol_[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)];
						const int kk0 = i - u_ > 0 ? i - u_ : 0;
						for (int k = kk0; k < j; ++k)
							v -= chol_[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * chol_[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						chol_[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)] = v / d;
					}
				}
				return true;
			}

			bool ok_ = false;
			int r_ = 0;
			int taps_ = 0;
			int L_ = 0;
			int m_ = 0;
			int u_ = 0;
			int margin_ = 0;
			std::vector<double> w_;      // L x taps
			std::vector<int> cl_;        // first latent node (relative) of every row
			std::vector<double> chol_;   // m x (u + 1) banded Cholesky factor, L(i, i-d) at [i * (u+1) + d]
		};
	}
}
