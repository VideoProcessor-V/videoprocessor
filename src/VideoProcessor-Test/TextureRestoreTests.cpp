#include "pch.h"
#include "CppUnitTest.h"

#include "TextureRestoreModel.h"
#include "ExtendedRangeGpuProbe.h"

#include <vprenderer/TextureRestore.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// texture_restore (docs/VP-detail-restore.md; algorithm and constants:
// src/VideoProcessor-Lib/vprenderer/TextureRestore.h). CPU tests run the reference model
// (TextureRestoreModel.h, which mirrors the shader); TextureRestoreGpu* tests check the shader
// against it on a libplacebo device (WARP unless VP_TEST_GPU=hardware); TextureRestoreRealClip*
// runs the model on the x265 breathing clips (VP_TEST_REAL_FOOTAGE).
namespace TextureRestoreTestsNs
{
	using TextureRestoreModel::Image;
	using TextureRestoreModel::Field;
	using TextureRestoreModel::Model;
	using TextureRestoreModel::BandEnergy;
	using TextureRestoreModel::MeasureBands;

	constexpr int kW = 256, kH = 128;
	constexpr double kCode8 = 4.0;   // one 8-bit code in 10-bit codes

	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	void Log(const char* text) { Logger::WriteMessage(text); }

	Image Blur(const Image& a, double s)
	{
		const int r = (std::max)(1, static_cast<int>(std::ceil(3.0 * s)));
		std::vector<double> k(2 * r + 1);
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5 * i * i / (s * s));
		for (double& v : k) v /= sum;
		Image t(a.w, a.h), o(a.w, a.h);
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x)
			{
				double acc = 0.0;
				for (int i = -r; i <= r; ++i) acc += k[i + r] * a.get(x + i, y);
				t.at(x, y) = acc;
			}
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x)
			{
				double acc = 0.0;
				for (int i = -r; i <= r; ++i) acc += k[i + r] * t.get(x, y + i);
				o.at(x, y) = acc;
			}
		return o;
	}

	Image Noise(int w, int h, std::mt19937& rng)
	{
		std::normal_distribution<double> n(0.0, 1.0);
		Image g(w, h);
		for (double& v : g.v) v = n(rng);
		return g;
	}

	double Std(const Image& a, int margin)
	{
		double mean = 0.0, sq = 0.0;
		int n = 0;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x) { mean += a.at(x, y); ++n; }
		mean /= n;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x) sq += (a.at(x, y) - mean) * (a.at(x, y) - mean);
		return std::sqrt(sq / n);
	}

	double Corr(const Image& a, const Image& b, int margin)
	{
		double ma = 0, mb = 0;
		int n = 0;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x) { ma += a.at(x, y); mb += b.at(x, y); ++n; }
		ma /= n; mb /= n;
		double sab = 0, saa = 0, sbb = 0;
		for (int y = margin; y < a.h - margin; ++y)
			for (int x = margin; x < a.w - margin; ++x)
			{
				const double da = a.at(x, y) - ma, db = b.at(x, y) - mb;
				sab += da * db; saa += da * da; sbb += db * db;
			}
		return sab / std::sqrt(saa * sbb + 1e-30);
	}

	Image Diff(const Image& a, const Image& b)
	{
		Image o(a.w, a.h);
		for (size_t i = 0; i < o.v.size(); ++i) o.v[i] = a.v[i] - b.v[i];
		return o;
	}

	Image Sum(const Image& a, const Image& b)
	{
		Image o(a.w, a.h);
		for (size_t i = 0; i < o.v.size(); ++i) o.v[i] = a.v[i] + b.v[i];
		return o;
	}

	// Film grain of RMS sigma (codes): white, or soft (a 0.7 px Gaussian), fresh per call.
	Image Grain(int w, int h, double sigma, bool soft, std::mt19937& rng)
	{
		Image g = Noise(w, h, rng);
		if (soft) g = Blur(g, 0.7);
		const double s = Std(g, 0);
		for (double& v : g.v) v *= sigma / s;
		return g;
	}

	// A natural, step-like picture (10-bit codes): sharp discs (edges) and patches of fine
	// texture. In focus its mid-band share F is about 0.37; defocused by 3 px 0.12, 3.5 px 0.11.
	Image Scene(int w, int h, std::mt19937& rng, double textureRms = 14.0)
	{
		std::uniform_real_distribution<double> u(0.0, 1.0);
		Image d(w, h);
		const int discs = w * h / 500;
		for (int k = 0; k < discs; ++k)
		{
			const double cx = u(rng) * w, cy = u(rng) * h, r = 2.0 + 12.0 * u(rng), a = -60.0 + 120.0 * u(rng);
			for (int y = (std::max)(0, static_cast<int>(cy - r) - 1); y < (std::min)(h, static_cast<int>(cy + r) + 2); ++y)
				for (int x = (std::max)(0, static_cast<int>(cx - r) - 1); x < (std::min)(w, static_cast<int>(cx + r) + 2); ++x)
					if (std::hypot(x - cx, y - cy) < r) d.at(x, y) += a;
		}
		d = Blur(d, 0.6);
		Image m = Blur(Noise(w, h, rng), 12.0);
		std::vector<double> sorted = m.v;
		std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
		const double median = sorted[sorted.size() / 2];
		for (double& v : m.v) v = v > median ? 1.0 : 0.0;
		m = Blur(m, 2.0);
		Image t = Sum(Blur(Noise(w, h, rng), 0.8), Blur(Noise(w, h, rng), 2.0));
		const double s1 = Std(t, 0);
		Image out(w, h);
		for (size_t i = 0; i < out.v.size(); ++i) out.v[i] = 512.0 + d.v[i] + m.v[i] * t.v[i] * textureRms / s1;
		return out;
	}

	// An HEVC-like weak deblocking filter across the 8x8 grid (x265's in-loop filter leaves no
	// block grid on the real clips): tc = q / 4, edges with local activity below q.
	void Deblock(Image& im, double q)
	{
		const double tc = q / 4.0, beta = q;
		auto clip = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
		auto edge = [&](double& p2, double& p1, double& p0, double& q0, double& q1, double& q2) {
			if (std::fabs(p2 - 2.0 * p1 + p0) + std::fabs(q2 - 2.0 * q1 + q0) >= beta) return;
			double d = (9.0 * (q0 - p0) - 3.0 * (q1 - p1)) / 16.0;
			if (std::fabs(d) >= 10.0 * tc) return;
			d = clip(d, -tc, tc);
			const double dp = clip(((p2 + p0) / 2.0 - p1 + d) / 2.0, -tc / 2.0, tc / 2.0);
			const double dq = clip(((q2 + q0) / 2.0 - q1 - d) / 2.0, -tc / 2.0, tc / 2.0);
			p0 += d; q0 -= d; p1 += dp; q1 += dq;
		};
		for (int x = 8; x + 3 <= im.w; x += 8)
			for (int y = 0; y < im.h; ++y)
				edge(im.at(x - 3, y), im.at(x - 2, y), im.at(x - 1, y), im.at(x, y), im.at(x + 1, y), im.at(x + 2, y));
		for (int y = 8; y + 3 <= im.h; y += 8)
			for (int x = 0; x < im.w; ++x)
				edge(im.at(x, y - 3), im.at(x, y - 2), im.at(x, y - 1), im.at(x, y), im.at(x, y + 1), im.at(x, y + 2));
	}

	// 8x8 DCT quantisation (a codec's residual coding without prediction): every AC
	// coefficient to a step q with a dead zone (x265 inter rounding, 1/6), DC kept; then the
	// in-loop deblocking.
	Image Dctq(const Image& img, double q)
	{
		static double c[8][8];
		static bool init = false;
		if (!init)
		{
			for (int u = 0; u < 8; ++u)
				for (int x = 0; x < 8; ++x)
					c[u][x] = (u == 0 ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
			init = true;
		}
		Image out(img.w, img.h);
		for (int by = 0; by + 8 <= img.h; by += 8)
			for (int bx = 0; bx + 8 <= img.w; bx += 8)
			{
				double tmp[8][8], co[8][8];
				for (int v = 0; v < 8; ++v)
					for (int x = 0; x < 8; ++x)
					{
						double acc = 0.0;
						for (int y = 0; y < 8; ++y) acc += c[v][y] * img.at(bx + x, by + y);
						tmp[v][x] = acc;
					}
				for (int v = 0; v < 8; ++v)
					for (int u = 0; u < 8; ++u)
					{
						double acc = 0.0;
						for (int x = 0; x < 8; ++x) acc += c[u][x] * tmp[v][x];
						if (u != 0 || v != 0)
						{
							const double a = std::floor(std::fabs(acc) / q + 1.0 / 6.0) * q;
							acc = acc < 0 ? -a : a;
						}
						co[v][u] = acc;
					}
				for (int v = 0; v < 8; ++v)
					for (int x = 0; x < 8; ++x)
					{
						double acc = 0.0;
						for (int u = 0; u < 8; ++u) acc += c[u][x] * co[v][u];
						tmp[v][x] = acc;
					}
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
					{
						double acc = 0.0;
						for (int v = 0; v < 8; ++v) acc += c[v][y] * tmp[v][x];
						out.at(bx + x, by + y) = acc;
					}
			}
		Deblock(out, q);
		return out;
	}

	TextureRestore::Params Frame(uint64_t sequence, double cap = TextureRestore::kDefaultCap, double hz = 24.0)
	{
		TextureRestore::Params p;
		p.cap = cap;
		p.colorDepth = 10;
		p.sourceHz = hz;
		p.bitrateQuality = 1.0;
		p.sequence = sequence;
		return p;
	}

	double MaxAbsDiff(const Image& a, const Image& b)
	{
		double m = 0.0;
		for (size_t i = 0; i < a.v.size(); ++i) m = (std::max)(m, std::fabs(a.v[i] - b.v[i]));
		return m;
	}

	// The mean square of (a - b) over a rectangle.
	double Mse(const Image& a, const Image& b, int x0, int y0, int x1, int y1)
	{
		double s = 0.0;
		int n = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) { const double d = a.at(x, y) - b.at(x, y); s += d * d; ++n; }
		return s / (std::max)(n, 1);
	}

	// Tiles whose centre lies in [x0, x1) x [y0, y1).
	template <typename F>
	void ForTilesIn(const Model& m, int x0, int y0, int x1, int y1, F f)
	{
		const int T = m.Tile();
		for (int ty = 0; ty < m.TileH(); ++ty)
			for (int tx = 0; tx < m.TileW(); ++tx)
			{
				const double cx = (tx + 0.5) * T, cy = (ty + 0.5) * T;
				if (cx >= x0 && cx < x1 && cy >= y0 && cy < y1) f(m.Debug()[static_cast<size_t>(ty) * m.TileW() + tx]);
			}
	}

	// A stand-in for the de-judder's field (test infrastructure only: the module never matches):
	// a global translation from a quarter-resolution search, refined per 16x16 block by +-3 px;
	// a block is trusted when its match error is small. v = the content's motion prev -> cur.
	Field BlockField(const Image& prev, const Image& cur)
	{
		auto quarter = [](const Image& a) {
			Image q(a.w / 4, a.h / 4);
			for (int y = 0; y < q.h; ++y)
				for (int x = 0; x < q.w; ++x)
				{
					double s = 0.0;
					for (int j = 0; j < 4; ++j)
						for (int i = 0; i < 4; ++i) s += a.at(4 * x + i, 4 * y + j);
					q.at(x, y) = s / 16.0;
				}
			return q;
		};
		const Image qp = quarter(prev), qc = quarter(cur);
		int gx = 0, gy = 0;
		double best = 1e300;
		const int R = 10, m = 12;
		for (int dy = -R; dy <= R; ++dy)
			for (int dx = -R; dx <= R; ++dx)
			{
				double s = 0.0;
				for (int y = m; y < qc.h - m; ++y)
					for (int x = m; x < qc.w - m; ++x) s += std::fabs(qc.at(x, y) - qp.at(x + dx, y + dy));
				if (s < best) { best = s; gx = dx; gy = dy; }
			}
		Field f((cur.w + 15) / 16, (cur.h + 15) / 16, 0.0f, 0.0f, 0.0f);
		for (int by = 0; by < f.h; ++by)
			for (int bx = 0; bx < f.w; ++bx)
			{
				double bs = 1e300;
				int ox = 0, oy = 0;
				for (int dy = -3; dy <= 3; ++dy)
					for (int dx = -3; dx <= 3; ++dx)
					{
						const int sx = 4 * gx + dx, sy = 4 * gy + dy;
						double s = 0.0;
						for (int y = by * 16; y < (std::min)(cur.h, by * 16 + 16); ++y)
							for (int x = bx * 16; x < (std::min)(cur.w, bx * 16 + 16); ++x) s += std::fabs(cur.at(x, y) - prev.get(x + sx, y + sy));
						if (s < bs) { bs = s; ox = sx; oy = sy; }
					}
				float* v = f.at(bx, by);
				v[0] = static_cast<float>(-ox);
				v[1] = static_cast<float>(-oy);
				v[2] = bs / 256.0 < 6.0 ? 1.0f : 0.0f;
			}
		return f;
	}

	// A GOP: I every `gop` frames (step qI), B between (qB).
	bool IsI(int t, int gop) { return t % gop == 0; }
}

