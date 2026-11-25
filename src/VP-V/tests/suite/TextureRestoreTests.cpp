#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/TextureRestoreModel.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <VP-V/modules/repair/texture_restore/TextureRestore.h>
#include <VP-V/core/Ramp.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraRecordPool.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>
#include <VP-V/modules/repair/grain_split/BandMemoFill.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <VP-V/modules/control/skeleton/PlTexBackend.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// texture_restore (docs/VP-detail-restore.md; algorithm and constants:
// src/VP-V/modules/repair/texture_restore/TextureRestore.h). CPU tests run the reference model
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
			// Hunt 3 F7: a repeat with ANOTHER picture (A moved 6 px: a phase picture, an extra)
			// leaves the references and the previous low band as they were: the next frame is the
			// same as without the repeat.
			{
				Model once, twice;
				const Image i0 = coded(a, true), b1 = coded(a, false);
				std::mt19937 g(9u);
				const Image b2 = Dctq(Sum(a, Grain(kW, kH, 2.0, false, g)), 24.0);
				Image moved(kW, kH);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x) moved.at(x, y) = b1.get(x - 6, y);
				once.Frame(i0, Frame(50));
				twice.Frame(i0, Frame(50));
				once.Frame(b1, Frame(51));
				twice.Frame(b1, Frame(51));
				twice.Frame(moved, Frame(51));
				Assert::IsTrue(twice.LastWasRepeat());
				Assert::AreEqual(0.0, MaxAbsDiff(once.Frame(b2, Frame(52)), twice.Frame(b2, Frame(52))),
					L"a repeat of another picture changes nothing for the next frame");
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
		BEGIN_TEST_METHOD_ATTRIBUTE(TextureRestoreRealClipBreathing)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TextureRestoreRealClipBreathing)
		{
			char* folderValue = nullptr;
			size_t folderLength = 0;
			std::string folder;
			if (_dupenv_s(&folderValue, &folderLength, "VP_TEST_REAL_FOOTAGE") == 0 && folderValue) folder = folderValue;
			std::free(folderValue);
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage,
				"TextureRestoreRealClipBreathing", !folder.empty(), "set VP_TEST_REAL_FOOTAGE"))
				return;
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
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage,
				"TextureRestoreRealClipBreathing", readLuma("orig.yuv", 0, probe),
				"breathing clips not found under " + dir))
				return;
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
						gated += RampMath::Smoothstep<double>(TextureRestore::kFocusLo, TextureRestore::kFocusHi, d.F, RampMath::SmoothRule::Unguarded) < 0.5 ? 1 : 0;
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
	// The scene readers of the shared cut / fade judge (registered fix D8); the default reader is replaced for the lifetime of a GpuDevice
	// (no skeleton store in the unit tests): no frame is a cut or a fade.
	bool NoCutReader(uint64_t, FrameAnalysis::Scene& scene) { scene = FrameAnalysis::Scene(); return true; }
	bool MissingReader(uint64_t, FrameAnalysis::Scene&) { return false; }
	uint64_t g_cutAt = 0;
	bool CutAtReader(uint64_t sequence, FrameAnalysis::Scene& scene)
	{
		scene = FrameAnalysis::Scene();
		scene.cut = sequence == g_cutAt;
		return true;
	}

	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;
		~GpuDevice()
		{
			VpDetailRestore::DefaultSceneReader() = &VpDetailRestore::ReadSceneFromSkeleton;   // the production default (C5)
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
			VpDetailRestore::DefaultSceneReader() = &NoCutReader;
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
					p.sceneReader = &NoCutReader;   // D8: module and model follow the same shared verdict (no cut)
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

		TEST_METHOD(TextureRestoreGpuSceneVerdictRestartsAndMissingIsCounted)
		{
			// Registered fix D8: the references restart on the shared judge's verdict (cut) and on a missing scene record (counted), never on a
			// private judgement; with no verdict the restoration acts on the coarse frame that follows a fine one. The model follows the
			// reader the same way (Params::sceneReader set) and applies the retired tile law as the test reference otherwise.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(77u);
			const Image scene = Scene(kW, kH, rng);
			const Image fine = Quantised(Dctq(scene, 4.0)), coarse = Quantised(Dctq(scene, 24.0));
			pl_tex fineTex = UploadLuma(gpu, fine), coarseTex = UploadLuma(gpu, coarse);
			auto run = [&](VpDetailRestore::SceneReaderFn reader, double& change, unsigned long long& missing) {
				TextureRestore::Module module;
				TextureRestore::Params p1 = Frame(1), p2 = Frame(2);
				p1.sceneReader = p2.sceneReader = reader;
				pl_frame o1{}, o2{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(fineTex), p1, o1), Wide(module.LastError()).c_str());
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(coarseTex), p2, o2), Wide(module.LastError()).c_str());
				change = MaxAbsDiff(Download(gpu, o2.planes[0].texture), coarse);
				missing = module.SceneMissing();
				module.Release(gpu);
			};
			double changeNone = 0.0, changeCut = 0.0, changeMissing = 0.0;
			unsigned long long missNone = 0, missCut = 0, missMissing = 0;
			run(&NoCutReader, changeNone, missNone);
			g_cutAt = 2;
			run(&CutAtReader, changeCut, missCut);
			run(&MissingReader, changeMissing, missMissing);
			Assert::IsTrue(changeNone > 0.01, L"no verdict: the restoration acts on the coarse frame");
			Assert::IsTrue(changeCut < 0.01, L"the shared judge's cut restarts the references: output is the input");
			Assert::IsTrue(changeMissing < 0.01, L"no scene record: restart, output is the input");
			Assert::AreEqual(0ull, missNone);
			Assert::AreEqual(0ull, missCut);
			Assert::AreEqual(1ull, missMissing, L"the missing record is counted");
			pl_tex_destroy(gpu, &fineTex);
			pl_tex_destroy(gpu, &coarseTex);
		}

		// GF-P5 S5: a stage that starts over (Forget) has no scene verdict yet: the "no scene record" state of the last frame and
		// its history keep are cleared, not left on the status line.
		TEST_METHOD(TextureRestoreForgetClearsTheSceneJudgeState)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(77u);
			const Image scene = Scene(kW, kH, rng);
			pl_tex tex = UploadLuma(gpu, Quantised(Dctq(scene, 4.0)));
			TextureRestore::Module module;
			for (uint64_t sequence = 1; sequence <= 2; ++sequence)
			{
				TextureRestore::Params p = Frame(sequence);
				p.sceneReader = &MissingReader;   // no scene record
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), Wide(module.LastError()).c_str());
			}
			Assert::IsTrue(module.SceneJudgeUnavailable(), L"precondition: the last frame had no scene record");
			module.Forget();
			Assert::IsFalse(module.SceneJudgeUnavailable(), L"Forget clears the scene-judge fallback");
			Assert::AreEqual(1.0f, module.LastHistoryKeep(), 1e-6f, L"and the history keep");
			module.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		// GF-P5 S5: a self-restart frame (a sequence gap: restartAll) asks no scene verdict, so the previous frame's "no scene record"
		// state is not carried over: not unavailable, keep 0.
		TEST_METHOD(TextureRestoreSelfRestartFrameClearsTheSceneJudgeState)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(77u);
			const Image scene = Scene(kW, kH, rng);
			pl_tex tex = UploadLuma(gpu, Quantised(Dctq(scene, 4.0)));
			TextureRestore::Module module;
			const uint64_t sequences[3] = { 1, 2, 9 };   // the last one is not the next frame: it restarts on its own
			for (int k = 0; k < 3; ++k)
			{
				TextureRestore::Params p = Frame(sequences[k]);
				p.sceneReader = &MissingReader;   // no scene record
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), Wide(module.LastError()).c_str());
				if (k == 1)
					Assert::IsTrue(module.SceneJudgeUnavailable(), L"precondition: a consecutive frame without a record is unavailable");
			}
			Assert::IsFalse(module.SceneJudgeUnavailable(), L"the self-restart frame is not unavailable");
			Assert::AreEqual(0.0f, module.LastHistoryKeep(), 1e-6f, L"its history keep is 0");
			module.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(TextureRestoreGpuRepeatOfAnotherPictureKeepsTheReferences)
		{
			// Hunt 3 F7: a repeat whose picture is another one (a Stage B phase picture, an extra:
			// A moved 4 px) writes its own low bands; the next new frame still compares against A's
			// (its low-band change veto and cut test). The repeat and every later frame follow the
			// model (half a code on 99.9 % of the pixels, 2 codes everywhere).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(171u);
			const Image a = Scene(kW + 8, kH, rng);
			TextureRestore::Module module;
			Model model;
			auto check = [&](const Image& in, const TextureRestore::Params& p, const char* what) {
				pl_tex tex = UploadLuma(gpu, in);
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), Wide(module.LastError()).c_str());
				const Image gpuOut = Download(gpu, out.planes[0].texture);
				const Image cpuOut = model.Frame(in, p);
				int over = 0;
				double worst = 0.0;
				for (size_t i = 0; i < gpuOut.v.size(); ++i)
				{
					const double d = std::fabs(gpuOut.v[i] - cpuOut.v[i]);
					worst = (std::max)(worst, d);
					over += d > 0.5 ? 1 : 0;
				}
				char text[160];
				snprintf(text, sizeof(text), "%s: worst %.3f codes, %d px over 0.5", what, worst, over);
				Logger::WriteMessage(text);
				Assert::IsTrue(worst <= 2.0 && over <= static_cast<int>(gpuOut.v.size() / 1000), Wide(text).c_str());
				pl_tex_destroy(gpu, &tex);
			};
			for (int t = 0; t < 7; ++t)
			{
				Image f(kW, kH);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x) f.at(x, y) = a.at(x + 4, y);
				const Image in = Quantised(Dctq(Sum(f, Grain(kW, kH, 1.5 * kCode8, true, rng)), IsI(t, 8) ? 4.0 : 24.0));
				TextureRestore::Params p = Frame(1100 + t);
				p.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module
				char what[64];
				snprintf(what, sizeof(what), "frame %d", t);
				check(in, p, what);
				if (t == 3)
				{
					Image moved(kW, kH);
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x) moved.at(x, y) = in.get(x - 4, y);
					check(moved, p, "repeat of frame 3, moved 4 px");
				}
			}
			module.Release(gpu);
		}

		TEST_METHOD(TextureRestoreGpuForeignGeometryKeepsTheReferences)
		{
			// Hunt 3 (crossfade lead): the path being left (a half-size plane) takes the frame's
			// gains with its own bands; the main path's references are untouched.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(181u);
			const Image a = Scene(kW, kH, rng);
			TextureRestore::Module module;
			Model model;
			double worstMain = 0.0, worstForeign = 0.0;
			for (int t = 0; t < 6; ++t)
			{
				const Image in = Quantised(Dctq(Sum(a, Grain(kW, kH, 1.5 * kCode8, true, rng)), IsI(t, 8) ? 4.0 : 24.0));
				TextureRestore::Params p = Frame(1200 + t);
				p.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module
				pl_tex tex = UploadLuma(gpu, in);
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), Wide(module.LastError()).c_str());
				const Image cpuOut = model.Frame(in, p);
				Image halfPic(kW / 2, kH / 2);
				for (int y = 0; y < halfPic.h; ++y)
					for (int x = 0; x < halfPic.w; ++x)
						halfPic.at(x, y) = std::round(0.25 * (in.at(2 * x, 2 * y) + in.at(2 * x + 1, 2 * y) + in.at(2 * x, 2 * y + 1) + in.at(2 * x + 1, 2 * y + 1)));
				TextureRestore::Params pf = p;
				pf.planeLines = kH / 2;
				pl_tex stex = UploadLuma(gpu, halfPic);
				pl_frame foreign{};
				Assert::IsTrue(module.ApplyForeign(gpu, dev.dispatch, LumaFrame(stex), pf, foreign), Wide(module.LastError()).c_str());
				Assert::IsTrue(foreign.planes[0].texture != out.planes[0].texture, L"its own output texture");
				const Image fo = Download(gpu, foreign.planes[0].texture), fm = model.ForeignFrame(halfPic, pf);
				for (size_t i = 0; i < fo.v.size(); ++i) worstForeign = (std::max)(worstForeign, std::fabs(fo.v[i] - fm.v[i]));
				const Image mo = Download(gpu, out.planes[0].texture);
				for (size_t i = 0; i < mo.v.size(); ++i) worstMain = (std::max)(worstMain, std::fabs(mo.v[i] - cpuOut.v[i]));
				pl_tex_destroy(gpu, &tex);
				pl_tex_destroy(gpu, &stex);
			}
			char text[160];
			snprintf(text, sizeof(text), "foreign geometry: worst %.3f codes (foreign), %.3f (main)", worstForeign, worstMain);
			Logger::WriteMessage(text);
			Assert::IsTrue(worstForeign <= 2.0 && worstMain <= 2.0, Wide(text).c_str());
			module.Release(gpu);
		}

		TEST_METHOD(TextureRestoreGpuArenaIsByteIdentical)
		{
			// SA1: the per-frame temporaries from the scratch arena change nothing: the same inputs through
			// a module without an arena and a module with one give exactly equal planes (main and foreign
			// geometry, with and without a field, a repeated frame included).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(191u);
			const Image big = Scene(kW + 64, kH, rng);
			const int fw = (kW + 15) / 16, fh = (kH + 15) / 16;
			Field moving(fw, fh, 0.0f, 0.0f, 1.0f);
			for (int y = 0; y < fh; ++y)
				for (int x = fw / 2; x < fw; ++x) moving.at(x, y)[0] = 3.0f;
			pl_tex fieldTex = UploadField(gpu, moving);
			Assert::IsNotNull(fieldTex);
			for (int withField = 0; withField < 2; ++withField)
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				const auto requests = TextureRestore::ArenaRequests(gpu, kW, kH, kW / 2, kH / 2);
				Assert::IsFalse(requests.empty(), L"the float formats map to arena formats");
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T2)), L"arena reserve");
				TextureRestore::Module a, b;
				int frames = 0;
				for (int t = 0; t < 8; ++t)
				{
					const int src = t == 4 ? 3 : t;   // frame 4 repeats frame 3 (same sequence, same picture)
					Image f(kW, kH);
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x)
							f.at(x, y) = x < kW / 2 ? big.at(x, y) : big.at((std::min)(kW + 63, x + 60 - 3 * src), y);
					std::mt19937 grainRng(5000u + static_cast<unsigned>(src));
					const Image in = Quantised(Dctq(Sum(f, Grain(kW, kH, 1.5 * kCode8, true, grainRng)), IsI(src, 8) ? 4.0 : 24.0));
					TextureRestore::Params pa = Frame(1300 + src);
					if (withField) { pa.field = fieldTex; pa.fieldScale = 1.0; }
					TextureRestore::Params pb = pa;
					pb.arena = &arena;
					pl_tex tex = UploadLuma(gpu, in);
					pl_frame outA{}, outB{};
					arena.BeginFrame();
					Assert::IsTrue(a.Apply(gpu, dev.dispatch, LumaFrame(tex), pa, outA), Wide(a.LastError()).c_str());
					Assert::IsTrue(b.Apply(gpu, dev.dispatch, LumaFrame(tex), pb, outB), Wide(b.LastError()).c_str());
					Assert::IsFalse(b.ScratchShort(), L"no shortage");
					const Image ia = Download(gpu, outA.planes[0].texture), ib = Download(gpu, outB.planes[0].texture);
					Assert::IsTrue(ia.v == ib.v, (L"arena output equals private output, main, frame " + std::to_wstring(t)).c_str());
					// The foreign geometry on the half-size picture.
					Image halfPic(kW / 2, kH / 2);
					for (int y = 0; y < halfPic.h; ++y)
						for (int x = 0; x < halfPic.w; ++x)
							halfPic.at(x, y) = std::round(0.25 * (in.at(2 * x, 2 * y) + in.at(2 * x + 1, 2 * y) + in.at(2 * x, 2 * y + 1) + in.at(2 * x + 1, 2 * y + 1)));
					TextureRestore::Params fa = pa, fb = pb;
					fa.planeLines = kH / 2;
					fb.planeLines = kH / 2;
					pl_tex stex = UploadLuma(gpu, halfPic);
					pl_frame foreignA{}, foreignB{};
					Assert::IsTrue(a.ApplyForeign(gpu, dev.dispatch, LumaFrame(stex), fa, foreignA), Wide(a.LastError()).c_str());
					Assert::IsTrue(b.ApplyForeign(gpu, dev.dispatch, LumaFrame(stex), fb, foreignB), Wide(b.LastError()).c_str());
					Assert::IsFalse(b.ScratchShort(), L"no foreign shortage");
					const Image fiA = Download(gpu, foreignA.planes[0].texture), fiB = Download(gpu, foreignB.planes[0].texture);
					Assert::IsTrue(fiA.v == fiB.v, (L"arena output equals private output, foreign, frame " + std::to_wstring(t)).c_str());
					pl_tex_destroy(gpu, &tex);
					pl_tex_destroy(gpu, &stex);
					++frames;
				}
				char text[160];
				snprintf(text, sizeof(text), "%s: %d frames identical, arena acquired %llu, exhausted %llu, shed %llu", withField ? "field" : "no field", frames,
					static_cast<unsigned long long>(arena.Acquired()), static_cast<unsigned long long>(arena.Exhausted()), static_cast<unsigned long long>(arena.Shed()));
				Logger::WriteMessage(text);
				Assert::AreEqual(uint64_t(0), arena.Exhausted(), L"the arena never ran short");
				Assert::IsFalse(arena.FrameDecision().engaged, L"scratch_arena released");
				Assert::IsTrue(arena.Acquired() > 0, L"the arena was really used");
				Assert::IsTrue(b.Failures() == 0 && a.Failures() == 0, L"no failures");
				a.Release(gpu);
				b.Release(gpu);
				arena.Release();
			}
			pl_tex_destroy(gpu, &fieldTex);
		}

		TEST_METHOD(TextureRestoreGpuArenaShortageIsVisibleAndRecovers)
		{
			// SA1: no scratch texture (empty plan, or the tier shed) = texture restore visibly off for that
			// frame (Apply false, ScratchShort, scratch_arena engaged, the failure counted); cap 1.0 needs
			// no arena; the next frame with a proper plan restarts by itself and equals a private module.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::mt19937 rng(201u);
			const Image a = Scene(kW, kH, rng);
			const Image in = Quantised(Dctq(Sum(a, Grain(kW, kH, 1.5 * kCode8, true, rng)), 24.0));
			pl_tex tex = UploadLuma(gpu, in);
			Assert::IsNotNull(tex);
			const auto requests = TextureRestore::ArenaRequests(gpu, kW, kH);
			Assert::IsFalse(requests.empty(), L"the float formats map to arena formats");
			for (int tierCase = 0; tierCase < 2; ++tierCase)
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				// 0: an empty plan; 1: a plan whose tier (T0) is below TextureRestore's (T1): shed.
				if (tierCase == 0)
					arena.Reserve(VpSkeleton::PlanArena(std::vector<VpSkeleton::ArenaRequest>(), VpSkeleton::Tier::T2));
				else
					arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T0));
				TextureRestore::Module module;
				TextureRestore::Params p = Frame(1400);
				p.arena = &arena;
				pl_frame out{};
				arena.BeginFrame();
				const unsigned long long before = module.Failures();
				Assert::IsFalse(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out), L"no scratch texture: Apply fails");
				Assert::IsTrue(module.ScratchShort(), L"ScratchShort");
				Assert::IsTrue(module.LastError().rfind("scratch memory short", 0) == 0, Wide(module.LastError()).c_str());
				Assert::AreEqual(before + 1, module.Failures(), L"one failure counted");
				const FallbackCatalog::Decision d = arena.FrameDecision();
				Assert::IsTrue(d.engaged && d.id == "scratch_arena", L"scratch_arena engaged");
				Assert::IsTrue(d.reason.find("texture_restore") != std::string::npos, Wide(d.reason).c_str());
				if (tierCase == 1)
					Assert::IsTrue(arena.Shed() > 0, L"shed by tier");
				else
					Assert::IsTrue(arena.Shed() > 0 || arena.Exhausted() > 0, L"counted");
				// cap 1.0 needs no arena: the input itself.
				{
					TextureRestore::Params p1 = Frame(1401, 1.0);
					p1.arena = &arena;
					pl_frame same{};
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p1, same));
					Assert::IsTrue(same.planes[0].texture == tex, L"cap 1.0 returns the input");
					Assert::IsFalse(module.ScratchShort(), L"ScratchShort reset by the next call");
				}
				// Recovery: a proper plan, a new frame, the failure made the next Apply restart.
				arena.BeginFrame();
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T2)), L"arena reserve");
				Assert::IsFalse(arena.FrameDecision().engaged, L"released after BeginFrame");
				TextureRestore::Params p2 = Frame(1402);
				p2.arena = &arena;
				pl_frame out2{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), p2, out2), Wide(module.LastError()).c_str());
				Assert::IsFalse(module.ScratchShort(), L"recovered");
				Assert::IsFalse(arena.FrameDecision().engaged, L"no shortage after recovery");
				const Image got = Download(gpu, out2.planes[0].texture);
				TextureRestore::Module fresh;
				TextureRestore::Params pf = Frame(1402);
				pl_frame outF{};
				Assert::IsTrue(fresh.Apply(gpu, dev.dispatch, LumaFrame(tex), pf, outF), Wide(fresh.LastError()).c_str());
				const Image want = Download(gpu, outF.planes[0].texture);
				Assert::IsTrue(got.v == want.v, L"the recovered frame equals a private-texture module's");
				Logger::WriteMessage(tierCase == 0 ? "empty plan: visible, recovered" : "tier shed: visible, recovered");
				module.Release(gpu);
				fresh.Release(gpu);
				arena.Release();
			}
			pl_tex_destroy(gpu, &tex);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(TextureRestoreGpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TextureRestoreGpuTiming)
		{
			// Hardware only (VP_TEST_GPU=hardware): Apply at 3840x2160 and 1920x1080, median wall
			// time with pl_gpu_finish per frame (targets: 4K <= 2.5 ms on an RTX 3050 Ti laptop,
			// about 0.6 ms on an RTX 3090).
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"TextureRestoreGpuTiming", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;
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
				const double medianMs = ms[ms.size() / 2];
				Assert::IsTrue(std::isfinite(medianMs) && medianMs > 0.0, L"TextureRestore median GPU time must be finite and positive");
				char text[160];
				snprintf(text, sizeof(text), "texture restore %dx%d: median %.3f ms (min %.3f, max %.3f) over %zu frames", s[0], s[1], medianMs, ms.front(), ms.back(), ms.size());
				Logger::WriteMessage(text);
				module.Release(gpu);
				pl_tex_destroy(gpu, &tex);
			}
		}

		// W23-B3 M2#9 (15c C3b): TextureRestore acquires shared P11 band energies (re_s2/re_s4) through SkeletonMemo,
		// increments P11EnergyHits with zero fallbacks; absent/unregistered memo increments P11EnergyFallbacks with a
		// visible reason; poisoning P11 energies shifts TextureRestore output on the exact same input (proving live consumption).
		BEGIN_TEST_METHOD_ATTRIBUTE(TextureRestoreAcquiresP11BandEnergiesWithVisibleFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TextureRestoreAcquiresP11BandEnergiesWithVisibleFallback)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;
			struct TexGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
				~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
			};
			struct ModGuard
			{
				pl_gpu gpu = nullptr;
				TextureRestore::Module mod;
				explicit ModGuard(pl_gpu g) : gpu(g) {}
				~ModGuard() { mod.Release(gpu); }
				ModGuard(const ModGuard&) = delete;
				ModGuard& operator=(const ModGuard&) = delete;
			};
			struct MemoGpuGuard
			{
				Vpv::Skeleton::OwnerGpuDevice ownerDev;
				VpSkeleton::SkeletonMemo memo;
				explicit MemoGpuGuard(pl_gpu g) : ownerDev(g) {}
				~MemoGpuGuard() { memo.ReleaseGpuSpots(); ownerDev.Release(); }
				MemoGpuGuard(const MemoGpuGuard&) = delete;
				MemoGpuGuard& operator=(const MemoGpuGuard&) = delete;
			};

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();

			std::mt19937 rng(409u);
			const Image scene = Scene(kW, kH, rng);
			const Image in0 = Quantised(Dctq(Sum(scene, Grain(kW, kH, 1.5 * kCode8, false, rng)), 4.0));
			const Image in1 = Quantised(Dctq(Sum(scene, Grain(kW, kH, 1.5 * kCode8, false, rng)), 28.0));
			TexGuard tex0(gpu, UploadLuma(gpu, in0));
			TexGuard tex1(gpu, UploadLuma(gpu, in1));
			Assert::IsTrue(tex0.tex != nullptr && tex1.tex != nullptr);

			const int p11B = RegionEvidence::BlockSizeFor(kH);
			const RegionEvidence::Grid g = RegionEvidence::MakeGrid(kW, kH, p11B);
			const size_t nb = g.Count();

			auto makeP11Tex = [&](const Image& img, std::vector<float>& s2Buf, std::vector<float>& s4Buf) {
				GrainSplit::Plane pl(kW, kH);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x)
						pl.at(x, y) = img.at(x, y);
				const GrainSplit::Split sp = GrainSplit::Compute(pl, TextureRestore::Scale(kH), p11B, GrainSplit::Params());
				s2Buf.assign(nb * 4u, 0.0f);
				s4Buf.assign(nb * 4u, 0.0f);
				for (size_t i = 0; i < nb; ++i)
				{
					s2Buf[i * 4 + 0] = static_cast<float>(sp.e1[i]);
					s2Buf[i * 4 + 1] = static_cast<float>(sp.e2[i]);
					s2Buf[i * 4 + 2] = static_cast<float>(sp.e3[i]);
					s4Buf[i * 4 + 0] = 1.0f;
					s4Buf[i * 4 + 2] = 1.0f;
					s4Buf[i * 4 + 3] = static_cast<float>(sp.N[i]);
				}
			};
			auto uploadRgba = [&](const std::vector<float>& data) {
				pl_tex_params tp{};
				tp.w = g.gw; tp.h = g.gh; tp.format = pl_find_named_fmt(gpu, "rgba32f"); tp.sampleable = true; tp.initial_data = data.data();
				return pl_tex_create(gpu, &tp);
			};

			std::vector<float> s20, s40, s21, s41;
			makeP11Tex(in0, s20, s40);
			makeP11Tex(in1, s21, s41);
			TexGuard s2Tex0(gpu, uploadRgba(s20)), s4Tex0(gpu, uploadRgba(s40));
			TexGuard s2Tex1(gpu, uploadRgba(s21)), s4Tex1(gpu, uploadRgba(s41));
			// Negative-control poisoned P11: reversed energy drop (frame 1 tiny, frame 2 huge, N=0) suppresses restoration gain.
			std::vector<float> pS20(nb * 4u, 0.0f), pS21(nb * 4u, 0.0f), pS4(nb * 4u, 0.0f);
			for (size_t i = 0; i < nb; ++i)
			{
				pS20[i * 4 + 0] = 0.01f; pS20[i * 4 + 1] = 0.01f; pS20[i * 4 + 2] = 0.01f;
				pS21[i * 4 + 0] = 2000.0f; pS21[i * 4 + 1] = 2000.0f; pS21[i * 4 + 2] = 500.0f;
				pS4[i * 4 + 0] = 1.0f; pS4[i * 4 + 2] = 1.0f; pS4[i * 4 + 3] = 0.0f;
			}
			TexGuard ps2Tex0(gpu, uploadRgba(pS20)), ps2Tex1(gpu, uploadRgba(pS21)), ps4Tex(gpu, uploadRgba(pS4));
			Assert::IsTrue(s2Tex0.tex && s4Tex0.tex && s2Tex1.tex && s4Tex1.tex && ps2Tex0.tex && ps2Tex1.tex && ps4Tex.tex);

			int hostTag = 0;
			auto initMemo = [&](MemoGpuGuard& mg, pl_tex a2, pl_tex a4, pl_tex b2, pl_tex b4) {
				mg.memo.SetHost(&hostTag);
				mg.memo.SetRenderThread(std::this_thread::get_id());
				mg.memo.SetGpuDispatcherPresent(true);
				mg.memo.SetGpuBudget(true, uint64_t(1) << 28);
				mg.memo.SetGpuDevice(&mg.ownerDev);
				VpSkeleton::OwnerFill fill;
				fill.view = true;
				fill.frameScoped = true;
				fill.fill = [a2, a4, b2, b4, g](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.seq != 501u && req.seq != 502u) return false;
					pl_tex s2 = (req.seq == 501u) ? a2 : b2;
					pl_tex s4 = (req.seq == 501u) ? a4 : b4;
					out.gpu.resource = const_cast<void*>(static_cast<const void*>(s2));
					out.gpu.view = const_cast<void*>(static_cast<const void*>(s4));
					out.gpu.width = uint32_t(g.gw);
					out.gpu.height = uint32_t(g.gh);
					out.gpu.bytes = 0;
					return true;
				};
				Assert::IsTrue(mg.memo.RegisterOwnerFill(VpSkeleton::kP11RegionBlockStats, VpSkeleton::AcquireForm::Gpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP11RegionBlockStats), std::move(fill)) == Reg::Registered);
			};

			MemoGpuGuard p11Memo(gpu), poisonMemo(gpu);
			initMemo(p11Memo, s2Tex0.tex, s4Tex0.tex, s2Tex1.tex, s4Tex1.tex);
			initMemo(poisonMemo, ps2Tex0.tex, ps4Tex.tex, ps2Tex1.tex, ps4Tex.tex);
			VpSkeleton::SkeletonMemo unregMemo;

			ModGuard modP11(gpu), modNull(gpu), modUnreg(gpu), modPoison(gpu);
			pl_tex inputs[2] = { tex0.tex, tex1.tex };
			pl_frame outP11{}, outNull{}, outUnreg{}, outPoison{};
			for (int k = 0; k < 2; ++k)
			{
				TextureRestore::Params p = Frame(501u + static_cast<uint64_t>(k));
				p.sceneReader = &NoCutReader;
				p.memo = &p11Memo.memo;
				Assert::IsTrue(modP11.mod.Apply(gpu, dev.dispatch, LumaFrame(inputs[k]), p, outP11), Wide(modP11.mod.LastError()).c_str());

				TextureRestore::Params pn = p; pn.memo = nullptr;
				Assert::IsTrue(modNull.mod.Apply(gpu, dev.dispatch, LumaFrame(inputs[k]), pn, outNull), Wide(modNull.mod.LastError()).c_str());

				TextureRestore::Params pu = p; pu.memo = &unregMemo;
				Assert::IsTrue(modUnreg.mod.Apply(gpu, dev.dispatch, LumaFrame(inputs[k]), pu, outUnreg), Wide(modUnreg.mod.LastError()).c_str());

				TextureRestore::Params pp = p; pp.memo = &poisonMemo.memo;
				Assert::IsTrue(modPoison.mod.Apply(gpu, dev.dispatch, LumaFrame(inputs[k]), pp, outPoison), Wide(modPoison.mod.LastError()).c_str());
			}

			Assert::AreEqual(2ull, modP11.mod.P11EnergyHits(), L"TR hit P11 on both frames");
			Assert::AreEqual(0ull, modP11.mod.P11EnergyFallbacks(), L"TR had zero P11 fallbacks when memo provided P11");
			Assert::IsTrue(modP11.mod.LastP11EnergyFallback().empty(), L"no fallback reason on P11 hit");

			Assert::AreEqual(0ull, modNull.mod.P11EnergyHits(), L"memo=nullptr has zero P11 hits");
			Assert::AreEqual(2ull, modNull.mod.P11EnergyFallbacks(), L"memo=nullptr increments P11EnergyFallbacks");
			Assert::IsFalse(modNull.mod.LastP11EnergyFallback().empty(), L"memo=nullptr records visible fallback reason");

			Assert::AreEqual(0ull, modUnreg.mod.P11EnergyHits(), L"unregistered memo has zero P11 hits");
			Assert::AreEqual(2ull, modUnreg.mod.P11EnergyFallbacks(), L"unregistered memo increments P11EnergyFallbacks");
			Assert::IsFalse(modUnreg.mod.LastP11EnergyFallback().empty(), L"unregistered memo records visible fallback reason");

			Assert::AreEqual(2ull, modPoison.mod.P11EnergyHits(), L"poisoned memo still hit P11");
			const Image imgP11 = Download(gpu, outP11.planes[0].texture);
			const Image imgNull = Download(gpu, outNull.planes[0].texture);
			const Image imgPoison = Download(gpu, outPoison.planes[0].texture);
			const double boostVsIn = MaxAbsDiff(imgP11, in1);
			const double nullBoostVsIn = MaxAbsDiff(imgNull, in1);
			const double nullDelta = MaxAbsDiff(imgP11, imgNull);
			const double poisonDelta = MaxAbsDiff(imgP11, imgPoison);
			double sqNull = 0.0, sqPoison = 0.0;
			for (size_t i = 0; i < imgP11.v.size(); ++i)
			{
				const double dn = imgP11.v[i] - imgNull.v[i];
				const double dp = imgP11.v[i] - imgPoison.v[i];
				sqNull += dn * dn;
				sqPoison += dp * dp;
			}
			const double rmsNullDelta = std::sqrt(sqNull / static_cast<double>(imgP11.v.size()));
			const double rmsPoisonDelta = std::sqrt(sqPoison / static_cast<double>(imgP11.v.size()));
			Assert::IsTrue(boostVsIn > 0.5, L"non-vacuous: TR with real P11 actively restores the coarse B-frame");
			Assert::IsTrue(nullBoostVsIn > 0.5, L"non-vacuous: TR fallback (memo=nullptr) actively restores the coarse B-frame");
			Assert::IsTrue(poisonDelta > 0.5, L"negative control: poisoned P11 re_s2/re_s4 shifts TR output on the exact same frame");
			Assert::IsTrue(rmsNullDelta < 1.0,
				L"TR P11 output matches memo=nullptr fallback quantitatively in RMS (accurate P11 re_s2/re_s4 ingest & scaling)");
			Assert::IsTrue(rmsNullDelta < 0.5 * rmsPoisonDelta && nullDelta < 0.75 * poisonDelta,
				L"TR P11 parity vs memo=nullptr is strictly tighter than poisoned P11 divergence");
		}

		// W23-B3 M2#12 / HIGH #3: end-to-end TextureRestore::Module::Apply exercises vector-veto with trusted P7
		// (Reason::Ok, confidence 0.85) on a pan-cut scene verdict: default params.vectorVeto=true suppresses
		// restart (SceneVectorVetoes() >= 1, keeps references); negative control params.vectorVeto=false restarts.
		BEGIN_TEST_METHOD_ATTRIBUTE(TextureRestoreGpuVectorVetoSuppressesPanCutRestart)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TextureRestoreGpuVectorVetoSuppressesPanCutRestart)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;
			struct TexGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
				~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
			};
			struct ModGuard
			{
				pl_gpu gpu = nullptr;
				TextureRestore::Module mod;
				explicit ModGuard(pl_gpu g) : gpu(g) {}
				~ModGuard() { mod.Release(gpu); }
				ModGuard(const ModGuard&) = delete;
				ModGuard& operator=(const ModGuard&) = delete;
			};

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();

			std::mt19937 rng(609u);
			const Image scene = Scene(kW, kH, rng);
			const Image in0 = Quantised(Dctq(Sum(scene, Grain(kW, kH, 1.5 * kCode8, false, rng)), 4.0));
			const Image in1 = Quantised(Dctq(Sum(scene, Grain(kW, kH, 1.5 * kCode8, false, rng)), 28.0));
			TexGuard tex0(gpu, UploadLuma(gpu, in0));
			TexGuard tex1(gpu, UploadLuma(gpu, in1));
			Assert::IsTrue(tex0.tex != nullptr && tex1.tex != nullptr);

			VpSkeleton::SkeletonMemo memoP7;
			CameraModel::CameraMotion cam;
			cam.seq = 602u; cam.pairFrom = 601u; cam.pairTo = 602u;
			cam.pairSeconds = 1.0 / 24.0;
			cam.valid = true;
			cam.reason = CameraModel::Reason::Ok;
			cam.confidence = 0.85;
			cam.params.type = CameraModel::ModelType::Translation;
			cam.params.h[2] = 12.0; cam.params.h[5] = 0.0;
			cam.picture.left = 0; cam.picture.top = 0; cam.picture.right = kW; cam.picture.bottom = kH;
			VpSkeleton::OwnerFill p7Fill;
			p7Fill.renderThreadOnly = false;
			p7Fill.fill = [cam](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) -> bool {
				if (req.seq != 602u) return false;
				uint8_t buf[CameraLink::kRecordPodMaxBytes]{};
				const size_t n = CameraLink::EncodeRecordPod(cam, buf, sizeof(buf));
				if (n == 0u) return false;
				out.bytes.assign(buf, buf + n);
				return true;
			};
			Assert::IsTrue(memoP7.RegisterOwnerFill(VpSkeleton::kP7CameraFit, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP7CameraFit), std::move(p7Fill)) == Reg::Registered);

			VpDetailRestore::SceneReaderFn panCutReader = [](uint64_t seq, FrameAnalysis::Scene& sc) -> bool {
				sc = FrameAnalysis::Scene();
				if (seq == 602u)
				{
					sc.cut = true;
					sc.confirmed = true;
					sc.cutStrength = 1.0f;
				}
				return true;
			};

			ModGuard modVeto(gpu), modNoVeto(gpu);
			pl_tex inputs[2] = { tex0.tex, tex1.tex };
			pl_frame outVeto{}, outNoVeto{};
			for (int k = 0; k < 2; ++k)
			{
				TextureRestore::Params pVeto = Frame(601u + static_cast<uint64_t>(k));
				Assert::IsTrue(pVeto.vectorVeto, L"TextureRestore::Params::vectorVeto defaults true");
				pVeto.sceneReader = panCutReader;
				pVeto.memo = &memoP7;
				Assert::IsTrue(modVeto.mod.Apply(gpu, dev.dispatch, LumaFrame(inputs[k]), pVeto, outVeto), Wide(modVeto.mod.LastError()).c_str());

				TextureRestore::Params pNoVeto = pVeto;
				pNoVeto.vectorVeto = false;
				Assert::IsTrue(modNoVeto.mod.Apply(gpu, dev.dispatch, LumaFrame(inputs[k]), pNoVeto, outNoVeto), Wide(modNoVeto.mod.LastError()).c_str());
			}

			Assert::AreEqual(1ull, modVeto.mod.SceneVectorVetoes(), L"TR with default vectorVeto=true vetoes pan-cut restart on trusted P7");
			Assert::AreEqual(1.0f, modVeto.mod.LastHistoryKeep(), 1e-6f, L"vetoed pan keeps full history");
			Assert::AreEqual(0ull, modNoVeto.mod.SceneVectorVetoes(), L"negative control: vectorVeto=false does NOT veto pan-cut restart");
			Assert::AreEqual(0.0f, modNoVeto.mod.LastHistoryKeep(), 1e-6f, L"negative control: unvetoed pan cut restarts references");

			const Image imgVeto = Download(gpu, outVeto.planes[0].texture);
			const Image imgNoVeto = Download(gpu, outNoVeto.planes[0].texture);
			Assert::IsTrue(MaxAbsDiff(imgVeto, in1) > 0.5, L"vetoed TR retained frame 0 references and restored coarse frame 1");
			Assert::IsTrue(MaxAbsDiff(imgVeto, imgNoVeto) > 0.5, L"negative control: suppressing vs taking restart materially alters TR GPU output");
		}
	};
}

