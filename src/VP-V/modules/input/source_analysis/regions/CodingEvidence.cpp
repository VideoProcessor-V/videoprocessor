// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P16 kCodingEvidence: see CodingEvidence.h. MeasureTile, GridBlockiness, BandEdgeBlock, EffectiveBitsOf and ReduceFrame are MOVED
// verbatim from SourceResolutionDetector.cpp (provenance: VP-V r2/job1-base f61d18823, Spectrum coding fields; hunt 2 M2/M3).
#include "pch.h"
#include "CodingEvidence.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <utility>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>

namespace VpUnit
{
namespace CodingEvidence
{
	TileMeasure MeasureTile(const float* luma, int stride, int n, int codeMaximum)
	{
		TileMeasure out;
		// Spec 1b / 2d on the tile's own codes (the tile sits on the frame's
		// 16-pixel grid, so block boundaries fall at 7 mod 8 / 15 mod 16).
		const int codes = (std::max)(1, codeMaximum);
		// W21-2 (R1/R2): the step-derived constants below are written in axis (10-bit) codes; a deeper signal has `axisScale` native codes per
		// axis code (4 at 12 bits), so a quarter-code floor, a 2-code minimum band step and an 8-code maximum band step keep their axis meaning.
		// Exactly 1 at 10 bits and below: 10-bit and 8-bit results are unchanged by construction.
		const int axisScale = codes >= static_cast<int>(Vpv::AnalysisDomain::kAxisMax)
			? (std::max)(1, static_cast<int>(Vpv::AnalysisDomain::NativeCodesPerAxisCode(static_cast<uint32_t>(codes)))) : 1;
		std::vector<int> c(static_cast<size_t>(n) * n);
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
				c[static_cast<size_t>(y) * n + x] = (std::min)(codes, (std::max)(0, static_cast<int>(std::lround(luma[static_cast<size_t>(y) * stride + x] * codes))));
		out.histogram.assign(static_cast<size_t>(codes) + 1, 0u);
		for (int v : c) ++out.histogram[static_cast<size_t>(v)];

		// Blocking: mean absolute step at each position within the 8 and 16
		// pixel periods, both axes; the boundary step against the others.
		double g8[8] = {}, g16[16] = {};
		double count8[8] = {}, count16[16] = {};
		for (int y = 0; y < n; ++y)
			for (int x = 0; x + 1 < n; ++x)
			{
				const double h = std::abs(c[static_cast<size_t>(y) * n + x + 1] - c[static_cast<size_t>(y) * n + x]);
				const double v = std::abs(c[static_cast<size_t>(x + 1) * n + y] - c[static_cast<size_t>(x) * n + y]);
				g8[x % 8] += h + v; count8[x % 8] += 2;
				g16[x % 16] += h + v; count16[x % 16] += 2;
			}
		double inner8 = 0.0, inner16 = 0.0;
		for (int p = 0; p < 7; ++p) inner8 += g8[p] / (std::max)(1.0, count8[p]);
		inner8 /= 7.0;
		for (int p = 0; p < 15; ++p) if (p != 7) inner16 += g16[p] / (std::max)(1.0, count16[p]);
		inner16 /= 14.0;
		const double edge8 = g8[7] / (std::max)(1.0, count8[7]), edge16 = g16[15] / (std::max)(1.0, count16[15]);
		const double floor = 0.25 * axisScale;   // a quarter axis code: a near-flat tile is not 'blocked' by one stray step
		out.blockiness = (std::max)((edge8 + floor) / (inner8 + floor), (edge16 + floor) / (inner16 + floor));
		out.gradient = inner8;

		// Banding: 8x8 blocks holding exactly two adjacent levels (a band edge
		// across a flat area) among all blocks.
		int blocks = 0, bandEdges = 0, flat = 0;
		for (int by = 0; by + 8 <= n; by += 8)
			for (int bx = 0; bx + 8 <= n; bx += 8)
			{
				int lo = codes, hi = 0;
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
					{
						const int v = c[static_cast<size_t>(by + y) * n + bx + x];
						lo = (std::min)(lo, v);
						hi = (std::max)(hi, v);
					}
				++blocks;
				if (hi == lo) { ++flat; continue; }
				bool twoLevels = true;
				for (int y = 0; y < 8 && twoLevels; ++y)
					for (int x = 0; x < 8; ++x)
					{
						const int v = c[static_cast<size_t>(by + y) * n + bx + x];
						if (v != lo && v != hi) { twoLevels = false; break; }
					}
				// One step (up to 8 codes: 7-bit in 10). Planner G17 (2026-10-16): in a 10-bit (or
				// deeper) signal a one-code step is a gentle gradient, invisible - not a band; a
				// band is at least two codes (8-bit content shows four, coarse coding more).
				const int minimumStep = codes >= static_cast<int>(Vpv::AnalysisDomain::kAxisMax) ? 2 * axisScale : 1;
				const int maximumStep = 8 * axisScale;
				if (twoLevels && hi - lo <= maximumStep && hi - lo >= minimumStep) { ++flat; ++bandEdges; }
				else if (twoLevels && hi - lo <= maximumStep) ++flat;
			}
		out.blocks = blocks;
		out.bandEdges = bandEdges;
		out.flatBlocks = flat;

		// Hunt 2 M2/M3: the robust readings, beside the earlier ones (source_coding_fixes).
		const int signalBits = (std::max)(1, static_cast<int>(std::lround(std::log2(codes + 1.0))));
		const int flatRange = 1 << (std::max)(0, signalBits - 5);              // 32 codes at 10 bits, 8 at 8
		// One depth law with MeasureTile's axisScale: at >= 10 bits a band is 2..8 AXIS codes (axisScale native codes each); below, the 7-bit step.
		const bool axisDepth = signalBits >= Vpv::AnalysisDomain::kAxisBits;
		const int bandMaximum = axisDepth ? 8 * axisScale : (std::max)(2, 1 << (std::max)(0, signalBits - 7));   // one 7-bit step
		const int bandMinimum = axisDepth ? 2 * axisScale : 1;                       // G17: a 1-code step at 10 bits is no band
		out.flatHistogram.assign(static_cast<size_t>(codes) + 1, 0u);
		int robustEdges = 0;
		int block[64];
		for (int by = 0; by + 8 <= n; by += 8)
			for (int bx = 0; bx + 8 <= n; bx += 8)
			{
				int lo = codes, hi = 0;
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
					{
						const int v = c[static_cast<size_t>(by + y) * n + bx + x];
						block[y * 8 + x] = v;
						lo = (std::min)(lo, v);
						hi = (std::max)(hi, v);
					}
				if (hi - lo <= flatRange)
					for (int v : block) ++out.flatHistogram[static_cast<size_t>(v)];
				if (BandEdgeBlock(block, bandMinimum, bandMaximum)) ++robustEdges;
			}
		out.bandEdgesRobust = robustEdges;
		std::vector<double> stepsH(static_cast<size_t>(n - 1), 0.0), stepsV(static_cast<size_t>(n - 1), 0.0);
		double signedH = 0.0, signedV = 0.0;
		for (int y = 0; y < n; ++y)
			for (int x = 0; x + 1 < n; ++x)
			{
			{
				const int dh = c[static_cast<size_t>(y) * n + x + 1] - c[static_cast<size_t>(y) * n + x];
				const int dv = c[static_cast<size_t>(x + 1) * n + y] - c[static_cast<size_t>(x) * n + y];
				stepsH[static_cast<size_t>(x)] += std::abs(dh);
				stepsV[static_cast<size_t>(x)] += std::abs(dv);
				signedH += dh;
				signedV += dv;
			}
			}
		for (int x = 0; x + 1 < n; ++x) { stepsH[static_cast<size_t>(x)] /= n; stepsV[static_cast<size_t>(x)] /= n; }
		double periodH = 0.0, periodV = 0.0;
		const double gridH = GridBlockiness(stepsH, periodH), gridV = GridBlockiness(stepsV, periodV);
		// The stronger axis: a coded gradient shows its block edges only across the gradient
		// (hunt 2 lead sweep; the minimum of both axes missed 2x-upscaled grids). The
		// quantisation staircase of a slow gradient repeats at a fixed period too (a step every
		// 1 / slope px), but it is flat between its steps (the median step is zero) and every
		// step has the same sign; such an axis is bands (counted below), not blocking. Coded
		// gradients change between the edges, and flat coded blocks step both ways.
		auto staircase = [&](const std::vector<double>& steps, double signedSum) {
			double total = 0.0;
			for (double v : steps) total += v;
			if (total <= 0.0) return false;
			const double median = RobustStats::Median(steps, RobustStats::MedianRule::Upper);
			const double meanStep = total / steps.size();
			const double coherence = std::fabs(signedSum) / (total * n);
			return median <= 0.1 * meanStep && coherence > 0.9;
		};
		const double usedH = staircase(stepsH, signedH) ? 1.0 : gridH;
		const double usedV = staircase(stepsV, signedV) ? 1.0 : gridV;
		out.blockinessRobust = (std::max)(usedH, usedV);
		out.blockPeriod = usedH >= usedV ? periodH : periodV;
		return out;
	}

