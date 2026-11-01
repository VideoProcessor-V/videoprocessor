// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The frame-level quality signal for the ABR step detector (doc 61 section 2.2, README call site 9). The monitor's own
// `Snapshot.bitrateQuality` is SMOOTHED over ~2 s and blocking/banding based, so it lags a rendition switch and is weak on modern
// encodes (tools/eval/findings-streaming.md: blocking/banding barely move at 3-6 Mb/s; absolute NR thresholds do not separate
// rungs). This unit gives the detector a per-analysed-frame value that steps at a switch:
//
//   quality = min( delivered resolution term , noise term )
//     resolution term  the frame's delivered/native ratio (1 = native). A rung with fewer lines is upscaled by the player and the
//                      monitor reads that ratio per frame, so a resolution rung drop is a step in this term.
//     noise term       the picture's flat-area residual noise against the stream's OWN recent upper level (RELATIVE: never an
//                      absolute threshold). A starved or lower-bitrate rung denoises, so the term falls. Content that carries no
//                      noise at all (clean animation, film masters) gives no step: this signal cannot see a rate-only switch there,
//                      and the detector then reports nothing (documented limit, measured in tools/eval/source_id).
//
// One implementation (owner rule "one unit per shared computation"): the CPU reference below is what the monitor worker calls on
// the tiles it already holds and what the offline evaluation calls on the same tiles. Pure, header-only, C++14, deterministic
// (fixed reduction orders), no clock: the caller passes SOURCE time.
#include "../../../repair/grain_split/shared/BandFromBase.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace SourceIdentity
{
	namespace FrameQuality
	{
		// The version of the signal's law (bump on ANY change of the measurement or the combination).
		constexpr uint32_t kSignalVersion = 1;

		struct TileNoise
		{
			bool valid = false;
			double sigma = 0.0;      // robust sigma of the 1-px residual in the flattest part, in 8-bit code units
			double coverage = 0.0;   // share of the tile that was flat enough to measure
		};

		namespace detail
		{
			// The taps come from shared unit U2 (VpUnit::BandFromBase): radius ceil(3 sigma), normalised Gaussian weights.
			inline std::vector<float> GaussTaps(double sigma)
			{
				const int r = VpUnit::BandFromBase::Radius(sigma, 3.0, false, 0.0);
				std::vector<float> t(static_cast<size_t>(2 * r + 1));
				for (int i = -r; i <= r; ++i)
					t[static_cast<size_t>(i + r)] = static_cast<float>(VpUnit::BandFromBase::Weight(i, sigma, r));
				return t;
			}

			inline int Reflect(int i, int n)
			{
				if (i < 0) i = -i;
				if (i >= n) i = 2 * (n - 1) - i;
				return i < 0 ? 0 : (i >= n ? n - 1 : i);
			}

			// Separable Gaussian with reflected borders (rows then columns, fixed order).
			inline void Blur(const std::vector<float>& src, std::vector<float>& dst, int w, int h, const std::vector<float>& taps)
			{
				const int r = static_cast<int>(taps.size() / 2);
				std::vector<float> tmp(src.size());
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						float a = 0.f;
						for (int k = -r; k <= r; ++k)
							a += taps[static_cast<size_t>(k + r)] * src[static_cast<size_t>(y) * w + Reflect(x + k, w)];
						tmp[static_cast<size_t>(y) * w + x] = a;
					}
				dst.resize(src.size());
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						float a = 0.f;
						for (int k = -r; k <= r; ++k)
							a += taps[static_cast<size_t>(k + r)] * tmp[static_cast<size_t>(Reflect(y + k, h)) * w + x];
						dst[static_cast<size_t>(y) * w + x] = a;
					}
			}

			inline double Median(std::vector<double>& v)
			{
				if (v.empty()) return 0.0;
				std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
				return v[v.size() / 2];
			}
		}

		// `luma`: T x T samples normalised 0..1 (the monitor's tile, before the code-range scaling). 8-bit units = x255.
		// The measure follows tools/eval/nr_metrics.py noise_sigma: the flattest 30% of the tile (low local energy of the 1-px
		// residual, gentle slope), MAD sigma of the residual there.
		inline TileNoise MeasureNoise(const float* luma, int T)
		{
			TileNoise out;
			if (!luma || T < 32)
				return out;
			const size_t n = static_cast<size_t>(T) * T;
			std::vector<float> u(n), g1, g2;
			for (size_t i = 0; i < n; ++i)
				u[i] = luma[i] * 255.0f;
			static const std::vector<float> t1 = detail::GaussTaps(1.0), t2 = detail::GaussTaps(2.0);
			detail::Blur(u, g1, T, T, t1);
			detail::Blur(u, g2, T, T, t2);
			std::vector<float> hp(n), e(n);
			for (size_t i = 0; i < n; ++i)
				hp[i] = u[i] - g1[i];
			// Local energy of the residual over 7 x 7 (box, summed in a fixed order).
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x)
				{
					double s = 0.0;
					for (int dy = -3; dy <= 3; ++dy)
						for (int dx = -3; dx <= 3; ++dx)
						{
							const float v = hp[static_cast<size_t>(detail::Reflect(y + dy, T)) * T + detail::Reflect(x + dx, T)];
							s += static_cast<double>(v) * v;
						}
					e[static_cast<size_t>(y) * T + x] = static_cast<float>(s / 49.0);
				}
			// Gentle large-scale slope only.
			std::vector<double> okEnergy;
			std::vector<char> ok(n, 0);
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x)
				{
					const int xm = detail::Reflect(x - 1, T), xp = detail::Reflect(x + 1, T);
					const int ym = detail::Reflect(y - 1, T), yp = detail::Reflect(y + 1, T);
					const double gx = 0.5 * (g2[static_cast<size_t>(y) * T + xp] - g2[static_cast<size_t>(y) * T + xm]);
					const double gy = 0.5 * (g2[static_cast<size_t>(yp) * T + x] - g2[static_cast<size_t>(ym) * T + x]);
					if (std::sqrt(gx * gx + gy * gy) < 3.0)
					{
						ok[static_cast<size_t>(y) * T + x] = 1;
						okEnergy.push_back(e[static_cast<size_t>(y) * T + x]);
					}
				}
			if (okEnergy.size() < 64)
				return out;
			std::vector<double> sorted = okEnergy;
			std::sort(sorted.begin(), sorted.end());
			const double thr = sorted[static_cast<size_t>(0.3 * static_cast<double>(sorted.size() - 1))];
			std::vector<double> residual;
			for (size_t i = 0; i < n; ++i)
				if (ok[i] && e[i] <= thr)
					residual.push_back(hp[i]);
			if (residual.size() < 64)
				return out;
			std::vector<double> copy = residual;
			const double med = detail::Median(copy);
			for (double& v : residual)
				v = std::fabs(v - med);
			out.sigma = 1.4826 * detail::Median(residual);
			out.coverage = static_cast<double>(residual.size()) / static_cast<double>(n);
			out.valid = true;
			return out;
		}

		struct Settings
		{
			// The reference noise level is this quantile of the stream's own recent measurements ("its upper level").
			double referenceQuantile = 0.8;
			size_t referenceWindow = 120;        // measurements kept (the monitor analyses about 2 frames per second: ~60 s)
			size_t referenceMinimum = 8;         // measurements (about 4 s) before the noise term is used at all: a short history's quantile is its start
			double minimumSigma = 0.25;          // below this (8-bit units) a picture carries no noise to lose: the term abstains
			double minimumCoverage = 0.02;       // of a tile
		};

		// The per-stream state: the noise reference. Reset at a discontinuity (a new title restarts its own baseline).
		class Signal
		{
		public:
			Signal() {}
			explicit Signal(const Settings& s) : s_(s) {}

			void Reset()
			{
				history_.clear();
				lastNoise_ = -1.0;
				lastTerm_ = 1.0;
			}

			// `ratio`: the frame's delivered/native reading (<= 0 = none: the term abstains); `noise`: the per-tile
			// measurements of the frame's informative tiles (invalid ones are skipped). Returns the frame quality in 0..1.
			double Push(double ratio, const std::vector<TileNoise>& noise)
			{
				std::vector<double> sigmas;
				for (const TileNoise& t : noise)
					if (t.valid && t.coverage >= s_.minimumCoverage)
						sigmas.push_back(t.sigma);
				double term = 1.0;
				lastNoise_ = -1.0;
				if (!sigmas.empty())
				{
					const double sigma = detail::Median(sigmas);
					lastNoise_ = sigma;
					history_.push_back(sigma);
					while (history_.size() > s_.referenceWindow)
						history_.pop_front();
					if (history_.size() >= s_.referenceMinimum)
					{
						std::vector<double> sorted(history_.begin(), history_.end());
						std::sort(sorted.begin(), sorted.end());
						const double ref = sorted[static_cast<size_t>(s_.referenceQuantile * static_cast<double>(sorted.size() - 1))];
						if (ref >= s_.minimumSigma)
							term = (std::min)(1.0, sigma / ref);
					}
				}
				lastTerm_ = term;
				const double res = ratio > 0.0 ? (std::min)(1.0, ratio) : 1.0;
				return (std::max)(0.0, (std::min)(res, term));
			}

			double LastNoise() const { return lastNoise_; }
			double LastNoiseTerm() const { return lastTerm_; }

		private:
			Settings s_;
			std::deque<double> history_;
			double lastNoise_ = -1.0;
			double lastTerm_ = 1.0;
		};
	}
}