namespace TextureRestoreTestsNs
{
	TEST_CLASS(TextureRestoreTests)
	{
	public:
		TEST_METHOD(TextureRestoreKeepsSteadyGrain)
		{
			// A static picture with fresh grain every frame (no coding: every frame equally good):
			// grain RMS within +-5 % at every frame, no more frame-to-frame grain correlation than
			// the input; white and soft, 0.5-4 8-bit codes.
			for (bool soft : { false, true })
				for (double sigma8 : { 0.5, 1.0, 2.0, 4.0 })
				{
					std::mt19937 rng(11u + static_cast<unsigned>(sigma8 * 8) + (soft ? 100u : 0u));
					const Image base = Scene(kW, kH, rng);
					Model model;
					Image prevIn, prevOut;
					double worst = 0.0, corrIn = 0.0, corrOut = 0.0;
					int n = 0;
					for (int t = 0; t < 24; ++t)
					{
						const Image g = Grain(kW, kH, sigma8 * kCode8, soft, rng);
						const Image in = Sum(base, g);
						const Image out = model.Frame(in, Frame(100 + t));
						const Image gin = Diff(in, base), gout = Diff(out, base);
						const double ratio = Std(gout, 16) / Std(gin, 16);
						worst = (std::max)(worst, std::fabs(ratio - 1.0));
						if (t > 0)
						{
							corrIn += Corr(gin, prevIn, 16);
							corrOut += Corr(gout, prevOut, 16);
							++n;
						}
						prevIn = gin;
						prevOut = gout;
					}
					corrIn /= n; corrOut /= n;
					char text[200];
					snprintf(text, sizeof(text), "steady %s grain %.1f: worst RMS change %.4f, tcorr in %.4f out %.4f", soft ? "soft" : "white", sigma8, worst, corrIn, corrOut);
					Log(text);
					Assert::IsTrue(worst <= 0.05, Wide(text).c_str());
					Assert::IsTrue(corrOut <= corrIn + 0.01, Wide(text).c_str());
				}
		}

		TEST_METHOD(TextureRestoreSynthesisesNoGrain)
		{
			// The reference frame is grainy, the next frames have none (a flat picture): nothing
			// is added - the stage only scales detail the frame has (output == input exactly).
			// And a textured frame whose grain the coder removed gets no grain-like energy in its
			// flat parts.
			std::mt19937 rng(5u);
			Image flat(kW, kH, 400.0);
			Model model;
			model.Frame(Sum(flat, Grain(kW, kH, 8.0 * kCode8, false, rng)), Frame(1));
			for (int t = 1; t < 6; ++t)
			{
				const Image out = model.Frame(flat, Frame(1 + t));
				Assert::AreEqual(0.0, MaxAbsDiff(out, flat), L"a frame without grain gets none");
			}
			// Coded: the I-frame keeps grain, the B-frames lose it; a flat patch stays flat.
			Image picture = Scene(kW, kH, rng);
			for (int y = 32; y < 96; ++y)
				for (int x = 128; x < 224; ++x) picture.at(x, y) = 500.0;
			Model coded;
			double worstAdded = 0.0;
			for (int t = 0; t < 8; ++t)
			{
				// The coder kept the grain on the I-frame and removed it from the B-frames.
				const Image in = t == 0 ? Dctq(Sum(picture, Grain(kW, kH, 3.0 * kCode8, false, rng)), 2.0) : Dctq(picture, 40.0);
				const Image out = coded.Frame(in, Frame(10 + t));
				if (t > 0) worstAdded = (std::max)(worstAdded, Mse(out, in, 152, 48, 200, 80));
			}
			char text[160];
			snprintf(text, sizeof(text), "flat patch after the coder removed the grain: added energy %.6f codes^2", worstAdded);
			Log(text);
			Assert::IsTrue(worstAdded <= 1e-12, Wide(text).c_str());
		}

		TEST_METHOD(TextureRestoreAddsNoTextureWhereDefocused)
		{
			// (a) The right half defocused (3.5 px) in every frame, GOP coded: its tiles get texture
			// gain 1 exactly; added energy no more than the grain equalisation of its own grain
			// allows ((cap^2 - 1) x its fine band). The left half (in focus) is restored.
			for (double sigma8 : { 0.0, 2.0 })
			{
				std::mt19937 rng(21u + static_cast<unsigned>(sigma8));
				const Image sharp = Scene(kW, kH, rng);
				const Image soft = Blur(sharp, 3.5);
				Image base = sharp;
				for (int y = 0; y < kH; ++y)
					for (int x = kW / 2; x < kW; ++x) base.at(x, y) = soft.at(x, y);
				Model model;
				double maxGainT = 1.0, added = 0.0, allowed = 0.0, leftGain = 1.0;
				for (int t = 0; t < 16; ++t)
				{
					const Image in = Dctq(Sum(base, Grain(kW, kH, sigma8 * kCode8, true, rng)), IsI(t, 8) ? 4.0 : 24.0);
					const Image out = model.Frame(in, Frame(30 + t));
					ForTilesIn(model, kW / 2 + 32, 0, kW, kH, [&](const TextureRestoreModel::TileDebug& d) { maxGainT = (std::max)(maxGainT, (std::max)(d.gT1, d.gT2)); });
					ForTilesIn(model, 16, 16, kW / 2 - 32, kH - 16, [&](const TextureRestoreModel::TileDebug& d) { leftGain = (std::max)(leftGain, d.gT1); });
					if (!IsI(t, 8))
					{
						added = (std::max)(added, Mse(out, in, kW / 2 + 40, 8, kW - 8, kH - 8));
						const BandEnergy e = MeasureBands(in, kW / 2 + 40, 8, kW - 8, kH - 8);
						allowed = (std::max)(allowed, (1.5 * 1.5 - 1.0) * e.e1);
					}
				}
				char text[200];
				snprintf(text, sizeof(text), "defocused half, grain %.1f: max texture gain %.6f, added %.5f codes^2 (grain equalisation bound %.5f); in-focus half gain %.3f",
					sigma8, maxGainT, added, allowed, leftGain);
				Log(text);
				Assert::IsTrue(maxGainT <= 1.0 + 1e-9, Wide(text).c_str());
				if (sigma8 == 0.0) Assert::AreEqual(1.0, maxGainT, Wide(text).c_str());
				if (sigma8 == 0.0) Assert::IsTrue(added <= 1e-3, Wide(text).c_str());
				Assert::IsTrue(added <= allowed + 1e-3, Wide(text).c_str());
				Assert::IsTrue(leftGain > 1.05, (L"the in-focus half is restored: " + Wide(text)).c_str());
			}
			// (b) A focus pull: frames 0-7 sharp (I at 0), from frame 8 the right half defocused
			// by 3 px (B-frames): no texture gain there, no texture energy added.
			for (double sigma8 : { 0.0, 2.0 })
			{
				std::mt19937 rng(41u + static_cast<unsigned>(sigma8));
				const Image sharp = Scene(kW, kH, rng);
				const Image soft = Blur(sharp, 3.0);
				Model model;
				double maxGainT = 1.0, added = 0.0, allowed = 0.0;
				for (int t = 0; t < 14; ++t)
				{
					Image base = sharp;
					if (t >= 8)
						for (int y = 0; y < kH; ++y)
							for (int x = kW / 2; x < kW; ++x) base.at(x, y) = soft.at(x, y);
					const Image in = Dctq(Sum(base, Grain(kW, kH, sigma8 * kCode8, true, rng)), t == 0 ? 4.0 : 24.0);
					const Image out = model.Frame(in, Frame(60 + t));
					if (t >= 8)
					{
						ForTilesIn(model, kW / 2 + 32, 0, kW, kH, [&](const TextureRestoreModel::TileDebug& d) { maxGainT = (std::max)(maxGainT, (std::max)(d.gT1, d.gT2)); });
						added = (std::max)(added, Mse(out, in, kW / 2 + 40, 8, kW - 8, kH - 8));
						const BandEnergy e = MeasureBands(in, kW / 2 + 40, 8, kW - 8, kH - 8);
						allowed = (std::max)(allowed, (1.5 * 1.5 - 1.0) * e.e1);
					}
				}
				char text[200];
				snprintf(text, sizeof(text), "focus pull, grain %.1f: max texture gain %.6f, added %.5f codes^2 (grain equalisation bound %.5f)", sigma8, maxGainT, added, allowed);
				Log(text);
				Assert::IsTrue(maxGainT <= 1.0 + 1e-9, Wide(text).c_str());
				if (sigma8 == 0.0) Assert::AreEqual(1.0, maxGainT, Wide(text).c_str());
				if (sigma8 == 0.0) Assert::IsTrue(added <= 1e-3, Wide(text).c_str());
				Assert::IsTrue(added <= allowed + 1e-3, Wide(text).c_str());
			}
		}

		TEST_METHOD(TextureRestoreRestoresCodedTexture)
		{
			// A static textured picture, fresh grain per frame, GOP 8 (I finely coded, B coarsely):
			// on B-frames the fine and mid bands move towards the original's and never above the
			// best-coded (I) frame's; I-frames unchanged (band gain 1.00 +- 0.01).
			for (int variant = 0; variant < 3; ++variant)
			{
				const double sigma8 = variant == 0 ? 0.0 : (variant == 1 ? 0.75 : 1.5);
				const bool softGrain = variant == 2;
				std::mt19937 rng(71u + variant);
				const Image base = Scene(kW, kH, rng);
				Model model;
				BandEnergy lastI;
				double towards1 = 0.0, towards2 = 0.0, overI = 0.0, iGain = 0.0, mseIn = 0.0, mseOut = 0.0;
				bool closerToOrig = true;
				int nb = 0;
				for (int t = 0; t < 24; ++t)
				{
					const Image orig = Sum(base, Grain(kW, kH, sigma8 * kCode8, softGrain, rng));
					const Image in = Dctq(orig, IsI(t, 8) ? 4.0 : 24.0);
					const Image out = model.Frame(in, Frame(200 + t));
					const BandEnergy eo = MeasureBands(orig, 16, 16, kW - 16, kH - 16);
					const BandEnergy ei = MeasureBands(in, 16, 16, kW - 16, kH - 16);
					const BandEnergy eu = MeasureBands(out, 16, 16, kW - 16, kH - 16);
					if (IsI(t, 8))
					{
						lastI = ei;
						iGain = (std::max)(iGain, std::fabs(std::sqrt(eu.e1 / ei.e1) - 1.0));
						iGain = (std::max)(iGain, std::fabs(std::sqrt(eu.e2 / ei.e2) - 1.0));
						continue;
					}
					// The share of the gap to the best-coded (I) frame's level closed; the original is
					// further up (its lost grain cannot come back: nothing is synthesised).
					towards1 += (eu.e1 - ei.e1) / (lastI.e1 - ei.e1);
					towards2 += (eu.e2 - ei.e2) / (lastI.e2 - ei.e2);
					closerToOrig = closerToOrig && std::fabs(eu.e1 - eo.e1) <= std::fabs(ei.e1 - eo.e1) && std::fabs(eu.e2 - eo.e2) <= std::fabs(ei.e2 - eo.e2);
					overI = (std::max)(overI, (std::max)(eu.e1 / lastI.e1, eu.e2 / lastI.e2));
					mseIn += Mse(in, orig, 16, 16, kW - 16, kH - 16);
					mseOut += Mse(out, orig, 16, 16, kW - 16, kH - 16);
					++nb;
				}
				towards1 /= nb; towards2 /= nb;
				char text[240];
				snprintf(text, sizeof(text), "coded texture, %s grain %.2f: B gap to the I level closed fine %.3f mid %.3f; closer to the original %d; max over the I level %.3f; I-frame band gain |g-1| %.4f; B MSE %.2f -> %.2f",
					softGrain ? "soft" : "white", sigma8, towards1, towards2, closerToOrig ? 1 : 0, overI, iGain, mseIn / nb, mseOut / nb);
				Log(text);
				// With grain the I level includes grain the B-frames lost (it cannot come back).
				Assert::IsTrue(towards1 > (sigma8 > 0.0 ? 0.1 : 0.3) && towards1 <= 1.0, Wide(text).c_str());
				Assert::IsTrue(towards2 > 0.15 && towards2 <= 1.0, Wide(text).c_str());
				Assert::IsTrue(closerToOrig, Wide(text).c_str());
				Assert::IsTrue(overI <= 1.0, Wide(text).c_str());
				Assert::IsTrue(iGain <= 0.01, Wide(text).c_str());
			}
		}

		TEST_METHOD(TextureRestoreFollowsVectorsOrStaysStill)
		{
			// The left half static, the right half panning 3 px per frame (GOP coded).
			// Without vectors: static tiles restore, moving ones do not (their references restart
			// where the low band moved). With a trusted field the moving half restores along the
			// vectors; an untrusted field restarts every block (output == input).
			std::mt19937 rng(91u);
			const Image big = Scene(kW + 64, kH, rng);
			auto frameAt = [&](int t) {
				Image f(kW, kH);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x)
						f.at(x, y) = x < kW / 2 ? big.at(x, y) : big.at((std::min)(kW + 63, x + 60 - 3 * t), y);
				return f;
			};
			const int fw = (kW + 15) / 16, fh = (kH + 15) / 16;
			Field moving(fw, fh, 0.0f, 0.0f, 1.0f);
			for (int y = 0; y < fh; ++y)
				for (int x = fw / 2; x < fw; ++x) moving.at(x, y)[0] = 3.0f;   // the content moves right by 3 px a frame
			const Field untrusted(fw, fh, 0.0f, 0.0f, 0.0f);
			for (int mode = 0; mode < 3; ++mode)
			{
				Model model;
				double staticGain = 0.0, movingGain = 0.0, movingDiff = 0.0;
				int nb = 0;
				for (int t = 0; t < 8; ++t)
				{
					const Image in = Dctq(frameAt(t), t == 0 ? 4.0 : 24.0);
					const Image out = model.Frame(in, Frame(300 + t), mode == 1 ? &moving : (mode == 2 ? &untrusted : nullptr));
					if (t == 0) continue;
					const BandEnergy si = MeasureBands(in, 16, 16, kW / 2 - 24, kH - 16), so = MeasureBands(out, 16, 16, kW / 2 - 24, kH - 16);
					const BandEnergy mi = MeasureBands(in, kW / 2 + 24, 16, kW - 16, kH - 16), mo = MeasureBands(out, kW / 2 + 24, 16, kW - 16, kH - 16);
					staticGain += so.e1 / si.e1;
					movingGain += mo.e1 / mi.e1;
					for (int y = 0; y < kH; ++y)
						for (int x = kW / 2 + 24; x < kW; ++x) movingDiff = (std::max)(movingDiff, std::fabs(out.at(x, y) - in.at(x, y)));
					if (mode == 2) Assert::AreEqual(0.0, MaxAbsDiff(out, in), L"untrusted blocks restart: output == input");
					++nb;
				}
				staticGain /= nb; movingGain /= nb;
				char text[200];
				snprintf(text, sizeof(text), "%s: static half fine-band energy x%.3f, moving half x%.3f (max change %.4f codes)",
					mode == 0 ? "no vectors" : (mode == 1 ? "trusted vectors" : "untrusted field"), staticGain, movingGain, movingDiff);
				Log(text);
				if (mode == 0)
				{
					Assert::IsTrue(staticGain > 1.1, Wide(text).c_str());
					Assert::IsTrue(movingGain < 1.03, (L"moving content without vectors is not restored: " + Wide(text)).c_str());
				}
				else if (mode == 1)
				{
					Assert::IsTrue(staticGain > 1.1, Wide(text).c_str());
					Assert::IsTrue(movingGain > 1.1, Wide(text).c_str());
				}
			}
		}

