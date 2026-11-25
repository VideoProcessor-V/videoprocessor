// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Frame-level quality signal for the ABR step detector (doc 61 s2.2):
//   quality = min(delivered resolution term, relative flat-area residual noise term).
// Pure CPU, C++14, deterministic (fixed reduction orders), no clock.
#include "../../../../core/RobustStats.h"
#include "../../../repair/grain_split/shared/BandFromBase.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace VpSkeleton { class SkeletonMemo; }

namespace SourceIdentity
{
	namespace FrameQuality
	{
		// The version of the signal's law (bump on ANY change of the measurement or the combination).
		constexpr uint32_t kSignalVersion = 1;

		struct TileNoise
		{
			bool valid = false;
			double sigma = 0.0;      // robust sigma of 1-px residual in flattest part (8-bit codes)
			double coverage = 0.0;   // share of tile flat enough to measure
			bool usedBandMemo = false;
			std::string bandMemoFallback;
		};

		namespace detail
		{
			// Out-of-line in CutFadeJudge.cpp (VideoProcessor-Lib): acquires CPU kBandMemo for sigma 1 & 2 (Reflect) into g1/g2 (8-bit codes).
			bool TryAcquireTileBands(VpSkeleton::SkeletonMemo* memo, uint64_t seq, int T,
				std::vector<float>& g1, std::vector<float>& g2, std::string& fallbackWhy);
		}

		// `luma`: T x T samples in 0..1 (8-bit = x255). Optional `memo` + `seq`: acquires CPU kBandMemo (sigma 1 & 2,
		// Reflect edge) with visible fallback to shared unit U2 (VpUnit::BandFromBase::Taps / BlurTaps).
		inline TileNoise MeasureNoise(const float* luma, int T, VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seq = 0)
		{
			TileNoise out;
			if (!luma || T < 32) return out;
			const size_t n = static_cast<size_t>(T) * T;
			std::vector<float> u(n), g1, g2;
			for (size_t i = 0; i < n; ++i) u[i] = luma[i] * 255.0f;
			if (memo != nullptr && seq != 0u && detail::TryAcquireTileBands(memo, seq, T, g1, g2, out.bandMemoFallback)) {
				out.usedBandMemo = true;
			} else {
				if (memo == nullptr) out.bandMemoFallback = "no skeleton memo bound";
				else if (seq == 0u) out.bandMemoFallback = "frame sequence 0 (unkeyed)";
				static const std::vector<float> t1 = VpUnit::BandFromBase::Taps<float>(1.0, 3.0, false, 0.0);
				static const std::vector<float> t2 = VpUnit::BandFromBase::Taps<float>(2.0, 3.0, false, 0.0);
				VpUnit::BandFromBase::BlurTaps<float>(u, g1, T, T, t1, VpUnit::BandFromBase::EdgeMode::Reflect);
				VpUnit::BandFromBase::BlurTaps<float>(u, g2, T, T, t2, VpUnit::BandFromBase::EdgeMode::Reflect);
			}
			std::vector<float> hp(n), e(n);
			for (size_t i = 0; i < n; ++i) hp[i] = u[i] - g1[i];
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x) {
					double s = 0.0;
					for (int dy = -3; dy <= 3; ++dy)
						for (int dx = -3; dx <= 3; ++dx) {
							const float v = hp[static_cast<size_t>(VpUnit::BandFromBase::ReflectIndex(y + dy, T)) * T + VpUnit::BandFromBase::ReflectIndex(x + dx, T)];
							s += static_cast<double>(v) * v;
						}
					e[static_cast<size_t>(y) * T + x] = static_cast<float>(s / 49.0);
				}
			std::vector<double> okEnergy;
			std::vector<char> ok(n, 0);
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x) {
					const int xm = VpUnit::BandFromBase::ReflectIndex(x - 1, T), xp = VpUnit::BandFromBase::ReflectIndex(x + 1, T);
					const int ym = VpUnit::BandFromBase::ReflectIndex(y - 1, T), yp = VpUnit::BandFromBase::ReflectIndex(y + 1, T);
					const double gx = 0.5 * (g2[static_cast<size_t>(y) * T + xp] - g2[static_cast<size_t>(y) * T + xm]);
					const double gy = 0.5 * (g2[static_cast<size_t>(yp) * T + x] - g2[static_cast<size_t>(ym) * T + x]);
					if (std::sqrt(gx * gx + gy * gy) < 3.0) { ok[static_cast<size_t>(y) * T + x] = 1; okEnergy.push_back(e[static_cast<size_t>(y) * T + x]); }
				}
			if (okEnergy.size() < 64)
				return out;
			const double thr = RobustStats::Quantile(okEnergy, 0.3, RobustStats::RankRule::FloorIndex);
			std::vector<double> residual;
			for (size_t i = 0; i < n; ++i) if (ok[i] && e[i] <= thr) residual.push_back(hp[i]);
			if (residual.size() < 64) return out;
			const double med = RobustStats::Median(residual, RobustStats::MedianRule::Upper);
			out.sigma = RobustStats::MadSigma(RobustStats::MadAbout(residual, med, RobustStats::MedianRule::Upper));
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
					const double sigma = RobustStats::Median(sigmas, RobustStats::MedianRule::Upper);
					lastNoise_ = sigma;
					history_.push_back(sigma);
					while (history_.size() > s_.referenceWindow)
						history_.pop_front();
					if (history_.size() >= s_.referenceMinimum)
					{
						const double ref = RobustStats::Quantile(std::vector<double>(history_.begin(), history_.end()), s_.referenceQuantile, RobustStats::RankRule::FloorIndex);
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
