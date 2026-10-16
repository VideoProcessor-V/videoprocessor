#include "pch.h"
#include "SourceResolutionDetector.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <cstdio>

namespace SourceResolution
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;

		bool PowerOfTwo(int n) { return n >= 64 && n <= 1024 && (n & (n - 1)) == 0; }

		// In-place iterative radix-2 FFT.
		void Fft(std::vector<std::complex<double>>& a)
		{
			const size_t n = a.size();
			for (size_t i = 1, j = 0; i < n; ++i)
			{
				size_t bit = n >> 1;
				for (; j & bit; bit >>= 1) j ^= bit;
				j ^= bit;
				if (i < j) std::swap(a[i], a[j]);
			}
			for (size_t len = 2; len <= n; len <<= 1)
			{
				const double angle = -2.0 * kPi / static_cast<double>(len);
				const std::complex<double> wl(std::cos(angle), std::sin(angle));
				for (size_t i = 0; i < n; i += len)
				{
					std::complex<double> w(1.0, 0.0);
					for (size_t k = 0; k < len / 2; ++k)
					{
						const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
						a[i + k] = u + v;
						a[i + k + len / 2] = u - v;
						w *= wl;
					}
				}
			}
		}

		// Bins at multiples of 1/8 and 1/16 cycles per pixel carry block-coding
		// edges (8x8 / 16x16 transforms), not picture detail: left out.
		bool BlockHarmonic(int bin, int n)
		{
			const int period = n / 16;
			if (period < 2) return false;
			const int r = bin % period;
			return r == 0 || r == 1 || r == period - 1;
		}
	}

	namespace
	{
		// Overlay text: sharp steps across (|a - b| >= a third of the range) counted per row;
		// the share of them in the densest band of n / 4 rows. Text drawn at the output
		// resolution (white strokes, dark edges) is a band of many such steps; an upscale
		// spreads every edge over two or more pixels and natural detail rarely concentrates.
		double TextBand(const float* luma, int stride, int n)
		{
			std::vector<int> rows(static_cast<size_t>(n), 0);
			long total = 0;
			for (int y = 0; y < n; ++y)
			{
				const float* row = luma + static_cast<size_t>(y) * stride;
				int count = 0;
				for (int x = 1; x < n; ++x) count += std::fabs(row[x] - row[x - 1]) >= 1.0f / 3.0f ? 1 : 0;
				rows[static_cast<size_t>(y)] = count;
				total += count;
			}
			if (total < static_cast<long>(n) * n / 500) return 0.0;   // under 0.2 %: nothing to judge
			const int band = n / 4;
			long sum = 0, best = 0;
			for (int y = 0; y < n; ++y)
			{
				sum += rows[static_cast<size_t>(y)];
				if (y >= band) sum -= rows[static_cast<size_t>(y - band)];
				best = (std::max)(best, sum);
			}
			return static_cast<double>(best) / total;
		}
	}

	bool Analyse(const float* luma, int stride, int n, Spectrum& out, int codeMaximum)
	{
		if (!luma || !PowerOfTwo(n) || stride < n) return false;
		out.n = n;
		out.textBand = TextBand(luma, stride, n);
		out.vertical.assign(n / 2 + 1, 0.0);
		out.horizontal.assign(n / 2 + 1, 0.0);
		std::vector<double> window(n);
		double windowEnergy = 0.0;
		for (int i = 0; i < n; ++i)
		{
			window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / n);
			windowEnergy += window[i] * window[i];
		}
		double total = 0.0;
		std::vector<std::complex<double>> line(n);
		const int bins = n / 2 + 1;
		std::vector<std::complex<double>> lines(static_cast<size_t>(n) * bins);
		// Columns (vertical frequencies) then rows (horizontal).
		for (int pass = 0; pass < 2; ++pass)
		{
			std::vector<double>& power = pass == 0 ? out.vertical : out.horizontal;
			std::vector<double>& mirror = pass == 0 ? out.mirrorVertical : out.mirrorHorizontal;
			for (int l = 0; l < n; ++l)
			{
				double mean = 0.0;
				for (int i = 0; i < n; ++i)
					mean += pass == 0 ? luma[static_cast<size_t>(i) * stride + l] : luma[static_cast<size_t>(l) * stride + i];
				mean /= n;
				if (pass == 0) total += mean;
				for (int i = 0; i < n; ++i)
				{
					const double v = pass == 0 ? luma[static_cast<size_t>(i) * stride + l] : luma[static_cast<size_t>(l) * stride + i];
					line[i] = std::complex<double>((v - mean) * window[i], 0.0);
				}
				Fft(line);
				for (int k = 0; k <= n / 2; ++k)
				{
					power[k] += std::norm(line[k]) * (k == 0 || k == n / 2 ? 1.0 : 2.0);
					lines[static_cast<size_t>(l) * bins + k] = line[k];
				}
			}
			// White noise of variance v: sum over lines of |X|^2 = n * v * windowEnergy per bin
			// (one-sided doubling gives 2v); normalise to v per bin.
			for (double& p : power) p /= static_cast<double>(n) * windowEnergy * 2.0;

			// Mirror coherence. A picture resampled from samples at rate r (per
			// output pixel) has a spectrum periodic in r: its content at f and
			// at r - f are copies, Y(f) Y(r - f) = H(f) H(r - f) |X|^2 e^(i phi)
			// with one phase phi for every f and every line (it depends only on
			// where the source grid sits). Real detail at the two frequencies is
			// independent: random phases, coherence ~ 1 / sqrt(terms). For each
			// candidate r = m / n: |sum Y(k) Y(m - k)| / sum |Y(k) Y(m - k)| over
			// the lines and the bins just below r / 2 (bins within 2 of the
			// mirror point share the window's main lobe and are left out).
			mirror.assign(n, 0.0);
			for (int m = n / 8; m < n; ++m)
			{
				const int half = m / 2;
				const int band = (std::max)(4, static_cast<int>(std::lround(0.3 * half)));
				const int kFrom = (std::max)({ 2, half - band, m - n / 2 });
				const int kTo = (m - 5) / 2;   // m - k - k >= 5
				if (kTo - kFrom < 2) continue;
				std::complex<double> sum(0.0, 0.0);
				double magnitude = 0.0;
				for (int k = kFrom; k <= kTo; ++k)
				{
					if (BlockHarmonic(k, n) || BlockHarmonic(m - k, n)) continue;
					for (int l = 0; l < n; ++l)
					{
						const std::complex<double> product = lines[static_cast<size_t>(l) * bins + k] * lines[static_cast<size_t>(l) * bins + (m - k)];
						sum += product;
						magnitude += std::abs(product);
					}
				}
				mirror[m] = magnitude > 0.0 ? std::abs(sum) / magnitude : 0.0;
			}
		}
		out.mean = total / n;
		MeasureCoding(luma, stride, n, codeMaximum, out);
		return true;
	}

	void MeasureCoding(const float* luma, int stride, int n, int codeMaximum, Spectrum& out)
	{
		// Spec 1b / 2d on the tile's own codes (the tile sits on the frame's
		// 16-pixel grid, so block boundaries fall at 7 mod 8 / 15 mod 16).
		const int codes = (std::max)(1, codeMaximum);
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
		const double floor = 0.25;   // a quarter code: a near-flat tile is not 'blocked' by one stray step
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
				const int minimumStep = codes >= 1023 ? 2 : 1;
				if (twoLevels && hi - lo <= 8 && hi - lo >= minimumStep) { ++flat; ++bandEdges; }
				else if (twoLevels && hi - lo <= 8) ++flat;
			}
		out.blocks = blocks;
		out.bandEdges = bandEdges;
		out.flatBlocks = flat;
	}

	double Peakiness(const Spectrum& spectrum)
	{
		const int n = spectrum.n;
		if (!PowerOfTwo(n) || spectrum.vertical.size() != static_cast<size_t>(n / 2 + 1)) return 0.0;
		double result = 0.0;
		for (const std::vector<double>* power : { &spectrum.vertical, &spectrum.horizontal })
		{
			std::vector<double> band;
			double peak = 0.0;
			for (int k = n / 16; k < n / 2; ++k)
			{
				if (BlockHarmonic(k, n)) continue;
				band.push_back((*power)[static_cast<size_t>(k)]);
				peak = (std::max)(peak, (*power)[static_cast<size_t>(k)]);
			}
			if (band.empty()) continue;
			std::nth_element(band.begin(), band.begin() + band.size() / 2, band.end());
			const double median = band[band.size() / 2];
			result = (std::max)(result, median > 0.0 ? peak / median : 0.0);
		}
		return result;
	}

	bool Informative(const Spectrum& spectrum, const Thresholds& t)
	{
		// The lower picture band stands well above the quantisation floor
		// (variance step^2 / 12) on both axes: flat or dark tiles carry no evidence.
		const int n = spectrum.n;
		if (!PowerOfTwo(n) || spectrum.vertical.size() != static_cast<size_t>(n / 2 + 1) || spectrum.horizontal.size() != spectrum.vertical.size())
			return false;
		const double step = 1.0 / (std::max)(1, t.codeMaximum);
		const double floor = std::log10(step * step / 12.0);
		for (const std::vector<double>* power : { &spectrum.vertical, &spectrum.horizontal })
		{
			double sum = 0.0;
			int count = 0;
			for (int k = (std::max)(2, n / 32); k <= n / 8; ++k)
				if (!BlockHarmonic(k, n)) { sum += std::log10((std::max)((*power)[k], 1e-20)); ++count; }
			if (count == 0 || sum / count < floor + t.informativeDecades) return false;
		}
		return true;
	}

	MirrorPeak FindPeak(const std::vector<double>& curve, int n, const Thresholds& t)
	{
		MirrorPeak peak;
		if (!PowerOfTwo(n) || curve.size() != static_cast<size_t>(n)) return peak;
		const int from = (std::max)(n / 8, static_cast<int>(std::ceil(t.minimumRatio * n)));
		const int to = (std::min)(n - 1, static_cast<int>(std::floor(t.maximumRatio * n)));
		// The 8x8 block grid (coding edges) is coherent at every multiple of
		// 1/8: a candidate there counts only when it stands clear of the other
		// grid positions (an upscale is coherent at its own rate alone).
		auto level = [&](int m) {
			double v = 0.0;
			for (int j = m - 2; j <= m + 2; ++j) if (j >= 0 && j < n) v = (std::max)(v, curve[j]);
			return v;
		};
		auto gridIndex = [&](int m) {
			const int j = static_cast<int>(std::lround(8.0 * m / n));
			return std::abs(m - j * n / 8) <= 2 ? j : -1;
		};
		// Coding edges light the grid positions unevenly (7/8 strongest), so a
		// grid candidate counts only when the rest of the grid is quiet - no
		// coding signature at all - and it stands clear of it.
		peak.grid = level(7 * n / 8);   // the coding signature (see allowed)
		auto gridQuietExcept = [&](int j) {
			double others = 0.0;
			for (int g = 2; g <= 7; ++g) if (g != j) others = (std::max)(others, level(g * n / 8));
			return others;
		};
		// Block edges make grid position j/8 coherent in proportion to the
		// block pattern's (8 - j)th harmonic: 7/8 follows its fundamental, so
		// 7/8 is the coding signature itself and never read as an upscale (a
		// source that really is 7/8 of the height stays native - the safe way).
		// Variable-bitrate streams (owner 2026-10-16): HEVC / AV1 code with 16x16
		// and 32x32 transforms too, whose edges light the odd j/16 and j/32 the
		// same way the 8x8 grid lights j/8 - (P-1)/P is each grid's signature
		// (found: 15/16 = 0.937 and 29/32 = 0.906 on coded 1080 streams, so the
		// frame read native and would have disengaged auto). (P-1)/P is never
		// accepted; another odd position only when that grid is quiet or it
		// stands above both of its grid neighbours.
		auto allowedOnGrid = [&](int m, int P) {
			if (n / P < 4) return true;   // too few bins per grid step to tell
			const int j = static_cast<int>(std::lround(static_cast<double>(P) * m / n));
			if (j % 2 == 0 || std::abs(m - j * n / P) > 1) return true;
			if (j == P - 1) return false;
			if (level((P - 1) * n / P) < t.gridQuiet) return true;
			const double neighbours = (std::max)(level((j - 2) * n / P), j + 2 < P ? level((j + 2) * n / P) : 0.0);
			return curve[m] >= neighbours + t.gridPeakMargin;
		};
		auto allowed = [&](int m) {
			if (!allowedOnGrid(m, 16) || !allowedOnGrid(m, 32)) return false;
			const int j = gridIndex(m);
			if (j <= 0) return true;
			if (j == 7) return false;
			const double others = gridQuietExcept(j);
			if (others < t.gridQuiet && curve[m] >= others + t.gridMargin) return true;
			// With coding present the grid rises steadily towards 7/8 (each
			// position follows a lower harmonic than the next); an upscale on
			// a grid position stands above both grid neighbours instead.
			// Measured on the real clips: coded genuine 4K at 4/8 at most 0.14
			// with 7/8 at 0.77; a coded 2x upscale 0.66-0.88 at 4/8, at least
			// 0.13 above 3/8 and 5/8.
			const double neighbours = (std::max)(level((j - 1) * n / 8), level((j + 1) * n / 8));
			return curve[m] >= neighbours + t.gridPeakMargin;
		};
		int best = -1;
		double bestValue = 0.0;
		for (int m = from; m <= to; ++m)
			if (allowed(m) && curve[m] > bestValue) { bestValue = curve[m]; best = m; }
		if (best < 0) return peak;
		// A resample at rate r is periodic in r, so it is coherent at 2r and 3r
		// too: take the fundamental (the lowest rate nearly as strong).
		for (int d = 3; d >= 2; --d)
		{
			const int centre = static_cast<int>(std::lround(static_cast<double>(best) / d));
			int sub = -1;
			double subValue = 0.0;
			for (int m = centre - 2; m <= centre + 2; ++m)
				if (m >= from && m <= to && allowed(m) && curve[m] > subValue) { subValue = curve[m]; sub = m; }
			if (sub >= 0 && subValue >= t.harmonicShare * bestValue && subValue >= t.coherence)
			{
				best = sub;
				bestValue = subValue;
				break;
			}
		}
		// Runner-up away from the peak (how unique it is).
		for (int m = from; m <= to; ++m) if (std::abs(m - best) > 3) peak.runnerUp = (std::max)(peak.runnerUp, curve[m]);
		// Planner G1 (2026-10-16): periodic content (grilles, bricks, text) locks its
		// harmonics at many unrelated rates - a comb of strong isolated peaks. Count the
		// strong local maxima that are not harmonics of the chosen rate (k x best) and not
		// coding-grid positions (j/8, odd j/16, odd j/32).
		{
			const double strong = (std::max)(t.coherence, 0.8 * bestValue);
			auto onGrid = [&](int m) {
				for (int P : { 8, 16, 32 })
				{
					const int step = n / P;
					if (step < 2) continue;
					const int j = static_cast<int>(std::lround(static_cast<double>(m) / step));
					if (std::abs(m - j * step) <= (P == 8 ? 2 : 1)) return true;
				}
				return false;
			};
			for (int m = from + 1; m < to; ++m)
			{
				if (curve[m] < strong || curve[m] < curve[m - 1] || curve[m] < curve[m + 1] || std::abs(m - best) <= 3) continue;
				bool harmonic = false;
				for (int k = 2; k <= 6 && !harmonic; ++k) harmonic = std::abs(m - k * best) <= 3;
				if (!harmonic && !onGrid(m)) ++peak.foreignPeaks;
			}
			for (int m = from; m <= to; ++m)
			{
				bool harmonic = false;
				for (int k = 1; k <= 6 && !harmonic; ++k) harmonic = std::abs(m - k * best) <= 3;
				if (!harmonic) peak.outranked = (std::max)(peak.outranked, curve[m] - bestValue);
			}
		}
		// Sub-bin position (parabola through the peak and its neighbours).
		double offset = 0.0;
		if (best > 0 && best < n - 1)
		{
			const double a = curve[best - 1], b = curve[best], c = curve[best + 1], d = a - 2.0 * b + c;
			if (d < 0.0) offset = (std::max)(-0.5, (std::min)(0.5, 0.5 * (a - c) / d));
		}
		peak.coherence = bestValue;
		peak.ratio = (best + offset) / n;
		{
			std::vector<double> body(curve.begin() + from, curve.begin() + to + 1);
			std::nth_element(body.begin(), body.begin() + body.size() / 2, body.end());
			peak.floor = body.empty() ? 0.0 : body[body.size() / 2];
		}
		return peak;
	}

	void ReadCoding(const std::vector<Spectrum>& tiles, FrameReading& frame)
	{
		// Effective bit depth: the share of codes in use. Content made with
		// fewer levels leaves gaps however it was mapped since (8-bit x 4, or
		// through a curve such as SDR re-encoded to PQ). Windows of 16 codes
		// with enough pixels to fill every code (16 per code) are judged; the
		// median occupied share s gives bits = log2(levels) + log2(s).
		std::vector<unsigned long long> histogram;
		double blockSum = 0.0, blockWeight = 0.0;
		long long blocks = 0, bandEdges = 0;
		for (const Spectrum& s : tiles)
		{
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
				std::nth_element(shares.begin(), shares.begin() + shares.size() / 2, shares.end());
				const double share = (std::max)(1.0 / 16.0, shares[shares.size() / 2]);
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
	}

	FrameReading ReadFrame(const std::vector<Spectrum>& tiles, const Thresholds& t)
	{
		FrameReading frame;
		frame.tiles = static_cast<int>(tiles.size());
		if (tiles.empty()) return frame;
		const int n = tiles.front().n;
		std::vector<double> vertical(static_cast<size_t>((std::max)(n, 0)), 0.0), horizontal(vertical.size(), 0.0);
		double sum = 0.0, signature = 0.0;
		for (size_t i = 0; i < tiles.size(); ++i)
		{
			const Spectrum& s = tiles[i];
			sum += s.mean;
			signature += s.mean * static_cast<double>(i + 1);
			frame.peakiness = (std::max)(frame.peakiness, Peakiness(s));
			if (t.textBand > 0.0 && t.textBand <= 1.0 && s.textBand >= t.textBand) { ++frame.textTiles; continue; }
			if (s.n != n || !Informative(s, t) || s.mirrorVertical.size() != vertical.size() || s.mirrorHorizontal.size() != vertical.size()) continue;
			++frame.informativeTiles;
			for (size_t m = 0; m < vertical.size(); ++m) { vertical[m] += s.mirrorVertical[m]; horizontal[m] += s.mirrorHorizontal[m]; }
			if (frame.verticalPower.empty()) { frame.verticalPower.assign(s.vertical.size(), 0.0); frame.horizontalPower.assign(s.horizontal.size(), 0.0); }
			for (size_t k = 0; k < frame.verticalPower.size() && k < s.vertical.size() && k < s.horizontal.size(); ++k)
			{
				frame.verticalPower[k] += s.vertical[k];
				frame.horizontalPower[k] += s.horizontal[k];
			}
		}
		frame.mean = sum / tiles.size();
		frame.signature = signature;
		ReadCoding(tiles, frame);
		if (frame.informativeTiles == 0) return frame;
		for (size_t m = 0; m < vertical.size(); ++m) { vertical[m] /= frame.informativeTiles; horizontal[m] /= frame.informativeTiles; }
		for (double& p : frame.verticalPower) p /= frame.informativeTiles;
		for (double& p : frame.horizontalPower) p /= frame.informativeTiles;
		frame.verticalCurve = vertical;
		frame.horizontalCurve = horizontal;
		frame.vertical = FindPeak(vertical, n, t);
		frame.horizontal = FindPeak(horizontal, n, t);
		// Variable bitrate (owner 2026-10-16): under hard coding a coding-grid position
		// can edge out the true rate on one axis (found: 29/32 at 0.512 against 1080's
		// 0.509 on 32-px blocks, while the other axis read 1080 cleanly). When the axes
		// disagree and one axis's curve holds the other axis's rate nearly as strongly
		// as its own peak (and above the threshold), the axes agree on that rate. Real
		// detail is never this coherent (0.1-0.2), so native content is unaffected;
		// a true anisotropic upscale has no such second peak and is left alone.
		auto agree = [&](MirrorPeak& self, const std::vector<double>& curve, const MirrorPeak& other) {
			if (other.coherence < t.coherence || std::fabs(self.ratio - other.ratio) <= 0.02) return;
			const int centre = static_cast<int>(std::lround(other.ratio * n));
			double there = 0.0;
			for (int m = centre - 2; m <= centre + 2; ++m) if (m >= 0 && m < n) there = (std::max)(there, curve[m]);
			if (there >= t.coherence && there >= self.coherence - t.axisAgreement)
			{
				self.ratio = other.ratio;
				self.coherence = there;
			}
		};
		if (frame.vertical.coherence >= frame.horizontal.coherence) agree(frame.horizontal, horizontal, frame.vertical);
		else agree(frame.vertical, vertical, frame.horizontal);
		// Both axes resampled (a uniform upscale), else native: real detail on
		// either axis must survive. Anamorphic sources take the larger ratio so
		// nothing either axis holds is lost.
		// Planner G1: a high curve floor is periodic content, not a resample (either axis).
		const bool quietV = frame.vertical.floor <= t.maximumFloor && frame.vertical.foreignPeaks < 2 && frame.vertical.outranked <= 0.1;
		const bool quietH = frame.horizontal.floor <= t.maximumFloor && frame.horizontal.foreignPeaks < 2 && frame.horizontal.outranked <= 0.1;
		if (frame.vertical.coherence >= t.coherence && frame.horizontal.coherence >= t.coherence && quietV && quietH)
			frame.ratio = (std::max)(frame.vertical.ratio, frame.horizontal.ratio);
		// Per axis, at the stricter single-axis coherence.
		frame.ratioV = frame.vertical.coherence >= t.singleAxisCoherence && quietV && frame.vertical.ratio <= t.singleAxisMaximumRatio ? frame.vertical.ratio : 1.0;
		frame.ratioH = frame.horizontal.coherence >= t.singleAxisCoherence && quietH && frame.horizontal.ratio <= t.singleAxisMaximumRatio ? frame.horizontal.ratio : 1.0;
		return frame;
	}

	double DetailAbove(const FrameReading& frame, double ratio)
	{
		const double unknown = std::numeric_limits<double>::infinity();
		const size_t bins = frame.verticalPower.size();
		if (ratio <= 0.0 || ratio >= 1.0 || bins < 33 || frame.horizontalPower.size() != bins) return unknown;
		const int half = static_cast<int>(bins) - 1;   // n / 2
		const int n = 2 * half;
		const double cut = ratio * half;               // r's Nyquist, in bins
		// Below: the top of the source's own band; above: past the kernel's transition,
		// short of the signal's own Nyquist. Block-coding harmonics are left out.
		const int b0 = static_cast<int>(std::ceil(0.55 * cut)), b1 = static_cast<int>(std::floor(0.9 * cut));
		const int a0 = static_cast<int>(std::ceil(1.15 * cut)), a1 = (std::min)(static_cast<int>(std::floor(0.97 * half)), a0 + (std::max)(4, b1 - b0));
		if (a1 - a0 < 3 || b1 - b0 < 3) return unknown;
		double best = -unknown;
		for (const std::vector<double>* power : { &frame.verticalPower, &frame.horizontalPower })
		{
			auto band = [&](int from, int to) {
				double sum = 0.0;
				int count = 0;
				for (int k = from; k <= to; ++k)
					if (!BlockHarmonic(k, n)) { sum += std::log10((std::max)((*power)[k], 1e-20)); ++count; }
				return count > 0 ? sum / count : 0.0;
			};
			best = (std::max)(best, band(a0, a1) - band(b0, b1));
		}
		return best;
	}

	FrameReading AxisFrame(const FrameReading& frame, bool vertical)
	{
		FrameReading axis = frame;
		axis.ratio = vertical ? frame.ratioV : frame.ratioH;
		const std::vector<double>& power = vertical ? frame.verticalPower : frame.horizontalPower;
		axis.verticalPower = power;
		axis.horizontalPower = power;
		return axis;
	}

	void Tracker::Reset()
	{
		Discontinuity("reset");
		cuts = 0;
		discontinuities = 0;
		lastReason.clear();
	}

	void Tracker::Discontinuity(const char* reason)
	{
		// Seek / skip, source or format change, signal loss, resync, a time
		// jump: the evidence belonged to what came before. Back to native and
		// start again (owner: robust disengagement).
		window.clear();
		declared = 1.0;
		haveLastNative = false;
		streak = 0;
		streakDetail.clear();
		renditionMode = false;
		lastMean = -1.0;
		lastSignature = 0.0;
		lastSeconds = -1.0;
		evidence = 0.0;
		haveLast = false;
		++discontinuities;
		lastReason = reason ? reason : "";
	}

	void Tracker::Update(const FrameReading& frame, double seconds, bool repeated)
	{
		// Time running backwards or a gap with no frames: a discontinuity the
		// caller may not have reported (a skip, a pause on the source side).
		if (lastSeconds >= 0.0 && (seconds < lastSeconds || seconds - lastSeconds > settings.maximumGapSeconds))
			Discontinuity(seconds < lastSeconds ? "time went backwards" : "gap");
		// A repeated frame (pause, a repeated cadence frame, a frozen source)
		// is no new evidence and no time.
		if (repeated || (haveLast && frame.tiles > 0 && std::fabs(frame.signature - lastSignature) < 1e-9 && std::fabs(frame.mean - lastMean) < 1e-9))
		{
			lastSeconds = seconds;
			return;
		}
		const double dt = lastSeconds >= 0.0 ? seconds - lastSeconds : 0.0;
		lastSeconds = seconds;
		if (frame.tiles > 0)
		{
			if (haveLast && std::fabs(frame.mean - lastMean) > settings.cutLevel) { ++shot; ++cuts; haveLastNative = false; streak = 0; streakDetail.clear(); }
			lastMean = frame.mean;
			lastSignature = frame.signature;
			haveLast = true;
		}
		while (!window.empty() && seconds - window.front().seconds > settings.windowSeconds)
		{
			evidence -= window.front().dt;
			window.pop_front();
		}
		if (window.empty()) evidence = 0.0;
		if (!frame.informative()) return;
		const double ratio = frame.ratio >= settings.nativeAbove ? 1.0 : frame.ratio;
		// Variable bitrate (owner 2026-10-16): native needs detail above the rate in
		// question - the declared one, else the lower one the window is gathering.
		if (ratio >= 1.0)
		{
			const double inQuestion = declared < 1.0 ? declared : Candidate();
			const double reference = inQuestion < 1.0 ? ReferenceDetail(inQuestion) : std::numeric_limits<double>::infinity();
			if (!std::isinf(reference) && DetailAbove(frame, inQuestion) < reference + settings.detailMarginDecades)
			{
				++starved;
				return;
			}
		}
		// Evidence time counts only between informative, changing frames.
		const double step = window.empty() ? 0.0 : (std::min)(dt, settings.maximumGapSeconds);
		// F12: the rendition-change event (see Settings).
		if (ratio >= 1.0)
		{
			lastNative = frame;
			haveLastNative = !frame.verticalPower.empty();
			lastNativeShot = shot;
			streak = 0;
			streakDetail.clear();
		}
		else
		{
			if (streak == 0 || std::fabs(ratio - streakRatio) > settings.tolerance || shot != lastNativeShot)
			{
				streak = 0;
				streakDetail.clear();
				streakRatio = ratio;
				streakStart = seconds;
			}
			++streak;
			const double d = DetailAbove(frame, ratio);
			if (!std::isinf(d)) streakDetail.push_back(d);
			if (streak >= settings.renditionFrames && haveLastNative && shot == lastNativeShot && !renditionMode &&
				declared >= 1.0 && streakDetail.size() >= static_cast<size_t>(settings.renditionFrames))
			{
				std::vector<double> sorted = streakDetail;
				std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
				const double before = DetailAbove(lastNative, streakRatio);
				if (!std::isinf(before) && before - sorted[sorted.size() / 2] >= settings.renditionDetailDrop)
				{
					// The older (native) evidence belonged to the other rendition.
					while (!window.empty() && window.front().seconds < streakStart) window.pop_front();
					evidence = 0.0;
					for (const Sample& s : window) evidence += s.dt;
					renditionMode = true;
					++renditionChanges;
				}
			}
		}
		window.push_back({ seconds, ratio, shot, step, ratio < 1.0 ? DetailAbove(frame, ratio) : std::numeric_limits<double>::infinity() });
		evidence += step;
		// Detail above the declared cutoff: up at once (bias to native).
		if (ratio > declared + settings.tolerance)
		{
			declared = ratio;
			renditionMode = false;
			return;
		}
		// F12: after a rendition change, time only (a mid-shot switch has no shots).
		if (renditionMode)
		{
			const double candidate = Candidate();
			if (candidate < declared - settings.tolerance && EvidenceSeconds() >= settings.renditionSeconds)
			{
				declared = candidate;
				renditionMode = false;
			}
			return;
		}
		// Lower only when the whole window - several shots, a sustained time -
		// stays below the declared cutoff.
		const double candidate = Candidate();
		if (candidate < declared - settings.tolerance && Shots() >= settings.minimumShots && EvidenceSeconds() >= settings.minimumSeconds)
			declared = candidate;
	}

	double Tracker::Candidate() const
	{
		double ratio = 0.0;
		for (const Sample& s : window) ratio = (std::max)(ratio, s.ratio);
		return window.empty() ? 1.0 : ratio;
	}

	double Tracker::ReferenceDetail(double ratio) const
	{
		std::vector<double> levels;
		for (const Sample& s : window)
			if (s.ratio < 1.0 && std::fabs(s.ratio - ratio) <= settings.tolerance && !std::isinf(s.detail)) levels.push_back(s.detail);
		if (levels.empty()) return std::numeric_limits<double>::infinity();
		std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
		return levels[levels.size() / 2];
	}

	int Tracker::Shots() const
	{
		return window.empty() ? 0 : static_cast<int>(window.back().shot - window.front().shot) + 1;
	}

	double Tracker::EvidenceSeconds() const
	{
		return evidence;
	}

	int Tracker::Height(int nativeHeight) const
	{
		if (declared >= 1.0 || nativeHeight <= 0) return nativeHeight;
		return 2 * static_cast<int>(std::lround(declared * nativeHeight * 0.5));
	}

	std::string Describe(double ratio, int nativeHeight)
	{
		if (ratio >= 1.0 || nativeHeight <= 0) return "native";
		char text[64];
		snprintf(text, sizeof(text), "~%d (%.2f)", 2 * static_cast<int>(std::lround(ratio * nativeHeight * 0.5)), ratio);
		return text;
	}
}