	double GridBlockiness(const std::vector<double>& steps, double& period)
	{
		// A coding grid of block size b upscaled by 1 / r has its boundaries every b / r px
		// (8 px native, 16 px for 1080 -> 2160, 24 px for 720 -> 2160, 12 px for 1440 -> 2160):
		// no fixed period. Every period from 8 px to an eighth of the tile (at least eight
		// repeats, so the medians below resist single edges) is tried in quarter-pixel steps,
		// at every phase, with the boundary 1 to 3 px wide (an upscaler spreads it). Per
		// phase bin the median over the repeats; the grid's excess over the bins' median must
		// clear the best other phase's (texture is not periodic) and show in at least three
		// quarters of the repeats (one strong edge is not a grid). Natural texture reads
		// ~1.0-1.15, a coded picture far above (CPU model and tests).
		period = 0.0;
		const int n1 = static_cast<int>(steps.size());
		const double floorCodes = 0.25;   // as the earlier measure: a near-flat tile is not 'blocked'
		double best = 1.0;
		std::vector<std::vector<double>> bins;
		std::vector<double> m, ex;
		std::vector<char> ok;
		for (double P = 8.0; P <= n1 / 8.0 + 1e-9; P += 0.25)
		{
			const int B = static_cast<int>(std::ceil(P - 1e-9));
			bins.assign(static_cast<size_t>(B), {});
			for (int x = 0; x < n1; ++x)
			{
				const int b = (std::min)(B - 1, static_cast<int>(std::fmod(x + 0.5, P)));
				bins[static_cast<size_t>(b)].push_back(steps[static_cast<size_t>(x)]);
			}
			m.assign(static_cast<size_t>(B), 0.0);
			ok.assign(static_cast<size_t>(B), 0);
			std::vector<double> valid;
			for (int b = 0; b < B; ++b)
				if (bins[static_cast<size_t>(b)].size() >= 6)
				{
					ok[static_cast<size_t>(b)] = 1;
					m[static_cast<size_t>(b)] = RobustStats::Median(bins[static_cast<size_t>(b)]);
					valid.push_back(m[static_cast<size_t>(b)]);
				}
			if (valid.size() < 6) continue;
			const double inner = RobustStats::Median(valid);
			ex.assign(static_cast<size_t>(B), 0.0);
			for (int b = 0; b < B; ++b)
				if (ok[static_cast<size_t>(b)]) ex[static_cast<size_t>(b)] = (std::max)(0.0, m[static_cast<size_t>(b)] - inner);
			auto window = [&](int b, int W, double& sum) {
				sum = 0.0;
				for (int j = 0; j < W; ++j)
				{
					const int k = (b + j) % B;
					if (!ok[static_cast<size_t>(k)]) return false;
					sum += ex[static_cast<size_t>(k)];
				}
				return true;
			};
			for (int W = 1; W <= 3; ++W)
				for (int b = 0; b < B; ++b)
				{
					double win = 0.0;
					if (!window(b, W, win) || win <= 0.0) continue;
					// A narrow boundary: the bins either side hold at most half of it (an upscaler's
					// overshoot lobes sit beside a spread step; hunt 2 lead sweep, was a quarter).
					const double side = (std::max)(ex[static_cast<size_t>((b - 1 + B) % B)], ex[static_cast<size_t>((b + W) % B)]);
					if (side > 0.5 * win) continue;
					// Against the best other phase (no overlap).
					double other = 0.0;
					for (int b2 = 0; b2 < B; ++b2)
					{
						const int d = (std::min)((b2 - b + B) % B, (b - b2 + B) % B);
						double w2 = 0.0;
						if (d > W && window(b2, W, w2)) other = (std::max)(other, w2);
					}
					const double ratio = (inner + (std::max)(0.0, win - other) + floorCodes) / (inner + floorCodes);
					if (ratio <= best) continue;
					// In most repeats: each repeat's own excess over the window.
					int repeats = 0, strong = 0, last = INT_MIN;
					double sum = 0.0;
					for (int x = 0; x < n1; ++x)
					{
						const int bx = (std::min)(B - 1, static_cast<int>(std::fmod(x + 0.5, P)));
						const int d = (bx - b + B) % B;
						if (d >= W) continue;
						const int k = static_cast<int>(std::floor((x + 0.5 - b) / P));
						if (k != last)
						{
							if (last != INT_MIN) { ++repeats; strong += sum >= 0.5 * win; }
							last = k;
							sum = 0.0;
						}
						sum += steps[static_cast<size_t>(x)] - inner;
					}
					if (last != INT_MIN) { ++repeats; strong += sum >= 0.5 * win; }
					if (repeats < 6 || strong < 0.75 * repeats) continue;
					best = ratio;
					period = P;
				}
		}
		return best;
	}

