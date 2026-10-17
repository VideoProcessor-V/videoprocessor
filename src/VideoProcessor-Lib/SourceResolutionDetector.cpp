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
		// Planner Q6 (2026-10-16): either axis - a band of rows (subtitles, lower thirds, anywhere
		// in the tile) or of columns (vertical CJK text, side captions); for the columns the
		// steps down are counted.
		// `lines` (optional): the separate bands of sharp steps along the axis that gave the share
		// (text lines; mixed-screen guard: subtitles are one or two, UI panels three or more).
		double TextBand(const float* luma, int stride, int n, int* lines = nullptr, double* sharpAcross = nullptr, double* sharpDown = nullptr)
		{
			std::vector<int> rows(static_cast<size_t>(n), 0), columns(static_cast<size_t>(n), 0);
			long acrossTotal = 0, downTotal = 0;
			for (int y = 0; y < n; ++y)
			{
				const float* row = luma + static_cast<size_t>(y) * stride;
				const float* above = y > 0 ? row - stride : nullptr;
				int count = 0;
				for (int x = 0; x < n; ++x)
				{
					if (x > 0 && std::fabs(row[x] - row[x - 1]) >= 1.0f / 3.0f) ++count;
					if (above && std::fabs(row[x] - above[x]) >= 1.0f / 3.0f) { ++columns[static_cast<size_t>(x)]; ++downTotal; }
				}
				rows[static_cast<size_t>(y)] = count;
				acrossTotal += count;
			}
			const int band = n / 4;
			auto densest = [band, n](const std::vector<int>& lines, long total) {
				if (total < static_cast<long>(n) * n / 500) return 0.0;   // under 0.2 %: nothing to judge
				long sum = 0, best = 0;
				for (int i = 0; i < n; ++i)
				{
					sum += lines[static_cast<size_t>(i)];
					if (i >= band) sum -= lines[static_cast<size_t>(i - band)];
					best = (std::max)(best, sum);
				}
				return static_cast<double>(best) / total;
			};
			// Text lines: runs of lines (rows or columns) holding at least two sharp steps, gaps of up
			// to 3 bridged, at least 6 long and holding a fiftieth of the steps (stray steps in the
			// picture under the text are not a line).
			auto runs = [n](const std::vector<int>& counts, long total) {
				if (total < static_cast<long>(n) * n / 500) return 0;
				int found = 0, start = -1, last = -1;
				long sum = 0;
				auto close = [&]() {
					if (start >= 0 && last - start + 1 >= 6 && sum * 50 >= total) ++found;
					start = -1;
					sum = 0;
				};
				for (int i = 0; i < n; ++i)
				{
					if (counts[static_cast<size_t>(i)] < 2) continue;
					if (start >= 0 && i - last > 4) close();
					if (start < 0) start = i;
					last = i;
					sum += counts[static_cast<size_t>(i)];
				}
				close();
				return found;
			};
			if (sharpAcross) *sharpAcross = static_cast<double>(acrossTotal) / (static_cast<double>(n) * (n - 1));
			if (sharpDown) *sharpDown = static_cast<double>(downTotal) / (static_cast<double>(n) * (n - 1));
			const double acrossShare = densest(rows, acrossTotal), downShare = densest(columns, downTotal);
			if (lines) *lines = acrossShare >= downShare ? runs(rows, acrossTotal) : runs(columns, downTotal);
			return (std::max)(acrossShare, downShare);
		}
	}

	bool Analyse(const float* luma, int stride, int n, Spectrum& out, int codeMaximum)
	{
		if (!luma || !PowerOfTwo(n) || stride < n) return false;
		out.n = n;
		out.textBand = TextBand(luma, stride, n, &out.textLines, &out.sharpStepsH, &out.sharpStepsV);
		out.sharpSteps = 0.5 * (out.sharpStepsH + out.sharpStepsV);
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
		// Planner Q5: exact periodicity from each axis's averaged power spectrum (Wiener-
		// Khinchin), divided by the window's own autocorrelation. A repeat counts only as a
		// local maximum standing at least 0.2 above the lowest value at a shorter lag: smooth
		// content decays without a return, periodic content dips and comes back.
		{
			const int maxLag = n / 4;
			std::vector<double> windowAuto(static_cast<size_t>(maxLag) + 2, 0.0);
			for (int lag = 0; lag <= maxLag + 1; ++lag)
			{
				double s = 0.0;
				for (int i = 0; i + lag < n; ++i) s += window[i] * window[i + lag];
				windowAuto[static_cast<size_t>(lag)] = s / windowEnergy;
			}
			auto repeat = [&](const std::vector<double>& power) {
				std::vector<double> r(static_cast<size_t>(maxLag) + 2, 0.0);
				for (int lag = 0; lag <= maxLag + 1; ++lag)
				{
					double s = 0.0;
					for (int k = 1; k <= n / 2; ++k) s += power[static_cast<size_t>(k)] * std::cos(2.0 * kPi * k * lag / n);
					r[static_cast<size_t>(lag)] = s;
				}
				if (r[0] <= 0.0) return 0.0;
				std::vector<double> v(r.size(), 0.0);
				for (size_t lag = 0; lag < r.size(); ++lag)
					v[lag] = windowAuto[lag] > 0.05 ? r[lag] / r[0] / windowAuto[lag] : 0.0;
				double lowest = v[1], peak = 0.0;
				for (int lag = 2; lag <= maxLag; ++lag)
				{
					const double here = v[static_cast<size_t>(lag)];
					if (lag >= 3 && here >= v[static_cast<size_t>(lag - 1)] && here >= v[static_cast<size_t>(lag + 1)] && here - lowest >= 0.2)
						peak = (std::max)(peak, here);
					lowest = (std::min)(lowest, here);
				}
				return peak;
			};
			out.periodicPeak = (std::max)(repeat(out.vertical), repeat(out.horizontal));
		}
		out.mean = total / n;
		// Latch review 2026-10-17: the spread and the quadrant means (cut shape, flat bars).
		{
			double q[4] = {}, sq = 0.0;
			const int h = n / 2;
			for (int y = 0; y < n; ++y)
			{
				const float* row = luma + static_cast<size_t>(y) * stride;
				for (int x = 0; x < n; ++x)
				{
					const double v = row[x];
					q[(y >= h ? 2 : 0) + (x >= h ? 1 : 0)] += v;
					sq += (v - out.mean) * (v - out.mean);
				}
			}
			for (int i = 0; i < 4; ++i) out.quadrants[i] = q[i] / (static_cast<double>(h) * h);
			out.spread = std::sqrt(sq / (static_cast<double>(n) * n));
			// Bars: constant (two codes) dark (a tenth of the range) full columns / rows from each edge.
			const double flat = 2.0 / (std::max)(1, codeMaximum), dark = 0.1;
			auto column = [&](int x) {
				double lo = 1e9, hi = -1e9;
				for (int y = 0; y < n; ++y) { const double v = luma[static_cast<size_t>(y) * stride + x]; lo = (std::min)(lo, v); hi = (std::max)(hi, v); }
				return hi - lo <= flat && hi <= dark;
			};
			auto row = [&](int y) {
				double lo = 1e9, hi = -1e9;
				const float* r = luma + static_cast<size_t>(y) * stride;
				for (int x = 0; x < n; ++x) { lo = (std::min)(lo, static_cast<double>(r[x])); hi = (std::max)(hi, static_cast<double>(r[x])); }
				return hi - lo <= flat && hi <= dark;
			};
			out.barLeft = out.barRight = out.barTop = out.barBottom = 0;
			while (out.barLeft < n && column(out.barLeft)) ++out.barLeft;
			while (out.barRight < n - out.barLeft && column(n - 1 - out.barRight)) ++out.barRight;
			while (out.barTop < n && row(out.barTop)) ++out.barTop;
			while (out.barBottom < n - out.barTop && row(n - 1 - out.barBottom)) ++out.barBottom;
		}
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

	namespace
	{
		// One tile's mirror coherence at `ratio` (the strongest within two candidates).
		double CoherenceAt(const std::vector<double>& curve, double ratio, int n)
		{
			if (curve.size() != static_cast<size_t>(n) || ratio <= 0.0 || ratio >= 1.0) return 0.0;
			const int centre = static_cast<int>(std::lround(ratio * n));
			double v = 0.0;
			for (int m = centre - 2; m <= centre + 2; ++m) if (m >= 0 && m < n) v = (std::max)(v, curve[static_cast<size_t>(m)]);
			return v;
		}

		// One tile's curve floor (its median over the candidates, as MirrorPeak::floor): low
		// beside a resample, high for sharp edges and text (coherent at almost every rate).
		double CurveFloor(const std::vector<double>& curve, int n, const Thresholds& t)
		{
			const int from = (std::max)(n / 8, static_cast<int>(std::ceil(t.minimumRatio * n)));
			const int to = (std::min)(n - 1, static_cast<int>(std::floor(t.maximumRatio * n)));
			if (curve.size() != static_cast<size_t>(n) || to < from) return 0.0;
			std::vector<double> body(curve.begin() + from, curve.begin() + to + 1);
			std::nth_element(body.begin(), body.begin() + body.size() / 2, body.end());
			return body[body.size() / 2];
		}

		// Mixed screens (Thresholds::mixedGuard; owner 2026-10-16): the frame read lower on the
		// averaged curves; look for tiles holding native-resolution content at that rate - a UI
		// around or over the video. See Thresholds for the rules.
		void ReadMixedScreen(const std::vector<Spectrum>& tiles, const Thresholds& t, FrameReading& frame, const std::vector<char>& bars)
		{
			const bool both = frame.ratio < 1.0;
			const bool useV = both || frame.ratioV < 1.0, useH = both || frame.ratioH < 1.0;
			if (!useV && !useH) return;
			// Detail is measured at the reading's rate (the both-axes reading uses one rate for both
			// axes, as DetailAbove); resampling per axis at that axis's own rate.
			const int axes = (useV ? 1 : 0) | (useH ? 2 : 0);
			const double rate = both ? frame.ratio : (useV ? frame.ratioV : frame.ratioH);
			const double rateV = frame.vertical.ratio, rateH = frame.horizontal.ratio;
			const int n = tiles.front().n;
			const double step = 1.0 / (std::max)(1, t.codeMaximum);
			const double floorLevel = std::log10(step * step / 12.0) + t.mixedDetailFloorDecades;
			const bool textGuard = t.textBand > 0.0 && t.textBand <= 1.0;
			struct View { bool valid = false, coherent = false, incoherent = false, informative = false; double detail = 0.0, level = 0.0; };
			std::vector<View> views(tiles.size());
			std::vector<double> reference;
			// One-pixel full-contrast steps (Spectrum::sharpSteps): an upscale spreads every edge over
			// 1 / r pixels (measured: 0.0000 at r 1/3, 0.0001 at 1/2, 0.0015 at 2/3, 0.004 at 3/4),
			// native picture holds 0.014 - 0.018 and UI text 0.03 - 0.05. Such a tile is native, never
			// the resampled picture; not text-like (not a subtitle band) it is UI - also when it is
			// what made the frame read lower (evenly spaced UI text lines are mirror-coherent and can
			// read a false rate, ~0.3 measured, with no resampled tile at that rate at all).
			const bool sharpRule = t.mixedRowGuard && t.mixedSharpSteps > 0.0;
			// On the axes read: a one-axis upscale is native - and sharp - across the other axis.
			auto sharpness = [&](const Spectrum& s) {
				return useV && useH ? s.sharpSteps : useV ? s.sharpStepsV : s.sharpStepsH;
			};
			int sharpUi = 0;
			for (size_t i = 0; i < tiles.size(); ++i)
			{
				const Spectrum& s = tiles[i];
				// Latch fixes: a player-added bar's edge is not native UI.
				if (i < bars.size() && bars[i]) continue;
				if (sharpRule && s.n == n && sharpness(s) >= t.mixedSharpSteps && s.textBand < t.mixedTextBand && Informative(s, t)) ++sharpUi;
				if (s.n != n || s.vertical.size() != static_cast<size_t>(n / 2 + 1) || s.horizontal.size() != s.vertical.size() ||
					s.mirrorVertical.size() != static_cast<size_t>(n) || s.mirrorHorizontal.size() != static_cast<size_t>(n))
					continue;
				View& v = views[i];
				double level = 0.0;
				const double detail = DetailAbove(s, rate, axes, &level);
				if (std::isinf(detail)) continue;
				const double cohV = CoherenceAt(s.mirrorVertical, rateV, n), cohH = CoherenceAt(s.mirrorHorizontal, rateH, n);
				v.valid = true;
				v.detail = detail;
				v.level = level;
				// Resampled at the rate on an axis read (one tile's coherence can dip below the frame
				// threshold on one axis: 0.36 / 0.53 measured on a clean 2x upscale; native detail
				// reads 0.03 - 0.13 on both), or on none of them.
				v.coherent = (useV && cohV >= t.coherence) || (useH && cohH >= t.coherence);
				v.incoherent = !v.coherent;
				v.informative = Informative(s, t);
				const bool excluded = (textGuard && s.textBand >= t.textBand) || (t.periodicPeak > 0.0 && s.periodicPeak >= t.periodicPeak);
				const double curveFloor = (std::max)(useV ? CurveFloor(s.mirrorVertical, n, t) : 0.0, useH ? CurveFloor(s.mirrorHorizontal, n, t) : 0.0);
				// The resampled picture: the tiles that carry the reading themselves - not text or
				// sharp edges, which are coherent at every rate (a high floor) and hold native detail.
				const bool sharp = t.mixedSharpSteps > 0.0 && sharpness(s) >= t.mixedSharpSteps;
				if (v.coherent && v.informative && !excluded && !sharp && s.textBand < t.mixedTextBand && curveFloor <= t.maximumFloor) reference.push_back(detail);
			}
			frame.mixedSharpTiles = sharpUi;
			if (sharpUi > 0)
			{
				frame.mixed = true;
				frame.mixedRatio = both ? frame.ratio : rate;
				frame.ratio = frame.ratioV = frame.ratioH = 1.0;
				return;
			}
			// No tile is resampled on its own (the reading came from the average only): nothing to
			// compare with - left as before.
			if (reference.empty()) return;
			// The lower quartile, not the median: tiles of native text over a dimmed video (a panel,
			// a menu) are partly coherent and can make up half of the candidates; the resampled
			// tiles themselves agree within ~0.15 decades, so the quartile moves them little.
			const size_t quartile = reference.size() / 4;
			std::nth_element(reference.begin(), reference.begin() + quartile, reference.end());
			const double resampled = reference[quartile];
			const int count = static_cast<int>(tiles.size());
			// Each tile's centre (normalised): the explicit centres, else the gridColumns layout.
			std::vector<float> cx(tiles.size()), cy(tiles.size());
			if (t.tileX.size() == tiles.size() && t.tileY.size() == tiles.size()) { cx = t.tileX; cy = t.tileY; }
			else
			{
				const int columns = t.gridColumns > 0 ? (std::min)(t.gridColumns, count) : count;
				const int gridRows = (count + columns - 1) / columns;
				for (int i = 0; i < count; ++i)
				{
					cx[static_cast<size_t>(i)] = static_cast<float>((i % columns + 0.5) / columns);
					cy[static_cast<size_t>(i)] = static_cast<float>((i / columns + 0.5) / gridRows);
				}
			}
			std::vector<float> overlayX, overlayY;
			int picture = 0, overlay = 0, ui = 0;
			for (size_t i = 0; i < tiles.size(); ++i)
			{
				const View& v = views[i];
				if (!v.valid || v.detail < resampled + t.mixedDetailDecades || v.level < floorLevel) continue;
				const bool textLike = tiles[i].textBand >= t.mixedTextBand;
				if (!textLike && v.incoherent && v.informative) { ++picture; continue; }
				++overlay;
				// Native detail over (or beside) the resampled picture that is not a band of one or
				// two text lines: UI, wherever it sits (a single row included).
				// Three or more text lines in one tile are a UI panel or menu, not subtitles (one or two
				// lines), whatever the band share reads (a panel's lines spaced near a quarter tile
				// read 0.36 - 0.5, at the text-like threshold).
				if (!textLike || (t.mixedTextLines > 0 && tiles[i].textLines >= t.mixedTextLines)) ++ui;
				overlayX.push_back(cx[i]);
				overlayY.push_back(cy[i]);
			}
			// Rows / columns: distinct centres (a 1e-3 of the picture apart).
			auto distinct = [](std::vector<float> v) {
				std::sort(v.begin(), v.end());
				int k = 0;
				for (size_t i = 0; i < v.size(); ++i) if (i == 0 || v[i] - v[i - 1] > 1e-3f) ++k;
				return k;
			};
			int rowMost = 0;
			for (size_t i = 0; i < overlayY.size(); ++i)
			{
				int same = 0;
				for (const float y : overlayY) same += std::fabs(y - overlayY[i]) <= 1e-3f ? 1 : 0;
				rowMost = (std::max)(rowMost, same);
			}
			frame.mixedUiTiles = ui;
			frame.mixedRowMost = rowMost;
			frame.mixedPictureTiles = picture;
			frame.mixedOverlayTiles = overlay;
			frame.mixedOverlayRows = distinct(overlayY);
			frame.mixedOverlayColumns = distinct(overlayX);
			// The spread: overlay far enough apart both ways (the 4 x 2 grid: two rows and two
			// columns, exactly as before; a subtitle band never spans 0.3 of the height).
			const bool spread = !overlayY.empty() &&
				*std::max_element(overlayY.begin(), overlayY.end()) - *std::min_element(overlayY.begin(), overlayY.end()) >= t.mixedSpreadHeight - 1e-6 &&
				*std::max_element(overlayX.begin(), overlayX.end()) - *std::min_element(overlayX.begin(), overlayX.end()) >= t.mixedSpreadWidth - 1e-6;
			const bool row = (t.mixedRowGuard && ui > 0) || (t.mixedRowTiles > 0 && frame.mixedRowMost >= t.mixedRowTiles);
			if ((t.mixedPictureTiles > 0 && picture >= t.mixedPictureTiles) || spread || row)
			{
				frame.mixed = true;
				frame.mixedRatio = both ? frame.ratio : rate;
				frame.ratio = frame.ratioV = frame.ratioH = 1.0;
			}
		}
	}

	FrameReading ReadFrame(const std::vector<Spectrum>& tiles, const Thresholds& t)
	{
		FrameReading frame;
		frame.tiles = static_cast<int>(tiles.size());
		if (tiles.empty()) return frame;
		const int n = tiles.front().n;
		std::vector<double> vertical(static_cast<size_t>((std::max)(n, 0)), 0.0), horizontal(vertical.size(), 0.0);
		double sum = 0.0, signature = 0.0;
		// The mean and signature from the base tiles only (Thresholds::signatureTiles).
		const size_t signatureTiles = t.signatureTiles > 0 && static_cast<size_t>(t.signatureTiles) < tiles.size()
			? static_cast<size_t>(t.signatureTiles) : tiles.size();
		// Latch fixes: flat bar tiles (near-constant at or near black) carry no shape and no level.
		const double codeStep = 1.0 / (std::max)(1, t.codeMaximum);
		frame.blackLevel = t.blackLevel;
		frame.noiseLevel = t.flatSpreadCodes * codeStep;
		auto flatBar = [&](const Spectrum& s) {
			return s.spread <= t.flatSpreadCodes * codeStep && s.mean <= t.blackLevel + t.nearBlackShare * (1.0 - t.blackLevel);
		};
		// Latch fixes: bar tiles (see Thresholds::latchFixes) - by the tile centres, else the grid.
		std::vector<float> centreX(tiles.size(), 0.5f), centreY(tiles.size(), 0.5f);
		if (t.tileX.size() == tiles.size() && t.tileY.size() == tiles.size()) { centreX = t.tileX; centreY = t.tileY; }
		else
		{
			const int count = static_cast<int>(tiles.size());
			const int columns = t.gridColumns > 0 ? (std::min)(t.gridColumns, count) : count;
			const int gridRows = (count + columns - 1) / columns;
			for (int i = 0; i < count; ++i)
			{
				centreX[static_cast<size_t>(i)] = static_cast<float>((i % columns + 0.5) / columns);
				centreY[static_cast<size_t>(i)] = static_cast<float>((i / columns + 0.5) / gridRows);
			}
		}
		auto barTile = [&](size_t i) {
			if (!t.latchFixes) return false;
			const Spectrum& s = tiles[i];
			const int minimum = (std::max)(1, static_cast<int>(std::ceil(t.barMinimumShare * s.n)));
			if (s.barLeft >= s.n || s.barTop >= s.n) return false;   // all flat: no picture to protect
			return (centreX[i] < 0.3f && s.barLeft >= minimum) || (centreX[i] > 0.7f && s.barRight >= minimum) ||
				(centreY[i] < 0.3f && s.barTop >= minimum) || (centreY[i] > 0.7f && s.barBottom >= minimum);
		};
		std::vector<char> bars(tiles.size(), 0);
		for (size_t i = 0; i < tiles.size(); ++i) bars[i] = barTile(i) ? 1 : 0;
		size_t counted = 0;
		bool allFlat = true;
		for (size_t i = 0; i < tiles.size(); ++i) allFlat = allFlat && flatBar(tiles[i]);
		for (size_t i = 0; i < tiles.size(); ++i)
		{
			const Spectrum& s = tiles[i];
			if (i < signatureTiles)
			{
				const bool flat = t.latchFixes && flatBar(s);
				if (t.latchFixes)
				{
					for (int q = 0; q < 4; ++q) { frame.pattern.push_back(s.quadrants[q]); frame.patternFlat.push_back(flat ? 1 : 0); }
				}
				// A flat bar tile stays out of the mean / signature unless every tile is flat (black).
				if (!flat || allFlat)
				{
					sum += s.mean;
					signature += s.mean * static_cast<double>(i + 1);
					++counted;
				}
			}
			frame.peakiness = (std::max)(frame.peakiness, Peakiness(s));
			if (t.textBand > 0.0 && t.textBand <= 1.0 && s.textBand >= t.textBand) { ++frame.textTiles; continue; }
			if (t.periodicPeak > 0.0 && s.periodicPeak >= t.periodicPeak) { ++frame.periodicTiles; continue; }
			if (s.n != n || !Informative(s, t) || s.mirrorVertical.size() != vertical.size() || s.mirrorHorizontal.size() != vertical.size()) continue;
			// Latch fixes: a tile holding a player-added bar (its sharp edge is coherent at every rate):
			// left out (evidence only lost).
			if (barTile(i)) { ++frame.barTiles; continue; }
			++frame.informativeTiles;
			frame.contrast += s.spread;
			for (size_t m = 0; m < vertical.size(); ++m) { vertical[m] += s.mirrorVertical[m]; horizontal[m] += s.mirrorHorizontal[m]; }
			if (frame.verticalPower.empty()) { frame.verticalPower.assign(s.vertical.size(), 0.0); frame.horizontalPower.assign(s.horizontal.size(), 0.0); }
			for (size_t k = 0; k < frame.verticalPower.size() && k < s.vertical.size() && k < s.horizontal.size(); ++k)
			{
				frame.verticalPower[k] += s.vertical[k];
				frame.horizontalPower[k] += s.horizontal[k];
			}
		}
		frame.mean = counted > 0 ? sum / counted : sum / signatureTiles;
		frame.signature = signature;
		ReadCoding(tiles, frame);
		frame.black = t.latchFixes && allFlat;
		{
			// The rounding floor at the effective bit depth (8-bit content in a 10-bit signal rounds
			// in steps of 4 codes), the signal's own when unknown.
			const double signalBits = std::log2(static_cast<double>((std::max)(1, t.codeMaximum)) + 1.0);
			double step = codeStep;
			if (frame.effectiveBits > 0.0 && frame.effectiveBits < signalBits) step *= std::pow(2.0, signalBits - frame.effectiveBits);
			frame.quantisationFloor = std::log10(step * step / 12.0);
		}
		if (frame.informativeTiles == 0) { frame.contrast = 0.0; return frame; }
		frame.contrast = t.latchFixes ? frame.contrast / frame.informativeTiles / (std::max)(1e-6, 1.0 - t.blackLevel) : 0.0;
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
		// Mixed screens: a lower reading with native UI in the frame reads native (off: as before).
		if (t.mixedGuard && (frame.ratio < 1.0 || frame.ratioV < 1.0 || frame.ratioH < 1.0)) ReadMixedScreen(tiles, t, frame, bars);
		return frame;
	}

	namespace
	{
		// DetailAbove on a pair of power spectra (bins 0..n/2). `axes`: 1 vertical, 2 horizontal,
		// 3 both (the larger). `level`: the above band's mean log10 power on the axis chosen.
		double DetailAboveSpectra(const std::vector<double>& vertical, const std::vector<double>& horizontal, double ratio, int axes, double* level)
		{
			const double unknown = std::numeric_limits<double>::infinity();
			const size_t bins = vertical.size();
			if (ratio <= 0.0 || ratio >= 1.0 || bins < 33 || horizontal.size() != bins || (axes & 3) == 0) return unknown;
			const int half = static_cast<int>(bins) - 1;   // n / 2
			const int n = 2 * half;
			const double cut = ratio * half;               // r's Nyquist, in bins
			// Below: the top of the source's own band; above: past the kernel's transition,
			// short of the signal's own Nyquist. Block-coding harmonics are left out.
			const int b0 = static_cast<int>(std::ceil(0.55 * cut)), b1 = static_cast<int>(std::floor(0.9 * cut));
			const int a0 = static_cast<int>(std::ceil(1.15 * cut)), a1 = (std::min)(static_cast<int>(std::floor(0.97 * half)), a0 + (std::max)(4, b1 - b0));
			if (a1 - a0 < 3 || b1 - b0 < 3) return unknown;
			double best = -unknown;
			for (int axis = 0; axis < 2; ++axis)
			{
				if (!(axes & (1 << axis))) continue;
				const std::vector<double>& power = axis == 0 ? vertical : horizontal;
				auto band = [&](int from, int to) {
					double sum = 0.0;
					int count = 0;
					for (int k = from; k <= to; ++k)
						if (!BlockHarmonic(k, n)) { sum += std::log10((std::max)(power[k], 1e-20)); ++count; }
					return count > 0 ? sum / count : 0.0;
				};
				const double above = band(a0, a1), d = above - band(b0, b1);
				if (d > best)
				{
					best = d;
					if (level) *level = above;
				}
			}
			return best;
		}
	}

	double DetailAbove(const FrameReading& frame, double ratio)
	{
		return DetailAboveSpectra(frame.verticalPower, frame.horizontalPower, ratio, 3, nullptr);
	}

	double DetailAbove(const FrameReading& frame, double ratio, double* level)
	{
		return DetailAboveSpectra(frame.verticalPower, frame.horizontalPower, ratio, 3, level);
	}

	double CoherenceAtRate(const FrameReading& frame, double ratio)
	{
		const int n = static_cast<int>(frame.verticalCurve.size());
		if (n == 0 || frame.horizontalCurve.size() != frame.verticalCurve.size()) return 0.0;
		return (std::min)(CoherenceAt(frame.verticalCurve, ratio, n), CoherenceAt(frame.horizontalCurve, ratio, n));
	}

	bool PatternChange(const std::vector<double>& x, const std::vector<char>& flatX, const std::vector<double>& y,
		const std::vector<char>& flatY, double blackLevel, double noise, double& decorrelation, double& share, double* jump)
	{
		decorrelation = share = 0.0;
		if (jump) *jump = 0.0;
		if (x.size() != y.size()) return false;
		std::vector<double> a, b;
		for (size_t i = 0; i < x.size(); ++i)
		{
			if ((i < flatX.size() && flatX[i]) || (i < flatY.size() && flatY[i])) continue;
			a.push_back(x[i]);
			b.push_back(y[i]);
		}
		if (a.size() < 8) return false;
		const double count = static_cast<double>(a.size());
		double ma = 0.0, mb = 0.0;
		for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
		ma /= count; mb /= count;
		double va = 0.0, vb = 0.0, cab = 0.0;
		for (size_t i = 0; i < a.size(); ++i)
		{
			va += (a[i] - ma) * (a[i] - ma);
			vb += (b[i] - mb) * (b[i] - mb);
			cab += (a[i] - ma) * (b[i] - mb);
		}
		va /= count; vb /= count; cab /= count;
		// No shape to compare (black, a flat frame): not judged.
		if (std::sqrt(va) <= noise || std::sqrt(vb) <= noise) return false;
		const double slope = cab / va;
		const double residual = std::sqrt((std::max)(0.0, vb - slope * cab));
		decorrelation = residual / std::sqrt(vb);
		const double level = (std::max)({ 0.5 * (ma + mb) - blackLevel, noise, 1e-6 });
		share = residual / level;
		if (jump) *jump = std::fabs(mb - ma) / level;
		return true;
	}

	int TileSize(double height, int maximum)
	{
		// 256 px at 2160 (the size the detector's thresholds were measured with).
		const double wanted = height * 256.0 / 2160.0;
		int size = 128;
		while (size * 2 <= maximum && std::fabs(std::log2(wanted / (size * 2.0))) < std::fabs(std::log2(wanted / size))) size *= 2;
		return (std::min)(maximum, size);
	}

	double DetailAbove(const Spectrum& tile, double ratio, int axes, double* level)
	{
		return DetailAboveSpectra(tile.vertical, tile.horizontal, ratio, axes, level);
	}

	std::vector<TilePlace> PlaceTiles(double x0, double y0, double w, double h, int tile, bool dense, int* baseTiles)
	{
		std::vector<TilePlace> places;
		if (baseTiles) *baseTiles = 0;
		if (tile <= 0 || w <= 0.0 || h <= 0.0) return places;
		const int cols = (std::min)(4, static_cast<int>((w - 16.0) / tile));
		const int rows = (std::min)(2, static_cast<int>((h - 16.0) / tile));
		if (cols <= 0 || rows <= 0) return places;
		// A tile centred at `centre`, on the 16-px grid, inside [start, start + span).
		auto place = [tile](double start, double span, double centre) {
			const double o = std::floor((centre - tile / 2) / 16.0) * 16.0;
			const double lo = std::ceil(start / 16.0) * 16.0, hi = std::floor((start + span - tile) / 16.0) * 16.0;
			return static_cast<int>((std::max)(lo, (std::min)(hi, o)));
		};
		auto add = [&](double centreY, int c) {
			TilePlace p;
			p.x0 = place(x0, w, x0 + (c + 0.5) * w / cols);
			p.y0 = place(y0, h, centreY);
			p.centreX = static_cast<float>((p.x0 + tile * 0.5 - x0) / w);
			p.centreY = static_cast<float>((p.y0 + tile * 0.5 - y0) / h);
			places.push_back(p);
		};
		for (int r = 0; r < rows; ++r)
			for (int c = 0; c < cols; ++c) add(y0 + (r + 0.5) * h / rows, c);
		if (baseTiles) *baseTiles = static_cast<int>(places.size());
		// (Latch review 2026-10-17: with the tile a fraction of the height - TileSize - every picture
		// is about eight tiles high, so a 1080 picture, cropped or not, gets the dense grid too.)
		if (dense && rows == 2 && static_cast<int>((h - 16.0) / tile) >= 4)
			for (const double share : { 0.5, 0.875 })
				for (int c = 0; c < cols; ++c) add(y0 + share * h, c);
		return places;
	}

	void SetTileCentres(const std::vector<TilePlace>& places, Thresholds& thresholds)
	{
		thresholds.tileX.clear();
		thresholds.tileY.clear();
		for (const TilePlace& p : places) { thresholds.tileX.push_back(p.centreX); thresholds.tileY.push_back(p.centreY); }
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
		declaredByRendition = false;
		lastMean = -1.0;
		lastSignature = 0.0;
		lastSeconds = -1.0;
		evidence = 0.0;
		haveLast = false;
		lastPattern.clear();
		lastPatternFlat.clear();
		lastPatternSeconds = -1.0;
		cutJudge = CutJudge{};
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
			bool cut = false;
			if (settings.latchFixes)
			{
				// Latch fixes: a cut is a change of shape, compared with the last frame that had one
				// (so a fade through black between two shots is a cut, a fade within a shot is not).
				double decorrelation = 0.0, share = 0.0, jump = 0.0;
				if (PatternChange(lastPattern, lastPatternFlat, frame.pattern, frame.patternFlat, frame.blackLevel, frame.noiseLevel, decorrelation, share, &jump))
				{
					cut = cutJudge.Judge(decorrelation, jump, lastPatternSeconds >= 0.0 ? seconds - lastPatternSeconds : 0.0, settings);
					if (cut) lastCutSpan = lastPatternSeconds >= 0.0 ? seconds - lastPatternSeconds : 0.0;
					lastPattern = frame.pattern;
					lastPatternFlat = frame.patternFlat;
					lastPatternSeconds = seconds;
				}
				else
				{
					// No comparison: keep this pattern when it has a shape of its own.
					double d = 0.0, sh = 0.0;
					if (PatternChange(frame.pattern, frame.patternFlat, frame.pattern, frame.patternFlat, frame.blackLevel, frame.noiseLevel, d, sh))
					{
						lastPattern = frame.pattern;
						lastPatternFlat = frame.patternFlat;
						lastPatternSeconds = seconds;
					}
				}
			}
			else
			{
				cut = haveLast && std::fabs(frame.mean - lastMean) > settings.cutLevel;
				if (cut) lastCutSpan = dt;
			}
			if (cut) { ++shot; ++cuts; haveLastNative = false; streak = 0; streakDetail.clear(); }
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
		// A mixed screen (ReadFrame's guard found native UI tiles) is native detail by itself:
		// its averaged spectra hold the video, not the UI that made it native.
		if (ratio >= 1.0 && !frame.mixed)
		{
			const double inQuestion = declared < 1.0 ? declared : Candidate();
			// Latch fixes: level-aware - detail above the rate that is no more than the rounding noise
			// of the picture's bit depth (a dark or low-contrast frame) is no evidence either way.
			const bool dark = frame.contrast <= 0.0 || frame.contrast < settings.nativeFloorContrast;
			if (settings.latchFixes && inQuestion < 1.0 && dark)
			{
				double level = 0.0;
				const double d = DetailAbove(frame, inQuestion, &level);
				if (!std::isinf(d) && level < frame.quantisationFloor + settings.nativeFloorDecades)
				{
					++starved;
					++floorStarved;
					return;
				}
			}
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
			// A mixed screen is not the stream's native rendition (F12 compares against that).
			haveLastNative = !frame.verticalPower.empty() && !frame.mixed;
			lastNativeShot = shot;
			streak = 0;
			streakDetail.clear();
			if (settings.latchFixes)
			{
				// Latch fixes: native evidence ends the lower one - the window starts again after it
				// (the standard, minimumShots and minimumSeconds, from the readings since), instead of
				// holding re-engagement for windowSeconds. A mixed screen's frames go with it.
				window.clear();
				evidence = 0.0;
				renditionMode = false;
				if (declared < 1.0)
				{
					declared = 1.0;
					declaredByRendition = false;
				}
				return;
			}
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
			const bool streakLong = settings.latchFixes
				? seconds - streakStart >= settings.renditionStreakSeconds - 1e-6 && streakDetail.size() >= 3
				: streak >= settings.renditionFrames && streakDetail.size() >= static_cast<size_t>(settings.renditionFrames);
			if (streakLong && haveLastNative && shot == lastNativeShot && !renditionMode &&
				declared >= 1.0)
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
			declaredByRendition = false;
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
				declaredByRendition = true;
			}
			return;
		}
		// Lower only when the whole window - several shots, a sustained time -
		// stays below the declared cutoff.
		const double candidate = Candidate();
		// Latch fixes (owner 2026-10-17): the shots OR the time, whichever comes first - the tiles are
		// independent readings, so one long shot can confirm on its own.
		const bool enough = settings.latchFixes
			? Shots() >= settings.minimumShots || EvidenceSeconds() >= settings.minimumSeconds
			: Shots() >= settings.minimumShots && EvidenceSeconds() >= settings.minimumSeconds;
		if (candidate < declared - settings.tolerance && enough)
		{
			declared = candidate;
			declaredByRendition = false;
		}
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
