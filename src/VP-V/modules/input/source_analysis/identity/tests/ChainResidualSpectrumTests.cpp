/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team adversarial synthetic-physics unit tests for ChainResidualSpectrum (SID-C3, Stage S5).
// Pure CPU C++14, synthetic only, deterministic SplitMix64 PRNG, zero clocks/sleeps.
// Every separation/recovery invariant includes an in-test naive/unprotected negative control
// executed on the exact same synthetic input lines (Rule 3).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainResidualSpectrum.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace ResidualSpectrumFx
	{
		using namespace SourceIdentity::Chain;

		constexpr double kFloor10Bit = 0.28867513459481288; // 1.0 / sqrt(12.0)

		inline ResidualSpectrumSettings Enabled()
		{
			ResidualSpectrumSettings s;
			s.enabled = true;
			return s;
		}

		// Deterministic SplitMix64 PRNG.
		struct DetRng
		{
			uint64_t state;
			explicit DetRng(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			// Uniform in [-0.5, 0.5)
			double UniformCentered()
			{
				const double u = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				return u - 0.5;
			}

			// TPDF in [-1.0, 1.0), standard deviation = 1 / sqrt(6)
			double TpdfUnit()
			{
				return UniformCentered() + UniformCentered();
			}

			// Box-Muller N(0, 1)
			double Normal01()
			{
				double u1 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				const double u2 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				if (u1 < 1e-12)
					u1 = 1e-12;
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
			}
		};

		// Generates a sharp broadband latent 1D signal of length n (10-bit codes, centered around 512)
		// combining sharp step edges, impulses, and full-band latent texture without artificial periodicity.
		inline std::vector<double> MakeBroadbandLatent(int n, DetRng& rng)
		{
			std::vector<double> z(static_cast<size_t>(n), 512.0);
			double level = 512.0;
			int nextEdge = 17 + static_cast<int>(rng.NextU64() % 23);
			for (int i = 0; i < n; ++i)
			{
				if (i >= nextEdge)
				{
					level = 512.0 + 260.0 * rng.UniformCentered();
					nextEdge = i + 11 + static_cast<int>(rng.NextU64() % 29);
				}
				const double hf = 75.0 * rng.Normal01();
				const double v = level + hf;
				z[static_cast<size_t>(i)] = ClampD(v, 140.0, 884.0);
			}
			return z;
		}

		// Exact 1D upscale using KernelBank centre-aligned mapping: p_i = (i + 0.5) * n / N - 0.5 + phase.
		inline std::vector<double> Upscale1DBank(const std::vector<double>& src, int N, int kernel, double phase = 0.0)
		{
			const int n = static_cast<int>(src.size());
			const int r = Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> out(static_cast<size_t>(N), 0.0);
			double w[8] = {};
			for (int i = 0; i < N; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(n) / static_cast<double>(N) - 0.5 + phase;
				const double fl = std::floor(p);
				TapWeights(kernel, p - fl, w);
				double acc = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - r + 1 + s;
					if (c < 0) c = 0;
					else if (c >= n) c = n - 1;
					acc += w[s] * src[static_cast<size_t>(c)];
				}
				out[static_cast<size_t>(i)] = acc;
			}
			return out;
		}

		// Exact 1D upscale with an arbitrary symmetric kernel function not present in KernelBank.
		template <class KernelFn>
		inline std::vector<double> Upscale1DCustom(const std::vector<double>& src, int N, int halfSupport, KernelFn fn, double phase = 0.0)
		{
			const int n = static_cast<int>(src.size());
			const int taps = 2 * halfSupport;
			std::vector<double> out(static_cast<size_t>(N), 0.0);
			std::vector<double> w(static_cast<size_t>(taps), 0.0);
			for (int i = 0; i < N; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(n) / static_cast<double>(N) - 0.5 + phase;
				const double fl = std::floor(p);
				double sum = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					const int node = static_cast<int>(fl) - halfSupport + 1 + s;
					const double d = std::fabs(p - static_cast<double>(node));
					w[static_cast<size_t>(s)] = fn(d);
					sum += w[static_cast<size_t>(s)];
				}
				if (sum > 1e-12)
				{
					for (int s = 0; s < taps; ++s)
						w[static_cast<size_t>(s)] /= sum;
				}
				double acc = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - halfSupport + 1 + s;
					if (c < 0) c = 0;
					else if (c >= n) c = n - 1;
					acc += w[static_cast<size_t>(s)] * src[static_cast<size_t>(c)];
				}
				out[static_cast<size_t>(i)] = acc;
			}
			return out;
		}

		inline std::vector<double> Quantize10Bit(const std::vector<double>& x)
		{
			std::vector<double> q(x.size(), 0.0);
			for (size_t i = 0; i < x.size(); ++i)
				q[i] = ClampD(std::floor(x[i] + 0.5), 0.0, 1023.0);
			return q;
		}

		// Spatial 1D Gaussian blur G_sigma with exact normalized discrete weights (clamped edges).
		inline std::vector<double> GaussianBlur1D(const std::vector<double>& x, double sigma)
		{
			const int n = static_cast<int>(x.size());
			const int radius = static_cast<int>(std::ceil(4.0 * sigma));
			std::vector<double> g(static_cast<size_t>(2 * radius + 1), 0.0);
			double sum = 0.0;
			for (int k = -radius; k <= radius; ++k)
			{
				const double v = std::exp(-0.5 * (static_cast<double>(k) * static_cast<double>(k)) / (sigma * sigma));
				g[static_cast<size_t>(k + radius)] = v;
				sum += v;
			}
			for (size_t i = 0; i < g.size(); ++i)
				g[i] /= sum;

			std::vector<double> out(static_cast<size_t>(n), 0.0);
			for (int i = 0; i < n; ++i)
			{
				double acc = 0.0;
				for (int k = -radius; k <= radius; ++k)
				{
					int j = i + k;
					if (j < 0) j = 0;
					else if (j >= n) j = n - 1;
					acc += g[static_cast<size_t>(k + radius)] * x[static_cast<size_t>(j)];
				}
				out[static_cast<size_t>(i)] = acc;
			}
			return out;
		}

		// Real non-oracle per-pixel least-squares descale residual: e = y - U z_hat
		// where z_hat = argmin_z || y - U z ||_2^2 for candidate (n, kernel, phase).
		// Computes exact banded Cholesky independently so tests never pass an oracle residual.
		inline LineSet ComputeRealDescaleResiduals(const LineSet& ls, int candidateN, int kernel, double phase = 0.0)
		{
			LineSet res;
			res.axis = ls.axis;
			res.axisLen = ls.axisLen;
			res.origin = ls.origin;
			res.Resize(ls.count, ls.length);
			res.cross = ls.cross;

			const int N = ls.axisLen;
			const int L = ls.length;
			const int r = Kernel(kernel).radius;
			const int taps = 2 * r;
			const int margin = OperatorMargin(N, candidateN, 4);
			const double ratio = static_cast<double>(candidateN) / static_cast<double>(N);

			std::vector<double> W(static_cast<size_t>(L) * static_cast<size_t>(taps), 0.0);
			std::vector<int> cl(static_cast<size_t>(L), 0);
			int cmin = 0;
			int cmax = 0;
			for (int j = 0; j < L; ++j)
			{
				const double p = (static_cast<double>(ls.origin + j) + 0.5) * ratio - 0.5 + phase;
				const double fl = std::floor(p);
				TapWeights(kernel, p - fl, &W[static_cast<size_t>(j) * static_cast<size_t>(taps)]);
				const int c = static_cast<int>(fl) - r + 1;
				cl[static_cast<size_t>(j)] = c;
				if (j == 0 || c < cmin) cmin = c;
				if (j == 0 || c > cmax) cmax = c;
			}
			const int m = cmax + taps - cmin;
			for (int j = 0; j < L; ++j)
				cl[static_cast<size_t>(j)] -= cmin;

			const int u = taps - 1;
			const size_t bw = static_cast<size_t>(u + 1);
			std::vector<double> A(static_cast<size_t>(m) * bw, 0.0);
			for (int j = 0; j < L; ++j)
			{
				const double* w = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
				const int c = cl[static_cast<size_t>(j)];
				for (int s0 = 0; s0 < taps; ++s0)
					for (int t0 = s0; t0 < taps; ++t0)
						A[static_cast<size_t>(c + t0) * bw + static_cast<size_t>(t0 - s0)] += w[s0] * w[t0];
			}
			double maxDiag = 0.0;
			for (int i = 0; i < m; ++i)
				if (A[static_cast<size_t>(i) * bw] > maxDiag)
					maxDiag = A[static_cast<size_t>(i) * bw];
			const double ridge = 1e-12 * maxDiag;
			for (int i = 0; i < m; ++i)
				A[static_cast<size_t>(i) * bw] += ridge;

			// Banded Cholesky: L(i, i-d) stored at A[i*bw + d], reciprocal diagonal at d=0.
			for (int j = 0; j < m; ++j)
			{
				double s = A[static_cast<size_t>(j) * bw];
				const int k0 = j - u > 0 ? j - u : 0;
				for (int k = k0; k < j; ++k)
				{
					const double l = A[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
					s -= l * l;
				}
				const double inv = 1.0 / std::sqrt(s > 1e-300 ? s : 1e-300);
				A[static_cast<size_t>(j) * bw] = inv;
				const int i1 = j + u < m - 1 ? j + u : m - 1;
				for (int i = j + 1; i <= i1; ++i)
				{
					double v = A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)];
					const int kk0 = i - u > 0 ? i - u : 0;
					for (int k = kk0; k < j; ++k)
						v -= A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * A[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
					A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)] = v * inv;
				}
			}

			std::vector<double> z(static_cast<size_t>(m), 0.0);
			for (int li = 0; li < ls.count; ++li)
			{
				const double* y = ls.Line(li);
				double* e = res.Line(li);
				std::fill(z.begin(), z.end(), 0.0);
				for (int j = 0; j < L; ++j)
				{
					const double* w = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const int c = cl[static_cast<size_t>(j)];
					for (int s = 0; s < taps; ++s)
						z[static_cast<size_t>(c + s)] += w[s] * y[j];
				}
				for (int i = 0; i < m; ++i)
				{
					double v = z[static_cast<size_t>(i)];
					const int k0 = i - u > 0 ? i - u : 0;
					for (int k = k0; k < i; ++k)
						v -= A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * z[static_cast<size_t>(k)];
					z[static_cast<size_t>(i)] = v * A[static_cast<size_t>(i) * bw];
				}
				for (int i = m - 1; i >= 0; --i)
				{
					double v = z[static_cast<size_t>(i)];
					const int k1 = i + u < m - 1 ? i + u : m - 1;
					for (int k = i + 1; k <= k1; ++k)
						v -= A[static_cast<size_t>(k) * bw + static_cast<size_t>(k - i)] * z[static_cast<size_t>(k)];
					z[static_cast<size_t>(i)] = v * A[static_cast<size_t>(i) * bw];
				}
				for (int j = 0; j < L; ++j)
				{
					if (j < margin || j >= L - margin)
					{
						e[j] = 0.0;
						continue;
					}
					const double* w = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const int c = cl[static_cast<size_t>(j)];
					double pred = 0.0;
					for (int s = 0; s < taps; ++s)
						pred += w[s] * z[static_cast<size_t>(c + s)];
					e[j] = y[j] - pred;
				}
			}
			return res;
		}

		// Computes mean inner RMS across a residual LineSet (ignoring outer margin samples).
		inline double MeanInnerRms(const LineSet& ls, int margin)
		{
			double sumRms = 0.0;
			int validLines = 0;
			for (int li = 0; li < ls.count; ++li)
			{
				const double* y = ls.Line(li);
				double ss = 0.0;
				int rows = 0;
				for (int i = margin; i < ls.length - margin; ++i)
				{
					ss += y[i] * y[i];
					++rows;
				}
				if (rows > 0)
				{
					sumRms += std::sqrt(ss / static_cast<double>(rows));
					++validLines;
				}
			}
			return validLines > 0 ? sumRms / static_cast<double>(validLines) : 0.0;
		}

		// Real descale log-dip statistic for candidate n vs +/-8% reference sizes using DescaleOperator.
		inline double ComputeRealDescaleStat(const LineSet& ls, int n, int kernel)
		{
			const int N = ls.axisLen;
			DescaleOperator opCand;
			DescaleOperator opLo;
			DescaleOperator opHi;
			const int nLo = static_cast<int>(std::floor(0.92 * static_cast<double>(n) + 0.5));
			const int nHi = static_cast<int>(std::floor(1.08 * static_cast<double>(n) + 0.5));
			if (!opCand.Build(N, n, kernel, 0.0, ls.origin, ls.length) ||
				!opLo.Build(N, nLo, kernel, 0.0, ls.origin, ls.length) ||
				!opHi.Build(N, nHi, kernel, 0.0, ls.origin, ls.length))
			{
				return 0.0;
			}
			std::vector<double> scratch;
			double segSum[kMaxSegs];
			int segRows[kMaxSegs];
			double sumDip = 0.0;
			for (int li = 0; li < ls.count; ++li)
			{
				const double* y = ls.Line(li);
				double rssC = 0.0, rssL = 0.0, rssH = 0.0;
				int rowsC = 0, rowsL = 0, rowsH = 0;
				opCand.Residual(y, scratch, rssC, rowsC, segSum, segRows);
				opLo.Residual(y, scratch, rssL, rowsL, segSum, segRows);
				opHi.Residual(y, scratch, rssH, rowsH, segSum, segRows);
				const double dofC = 1.0 - static_cast<double>(opCand.Nodes()) / static_cast<double>(rowsC);
				const double dofL = 1.0 - static_cast<double>(opLo.Nodes()) / static_cast<double>(rowsL);
				const double dofH = 1.0 - static_cast<double>(opHi.Nodes()) / static_cast<double>(rowsH);
				const double sC = std::sqrt(rssC / (static_cast<double>(rowsC) * (dofC > 0.05 ? dofC : 0.05)) + 0.0004);
				const double sL = std::sqrt(rssL / (static_cast<double>(rowsL) * (dofL > 0.05 ? dofL : 0.05)) + 0.0004);
				const double sH = std::sqrt(rssH / (static_cast<double>(rowsH) * (dofH > 0.05 ? dofH : 0.05)) + 0.0004);
				const double base = 0.5 * (sL + sH);
				sumDip += std::log(sC / base);
			}
			return sumDip / static_cast<double>(ls.count);
		}

		// Naive un-floored energy-quantile bandwidth estimator (negative control: ignores floorCodes!).
		inline double NaiveUnflooredBandwidthQuantile(const LineSet& ls, double quantile)
		{
			// Average periodogram over Hann-windowed zero-mean lines using a power-of-two segment.
			int M = 1;
			while (M * 2 <= ls.length)
				M *= 2;
			const int half = M / 2;
			std::vector<double> psd(static_cast<size_t>(half + 1), 0.0);
			std::vector<double> win(static_cast<size_t>(M), 0.0);
			for (int i = 0; i < M; ++i)
				win[static_cast<size_t>(i)] = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(M - 1)));

			for (int li = 0; li < ls.count; ++li)
			{
				const double* y = ls.Line(li);
				double mean = 0.0;
				for (int i = 0; i < M; ++i)
					mean += y[i];
				mean /= static_cast<double>(M);

				// Evaluate coarse DFT bins (every 4th bin is plenty for flat-noise bandwidth check).
				for (int k = 1; k <= half; k += 4)
				{
					double re = 0.0;
					double im = 0.0;
					const double w0 = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(M);
					for (int i = 0; i < M; ++i)
					{
						const double xw = (y[i] - mean) * win[static_cast<size_t>(i)];
						re += xw * std::cos(w0 * static_cast<double>(i));
						im -= xw * std::sin(w0 * static_cast<double>(i));
					}
					psd[static_cast<size_t>(k)] += (re * re + im * im);
				}
			}
			double total = 0.0;
			for (int k = 1; k <= half; k += 4)
				total += psd[static_cast<size_t>(k)];
			if (!(total > 0.0))
				return 0.0;
			double cum = 0.0;
			for (int k = 1; k <= half; k += 4)
			{
				cum += psd[static_cast<size_t>(k)];
				if (cum >= quantile * total)
					return static_cast<double>(k) / static_cast<double>(M);
			}
			return 0.5;
		}

		// Independent DFT average power per bin in normalized frequency band [fMin, fMax] (cycles/px)
		// over a 256-sample Hann-windowed interior window of each line (zero reliance on production spectrum code).
		inline double FarStopbandPower(const LineSet& ls, double fMin, double fMax)
		{
			const int M = ls.length >= 256 ? 256 : ls.length;
			const int start = (ls.length - M) / 2;
			int kMin = static_cast<int>(std::ceil(fMin * static_cast<double>(M)));
			int kMax = static_cast<int>(std::floor(fMax * static_cast<double>(M)));
			if (kMin < 1) kMin = 1;
			if (kMax > M / 2) kMax = M / 2;
			if (kMax < kMin || ls.count <= 0 || M < 16)
				return 0.0;

			std::vector<double> win(static_cast<size_t>(M), 0.0);
			double winSqSum = 0.0;
			for (int i = 0; i < M; ++i)
			{
				const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(M - 1)));
				win[static_cast<size_t>(i)] = w;
				winSqSum += w * w;
			}
			double bandSum = 0.0;
			int bins = 0;
			for (int k = kMin; k <= kMax; ++k)
			{
				const double w0 = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(M);
				double binPower = 0.0;
				for (int li = 0; li < ls.count; ++li)
				{
					const double* y = ls.Line(li) + start;
					double mean = 0.0;
					for (int i = 0; i < M; ++i)
						mean += y[i];
					mean /= static_cast<double>(M);
					double re = 0.0;
					double im = 0.0;
					for (int i = 0; i < M; ++i)
					{
						const double xw = (y[i] - mean) * win[static_cast<size_t>(i)];
						re += xw * std::cos(w0 * static_cast<double>(i));
						im -= xw * std::sin(w0 * static_cast<double>(i));
					}
					binPower += (re * re + im * im) / (winSqSum * static_cast<double>(M));
				}
				bandSum += binPower / static_cast<double>(ls.count);
				++bins;
			}
			return bins > 0 ? bandSum / static_cast<double>(bins) : 0.0;
		}

		// Independent mean Pearson correlation across matching lines of two LineSets.
		inline double MeanLineCorrelation(const LineSet& a, const LineSet& b)
		{
			const int lines = a.count < b.count ? a.count : b.count;
			const int len = a.length < b.length ? a.length : b.length;
			if (lines <= 0 || len < 2)
				return 0.0;
			double acc = 0.0;
			for (int li = 0; li < lines; ++li)
			{
				const double* xa = a.Line(li);
				const double* xb = b.Line(li);
				double ma = 0.0, mb = 0.0;
				for (int i = 0; i < len; ++i)
				{
					ma += xa[i];
					mb += xb[i];
				}
				ma /= static_cast<double>(len);
				mb /= static_cast<double>(len);
				double cov = 0.0, va = 0.0, vb = 0.0;
				for (int i = 0; i < len; ++i)
				{
					const double da = xa[i] - ma;
					const double db = xb[i] - mb;
					cov += da * db;
					va += da * da;
					vb += db * db;
				}
				const double denom = std::sqrt(va * vb);
				acc += denom > 1e-12 ? (cov / denom) : 0.0;
			}
			return acc / static_cast<double>(lines);
		}
	}

	TEST_CLASS(ChainResidualSpectrumTests)
	{
	public:
		TEST_METHOD(DefaultOffDisabledAndInvalidInputsAbstainVisibly)
		{
			using namespace ResidualSpectrumFx;

			ResidualSpectrumSettings def;
			Assert::IsFalse(def.enabled);

			LineSet dummy;
			dummy.axisLen = 1024;
			dummy.Resize(8, 1024);

			const ResidualSpectrumResult rOff = ClassifyResidualSpectrum(dummy, dummy, 512, kFloor10Bit, def);
			Assert::IsFalse(rOff.valid);
			Assert::IsTrue(rOff.reason == ResidualSpectrumReason::Disabled);
			Assert::IsNotNull(ResidualClassName(rOff.classification));
			Assert::IsNotNull(ResidualSpectrumReasonText(rOff.reason));

			const EffectiveBandwidthResult bwOff = EstimateEffectiveBandwidth(dummy, kFloor10Bit, def);
			Assert::IsFalse(bwOff.valid);
			Assert::IsTrue(bwOff.reason == ResidualSpectrumReason::Disabled);

			std::vector<DoubleScaleCandidate> cands;
			DoubleScaleCandidate c1;
			c1.n = 720;
			c1.descaleStat = -1.2;
			c1.valid = true;
			cands.push_back(c1);
			const DoubleScaleResult dsOff = EvaluateDoubleScale(dummy, 1080, cands, kFloor10Bit, def);
			Assert::IsFalse(dsOff.valid);
			Assert::IsTrue(dsOff.reason == ResidualSpectrumReason::Disabled);

			const SeparableKernelFitResult kfOff = FitSeparableKernel(dummy, 512, 0.0, kFloor10Bit, def);
			Assert::IsFalse(kfOff.valid);
			Assert::IsTrue(kfOff.reason == ResidualSpectrumReason::Disabled);

			// Enabled with empty / invalid / flat constant lines must abstain visibly.
			const ResidualSpectrumSettings on = Enabled();
			LineSet empty;
			const ResidualSpectrumResult rEmpty = ClassifyResidualSpectrum(empty, empty, 512, kFloor10Bit, on);
			Assert::IsFalse(rEmpty.valid);
			Assert::IsTrue(rEmpty.reason == ResidualSpectrumReason::InvalidInput);

			const EffectiveBandwidthResult bwEmpty = EstimateEffectiveBandwidth(empty, kFloor10Bit, on);
			Assert::IsFalse(bwEmpty.valid);
			Assert::IsTrue(bwEmpty.reason == ResidualSpectrumReason::InvalidInput);

			const SeparableKernelFitResult kfEmpty = FitSeparableKernel(empty, 512, 0.0, kFloor10Bit, on);
			Assert::IsFalse(kfEmpty.valid);
			Assert::IsTrue(kfEmpty.reason == ResidualSpectrumReason::InvalidInput);

			// Constant flat signal lines (zero signal detail).
			LineSet flat;
			flat.axisLen = 1024;
			flat.Resize(16, 1024);
			for (int li = 0; li < flat.count; ++li)
			{
				double* y = flat.Line(li);
				for (int i = 0; i < flat.length; ++i)
					y[i] = 512.0;
			}
			LineSet zeroRes;
			zeroRes.axisLen = 1024;
			zeroRes.Resize(16, 1024);

			const ResidualSpectrumResult rFlat = ClassifyResidualSpectrum(flat, zeroRes, 512, kFloor10Bit, on);
			Assert::IsFalse(rFlat.valid);
			Assert::IsTrue(rFlat.classification == ResidualClass::Abstain &&
				rFlat.reason == ResidualSpectrumReason::LowSignalDetail);

			const EffectiveBandwidthResult bwFlat = EstimateEffectiveBandwidth(flat, kFloor10Bit, on);
			Assert::IsFalse(bwFlat.valid);
			Assert::IsTrue(bwFlat.reason == ResidualSpectrumReason::LowSignalDetail);
		}

		TEST_METHOD(SeparatesUsmSharpeningDitherAndNativeCodedStudioUpscale)
		{
			using namespace ResidualSpectrumFx;
			const ResidualSpectrumSettings on = Enabled();

			constexpr int kN = 1024;
			constexpr int kLatentN = 512;
			constexpr int kLines = 32;
			constexpr int kLanczos3 = 5;
			const int margin = OperatorMargin(kN, kLatentN, 4);

			DetRng rng(2026112301ull);

			// Build 4 real synthetic 1D LineSets from the SAME underlying sharp broadband latent signal.
			LineSet sigClean, sigUsm, sigDither, sigStudio;
			sigClean.axisLen = kN;  sigClean.Resize(kLines, kN);
			sigUsm.axisLen = kN;    sigUsm.Resize(kLines, kN);
			sigDither.axisLen = kN; sigDither.Resize(kLines, kN);
			sigStudio.axisLen = kN; sigStudio.Resize(kLines, kN);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> latent = MakeBroadbandLatent(kLatentN, rng);
				const std::vector<double> upClean = Upscale1DBank(latent, kN, kLanczos3, 0.0);

				// (a) Clean 2:1 Lanczos3 upscale quantized to 10-bit codes.
				const std::vector<double> qClean = Quantize10Bit(upClean);

				// (b) Exact same 2:1 upscale + real spatial Unsharp Mask: y_usm = y + 0.6 * (y - G_1.0 * y).
				const std::vector<double> blurred = GaussianBlur1D(upClean, 1.0);
				std::vector<double> usm(static_cast<size_t>(kN), 0.0);
				for (int i = 0; i < kN; ++i)
					usm[static_cast<size_t>(i)] = upClean[static_cast<size_t>(i)] + 0.6 * (upClean[static_cast<size_t>(i)] - blurred[static_cast<size_t>(i)]);
				const std::vector<double> qUsm = Quantize10Bit(usm);

				// (c) Exact same 2:1 upscale + TRUE 10-bit TPDF dither (1-LSB TPDF, sigma = sqrt(2/12) ~= 0.408 codes;
				//     with 10-bit rounding total white error variance = 3/12 = 0.25 -> sigma ~= 0.50 codes, resFar ~= 3.0 * floorP
				//     strictly inside (gateP = 2.25 * floorP, tpdfUpperP = 4.5 * floorP]).
				std::vector<double> ditheredRaw(static_cast<size_t>(kN), 0.0);
				for (int i = 0; i < kN; ++i)
					ditheredRaw[static_cast<size_t>(i)] = upClean[static_cast<size_t>(i)] + rng.TpdfUnit();
				const std::vector<double> dithered = Quantize10Bit(ditheredRaw);

				// (d) Studio fake 4K (C8): same 2:1 upscale + genuine coded 4K high-frequency texture & 8-px block structure above Nyquist.
				std::vector<double> studio(static_cast<size_t>(kN), 0.0);
				std::vector<double> blockOffsets(static_cast<size_t>(kN / 8 + 1), 0.0);
				for (size_t b = 0; b < blockOffsets.size(); ++b)
					blockOffsets[b] = 16.0 * rng.Normal01();
				const double ph1 = 2.0 * kPi * (rng.UniformCentered() + 0.5);
				const double ph2 = 2.0 * kPi * (rng.UniformCentered() + 0.5);
				for (int i = 0; i < kN; ++i)
				{
					// Coherent high-band 4K texture (0.34 and 0.42 cyc/px, well above 0.25 Nyquist) + independent 8-px codec block offsets.
					const double blockBias = blockOffsets[static_cast<size_t>(i / 8)];
					const double fine4k = 18.0 * std::sin(2.0 * kPi * 0.34 * static_cast<double>(i) + ph1)
						+ 14.0 * std::cos(2.0 * kPi * 0.42 * static_cast<double>(i) + ph2);
					studio[static_cast<size_t>(i)] = ClampD(std::floor(upClean[static_cast<size_t>(i)] + fine4k + blockBias + 0.5), 0.0, 1023.0);
				}

				std::copy(qClean.begin(), qClean.end(), sigClean.Line(li));
				std::copy(qUsm.begin(), qUsm.end(), sigUsm.Line(li));
				std::copy(dithered.begin(), dithered.end(), sigDither.Line(li));
				std::copy(studio.begin(), studio.end(), sigStudio.Line(li));
			}

			// Compute real non-oracle LS descale residuals for all 4 streams against candidate (n = 512, Lanczos3).
			const LineSet resClean = ComputeRealDescaleResiduals(sigClean, kLatentN, kLanczos3, 0.0);
			const LineSet resUsm = ComputeRealDescaleResiduals(sigUsm, kLatentN, kLanczos3, 0.0);
			const LineSet resDither = ComputeRealDescaleResiduals(sigDither, kLatentN, kLanczos3, 0.0);
			const LineSet resStudio = ComputeRealDescaleResiduals(sigStudio, kLatentN, kLanczos3, 0.0);

			// (a) Clean 2:1 Lanczos3 upscale -> CleanUpscale, neverRebase == false.
			const ResidualSpectrumResult outClean = ClassifyResidualSpectrum(sigClean, resClean, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outClean.valid);
			Assert::IsTrue(outClean.classification == ResidualClass::CleanUpscale);
			Assert::IsFalse(outClean.neverRebase);
			Assert::IsFalse(outClean.ToSharpenGeom().present);

			// (b) Unsharp Mask -> UsmSharpened, usmAmount > 0.2, usmRadiusPx in (0.3, 3.0), ToSharpenGeom().present == true.
			// MANDATORY Rule 3 Negative Control on EXACT same USM lines:
			// A naive total-RMS residual threshold (residRms <= 1.5 * floorCodes) FAILS because USM halos explode RMS > 3x floor!
			const double usmResidRms = MeanInnerRms(resUsm, margin);
			const double cleanResidRms = MeanInnerRms(resClean, margin);
			Assert::IsTrue(cleanResidRms <= 1.5 * kFloor10Bit);
			Assert::IsTrue(usmResidRms > 3.0 * kFloor10Bit); // Naive threshold FAILS on USM lines (hazard proven real!)

			const ResidualSpectrumResult outUsm = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outUsm.valid);
			Assert::IsTrue(outUsm.classification == ResidualClass::UsmSharpened);
			Assert::IsTrue(outUsm.usmAmount > 0.2);
			Assert::IsTrue(outUsm.usmRadiusPx > 0.3 && outUsm.usmRadiusPx < 3.0);
			const SourceIdentity::Geometry::SharpenGeom sg = outUsm.ToSharpenGeom();
			Assert::IsTrue(sg.present);
			Assert::IsTrue(sg.amount > 0.2f);
			Assert::IsTrue(sg.radius > 0.3f && sg.radius < 3.0f);
			Assert::IsTrue(sg.conf > 0.0f);

			// (c) True 10-bit TPDF dither (sigma ~= 0.41..0.50 codes) -> DitherDominated, neverRebase == false,
			//     highBandFlatness >= 0.5, NOT USM or NativeCodedStudioUpscale.
			const double floorP10 = kFloor10Bit * kFloor10Bit;
			const double gateP10 = 2.25 * floorP10;
			const double tpdfUpperP10 = 2.25 * 2.0 * floorP10;
			const double ditherResidRms = MeanInnerRms(resDither, margin);
			Assert::IsTrue(ditherResidRms > kFloor10Bit, L"fixture: true 10-bit TPDF dither exceeds uniform rounding floor");
			const ResidualSpectrumResult outDither = ClassifyResidualSpectrum(sigDither, resDither, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outDither.valid);
			Assert::IsTrue(outDither.highBandPower > gateP10 && outDither.highBandPower <= tpdfUpperP10,
				L"true 10-bit TPDF dither sits inside physical (gateP, tpdfUpperP] band");
			Assert::IsTrue(outDither.classification == ResidualClass::DitherDominated);
			Assert::IsFalse(outDither.neverRebase);
			Assert::IsTrue(outDither.highBandFlatness >= 0.5);
			Assert::IsFalse(outDither.classification == ResidualClass::UsmSharpened);
			Assert::IsFalse(outDither.classification == ResidualClass::NativeCodedStudioUpscale);

			// (d) Studio fake 4K (C8) -> NativeCodedStudioUpscale, codedRaster == NativeCoded, neverRebase == true (OD-4 C=(1)).
			// MANDATORY Rule 3 Negative Control on EXACT same studio 4K lines:
			// A naive low-band/total-signal correlation check sees > 90% correlation with the 2:1 upscaled base
			// (failing to notice the coded 4K high-band structure), while independent far-stopband DFT power
			// in [0.30, 0.48] cyc/px is > 5x higher than the clean 2:1 upscale (proving real coded 4K detail above Nyquist!).
			const double studioCleanCorr = MeanLineCorrelation(sigStudio, sigClean);
			const double cleanFarPower = FarStopbandPower(sigClean, 0.30, 0.48);
			const double studioFarPower = FarStopbandPower(sigStudio, 0.30, 0.48);
			Assert::IsTrue(studioCleanCorr > 0.90);
			Assert::IsTrue(studioFarPower > 5.0 * (cleanFarPower > 1e-9 ? cleanFarPower : 1e-9));

			const ResidualSpectrumResult outStudio = ClassifyResidualSpectrum(sigStudio, resStudio, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outStudio.valid);
			Assert::IsTrue(outStudio.classification == ResidualClass::NativeCodedStudioUpscale);
			Assert::IsTrue(outStudio.codedRaster == SourceIdentity::Geometry::CodedRaster::NativeCoded);
			Assert::IsTrue(outStudio.neverRebase);
		}

		// H4 Red-Team proving test: Real 4K / 2K-DI film grain (sigma ~= 2.0 codes at 10-bit, flat white far-band)
		// MUST classify as NativeCodedStudioUpscale with neverRebase == true and codedRaster == NativeCoded,
		// NEVER as DitherDominated! Contrasted against true 10-bit TPDF dither (sigma ~= 0.41..0.50 codes) on the
		// exact same base upscale, with a same-input negative control proving an unbounded resFar > gateP rule
		// without the physical TPDF upper bound (gateMult * 2.0 * floorP) WOULD misclassify this exact grain!
		TEST_METHOD(Real4kFilmGrainClassifiesNativeCodedNeverRebaseNotDither)
		{
			using namespace ResidualSpectrumFx;
			const ResidualSpectrumSettings on = Enabled();

			constexpr int kN = 1024;
			constexpr int kLatentN = 512;
			constexpr int kLines = 32;
			constexpr int kLanczos3 = 5;

			DetRng rng(2026112344ull);

			LineSet sigGrain, sigTrueTpdf;
			sigGrain.axisLen = kN;    sigGrain.Resize(kLines, kN);
			sigTrueTpdf.axisLen = kN; sigTrueTpdf.Resize(kLines, kN);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> latent = MakeBroadbandLatent(kLatentN, rng);
				const std::vector<double> upClean = Upscale1DBank(latent, kN, kLanczos3, 0.0);

				// Real 4K / 2K-DI film grain: sigma = 2.0 codes white Gaussian grain at 10-bit (~5x stronger than TPDF dither).
				std::vector<double> grainRaw(static_cast<size_t>(kN), 0.0);
				// True 10-bit TPDF dither: 1-LSB TPDF (sigma_tpdf = 1/sqrt(6) ~= 0.408 codes; quantized error sigma ~= 0.50 codes).
				std::vector<double> tpdfRaw(static_cast<size_t>(kN), 0.0);
				for (int i = 0; i < kN; ++i)
				{
					grainRaw[static_cast<size_t>(i)] = upClean[static_cast<size_t>(i)] + 2.0 * rng.Normal01();
					tpdfRaw[static_cast<size_t>(i)] = upClean[static_cast<size_t>(i)] + rng.TpdfUnit();
				}
				const std::vector<double> qGrain = Quantize10Bit(grainRaw);
				const std::vector<double> qTpdf = Quantize10Bit(tpdfRaw);
				std::copy(qGrain.begin(), qGrain.end(), sigGrain.Line(li));
				std::copy(qTpdf.begin(), qTpdf.end(), sigTrueTpdf.Line(li));
			}

			const LineSet resGrain = ComputeRealDescaleResiduals(sigGrain, kLatentN, kLanczos3, 0.0);
			const LineSet resTrueTpdf = ComputeRealDescaleResiduals(sigTrueTpdf, kLatentN, kLanczos3, 0.0);

			const double floorP = kFloor10Bit * kFloor10Bit;               // 1/12 ~= 0.08333
			const double gateMult = on.lineGateFactor * on.lineGateFactor; // 2.25
			const double gateP = gateMult * floorP;                        // 2.25 * floorP ~= 0.1875
			const double tpdfUpperP = gateMult * 2.0 * floorP;             // 4.50 * floorP ~= 0.3750

			const ResidualSpectrumResult outGrain = ClassifyResidualSpectrum(sigGrain, resGrain, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outGrain.valid, L"real 4K film grain produces a valid residual spectrum classification");
			Assert::IsTrue(outGrain.highBandFlatness >= 0.5, L"fixture: 4K film grain is genuinely flat white in the far stopband");
			Assert::IsTrue(outGrain.midBandPower < gateMult * outGrain.highBandPower, L"fixture: 4K film grain has no USM halo peak");

			// Mandatory Rule 3 Negative Control on EXACT SAME 4K film grain input:
			// An unbounded resFar > gateP rule (missing the physical TPDF upper bound gateMult * 2.0 * floorP)
			// satisfies all three old DitherDominated conditions and WOULD falsely label this grain as DitherDominated (neverRebase == false)!
			const bool naiveUnboundedDitherTrigger =
				(outGrain.highBandPower > gateP) &&
				(outGrain.highBandFlatness >= 0.5) &&
				(outGrain.midBandPower < gateMult * outGrain.highBandPower);
			Assert::IsTrue(naiveUnboundedDitherTrigger,
				L"negative control: unbounded resFar > gateP rule WOULD misclassify real 4K film grain as DitherDominated (neverRebase=false)");
			Assert::IsTrue(outGrain.highBandPower > tpdfUpperP,
				L"negative control: 2-code 4K film grain far-band power exceeds physical TPDF upper bound gateMult * 2.0 * floorP");
			Assert::IsTrue(outGrain.highBandPower > 5.0 * tpdfUpperP,
				L"fixture: 2-code film grain far-band power is far above 10-bit TPDF dither");

			// Protected H4 classifier MUST protect real 4K film grain with neverRebase == true and NativeCoded!
			Assert::IsTrue(outGrain.classification == ResidualClass::NativeCodedStudioUpscale,
				L"H4: real 4K / 2K-DI film grain must classify as NativeCodedStudioUpscale, not DitherDominated");
			Assert::IsTrue(outGrain.codedRaster == SourceIdentity::Geometry::CodedRaster::NativeCoded,
				L"H4: real 4K film grain must report codedRaster == NativeCoded");
			Assert::IsTrue(outGrain.neverRebase,
				L"H4: real 4K film grain must set neverRebase == true");

			// Contrast control on EXACT SAME base upscale: genuine 10-bit TPDF dither stays DitherDominated (neverRebase == false).
			const ResidualSpectrumResult outTpdf = ClassifyResidualSpectrum(sigTrueTpdf, resTrueTpdf, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outTpdf.valid);
			Assert::IsTrue(outTpdf.highBandPower > gateP && outTpdf.highBandPower <= tpdfUpperP,
				L"contrast control: true 10-bit TPDF dither sits inside (gateP, gateMult * 2.0 * floorP]");
			Assert::IsTrue(outTpdf.classification == ResidualClass::DitherDominated,
				L"contrast control: true 10-bit TPDF dither classifies as DitherDominated");
			Assert::IsFalse(outTpdf.neverRebase,
				L"contrast control: true 10-bit TPDF dither allows re-basing (neverRebase == false)");

			// NEW-LOW-2: 2K-DI studio upscale with BOTH 4K film grain (> physical TPDF bound) AND strong USM halo
			// (resHalo >= gateMult * resFar) MUST STILL classify as NativeCodedStudioUpscale with neverRebase == true
			// (never downgraded to UsmSharpened)!
			LineSet sigGrainUsm;
			sigGrainUsm.axisLen = kN;
			sigGrainUsm.Resize(kLines, kN);
			DetRng rngUsm(2026112355ull);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> latent = MakeBroadbandLatent(kLatentN, rngUsm);
				const std::vector<double> upClean = Upscale1DBank(latent, kN, kLanczos3, 0.0);
				const std::vector<double> blurred = GaussianBlur1D(upClean, 1.0);
				std::vector<double> grainUsmRaw(static_cast<size_t>(kN), 0.0);
				for (int i = 0; i < kN; ++i)
				{
					// Strong USM halo (amount = 2.5, sigma = 1.0) + 1.0-code 4K white film grain (~2.9x above tpdfUpperP = 0.375):
					const double usmVal = upClean[static_cast<size_t>(i)] + 2.5 * (upClean[static_cast<size_t>(i)] - blurred[static_cast<size_t>(i)]);
					grainUsmRaw[static_cast<size_t>(i)] = usmVal + 1.0 * rngUsm.Normal01();
				}
				const std::vector<double> qGrainUsm = Quantize10Bit(grainUsmRaw);
				std::copy(qGrainUsm.begin(), qGrainUsm.end(), sigGrainUsm.Line(li));
			}
			const LineSet resGrainUsm = ComputeRealDescaleResiduals(sigGrainUsm, kLatentN, kLanczos3, 0.0);
			const ResidualSpectrumResult outGrainUsm = ClassifyResidualSpectrum(sigGrainUsm, resGrainUsm, kLatentN, kFloor10Bit, on);
			Assert::IsTrue(outGrainUsm.valid);
			Assert::IsTrue(outGrainUsm.highBandPower > tpdfUpperP,
				L"NEW-LOW-2 fixture: 1-code grain+USM far-band exceeds physical TPDF bound");
			Assert::IsTrue(outGrainUsm.highBandFlatness >= 0.5,
				L"NEW-LOW-2 fixture: 1-code grain+USM far-band is flat white grain");
			Assert::IsTrue(outGrainUsm.midBandPower >= gateMult * outGrainUsm.highBandPower,
				L"NEW-LOW-2 negative control: USM halo satisfies resHalo >= gateMult * resFar (would fall through to UsmSharpened if guarded on resHalo < gateMult * resFar)");
			Assert::IsTrue(outGrainUsm.classification == ResidualClass::NativeCodedStudioUpscale,
				L"NEW-LOW-2: 2K-DI studio upscale with 4K grain + USM halo must stay NativeCodedStudioUpscale");
			Assert::IsTrue(outGrainUsm.codedRaster == SourceIdentity::Geometry::CodedRaster::NativeCoded);
			Assert::IsTrue(outGrainUsm.neverRebase,
				L"NEW-LOW-2: 2K-DI studio upscale with 4K grain + USM halo must set neverRebase == true");
		}

		TEST_METHOD(DoubleScaleReportsLastStageAndSurfacesFirstStageSignal)
		{
			using namespace ResidualSpectrumFx;
			const ResidualSpectrumSettings on = Enabled();

			constexpr int kN = 2160;
			constexpr int kN1 = 720;
			constexpr int kN2 = 1080;
			constexpr int kLines = 32;
			constexpr int kLanczos3 = 5;

			DetRng rng(2026112302ull);

			// Real two-stage upscaled signal: 720 -> 1080 -> 2160 (Lanczos3 both stages).
			LineSet lsDouble;
			lsDouble.axisLen = kN;
			lsDouble.Resize(kLines, kN);

			// Real single-stage upscaled signal: 1080 -> 2160 (Lanczos3).
			LineSet lsSingle;
			lsSingle.axisLen = kN;
			lsSingle.Resize(kLines, kN);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> lat720 = MakeBroadbandLatent(kN1, rng);
				const std::vector<double> mid1080 = Upscale1DBank(lat720, kN2, kLanczos3, 0.0);
				const std::vector<double> outDouble = Quantize10Bit(Upscale1DBank(mid1080, kN, kLanczos3, 0.0));
				std::copy(outDouble.begin(), outDouble.end(), lsDouble.Line(li));

				const std::vector<double> lat1080 = MakeBroadbandLatent(kN2, rng);
				const std::vector<double> outSingle = Quantize10Bit(Upscale1DBank(lat1080, kN, kLanczos3, 0.0));
				std::copy(outSingle.begin(), outSingle.end(), lsSingle.Line(li));
			}

			// Compute real non-oracle DescaleOperator statistics for candidate set {540, 720, 1080}.
			auto makeCandidates = [&](const LineSet& ls) {
				std::vector<DoubleScaleCandidate> v;
				const int sizes[3] = { 540, 720, 1080 };
				for (int i = 0; i < 3; ++i)
				{
					DoubleScaleCandidate c;
					c.n = sizes[i];
					c.descaleStat = ComputeRealDescaleStat(ls, sizes[i], kLanczos3);
					c.valid = true;
					v.push_back(c);
				}
				return v;
			};

			const std::vector<DoubleScaleCandidate> candsDouble = makeCandidates(lsDouble);
			const std::vector<DoubleScaleCandidate> candsSingle = makeCandidates(lsSingle);

			// Sanity check on the real physics: on 720->1080->2160, BOTH 1080 and 720 dip strongly,
			// whereas a naive single-stage reporter keeping only lastStageN == 1080 loses the 720 bottleneck!
			Assert::IsTrue(candsDouble[1].descaleStat < -0.5); // 720 dips strongly on double-scale
			Assert::IsTrue(candsDouble[2].descaleStat < -0.5); // 1080 dips strongly on double-scale

			const DoubleScaleResult rDouble = EvaluateDoubleScale(lsDouble, kN2, candsDouble, kFloor10Bit, on);
			Assert::IsTrue(rDouble.valid);
			Assert::IsTrue(rDouble.doubleScalePresent);
			Assert::AreEqual(kN2, rDouble.reportedLastStageN); // OD-4 E=(1): reports last stage (1080)
			Assert::AreEqual(kN1, rDouble.firstStageN);        // Surfaces first-stage bottleneck (720)
			Assert::IsTrue(rDouble.bandDropDecades >= 1.0);
			Assert::IsTrue(rDouble.reason == ResidualSpectrumReason::DoubleScaleConfirmed);

			const SourceIdentity::Geometry::DoubleGeom dg = rDouble.ToDoubleGeom();
			Assert::IsTrue(dg.present);
			Assert::AreEqual(kN1, dg.n1);
			Assert::IsTrue(dg.conf > 0.0f);

			// MANDATORY Rule 3 Negative Control on EXACT same single-stage 1080 -> 2160 signal:
			// Must NOT falsely call double-scale (even when 720 is present in candidates).
			const DoubleScaleResult rSingle = EvaluateDoubleScale(lsSingle, kN2, candsSingle, kFloor10Bit, on);
			Assert::IsTrue(rSingle.valid);
			Assert::IsFalse(rSingle.doubleScalePresent);
			Assert::AreEqual(kN2, rSingle.reportedLastStageN);
			Assert::AreEqual(0, rSingle.firstStageN);
			Assert::IsTrue(rSingle.reason == ResidualSpectrumReason::SingleStageOnly);
			Assert::IsFalse(rSingle.ToDoubleGeom().present);

			// Adversarial negative control: even if a spoofed negative descaleStat at 720 is handed to
			// EvaluateDoubleScale on the single-stage 1080->2160 lines, the actual spectral energy in [720, 1080]
			// proves no band drop occurred -> must still reject double-scale!
			std::vector<DoubleScaleCandidate> spoofedCands = candsSingle;
			spoofedCands[1].descaleStat = -1.5;
			const DoubleScaleResult rSpoofed = EvaluateDoubleScale(lsSingle, kN2, spoofedCands, kFloor10Bit, on);
			Assert::IsFalse(rSpoofed.doubleScalePresent);
			Assert::AreEqual(0, rSpoofed.firstStageN);
		}

		TEST_METHOD(EffectiveBandwidthTracksTrueCutoffAndAbstainsOnFlatNoise)
		{
			using namespace ResidualSpectrumFx;
			const ResidualSpectrumSettings on = Enabled();

			constexpr int kN = 1024;
			constexpr int kLines = 32;
			constexpr int kLanczos3 = 5;

			DetRng rng(2026112303ull);

			LineSet lsNative, ls2x, ls3x, lsNoise;
			lsNative.axisLen = kN; lsNative.Resize(kLines, kN);
			ls2x.axisLen = kN;     ls2x.Resize(kLines, kN);
			ls3x.axisLen = kN;     ls3x.Resize(kLines, kN);
			lsNoise.axisLen = kN;  lsNoise.Resize(kLines, kN);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> nat = Quantize10Bit(MakeBroadbandLatent(kN, rng));
				const std::vector<double> up2 = Quantize10Bit(Upscale1DBank(MakeBroadbandLatent(kN / 2, rng), kN, kLanczos3, 0.0));
				const std::vector<double> up3 = Quantize10Bit(Upscale1DBank(MakeBroadbandLatent(kN / 3, rng), kN, kLanczos3, 0.0));

				std::copy(nat.begin(), nat.end(), lsNative.Line(li));
				std::copy(up2.begin(), up2.end(), ls2x.Line(li));
				std::copy(up3.begin(), up3.end(), ls3x.Line(li));

				// Pure white quantization-level noise at floorCodes = 1/sqrt(12) ~ 0.289 (zero real picture detail).
				double* yn = lsNoise.Line(li);
				for (int i = 0; i < kN; ++i)
					yn[i] = 512.0 + rng.UniformCentered();
			}

			const EffectiveBandwidthResult bwNat = EstimateEffectiveBandwidth(lsNative, kFloor10Bit, on);
			const EffectiveBandwidthResult bw2 = EstimateEffectiveBandwidth(ls2x, kFloor10Bit, on);
			const EffectiveBandwidthResult bw3 = EstimateEffectiveBandwidth(ls3x, kFloor10Bit, on);

			Assert::IsTrue(bwNat.valid);
			Assert::IsTrue(bw2.valid);
			Assert::IsTrue(bw3.valid);

			Assert::IsTrue(bwNat.cyclesPerPixel > 0.35);
			Assert::IsTrue(bw2.cyclesPerPixel > 0.18 && bw2.cyclesPerPixel < 0.27);
			Assert::IsTrue(bw3.cyclesPerPixel > 0.11 && bw3.cyclesPerPixel < 0.19);
			Assert::IsTrue(bwNat.cyclesPerPixel > bw2.cyclesPerPixel);
			Assert::IsTrue(bw2.cyclesPerPixel > bw3.cyclesPerPixel);

			// MANDATORY Rule 3 Negative Control on EXACT same pure white floor-noise lines:
			// Naive un-floored 95% cumulative PSD reports ~0.475 cyc/px (falsely claiming full native bandwidth!),
			// proving the floor subtraction / LowSignalDetail abstention guard is non-vacuous.
			const double naiveNoiseBw = NaiveUnflooredBandwidthQuantile(lsNoise, on.bandwidthEnergyQuantile);
			Assert::IsTrue(naiveNoiseBw > 0.40);

			const EffectiveBandwidthResult bwNoise = EstimateEffectiveBandwidth(lsNoise, kFloor10Bit, on);
			Assert::IsFalse(bwNoise.valid);
			Assert::IsTrue(bwNoise.reason == ResidualSpectrumReason::LowSignalDetail);
		}

		TEST_METHOD(SeparableKernelFitRecoversUnknownCustomKernelOverNaiveBilinear)
		{
			using namespace ResidualSpectrumFx;
			const ResidualSpectrumSettings on = Enabled();

			constexpr int kN = 1024;
			constexpr int kLatentN = 512;
			constexpr int kLines = 32;
			static constexpr int kHalfSupport = 3;
			static constexpr double kGaussianSigma = 0.85;

			auto customGaussian = [](double absDist) -> double {
				if (absDist >= static_cast<double>(kHalfSupport))
					return 0.0;
				return std::exp(-0.5 * (absDist * absDist) / (kGaussianSigma * kGaussianSigma));
			};

			DetRng rng(2026112304ull);
			LineSet ls;
			ls.axisLen = kN;
			ls.Resize(kLines, kN);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> latent = MakeBroadbandLatent(kLatentN, rng);
				const std::vector<double> up = Quantize10Bit(Upscale1DCustom(latent, kN, kHalfSupport, customGaussian, 0.0));
				std::copy(up.begin(), up.end(), ls.Line(li));
			}

			// MANDATORY Rule 3 Negative Control on EXACT same custom-Gaussian upscaled lines:
			// Fixed Bilinear (kernel 0) fails with large residual RMS against this unknown smooth kernel!
			DescaleOperator opBilinear;
			Assert::IsTrue(opBilinear.Build(kN, kLatentN, 0, 0.0, 0, kN));
			std::vector<double> scratch;
			double segSum[kMaxSegs];
			int segRows[kMaxSegs];
			double bilinearRmsAcc = 0.0;
			for (int li = 0; li < kLines; ++li)
			{
				double rss = 0.0;
				int rows = 0;
				opBilinear.Residual(ls.Line(li), scratch, rss, rows, segSum, segRows);
				Assert::IsTrue(rows > 0);
				bilinearRmsAcc += std::sqrt(rss / static_cast<double>(rows));
			}
			const double naiveBilinearRms = bilinearRmsAcc / static_cast<double>(kLines);
			Assert::IsTrue(naiveBilinearRms > 5.0 * kFloor10Bit); // Proves naive Bilinear has a large real mismatch!

			// FitSeparableKernel must recover the unknown custom kernel accurately and crush naive Bilinear RMS.
			const SeparableKernelFitResult fit = FitSeparableKernel(ls, kLatentN, 0.0, kFloor10Bit, on);
			Assert::IsTrue(fit.valid);
			Assert::AreEqual(kLatentN, fit.n);
			Assert::AreEqual(kN, fit.N);
			Assert::IsTrue(fit.rSquared > 0.95);
			Assert::IsTrue(fit.EvaluateKernel(0.0) > fit.EvaluateKernel(1.0));
			Assert::IsTrue(fit.EvaluateKernel(1.0) > fit.EvaluateKernel(2.0));
			Assert::IsTrue(fit.fitRms < 0.4 * naiveBilinearRms);
		}

		TEST_METHOD(SharedPrecomputedPsdAndHoistedKernelFitMatchDirectReference)
		{
			using namespace ResidualSpectrumFx;
			const ResidualSpectrumSettings on = Enabled();

			constexpr int kN = 1024;
			constexpr int kLatentN = 512;
			constexpr int kLines = 24;
			constexpr int kLanczos3 = 5;
			DetRng rng(2026112305ull);

			// Part A: Shared PrecomputedLinePsd on USM-sharpened 2:1 upscale lines.
			LineSet sigUsm;
			sigUsm.axisLen = kN;
			sigUsm.Resize(kLines, kN);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> lat = MakeBroadbandLatent(kLatentN, rng);
				const std::vector<double> up = Upscale1DBank(lat, kN, kLanczos3, 0.0);
				const std::vector<double> blur = GaussianBlur1D(up, 1.0);
				std::vector<double> usm(static_cast<size_t>(kN), 0.0);
				for (int i = 0; i < kN; ++i)
					usm[static_cast<size_t>(i)] = up[static_cast<size_t>(i)] + 0.6 * (up[static_cast<size_t>(i)] - blur[static_cast<size_t>(i)]);
				const std::vector<double> q = Quantize10Bit(usm);
				std::copy(q.begin(), q.end(), sigUsm.Line(li));
			}
			const LineSet resUsm = ComputeRealDescaleResiduals(sigUsm, kLatentN, kLanczos3, 0.0);

			PrecomputedLinePsd sigPsd;
			Assert::IsTrue(ComputeLineSetPsd(sigUsm, sigPsd));
			Assert::IsTrue(sigPsd.valid);
			Assert::AreEqual(kN, sigPsd.axisLen);
			Assert::AreEqual(kLines, sigPsd.lineCount);
			Assert::IsTrue(sigPsd.fftN > 0);
			Assert::IsFalse(sigPsd.power.empty());

			const ResidualSpectrumResult clsDirect = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on, nullptr);
			const ResidualSpectrumResult clsShared = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on, &sigPsd);
			Assert::IsTrue(clsDirect.valid && clsShared.valid);
			Assert::IsTrue(clsDirect.classification == clsShared.classification);
			Assert::IsTrue(clsDirect.codedRaster == clsShared.codedRaster);
			Assert::AreEqual(clsDirect.neverRebase, clsShared.neverRebase);
			Assert::IsTrue(clsDirect.reason == clsShared.reason);
			Assert::IsTrue(std::fabs(clsDirect.lowBandPower - clsShared.lowBandPower) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.midBandPower - clsShared.midBandPower) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.highBandPower - clsShared.highBandPower) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.signalHighBandPower - clsShared.signalHighBandPower) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.highBandFlatness - clsShared.highBandFlatness) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.usmAmount - clsShared.usmAmount) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.usmRadiusPx - clsShared.usmRadiusPx) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.usmConfidence - clsShared.usmConfidence) < 1e-12);
			Assert::IsTrue(std::fabs(clsDirect.confidence - clsShared.confidence) < 1e-12);

			const EffectiveBandwidthResult bwDirect = EstimateEffectiveBandwidth(sigUsm, kFloor10Bit, on, nullptr);
			const EffectiveBandwidthResult bwShared = EstimateEffectiveBandwidth(sigUsm, kFloor10Bit, on, &sigPsd);
			Assert::IsTrue(bwDirect.valid && bwShared.valid);
			Assert::IsTrue(bwDirect.reason == bwShared.reason);
			Assert::IsTrue(std::fabs(bwDirect.cyclesPerPixel - bwShared.cyclesPerPixel) < 1e-12);
			Assert::IsTrue(std::fabs(bwDirect.rollOff10dBCyclesPerPixel - bwShared.rollOff10dBCyclesPerPixel) < 1e-12);
			Assert::IsTrue(std::fabs(bwDirect.totalExcessPower - bwShared.totalExcessPower) < 1e-12);

			// Part B: Shared PrecomputedLinePsd on real 720 -> 1080 -> 2160 double-scale lines.
			constexpr int kDsN = 2160;
			constexpr int kDsLines = 16;
			LineSet lsDouble;
			lsDouble.axisLen = kDsN;
			lsDouble.Resize(kDsLines, kDsN);
			for (int li = 0; li < kDsLines; ++li)
			{
				const std::vector<double> lat720 = MakeBroadbandLatent(720, rng);
				const std::vector<double> mid1080 = Upscale1DBank(lat720, 1080, kLanczos3, 0.0);
				const std::vector<double> outDouble = Quantize10Bit(Upscale1DBank(mid1080, kDsN, kLanczos3, 0.0));
				std::copy(outDouble.begin(), outDouble.end(), lsDouble.Line(li));
			}
			std::vector<DoubleScaleCandidate> dsCands;
			const int dsSizes[3] = { 540, 720, 1080 };
			for (int i = 0; i < 3; ++i)
			{
				DoubleScaleCandidate c;
				c.n = dsSizes[i];
				c.descaleStat = ComputeRealDescaleStat(lsDouble, dsSizes[i], kLanczos3);
				c.valid = true;
				dsCands.push_back(c);
			}

			PrecomputedLinePsd dsPsd;
			Assert::IsTrue(ComputeLineSetPsd(lsDouble, dsPsd));
			Assert::IsTrue(dsPsd.valid);

			const DoubleScaleResult dsDirect = EvaluateDoubleScale(lsDouble, 1080, dsCands, kFloor10Bit, on, nullptr);
			const DoubleScaleResult dsShared = EvaluateDoubleScale(lsDouble, 1080, dsCands, kFloor10Bit, on, &dsPsd);
			Assert::IsTrue(dsDirect.valid && dsShared.valid);
			Assert::AreEqual(dsDirect.doubleScalePresent, dsShared.doubleScalePresent);
			Assert::AreEqual(dsDirect.reportedLastStageN, dsShared.reportedLastStageN);
			Assert::AreEqual(dsDirect.firstStageN, dsShared.firstStageN);
			Assert::IsTrue(dsDirect.reason == dsShared.reason);
			Assert::IsTrue(std::fabs(dsDirect.bandDropDecades - dsShared.bandDropDecades) < 1e-12);
			Assert::IsTrue(std::fabs(dsDirect.confidence - dsShared.confidence) < 1e-12);

			// Part C — MANDATORY Rule 3 Negative Control (Unmasked Mismatched & Invalid PrecomputedLinePsd Rejection):
			// Every validation predicate (valid == false, axisLen mismatch, pure fftN mismatch, pure lineCount mismatch)
			// is tested UNMASKED across ALL 3 consumers (ClassifyResidualSpectrum, EstimateEffectiveBandwidth, EvaluateDoubleScale).
			Assert::IsTrue(dsPsd.axisLen != sigPsd.axisLen);

			// (a) Pure fftN mismatch: matching axisLen AND matching lineCount, cropped length = 512 -> fftN = 512 != 1024.
			LineSet lsCrop512Sig;
			lsCrop512Sig.axisLen = kN;
			lsCrop512Sig.Resize(kLines, 512);
			for (int li = 0; li < kLines; ++li)
				std::copy(sigUsm.Line(li), sigUsm.Line(li) + 512, lsCrop512Sig.Line(li));
			PrecomputedLinePsd psdPureFftSig;
			Assert::IsTrue(ComputeLineSetPsd(lsCrop512Sig, psdPureFftSig));
			Assert::IsTrue(psdPureFftSig.valid);
			Assert::AreEqual(sigPsd.axisLen, psdPureFftSig.axisLen);
			Assert::AreEqual(sigPsd.lineCount, psdPureFftSig.lineCount);
			Assert::AreNotEqual(sigPsd.fftN, psdPureFftSig.fftN);

			const ResidualSpectrumResult clsPureFft = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on, &psdPureFftSig);
			const EffectiveBandwidthResult bwPureFft = EstimateEffectiveBandwidth(sigUsm, kFloor10Bit, on, &psdPureFftSig);
			Assert::IsFalse(clsPureFft.valid);
			Assert::IsTrue(clsPureFft.reason == ResidualSpectrumReason::InvalidInput);
			Assert::IsFalse(bwPureFft.valid);
			Assert::IsTrue(bwPureFft.reason == ResidualSpectrumReason::InvalidInput);

			LineSet lsCrop512Ds;
			lsCrop512Ds.axisLen = kDsN;
			lsCrop512Ds.Resize(kDsLines, 512);
			for (int li = 0; li < kDsLines; ++li)
				std::copy(lsDouble.Line(li), lsDouble.Line(li) + 512, lsCrop512Ds.Line(li));
			PrecomputedLinePsd psdPureFftDs;
			Assert::IsTrue(ComputeLineSetPsd(lsCrop512Ds, psdPureFftDs));
			Assert::IsTrue(psdPureFftDs.valid);
			Assert::AreEqual(dsPsd.axisLen, psdPureFftDs.axisLen);
			Assert::AreEqual(dsPsd.lineCount, psdPureFftDs.lineCount);
			Assert::AreNotEqual(dsPsd.fftN, psdPureFftDs.fftN);

			const DoubleScaleResult dsPureFft = EvaluateDoubleScale(lsDouble, 1080, dsCands, kFloor10Bit, on, &psdPureFftDs);
			Assert::IsFalse(dsPureFft.valid);
			Assert::IsTrue(dsPureFft.reason == ResidualSpectrumReason::InvalidInput);

			// (b) Pure lineCount mismatch: matching axisLen AND matching fftN, different count = kLines / 2.
			LineSet lsHalfLinesSig;
			lsHalfLinesSig.axisLen = kN;
			lsHalfLinesSig.Resize(kLines / 2, kN);
			for (int li = 0; li < kLines / 2; ++li)
				std::copy(sigUsm.Line(li), sigUsm.Line(li) + kN, lsHalfLinesSig.Line(li));
			PrecomputedLinePsd psdPureLinesSig;
			Assert::IsTrue(ComputeLineSetPsd(lsHalfLinesSig, psdPureLinesSig));
			Assert::IsTrue(psdPureLinesSig.valid);
			Assert::AreEqual(sigPsd.axisLen, psdPureLinesSig.axisLen);
			Assert::AreEqual(sigPsd.fftN, psdPureLinesSig.fftN);
			Assert::AreNotEqual(sigPsd.lineCount, psdPureLinesSig.lineCount);

			const ResidualSpectrumResult clsPureLines = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on, &psdPureLinesSig);
			const EffectiveBandwidthResult bwPureLines = EstimateEffectiveBandwidth(sigUsm, kFloor10Bit, on, &psdPureLinesSig);
			Assert::IsFalse(clsPureLines.valid);
			Assert::IsTrue(clsPureLines.reason == ResidualSpectrumReason::InvalidInput);
			Assert::IsFalse(bwPureLines.valid);
			Assert::IsTrue(bwPureLines.reason == ResidualSpectrumReason::InvalidInput);

			LineSet lsHalfLinesDs;
			lsHalfLinesDs.axisLen = kDsN;
			lsHalfLinesDs.Resize(kDsLines / 2, kDsN);
			for (int li = 0; li < kDsLines / 2; ++li)
				std::copy(lsDouble.Line(li), lsDouble.Line(li) + kDsN, lsHalfLinesDs.Line(li));
			PrecomputedLinePsd psdPureLinesDs;
			Assert::IsTrue(ComputeLineSetPsd(lsHalfLinesDs, psdPureLinesDs));
			Assert::IsTrue(psdPureLinesDs.valid);
			Assert::AreEqual(dsPsd.axisLen, psdPureLinesDs.axisLen);
			Assert::AreEqual(dsPsd.fftN, psdPureLinesDs.fftN);
			Assert::AreNotEqual(dsPsd.lineCount, psdPureLinesDs.lineCount);

			const DoubleScaleResult dsPureLines = EvaluateDoubleScale(lsDouble, 1080, dsCands, kFloor10Bit, on, &psdPureLinesDs);
			Assert::IsFalse(dsPureLines.valid);
			Assert::IsTrue(dsPureLines.reason == ResidualSpectrumReason::InvalidInput);

			// (c) Pure axisLen mismatch & valid == false across all 3 consumers.

			PrecomputedLinePsd badAxisSig = sigPsd;
			badAxisSig.axisLen = sigPsd.axisLen + 128;
			PrecomputedLinePsd invalidSig = sigPsd;
			invalidSig.valid = false;

			const ResidualSpectrumResult clsBadAxis = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on, &badAxisSig);
			const ResidualSpectrumResult clsInvalid = ClassifyResidualSpectrum(sigUsm, resUsm, kLatentN, kFloor10Bit, on, &invalidSig);
			Assert::IsFalse(clsBadAxis.valid);
			Assert::IsTrue(clsBadAxis.reason == ResidualSpectrumReason::InvalidInput);
			Assert::IsFalse(clsInvalid.valid);
			Assert::IsTrue(clsInvalid.reason == ResidualSpectrumReason::InvalidInput);

			const EffectiveBandwidthResult bwBadAxis = EstimateEffectiveBandwidth(sigUsm, kFloor10Bit, on, &badAxisSig);
			const EffectiveBandwidthResult bwInvalid = EstimateEffectiveBandwidth(sigUsm, kFloor10Bit, on, &invalidSig);
			Assert::IsFalse(bwBadAxis.valid);
			Assert::IsTrue(bwBadAxis.reason == ResidualSpectrumReason::InvalidInput);
			Assert::IsFalse(bwInvalid.valid);
			Assert::IsTrue(bwInvalid.reason == ResidualSpectrumReason::InvalidInput);

			PrecomputedLinePsd badAxisDs = dsPsd;
			badAxisDs.axisLen = dsPsd.axisLen - 128;
			PrecomputedLinePsd invalidDs = dsPsd;
			invalidDs.valid = false;

			const DoubleScaleResult dsBadAxis = EvaluateDoubleScale(lsDouble, 1080, dsCands, kFloor10Bit, on, &badAxisDs);
			const DoubleScaleResult dsInvalid = EvaluateDoubleScale(lsDouble, 1080, dsCands, kFloor10Bit, on, &invalidDs);
			Assert::IsFalse(dsBadAxis.valid);
			Assert::IsTrue(dsBadAxis.reason == ResidualSpectrumReason::InvalidInput);
			Assert::IsFalse(dsInvalid.valid);
			Assert::IsTrue(dsInvalid.reason == ResidualSpectrumReason::InvalidInput);

			// Part D: Hoisted FitSeparableKernel matches independent naive direct-basis evaluation on custom Gaussian.
			static constexpr int kHalfSupport = 3;
			static constexpr double kSigma = 0.85;
			auto customGaussian = [](double absDist) -> double {
				if (absDist >= static_cast<double>(kHalfSupport))
					return 0.0;
				return std::exp(-0.5 * (absDist * absDist) / (kSigma * kSigma));
			};

			LineSet lsGauss;
			lsGauss.axisLen = kN;
			lsGauss.Resize(kLines, kN);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> lat = MakeBroadbandLatent(kLatentN, rng);
				const std::vector<double> up = Quantize10Bit(Upscale1DCustom(lat, kN, kHalfSupport, customGaussian, 0.0));
				std::copy(up.begin(), up.end(), lsGauss.Line(li));
			}

			const SeparableKernelFitResult fit1 = FitSeparableKernel(lsGauss, kLatentN, 0.0, kFloor10Bit, on);
			const SeparableKernelFitResult fit2 = FitSeparableKernel(lsGauss, kLatentN, 0.0, kFloor10Bit, on);
			Assert::IsTrue(fit1.valid && fit2.valid);
			Assert::IsTrue(fit1.rSquared > 0.95);
			Assert::IsTrue(std::fabs(fit1.fitRms - fit2.fitRms) < 1e-12);
			Assert::AreEqual(fit1.knotDistances.size(), fit1.knotWeights.size());
			Assert::IsTrue(fit1.knotDistances.size() >= 3u);
			for (size_t k = 0; k < fit1.knotDistances.size(); ++k)
			{
				Assert::IsTrue(std::fabs(fit1.knotWeights[k] - fit2.knotWeights[k]) < 1e-12);
				Assert::IsTrue(std::fabs(fit1.EvaluateKernel(fit1.knotDistances[k]) - fit1.knotWeights[k]) < 1e-12);
			}

			// Independent naive direct-basis LS residual evaluation using any symmetric kernel function.
			auto evalDirectBasisRms = [&](const LineSet& ls, int candN, int halfSup, auto kernelFn) -> double {
				const int N = ls.axisLen;
				const int L = ls.length;
				const int taps = 2 * halfSup;
				const int margin = OperatorMargin(N, candN, 4);
				const double ratio = static_cast<double>(candN) / static_cast<double>(N);

				std::vector<double> W(static_cast<size_t>(L) * static_cast<size_t>(taps), 0.0);
				std::vector<int> cl(static_cast<size_t>(L), 0);
				int cmin = 0, cmax = 0;
				for (int j = 0; j < L; ++j)
				{
					const double p = (static_cast<double>(ls.origin + j) + 0.5) * ratio - 0.5;
					const double fl = std::floor(p);
					double sum = 0.0;
					for (int s = 0; s < taps; ++s)
					{
						const int node = static_cast<int>(fl) - halfSup + 1 + s;
						const double w = kernelFn(std::fabs(p - static_cast<double>(node)));
						W[static_cast<size_t>(j) * static_cast<size_t>(taps) + static_cast<size_t>(s)] = w;
						sum += w;
					}
					if (sum > 1e-12)
					{
						for (int s = 0; s < taps; ++s)
							W[static_cast<size_t>(j) * static_cast<size_t>(taps) + static_cast<size_t>(s)] /= sum;
					}
					const int c = static_cast<int>(fl) - halfSup + 1;
					cl[static_cast<size_t>(j)] = c;
					if (j == 0 || c < cmin) cmin = c;
					if (j == 0 || c > cmax) cmax = c;
				}
				const int m = cmax + taps - cmin;
				for (int j = 0; j < L; ++j)
					cl[static_cast<size_t>(j)] -= cmin;

				const int u = taps - 1;
				const size_t bw = static_cast<size_t>(u + 1);
				std::vector<double> A(static_cast<size_t>(m) * bw, 0.0);
				for (int j = 0; j < L; ++j)
				{
					const double* w = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const int c = cl[static_cast<size_t>(j)];
					for (int s0 = 0; s0 < taps; ++s0)
						for (int t0 = s0; t0 < taps; ++t0)
							A[static_cast<size_t>(c + t0) * bw + static_cast<size_t>(t0 - s0)] += w[s0] * w[t0];
				}
				double maxDiag = 0.0;
				for (int i = 0; i < m; ++i)
					if (A[static_cast<size_t>(i) * bw] > maxDiag)
						maxDiag = A[static_cast<size_t>(i) * bw];
				const double ridge = 1e-9 * maxDiag;
				for (int i = 0; i < m; ++i)
					A[static_cast<size_t>(i) * bw] += ridge;

				for (int j = 0; j < m; ++j)
				{
					double s = A[static_cast<size_t>(j) * bw];
					const int k0 = j - u > 0 ? j - u : 0;
					for (int k = k0; k < j; ++k)
					{
						const double l = A[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						s -= l * l;
					}
					const double inv = 1.0 / std::sqrt(s > 1e-300 ? s : 1e-300);
					A[static_cast<size_t>(j) * bw] = inv;
					const int i1 = j + u < m - 1 ? j + u : m - 1;
					for (int i = j + 1; i <= i1; ++i)
					{
						double v = A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)];
						const int kk0 = i - u > 0 ? i - u : 0;
						for (int k = kk0; k < j; ++k)
							v -= A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * A[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)] = v * inv;
					}
				}

				std::vector<double> z(static_cast<size_t>(m), 0.0);
				double sumRms = 0.0;
				for (int li = 0; li < ls.count; ++li)
				{
					const double* y = ls.Line(li);
					std::fill(z.begin(), z.end(), 0.0);
					for (int j = 0; j < L; ++j)
					{
						const double* w = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
						const int c = cl[static_cast<size_t>(j)];
						for (int s = 0; s < taps; ++s)
							z[static_cast<size_t>(c + s)] += w[s] * y[j];
					}
					for (int i = 0; i < m; ++i)
					{
						double v = z[static_cast<size_t>(i)];
						const int k0 = i - u > 0 ? i - u : 0;
						for (int k = k0; k < i; ++k)
							v -= A[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * z[static_cast<size_t>(k)];
						z[static_cast<size_t>(i)] = v * A[static_cast<size_t>(i) * bw];
					}
					for (int i = m - 1; i >= 0; --i)
					{
						double v = z[static_cast<size_t>(i)];
						const int k1 = i + u < m - 1 ? i + u : m - 1;
						for (int k = i + 1; k <= k1; ++k)
							v -= A[static_cast<size_t>(k) * bw + static_cast<size_t>(k - i)] * z[static_cast<size_t>(k)];
						z[static_cast<size_t>(i)] = v * A[static_cast<size_t>(i) * bw];
					}
					double ss = 0.0;
					int rows = 0;
					for (int j = margin; j < L - margin; ++j)
					{
						const double* w = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
						const int c = cl[static_cast<size_t>(j)];
						double pred = 0.0;
						for (int s = 0; s < taps; ++s)
							pred += w[s] * z[static_cast<size_t>(c + s)];
						const double e = y[j] - pred;
						ss += e * e;
						++rows;
					}
					const double dof = 1.0 - ratio;
					sumRms += rows > 0 ? std::sqrt(ss / (static_cast<double>(rows) * (dof > 0.05 ? dof : 0.05))) : 0.0;
				}
				return ls.count > 0 ? sumRms / static_cast<double>(ls.count) : 0.0;
			};

			const double directFittedRms = evalDirectBasisRms(lsGauss, kLatentN, fit1.halfSupport,
				[&](double d) { return fit1.EvaluateKernel(d); });
			const double directTrueGaussRms = evalDirectBasisRms(lsGauss, kLatentN, kHalfSupport, customGaussian);
			// Negative control: flat box kernel fails on the exact same Gaussian-upscaled lines.
			const double directFlatBoxRms = evalDirectBasisRms(lsGauss, kLatentN, kHalfSupport,
				[](double d) { return d < 3.0 ? 1.0 : 0.0; });

			Assert::IsTrue(directFlatBoxRms > 3.0 * directFittedRms);
			Assert::IsTrue(directFittedRms < 2.5 * (directTrueGaussRms > kFloor10Bit ? directTrueGaussRms : kFloor10Bit));
			Assert::IsTrue(std::fabs(directFittedRms - fit1.fitRms) < 0.15 * (fit1.fitRms + kFloor10Bit));
		}
	};
}