	bool BandEdgeBlock(const int* v, int minimumStep, int maximumStep)
	{
		int lo = v[0], hi = v[0];
		for (int i = 1; i < 64; ++i) { lo = (std::min)(lo, v[i]); hi = (std::max)(hi, v[i]); }
		if (hi == lo || hi - lo > 3 * maximumStep) return false;
		// The two most frequent levels.
		int values[64], counts[64], distinct = 0;
		for (int i = 0; i < 64; ++i)
		{
			int k = 0;
			while (k < distinct && values[k] != v[i]) ++k;
			if (k == distinct) { values[distinct] = v[i]; counts[distinct] = 0; ++distinct; }
			++counts[k];
		}
		if (distinct < 2) return false;
		int first = 0, second = -1;
		for (int k = 1; k < distinct; ++k)
			if (counts[k] > counts[first] || (counts[k] == counts[first] && values[k] < values[first])) first = k;
		for (int k = 0; k < distinct; ++k)
			if (k != first && (second < 0 || counts[k] > counts[second] || (counts[k] == counts[second] && values[k] < values[second]))) second = k;
		const int a = (std::min)(values[first], values[second]), b = (std::max)(values[first], values[second]);
		const int step = b - a;
		if (step < minimumStep || step > maximumStep) return false;
		if (counts[first] + counts[second] < 0.6 * 64) return false;   // dominant: not a gradient
		const int tolerance = (std::max)(1, step / 4);                   // coding noise around the levels
		int inside = 0;
		for (int i = 0; i < 64; ++i) inside += v[i] >= a - tolerance && v[i] <= b + tolerance;
		if (inside < 0.85 * 64) return false;
		// Split along a line, not salt and pepper (grain between two codes).
		int cls[64], high = 0;
		for (int i = 0; i < 64; ++i) { cls[i] = 2 * v[i] > a + b; high += cls[i]; }
		if (high < 8 || 64 - high < 8) return false;
		int major[64];
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
			{
				int s = 0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						s += cls[(std::min)(7, (std::max)(0, y + dy)) * 8 + (std::min)(7, (std::max)(0, x + dx))];
				major[y * 8 + x] = s >= 5;
			}
		int transitions = 0;
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
			{
				if (x + 1 < 8) transitions += major[y * 8 + x] != major[y * 8 + x + 1];
				if (y + 1 < 8) transitions += major[y * 8 + x] != major[(y + 1) * 8 + x];
			}
		return transitions <= 20;
	}

