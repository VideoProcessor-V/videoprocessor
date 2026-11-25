// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Frame-level quality signal for the ABR step detector (doc 61 s2.2):
//   quality = min(delivered resolution term, relative flat-area residual noise term).
// Pure CPU, header-only, C++14, deterministic (fixed reduction orders), no clock.
#include "../../../../core/RobustStats.h"
#include "../../../../core/SkeletonAcquire.h"
#include "../../../repair/grain_split/BandMemo.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
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
			double sigma = 0.0;      // robust sigma of 1-px residual in flattest part (8-bit codes)
			double coverage = 0.0;   // share of tile flat enough to measure
			bool usedBandMemo = false;
			std::string bandMemoFallback;
		};

		inline VpUnit::BandMemo::BandKey TileBandKey(double sigmaPx, int T)
		{
			VpUnit::BandMemo::BandKey k;
			k.resolution = VpUnit::BandMemo::Stage::Full;
			k.width = T > 0 ? static_cast<uint32_t>(T) : 0u;
			k.height = k.width;
			k.sigmaPx = sigmaPx;
			k.law = VpUnit::BandMemo::Law::Taps;
			k.edge = VpUnit::BandFromBase::EdgeMode::Reflect;
			k.factor = 3.0; k.minOne = false; k.eps = 0.0;
			return k;
		}

		namespace detail { inline int Reflect(int i, int n) { return VpUnit::BandFromBase::ReflectIndex(i, n); } }

		// `luma`: T x T samples in 0..1 (8-bit = x255). Optional `memo` + `seq`: CPU kBandMemo with visible BandMemo::CpuBand fallback.
		inline TileNoise MeasureNoise(const float* luma, int T, VpSkeleton::SkeletonMemo* memo = nullptr, uint64_t seq = 0)
		{
			TileNoise out;
			if (!luma || T < 32) return out;
			const size_t n = static_cast<size_t>(T) * T;
			std::vector<float> u(n), g1, g2;
			for (size_t i = 0; i < n; ++i) u[i] = luma[i] * 255.0f;
			const VpUnit::BandMemo::BandKey k1 = TileBandKey(1.0, T), k2 = TileBandKey(2.0, T);
			auto acquireBand = [&](const VpUnit::BandMemo::BandKey& key, std::vector<float>& dst, bool& hit, std::string& why) {
				hit = false;
				if (memo != nullptr && seq != 0u) {
					const VpSkeleton::AcquireResult res = memo->Acquire(VpSkeleton::kBandMemo, seq, VpSkeleton::AcquireForm::Cpu, VpUnit::BandMemo::ToVariant(key));
					if (res.Ok() && res.cpu != nullptr && res.cpuBytes == n * sizeof(float)) {
						dst.resize(n); std::memcpy(dst.data(), res.cpu, res.cpuBytes);
						for (size_t i = 0; i < n; ++i) dst[i] *= 255.0f;
						hit = true; return;
					}
					const std::string detail = memo->LastCause(VpSkeleton::kBandMemo);
					why = std::string("kBandMemo CPU unavailable (") + VpSkeleton::Token(res.cause) + (detail.empty() ? ")" : ": " + detail + ")");
				} else {
					why = memo == nullptr ? "no skeleton memo bound" : "frame sequence 0 (unkeyed)";
				}
				dst = VpUnit::BandMemo::CpuBand<float>(u, T, T, key);
			};
			bool hit1 = false, hit2 = false;
			std::string why1, why2;
			acquireBand(k1, g1, hit1, why1);
			acquireBand(k2, g2, hit2, why2);
			out.usedBandMemo = hit1 && hit2;
			if (!out.usedBandMemo) out.bandMemoFallback = !hit1 ? why1 : why2;
			std::vector<float> hp(n), e(n);
			for (size_t i = 0; i < n; ++i) hp[i] = u[i] - g1[i];
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x) {
					double s = 0.0;
					for (int dy = -3; dy <= 3; ++dy)
						for (int dx = -3; dx <= 3; ++dx) {
							const float v = hp[static_cast<size_t>(detail::Reflect(y + dy, T)) * T + detail::Reflect(x + dx, T)];
							s += static_cast<double>(v) * v;
						}
					e[static_cast<size_t>(y) * T + x] = static_cast<float>(s / 49.0);
				}
			std::vector<double> okEnergy;
			std::vector<char> ok(n, 0);
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x) {
					const int xm = detail::Reflect(x - 1, T), xp = detail::Reflect(x + 1, T);
					const int ym = detail::Reflect(y - 1, T), yp = detail::Reflect(y + 1, T);
					const double gx = 0.5 * (g2[static_cast<size_t>(y) * T + xp] - g2[static_cast<size_t>(y) * T + xm]);
					const double gy = 0.5 * (g2[static_cast<size_t>(yp) * T + x] - g2[static_cast<size_t>(ym) * T + x]);
					if (std::sqrt(gx * gx + gy * gy) < 3.0) { ok[static_cast<size_t>(y) * T + x] = 1; okEnergy.push_back(e[static_cast<size_t>(y) * T + x]); }
				}
			if (okEnergy.size() < 64) return out;
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
