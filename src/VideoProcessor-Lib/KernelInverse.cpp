#include "pch.h"
#include "KernelInverse.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>

namespace KernelInverse
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		double Sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

		Candidate Make(const std::string& name, const std::function<double(double)>& k, int radius)
		{
			Candidate c;
			c.name = name;
			// The upscale's phases: output 2i + p sits at 1080 position i - 0.25 (p = 0)
			// or i + 0.25 (p = 1); weight on sample i + d is k(s - d), normalised.
			c.phaseFirst = -radius - 1;
			const int taps = 2 * radius + 3;
			for (int p = 0; p < 2; ++p)
			{
				const double s = p == 0 ? -0.25 : 0.25;
				std::vector<double> f(static_cast<size_t>(taps));
				double sum = 0.0;
				for (int t = 0; t < taps; ++t) { f[static_cast<size_t>(t)] = k(s - (c.phaseFirst + t)); sum += f[static_cast<size_t>(t)]; }
				c.phase[p].resize(f.size());
				for (int t = 0; t < taps; ++t) c.phase[p][static_cast<size_t>(t)] = static_cast<float>(f[static_cast<size_t>(t)] / sum);
			}
			// The least-squares inverse P = (U'U)^-1 U': U'U is a convolution on the 1080
			// grid, g[d] = sum_p sum_e f_p[e] f_p[e + d]; its inverse h through the DFT of a
			// long period (the filter decays fast), then P's row for output 0 on the 4K grid.
			const int N = 512;
			std::vector<double> g(static_cast<size_t>(N), 0.0);
			for (int p = 0; p < 2; ++p)
				for (int a = 0; a < taps; ++a)
					for (int b = 0; b < taps; ++b)
					{
						const int d = b - a;   // f_p[e] f_p[e + d]
						g[static_cast<size_t>((d % N + N) % N)] += c.phase[p][static_cast<size_t>(a)] * c.phase[p][static_cast<size_t>(b)];
					}
			std::vector<double> h(static_cast<size_t>(N), 0.0);
			for (int w = 0; w < N; ++w)
			{
				std::complex<double> G(0.0, 0.0);
				for (int d = 0; d < N; ++d) G += g[static_cast<size_t>(d)] * std::polar(1.0, -2.0 * kPi * w * d / N);
				const std::complex<double> H = 1.0 / G;
				for (int d = 0; d < N; ++d) h[static_cast<size_t>(d)] += (H * std::polar(1.0, 2.0 * kPi * w * d / N)).real() / N;
			}
			auto hAt = [&](int d) { return h[static_cast<size_t>((d % N + N) % N)]; };
			// Output 0's weight on 4K sample j = 2i' + p: sum_d h[d] f_p[-d - i'].
			const int half = radius >= 4 ? 40 : 25;   // 80 / 50 taps: every inverse within a twentieth of a code (Lanczos-4 decays slowest)
			c.first = -half + 1;
			c.weights.assign(static_cast<size_t>(2 * half), 0.0f);
			double sum = 0.0;
			std::vector<double> wd(c.weights.size(), 0.0);
			for (int t = 0; t < static_cast<int>(wd.size()); ++t)
			{
				const int j = c.first + t;
				const int ip = static_cast<int>(std::floor(j / 2.0)), p = j - 2 * ip;
				double acc = 0.0;
				for (int e = 0; e < taps; ++e)
				{
					const int d = -(c.phaseFirst + e) - ip;   // f_p[e'] with e' = -d - i' = phaseFirst + e
					acc += hAt(d) * c.phase[p][static_cast<size_t>(e)];
				}
				wd[static_cast<size_t>(t)] = acc;
				sum += acc;
			}
			for (size_t t = 0; t < wd.size(); ++t) c.weights[t] = static_cast<float>(wd[t] / sum);   // truncation: keep DC exact
			return c;
		}

		std::function<double(double)> Bicubic(double B, double C)
		{
			return [B, C](double x) {
				x = std::fabs(x);
				if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
				if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
				return 0.0;
			};
		}
		std::function<double(double)> Lanczos(int a)
		{
			return [a](double x) { return std::fabs(x) < a ? Sinc(x) * Sinc(x / a) : 0.0; };
		}
	}

	const std::vector<Candidate>& Candidates()
	{
		static const std::vector<Candidate> list = [] {
			std::vector<Candidate> v;
			v.push_back(Make("bicubic C0.5 (Catmull-Rom)", Bicubic(0.0, 0.5), 2));
			v.push_back(Make("bicubic C0.6", Bicubic(0.0, 0.6), 2));
			v.push_back(Make("bicubic C0.75", Bicubic(0.0, 0.75), 2));
			v.push_back(Make("lanczos2", Lanczos(2), 2));
			v.push_back(Make("lanczos3", Lanczos(3), 3));
			v.push_back(Make("lanczos4", Lanczos(4), 4));
			v.push_back(Make("bilinear", [](double x) { return (std::max)(0.0, 1.0 - std::fabs(x)); }, 1));
			return v;
		}();
		return list;
	}

	int Find(const std::string& name)
	{
		const auto& list = Candidates();
		for (size_t i = 0; i < list.size(); ++i) if (list[i].name == name) return static_cast<int>(i);
		return -1;
	}

	void InverseLine(const Candidate& c, const float* in, int n4, float* out)
	{
		const int n1 = n4 / 2;
		for (int i = 0; i < n1; ++i)
		{
			double acc = 0.0;
			for (size_t t = 0; t < c.weights.size(); ++t)
			{
				const int j = (std::min)(n4 - 1, (std::max)(0, 2 * i + c.first + static_cast<int>(t)));
				acc += c.weights[t] * in[j];
			}
			out[i] = static_cast<float>(acc);
		}
	}

	void UpscaleLine(const Candidate& c, const float* in, int n1, float* out)
	{
		for (int i = 0; i < n1; ++i)
			for (int p = 0; p < 2; ++p)
			{
				double acc = 0.0;
				for (size_t t = 0; t < c.phase[p].size(); ++t)
				{
					const int k = (std::min)(n1 - 1, (std::max)(0, i + c.phaseFirst + static_cast<int>(t)));
					acc += c.phase[p][t] * in[k];
				}
				out[2 * i + p] = static_cast<float>(acc);
			}
	}

	TileFit FitTile(const float* tile, int n, int stride, int codeMaximum)
	{
		TileFit fit;
		const auto& list = Candidates();
		const int m = n / 2;
		std::vector<float> rows(static_cast<size_t>(n) * m), lowRes(static_cast<size_t>(m) * m), upRows(static_cast<size_t>(m) * n), up(static_cast<size_t>(n) * n);
		std::vector<float> lineIn(static_cast<size_t>(n)), lineOut(static_cast<size_t>(n));
		const int margin = 32;   // the filters' reach at the tile's edges
		for (const Candidate& c : list)
		{
			// Inverse across (each 4K row -> m samples), then down (each column).
			for (int y = 0; y < n; ++y) InverseLine(c, tile + static_cast<size_t>(y) * stride, n, &rows[static_cast<size_t>(y) * m]);
			for (int x = 0; x < m; ++x)
			{
				for (int y = 0; y < n; ++y) lineIn[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * m + x];
				InverseLine(c, lineIn.data(), n, lineOut.data());
				for (int y = 0; y < m; ++y) lowRes[static_cast<size_t>(y) * m + x] = lineOut[static_cast<size_t>(y)];
			}
			// Upscale again: across, then down.
			for (int y = 0; y < m; ++y) UpscaleLine(c, &lowRes[static_cast<size_t>(y) * m], m, &upRows[static_cast<size_t>(y) * n]);
			for (int x = 0; x < n; ++x)
			{
				for (int y = 0; y < m; ++y) lineIn[static_cast<size_t>(y)] = upRows[static_cast<size_t>(y) * n + x];
				UpscaleLine(c, lineIn.data(), m, lineOut.data());
				for (int y = 0; y < n; ++y) up[static_cast<size_t>(y) * n + x] = lineOut[static_cast<size_t>(y)];
			}
			double sum = 0.0;
			long count = 0;
			for (int y = margin; y < n - margin; ++y)
				for (int x = margin; x < n - margin; ++x)
				{
					const double d = (up[static_cast<size_t>(y) * n + x] - tile[static_cast<size_t>(y) * stride + x]) * codeMaximum;
					sum += d * d;
					++count;
				}
			fit.residuals.push_back(count > 0 ? std::sqrt(sum / count) : 1e9);
		}
		std::vector<double> sorted = fit.residuals;
		std::sort(sorted.begin(), sorted.end());
		fit.best = static_cast<int>(std::min_element(fit.residuals.begin(), fit.residuals.end()) - fit.residuals.begin());
		fit.bestResidual = sorted.empty() ? 0.0 : sorted[0];
		fit.secondResidual = sorted.size() > 1 ? sorted[1] : 1e9;
		return fit;
	}

	bool Accept(const TileFit& fit, double floorCodes, double margin)
	{
		return fit.best >= 0 && fit.bestResidual <= floorCodes && fit.secondResidual >= fit.bestResidual * margin;
	}

	double FloorCodes(double effectiveBits, int signalBits)
	{
		// Rounding to the content's step (step / sqrt(12)) in signal codes, 20% allowance,
		// never below the signal's own rounding.
		const double bits = effectiveBits > 0.0 ? (std::min)(static_cast<double>(signalBits), effectiveBits) : signalBits;
		const double step = std::pow(2.0, signalBits - bits);
		return 1.2 * step / std::sqrt(12.0) + 0.05;
	}
}