	double EffectiveBitsOf(const std::vector<unsigned long long>& histogram)
	{
		if (histogram.size() < 64) return 0.0;
		const size_t windows = histogram.size() / 16;
		std::vector<double> population(windows, 0.0);
		for (size_t w = 0; w < windows; ++w)
			for (size_t i = w * 16; i < w * 16 + 16; ++i) population[w] += static_cast<double>(histogram[i]);
		std::vector<double> shares;
		for (size_t w = 1; w + 1 < windows; ++w)
		{
			const double pop = population[w];
			if (pop < 16.0 * 16.0) continue;
			// Judged only where the histogram's envelope is smooth on this scale (both
			// neighbouring windows within 3x): a narrow peak (a flat area with grain, true
			// 10-bit) fills only a few codes of its window without any coarse step.
			bool smooth = true;
			for (size_t k : { w - 1, w + 1 })
				smooth = smooth && population[k] >= pop / 3.0 && population[k] <= pop * 3.0;
			if (!smooth) continue;
			// The window's occupancy as a Hill number of order 4, (sum p^4)^(-1/3) / 16:
			// equally used codes count fully, codes holding few pixels (an upscale's ramp
			// values, a few dithered pixels) little. CPU model: 8-bit upscaled 2x into 10
			// bits reads ~8.8 (one-pixel occupancy: 10), true 10-bit ~10, 8-bit via PQ ~8.8.
			double fourth = 0.0;
			for (size_t i = w * 16; i < w * 16 + 16; ++i)
			{
				const double p = static_cast<double>(histogram[i]) / pop;
				fourth += (p * p) * (p * p);
			}
			if (fourth > 0.0) shares.push_back(std::pow(fourth, -1.0 / 3.0) / 16.0);
		}
		if (shares.empty()) return 0.0;
		const double share = (std::max)(1.0 / 16.0, (std::min)(1.0, RobustStats::Median(std::move(shares), RobustStats::MedianRule::Upper)));
		return std::log2(static_cast<double>(histogram.size())) + std::log2(share);
	}