// B3 (shared unit U2): TextureRestore's Gaussians (sigma 1 at full resolution, the half-resolution pair for sigma 2.5 / 6)
// now call VpUnit::BandFromBase. The frozen copies below are the private implementations they replaced; the numbers (and
// so the GLSL weight tables) must be bit-identical.
namespace TextureRestoreBandFromBaseNs
{
	inline int OldRadius1(double scale) { return (std::max)(1, static_cast<int>(std::ceil(3.0 * TextureRestore::kSigma1 * scale - 1e-9))); }
	inline double OldWeight1(int k, double scale)
	{
		const double s = TextureRestore::kSigma1 * scale;
		const int r = OldRadius1(scale);
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += std::exp(-0.5 * i * i / (s * s));
		return std::exp(-0.5 * k * k / (s * s)) / sum;
	}
	inline double OldHalfSigma(double sigma) { return std::sqrt(sigma * sigma - 0.25) / 2.0; }
	inline int OldHalfRadius(double sigma) { return (std::max)(1, static_cast<int>(std::ceil(3.5 * OldHalfSigma(sigma) - 1e-9))); }
	inline double OldHalfWeight(int k, double sigma)
	{
		const double s = OldHalfSigma(sigma);
		const int r = OldHalfRadius(sigma);
		double sum = 0.0;
		for (int i = -r; i <= r; ++i) sum += std::exp(-0.5 * i * i / (s * s));
		return std::exp(-0.5 * k * k / (s * s)) / sum;
	}
	inline bool Same(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

	TEST_CLASS(TextureRestoreBandFromBaseIdentityTests)
	{
	public:
		TEST_METHOD(FullResolutionRadiusAndWeightAreBitIdentical)
		{
			std::vector<double> scales;
			for (double s = 1.0; s <= 4.0001; s += 0.0625) scales.push_back(s);
			for (double lines : { 480.0, 1080.0, 2160.0, 3240.0, 4320.0, 8640.0 }) scales.push_back(TextureRestore::Scale(lines));
			for (double scale : scales)
			{
				const int r = OldRadius1(scale);
				Assert::AreEqual(r, TextureRestore::Radius1(scale), L"radius");
				for (int k = -r; k <= r; ++k)
					Assert::IsTrue(Same(OldWeight1(k, scale), TextureRestore::Weight1(k, scale)), L"weight bits");
			}
		}

		TEST_METHOD(HalfResolutionSigmaRadiusAndWeightAreBitIdentical)
		{
			for (double scale : { 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0 })
				for (double base : { TextureRestore::kSigma2, TextureRestore::kSigma3 })
				{
					const double sigma = base * scale;
					Assert::IsTrue(Same(OldHalfSigma(sigma), TextureRestore::HalfSigma(sigma)), L"half sigma bits");
					const int r = OldHalfRadius(sigma);
					Assert::AreEqual(r, TextureRestore::HalfRadius(sigma), L"half radius");
					for (int k = -r; k <= r; ++k)
						Assert::IsTrue(Same(OldHalfWeight(k, sigma), TextureRestore::HalfWeight(k, sigma)), L"half weight bits");
				}
		}

		TEST_METHOD(EmittedWeightTablesTextIsByteIdentical)
		{
			for (double scale : { 1.0, 1.5, 2.0, 2.5 })
			{
				const int r1 = OldRadius1(scale);
				Assert::AreEqual(
					VpvGpu::ConstArray("w", 2 * r1 + 1, [&](int k) { return OldWeight1(k - r1, scale); }),
					VpvGpu::ConstArray("w", 2 * TextureRestore::Radius1(scale) + 1, [&](int k) { return TextureRestore::Weight1(k - TextureRestore::Radius1(scale), scale); }),
					L"w1 table text");
				for (double base : { TextureRestore::kSigma2, TextureRestore::kSigma3 })
				{
					const double sigma = base * scale;
					const int rh = OldHalfRadius(sigma);
					Assert::AreEqual(
						VpvGpu::ConstArray("w", 2 * rh + 1, [&](int k) { return OldHalfWeight(k - rh, sigma); }),
						VpvGpu::ConstArray("w", 2 * TextureRestore::HalfRadius(sigma) + 1, [&](int k) { return TextureRestore::HalfWeight(k - TextureRestore::HalfRadius(sigma), sigma); }),
						L"half table text");
				}
			}
		}

		// W23-B3 M2#2 / M1 (U2 B4 CPU BandMemo): TextureRestore::AcquireFineCpuBand and AcquireHalfCpuBand
		// (factor=3.5, eps=1e-9 via BandMemo bit 6) hit kBandMemo once per key, cache on repeat, and match
		// independent BlurTaps bit-for-bit; unencodable factor=5.0 fails BandMemo::Encodable and declines visibly.
		TEST_METHOD(TextureRestoreCpuBandAcquireAndFactor35VisibleFallback)
		{
			namespace BFB = VpUnit::BandFromBase;
			namespace GB = Vpv::GrainBand;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			const int W = 64, H = 48;
			const int HW = (W + 1) / 2, HH = (H + 1) / 2;
			const size_t nFull = static_cast<size_t>(W) * H;
			const size_t nHalf = static_cast<size_t>(HW) * HH;

			std::vector<float> fullPlane(nFull, 0.0f);
			uint32_t rng = 0x9e3779b9u;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					rng = rng * 1664525u + 1013904223u;
					const float noise = (static_cast<float>((rng >> 8) & 1023u) / 1023.0f - 0.5f) * 0.22f;
					const float box = (x >= 18 && x < 46 && y >= 10 && y < 38) ? 0.45f : 0.12f;
					fullPlane[static_cast<size_t>(y) * W + x] = (std::min)(1.0f, (std::max)(0.0f, box + noise));
				}

			std::vector<uint16_t> halfU16(nHalf, 0u);
			std::vector<float> halfFloat(nHalf, 0.0f);
			constexpr float kInvU16Max = 1.0f / 65535.0f;
			for (int y = 0; y < HH; ++y)
				for (int x = 0; x < HW; ++x)
				{
					const float avg = 0.25f * (fullPlane[static_cast<size_t>(2 * y) * W + 2 * x]
						+ fullPlane[static_cast<size_t>(2 * y) * W + 2 * x + 1]
						+ fullPlane[static_cast<size_t>(2 * y + 1) * W + 2 * x]
						+ fullPlane[static_cast<size_t>(2 * y + 1) * W + 2 * x + 1]);
					const uint16_t u = static_cast<uint16_t>(std::lround(avg * 65535.0f));
					halfU16[static_cast<size_t>(y) * HW + x] = u;
					halfFloat[static_cast<size_t>(y) * HW + x] = static_cast<float>(u) * kInvU16Max;
				}

			VpSkeleton::SkeletonMemo memo;
			const uint64_t kSeq = 60u;
			VpSkeleton::OwnerFill p13Cpu;
			p13Cpu.frameScoped = true;
			p13Cpu.renderThreadOnly = false;
			p13Cpu.fill = [&fullPlane, kSeq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq != kSeq) return false;
				out.bytes.resize(fullPlane.size() * sizeof(float));
				std::memcpy(out.bytes.data(), fullPlane.data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP13SourceRing), std::move(p13Cpu)) == Reg::Registered);