		TEST_METHOD(TextureRestoreRestartsAtDiscontinuities)
		{
			std::mt19937 rng(101u);
			const Image a = Scene(kW, kH, rng), b = Scene(kW, kH, rng);
			auto coded = [&](const Image& x, bool i) { return Dctq(x, i ? 4.0 : 24.0); };
			// Cut: the reference of A never reaches B.
			{
				Model model;
				model.Frame(coded(a, true), Frame(1));
				model.Frame(coded(a, false), Frame(2));
				const Image in = coded(b, false);
				const Image out = model.Frame(in, Frame(3));
				Assert::IsTrue(model.LastWasCut(), L"a cut is detected");
				Assert::AreEqual(0.0, MaxAbsDiff(out, in), L"cut: references restart (output == input)");
			}
			// Fades: the picture dims 6 % a frame (every tile changes: a cut) or 0.4 % a frame (below
			// the tiles' allowance: the frame mean's ramp); from the second ramp step on the
			// references restart.
			for (double rate : { 0.94, 0.996 })
			{
				Model model;
				bool fadeSeen = false;
				for (int t = 0; t < 6; ++t)
				{
					Image x = a;
					for (double& v : x.v) v = 64.0 + (v - 64.0) * std::pow(rate, t);
					const Image in = coded(x, t == 0);
					const Image out = model.Frame(in, Frame(10 + t));
					if (t >= 2)
					{
						fadeSeen = fadeSeen || model.LastWasCut();
						Assert::AreEqual(0.0, MaxAbsDiff(out, in), L"fade: references restart (output == input)");
					}
				}
				Assert::IsTrue(fadeSeen, L"the fade is detected");
			}
			// Forget, non-consecutive sequences, sequence 0.
			for (int kind = 0; kind < 3; ++kind)
			{
				Model model;
				model.Frame(coded(a, true), Frame(20));
				const Image in = coded(a, false);
				Image out;
				if (kind == 0) { model.Forget(); out = model.Frame(in, Frame(21)); }
				else if (kind == 1) out = model.Frame(in, Frame(25));
				else out = model.Frame(in, Frame(0));
				Assert::AreEqual(0.0, MaxAbsDiff(out, in), L"restart: output == input");
			}
			// Repeats reuse the frame's gains and never advance: I, B, B(repeat), B == I, B, B.
			{
				Model once, twice;
				const Image i0 = coded(a, true), b1 = coded(a, false);
				std::mt19937 g(7u);
				const Image b2 = Dctq(Sum(a, Grain(kW, kH, 2.0, false, g)), 24.0);
				once.Frame(i0, Frame(40));
				twice.Frame(i0, Frame(40));
				const Image o1 = once.Frame(b1, Frame(41));
				const Image t1 = twice.Frame(b1, Frame(41));
				const Image t1r = twice.Frame(b1, Frame(41));
				Assert::IsTrue(twice.LastWasRepeat());
				Assert::AreEqual(0.0, MaxAbsDiff(t1, t1r), L"a repeat gives the same picture");
				Assert::AreEqual(0.0, MaxAbsDiff(o1, t1), L"the same picture as without the repeat");
				Assert::IsTrue(MaxAbsDiff(o1, b1) > 0.1, L"the B-frame is restored");
				const Image o2 = once.Frame(b2, Frame(42));
				const Image t2 = twice.Frame(b2, Frame(42));
				Assert::AreEqual(0.0, MaxAbsDiff(o2, t2), L"the repeat did not advance the references");
			}
		}