	FrameCoding ReduceFrame(const std::vector<const TileMeasure*>& tiles, const std::vector<char>& eligible, bool codingFixes)
	{
		FrameCoding frame;
		if (codingFixes)
		{
			// Hunt 2 M2/M3 (source_coding_fixes): effective bits from the low-activity blocks
			// (all pixels when they are too few), by occupancy (Hill number); blocking at the grid's
			// own period; bands with two dominant levels. Exactly periodic tiles (planner Q5) are
			// no blocking evidence: their own repeat looks like a grid.
			std::vector<unsigned long long> flat, all;
			double blockSum = 0.0, blockWeight = 0.0;
			long long blocks = 0, bandEdges = 0;
			for (size_t ti = 0; ti < tiles.size(); ++ti)
			{
				const TileMeasure& s = *tiles[ti];
				if (all.size() < s.histogram.size()) all.resize(s.histogram.size(), 0ull);
				for (size_t i = 0; i < s.histogram.size(); ++i) all[i] += s.histogram[i];
				if (flat.size() < s.flatHistogram.size()) flat.resize(s.flatHistogram.size(), 0ull);
				for (size_t i = 0; i < s.flatHistogram.size(); ++i) flat[i] += s.flatHistogram[i];
				if (s.gradient > 0.05 && ti < eligible.size() && eligible[ti]) { blockSum += s.blockinessRobust; blockWeight += 1.0; }
				blocks += s.blocks;
				bandEdges += s.bandEdgesRobust;
			}
			unsigned long long flatPixels = 0;
			for (unsigned long long v : flat) flatPixels += v;
			// Enough low-activity pixels to judge several windows: a picture this flat is where
			// banding shows; otherwise the whole histogram.
			const double flatBits = flatPixels >= 16ull * 16ull * 4ull ? EffectiveBitsOf(flat) : 0.0;
			frame.effectiveBits = flatBits > 0.0 ? flatBits : EffectiveBitsOf(all);
			frame.blockiness = blockWeight > 0.0 ? blockSum / blockWeight : 1.0;
			frame.bandShare = blocks > 0 ? static_cast<double>(bandEdges) / blocks : 0.0;
			const double blocked = (std::min)(1.0, (std::max)(0.0, (frame.blockiness - 1.1) / 0.6));
			const double banded = (std::min)(1.0, (std::max)(0.0, frame.bandShare / 0.2));
			frame.bitrateQuality = 1.0 - (std::max)(blocked, banded);
			return frame;
		}

		// Effective bit depth: the share of codes in use. Content made with
		// fewer levels leaves gaps however it was mapped since (8-bit x 4, or
		// through a curve such as SDR re-encoded to PQ). Windows of 16 codes
		// with enough pixels to fill every code (16 per code) are judged; the
		// median occupied share s gives bits = log2(levels) + log2(s).
		std::vector<unsigned long long> histogram;
		double blockSum = 0.0, blockWeight = 0.0;
		long long blocks = 0, bandEdges = 0;
		for (size_t ti = 0; ti < tiles.size(); ++ti)
		{
			const TileMeasure& s = *tiles[ti];
			if (histogram.size() < s.histogram.size()) histogram.resize(s.histogram.size(), 0ull);
			for (size_t i = 0; i < s.histogram.size(); ++i) histogram[i] += s.histogram[i];
			// Blocking from tiles with some variation (a flat tile has no steps to compare).
			if (s.gradient > 0.05) { blockSum += s.blockiness; blockWeight += 1.0; }
			blocks += s.blocks;
			bandEdges += s.bandEdges;
		}
		if (histogram.size() >= 64)
		{
			std::vector<double> shares;
			for (size_t w = 0; w + 16 <= histogram.size(); w += 16)
			{
				unsigned long long pixels = 0;
				int used = 0;
				for (size_t i = w; i < w + 16; ++i) { pixels += histogram[i]; used += histogram[i] > 0; }
				if (pixels >= 16ull * 16ull) shares.push_back(used / 16.0);
			}
			if (!shares.empty())
			{
				const double share = (std::max)(1.0 / 16.0, RobustStats::Median(std::move(shares), RobustStats::MedianRule::Upper));
				frame.effectiveBits = std::log2(static_cast<double>(histogram.size())) + std::log2(share);
			}
		}
		frame.blockiness = blockWeight > 0.0 ? blockSum / blockWeight : 1.0;
		frame.bandShare = blocks > 0 ? static_cast<double>(bandEdges) / blocks : 0.0;
		// 1 = clean; blocking from 1.1 (none visible) to 1.7 (heavy); banding
		// from no band-edge blocks to a fifth of all blocks.
		const double blocked = (std::min)(1.0, (std::max)(0.0, (frame.blockiness - 1.1) / 0.6));
		const double banded = (std::min)(1.0, (std::max)(0.0, frame.bandShare / 0.2));
		frame.bitrateQuality = 1.0 - (std::max)(blocked, banded);
		return frame;
	}

