#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// Owner 2026-10-16 ("what about with the subtitles"): subtitle text drawn at the
// output resolution over an upscaled picture, as a streaming player draws its own
// subtitles (Apple TV: the video upscaled from 1080p, the text rendered at 4K).
// White strokes with a black outline, anti-aliased over one pixel, limited-range
// levels (64 / 940 of 1023). Values are code / 1023.
namespace SubtitleTextTestPattern
{
	constexpr double kBlack = 64.0 / 1023.0;
	constexpr double kWhite = 940.0 / 1023.0;

	inline double SegmentDistance(double px, double py, double ax, double ay, double bx, double by)
	{
		const double vx = bx - ax, vy = by - ay;
		const double len2 = vx * vx + vy * vy;
		const double t = len2 > 0 ? (std::min)(1.0, (std::max)(0.0, ((px - ax) * vx + (py - ay) * vy) / len2)) : 0.0;
		const double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
		return std::sqrt(dx * dx + dy * dy);
	}

	// One line of glyph-like characters in [x0, x1) x [y0, y0 + height): each glyph a few
	// strokes (stems, bars, diagonals) in its cell. `coverage` (optional) receives 1 where
	// the text (outline included) covers the pixel, for masks in the measurements.
	inline void Draw(std::vector<float>& image, int width, int height, int x0, int x1, int y0, int textHeight, unsigned seed,
		std::vector<unsigned char>* coverage = nullptr)
	{
		std::mt19937 rng(seed);
		std::uniform_real_distribution<double> u(0.0, 1.0);
		const double cell = textHeight * 0.62;
		const double stroke = (std::max)(1.6, textHeight * 0.055);   // half-width of the white stroke
		const double outline = stroke + (std::max)(1.5, textHeight * 0.045);   // half-width with the black edge
		struct Seg { double ax, ay, bx, by; };
		std::vector<Seg> segs;
		for (double cx = x0 + 4; cx + cell <= x1 - 4; cx += cell)
		{
			if (u(rng) < 0.15) continue;   // a space
			const int strokes = 2 + static_cast<int>(u(rng) * 3);
			for (int s = 0; s < strokes; ++s)
			{
				const double l = cx + cell * 0.18, r = cx + cell * 0.82, t = y0 + textHeight * 0.12, b = y0 + textHeight * 0.88;
				const double m = (t + b) / 2;
				switch (static_cast<int>(u(rng) * 5))
				{
				case 0: segs.push_back({ l, t, l, b }); break;            // left stem
				case 1: segs.push_back({ r, m, r, b }); break;            // short right stem
				case 2: segs.push_back({ l, m, r, m }); break;            // bar
				case 3: segs.push_back({ l, b, r, t }); break;            // diagonal
				default: segs.push_back({ l, t, r, t }); break;           // top bar
				}
			}
		}
		const int top = (std::max)(0, static_cast<int>(y0 - outline - 2));
		const int bottom = (std::min)(height, static_cast<int>(y0 + textHeight + outline + 2));
		for (int y = top; y < bottom; ++y)
			for (int x = (std::max)(0, x0); x < (std::min)(width, x1); ++x)
			{
				double d = 1e9;
				for (const Seg& sg : segs)
				{
					if (x + outline + 2 < (std::min)(sg.ax, sg.bx) || x - outline - 2 > (std::max)(sg.ax, sg.bx)) continue;
					d = (std::min)(d, SegmentDistance(x + 0.5, y + 0.5, sg.ax, sg.ay, sg.bx, sg.by));
				}
				// Anti-aliased: coverage of the outline disc, then of the white stroke within it.
				const double outer = (std::min)(1.0, (std::max)(0.0, outline + 0.5 - d));
				if (outer <= 0.0) continue;
				const double inner = (std::min)(1.0, (std::max)(0.0, stroke + 0.5 - d));
				float& v = image[static_cast<size_t>(y) * width + x];
				const double text = inner * kWhite + (1.0 - inner) * kBlack;
				v = static_cast<float>(std::round((outer * text + (1.0 - outer) * v) * 1023.0) / 1023.0);
				if (coverage) (*coverage)[static_cast<size_t>(y) * width + x] = 1;
			}
	}
}