		TEST_METHOD(TextureRestoreIsRateIndependent)
		{
			// The same pictures at 24p (GOP 8) and 48p (each picture twice, GOP 16): the
			// references decay per second, so the gains at the same instants agree.
			std::mt19937 rng(111u);
			const Image base = Scene(kW, kH, rng);
			const Image iFrame = Dctq(base, 4.0), bFrame = Dctq(base, 24.0);
			Model m24, m48;
			for (int t = 0; t < 8; ++t)
			{
				const Image& in = t == 0 ? iFrame : bFrame;
				const Image o24 = m24.Frame(in, Frame(500 + t, 1.5, 24.0));
				m48.Frame(in, Frame(500 + 2 * t, 1.5, 48.0));
				const Image o48 = m48.Frame(in, Frame(501 + 2 * t, 1.5, 48.0));
				char text[120];
				snprintf(text, sizeof(text), "frame %d: 24p vs 48p max diff %.3g codes", t, MaxAbsDiff(o24, o48));
				Assert::IsTrue(MaxAbsDiff(o24, o48) < 1e-6, Wide(text).c_str());
			}
			// Unknown rate = 23.976 (reference): within a hair of 24.
			Model mu, m24b;
			for (int t = 0; t < 8; ++t)
			{
				const Image& in = t == 0 ? iFrame : bFrame;
				const Image a = mu.Frame(in, Frame(600 + t, 1.5, 0.0));
				const Image b = m24b.Frame(in, Frame(600 + t, 1.5, 24.0));
				Assert::IsTrue(MaxAbsDiff(a, b) < 0.05, L"unknown rate = the 23.976 reference");
			}
		}