	// ---- the product's bytes ----
	namespace
	{
		constexpr uint32_t kMagic = 0x31454350u;   // "PCE1"
		struct Header { uint32_t magic, version, tiles, tileSize, codeMaximum; };
		template <class T> void Put(std::vector<uint8_t>& out, const T& v)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
			out.insert(out.end(), p, p + sizeof(T));
		}
		template <class T> bool Take(const uint8_t*& p, const uint8_t* end, T& v)
		{
			if (size_t(end - p) < sizeof(T)) return false;
			std::memcpy(&v, p, sizeof(T));
			p += sizeof(T);
			return true;
		}
		void PutArray(std::vector<uint8_t>& out, const std::vector<unsigned>& a)
		{
			Put(out, uint32_t(a.size()));
			const uint8_t* p = reinterpret_cast<const uint8_t*>(a.data());
			out.insert(out.end(), p, p + a.size() * sizeof(unsigned));
		}
		bool TakeArray(const uint8_t*& p, const uint8_t* end, std::vector<unsigned>& a)
		{
			uint32_t n = 0;
			if (!Take(p, end, n) || size_t(end - p) / sizeof(unsigned) < n) return false;
			a.resize(n);
			if (n != 0) std::memcpy(a.data(), p, size_t(n) * sizeof(unsigned));
			p += size_t(n) * sizeof(unsigned);
			return true;
		}
	}

