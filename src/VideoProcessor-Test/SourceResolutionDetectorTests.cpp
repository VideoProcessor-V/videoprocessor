#include "pch.h"
#include "CppUnitTest.h"

#include <SourceResolutionDetector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr int kTile = 256;

		// A synthetic scene: a sum of separable cosines, amplitude falling as
		// 1/f (natural pictures), frequencies up to the master's Nyquist, an
		// optional defocus (Gaussian, sigma in master pixels).
		struct Scene
		{
			struct Wave { double fx, fy, px, py, amplitude; };
			std::vector<Wave> waves;
			Scene(unsigned seed, double defocusSigma = 0.0, int count = 260)
			{
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				for (int i = 0; i < count; ++i)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, u(rng));   // log-uniform up to Nyquist
					const double angle = u(rng) * kPi;
					Wave w{ f * std::cos(angle), f * std::sin(angle), u(rng) * 2 * kPi, u(rng) * 2 * kPi, 0.0 };
					w.amplitude = 0.012 / std::sqrt(f) * std::exp(-2.0 * kPi * kPi * defocusSigma * defocusSigma * f * f);
					waves.push_back(w);
				}
			}
			// The scene sampled on a w x h grid of master pixels starting at (x0, y0).
			std::vector<double> Sample(int x0, int y0, int w, int h) const
			{
				std::vector<double> out(static_cast<size_t>(w) * h, 0.45);
				std::vector<double> cx(w), cy(h);
				for (const Wave& v : waves)
				{
					// cos(a + b) split into separable terms.
					std::vector<double> sx(w), sy(h);
					for (int x = 0; x < w; ++x) { const double a = 2 * kPi * v.fx * (x0 + x) + v.px; cx[x] = std::cos(a); sx[x] = std::sin(a); }
					for (int y = 0; y < h; ++y) { const double b = 2 * kPi * v.fy * (y0 + y) + v.py; cy[y] = std::cos(b); sy[y] = std::sin(b); }
					for (int y = 0; y < h; ++y)
					{
						double* row = &out[static_cast<size_t>(y) * w];
						const double c = v.amplitude * cy[y], s = v.amplitude * sy[y];
						for (int x = 0; x < w; ++x) row[x] += cx[x] * c - sx[x] * s;
					}
				}
				return out;
			}
		};

		using Kernel = std::function<double(double)>;
		double Sinc(double x) { return std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
		const Kernel kLanczos3 = [](double x) { return std::fabs(x) < 3.0 ? Sinc(x) * Sinc(x / 3.0) : 0.0; };
		const Kernel kCatmullRom = [](double x) {
			x = std::fabs(x);
			return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
		const Kernel kMitchell = [](double x) {
			x = std::fabs(x);
			const double B = 1.0 / 3, C = 1.0 / 3;
			return x < 1 ? ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6
				: x < 2 ? ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6 : 0.0; };
		const Kernel kBilinear = [](double x) { x = std::fabs(x); return x < 1 ? 1 - x : 0.0; };

		// The scene mastered at `ratio` of the native height and upscaled to a
		// native kTile x kTile tile with `kernel` (separable), or sampled
		// directly at native resolution when ratio is 1.
		std::vector<float> Tile(const Scene& scene, double ratio, const Kernel& kernel, double grain = 0.0, int codeMax = 1023, unsigned seed = 7, double blocks = 0.0,
			const std::function<double(double)>& transfer = nullptr)
		{
			std::vector<double> native(static_cast<size_t>(kTile) * kTile);
			if (ratio >= 1.0)
				native = scene.Sample(1000, 700, kTile, kTile);
			else
			{
				const int margin = 4;
				const int mw = static_cast<int>(std::ceil(kTile * ratio)) + 2 * margin;
				const int mx0 = static_cast<int>(std::floor(1000 * ratio)) - margin, my0 = static_cast<int>(std::floor(700 * ratio)) - margin;
				const std::vector<double> master = scene.Sample(mx0, my0, mw, mw);
				// Master pixel coordinate of native pixel x: (x + 0.5) * ratio - 0.5.
				auto resample = [&](const double* src, int stride, int count, double* dst, int dstStride, int origin, int m0) {
					for (int x = 0; x < kTile; ++x)
					{
						const double u = (origin + x + 0.5) * ratio - 0.5 - m0;   // master index in the sampled region
						const int c = static_cast<int>(std::floor(u));
						double sum = 0, wsum = 0;
						for (int k = c - 3; k <= c + 4; ++k)
						{
							if (k < 0 || k >= count) continue;
							const double w = kernel(u - k);
							sum += w * src[static_cast<size_t>(k) * stride];
							wsum += w;
						}
						dst[static_cast<size_t>(x) * dstStride] = wsum != 0 ? sum / wsum : 0;
					}
				};
				std::vector<double> rows(static_cast<size_t>(mw) * kTile);
				for (int y = 0; y < mw; ++y) resample(&master[static_cast<size_t>(y) * mw], 1, mw, &rows[static_cast<size_t>(y) * kTile], 1, 1000, mx0);
				for (int x = 0; x < kTile; ++x) resample(&rows[x], kTile, mw, &native[x], kTile, 700, my0);
			}
			std::mt19937 rng(seed);
			std::normal_distribution<double> noise(0.0, 1.0);
			std::vector<float> out(native.size());
			for (int y = 0; y < kTile; ++y)
				for (int x = 0; x < kTile; ++x)
				{
					double v = native[static_cast<size_t>(y) * kTile + x];
					if (grain > 0) v += grain * noise(rng);
					if (blocks > 0) v += blocks * std::sin(13.7 * (x / 8) + 7.1 * (y / 8));   // a per-8x8-block offset
					v = (std::min)(1.0, (std::max)(0.0, v));
					if (transfer) v = transfer(v);
					v = std::round(v * codeMax) / codeMax;
					out[static_cast<size_t>(y) * kTile + x] = static_cast<float>(v);
				}
			return out;
		}

		// Apple TV with Match Dynamic Range off while Dolby Vision is active
		// (owner 2026-10-15): SDR re-encoded into PQ. SDR signal -> light
		// (gamma 2.4) -> SDR white at 203 nits -> PQ signal.
		double SdrToPq(double v)
		{
			const double light = std::pow(v, 2.4) * 203.0 / 10000.0;
			const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
			const double p = std::pow(light, m1);
			return std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
		}

		// A frame of `count` tiles, each from its own scene (seed + i).
		SourceResolution::FrameReading ReadTiles(const std::vector<std::vector<float>>& tiles, int codeMax = 1023)
		{
			std::vector<SourceResolution::Spectrum> spectra(tiles.size());
			for (size_t i = 0; i < tiles.size(); ++i)
				Assert::IsTrue(SourceResolution::Analyse(tiles[i].data(), kTile, kTile, spectra[i], codeMax));
			SourceResolution::Thresholds t;
			t.codeMaximum = codeMax;
			return SourceResolution::ReadFrame(spectra, t);
		}

		template <typename MakeTile>
		SourceResolution::FrameReading ReadScene(MakeTile make, int count = 4, int codeMax = 1023)
		{
			std::vector<std::vector<float>> tiles;
			for (int i = 0; i < count; ++i) tiles.push_back(make(static_cast<unsigned>(i + 1)));
			return ReadTiles(tiles, codeMax);
		}

		void Log(const char* name, const SourceResolution::FrameReading& r)
		{
			char line[256];
			snprintf(line, sizeof(line), "%-34s tiles %d/%d  ratio %.4f | v %.3f @%.4f (2nd %.3f, grid %.3f)  h %.3f @%.4f (2nd %.3f, grid %.3f)\n",
				name, r.informativeTiles, r.tiles, r.ratio, r.vertical.coherence, r.vertical.ratio, r.vertical.runnerUp, r.vertical.grid,
				r.horizontal.coherence, r.horizontal.ratio, r.horizontal.runnerUp, r.horizontal.grid);
			Logger::WriteMessage(line);
		}

		// ---- Real footage (VP_TEST_REAL_FOOTAGE=<folder of the 4K clips>) ----
		std::string EnvValue(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string value;
			if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
			std::free(v);
			return value;
		}

		struct Region
		{
			int x0 = 0, y0 = 0, w = 0, h = 0;
			std::vector<double> v;
			double at(int x, int y) const
			{
				x = (std::min)((std::max)(x - x0, 0), w - 1);
				y = (std::min)((std::max)(y - y0, 0), h - 1);
				return v[static_cast<size_t>(y) * w + x];
			}
		};

		std::vector<std::pair<int, double>> Taps(double u, double stretch, int reach, const Kernel& kernel, double& wsum)
		{
			std::vector<std::pair<int, double>> taps;
			wsum = 0;
			const int c = static_cast<int>(std::floor(u));
			for (int k = c - reach; k <= c + reach + 1; ++k)
			{
				const double w = kernel((u - k) / stretch);
				if (w != 0) { taps.push_back({ k, w }); wsum += w; }
			}
			return taps;
		}

		// Separable resample of `in` (global pixel grid) by `scale` (out / in
		// size) into the out-grid region (ox0, oy0, ow, oh). Out pixel X samples
		// in-coordinate (X + 0.5) / scale - 0.5; downscales widen the kernel.
		Region Resample(const Region& in, double scale, int ox0, int oy0, int ow, int oh, const Kernel& kernel, double radius)
		{
			const double stretch = scale < 1.0 ? 1.0 / scale : 1.0;
			const int reach = static_cast<int>(std::ceil(radius * stretch)) + 1;
			Region rows;
			rows.x0 = ox0; rows.y0 = in.y0; rows.w = ow; rows.h = in.h;
			rows.v.assign(static_cast<size_t>(ow) * in.h, 0.0);
			for (int x = 0; x < ow; ++x)
			{
				double wsum = 0;
				const auto taps = Taps((ox0 + x + 0.5) / scale - 0.5, stretch, reach, kernel, wsum);
				for (int y = 0; y < in.h; ++y)
				{
					double sum = 0;
					for (const auto& t : taps) sum += t.second * in.at(t.first, in.y0 + y);
					rows.v[static_cast<size_t>(y) * ow + x] = sum / wsum;
				}
			}
			Region out;
			out.x0 = ox0; out.y0 = oy0; out.w = ow; out.h = oh;
			out.v.assign(static_cast<size_t>(ow) * oh, 0.0);
			for (int y = 0; y < oh; ++y)
			{
				double wsum = 0;
				const auto taps = Taps((oy0 + y + 0.5) / scale - 0.5, stretch, reach, kernel, wsum);
				for (int x = 0; x < ow; ++x)
				{
					double sum = 0;
					for (const auto& t : taps) sum += t.second * rows.at(ox0 + x, t.first);
					out.v[static_cast<size_t>(y) * ow + x] = sum / wsum;
				}
			}
			return out;
		}

		// A coarse coding model: 8x8 DCT, coefficients quantised with a step
		// growing with frequency (`q` codes at DC .. 4q at the corner), inverse.
		void Code8x8(std::vector<float>& tile, int n, double q, int codeMax)
		{
			if (q <= 0) return;
			double basis[8][8];
			for (int k = 0; k < 8; ++k)
				for (int i = 0; i < 8; ++i)
					basis[k][i] = (k == 0 ? std::sqrt(1.0 / 8) : std::sqrt(2.0 / 8)) * std::cos(kPi * (i + 0.5) * k / 8);
			for (int by = 0; by + 8 <= n; by += 8)
				for (int bx = 0; bx + 8 <= n; bx += 8)
				{
					double block[8][8], coef[8][8];
					for (int y = 0; y < 8; ++y)
						for (int x = 0; x < 8; ++x) block[y][x] = tile[static_cast<size_t>(by + y) * n + bx + x] * codeMax;
					for (int v = 0; v < 8; ++v)
						for (int u = 0; u < 8; ++u)
						{
							double sum = 0;
							for (int y = 0; y < 8; ++y)
								for (int x = 0; x < 8; ++x) sum += basis[v][y] * basis[u][x] * block[y][x];
							const double step = q * (1.0 + 3.0 * (u + v) / 14.0);
							coef[v][u] = std::round(sum / step) * step;
						}
					for (int y = 0; y < 8; ++y)
						for (int x = 0; x < 8; ++x)
						{
							double sum = 0;
							for (int v = 0; v < 8; ++v)
								for (int u = 0; u < 8; ++u) sum += basis[v][y] * basis[u][x] * coef[v][u];
							sum = (std::min)(static_cast<double>(codeMax), (std::max)(0.0, sum));
							tile[static_cast<size_t>(by + y) * n + bx + x] = static_cast<float>(std::round(sum) / codeMax);
						}
				}
		}

		// A streaming coder's coarse model (owner 2026-10-16, variable bitrate): square
		// transforms of `block` px (8 / 16 / 32, as HEVC and AV1 choose per area),
		// coefficients quantised with a step growing with frequency (`q` codes at DC ..
		// 4q at the corner), inverse, then an in-loop deblocking filter across the
		// block edges (smooths steps up to 2q codes, as the codecs' filters do).
		void CodeBlocks(std::vector<float>& tile, int n, double q, int codeMax, int block, bool deblock)
		{
			if (q <= 0) return;
			const int B = block;
			std::vector<double> basis(static_cast<size_t>(B) * B);
			for (int k = 0; k < B; ++k)
				for (int i = 0; i < B; ++i)
					basis[static_cast<size_t>(k) * B + i] = (k == 0 ? std::sqrt(1.0 / B) : std::sqrt(2.0 / B)) * std::cos(kPi * (i + 0.5) * k / B);
			std::vector<double> a(static_cast<size_t>(B) * B), b(a.size());
			for (int by = 0; by + B <= n; by += B)
				for (int bx = 0; bx + B <= n; bx += B)
				{
					for (int y = 0; y < B; ++y)
						for (int x = 0; x < B; ++x) a[static_cast<size_t>(y) * B + x] = tile[static_cast<size_t>(by + y) * n + bx + x] * codeMax;
					// Forward, separable: rows then columns.
					for (int y = 0; y < B; ++y)
						for (int u = 0; u < B; ++u)
						{
							double sum = 0;
							for (int x = 0; x < B; ++x) sum += basis[static_cast<size_t>(u) * B + x] * a[static_cast<size_t>(y) * B + x];
							b[static_cast<size_t>(y) * B + u] = sum;
						}
					for (int u = 0; u < B; ++u)
						for (int v = 0; v < B; ++v)
						{
							double sum = 0;
							for (int y = 0; y < B; ++y) sum += basis[static_cast<size_t>(v) * B + y] * b[static_cast<size_t>(y) * B + u];
							const double step = q * (1.0 + 3.0 * (u + v) / (2.0 * (B - 1)));
							a[static_cast<size_t>(v) * B + u] = std::round(sum / step) * step;
						}
					// Inverse.
					for (int u = 0; u < B; ++u)
						for (int y = 0; y < B; ++y)
						{
							double sum = 0;
							for (int v = 0; v < B; ++v) sum += basis[static_cast<size_t>(v) * B + y] * a[static_cast<size_t>(v) * B + u];
							b[static_cast<size_t>(y) * B + u] = sum;
						}
					for (int y = 0; y < B; ++y)
						for (int x = 0; x < B; ++x)
						{
							double sum = 0;
							for (int u = 0; u < B; ++u) sum += basis[static_cast<size_t>(u) * B + x] * b[static_cast<size_t>(y) * B + u];
							tile[static_cast<size_t>(by + y) * n + bx + x] = static_cast<float>((std::min)(static_cast<double>(codeMax), (std::max)(0.0, sum)));
						}
				}
			if (deblock)
			{
				const double limit = 2.0 * q;
				auto filter = [&](float& p1, float& p0, float& q0, float& q1) {
					if (std::fabs(p0 - q0) > limit) return;   // a real edge
					const float np0 = (p1 + 2 * p0 + q0) / 4, nq0 = (p0 + 2 * q0 + q1) / 4;
					p0 = np0; q0 = nq0;
				};
				for (int y = 0; y < n; ++y)
					for (int x = B; x < n; x += B)
					{
						float* r = &tile[static_cast<size_t>(y) * n];
						filter(r[x - 2], r[x - 1], r[x], r[x + 1]);
					}
				for (int y = B; y < n; y += B)
					for (int x = 0; x < n; ++x)
						filter(tile[static_cast<size_t>(y - 2) * n + x], tile[static_cast<size_t>(y - 1) * n + x],
							tile[static_cast<size_t>(y) * n + x], tile[static_cast<size_t>(y + 1) * n + x]);
			}
			for (float& v : tile) v = static_cast<float>(std::round(v) / codeMax);
		}

		SourceResolution::FrameReading Frame(double ratio, double mean, int informative = 4)
		{
			SourceResolution::FrameReading f;
			f.tiles = 4;
			f.informativeTiles = informative;
			f.ratio = ratio;
			f.mean = mean;
			f.signature = mean * 10.0;
			return f;
		}
	}

	TEST_CLASS(SourceResolutionDetectorTests)
	{
	public:
		// False positives first (owner 2026-10-15): real 4K - sharp, grainy,
		// soft, 8-bit - must read native, and flat or dark frames carry no evidence.
		TEST_METHOD(RealFourKReadsNative)
		{
			struct Case { const char* name; double defocus; double grain; int codeMax; };
			const Case cases[] = {
				{ "4K sharp", 0.0, 0.0, 1023 },
				{ "4K light grain", 0.0, 1.5 / 255.0, 1023 },
				{ "4K heavy grain", 0.0, 5.0 / 255.0, 1023 },
				{ "4K 8-bit", 0.0, 0.0, 255 },
				{ "4K soft (sigma 1)", 1.0, 0.0, 1023 },
				{ "4K defocused (sigma 3) + grain", 3.0, 0.5 / 255.0, 1023 },
			};
			for (const Case& c : cases)
			{
				const SourceResolution::FrameReading r = ReadScene([&](unsigned seed) {
					return Tile(Scene(seed, c.defocus), 1.0, kLanczos3, c.grain, c.codeMax, seed); }, 4, c.codeMax);
				Log(c.name, r);
				Assert::IsTrue(r.informative(), L"textured tiles are informative");
				Assert::AreEqual(1.0, r.ratio, (std::wstring(L"native: ") + std::wstring(c.name, c.name + strlen(c.name))).c_str());
			}
		}

		TEST_METHOD(FlatAndDarkTilesCarryNoEvidence)
		{
			std::vector<float> flat(static_cast<size_t>(kTile) * kTile, 0.5f);
			std::vector<float> black(flat.size());
			std::mt19937 rng(3);
			std::uniform_int_distribution<int> lsb(0, 1);
			for (float& v : black) v = (64 + lsb(rng)) / 1023.0f;   // black with 1-code dither
			const SourceResolution::FrameReading r = ReadTiles({ flat, black });
			Assert::AreEqual(0, r.informativeTiles);
			Assert::AreEqual(1.0, r.ratio);
		}

		TEST_METHOD(UpscaledSourcesReadTheirHeight)
		{
			// General: any ratio, any of several upscalers, to a fraction of a percent.
			struct K { const char* name; const Kernel* kernel; };
			const K kernels[] = { { "lanczos3", &kLanczos3 }, { "catmull-rom", &kCatmullRom }, { "mitchell", &kMitchell }, { "bilinear", &kBilinear } };
			for (double ratio : { 1440.0 / 2160, 1080.0 / 2160, 900.0 / 2160, 720.0 / 2160 })
				for (const K& k : kernels)
				{
					const SourceResolution::FrameReading r = ReadScene([&](unsigned seed) { return Tile(Scene(seed), ratio, *k.kernel, 0.0, 1023, seed); });
					char name[96];
					snprintf(name, sizeof(name), "up %.4f %s", ratio, k.name);
					Log(name, r);
					Assert::AreEqual(ratio, r.ratio, 0.01, L"reads the source height");
				}
		}

		TEST_METHOD(OneAxisResampledStaysNative)
		{
			// Real detail on either axis keeps native (a source scaled in one
			// direction only - the 120 fps 4K clips here are vertically scaled).
			std::vector<std::vector<float>> tiles;
			for (unsigned seed = 1; seed <= 4; ++seed)
			{
				const std::vector<float> up = Tile(Scene(seed), 0.5, kLanczos3, 0.0, 1023, seed);
				const std::vector<float> native = Tile(Scene(seed), 1.0, kLanczos3, 0.0, 1023, seed);
				// Rows from the upscale's vertical structure, columns native: take
				// the upscale and add back the native's horizontal-only detail is not
				// separable here, so use the transpose trick: vertical upscale = upscale
				// of the transposed horizontal-native picture.
				std::vector<float> mixed(up.size());
				for (int y = 0; y < kTile; ++y)
					for (int x = 0; x < kTile; ++x)
						mixed[static_cast<size_t>(y) * kTile + x] = 0.5f * (up[static_cast<size_t>(y) * kTile + x] + native[static_cast<size_t>(x) * kTile + y]);
				tiles.push_back(mixed);
			}
			const SourceResolution::FrameReading r = ReadTiles(tiles);
			Log("mixed: half native detail", r);
			Assert::AreEqual(1.0, r.ratio, L"native detail present: native");
		}

		TEST_METHOD(UpscaledWithCodingAndGrain)
		{
			const double ratio = 0.5;
			const SourceResolution::FrameReading eight = ReadScene([&](unsigned seed) { return Tile(Scene(seed), ratio, kLanczos3, 0.0, 255, seed); }, 4, 255);
			Log("up 0.5 lanczos 8-bit", eight);
			Assert::AreEqual(ratio, eight.ratio, 0.01, L"8-bit upscale");
			// Coded or re-grained after the upscale: the height or native, never a wrong lower height.
			for (double q : { 1.0, 2.0, 4.0 })
			{
				const SourceResolution::FrameReading coded = ReadScene([&](unsigned seed) {
					std::vector<float> t = Tile(Scene(seed), ratio, kCatmullRom, 0.0, 1023, seed);
					Code8x8(t, kTile, q, 1023);
					return t; });
				char name[64];
				snprintf(name, sizeof(name), "up 0.5 catmull coded q%.0f", q);
				Log(name, coded);
				Assert::IsTrue(coded.ratio == 1.0 || std::fabs(coded.ratio - ratio) <= 0.01, L"coded: height or native");
			}
			for (double grain : { 0.5, 1.5, 4.0 })
			{
				const SourceResolution::FrameReading g = ReadScene([&](unsigned seed) { return Tile(Scene(seed), ratio, kLanczos3, grain / 255.0, 1023, seed); });
				char name[64];
				snprintf(name, sizeof(name), "up 0.5 lanczos + grain %.1f", grain);
				Log(name, g);
				Assert::IsTrue(g.ratio == 1.0 || std::fabs(g.ratio - ratio) <= 0.01, L"re-grained: height or native");
			}
		}

		TEST_METHOD(SdrRemappedToPqKeepsItsReading)
		{
			// Apple TV, Match Dynamic Range off while Dolby Vision is active
			// (owner 2026-10-15): SDR re-encoded into PQ. A per-value curve does
			// not move the resample rate: real 4K stays native, an upscaled 1080
			// still reads 1080, in PQ codes.
			const SourceResolution::FrameReading native = ReadScene([&](unsigned seed) { return Tile(Scene(seed), 1.0, kLanczos3, 0.0, 1023, seed, 0.0, SdrToPq); });
			Log("4K SDR in PQ", native);
			Assert::IsTrue(native.informative(), L"SDR texture in PQ codes is still informative");
			Assert::AreEqual(1.0, native.ratio, L"real 4K in PQ reads native");
			const SourceResolution::FrameReading up = ReadScene([&](unsigned seed) { return Tile(Scene(seed), 0.5, kCatmullRom, 0.0, 1023, seed, 0.0, SdrToPq); });
			Log("up 0.5 SDR in PQ", up);
			Assert::AreEqual(0.5, up.ratio, 0.01, L"upscaled 1080 in PQ reads 1080");
		}

		// Spec 1b / 2d (owner 2026-10-15: the debander serves any low-bitrate
		// source, native 4K included): bitrate quality and effective bit depth.
		TEST_METHOD(BlockedFrameReadsLowQualityAndCleanStaysHigh)
		{
			// The spec's blocked frame: a smooth gradient (a sky) coded hard, so
			// the blocks lose their inner detail and step at their edges.
			auto smooth = [](unsigned seed) {
				std::vector<float> t(static_cast<size_t>(kTile) * kTile);
				for (int y = 0; y < kTile; ++y)
					for (int x = 0; x < kTile; ++x)
						t[static_cast<size_t>(y) * kTile + x] = static_cast<float>(std::round((0.3 + 0.25 * (x + 0.6 * y) / kTile + 0.02 * std::sin(0.02 * x * seed)) * 1023.0) / 1023.0);
				return t; };
			const SourceResolution::FrameReading clean = ReadScene([&](unsigned seed) {
				return Tile(Scene(seed), 1.0, kLanczos3, 1.0 / 255.0, 1023, seed); });
			const SourceResolution::FrameReading blocked = ReadScene([&](unsigned seed) {
				std::vector<float> t = smooth(seed);
				Code8x8(t, kTile, 24.0, 1023);
				return t; });
			char line[200];
			snprintf(line, sizeof(line), "clean: quality %.2f blockiness %.2f band %.3f bits %.1f | blocked: quality %.2f blockiness %.2f band %.3f\n",
				clean.bitrateQuality, clean.blockiness, clean.bandShare, clean.effectiveBits, blocked.bitrateQuality, blocked.blockiness, blocked.bandShare);
			Logger::WriteMessage(line);
			Assert::IsTrue(clean.bitrateQuality > 0.8, L"a clean frame reads clean");
			Assert::IsTrue(blocked.bitrateQuality < 0.5, L"a blocked frame reads below 0.5 (spec 4)");
		}

		TEST_METHOD(EffectiveBitDepthReadsEightBitContentInATenBitSignal)
		{
			// Ramps (diagonal, a few hundred codes across a tile) plus gentle texture.
			auto ramp = [](unsigned seed, int bits, bool pq, double grain) {
				std::mt19937 rng(seed);
				std::normal_distribution<double> noise(0.0, 1.0);
				const double levels = std::pow(2.0, bits) - 1.0;
				std::vector<float> t(static_cast<size_t>(kTile) * kTile);
				for (int y = 0; y < kTile; ++y)
					for (int x = 0; x < kTile; ++x)
					{
						double v = 0.15 + 0.6 * (x + y) / (2.0 * kTile) + 0.01 * std::sin(0.05 * x * (seed + 1));
						if (grain > 0) v += grain * noise(rng);
						v = std::round((std::min)(1.0, (std::max)(0.0, v)) * levels) / levels;   // the content's own depth
						if (pq) v = SdrToPq(v);
						t[static_cast<size_t>(y) * kTile + x] = static_cast<float>(std::round(v * 1023.0) / 1023.0);   // the 10-bit signal
					}
				return t;
			};
			const SourceResolution::FrameReading eight = ReadScene([&](unsigned seed) { return ramp(seed, 8, false, 0.0); });
			const SourceResolution::FrameReading ten = ReadScene([&](unsigned seed) { return ramp(seed, 10, false, 0.0); });
			const SourceResolution::FrameReading eightPq = ReadScene([&](unsigned seed) { return ramp(seed, 8, true, 0.0); });
			const SourceResolution::FrameReading tenGrain = ReadScene([&](unsigned seed) { return ramp(seed, 10, false, 2.0 / 1023.0); });
			char line[200];
			snprintf(line, sizeof(line), "bits: 8-in-10 %.2f, 10 %.2f, 8 via SDR->PQ %.2f, 10 + grain %.2f; banding 8-in-10 %.3f, 10 %.3f\n",
				eight.effectiveBits, ten.effectiveBits, eightPq.effectiveBits, tenGrain.effectiveBits, eight.bandShare, ten.bandShare);
			Logger::WriteMessage(line);
			Assert::AreEqual(8.0, eight.effectiveBits, 0.3, L"8-bit content in a 10-bit signal reads 8");
			Assert::AreEqual(10.0, ten.effectiveBits, 0.3, L"true 10-bit reads 10");
			// 8-bit SDR re-encoded to PQ spans about half the PQ code range (SDR
			// white at 203 nits is PQ code ~590), two codes per level: the step
			// the debander must hide is a 9-bit step in PQ codes - read as such,
			// not as true 10-bit.
			Assert::AreEqual(9.0, eightPq.effectiveBits, 0.35, L"8-bit content re-encoded to PQ (Apple TV LLDV) reads its step in PQ codes");
			Assert::AreEqual(10.0, tenGrain.effectiveBits, 0.3, L"grain does not change a true 10-bit reading");
		}

		TEST_METHOD(ShallowEightBitSkyReadsAsBanded)
		{
			// A sky: a shallow gradient (a few 8-bit levels across a tile) shows
			// band edges; the same sky in 10-bit with a little grain does not.
			auto sky = [](unsigned seed, double levels, double grain) {
				std::mt19937 rng(seed);
				std::normal_distribution<double> noise(0.0, 1.0);
				std::vector<float> t(static_cast<size_t>(kTile) * kTile);
				for (int y = 0; y < kTile; ++y)
					for (int x = 0; x < kTile; ++x)
					{
						double v = 0.55 + (12.0 / 255.0) * (0.7 * x + 0.3 * y) / kTile + 0.002 * seed;
						if (grain > 0) v += grain * noise(rng);
						v = std::round(v * levels) / levels;
						t[static_cast<size_t>(y) * kTile + x] = static_cast<float>(std::round(v * 1023.0) / 1023.0);
					}
				return t;
			};
			const SourceResolution::FrameReading banded = ReadScene([&](unsigned seed) { return sky(seed, 255.0, 0.0); });
			const SourceResolution::FrameReading smooth = ReadScene([&](unsigned seed) { return sky(seed, 1023.0, 1.5 / 1023.0); });
			char line[160];
			snprintf(line, sizeof(line), "sky: 8-bit band share %.3f quality %.2f | 10-bit + grain band share %.3f quality %.2f\n",
				banded.bandShare, banded.bitrateQuality, smooth.bandShare, smooth.bitrateQuality);
			Logger::WriteMessage(line);
			Assert::IsTrue(banded.bandShare > 0.05 && banded.bitrateQuality < 0.8, L"the 8-bit sky reads banded");
			Assert::IsTrue(smooth.bandShare < 0.02 && smooth.bitrateQuality > 0.9, L"the 10-bit grainy sky reads clean");
		}

		// Real 4K footage (VP_TEST_REAL_FOOTAGE; skipped when unset): the
		// genuine frames, the same frames coded, and re-mastered at lower
		// heights then upscaled with several kernels (and coded). Gates: every
		// genuine frame reads native; every uncoded upscale reads its ratio
		// within 0.01; coded upscales read their ratio or native. CSV in
		// VP_TEST_CORPUS_DIR when set.
		TEST_METHOD(RealFootageReadings)
		{
			const std::string folder = EnvValue("VP_TEST_REAL_FOOTAGE");
			if (folder.empty()) { Logger::WriteMessage("VP_TEST_REAL_FOOTAGE not set: skipped\n"); return; }
			struct Clip { std::string file; bool y4m; long long frames; };
			const Clip clips[] = {
				{ "Bosphorus_3840x2160_120fps_420_10bit_YUV.yuv", false, 600 }, { "Jockey_3840x2160_120fps_420_10bit_YUV.yuv", false, 600 },
				{ "ReadySetGo_3840x2160_120fps_420_10bit_YUV.yuv", false, 600 }, { "YachtRide_3840x2160_120fps_420_10bit_YUV.yuv", false, 600 },
				{ "ShakeNDry_3840x2160_120fps_420_10bit_YUV.yuv", false, 300 }, { "park_joy_2160p50.y4m", true, 500 }, { "crowd_run_2160p50.y4m", true, 500 } };
			const int W = 3840, H = 2160;
			struct K { const char* name; const Kernel* kernel; double radius; };
			const K kernels[] = { { "lanczos3", &kLanczos3, 3 }, { "catmull", &kCatmullRom, 2 }, { "mitchell", &kMitchell, 2 }, { "bilinear", &kBilinear, 1 } };
			const std::string corpus = EnvValue("VP_TEST_CORPUS_DIR");
			FILE* csv = nullptr;
			if (!corpus.empty()) fopen_s(&csv, (corpus + "\\source-resolution.csv").c_str(), "w");
			if (csv) fprintf(csv, "clip,frame,case,truth,informative,ratio,v_coherence,v_ratio,v_runnerup,v_grid,h_coherence,h_ratio,h_runnerup,h_grid,ms_per_tile\n");
			int clipsRead = 0, failures = 0;
			std::string failureText;
			double nativeDetailLowest = 1e9, upscaleDetailHighest = -1e9, detailGap = 1e9;
			for (const Clip& clip : clips)
			{
				FILE* f = nullptr;
				if (fopen_s(&f, (folder + "\\" + clip.file).c_str(), "rb") != 0 || !f) continue;
				++clipsRead;
				long long header = 0;
				if (clip.y4m)
				{
					int c;
					while ((c = fgetc(f)) != EOF && c != '\n') ++header;
					++header;
				}
				const int codeMax = clip.y4m ? 255 : 1023;
				for (long long frameIndex : { clip.frames / 5, clip.frames * 3 / 5 })
				{
					Region frame;
					frame.w = W; frame.h = H;
					frame.v.resize(static_cast<size_t>(W) * H);
					bool ok = true;
					if (clip.y4m)
					{
						_fseeki64(f, header + frameIndex * (static_cast<long long>(W) * H * 3 / 2 + 6) + 6, SEEK_SET);
						std::vector<uint8_t> y(static_cast<size_t>(W) * H);
						ok = fread(y.data(), 1, y.size(), f) == y.size();
						for (size_t i = 0; ok && i < y.size(); ++i) frame.v[i] = y[i];
					}
					else
					{
						_fseeki64(f, frameIndex * static_cast<long long>(W) * H * 3, SEEK_SET);
						std::vector<uint16_t> y(static_cast<size_t>(W) * H);
						ok = fread(y.data(), 2, y.size(), f) == y.size();
						for (size_t i = 0; ok && i < y.size(); ++i) frame.v[i] = y[i];
					}
					if (!ok) continue;
					for (double& v : frame.v) v /= codeMax;
					struct Case { std::string name; double ratio; const K* kernel; double q; };
					std::vector<Case> cases = { { "native", 1.0, nullptr, 0.0 }, { "native coded q2", 1.0, nullptr, 2.0 }, { "native coded q6", 1.0, nullptr, 6.0 } };
					for (double ratio : { 1440.0 / 2160, 1080.0 / 2160, 900.0 / 2160, 720.0 / 2160 })
						for (const K& k : kernels)
						{
							char name[64];
							snprintf(name, sizeof(name), "up %.3f %s", ratio, k.name);
							cases.push_back({ name, ratio, &k, 0.0 });
							if (&k == &kernels[0] || &k == &kernels[1]) cases.push_back({ std::string(name) + " q4", ratio, &k, 4.0 });
						}
					double frameNative = 1e9, frameUpscale = -1e9;
					for (const Case& c : cases)
					{
						std::vector<SourceResolution::Spectrum> spectra;
						double analyseMs = 0.0;
						for (int ty = 0; ty < 3; ++ty)
							for (int tx = 0; tx < 4; ++tx)
							{
								const int X0 = 320 + tx * 860, Y0 = 300 + ty * 620;
								std::vector<float> tile(static_cast<size_t>(kTile) * kTile);
								if (c.ratio >= 1.0)
								{
									for (int y = 0; y < kTile; ++y)
										for (int x = 0; x < kTile; ++x)
											tile[static_cast<size_t>(y) * kTile + x] = static_cast<float>(frame.v[static_cast<size_t>(Y0 + y) * W + X0 + x]);
								}
								else
								{
									// The master (lanczos3 downscale of the native frame), coded values, then the upscale back.
									const int m0x = static_cast<int>(std::floor(X0 * c.ratio)) - 8, m0y = static_cast<int>(std::floor(Y0 * c.ratio)) - 8;
									const int mw = static_cast<int>(std::ceil(kTile * c.ratio)) + 16;
									const int reach = static_cast<int>(std::ceil(3.0 / c.ratio)) + 4;
									Region src;
									src.x0 = static_cast<int>(m0x / c.ratio) - reach; src.y0 = static_cast<int>(m0y / c.ratio) - reach;
									src.w = static_cast<int>(mw / c.ratio) + 2 * reach; src.h = src.w;
									src.v.resize(static_cast<size_t>(src.w) * src.h);
									for (int y = 0; y < src.h; ++y)
										for (int x = 0; x < src.w; ++x)
											src.v[static_cast<size_t>(y) * src.w + x] = frame.v[static_cast<size_t>((std::min)(H - 1, (std::max)(0, src.y0 + y))) * W +
												(std::min)(W - 1, (std::max)(0, src.x0 + x))];
									Region master = Resample(src, c.ratio, m0x, m0y, mw, mw, kLanczos3, 3);
									for (double& v : master.v) v = std::round((std::min)(1.0, (std::max)(0.0, v)) * codeMax) / codeMax;
									const Region up = Resample(master, 1.0 / c.ratio, X0, Y0, kTile, kTile, *c.kernel->kernel, c.kernel->radius);
									for (size_t i = 0; i < tile.size(); ++i)
										tile[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, up.v[i])) * codeMax) / codeMax);
								}
								Code8x8(tile, kTile, c.q, codeMax);
								SourceResolution::Spectrum spectrum;
								const auto start = std::chrono::steady_clock::now();
								SourceResolution::Analyse(tile.data(), kTile, kTile, spectrum, codeMax);
								analyseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
								spectra.push_back(std::move(spectrum));
							}
						SourceResolution::Thresholds t;
						t.codeMaximum = codeMax;
						const SourceResolution::FrameReading r = SourceResolution::ReadFrame(spectra, t);
						char name[160];
						snprintf(name, sizeof(name), "%.10s f%lld %s [q %.2f bits %.1f blk %.2f band %.3f]", clip.file.c_str(), frameIndex, c.name.c_str(), r.bitrateQuality, r.effectiveBits, r.blockiness, r.bandShare);
						Log(name, r);
						{
							// Variable bitrate (owner 2026-10-16): detail above the rate in question.
							// Per frame: native (clean or light coding) against this frame's 1080 upscales.
							const double d = SourceResolution::DetailAbove(r, 0.5);
							if (!std::isinf(d))
							{
								if (c.ratio >= 1.0 && c.q < 6.0) frameNative = (std::min)(frameNative, d);
								else if (c.ratio == 0.5) frameUpscale = (std::max)(frameUpscale, d);
							}
						}
						if (csv) fprintf(csv, "%s,%lld,%s,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f\n", clip.file.c_str(), frameIndex, c.name.c_str(), c.ratio,
							r.informativeTiles, r.ratio, r.vertical.coherence, r.vertical.ratio, r.vertical.runnerUp, r.vertical.grid,
							r.horizontal.coherence, r.horizontal.ratio, r.horizontal.runnerUp, r.horizontal.grid, analyseMs / spectra.size());
						if (csv && !r.verticalCurve.empty())
						{
							auto lv = [&](const std::vector<double>& c, int j) { double v = 0; for (int m = j * kTile / 8 - 2; m <= j * kTile / 8 + 2; ++m) if (m >= 0 && m < kTile) v = (std::max)(v, c[m]); return v; };
							fprintf(csv, "#grid,%s,%lld,%s", clip.file.c_str(), frameIndex, c.name.c_str());
							for (int j = 2; j <= 7; ++j) fprintf(csv, ",%.3f/%.3f", lv(r.verticalCurve, j), lv(r.horizontalCurve, j));
							fprintf(csv, "\n");
						}
						const bool pass = c.ratio >= 1.0 ? r.ratio == 1.0
							: c.q > 0.0 ? (r.ratio == 1.0 || std::fabs(r.ratio - c.ratio) <= 0.01)
							: std::fabs(r.ratio - c.ratio) <= 0.01;
						if (!pass)
						{
							++failures;
							if (failureText.size() < 1500) failureText += std::string(name) + "; ";
						}
					}
					if (frameNative < 1e9 && frameUpscale > -1e9)
					{
						detailGap = (std::min)(detailGap, frameNative - frameUpscale);
						nativeDetailLowest = (std::min)(nativeDetailLowest, frameNative);
						upscaleDetailHighest = (std::max)(upscaleDetailHighest, frameUpscale);
					}
				}
				fclose(f);
			}
			if (csv) fclose(csv);
			char summary[128];
			snprintf(summary, sizeof(summary), "Real footage: %d clips, %d gate failures\n", clipsRead, failures);
			Logger::WriteMessage(summary);
			if (!failureText.empty()) Logger::WriteMessage(("Failed: " + failureText + "\n").c_str());
			char detail[200];
			snprintf(detail, sizeof(detail), "Detail above 1080: native lowest %.2f, its 1080 upscales highest %.2f; smallest gap in a frame %.2f (margin %.2f)\n",
				nativeDetailLowest, upscaleDetailHighest, detailGap, SourceResolution::Tracker::Settings{}.detailMarginDecades);
			Logger::WriteMessage(detail);
			Assert::AreEqual(0, failures, L"real-footage gates (see the log)");
			if (clipsRead > 0)
			{
				// Every real 4K frame stands above its own 1080 upscales by more than the margin.
				Assert::IsTrue(detailGap > SourceResolution::Tracker::Settings{}.detailMarginDecades, L"real 4K holds more detail above than its upscales");
			}
		}

		// Planner G18 (2026-10-16): real codecs. tools/make_codec_corpus.py makes x265 /
		// x264 / AV1-with-film-grain encodes of the real clips - native 4K, a 1080 rendition
		// upscaled by the player, and a 1080 master upscaled then coded at 4K - and decodes
		// P/B frames to raw 10-bit 4:2:0. Gate: no native frame reads lower; an upscale
		// reads its height or native, never a wrong height. The share read is reported.
		TEST_METHOD(RealCodecReadings)
		{
			const std::string folder = EnvValue("VP_TEST_CODEC_CORPUS");
			if (folder.empty()) { Logger::WriteMessage("VP_TEST_CODEC_CORPUS not set: skipped\n"); return; }
			FILE* manifest = nullptr;
			if (fopen_s(&manifest, (folder + "\\manifest.csv").c_str(), "r") != 0 || !manifest) { Logger::WriteMessage("no manifest: skipped\n"); return; }
			const int W = 3840, H = 2160, codeMax = 1023;
			struct Tally { int frames = 0, height = 0, native = 0, wrong = 0; };
			std::map<std::string, Tally> tallies;
			int nativeLower = 0, wrongHeight = 0, cases = 0;
			std::string failures;
			char lineBuffer[512];
			fgets(lineBuffer, sizeof(lineBuffer), manifest);   // header
			while (fgets(lineBuffer, sizeof(lineBuffer), manifest))
			{
				// file,clip,scene,codec,crf,upscaler,truth,frames
				std::vector<std::string> f;
				std::string field;
				for (const char* c = lineBuffer; *c && *c != '\n' && *c != '\r'; ++c)
				{
					if (*c == ',') { f.push_back(field); field.clear(); }
					else field += *c;
				}
				f.push_back(field);
				if (f.size() < 8) continue;
				const double truth = atof(f[6].c_str());
				const int frames = atoi(f[7].c_str());
				FILE* in = nullptr;
				if (fopen_s(&in, (folder + "\\" + f[0]).c_str(), "rb") != 0 || !in) continue;
				++cases;
				const std::string key = f[2] + " " + f[3] + " crf" + f[4] + (f[5].empty() ? "" : " " + f[5]);
				std::vector<uint16_t> y(static_cast<size_t>(W) * H);
				for (int frame = 0; frame < frames; ++frame)
				{
					_fseeki64(in, static_cast<long long>(frame) * W * H * 3, SEEK_SET);
					if (fread(y.data(), 2, y.size(), in) != y.size()) break;
					std::vector<SourceResolution::Spectrum> spectra;
					for (int ty = 0; ty < 3; ++ty)
						for (int tx = 0; tx < 4; ++tx)
						{
							const int X0 = 320 + tx * 860, Y0 = 300 + ty * 620;
							std::vector<float> tile(static_cast<size_t>(kTile) * kTile);
							for (int yy = 0; yy < kTile; ++yy)
								for (int xx = 0; xx < kTile; ++xx)
									tile[static_cast<size_t>(yy) * kTile + xx] = y[static_cast<size_t>(Y0 + yy) * W + X0 + xx] / static_cast<float>(codeMax);
							SourceResolution::Spectrum spectrum;
							SourceResolution::Analyse(tile.data(), kTile, kTile, spectrum, codeMax);
							spectra.push_back(std::move(spectrum));
						}
					SourceResolution::Thresholds t;
					t.codeMaximum = codeMax;
					const SourceResolution::FrameReading r = SourceResolution::ReadFrame(spectra, t);
					Tally& tally = tallies[key];
					++tally.frames;
					const bool native = r.ratio >= 1.0, height = std::fabs(r.ratio - truth) <= 0.01;
					if (truth >= 1.0)
					{
						if (native) ++tally.native;
						else { ++tally.wrong; ++nativeLower; }
					}
					else if (height) ++tally.height;
					else if (native) ++tally.native;
					else { ++tally.wrong; ++wrongHeight; }
					if (!native && !height && failures.size() < 1500)
					{
						char what[160];
						snprintf(what, sizeof(what), "%s f%d reads %.3f (v %.3f@%.3f h %.3f@%.3f); ", f[0].c_str(), frame, r.ratio,
							r.vertical.coherence, r.vertical.ratio, r.horizontal.coherence, r.horizontal.ratio);
						failures += what;
					}
				}
				fclose(in);
			}
			fclose(manifest);
			for (const auto& entry : tallies)
			{
				char row[200];
				snprintf(row, sizeof(row), "%-34s frames %3d: height %3d, native %3d, wrong %d\n", entry.first.c_str(), entry.second.frames,
					entry.second.height, entry.second.native, entry.second.wrong);
				Logger::WriteMessage(row);
			}
			char summary[160];
			snprintf(summary, sizeof(summary), "Real codecs: %d cases; native frames read lower %d; upscales read a wrong height %d\n", cases, nativeLower, wrongHeight);
			Logger::WriteMessage(summary);
			if (!failures.empty()) Logger::WriteMessage(("Wrong: " + failures + "\n").c_str());
			Assert::AreEqual(0, nativeLower, L"no native frame reads lower (the invariant)");
			Assert::AreEqual(0, wrongHeight, L"an upscale reads its height or native, never a wrong height");
		}

		// The tracker: lower only with sustained evidence, native at once.
		TEST_METHOD(TrackerNeedsShotsAndTimeBeforeALowerReading)
		{
			SourceResolution::Tracker tracker;
			double t = 0.0;
			// 10 shots of 4 s each (every shot a new mean level), at 4 readings a second.
			int declaredAt = -1;
			for (int shot = 0; shot < 20; ++shot)
				for (int i = 0; i < 16; ++i, t += 0.25)
				{
					tracker.Update(Frame(0.5, 0.2 + 0.1 * (shot % 2) + 0.001 * i), t);
					if (declaredAt < 0 && tracker.Ratio() < 1.0) declaredAt = static_cast<int>(t);
				}
			Assert::AreEqual(0.5, tracker.Ratio(), 1e-9);
			Assert::AreEqual(1080, tracker.Height(2160));
			Assert::IsTrue(declaredAt >= 20, L"not before 20 s of evidence");
			// One frame of real detail: native at once.
			tracker.Update(Frame(1.0, 0.25), t += 0.25);
			Assert::AreEqual(1.0, tracker.Ratio());
		}

		TEST_METHOD(TrackerKeepsNativeThroughSoftShotsOfARealFilm)
		{
			SourceResolution::Tracker tracker;
			double t = 0.0;
			// Long soft stretches (reading 0.5) with a sharp shot every 90 s.
			for (int minute = 0; minute < 10; ++minute)
				for (int i = 0; i < 360; ++i, t += 0.25)
				{
					const bool sharp = (i % 360) < 8;
					tracker.Update(Frame(sharp ? 1.0 : 0.5, 0.2 + 0.1 * ((i / 40) % 2) + 0.0001 * i), t);
					Assert::AreEqual(1.0, tracker.Ratio(), L"a sharp shot within the window keeps native");
				}
		}

		TEST_METHOD(TrackerDisengagesOnSkipsSourceChangesAndPauses)
		{
			auto engaged = [](SourceResolution::Tracker& tracker, double& t) {
				for (int shot = 0; shot < 20; ++shot)
					for (int i = 0; i < 16; ++i, t += 0.25)
						tracker.Update(Frame(0.5, 0.2 + 0.1 * (shot % 2) + 0.001 * i), t);
				Assert::AreEqual(0.5, tracker.Ratio(), 1e-9);
			};
			SourceResolution::Tracker tracker;
			double t = 0.0;
			engaged(tracker, t);
			// Reported discontinuity (seek, format change, signal loss, resync).
			tracker.Discontinuity("seek");
			Assert::AreEqual(1.0, tracker.Ratio(), L"native after a seek");
			Assert::AreEqual(0.0, tracker.EvidenceSeconds());
			Assert::AreEqual(std::string("seek"), tracker.LastDiscontinuity());
			// Evidence starts again from nothing.
			tracker.Update(Frame(0.5, 0.2), t += 0.25);
			Assert::AreEqual(1.0, tracker.Ratio());

			// Unreported skip: the clock jumps (either way).
			engaged(tracker, t);
			tracker.Update(Frame(0.5, 0.2), t - 30.0);
			Assert::AreEqual(1.0, tracker.Ratio(), L"time going backwards");
			t -= 30.0;
			engaged(tracker, t);
			t += 10.0;
			tracker.Update(Frame(0.5, 0.3), t);
			Assert::AreEqual(1.0, tracker.Ratio(), L"a gap");

			// A pause: the same frame repeated for minutes builds nothing.
			SourceResolution::Tracker paused;
			double p = 0.0;
			for (int shot = 0; shot < 3; ++shot)
				for (int i = 0; i < 16; ++i, p += 0.25)
					paused.Update(Frame(0.5, 0.2 + 0.1 * (shot % 2) + 0.001 * i), p);
			const double before = paused.EvidenceSeconds();
			for (int i = 0; i < 2000; ++i, p += 0.25) paused.Update(Frame(0.5, 0.777), p);   // identical frames
			Assert::IsTrue(paused.EvidenceSeconds() - before < 0.5, L"a repeated frame adds no time");
			for (int i = 0; i < 2000; ++i, p += 0.25) paused.Update(Frame(0.5, 0.1 + 0.0001 * i), p, true);   // flagged repeats
			Assert::IsTrue(paused.EvidenceSeconds() - before < 0.5, L"flagged repeats add no time");
			Assert::AreEqual(1.0, paused.Ratio(), L"a pause never builds a lower reading");

			// Flat / dark frames carry no evidence either.
			SourceResolution::Tracker dark;
			double d = 0.0;
			for (int i = 0; i < 4000; ++i, d += 0.25) dark.Update(Frame(0.5, 0.06 + 0.001 * (i % 50), 0), d);
			Assert::AreEqual(1.0, dark.Ratio());
			Assert::AreEqual(0.0, dark.EvidenceSeconds());
		}

		// Variable-bitrate streams (owner 2026-10-16: "with variable bitrate streams we
		// need to make sure we properly detect"). A starved frame can lose an upscale's
		// coherence and read none found; it must not count as native detail.
		TEST_METHOD(DetailAboveSeparatesStarvedFramesFromNativeDetail)
		{
			const double margin = SourceResolution::Tracker::Settings{}.detailMarginDecades;
			struct K { const char* name; const Kernel* kernel; };
			const K kernels[] = { { "lanczos3", &kLanczos3 }, { "catmull", &kCatmullRom }, { "mitchell", &kMitchell }, { "bilinear", &kBilinear } };
			auto read = [&](double ratio, const Kernel& kernel, double defocus, double grain, double q, int block) {
				return ReadScene([&](unsigned seed) {
					std::vector<float> t = Tile(Scene(seed + 30, defocus), ratio, kernel, grain, 1023, seed);
					CodeBlocks(t, kTile, q, 1023, block, true);
					return t; });
			};
			std::map<double, double> sharpNativeLowest, upscaleHighest;   // per rate in question
			double codingSpread = 0.0;
			// Native 4K, sharp to soft, clean to moderately coded: detail above every rate it could be asked about.
			for (double defocus : { 0.0, 0.6 })
				for (double q : { 0.0, 2.0, 4.0 })
					for (int block : { 8, 32 })
					{
						if (q == 0.0 && block == 32) continue;
						const SourceResolution::FrameReading r = read(1.0, kLanczos3, defocus, 0.5 / 255.0, q, block);
						for (double inQuestion : { 0.5, 1440.0 / 2160, 0.75 })
						{
							const double d = SourceResolution::DetailAbove(r, inQuestion);
							char line[160];
							snprintf(line, sizeof(line), "native defocus %.1f q%.0f b%d: ratio %.3f, detail above %.3f = %.2f decades\n", defocus, q, block, r.ratio, inQuestion, d);
							Logger::WriteMessage(line);
							if (defocus == 0.0)
								sharpNativeLowest[inQuestion] = sharpNativeLowest.count(inQuestion) ? (std::min)(sharpNativeLowest[inQuestion], d) : d;
						}
					}
			// Upscales, any kernel, clean or coded hard: no detail above their own rate.
			for (double ratio : { 0.5, 1440.0 / 2160, 0.75 })
				for (const K& k : kernels)
				{
					double lo = 1e9, hi = -1e9;
					for (double q : { 0.0, 4.0, 12.0 })
					{
						const SourceResolution::FrameReading r = read(ratio, *k.kernel, 0.0, 0.0, q, q > 4.0 ? 32 : 8);
						const double d = SourceResolution::DetailAbove(r, ratio);
						char line[160];
						snprintf(line, sizeof(line), "up %.3f %-8s q%-2.0f: ratio %.3f, detail above %.2f decades\n", ratio, k.name, q, r.ratio, d);
						Logger::WriteMessage(line);
						const double key = ratio == 0.5 ? 0.5 : ratio == 0.75 ? 0.75 : 1440.0 / 2160;
						upscaleHighest[key] = upscaleHighest.count(key) ? (std::max)(upscaleHighest[key], d) : d;
						lo = (std::min)(lo, d);
						hi = (std::max)(hi, d);
					}
					codingSpread = (std::max)(codingSpread, hi - lo);
				}
			for (const auto& entry : upscaleHighest)
			{
				char line[160];
				snprintf(line, sizeof(line), "rate %.3f: sharp native lowest %.2f, upscale highest %.2f (margin %.2f)\n", entry.first, sharpNativeLowest[entry.first], entry.second, margin);
				Logger::WriteMessage(line);
				Assert::IsTrue(sharpNativeLowest[entry.first] > entry.second + margin, L"sharp 4K stands above every upscale at that rate by more than the margin");
			}
			char spread[96];
			snprintf(spread, sizeof(spread), "coding moves an upscale's level by %.3f decades at most\n", codingSpread);
			Logger::WriteMessage(spread);
			Assert::IsTrue(codingSpread < margin, L"coding (to starved) adds no detail above: a starved frame stays at its stream's level");
			// Unknown (no spectra: the tracker tests' frames) is detail.
			Assert::IsTrue(std::isinf(SourceResolution::DetailAbove(Frame(1.0, 0.3), 0.5)));
		}

		TEST_METHOD(VariableBitrateStreamsReadTheirSource)
		{
			// A simulated stream: shots of 6 s, each at its own bitrate (the coder's q from
			// near transparent to starved) and transform size, I-frames every fourth frame
			// coded finer than the P/B frames between, deblocked. Analysed at 2 Hz like VP.
			struct Source { const char* name; double ratio; const Kernel* kernel; };
			const Source sources[] = { { "native", 1.0, &kLanczos3 }, { "1080 catmull", 0.5, &kCatmullRom },
				{ "1080 lanczos", 0.5, &kLanczos3 }, { "1440 bilinear", 1440.0 / 2160, &kBilinear } };
			const double shotQ[] = { 1.0, 6.0, 2.0, 14.0, 4.0, 10.0, 0.5, 16.0, 3.0, 8.0, 12.0, 2.0 };
			const int shotBlock[] = { 8, 16, 8, 32, 16, 32, 8, 32, 8, 16, 32, 8 };
			for (const Source& src : sources)
			{
				SourceResolution::Tracker tracker;
				double t = 0.0;
				int frames = 0, readLower = 0, readNone = 0, wrong = 0;
				double lowestRead = 1.0, highestRead = 0.0;
				int odd = 0;
				double declaredAt = -1.0;
				for (int shot = 0; shot < 12; ++shot)
				{
					std::vector<std::vector<float>> base;
					for (unsigned i = 0; i < 4; ++i)
					{
						std::vector<float> tile = Tile(Scene(200 + 11 * shot + i), src.ratio, *src.kernel, 0.5 / 255.0, 1023, i + 1);
						if (shot % 2) for (float& v : tile) v = (std::min)(1.0f, v + 0.2f);   // a cut between shots (above the frame steps)
						base.push_back(std::move(tile));
					}
					for (int f = 0; f < 12; ++f, t += 0.5)
					{
						const double q = shotQ[shot] * (f % 4 == 0 ? 0.6 : 1.3);
						std::vector<std::vector<float>> tiles = base;
						for (std::vector<float>& tile : tiles)
						{
							// Not a repeat: a step the coder cannot quantise away (a DC step of 4 codes x the block).
							for (float& v : tile) v = (std::min)(1.0f, v + static_cast<float>(4 * f) / 1023.0f);
							CodeBlocks(tile, kTile, q, 1023, shotBlock[shot], true);
						}
						const SourceResolution::FrameReading r = ReadTiles(tiles);
						++frames;
						readLower += r.ratio < 1.0;
						readNone += r.ratio >= 1.0;
						if (r.informative()) { lowestRead = (std::min)(lowestRead, r.ratio); highestRead = (std::max)(highestRead, r.ratio); }
						if (r.informative() && std::fabs(r.ratio - src.ratio) > 0.01 && odd++ < 6)
						{
							char what[96];
							snprintf(what, sizeof(what), "  odd: %s shot %d frame %d q %.1f b%d", src.name, shot, f, q, shotBlock[shot]);
							Log(what, r);
						}
						tracker.Update(r, t);
						const double now = tracker.Ratio();
						if (src.ratio < 1.0 && now < 1.0 && declaredAt < 0) declaredAt = t;
						if (now < 1.0 && std::fabs(now - src.ratio) > 0.01) ++wrong;
					}
				}
				char line[200];
				snprintf(line, sizeof(line), "%-14s frames %d: read lower %d, none found %d (starved %llu), readings %.3f..%.3f; shots %d, evidence %.1f s; declared %s at %.1f s; final %.3f\n",
					src.name, frames, readLower, readNone, tracker.Starved(), lowestRead, highestRead, tracker.Shots(), tracker.EvidenceSeconds(),
					tracker.Ratio() < 1.0 ? "yes" : "no", declaredAt, tracker.Ratio());
				Logger::WriteMessage(line);
				Assert::AreEqual(0, wrong, L"never a wrong lower reading");
				if (src.ratio >= 1.0)
					Assert::AreEqual(1.0, tracker.Ratio(), L"native 4K at any bitrate stays native");
				else
				{
					Assert::IsTrue(declaredAt >= 0.0 && declaredAt <= 45.0, L"an upscaled stream is declared despite its starved shots");
					Assert::AreEqual(src.ratio, tracker.Ratio(), 0.01, L"and stays declared through them");
				}
			}
		}

		TEST_METHOD(StreamSwitchingRenditionsWithoutACut)
		{
			// Adaptive streaming changes rendition mid-shot (no cut): 4K -> the 1080
			// rendition when the bandwidth falls, back when it recovers. Down: the lower
			// reading once the native evidence has left the window; up: native at once.
			auto frame = [&](double ratio, unsigned seed, int f) {
				std::vector<std::vector<float>> tiles;
				for (unsigned i = 0; i < 4; ++i)
				{
					std::vector<float> tile = Tile(Scene(400 + seed * 7 + i), ratio, kCatmullRom, 0.5 / 255.0, 1023, i + 1);
					for (float& v : tile) v = (std::min)(1.0f, v + static_cast<float>((f % 8) + (seed % 2) * 100) / 1023.0f);
					CodeBlocks(tile, kTile, 3.0, 1023, 16, true);
					tiles.push_back(std::move(tile));
				}
				return ReadTiles(tiles);
			};
			const SourceResolution::Tracker::Settings settings;
			SourceResolution::Tracker tracker;
			double t = 0.0;
			// 30 s of the 4K rendition (shots every 6 s).
			for (int i = 0; i < 60; ++i, t += 0.5) tracker.Update(frame(1.0, i / 12, i), t);
			Assert::AreEqual(1.0, tracker.Ratio());
			// The 1080 rendition, the same shots continuing.
			double declaredAt = -1.0;
			const double switchedAt = t;
			for (int i = 60; i < 60 + 2 * static_cast<int>(settings.windowSeconds + 40); ++i, t += 0.5)
			{
				tracker.Update(frame(0.5, i / 12, i), t);
				if (declaredAt < 0 && tracker.Ratio() < 1.0) declaredAt = t;
			}
			char line[160];
			snprintf(line, sizeof(line), "down-switch declared %.1f s after the switch (window %.0f s)\n", declaredAt - switchedAt, settings.windowSeconds);
			Logger::WriteMessage(line);
			Assert::AreEqual(0.5, tracker.Ratio(), 0.01, L"the 1080 rendition is declared");
			Assert::IsTrue(declaredAt - switchedAt <= settings.windowSeconds + settings.minimumSeconds, L"once the native evidence has aged out");
			// Back to 4K: native on its first frame.
			const int shotNow = static_cast<int>((t / 0.5)) / 12;
			tracker.Update(frame(1.0, shotNow, 3), t += 0.5);
			Assert::AreEqual(1.0, tracker.Ratio(), L"the 4K rendition returns native at once");
		}

		TEST_METHOD(DescribeIsGeneral)
		{
			Assert::AreEqual(std::string("native"), SourceResolution::Describe(1.0, 2160));
			Assert::AreEqual(std::string("~1080 (0.50)"), SourceResolution::Describe(0.5, 2160));
			Assert::AreEqual(std::string("~900 (0.42)"), SourceResolution::Describe(900.0 / 2160, 2160));
			Assert::AreEqual(std::string("~540 (0.50)"), SourceResolution::Describe(0.5, 1080), L"from the frame's own height");
		}
	};
}