		TEST_METHOD(TextureRestoreIsResolutionIndependent)
		{
			// A 2160-line-class picture and the same content at twice the size labelled with twice
			// the lines (4320, GrainScale 2): tiles, bands and gates scale with it, so the restored
			// share and the defocus gate agree.
			std::mt19937 rng(121u);
			const int w = 128, h = 96;
			const Image pic = Scene(w, h, rng);
			Image half = pic;   // the right half defocused
			const Image soft = Blur(pic, 3.5);
			for (int y = 0; y < h; ++y)
				for (int x = w / 2; x < w; ++x) half.at(x, y) = soft.at(x, y);
			// 2x: each picture pixel 2 x 2, then the same optics at twice the distance (0.6 px).
			Image big(2 * w, 2 * h);
			for (int y = 0; y < 2 * h; ++y)
				for (int x = 0; x < 2 * w; ++x) big.at(x, y) = half.at(x / 2, y / 2);
			big = Blur(big, 0.6);
			double share[2] = {}, defocusGain[2] = {};
			for (int k = 0; k < 2; ++k)
			{
				const Image& base = k == 0 ? half : big;
				const int bw = base.w, bh = base.h;
				Model model;
				for (int t = 0; t < 6; ++t)
				{
					TextureRestore::Params p = Frame(700 + t);
					p.planeLines = k == 0 ? 2160.0 : 4320.0;
					// The B-frames lose most of the fine band (the same loss at both sizes).
					const double s = k == 0 ? 1.0 : 2.0;
					Image in = base;
					if (t > 0)
					{
						const Image soft = Blur(base, 0.7 * s);
						for (size_t i = 0; i < in.v.size(); ++i) in.v[i] = base.v[i] - 0.6 * (base.v[i] - soft.v[i]);
					}
					const Image out = model.Frame(in, p);
					if (t == 0) continue;
					const BandEnergy ei = MeasureBands(in, bw / 16, bh / 8, bw / 2 - bw / 8, bh - bh / 8, k == 0 ? 1.0 : 2.0);
					const BandEnergy eo = MeasureBands(out, bw / 16, bh / 8, bw / 2 - bw / 8, bh - bh / 8, k == 0 ? 1.0 : 2.0);
					share[k] += (eo.e1 + eo.e2) / (ei.e1 + ei.e2) / 5.0;
					ForTilesIn(model, bw / 2 + bw / 8, 0, bw, bh, [&](const TextureRestoreModel::TileDebug& d) { defocusGain[k] = (std::max)(defocusGain[k], (std::max)(d.gT1, d.gT2) - 1.0); });
				}
				Assert::AreEqual(k == 0 ? 16 : 32, model.Tile(), L"the tile scales with GrainScale");
			}
			char text[200];
			snprintf(text, sizeof(text), "in-focus band energy x%.3f (2160) x%.3f (4320 at 2x); defocused texture gain-1 %.4f / %.4f", share[0], share[1], defocusGain[0], defocusGain[1]);
			Log(text);
			Assert::IsTrue(share[0] > 1.02 && share[1] > 1.02, Wide(text).c_str());
			Assert::IsTrue(std::fabs(share[0] - share[1]) < 0.5 * (share[0] - 1.0) + 0.02, Wide(text).c_str());
			Assert::IsTrue(defocusGain[0] <= 1e-4 && defocusGain[1] <= 1e-4, Wide(text).c_str());
		}