	std::vector<uint8_t> Serialise(const std::vector<TileMeasure>& tiles, int tileSize, int codeMaximum)
	{
		std::vector<uint8_t> out;
		const Header h{ kMagic, kUnitVersion, uint32_t(tiles.size()), uint32_t(tileSize), uint32_t(codeMaximum) };
		Put(out, h);
		for (const TileMeasure& t : tiles)
		{
			Put(out, t.blockiness); Put(out, t.gradient); Put(out, t.blockinessRobust); Put(out, t.blockPeriod);
			Put(out, int32_t(t.blocks)); Put(out, int32_t(t.bandEdges)); Put(out, int32_t(t.flatBlocks)); Put(out, int32_t(t.bandEdgesRobust));
			PutArray(out, t.histogram);
			PutArray(out, t.flatHistogram);
		}
		return out;
	}

	bool Deserialise(const uint8_t* bytes, size_t size, std::vector<TileMeasure>& tiles)
	{
		tiles.clear();
		const uint8_t* p = bytes;
		const uint8_t* end = bytes + size;
		Header h{};
		if (bytes == nullptr || !Take(p, end, h) || h.magic != kMagic || h.version != kUnitVersion) return false;
		if (h.tiles > size) return false;   // a record is larger than a byte: a corrupt count cannot allocate
		tiles.resize(h.tiles);
		for (TileMeasure& t : tiles)
		{
			int32_t a = 0, b = 0, c = 0, d = 0;
			if (!Take(p, end, t.blockiness) || !Take(p, end, t.gradient) || !Take(p, end, t.blockinessRobust) || !Take(p, end, t.blockPeriod)
				|| !Take(p, end, a) || !Take(p, end, b) || !Take(p, end, c) || !Take(p, end, d)
				|| !TakeArray(p, end, t.histogram) || !TakeArray(p, end, t.flatHistogram))
			{
				tiles.clear();
				return false;
			}
			t.blocks = a; t.bandEdges = b; t.flatBlocks = c; t.bandEdgesRobust = d;
		}
		return true;
	}

	// ---- the hub and the owner fill ----
	bool MeasurePack(const TilePack& pack, std::vector<TileMeasure>& tiles)
	{
		tiles.clear();
		if (pack.tiles == 0)
			return true;   // nothing to measure (an empty frame): an empty, valid reading
		if (pack.tiles < 0 || pack.tileSize <= 0 || pack.luma.size() < size_t(pack.tiles) * size_t(pack.tileSize) * size_t(pack.tileSize))
			return false;
		tiles.resize(static_cast<size_t>(pack.tiles));
		const size_t pixels = size_t(pack.tileSize) * size_t(pack.tileSize);
		for (int t = 0; t < pack.tiles; ++t)
			tiles[static_cast<size_t>(t)] = MeasureTile(pack.luma.data() + size_t(t) * pixels, pack.tileSize, pack.tileSize, pack.codeMaximum);
		return true;
	}