			VpSkeleton::OwnerFill p1Cpu;
			p1Cpu.frameScoped = true;
			p1Cpu.renderThreadOnly = false;
			p1Cpu.fill = [&halfU16, kSeq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq != kSeq) return false;
				out.bytes.resize(halfU16.size() * sizeof(uint16_t));
				std::memcpy(out.bytes.data(), halfU16.data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP1HalfLuma), std::move(p1Cpu)) == Reg::Registered);

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Cpu,
				GB::kBandMemoUnit, GB::MakeBandMemoCpuOwnerFill(counters)) == Reg::Registered);

			// 1. AcquireFineCpuBand: memo hit, single compute, bit-exact parity with independent BlurTaps.
			std::vector<float> fineRef;
			const std::vector<float> fineTaps = BFB::Taps<float>(BFB::kSigmaFine, 3.0, true, 0.0);
			BFB::BlurTaps<float>(fullPlane, fineRef, W, H, fineTaps, BFB::EdgeMode::Replicate);

			const TextureRestore::CpuBandResult fineMemo = TextureRestore::AcquireFineCpuBand(fullPlane, W, H, 1.0, &memo, kSeq);
			Assert::IsTrue(fineMemo.fromBandMemo && fineMemo.fallbackReason.empty(), L"AcquireFineCpuBand hits kBandMemo");
			Assert::AreEqual(uint64_t(1), counters->fills.load(), L"fine band computed once");
			Assert::IsTrue(std::memcmp(fineMemo.samples.data(), fineRef.data(), fineRef.size() * sizeof(float)) == 0,
				L"AcquireFineCpuBand matches independent BlurTaps bit-for-bit");

			const TextureRestore::CpuBandResult fineRepeat = TextureRestore::AcquireFineCpuBand(fullPlane, W, H, 1.0, &memo, kSeq);
			Assert::AreEqual(uint64_t(1), counters->fills.load(), L"repeated AcquireFineCpuBand reuses memo");
			Assert::IsTrue(fineRepeat.fromBandMemo && fineRepeat.fallbackReason.empty());

			// Negative controls for AcquireFineCpuBand: memo=nullptr, seq=0, unregistered memo visibly report fallback.
			VpSkeleton::SkeletonMemo emptyMemo;
			const TextureRestore::CpuBandResult fineNull = TextureRestore::AcquireFineCpuBand(fullPlane, W, H, 1.0, nullptr, kSeq);
			const TextureRestore::CpuBandResult fineUnkeyed = TextureRestore::AcquireFineCpuBand(fullPlane, W, H, 1.0, &memo, 0u);
			const TextureRestore::CpuBandResult fineUnreg = TextureRestore::AcquireFineCpuBand(fullPlane, W, H, 1.0, &emptyMemo, kSeq);
			Assert::IsFalse(fineNull.fromBandMemo || fineNull.fallbackReason.empty(), L"fine memo=nullptr visible fallback");
			Assert::IsFalse(fineUnkeyed.fromBandMemo || fineUnkeyed.fallbackReason.empty(), L"fine seq=0 visible fallback");
			Assert::IsFalse(fineUnreg.fromBandMemo || fineUnreg.fallbackReason.empty(), L"fine unregistered visible fallback");
			Assert::IsTrue(std::memcmp(fineNull.samples.data(), fineRef.data(), fineRef.size() * sizeof(float)) == 0, L"fine fallback bit-exact");

			// 2. AcquireHalfCpuBand (factor=3.5, eps=1e-9, encoded in BandMemo bit 6): hits kBandMemo once per
			//    sigma key, caches on repeat, matches independent BlurTaps bit-for-bit, and visibly falls back when memo=nullptr.
			uint64_t expectedFills = 1u;
			for (double fullSigma : { TextureRestore::kSigma2, TextureRestore::kSigma3 })
			{
				const TextureRestore::CpuBandResult halfRes = TextureRestore::AcquireHalfCpuBand(halfFloat, HW, HH, fullSigma, &memo, kSeq);
				Assert::IsTrue(halfRes.fromBandMemo && halfRes.fallbackReason.empty(),
					L"factor=3.5 half band hits kBandMemo via bit-6 codec extension");
				++expectedFills;
				Assert::AreEqual(expectedFills, counters->fills.load(), L"half band computed once per distinct sigma");

				const TextureRestore::CpuBandResult halfRepeat = TextureRestore::AcquireHalfCpuBand(halfFloat, HW, HH, fullSigma, &memo, kSeq);
				Assert::IsTrue(halfRepeat.fromBandMemo && halfRepeat.fallbackReason.empty(), L"repeat half band hits cached kBandMemo");
				Assert::AreEqual(expectedFills, counters->fills.load(), L"repeat half band reuses memo without recompute");

				const int rh = TextureRestore::HalfRadius(fullSigma);
				std::vector<float> halfTaps(static_cast<size_t>(2 * rh + 1));
				for (int i = -rh; i <= rh; ++i)
					halfTaps[static_cast<size_t>(i + rh)] = static_cast<float>(TextureRestore::HalfWeight(i, fullSigma));
				std::vector<float> halfRef;
				BFB::BlurTaps<float>(halfFloat, halfRef, HW, HH, halfTaps, BFB::EdgeMode::Replicate);
				Assert::AreEqual(halfRef.size(), halfRes.samples.size());
				Assert::IsTrue(std::memcmp(halfRes.samples.data(), halfRef.data(), halfRef.size() * sizeof(float)) == 0,
					L"AcquireHalfCpuBand memo hit matches independent BlurTaps bit-for-bit");
				Assert::IsTrue(std::memcmp(halfRepeat.samples.data(), halfRef.data(), halfRef.size() * sizeof(float)) == 0,
					L"AcquireHalfCpuBand cached repeat matches independent BlurTaps bit-for-bit");

				const TextureRestore::CpuBandResult halfNull = TextureRestore::AcquireHalfCpuBand(halfFloat, HW, HH, fullSigma, nullptr, kSeq);
				Assert::IsFalse(halfNull.fromBandMemo || halfNull.fallbackReason.empty(), L"half memo=nullptr visible fallback");
				Assert::IsTrue(std::memcmp(halfNull.samples.data(), halfRef.data(), halfRef.size() * sizeof(float)) == 0,
					L"half memo=nullptr fallback matches independent BlurTaps bit-for-bit");

				// Non-vacuity negative control: raw unblurred halfFloat diverges substantially from blurred band.
				float maxHalfDiff = 0.0f;
				for (size_t i = 0; i < nHalf; ++i)
					maxHalfDiff = (std::max)(maxHalfDiff, std::fabs(halfRes.samples[i] - halfFloat[i]));
				Assert::IsTrue(maxHalfDiff > 0.05f, L"negative control: half-res blur materially transforms input");
			}

			// 3. Negative control: unencodable factor=5.0 is rejected by BandMemo::Encodable and visibly declined on Acquire.
			{
				VpUnit::BandMemo::BandKey badKey;
				badKey.resolution = VpUnit::BandMemo::Stage::Half;
				badKey.width = static_cast<uint32_t>(HW);
				badKey.height = static_cast<uint32_t>(HH);
				badKey.sigmaPx = BFB::HalfSigma(TextureRestore::kSigma2);
				badKey.law = VpUnit::BandMemo::Law::Taps;
				badKey.edge = BFB::EdgeMode::Replicate;
				badKey.factor = 5.0;
				badKey.minOne = true;
				badKey.eps = 1e-9;
				Assert::IsFalse(VpUnit::BandMemo::Encodable(badKey), L"negative control: factor=5.0 is outside BandMemo codec");
				const VpSkeleton::VariantKey badVar = VpUnit::BandMemo::ToVariant(badKey);
				Assert::IsFalse(VpUnit::BandMemo::FromVariant(badVar, nullptr), L"unencodable ToVariant does not decode");
				const VpSkeleton::AcquireResult badAcq = memo.Acquire(VpSkeleton::kBandMemo, kSeq, VpSkeleton::AcquireForm::Cpu, badVar);
				Assert::IsFalse(badAcq.Ok(), L"Acquire on unencodable factor=5.0 key declines");
				Assert::IsFalse(memo.LastCause(VpSkeleton::kBandMemo).empty(), L"unencodable Acquire decline reason is visible");
				Assert::AreEqual(expectedFills, counters->fills.load(), L"declined key never increments BandMemo fills");

				// Also verify an out-of-range encoded factor index (fi=5: bits 3..4 = 1, bit 6 = 1) is rejected on Acquire.
				VpUnit::BandMemo::BandKey validKey = badKey;
				validKey.factor = 3.5;
				Assert::IsTrue(VpUnit::BandMemo::Encodable(validKey), L"factor=3.5 is encodable");
				VpSkeleton::VariantKey badIdxVar = VpUnit::BandMemo::ToVariant(validKey);
				badIdxVar.param1 = static_cast<uint16_t>((badIdxVar.param1 & ~uint16_t(0x58u)) | uint16_t((1u << 3) | 64u));
				Assert::IsFalse(VpUnit::BandMemo::FromVariant(badIdxVar, nullptr), L"out-of-range factor index 5 fails FromVariant");
				const VpSkeleton::AcquireResult badIdxAcq = memo.Acquire(VpSkeleton::kBandMemo, kSeq, VpSkeleton::AcquireForm::Cpu, badIdxVar);
				Assert::IsFalse(badIdxAcq.Ok(), L"Acquire with out-of-range factor index visibly declines");
				Assert::IsFalse(memo.LastCause(VpSkeleton::kBandMemo).empty(), L"out-of-range factor decline reason is visible");
				Assert::AreEqual(expectedFills, counters->fills.load(), L"out-of-range factor never increments fills");
			}
		}
	};
}