		TEST_METHOD(TextureRestoreRespectsCap)
		{
			std::mt19937 rng(131u);
			const Image base = Scene(kW, kH, rng);
			for (double cap : { 1.0, 1.2, 1.5, 2.0, 3.0 })
			{
				Model model;
				double maxGain = 1.0;
				for (int t = 0; t < 8; ++t)
				{
					const Image in = Dctq(Sum(base, Grain(kW, kH, 2.0 * kCode8, false, rng)), t == 0 ? 2.0 : 40.0);
					const Image out = model.Frame(in, Frame(800 + t, cap));
					if (cap == 1.0) Assert::IsTrue(out.v == in.v, L"cap 1.0: the input bit for bit");
					for (const TextureRestoreModel::TileDebug& d : model.Debug())
						maxGain = (std::max)(maxGain, (std::max)(d.gG, (std::max)(d.gT1, d.gT2)));
				}
				char text[120];
				snprintf(text, sizeof(text), "cap %.2f: largest gain %.4f", cap, maxGain);
				Log(text);
				Assert::IsTrue(maxGain <= TextureRestore::EffectiveCap(cap) + 1e-12, Wide(text).c_str());
				if (cap >= 1.5) Assert::IsTrue(maxGain > 1.2, Wide(text).c_str());
			}
		}

