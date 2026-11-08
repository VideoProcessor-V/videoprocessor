// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Scaling-chain estimator (SID-C1), shared vocabulary: constants, reason bits, the line container and a few
// deterministic helpers. Pure standard-library C++14: no clock, no threads, no Windows, no unordered
// containers, no random device. Doubles everywhere; every loop and reduction has a fixed order, so the same
// input gives bit-identical output.
//
// Axis convention (as the Indicator port): axis 0 = horizontal (the lines are picture ROWS and run along x),
// axis 1 = vertical (the lines are picture COLUMNS and run along y). All coordinates along an axis are
// relative to the start of the ACTIVE rectangle (bars excluded), never to the frame.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		constexpr int kMaxLines = 64;       // lines per axis the containers are sized for
		constexpr int kMaxSegs = 32;        // 256-px segments per line (a line longer than 8192 px merges its tail)
		constexpr int kSegPx = 256;
		constexpr int kMaxCandidates = 8;   // candidate latent sizes per axis and evaluation
		constexpr double kPi = 3.14159265358979323846;

		// Reason bits returned with every product of this module (never a bare "no").
		constexpr uint32_t kReasonNone = 0u;
		constexpr uint32_t kReasonLowDetail = 1u << 0;     // line gate: fewer informative lines than required
		constexpr uint32_t kReasonFlat = 1u << 1;          // probe unseen, picture is flat
		constexpr uint32_t kReasonDark = 1u << 2;          // probe unseen, picture is dark
		constexpr uint32_t kReasonSoft = 1u << 3;          // probe unseen, picture has structure but no fine detail
		constexpr uint32_t kReasonNoPower = 1u << 4;       // probe did not see its own synthetic upscale: native claims abstain
		constexpr uint32_t kReasonMixed = 1u << 5;         // more than the mask limit of lines/segments contradict the majority: HOLD hint
		constexpr uint32_t kReasonMasked = 1u << 6;        // some contradicting lines were masked out of the statistic
		constexpr uint32_t kReasonNoCandidates = 1u << 7;  // nothing to verify
		constexpr uint32_t kReasonShortLines = 1u << 8;    // lines too short for the operators (margins)
		constexpr uint32_t kReasonOneAxisAbstains = 1u << 9;
		constexpr uint32_t kReasonBadInput = 1u << 10;
		constexpr uint32_t kReasonNoReference = 1u << 11;  // fewer than two valid baseline sizes around a candidate
		constexpr uint32_t kReasonCoarseOnly = 1u << 12;   // candidate ranked by the coarse scan only (not verified at full lines)

		// One axis worth of sampled lines in 10-bit code units (8-bit planes are scaled by 4 by the caller).
		struct LineSet
		{
			int axis = 0;          // 0 horizontal (rows), 1 vertical (columns)
			int axisLen = 0;       // N: active-rectangle size along the axis, pixels
			int origin = 0;        // offset of the first sample from the active-rectangle start, pixels
			int length = 0;        // L: samples per line
			int count = 0;         // number of lines
			std::vector<double> data;   // count x length, row-major
			std::vector<int> cross;     // cross coordinate of every line (active-rect relative), informational

			void Resize(int lines, int len)
			{
				count = lines;
				length = len;
				data.assign(static_cast<size_t>(lines) * static_cast<size_t>(len), 0.0);
				cross.assign(static_cast<size_t>(lines), 0);
			}
			const double* Line(int j) const { return data.data() + static_cast<size_t>(j) * static_cast<size_t>(length); }
			double* Line(int j) { return data.data() + static_cast<size_t>(j) * static_cast<size_t>(length); }
		};

		inline int GcdInt(int a, int b)
		{
			a = a < 0 ? -a : a;
			b = b < 0 ? -b : b;
			while (b != 0)
			{
				const int t = a % b;
				a = b;
				b = t;
			}
			return a;
		}

		// Murmur3 finalizer: a fixed, platform independent integer hash.
		inline uint32_t Hash32(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x85ebca6bu;
			x ^= x >> 13;
			x *= 0xc2b2ae35u;
			x ^= x >> 16;
			return x;
		}

		inline uint32_t HashMix(uint64_t seq, uint32_t salt)
		{
			const uint32_t lo = static_cast<uint32_t>(seq & 0xffffffffull);
			const uint32_t hi = static_cast<uint32_t>(seq >> 32);
			return Hash32(Hash32(lo ^ 0x9e3779b9u) + Hash32(hi + 0x7f4a7c15u) * 31u + salt * 0x85ebca77u);
		}

		// Median of n values (n >= 1); copies and sorts, so the input is untouched. Even n: mean of the two middle values.
		inline double MedianOf(const double* v, int n)
		{
			if (n <= 0)
				return 0.0;
			std::vector<double> t(v, v + n);
			std::sort(t.begin(), t.end());
			if ((n & 1) != 0)
				return t[static_cast<size_t>(n / 2)];
			return 0.5 * (t[static_cast<size_t>(n / 2 - 1)] + t[static_cast<size_t>(n / 2)]);
		}

		// Mean after dropping floor(trim * n) values from each end of the sorted sample (ascending summation order).
		inline double TrimmedMeanOf(const double* v, int n, double trim)
		{
			if (n <= 0)
				return 0.0;
			std::vector<double> t(v, v + n);
			std::sort(t.begin(), t.end());
			int k = static_cast<int>(std::floor(trim * static_cast<double>(n)));
			if (k < 0)
				k = 0;
			if (n - 2 * k <= 0)
				k = 0;
			double s = 0.0;
			for (int i = k; i < n - k; ++i)
				s += t[static_cast<size_t>(i)];
			return s / static_cast<double>(n - 2 * k);
		}

		inline double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

		inline int RoundToInt(double x) { return static_cast<int>(std::floor(x + 0.5)); }
	}
}