	Hub& Hub::Instance()
	{
		// Leaked on purpose: never destroyed, so ~SkeletonModule during static destruction (ExtensionHub's function-static outlives a
		// function-static first touched inside a module constructor) never reaches a destroyed hub (audit-P16 SHOULD 1).
		static Hub* const hub = new Hub();
		return *hub;
	}

	uint64_t Hub::SetMemoProvider(MemoProvider provider)
	{
		std::unique_lock<std::shared_timed_mutex> g(providerMutex_);   // waits for in-flight provider calls
		provider_ = std::move(provider);
		providerToken_ = provider_ ? ++nextToken_ : 0;
		return providerToken_;
	}

	void Hub::ClearMemoProvider(uint64_t token)
	{
		std::unique_lock<std::shared_timed_mutex> g(providerMutex_);   // waits for in-flight provider calls
		if (token == 0 || token != providerToken_)
			return;   // another (newer) installation: kept
		provider_ = nullptr;
		providerToken_ = 0;
	}

	bool Hub::MeasureWithoutMemo(const TilePack& pack, std::vector<TileMeasure>& tiles)
	{
		if (!MeasurePack(pack, tiles))
			return false;
		withoutMemo_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void Hub::Stage(uint64_t seq, std::shared_ptr<const TilePack> pack)
	{
		std::lock_guard<std::mutex> g(mutex_);
		staged_[seq] = std::move(pack);
		while (staged_.size() > 8) staged_.erase(staged_.begin());   // bounded: the oldest unclaimed pack is dropped first
	}

	std::shared_ptr<const TilePack> Hub::Staged(uint64_t seq) const
	{
		std::lock_guard<std::mutex> g(mutex_);
		const auto it = staged_.find(seq);
		return it == staged_.end() ? nullptr : it->second;
	}

	void Hub::Unstage(uint64_t seq)
	{
		std::lock_guard<std::mutex> g(mutex_);
		staged_.erase(seq);
	}

	bool Hub::AcquireTiles(const void* host, uint64_t seq, std::shared_ptr<const TilePack> pack, std::vector<TileMeasure>& tiles, std::string& why)
	{
		tiles.clear();
		std::shared_ptr<VpSkeleton::SkeletonMemo> memo;
		{
			// Called under the shared lock: the module that installed the provider cannot finish its ClearMemoProvider (in its
			// destructor) while this call runs, so the lambda's `this` is alive for the whole call (audit-P16 SHOULD 1).
			std::shared_lock<std::shared_timed_mutex> g(providerMutex_);
			if (provider_) memo = provider_(host);
		}
		if (!memo)
		{
			why = "no skeleton memo";
			return false;
		}
		Stage(seq, std::move(pack));
		const VpSkeleton::AcquireResult r = memo->Acquire(VpSkeleton::kCodingEvidence, seq, VpSkeleton::AcquireForm::Cpu);
		Unstage(seq);
		if (!r.Ok() || r.cpu == nullptr)
		{
			why = VpSkeleton::Token(r.cause);
			return false;
		}
		if (!Deserialise(r.cpu, r.cpuBytes, tiles))
		{
			why = "coding evidence bytes unreadable";
			return false;
		}
		return true;
	}

	VpSkeleton::OwnerFill MakeOwnerFill(Hub& hub, uint8_t keepFrames)
	{
		VpSkeleton::OwnerFill f;
		f.keepFrames = keepFrames;
		Hub* h = &hub;
		f.fill = [h](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
			const std::shared_ptr<const TilePack> pack = h->Staged(req.seq);
			std::vector<TileMeasure> tiles;
			if (!pack || pack->tiles <= 0 || !MeasurePack(*pack, tiles))
				return false;   // nothing staged for this frame: declined (visible)
			out.bytes = Serialise(tiles, pack->tileSize, pack->codeMaximum);
			VPV_COMPUTE_ONCE_AT("U13", req.seq, ::ComputeOnce::Variant(0));   // compute-once detector 2 (15d J2): P16 default variant, counted only when computed
			return true;
		};
		return f;
	}
}
}