		// ---- Real clip (VP_TEST_REAL_FOOTAGE\breathing: x265 CRF 24 / 32 of a clean 1536x768 source) ----
		TEST_METHOD(TextureRestoreRealClipBreathing)
		{
			char* folderValue = nullptr;
			size_t folderLength = 0;
			std::string folder;
			if (_dupenv_s(&folderValue, &folderLength, "VP_TEST_REAL_FOOTAGE") == 0 && folderValue) folder = folderValue;
			std::free(folderValue);
			if (folder.empty())
			{
				Log("TextureRestoreRealClipBreathing: VP_TEST_REAL_FOOTAGE not set; skipped");
				return;
			}
			const std::string dir = folder + "\\breathing\\";
			const int W = 1536, H = 768, N = 48;
			auto readLuma = [&](const std::string& name, int i, Image& y) {
				std::ifstream f(dir + name, std::ios::binary);
				if (!f) return false;
				f.seekg(static_cast<std::streamoff>(i) * W * H * 3, std::ios::beg);   // 4:2:0, 2 bytes a sample: W*H*1.5*2
				std::vector<uint16_t> raw(static_cast<size_t>(W) * H);
				f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
				if (!f) return false;
				y = Image(W, H);
				for (size_t k = 0; k < raw.size(); ++k) y.v[k] = raw[k];
				return true;
			};
			auto readTypes = [&](const std::string& name) {
				std::ifstream f(dir + name);
				std::vector<char> t;
				std::string s;
				while (f >> s) if (!s.empty() && (s[0] == 'I' || s[0] == 'P' || s[0] == 'B')) t.push_back(s[0]);
				return t;
			};
			auto psnr = [](double mse) { return 10.0 * std::log10(1023.0 * 1023.0 / (std::max)(mse, 1e-12)); };
			Image probe;
			if (!readLuma("orig.yuv", 0, probe))
			{
				Log("TextureRestoreRealClipBreathing: breathing clips not found; skipped");
				return;
			}
			for (int run = 0; run < 4; ++run)
			{
				const int crf = run < 2 ? 24 : 32;
				const bool withField = run % 2 == 1;
				const std::vector<char> types = readTypes("types" + std::to_string(crf) + ".txt");
				Assert::IsTrue(types.size() >= static_cast<size_t>(N), L"frame types");
				Model model;
				Image prevDec;
				double trusted = 0.0;
				double meanChange = 0.0, worstChange = 0.0, iChange = 0.0, bandIn = 0.0, bandOut = 0.0, bandOrig = 0.0, defocused = 0.0;
				int nb = 0;
				for (int i = 0; i < N; ++i)
				{
					Image orig, dec;
					Assert::IsTrue(readLuma("orig.yuv", i, orig) && readLuma("dec" + std::to_string(crf) + ".yuv", i, dec));
					TextureRestore::Params p = Frame(1000 + i);
					p.sourceHz = 24.0;
					Field field;
					const bool useField = withField && i > 0;
					if (useField)
					{
						field = BlockField(prevDec, dec);
						int nt = 0;
						for (size_t k = 2; k < field.v.size(); k += 4) nt += field.v[k] > 0.5f ? 1 : 0;
						trusted += static_cast<double>(nt) / (field.w * field.h) / (N - 1);
					}
					const Image out = model.Frame(dec, p, useField ? &field : nullptr);
					prevDec = dec;
					const double before = psnr(Mse(dec, orig, 0, 0, W, H)), after = psnr(Mse(out, orig, 0, 0, W, H));
					const double change = after - before;
					meanChange += change / N;
					worstChange = (std::min)(worstChange, change);
					int gated = 0;
					for (const TextureRestoreModel::TileDebug& d : model.Debug())
						gated += TextureRestore::Smoothstep(TextureRestore::kFocusLo, TextureRestore::kFocusHi, d.F) < 0.5 ? 1 : 0;
					defocused += static_cast<double>(gated) / model.Debug().size() / N;
					if (types[i] == 'I') iChange = (std::max)(iChange, std::fabs(change));
					if (types[i] == 'B')
					{
						const BandEnergy eo = MeasureBands(orig, 16, 16, W - 16, H - 16), ei = MeasureBands(dec, 16, 16, W - 16, H - 16), eu = MeasureBands(out, 16, 16, W - 16, H - 16);
						bandOrig += eo.e1 + eo.e2; bandIn += ei.e1 + ei.e2; bandOut += eu.e1 + eu.e2;
						++nb;
					}
				}
				char text[400];
				snprintf(text, sizeof(text), "breathing CRF %d, %s: PSNR change mean %+.4f dB, worst %+.4f dB, I-frames |%.4f| dB; B-frame band (D1+D2) energy vs orig: in %.3f, out %.3f; tiles gated as defocused %.1f %%%s",
					crf, withField ? "block field" : "no vectors", meanChange, worstChange, iChange, bandIn / bandOrig, bandOut / bandOrig, 100.0 * defocused,
					withField ? (" (blocks trusted " + std::to_string(static_cast<int>(100.0 * trusted + 0.5)) + " %)").c_str() : "");
				Log(text);
				Assert::IsTrue(worstChange >= -0.35, Wide(text).c_str());
				Assert::IsTrue(meanChange >= -0.15, Wide(text).c_str());
				Assert::IsTrue(iChange <= 0.01, Wide(text).c_str());
				Assert::IsTrue(bandOut >= bandIn, Wide(text).c_str());
			}
		}
	};
}

// ---- GPU (shader against the model; timings). Never run by the agents: WARP or VP_TEST_GPU=hardware. ----
namespace TextureRestoreTestsNs
{
	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;
		~GpuDevice()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
		bool Create()
		{
			pl_log_params lp{};
			lp.log_level = PL_LOG_WARN;
			log = pl_log_create(PL_API_VER, &lp);
			std::string name;
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			Logger::WriteMessage(("texture restore GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	// 10-bit codes in a 16-bit plane (P010-like: bit_shift 6).
	pl_tex UploadLuma(pl_gpu gpu, const Image& y)
	{
		std::vector<uint16_t> data(y.v.size());
		for (size_t i = 0; i < data.size(); ++i)
			data[i] = static_cast<uint16_t>((std::min)(1023L, (std::max)(0L, std::lround(y.v[i]))) << 6);
		pl_tex_params p{};
		p.w = y.w; p.h = y.h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	pl_tex UploadField(pl_gpu gpu, const Field& f)
	{
		pl_tex_params p{};
		p.w = f.w; p.h = f.h; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = f.v.data();
		return pl_tex_create(gpu, &p);
	}

	Image Quantised(const Image& y)
	{
		Image q(y.w, y.h);
		for (size_t i = 0; i < q.v.size(); ++i) q.v[i] = static_cast<double>((std::min)(1023L, (std::max)(0L, std::lround(y.v[i]))));
		return q;
	}

	Image Download(pl_gpu gpu, pl_tex tex)
	{
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
		pl_tex_blit(gpu, &bp);
		std::vector<uint16_t> raw(static_cast<size_t>(rp.w) * rp.h * tex->params.format->num_components);
		pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
		const bool ok = pl_tex_download(gpu, &d);
		pl_tex_destroy(gpu, &readable);
		Image out(rp.w, rp.h);
		const int nc = tex->params.format->num_components;
		if (ok)
			for (size_t i = 0; i < out.v.size(); ++i) out.v[i] = raw[i * nc] / 64.0;
		return out;
	}

	pl_frame LumaFrame(pl_tex tex)
	{
		pl_frame f{};
		f.num_planes = 1;
		f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
		f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
		f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
		f.color = pl_color_space_bt709;
		return f;
	}

	TEST_CLASS(TextureRestoreGpuTests)
	{
	public:
		TEST_METHOD(TextureRestoreGpuMatchesModel)
		{
			// The shader follows the CPU model (same inputs, 10-bit codes in a 16-bit plane) within
			// half a code on 99.9 % of the pixels and 2 codes everywhere, over a coded GOP with
			// grain, with and without a field (static left half, right half panning 3 px a frame).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(151u);
			const Image big = Scene(kW + 64, kH, rng);
			const int fw = (kW + 15) / 16, fh = (kH + 15) / 16;
			Field moving(fw, fh, 0.0f, 0.0f, 1.0f);
			for (int y = 0; y < fh; ++y)
				for (int x = fw / 2; x < fw; ++x) moving.at(x, y)[0] = 3.0f;
			pl_tex fieldTex = UploadField(gpu, moving);
			Assert::IsNotNull(fieldTex);
			for (int withField = 0; withField < 2; ++withField)
			{
				TextureRestore::Module module;
				Model model;
				for (int t = 0; t < 10; ++t)
				{
					Image f(kW, kH);
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x)
							f.at(x, y) = x < kW / 2 ? big.at(x, y) : big.at((std::min)(kW + 63, x + 60 - 3 * t), y);
					const Image in = Quantised(Dctq(Sum(f, Grain(kW, kH, 1.5 * kCode8, true, rng)), IsI(t, 8) ? 4.0 : 24.0));
					pl_tex tex = UploadLuma(gpu, in);
					TextureRestore::Params p = Frame(900 + t);
					if (withField) { p.field = fieldTex; p.fieldScale = 1.0; }
					pl_frame out{};
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), Wide(module.LastError()).c_str());
					const Image gpuOut = Download(gpu, out.planes[0].texture);
					const Image cpuOut = model.Frame(in, p, withField ? &moving : nullptr);
					int over = 0;
					double worst = 0.0;
					for (size_t i = 0; i < gpuOut.v.size(); ++i)
					{
						const double d = std::fabs(gpuOut.v[i] - cpuOut.v[i]);
						worst = (std::max)(worst, d);
						over += d > 0.5 ? 1 : 0;
					}
					char text[160];
					snprintf(text, sizeof(text), "%s frame %d: worst %.3f codes, %d px over 0.5", withField ? "field" : "no field", t, worst, over);
					Logger::WriteMessage(text);
					Assert::IsTrue(worst <= 2.0, Wide(text).c_str());
					Assert::IsTrue(over <= static_cast<int>(gpuOut.v.size() / 1000), Wide(text).c_str());
					pl_tex_destroy(gpu, &tex);
				}
				// cap 1.0: the source frame itself.
				{
					pl_tex tex = UploadLuma(gpu, Image(kW, kH, 300.0));
					TextureRestore::Params p = Frame(990, 1.0);
					pl_frame out{};
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out));
					Assert::IsTrue(out.planes[0].texture == tex, L"cap 1.0 returns the input");
					pl_tex_destroy(gpu, &tex);
				}
				module.Release(gpu);
			}
			pl_tex_destroy(gpu, &fieldTex);
		}

		TEST_METHOD(TextureRestoreGpuTiming)
		{
			// Hardware only (VP_TEST_GPU=hardware): Apply at 3840x2160 and 1920x1080, median wall
			// time with pl_gpu_finish per frame (targets: 4K <= 2.5 ms on an RTX 3050 Ti laptop,
			// about 0.6 ms on an RTX 3090).
			if (!ExtendedRangeGpuProbe::OnHardwareGpu())
			{
				Logger::WriteMessage("TextureRestoreGpuTiming: VP_TEST_GPU=hardware only; not run");
				return;
			}
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int sizes[2][2] = { { 3840, 2160 }, { 1920, 1080 } };
			for (const auto& s : sizes)
			{
				std::mt19937 rng(161u);
				Image y(s[0], s[1]);
				std::normal_distribution<double> n(0.0, 8.0);
				for (int yy = 0; yy < s[1]; ++yy)
					for (int x = 0; x < s[0]; ++x) y.at(x, yy) = 400.0 + 100.0 * std::sin(x * 0.05) * std::cos(yy * 0.03) + n(rng);
				pl_tex tex = UploadLuma(gpu, y);
				Assert::IsNotNull(tex);
				TextureRestore::Module module;
				std::vector<double> ms;
				for (int k = 0; k < 40; ++k)
				{
					TextureRestore::Params p = Frame(2000 + k);
					pl_frame out{};
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), Wide(module.LastError()).c_str());
					pl_gpu_finish(gpu);
					const auto t1 = std::chrono::steady_clock::now();
					if (k >= 8) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				char text[160];
				snprintf(text, sizeof(text), "texture restore %dx%d: median %.3f ms (min %.3f, max %.3f) over %zu frames", s[0], s[1], ms[ms.size() / 2], ms.front(), ms.back(), ms.size());
				Logger::WriteMessage(text);
				module.Release(gpu);
				pl_tex_destroy(gpu, &tex);
			}
		}
	};
}
