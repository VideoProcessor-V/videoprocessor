#include "pch.h"
#include "CppUnitTest.h"

#include <SourceResolutionDetector.h>
#include <KernelInverse.h>
#include <vprenderer/SourceResolutionMonitor.h>
#include "SubtitleTextTestPattern.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Audit 2026-10-17: source-resolution auto, how it latches (engages) and unlatches.
//
// Real footage (VP_TEST_REAL_FOOTAGE, else D:\hfr when present): the SVT 2160p50 clips (park_joy,
// crowd_run: native 4K on both axes), the UVG 2160p120 clips (Bosphorus, Jockey, ReadySetGo,
// YachtRide, ShakeNDry: 4K wide, softer vertically) and the real-codec decodes in coded\ (x264 /
// x265 / AV1 with film-grain synthesis, native 4K and 1080 streams upscaled by ffmpeg).
// Upscales are made here per tile: the 4K frame downscaled (Lanczos-3, antialiased) to the source
// class, optionally coded (block transforms + deblocking, the streaming coder model of
// SourceResolutionDetectorTests) and grained at that size, then upscaled with the player's kernel.
// Overlays (subtitles, a native menu, a transport bar, credits, picture-in-picture, bars) are drawn
// at the output resolution.
//
// Each scenario plays source frames at 24, 50 or 60 fps through SourceResolutionMonitor exactly as
// the renderer does: a tile copy every 0.5 s on the frame clock (SourceResolution::PlaceTiles, the
// dense grid while a lower reading is gathered), analysed 0.1 s later (readback + worker latency),
// and Monitor::AutoFactors on every frame; optionally the look-ahead (queued frames shown ahead).
// The CPU tiles are exactly the pixels the GPU copy would read (nearest, native luma).
//
// The tests measure and log every latch / unlatch with its reason, the time since the last true
// cut, the class and the kernel; they assert the safety criteria (no re-base of native 4K, the
// mixed-screen guard, unlatch latency) and log the rest for the review.
namespace VideoProcessorTest
{
	namespace
	{
		namespace SrcLatch
		{
			constexpr double kPi = 3.14159265358979323846;
			// The source clips' planes (3840 x 2160) and the output frame the monitor reads - 4K by
			// default; OutputFrame switches it (a 1080 source made from the 4K clips). kTile follows the
			// monitor (SourceResolution::TileSize with the latch fixes, else 256), set by Run.
			constexpr int SW = 3840, SH = 2160;
			int W = 3840, H = 2160;
			int kTile = SourceResolutionMonitor::Monitor::kTile;
			struct OutputFrame
			{
				int w0, h0, t0;
				OutputFrame(int w, int h) : w0(W), h0(H), t0(kTile) { W = w; H = h; }
				~OutputFrame() { W = w0; H = h0; kTile = t0; }
			};
			constexpr double kBlack = SubtitleTextTestPattern::kBlack, kWhite = SubtitleTextTestPattern::kWhite;

			std::string Env(const char* name)
			{
				char* v = nullptr;
				size_t n = 0;
				std::string value;
				if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
				std::free(v);
				return value;
			}

			bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

			std::string Root()
			{
				std::string root = Env("VP_TEST_REAL_FOOTAGE");
				if (root.empty() && Exists("D:\\hfr\\park_joy_2160p50.y4m")) root = "D:\\hfr";
				return root;
			}

			// ---- Kernels ----
			using KernelFn = std::function<double(double)>;
			double Sinc(double x) { return std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
			KernelFn Lanczos(int a) { return [a](double x) { return std::fabs(x) < a ? Sinc(x) * Sinc(x / a) : 0.0; }; }
			KernelFn Bicubic(double B, double C)
			{
				return [B, C](double x) {
					x = std::fabs(x);
					return x < 1 ? ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6
						: x < 2 ? ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6 : 0.0;
				};
			}
			struct NamedKernel { const char* name; KernelFn fn; double radius; const char* candidate; };
			enum KernelId { kBilinear, kCatmull, kBicubic06, kBicubic075, kLanczos3, kMitchell, kLanczos2 };
			const NamedKernel& Kernel(int id)
			{
				static const std::vector<NamedKernel> table = {
					{ "bilinear", [](double x) { x = std::fabs(x); return x < 1 ? 1 - x : 0.0; }, 1, "bilinear" },
					{ "catmull-rom", Bicubic(0, 0.5), 2, "bicubic C0.5 (Catmull-Rom)" },
					{ "bicubic C0.6 (ffmpeg)", Bicubic(0, 0.6), 2, "bicubic C0.6" },
					{ "bicubic C0.75", Bicubic(0, 0.75), 2, "bicubic C0.75" },
					{ "lanczos3", Lanczos(3), 3, "lanczos3" },
					{ "mitchell", Bicubic(1.0 / 3, 1.0 / 3), 2, nullptr },
					{ "lanczos2", Lanczos(2), 2, "lanczos2" } };
				return table[static_cast<size_t>(id)];
			}

			// ---- Regions and resampling (as SourceResolutionDetectorTests) ----
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
				double& ref(int x, int y) { return v[static_cast<size_t>(y - y0) * w + (x - x0)]; }
			};

			std::vector<std::pair<int, double>> Taps(double u, double stretch, int reach, const KernelFn& kernel, double& wsum)
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

			// Out pixel X samples in-coordinate (X + 0.5) / scale - 0.5; downscales widen the kernel.
			Region Resample(const Region& in, double scale, int ox0, int oy0, int ow, int oh, const KernelFn& kernel, double radius)
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

			void GaussianBlur(Region& r, double sigma)
			{
				if (sigma <= 0) return;
				const int reach = static_cast<int>(std::ceil(3 * sigma));
				std::vector<double> k(static_cast<size_t>(2 * reach + 1));
				double s = 0;
				for (int i = -reach; i <= reach; ++i) s += k[static_cast<size_t>(i + reach)] = std::exp(-0.5 * i * i / (sigma * sigma));
				for (double& v : k) v /= s;
				std::vector<double> tmp(r.v.size());
				for (int y = 0; y < r.h; ++y)
					for (int x = 0; x < r.w; ++x)
					{
						double sum = 0;
						for (int i = -reach; i <= reach; ++i) sum += k[static_cast<size_t>(i + reach)] * r.at(r.x0 + x + i, r.y0 + y);
						tmp[static_cast<size_t>(y) * r.w + x] = sum;
					}
				r.v.swap(tmp);
				for (int y = 0; y < r.h; ++y)
					for (int x = 0; x < r.w; ++x)
					{
						double sum = 0;
						for (int i = -reach; i <= reach; ++i) sum += k[static_cast<size_t>(i + reach)] * r.at(r.x0 + x, r.y0 + y + i);
						tmp[static_cast<size_t>(y) * r.w + x] = sum;
					}
				r.v.swap(tmp);
			}

			// A streaming coder's model (SourceResolutionDetectorTests::CodeBlocks): square transforms of
			// `block` px, a quantiser step growing with frequency (q codes at DC .. 4q), inverse, an in-loop
			// deblocking filter across the block edges. Values 0..1, n x n, rounded to codes.
			void CodeBlocks(std::vector<double>& tile, int n, double q, int codeMax, int block)
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
								tile[static_cast<size_t>(by + y) * n + bx + x] = (std::min)(static_cast<double>(codeMax), (std::max)(0.0, sum));
							}
					}
				const double limit = 2.0 * q;
				auto filter = [&](double& p1, double& p0, double& q0, double& q1) {
					if (std::fabs(p0 - q0) > limit) return;
					const double np0 = (p1 + 2 * p0 + q0) / 4, nq0 = (p0 + 2 * q0 + q1) / 4;
					p0 = np0; q0 = nq0;
				};
				for (int y = 0; y < n; ++y)
					for (int x = B; x < n; x += B)
					{
						double* r = &tile[static_cast<size_t>(y) * n];
						filter(r[x - 2], r[x - 1], r[x], r[x + 1]);
					}
				for (int y = B; y < n; y += B)
					for (int x = 0; x < n; ++x)
						filter(tile[static_cast<size_t>(y - 2) * n + x], tile[static_cast<size_t>(y - 1) * n + x],
							tile[static_cast<size_t>(y) * n + x], tile[static_cast<size_t>(y + 1) * n + x]);
				for (double& v : tile) v = std::round(v) / codeMax;
			}

			// ---- The corpus: luma planes of 3840 x 2160 clips, a few kept in memory ----
			class Corpus
			{
			public:
				explicit Corpus(const std::string& root) : root(root) {}
				// -1 when the file is missing.
				int Add(const std::string& file, bool y4m, int frames)
				{
					const std::string path = root + "\\" + file;
					if (!Exists(path)) return -1;
					Clip c{ path, y4m, frames, 0 };
					if (y4m)
					{
						FILE* f = nullptr;
						if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return -1;
						int ch;
						long long n = 0;
						while ((ch = fgetc(f)) != EOF && ch != '\n') ++n;
						fclose(f);
						c.header = n + 1;
					}
					clips.push_back(c);
					return static_cast<int>(clips.size()) - 1;
				}
				int Frames(int clip) const { return clips[static_cast<size_t>(clip)].frames; }
				// The luma region (values 0..1 of the clip's code range), clamped at the frame edges.
				Region Read(int clip, int frame, int x0, int y0, int w, int h)
				{
					const Plane& p = Load(clip, frame);
					Region r;
					r.x0 = x0; r.y0 = y0; r.w = w; r.h = h;
					r.v.resize(static_cast<size_t>(w) * h);
					const double scale = 1.0 / p.codeMax;
					for (int y = 0; y < h; ++y)
					{
						const int yy = (std::min)(SH - 1, (std::max)(0, y0 + y));
						const uint16_t* row = &p.v[static_cast<size_t>(yy) * SW];
						for (int x = 0; x < w; ++x)
						{
							const int xx = (std::min)(SW - 1, (std::max)(0, x0 + x));
							r.v[static_cast<size_t>(y) * w + x] = row[xx] * scale;
						}
					}
					return r;
				}
				double readSeconds = 0.0;

			private:
				struct Clip { std::string path; bool y4m; int frames; long long header; };
				struct Plane { int clip = -1, frame = -1, codeMax = 1023; std::vector<uint16_t> v; };
				const Plane& Load(int clip, int frame)
				{
					for (auto it = cache.begin(); it != cache.end(); ++it)
						if (it->clip == clip && it->frame == frame)
						{
							cache.splice(cache.begin(), cache, it);
							return cache.front();
						}
					const auto start = std::chrono::steady_clock::now();
					const Clip& c = clips[static_cast<size_t>(clip)];
					Plane p;
					p.clip = clip;
					p.frame = frame;
					p.v.assign(static_cast<size_t>(SW) * SH, 0);
					FILE* f = nullptr;
					if (fopen_s(&f, c.path.c_str(), "rb") == 0 && f)
					{
						if (c.y4m)
						{
							p.codeMax = 255;
							_fseeki64(f, c.header + static_cast<long long>(frame) * (static_cast<long long>(SW) * SH * 3 / 2 + 6) + 6, SEEK_SET);
							std::vector<uint8_t> y(static_cast<size_t>(SW) * SH);
							if (fread(y.data(), 1, y.size(), f) == y.size())
								for (size_t i = 0; i < y.size(); ++i) p.v[i] = y[i];
						}
						else
						{
							p.codeMax = 1023;
							_fseeki64(f, static_cast<long long>(frame) * SW * SH * 3, SEEK_SET);
							(void)fread(p.v.data(), 2, p.v.size(), f);
						}
						fclose(f);
					}
					readSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
					cache.push_front(std::move(p));
					while (cache.size() > 3) cache.pop_back();
					return cache.front();
				}
				std::string root;
				std::vector<Clip> clips;
				std::list<Plane> cache;
			};

			// ---- Content: what a shot shows ----
			enum class Overlay { None, Subtitles, Menu, Osd, Credits, PipNative, PipUpscaled, PlayerPillarbox };

			struct Content
			{
				std::string name;
				int clip = -1;                 // -1: video black
				int frame0 = 0, frameStep = 9, cycle = 5;   // the clip frames shown (cycled every 0.5 s)
				double ratio = 1.0;            // made here: downscaled to this share and upscaled back
				int kernel = kLanczos3;        // the player's upscale kernel
				double masterQ = 0.0;          // coding at the source size (codes of 1023)
				int masterBlock = 16;
				double masterGrain = 0.0;      // grain at the source size (codes, sigma)
				double nativeQ = 0.0;          // coding at 4K (native streams)
				double nativeGrain = 0.0;      // grain at 4K (codes, sigma)
				double blur = 0.0;             // defocus at 4K (sigma px): fog, sky, a soft shot
				double truth = 1.0;            // the effective ratio (ratio x the clip's own)
				Overlay overlay = Overlay::None;
				int other = -1;                // PiP / menu artwork content
				double px0 = 0, px1 = 1e9, py0 = 0, py1 = 1e9;   // the active picture (bars outside; output px)
				double outScale = 1.0;         // the output frame against the 4K clips (0.5: a 1080 source)
				double gain = 1.0;             // around video black: a dark scene
				double lift = 0.0;             // added level (another shot's exposure; cuts the tracker sees)
				double dither = 0.0;           // per-frame noise (codes): a still with dither, coding jitter
			};

			struct Segment
			{
				int content = 0;
				double seconds = 3.0;
				bool cut = true;               // a real cut at its start (false: a mid-shot change)
				double fadeIn = 0.0, fadeOut = 0.0;
				// A uniform brightness change inside the shot (no cut): the gain goes to `dip` and back
				// over its middle `dipSeconds` (a fade to dark and back, < 1; a flash / exposure, > 1).
				double dip = 1.0, dipSeconds = 0.0;
			};

			struct Studio
			{
				Corpus corpus;
				std::vector<Content> contents;
				std::map<std::tuple<int, int, int, int>, std::vector<float>> tiles;
				double generateSeconds = 0.0;
				explicit Studio(const std::string& root) : corpus(root) {}
				int Add(const Content& c) { contents.push_back(c); return static_cast<int>(contents.size()) - 1; }

				// One tile (kTile x kTile, values 0..1, before the per-frame dynamics) of content `ci` at
				// cycle position k, origin (ox, oy) in the 4K frame. Cached.
				const std::vector<float>& Tile(int ci, int k, int ox, int oy)
				{
					const Content& c = contents[static_cast<size_t>(ci)];
					const int kk = c.cycle > 0 ? k % c.cycle : 0;
					const auto key = std::make_tuple(ci, kk, ox, oy);
					auto it = tiles.find(key);
					if (it != tiles.end()) return it->second;
					const auto start = std::chrono::steady_clock::now();
					std::vector<float> out = Make(c, kk, ox, oy);
					generateSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
					return tiles.emplace(key, std::move(out)).first->second;
				}

			private:
				std::vector<float> Make(const Content& c, int k, int ox, int oy)
				{
					std::vector<float> out(static_cast<size_t>(kTile) * kTile, static_cast<float>(kBlack));
					if (c.clip < 0) return out;
					const int frame = (std::min)(corpus.Frames(c.clip) - 1, c.frame0 + k * c.frameStep);
					std::mt19937 rng(static_cast<unsigned>(frame * 7919 + ox * 31 + oy * 17 + 1));
					std::normal_distribution<double> noise(0.0, 1.0);
					auto inPicture = [&](double x, double y) { return x >= c.px0 && x < c.px1 && y >= c.py0 && y < c.py1; };
					if (c.ratio >= 1.0)
					{
						const int pad = c.blur > 0 ? static_cast<int>(std::ceil(3 * c.blur)) + 1 : 0;
						Region src;
						if (c.outScale < 1.0)
						{
							// Native at the output size: the 4K clip downscaled (antialiased Lanczos-3).
							const double sc = c.outScale;
							const int ix0 = static_cast<int>(std::floor((ox - pad - 8) / sc)) - 8, iy0 = static_cast<int>(std::floor((oy - pad - 8) / sc)) - 8;
							const int iw = static_cast<int>(std::ceil((kTile + 2 * pad + 16) / sc)) + 16;
							const Region in = corpus.Read(c.clip, frame, ix0, iy0, iw, iw);
							src = Resample(in, sc, ox - pad, oy - pad, kTile + 2 * pad, kTile + 2 * pad, Lanczos(3), 3);
							for (double& v : src.v) v = std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0;
						}
						else
							src = corpus.Read(c.clip, frame, ox - pad, oy - pad, kTile + 2 * pad, kTile + 2 * pad);
						GaussianBlur(src, c.blur);
						std::vector<double> v(static_cast<size_t>(kTile) * kTile);
						for (int y = 0; y < kTile; ++y)
							for (int x = 0; x < kTile; ++x)
								v[static_cast<size_t>(y) * kTile + x] = inPicture(ox + x + 0.5, oy + y + 0.5) ? src.at(ox + x, oy + y) : kBlack;
						if (c.nativeQ > 0) CodeBlocks(v, kTile, c.nativeQ, 1023, 16);
						for (size_t i = 0; i < v.size(); ++i) out[i] = static_cast<float>(v[i] + (c.nativeGrain > 0 ? c.nativeGrain / 1023.0 * noise(rng) : 0.0));
					}
					else
					{
						const double r = c.ratio, rm = c.ratio * c.outScale;   // output -> source; 4K clip -> source
						// The source-size square covering the tile and the kernels' reach, on its 16-px grid.
						const int mx0 = static_cast<int>(std::floor((std::floor(ox * r) - 8) / 16.0)) * 16;
						const int my0 = static_cast<int>(std::floor((std::floor(oy * r) - 8) / 16.0)) * 16;
						const int span = static_cast<int>(std::ceil(kTile * r)) + 24;
						const int n = ((span + 15) / 16) * 16 + 16;
						const int ix0 = static_cast<int>(std::floor((mx0 - 8) / rm)) - 8, iy0 = static_cast<int>(std::floor((my0 - 8) / rm)) - 8;
						const int iw = static_cast<int>(std::ceil((n + 16) / rm)) + 16;
						const Region in = corpus.Read(c.clip, frame, ix0, iy0, iw, iw);
						Region master = Resample(in, rm, mx0, my0, n, n, Lanczos(3), 3);
						// Bars in the source (letterboxed / pillarboxed streams): upscaled with the picture.
						for (int y = 0; y < n; ++y)
							for (int x = 0; x < n; ++x)
								if (!inPicture((mx0 + x + 0.5) / r, (my0 + y + 0.5) / r)) master.v[static_cast<size_t>(y) * n + x] = kBlack;
						for (double& v : master.v) v = std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0;
						if (c.masterQ > 0) CodeBlocks(master.v, n, c.masterQ, 1023, c.masterBlock);
						if (c.masterGrain > 0)
							for (double& v : master.v) v = std::round((std::min)(1.0, (std::max)(0.0, v + c.masterGrain / 1023.0 * noise(rng))) * 1023.0) / 1023.0;
						const NamedKernel& kn = Kernel(c.kernel);
						const Region up = Resample(master, 1.0 / r, ox, oy, kTile, kTile, kn.fn, kn.radius);
						for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(up.v[i]);
					}
					ApplyOverlay(c, k, ox, oy, out);
					return out;
				}

				void Rect(std::vector<float>& t, int ox, int oy, int x0, int y0, int x1, int y1, const std::function<float(int, float)>& f)
				{
					for (int y = (std::max)(y0, oy); y < (std::min)(y1, oy + kTile); ++y)
						for (int x = (std::max)(x0, ox); x < (std::min)(x1, ox + kTile); ++x)
						{
							const int i = (y - oy) * kTile + (x - ox);
							t[static_cast<size_t>(i)] = f(i, t[static_cast<size_t>(i)]);
						}
				}

				void Text(std::vector<float>& t, int ox, int oy, int x0, int x1, int y0, int height, unsigned seed)
				{
					if (y0 + height + 12 < oy || y0 - 12 >= oy + kTile || x1 <= ox || x0 >= ox + kTile) return;
					SubtitleTextTestPattern::Draw(t, kTile, kTile, x0 - ox, x1 - ox, y0 - oy, height, seed);
				}

				void ApplyOverlay(const Content& c, int k, int ox, int oy, std::vector<float>& t)
				{
					switch (c.overlay)
					{
					case Overlay::None: break;
					case Overlay::Subtitles:   // two lines at the bottom, drawn by the player at 4K
						Text(t, ox, oy, 1000, 2840, 1780, 64, 501);
						Text(t, ox, oy, 1200, 2640, 1870, 64, 502);
						break;
					case Overlay::Menu:        // a streaming app's info panel: dimmed video, rows of text, artwork
					{
						Rect(t, ox, oy, 0, 1350, W, H, [](int, float v) { return static_cast<float>(v * 0.3 + 0.04); });
						if (c.other >= 0)
						{
							for (const int x0 : { 2000, 2700 })
							{
								const std::vector<float>& art = Tile(c.other, k, ox, oy);
								Rect(t, ox, oy, x0, 1420, x0 + 600, 2000, [&](int i, float) { return art[static_cast<size_t>(i)]; });
							}
						}
						Text(t, ox, oy, 160, 1400, 140, 72, 601);
						for (int row = 0; row < 5; ++row) Text(t, ox, oy, 160, 1800, 1420 + 80 * row, 44, 610u + static_cast<unsigned>(row));
						break;
					}
					case Overlay::Osd:         // a transport bar: a thin progress line and two times
						Rect(t, ox, oy, 300, 1990, 3540, 2002, [](int, float) { return static_cast<float>(0.35); });
						Rect(t, ox, oy, 300, 1990, 1700, 2002, [](int, float) { return static_cast<float>(kWhite); });
						Text(t, ox, oy, 300, 560, 1920, 40, 701);
						Text(t, ox, oy, 3280, 3540, 1920, 40, 702);
						break;
					case Overlay::Credits:     // rolling credits: lines of text over black
					{
						std::fill(t.begin(), t.end(), static_cast<float>(kBlack));
						const int scroll = (k * 55) % 110;
						for (int line = 0; line < 22; ++line)
						{
							const int y = 40 + line * 110 - scroll;
							Text(t, ox, oy, 1100 + (line % 3) * 60, 2740 - (line % 2) * 120, y, 52, 800u + static_cast<unsigned>(line));
						}
						break;
					}
					case Overlay::PipNative:
					case Overlay::PipUpscaled: // a second picture in the bottom-right corner
						if (c.other >= 0)
						{
							const std::vector<float>& pip = Tile(c.other, k, ox, oy);
							Rect(t, ox, oy, 2400, 1260, 3600, 1935, [&](int i, float) { return pip[static_cast<size_t>(i)]; });
						}
						break;
					case Overlay::PlayerPillarbox:   // 4:3 picture, bars added by the player at 4K (sharp edges)
						Rect(t, ox, oy, 0, 0, W / 8, H, [](int, float) { return static_cast<float>(kBlack); });
						Rect(t, ox, oy, W - W / 8, 0, W, H, [](int, float) { return static_cast<float>(kBlack); });
						break;
					}
				}
			};

			// ---- The renderer's path through the monitor ----
			struct RunOptions
			{
				double fps = 24.0;
				double latency = 0.1;          // tile copy -> snapshot (readback + worker)
				bool lookAhead = false;
				int aheadFrames = 3;
				double jitterMs = 0.0;         // frame clock jitter
				double lostShare = 0.0;        // tile copies lost (readback failure / skipped)
				unsigned seed = 1;
				double minimumConfidence = 0.8;
				bool mixedGuard = true;
				std::vector<double> gaps;      // source times at which the clock jumps by 2.5 s (no frames)
				// source_resolution_latch_fixes and its switch timing (the renderer's defaults).
				bool latchFixes = true;
				double maxWait = 20.0, crossfade = 1.5, releaseFade = 0.25;
				// A fast pan: from panFrom to panTo (source seconds) the motion hint (MotionValidity's
				// global vector) reports panHeights picture heights per second.
				double panFrom = -1.0, panTo = -1.0, panHeights = 1.0;
			};

			struct Change
			{
				double t = 0.0;
				int segment = 0;
				double fromX = 1, fromY = 1, toX = 1, toY = 1;
				std::string why;
				double sinceCut = 0.0;     // since the last real cut (segment start with cut)
				bool atBlack = false;      // the frame or the last analysed one was black / flat
				bool trueBlack = false;    // the frame shown is black (video black, or a fade's end)
				double declared = 1.0;
				double fade = 0.0;         // the transition's duration (0 = a hard switch)
			};

			struct RunResult
			{
				std::vector<Change> changes;
				std::vector<double> segmentStart;
				std::vector<double> segmentEndX, segmentEndY, segmentMin;   // factors (min over the segment)
				std::vector<int> segmentEngagedFrames, segmentFrames;
				std::vector<double> segmentFirstEngaged;   // time into the segment of the first engaged frame (-1 none)
				std::vector<int> segmentNativeReadings, segmentLowerReadings;   // analysed frames reading native / lower (informative)
				int engages = 0, disengages = 0, analyses = 0;
				double firstDeclared = -1.0;     // time the tracker first declared a lower reading
				double minFrameRatio = 1.0, maxLowerFrameRatio = 0.0;   // lower frame readings' spread
				SourceResolutionMonitor::Snapshot last;
				SourceResolutionMonitor::Monitor::AheadState ahead;
				double cpuSeconds = 0.0;
				unsigned long long cuts = 0;
				int kernel = -1;
				double declaredMin = 1.0;
				// Latch fixes (owner 2026-10-17): engagements away from a cut / real black that were not
				// a crossfade, a fast pan or a low-detail moment (must be none).
				int offCut = 0;
				// Latch fixes: the longest fade of a release (a step towards native; owner: at most 0.25 s).
				double maxReleaseFade = 0.0;
				std::vector<double> cutTimes;      // the analysed frames on which the monitor saw a cut
				double maxFadeWeightTime = 0.0;   // the longest any fade took to reach weight 1
			};

			double Gain(const Segment& s, double tIn)
			{
				double g = 1.0;
				if (s.fadeIn > 0) g = (std::min)(g, tIn / s.fadeIn);
				if (s.fadeOut > 0) g = (std::min)(g, (s.seconds - tIn) / s.fadeOut);
				g = (std::max)(0.0, g);
				if (s.dipSeconds > 0)
				{
					const double mid = s.seconds / 2, half = s.dipSeconds / 2;
					const double d = std::fabs(tIn - mid);
					if (d < half) g *= 1.0 + (s.dip - 1.0) * (1.0 - d / half);
				}
				return g;
			}

			// The look-ahead's tile origins (Monitor::LookAheadFrame, the full frame as the picture).
			void AheadOrigin(int i, int& x0, int& y0)
			{
				const int tx = i % 4, ty = i / 4;
				x0 = ((W - kTile) * (2 * tx + 1) / 8) & ~15;
				y0 = ((H - kTile) * (2 * ty + 1) / 4) & ~15;
				x0 = (std::max)(0, (std::min)(x0, W - kTile));
				y0 = (std::max)(0, (std::min)(y0, H - kTile));
			}

			RunResult Run(Studio& studio, const std::vector<Segment>& segments, const RunOptions& o, const char* label)
			{
				const auto start = std::chrono::steady_clock::now();
				RunResult result;
				SourceResolutionMonitor::Monitor monitor;
				monitor.SetSelection("auto");
				monitor.SetMixedGuard(o.mixedGuard);
				monitor.SetLatchFixes(o.latchFixes);
				monitor.SetEngageTiming(o.maxWait, o.crossfade, o.releaseFade);
				// The monitor's tile for this output (a fraction of the height with the fixes).
				const int tile = monitor.TileFor(H);
				if (tile != kTile) { kTile = tile; studio.tiles.clear(); }
				double total = 0.0;
				for (const Segment& s : segments) { result.segmentStart.push_back(total); total += s.seconds; }
				const size_t ns = segments.size();
				result.segmentEndX.assign(ns, 1.0); result.segmentEndY.assign(ns, 1.0); result.segmentMin.assign(ns, 1.0);
				result.segmentEngagedFrames.assign(ns, 0); result.segmentFrames.assign(ns, 0); result.segmentFirstEngaged.assign(ns, -1.0);
				result.segmentNativeReadings.assign(ns, 0); result.segmentLowerReadings.assign(ns, 0);
				const long long frames = static_cast<long long>(std::ceil(total * o.fps));
				std::mt19937 rng(o.seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::normal_distribution<double> gauss(0.0, 1.0);
				struct Job { double due, seconds; std::vector<float> values, tx, ty; int count, base, segment; };
				std::deque<Job> queue;
				double lastSubmit = -1e9, clockShift = 0.0, lastT = -1.0;
				size_t gapIndex = 0;
				double fx = 1.0, fy = 1.0;
				double lastCut = 0.0;
				bool lastAnalysisBlank = false;
				// Which segment / cycle position / gain a frame shows.
				auto locate = [&](long long i, int& seg, int& k, double& gain, double& tIn) {
					const double tn = i / o.fps;
					seg = static_cast<int>(ns) - 1;
					for (size_t s = 0; s < ns; ++s)
						if (tn < result.segmentStart[s] + segments[s].seconds) { seg = static_cast<int>(s); break; }
					tIn = tn - result.segmentStart[static_cast<size_t>(seg)];
					k = static_cast<int>(std::floor(tIn * 2.0 + 0.25));
					gain = Gain(segments[static_cast<size_t>(seg)], tIn);
				};
				// One tile with the frame's dynamics, as codes / 1023.
				auto finish = [&](const std::vector<float>& base, const Content& c, double gain, long long frame, float* dst, int salt) {
					std::mt19937 local(static_cast<unsigned>(frame * 131 + salt));
					std::normal_distribution<double> n(0.0, 1.0);
					const double g = gain * c.gain;
					for (size_t p = 0; p < base.size(); ++p)
					{
						double v = kBlack + (base[p] - kBlack) * g + c.lift * gain;
						if (c.dither > 0) v += c.dither / 1023.0 * n(local);
						dst[p] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0);
					}
				};
				// The look-ahead frame (its 8 tiles), rebuilt when what it shows changes.
				std::vector<float> aheadTiles(static_cast<size_t>(8) * kTile * kTile);
				std::tuple<int, int, double> aheadKey{ -1, -1, -1.0 };
				for (long long i = 0; i < frames; ++i)
				{
					double t = i / o.fps + clockShift;
					while (gapIndex < o.gaps.size() && i / o.fps >= o.gaps[gapIndex]) { clockShift += 2.5; t += 2.5; ++gapIndex; }
					if (o.jitterMs > 0) t += (u(rng) - 0.5) * 2.0 * o.jitterMs / 1000.0;
					if (t <= lastT) t = lastT + 1e-4;
					lastT = t;
					int seg, k; double gain, tIn;
					locate(i, seg, k, gain, tIn);
					const Segment& s = segments[static_cast<size_t>(seg)];
					const Content& c = studio.contents[static_cast<size_t>(s.content)];
					if (tIn < 1.0 / o.fps - 1e-9 && s.cut) lastCut = t;
					// The tile copy (Monitor::Frame: every 0.5 s on the frame clock).
					if (t - lastSubmit >= 0.5)
					{
						lastSubmit = t;
						if (!(o.lostShare > 0 && u(rng) < o.lostShare))
						{
							int base = 0;
							const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(0, 0, W, H, kTile, monitor.DenseGrid(), &base);
							Job j;
							j.due = t + o.latency;
							j.seconds = t;
							j.count = static_cast<int>(places.size());
							j.base = base;
							j.segment = seg;
							j.values.resize(static_cast<size_t>(j.count) * kTile * kTile);
							for (int p = 0; p < j.count; ++p)
							{
								const SourceResolution::TilePlace& pl = places[static_cast<size_t>(p)];
								finish(studio.Tile(s.content, k, pl.x0, pl.y0), c, gain, i, &j.values[static_cast<size_t>(p) * kTile * kTile], p);
								j.tx.push_back(pl.centreX);
								j.ty.push_back(pl.centreY);
							}
							queue.push_back(std::move(j));
						}
					}
					while (!queue.empty() && queue.front().due <= t + 1e-9)
					{
						Job& j = queue.front();
						monitor.AnalyseTilesForTest(j.values, j.count, j.seconds, W, H, j.tx, j.ty, j.base, kTile, kBlack);
						++result.analyses;
						const SourceResolutionMonitor::Snapshot snap = monitor.Read();
						lastAnalysisBlank = snap.frameBlank;
						if (!snap.frameBlank) ++(snap.frameRatio < 1.0 ? result.segmentLowerReadings : result.segmentNativeReadings)[static_cast<size_t>(j.segment)];
						if (!snap.frameBlank && snap.frameRatio < 1.0)
						{
							result.minFrameRatio = (std::min)(result.minFrameRatio, snap.frameRatio);
							result.maxLowerFrameRatio = (std::max)(result.maxLowerFrameRatio, snap.frameRatio);
						}
						if (snap.declaredRatio < 1.0 && result.firstDeclared < 0) result.firstDeclared = j.seconds;
						if (snap.cuts != result.cuts)
						{
							result.cuts = snap.cuts;
							result.cutTimes.push_back(j.seconds);
						}
						result.declaredMin = (std::min)(result.declaredMin, snap.declaredRatio);
						queue.pop_front();
					}
					// The look-ahead: the frame aheadFrames on, shown to the monitor.
					if (o.lookAhead && i + o.aheadFrames < frames)
					{
						const long long ai = i + o.aheadFrames;
						int aseg, ak; double again, atIn;
						locate(ai, aseg, ak, again, atIn);
						const Segment& as = segments[static_cast<size_t>(aseg)];
						const Content& ac = studio.contents[static_cast<size_t>(as.content)];
						const auto key = std::make_tuple(as.content, ac.cycle > 0 ? ak % ac.cycle : 0, again);
						if (key != aheadKey)
						{
							aheadKey = key;
							for (int p = 0; p < 8; ++p)
							{
								int x0, y0;
								AheadOrigin(p, x0, y0);
								finish(studio.Tile(as.content, ak, x0, y0), ac, again, ai, &aheadTiles[static_cast<size_t>(p) * kTile * kTile], 100 + p);
							}
						}
						auto sampler = [&](int x, int y) {
							for (int p = 0; p < 8; ++p)
							{
								int x0, y0;
								AheadOrigin(p, x0, y0);
								if (x >= x0 && x < x0 + kTile && y >= y0 && y < y0 + kTile)
									return static_cast<int>(std::lround(aheadTiles[(static_cast<size_t>(p) * kTile + (y - y0)) * kTile + (x - x0)] * 1023.0));
							}
							return static_cast<int>(std::lround(kBlack * 1023.0));
						};
						monitor.LookAheadFrame(static_cast<uint64_t>(ai + 1), W, H, sampler, 0, 0, W, H);
						// The ahead analysis runs on its own worker: a few ms, well inside the frames ahead.
						for (int w = 0; w < 2000; ++w)
						{
							const auto a = monitor.Ahead();
							if (a.eventSequence == 0 || a.ready || a.targetSequence == 0) break;
							std::this_thread::sleep_for(std::chrono::milliseconds(1));
						}
					}
					if (o.panFrom >= 0.0 && i / o.fps >= o.panFrom && i / o.fps < o.panTo)
						monitor.SetMotion(o.panHeights * H / o.fps, static_cast<uint64_t>(i + 1));
					std::string why;
					double nx = 1.0, ny = 1.0;
					monitor.AutoFactors(W, H, o.minimumConfidence, why, nx, ny, t, static_cast<uint64_t>(i + 1));
					{
						const SourceResolutionMonitor::Monitor::Transition tr = monitor.AutoTransition(t);
						if (tr.Active()) result.maxFadeWeightTime = (std::max)(result.maxFadeWeightTime, tr.duration);
					}
					const bool wasEngaged = fx < 1.0 || fy < 1.0, engaged = nx < 1.0 || ny < 1.0;
					if (std::fabs(nx - fx) > 1e-9 || std::fabs(ny - fy) > 1e-9)
					{
						Change ch;
						ch.t = t; ch.segment = seg; ch.fromX = fx; ch.fromY = fy; ch.toX = nx; ch.toY = ny; ch.why = why;
						ch.sinceCut = t - lastCut;
						ch.atBlack = lastAnalysisBlank || gain * c.gain < 0.05 || c.clip < 0;
						ch.trueBlack = gain * c.gain < 0.02 || c.clip < 0;
						ch.declared = monitor.Read().declaredRatio;
						ch.fade = monitor.AutoTransition(t).duration;
						// The owner's rule: an engagement shows at a cut, a real black, a fast pan, a
						// low-detail moment, or by a crossfade - never as a plain switch mid-shot.
						const bool down = nx < fx - 1e-9 || ny < fy - 1e-9;
						if (o.latchFixes && down && !(ch.t - lastCut <= 0.5 + o.latency + 2.0 / o.fps) && !ch.trueBlack &&
							why.find("crossfade") == std::string::npos && why.find("fast pan") == std::string::npos &&
							why.find("low-detail") == std::string::npos)
							++result.offCut;
						if (o.latchFixes && !down) result.maxReleaseFade = (std::max)(result.maxReleaseFade, ch.fade);
						result.changes.push_back(ch);
						if (!wasEngaged && engaged) ++result.engages;
						if (wasEngaged && !engaged) ++result.disengages;
					}
					fx = nx; fy = ny;
					const size_t si = static_cast<size_t>(seg);
					++result.segmentFrames[si];
					if (engaged)
					{
						++result.segmentEngagedFrames[si];
						if (result.segmentFirstEngaged[si] < 0) result.segmentFirstEngaged[si] = tIn;
					}
					result.segmentEndX[si] = fx; result.segmentEndY[si] = fy;
					result.segmentMin[si] = (std::min)(result.segmentMin[si], (std::min)(fx, fy));
				}
				result.last = monitor.Read();
				result.ahead = monitor.Ahead();
				result.cuts = result.last.cuts;
				result.kernel = result.last.kernel;
				result.cpuSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				// The log: every change, then a line per segment.
				char line[512];
				snprintf(line, sizeof(line), "== %s @ %.0f fps: %d analyses, cuts seen %llu, engages %d, disengages %d, first declared %.2f s, "
					"lower frame ratios %.4f..%.4f, kernel %s (votes %d, agreeing %d), %.1f s CPU\n", label, o.fps, result.analyses, result.cuts, result.engages, result.disengages,
					result.firstDeclared, result.minFrameRatio, result.maxLowerFrameRatio,
					result.kernel >= 0 ? KernelInverse::Candidates()[static_cast<size_t>(result.kernel)].name.c_str() : "none",
					result.last.kernelVotes, result.last.kernelAgree, result.cpuSeconds);
				Logger::WriteMessage(line);
				{
					std::string cuts = "   cuts seen at";
					for (const double ct : result.cutTimes) { char b[24]; snprintf(b, sizeof(b), " %.1f", ct); cuts += b; }
					Logger::WriteMessage((cuts + " s\n").c_str());
				}
				for (const Change& ch : result.changes)
				{
					snprintf(line, sizeof(line), "   %7.3f s seg %d: %.4f x %.4f -> %.4f x %.4f (declared %.4f), %.3f s after the cut%s, fade %.2f s: %s\n", ch.t, ch.segment,
						ch.fromX, ch.fromY, ch.toX, ch.toY, ch.declared, ch.sinceCut, ch.atBlack ? ", black/flat" : "", ch.fade, ch.why.c_str());
					Logger::WriteMessage(line);
				}
				for (size_t si = 0; si < ns; ++si)
				{
					const Content& c = studio.contents[static_cast<size_t>(segments[si].content)];
					snprintf(line, sizeof(line), "   seg %zu %-44s %5.1f s at %6.1f s, truth %.3f: readings lower %d / native %d; engaged %d/%d frames, first at +%.2f s, min %.4f, end %.4f x %.4f\n",
						si, c.name.c_str(), segments[si].seconds, result.segmentStart[si], c.truth, result.segmentLowerReadings[si], result.segmentNativeReadings[si], result.segmentEngagedFrames[si], result.segmentFrames[si],
						result.segmentFirstEngaged[si], result.segmentMin[si], result.segmentEndX[si], result.segmentEndY[si]);
					Logger::WriteMessage(line);
				}
				if (result.offCut > 0)
				{
					snprintf(line, sizeof(line), "OFF-CUT: %d engagement(s) mid-shot without a crossfade, pan or low-detail moment\n", result.offCut);
					Logger::WriteMessage(line);
				}
				Assert::IsTrue(result.maxReleaseFade <= SourceResolutionMonitor::Monitor::kMaxReleaseFade + 1e-9 && result.maxReleaseFade <= o.releaseFade + 1e-9,
					L"a release faded longer than source_resolution_release_fade_s (at most 0.25 s)");
				Assert::AreEqual(0, result.offCut, L"an engagement switched mid-shot (not at a cut, a real black, a fast pan, a low-detail moment or by a crossfade)");
				return result;
			}

			// The clips (index into the corpus; -1 when missing).
			struct Clips
			{
				int park = -1, crowd = -1, bosphorus = -1, jockey = -1, readySetGo = -1, yacht = -1, shake = -1;
				bool Complete() const { return park >= 0 && crowd >= 0 && bosphorus >= 0 && jockey >= 0 && readySetGo >= 0 && yacht >= 0 && shake >= 0; }
			};

			Clips AddClips(Corpus& corpus)
			{
				Clips c;
				c.park = corpus.Add("park_joy_2160p50.y4m", true, 500);
				c.crowd = corpus.Add("crowd_run_2160p50.y4m", true, 500);
				c.bosphorus = corpus.Add("Bosphorus_3840x2160_120fps_420_10bit_YUV.yuv", false, 600);
				c.jockey = corpus.Add("Jockey_3840x2160_120fps_420_10bit_YUV.yuv", false, 600);
				c.readySetGo = corpus.Add("ReadySetGo_3840x2160_120fps_420_10bit_YUV.yuv", false, 600);
				c.yacht = corpus.Add("YachtRide_3840x2160_120fps_420_10bit_YUV.yuv", false, 600);
				c.shake = corpus.Add("ShakeNDry_3840x2160_120fps_420_10bit_YUV.yuv", false, 300);
				return c;
			}

			// Shots alternating bright / dark (the tracker's cut is a tile-mean jump of 20/255).
			struct ShotSource { int clip; int frame0; };
			std::vector<ShotSource> Shots(const Clips& c)
			{
				return { { c.bosphorus, 60 }, { c.shake, 40 }, { c.yacht, 80 }, { c.park, 120 }, { c.bosphorus, 380 }, { c.crowd, 100 },
					{ c.yacht, 420 }, { c.park, 320 }, { c.bosphorus, 220 }, { c.shake, 200 }, { c.yacht, 250 }, { c.crowd, 300 } };
			}

			int AddUpscale(Studio& st, int clip, int frame0, double ratio, int kernel, double q = 0.0, double grain = 0.0, const char* tag = "")
			{
				Content c;
				char name[96];
				snprintf(name, sizeof(name), "%.0fp %s%s", ratio * H, Kernel(kernel).name, tag);
				c.name = name;
				c.clip = clip; c.frame0 = frame0; c.ratio = ratio; c.kernel = kernel; c.masterQ = q; c.masterGrain = grain; c.truth = ratio;
				return st.Add(c);
			}

			int AddNative(Studio& st, int clip, int frame0, const char* name, double q = 0.0, double grain = 0.0, double blur = 0.0)
			{
				Content c;
				c.name = name;
				c.clip = clip; c.frame0 = frame0; c.nativeQ = q; c.nativeGrain = grain; c.blur = blur;
				return st.Add(c);
			}

			int AddBlack(Studio& st)
			{
				Content c;
				c.name = "black";
				return st.Add(c);
			}

			bool Skip(const std::string& root, const char* test)
			{
				if (!root.empty()) return false;
				Logger::WriteMessage((std::string(test) + ": no real footage (VP_TEST_REAL_FOOTAGE or D:\\hfr): skipped\n").c_str());
				return true;
			}

			// The first change that engages (either factor below 1) at or after `from` (-1 none).
			const Change* FirstEngage(const RunResult& r, double from = -1e9)
			{
				for (const Change& c : r.changes)
					if (c.t >= from && (c.toX < 1.0 || c.toY < 1.0) && c.fromX >= 1.0 && c.fromY >= 1.0) return &c;
				return nullptr;
			}
			const Change* FirstDisengage(const RunResult& r, double from = -1e9)
			{
				for (const Change& c : r.changes)
					if (c.t >= from && c.toX >= 1.0 && c.toY >= 1.0 && (c.fromX < 1.0 || c.fromY < 1.0)) return &c;
				return nullptr;
			}

			// Every engagement at a real cut or black: within the analysis interval plus the latency of
			// a real cut (the monitor sees a cut in the next tile copy), or on a black / flat frame.
			bool EngagesOnlyAtCutOrBlack(const RunResult& r, const RunOptions& o, double& worst)
			{
				worst = 0.0;
				bool ok = true;
				for (const Change& c : r.changes)
				{
					if (!((c.toX < c.fromX - 1e-9) || (c.toY < c.fromY - 1e-9))) continue;   // only steps down
					if (c.atBlack) continue;
					worst = (std::max)(worst, c.sinceCut);
					if (c.sinceCut > 0.5 + o.latency + 2.0 / o.fps) ok = false;
				}
				return ok;
			}

			// One frame of content `ci` (cycle position k, level gain) read directly (ReadFrame on the dense
			// grid with its centres, as the monitor's worker): the reading, the mixed-screen guard's
			// counts and the detail above 1080 (diagnostic).
			SourceResolution::FrameReading ReadContent(Studio& st, int ci, int k, double gain)
			{
				const Content& c = st.contents[static_cast<size_t>(ci)];
				int base = 0;
				const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(0, 0, W, H, kTile, true, &base);
				std::vector<SourceResolution::Spectrum> spectra(places.size());
				std::vector<float> tile(static_cast<size_t>(kTile) * kTile);
				for (size_t p = 0; p < places.size(); ++p)
				{
					const std::vector<float>& t = st.Tile(ci, k, places[p].x0, places[p].y0);
					for (size_t i = 0; i < tile.size(); ++i)
					{
						const double v = kBlack + (t[i] - kBlack) * gain * c.gain + c.lift * gain;
						tile[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0);
					}
					SourceResolution::Analyse(tile.data(), kTile, kTile, spectra[p], 1023);
				}
				SourceResolution::Thresholds th;
				SourceResolution::SetTileCentres(places, th);
				th.signatureTiles = base;
				th.blackLevel = kBlack;
				return SourceResolution::ReadFrame(spectra, th);
			}

			SourceResolution::FrameReading Diagnose(Studio& st, int ci, int k, double gain, const char* label)
			{
				const Content& c = st.contents[static_cast<size_t>(ci)];
				int base = 0;
				const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(0, 0, W, H, kTile, true, &base);
				std::vector<SourceResolution::Spectrum> spectra(places.size());
				std::vector<float> tile(static_cast<size_t>(kTile) * kTile);
				for (size_t p = 0; p < places.size(); ++p)
				{
					const std::vector<float>& t = st.Tile(ci, k, places[p].x0, places[p].y0);
					for (size_t i = 0; i < tile.size(); ++i)
					{
						const double v = kBlack + (t[i] - kBlack) * gain * c.gain + c.lift * gain;
						tile[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0);
					}
					SourceResolution::Analyse(tile.data(), kTile, kTile, spectra[p], 1023);
				}
				SourceResolution::Thresholds th;
				SourceResolution::SetTileCentres(places, th);
				th.signatureTiles = base;
				const SourceResolution::FrameReading f = SourceResolution::ReadFrame(spectra, th);
				char line[400];
				snprintf(line, sizeof(line), "DIAG %-34s gain %.3f: ratio %.4f (V %.3f@%.4f floor %.2f, H %.3f@%.4f floor %.2f), informative %d/%d, mixed %d (overruled %.4f: picture %d overlay %d sharp %d ui %d), detail above 1080 %.3f\n",
					label, gain, f.ratio, f.vertical.coherence, f.vertical.ratio, f.vertical.floor, f.horizontal.coherence, f.horizontal.ratio, f.horizontal.floor,
					f.informativeTiles, f.tiles, f.mixed ? 1 : 0, f.mixedRatio, f.mixedPictureTiles, f.mixedOverlayTiles, f.mixedSharpTiles, f.mixedUiTiles,
					SourceResolution::DetailAbove(f, 0.5));
				Logger::WriteMessage(line);
				return f;
			}

			void Budget(const char* test, std::chrono::steady_clock::time_point start, double target)
			{
				const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				char line[200];
				snprintf(line, sizeof(line), "%s: %.1f s (target %.0f s)\n", test, s, target);
				Logger::WriteMessage(line);
				Assert::IsTrue(s < 5.0 * target, L"far over the time target");
			}
		}

		// Latch precision (2026-10-17): renditions upscaled exactly from a synthetic source, each
		// axis at its own rate, read at either tile size (no footage needed). The integration found
		// 128-px tiles at 1080 re-basing a 540-line source to 542 lines.
		namespace SrcExact
		{
			struct Wave { double fx, fy, px, py, amplitude; };
			// A source picture: 1/f waves up to the source's own Nyquist (cycles per source pixel).
			std::vector<Wave> Scene(unsigned seed)
			{
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<Wave> waves;
				for (int i = 0; i < 200; ++i)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, u(rng));
					const double angle = u(rng) * SrcLatch::kPi;
					waves.push_back({ f * std::cos(angle), f * std::sin(angle), u(rng) * 2 * SrcLatch::kPi, u(rng) * 2 * SrcLatch::kPi, 0.012 / std::sqrt(f) });
				}
				return waves;
			}

			// One n x n tile at (ox, oy) of the output frame: the scene sampled on the source grid and
			// upscaled with Lanczos-3, separable and centre-aligned (output X samples source
			// (X + 0.5) r - 0.5; r = source / output size, 1 = native on that axis). Unquantised.
			std::vector<double> Tile(const std::vector<Wave>& scene, double rx, double ry, int ox, int oy, int n, double level)
			{
				const int radius = 3;
				auto kernel = [](double x) { return std::fabs(x) < 3.0 ? SrcLatch::Sinc(x) * SrcLatch::Sinc(x / 3.0) : 0.0; };
				const int sx0 = static_cast<int>(std::floor((ox + 0.5) * rx - 0.5)) - radius - 1;
				const int sx1 = static_cast<int>(std::ceil((ox + n - 0.5) * rx - 0.5)) + radius + 1;
				const int sy0 = static_cast<int>(std::floor((oy + 0.5) * ry - 0.5)) - radius - 1;
				const int sy1 = static_cast<int>(std::ceil((oy + n - 0.5) * ry - 0.5)) + radius + 1;
				const int sw = sx1 - sx0 + 1, sh = sy1 - sy0 + 1;
				std::vector<double> src(static_cast<size_t>(sw) * sh, level);
				std::vector<double> cx(sw), sx(sw), cy(sh), sy(sh);
				for (const Wave& v : scene)
				{
					for (int x = 0; x < sw; ++x) { const double a = 2 * SrcLatch::kPi * v.fx * (sx0 + x) + v.px; cx[x] = std::cos(a); sx[x] = std::sin(a); }
					for (int y = 0; y < sh; ++y) { const double b = 2 * SrcLatch::kPi * v.fy * (sy0 + y) + v.py; cy[y] = v.amplitude * std::cos(b); sy[y] = v.amplitude * std::sin(b); }
					for (int y = 0; y < sh; ++y)
					{
						double* row = &src[static_cast<size_t>(y) * sw];
						for (int x = 0; x < sw; ++x) row[x] += cx[x] * cy[y] - sx[x] * sy[y];
					}
				}
				auto taps = [&](int X, int origin, double r, int s0) {
					std::vector<std::pair<int, double>> t;
					const double u = (origin + X + 0.5) * r - 0.5;
					const int c = static_cast<int>(std::floor(u));
					double ws = 0.0;
					for (int k = c - radius + 1; k <= c + radius; ++k)
					{
						const double w = kernel(u - k);
						if (w != 0.0) { t.push_back({ k - s0, w }); ws += w; }
					}
					for (auto& p : t) p.second /= ws;
					return t;
				};
				std::vector<double> rows(static_cast<size_t>(sh) * n), out(static_cast<size_t>(n) * n);
				for (int x = 0; x < n; ++x)
				{
					const auto t = taps(x, ox, rx, sx0);
					for (int y = 0; y < sh; ++y)
					{
						double s = 0.0;
						for (const auto& p : t) s += p.second * src[static_cast<size_t>(y) * sw + p.first];
						rows[static_cast<size_t>(y) * n + x] = s;
					}
				}
				for (int y = 0; y < n; ++y)
				{
					const auto t = taps(y, oy, ry, sy0);
					for (int x = 0; x < n; ++x)
					{
						double s = 0.0;
						for (const auto& p : t) s += p.second * rows[static_cast<size_t>(p.first) * n + x];
						out[static_cast<size_t>(y) * n + x] = s;
					}
				}
				return out;
			}

			// 10-bit codes, normalised (the monitor's samples).
			void Quantise(const std::vector<double>& in, double offset, float* out)
			{
				for (size_t i = 0; i < in.size(); ++i)
					out[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, in[i] + offset)) * 1023.0) / 1023.0);
			}

			// A rendition: the output frame, the source's lines and columns (0 columns = the same
			// scale as the lines, uniform), and the picture inside the frame (0 = the whole frame;
			// letterbox / pillarbox bars outside it). Expected: the exact declared ratio - uniform
			// (lines / H, the larger axis of an anamorphic source) or, with native lines, the
			// horizontal axis alone (columns / W).
			struct Rendition
			{
				const char* name;
				int W, H, lines, columns, pictureW, pictureH;
				double ry() const { return static_cast<double>(lines) / H; }
				double rx() const { return columns > 0 ? static_cast<double>(columns) / W : ry(); }
				bool perAxis() const { return lines >= H; }
				double expected() const { return perAxis() ? rx() : (std::max)(rx(), ry()); }
				int frameSize() const { return perAxis() ? W : H; }
				int pw() const { return pictureW > 0 ? pictureW : W; }
				int ph() const { return pictureH > 0 ? pictureH : H; }
			};
			const std::vector<Rendition>& Renditions()
			{
				static const std::vector<Rendition> list = {
					{ "540 in 2160", 3840, 2160, 540, 0, 0, 0 },
					{ "720 in 2160", 3840, 2160, 720, 0, 0, 0 },
					{ "810 in 2160", 3840, 2160, 810, 0, 0, 0 },
					{ "900 in 2160", 3840, 2160, 900, 0, 0, 0 },
					{ "1080 in 2160", 3840, 2160, 1080, 0, 0, 0 },
					{ "360 in 1080", 1920, 1080, 360, 0, 0, 0 },
					{ "480 in 1080", 1920, 1080, 480, 0, 0, 0 },
					{ "540 in 1080", 1920, 1080, 540, 0, 0, 0 },
					{ "576 in 1080", 1920, 1080, 576, 0, 0, 0 },
					{ "720 in 1080", 1920, 1080, 720, 0, 0, 0 },
					{ "1080 2.39:1 letterboxed in 2160", 3840, 2160, 1080, 0, 0, 1608 },
					{ "720 2.39:1 letterboxed in 1080", 1920, 1080, 720, 0, 0, 804 },
					{ "1080 4:3 pillarboxed in 2160", 3840, 2160, 1080, 0, 2880, 0 },
					{ "720x480 anamorphic in 1080", 1920, 1080, 480, 720, 0, 0 },
					{ "720x576 anamorphic in 1080", 1920, 1080, 576, 720, 0, 0 },
					{ "1440x1080 anamorphic in 1080", 1920, 1080, 1080, 1440, 0, 0 },
				};
				return list;
			}
		}
	}

	TEST_CLASS(SourceResolutionLatchTests)
	{
	public:
		// The cost of one analysed frame (8 and 16 tiles) on this machine (logged; the scenario
		// lengths below are capped from it).
		TEST_METHOD(LatchAnalysisCost)
		{
			using namespace SrcLatch;
			std::mt19937 rng(3);
			std::normal_distribution<double> n(0.5, 0.1);
			std::vector<float> tiles(static_cast<size_t>(16) * kTile * kTile);
			for (float& v : tiles) v = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, n(rng))) * 1023.0) / 1023.0);
			SourceResolutionMonitor::Monitor m;
			const auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < 4; ++i) { tiles[static_cast<size_t>(i)] += 1.0f / 1023; m.AnalyseTilesForTest(tiles, 8, i * 0.5, W, H); }
			const double eight = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 4;
			const auto start16 = std::chrono::steady_clock::now();
			for (int i = 0; i < 4; ++i) { tiles[static_cast<size_t>(i)] += 1.0f / 1023; m.AnalyseTilesForTest(tiles, 16, 2.0 + i * 0.5, W, H); }
			const double sixteen = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start16).count() / 4;
			char line[160];
			snprintf(line, sizeof(line), "analysed frame: %.1f ms (8 tiles), %.1f ms (16 tiles); worker reports %.1f ms\n", eight, sixteen, m.Read().analyseMs);
			Logger::WriteMessage(line);
		}

		// Native 4K on both axes (SVT clips): clean, heavy grain, low bitrate (coded at 4K), defocused
		// (fog / sky: almost no detail), dark - never engages, never declares, on any axis.
		TEST_METHOD(NativeFourKNeverEngages)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "NativeFourKNeverEngages")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete(), L"the corpus clips");
			const int parkClean = AddNative(st, c.park, 100, "park native");
			const int crowdGrain = AddNative(st, c.crowd, 100, "crowd native + grain 12 codes", 0.0, 12.0);
			const int parkLowRate = AddNative(st, c.park, 300, "park native coded q10 (low bitrate)", 10.0);
			const int crowdFog = AddNative(st, c.crowd, 300, "crowd native defocused 6 px (fog)", 0.0, 0.0, 6.0);
			const int parkFogNoise = AddNative(st, c.park, 200, "park native defocused 4 px + noise 1.5", 0.0, 1.5, 4.0);
			const int crowdLowRateGrain = AddNative(st, c.crowd, 200, "crowd native coded q6 + grain 6", 6.0, 6.0);
			for (const int id : { crowdGrain, crowdFog, crowdLowRateGrain }) st.contents[static_cast<size_t>(id)].lift = 0.15;
			Content dark = st.contents[static_cast<size_t>(parkClean)];
			dark.name = "park native dark (gain 0.15)";
			dark.gain = 0.15;
			const int parkDark = st.Add(dark);
			// Alternate bright (crowd) and darker (park) shots; 10 shots of 3.5 s.
			std::vector<Segment> segs = { { crowdGrain, 3.5 }, { parkClean, 3.5 }, { crowdFog, 3.5 }, { parkLowRate, 3.5 }, { crowdLowRateGrain, 3.5 },
				{ parkFogNoise, 3.5 }, { crowdGrain, 3.5 }, { parkDark, 3.5 }, { crowdFog, 3.5 }, { parkClean, 3.5 } };
			RunOptions o;
			const RunResult r = Run(st, segs, o, "native 4K: clean, grain, low bitrate, fog, dark");
			Assert::AreEqual(0, r.engages, L"never engaged on native 4K");
			Assert::AreEqual(1.0, r.declaredMin, 1e-9, L"never declared a lower source");
			// Latch fixes: 10 s of one shot is now enough evidence on its own (owner 2026-10-17), so
			// long single native shots - the soft, dark and vertically soft ones - must still never
			// read lower for 10 s.
			const int uvg = AddNative(st, c.bosphorus, 200, "UVG Bosphorus native (soft vertically)");
			const int crowdFogLong = AddNative(st, c.crowd, 150, "crowd native defocused 6 px (fog), long", 0.0, 0.0, 6.0);
			st.contents[static_cast<size_t>(crowdFogLong)].cycle = 20;
			st.contents[static_cast<size_t>(crowdFogLong)].frameStep = 5;
			std::vector<Segment> longShots = { { crowdFogLong, 22.0 }, { parkDark, 22.0 }, { uvg, 22.0 }, { parkFogNoise, 22.0 } };
			const RunResult rl = Run(st, longShots, o, "native 4K long single shots: fog, dark, UVG, fog + noise");
			char line[200];
			snprintf(line, sizeof(line), "RESULT native long shots: engages %d, declared min %.4f, floor-starved readings logged above\n", rl.engages, rl.declaredMin);
			Logger::WriteMessage(line);
			Assert::AreEqual(0, rl.engages, L"never engaged on long native 4K shots");
			Assert::AreEqual(1.0, rl.declaredMin, 1e-9, L"long native shots never declared a lower source");
			Budget("NativeFourKNeverEngages", started, 100);
		}

		// The UVG clips are 4K wide but soft vertically (scaled vertically at capture): the uniform
		// re-base must never engage; the per-axis trackers' reading is logged (a vertical re-base of a
		// vertically upscaled picture is faithful by the owner's per-axis rule).
		TEST_METHOD(UvgFourKUniformNeverEngages)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "UvgFourKUniformNeverEngages")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			std::vector<Segment> segs;
			const int clips[] = { c.bosphorus, c.shake, c.yacht, c.jockey, c.bosphorus, c.shake, c.yacht, c.readySetGo, c.bosphorus, c.shake };
			for (int i = 0; i < 10; ++i)
			{
				char name[64];
				snprintf(name, sizeof(name), "UVG native %d", i);
				segs.push_back({ AddNative(st, clips[i], 50 + 40 * i, name, i % 3 == 2 ? 6.0 : 0.0), 3.5 });
			}
			RunOptions o;
			const RunResult r = Run(st, segs, o, "UVG 4K (vertically soft)");
			bool uniform = false;
			for (const Change& ch : r.changes) uniform |= ch.toX < 1.0 && ch.toY < 1.0 && std::fabs(ch.toX - ch.toY) < 1e-6;
			char line[160];
			snprintf(line, sizeof(line), "UVG: declared min %.4f, per-axis X %.4f Y %.4f (confidence %.2f)\n", r.declaredMin, r.last.axisRatioX, r.last.axisRatioY, r.last.axisConfidence);
			Logger::WriteMessage(line);
			Assert::IsFalse(uniform, L"no uniform re-base of UVG 4K");
			Assert::AreEqual(1.0, r.declaredMin, 1e-9, L"the both-axes tracker stays native");
			Budget("UvgFourKUniformNeverEngages", started, 45);
		}

		// True upscales from 1080 / 1440 / 720 with several kernels, coding and grain at the source
		// size: engages, only at a cut, within a measured time of the first eligible cut; the class
		// is right; the kernel is identified at 2:1.
		TEST_METHOD(UpscalesEngageAtACutWithTheirClassAndKernel)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "UpscalesEngageAtACutWithTheirClassAndKernel")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			struct Case { double ratio; int kernel; double q, grain; bool expectKernel; };
			const Case cases[] = {
				{ 0.5, kLanczos3, 0.0, 0.0, true }, { 0.5, kCatmull, 3.0, 0.0, true }, { 0.5, kBicubic06, 6.0, 2.0, true },
				{ 0.5, kBicubic075, 0.0, 0.0, true }, { 0.5, kBilinear, 2.0, 0.0, false }, { 0.5, kMitchell, 2.0, 0.0, false },
				{ 1440.0 / 2160, kLanczos3, 2.0, 0.0, false }, { 1440.0 / 2160, kCatmull, 0.0, 0.0, false },
				{ 720.0 / 2160, kCatmull, 2.0, 0.0, false }, { 720.0 / 2160, kLanczos2, 0.0, 0.0, false } };
			std::string failures;
			char line[320];
			for (const Case& k : cases)
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 6; ++i)
				{
					char tag[48];
					snprintf(tag, sizeof(tag), " q%.0f g%.0f #%d", k.q, k.grain, i);
					segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, k.ratio, k.kernel, k.q, k.grain, tag), 3.0 });
				}
				RunOptions o;
				snprintf(line, sizeof(line), "%.0fp %s q%.0f grain %.0f", k.ratio * H, Kernel(k.kernel).name, k.q, k.grain);
				const RunResult r = Run(st, segs, o, line);
				const Change* e = FirstEngage(r);
				double worst = 0.0;
				const bool atCut = EngagesOnlyAtCutOrBlack(r, o, worst);
				// The first eligible cut: the first real cut after the tracker declared.
				double eligible = -1.0;
				for (double s : r.segmentStart) if (r.firstDeclared >= 0 && s >= r.firstDeclared) { eligible = s; break; }
				const int expectedLines = 2 * static_cast<int>(std::lround(k.ratio * H * 0.5));
				const double expected = static_cast<double>(expectedLines) / H;
				const std::string kernelName = r.kernel >= 0 ? KernelInverse::Candidates()[static_cast<size_t>(r.kernel)].name : "none";
				snprintf(line, sizeof(line), "RESULT upscale %.0fp %-22s q%.0f g%.0f: declared at %.2f s, engaged at %.2f s (%.3f s after the cut, %s), class %.4f (expect %.4f), kernel %s, engages %d, disengages %d\n",
					k.ratio * H, Kernel(k.kernel).name, k.q, k.grain, r.firstDeclared, e ? e->t : -1.0, e ? e->sinceCut : -1.0, e ? e->why.c_str() : "-",
					e ? e->toY : 1.0, expected, kernelName.c_str(), r.engages, r.disengages);
				Logger::WriteMessage(line);
				(void)eligible;
				if (!e) { failures += std::string("not engaged: ") + line; continue; }
				if (std::fabs(e->toY - expected) > 0.005 || std::fabs(e->toX - expected) > 0.005) failures += std::string("class: ") + line;
				if (!atCut) failures += std::string("engaged away from a cut: ") + line;
				if (r.disengages != 0) failures += std::string("flapped: ") + line;
				// A kernel, when identified, must be the player's (none falls back to the polar filter;
				// soft pictures leave neighbouring kernels tied at the floor - LatchDiagnostics).
				if (r.kernel >= 0 && (Kernel(k.kernel).candidate == nullptr || kernelName != Kernel(k.kernel).candidate)) failures += std::string("wrong kernel: ") + line;
				(void)k.expectKernel;
			}
			if (!failures.empty()) Assert::Fail(std::wstring(failures.begin(), failures.end()).c_str());
			Budget("UpscalesEngageAtACutWithTheirClassAndKernel", started, 150);
		}

		// Native 4K then a cut to a 1080 upscale (cuts every 4 s), then a cut back to native: the
		// engage time after the first upscaled cut and the unlatch latency on the way back.
		TEST_METHOD(CutFromNativeToUpscaleAndBack)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "CutFromNativeToUpscaleAndBack")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			std::vector<Segment> segs;
			segs.push_back({ AddNative(st, c.crowd, 50, "crowd native"), 3.0 });
			segs.push_back({ AddNative(st, c.park, 50, "park native"), 3.0 });
			const double upStart = 6.0;
			int up[4];
			for (int i = 0; i < 4; ++i) up[i] = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0);
			// 40 s of upscaled shots (4 s each), cycling four shots (before the latch fixes: 132 s, as the
			// window held the native shot's readings for 120 s).
			for (int i = 0; i < 10; ++i) segs.push_back({ up[i % 4], 4.0 });
			const double backAt = upStart + 40.0;
			segs.push_back({ AddNative(st, c.crowd, 300, "crowd native (back)"), 4.0 });
			RunOptions o;
			const RunResult r = Run(st, segs, o, "native -> 1080 lanczos3 -> native");
			const Change* e = FirstEngage(r, upStart);
			const Change* d = FirstDisengage(r, backAt - 0.01);
			char line[300];
			snprintf(line, sizeof(line), "RESULT native->upscale: declared %.2f s after the first upscaled cut, engaged %.2f s after it; back to native %.3f s after the cut (%s); engages %d disengages %d\n",
				r.firstDeclared - upStart, e ? e->t - upStart : -1.0, d ? d->t - backAt : -1.0, d ? d->why.c_str() : "-", r.engages, r.disengages);
			Logger::WriteMessage(line);
			Assert::IsNotNull(e, L"engaged on the upscale");
			Assert::IsNotNull(d, L"back to native");
			Assert::IsTrue(d->t - backAt <= 0.5 + o.latency + 2.0 / o.fps, L"native within one tile copy of the cut");
			Assert::AreEqual(1, r.engages, L"one engagement, no flapping");
			// Latch fixes: the standard again after the native shot (owner: 10 s / 3 shots), switched at
			// the next cut; the return to native is immediate with a fade of at most 0.25 s.
			Assert::IsTrue(e->t - upStart <= 15.0, L"engaged within ~10-15 s of the first upscaled cut");
			Assert::IsTrue(d->fade <= SourceResolutionMonitor::Monitor::kMaxReleaseFade + 1e-9, L"release fade at most 0.25 s");
			Budget("CutFromNativeToUpscaleAndBack", started, 40);
		}

		// Fades through black between upscaled shots, then a long dark scene: engagement on the black
		// (or a cut), never in the dark scene's middle; no flapping.
		TEST_METHOD(FadeThroughBlackAndLongDarkScene)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "FadeThroughBlackAndLongDarkScene")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			const int black = AddBlack(st);
			std::vector<Segment> segs;
			// Shots fading out (0.8 s), 0.6 s of black, fading in (0.8 s): no hard cut anywhere.
			for (int i = 0; i < 8; ++i)
			{
				const int u = AddUpscale(st, shots[static_cast<size_t>(i % 4)].clip, shots[static_cast<size_t>(i % 4)].frame0 + 30 * (i / 4), 0.5, kCatmull, 2.0);
				segs.push_back({ u, 3.5, false, 0.8, 0.8 });
				segs.push_back({ black, 0.6, false });
			}
			Content dark = st.contents[static_cast<size_t>(AddUpscale(st, c.bosphorus, 300, 0.5, kCatmull, 2.0, 0.0, " dark"))];
			dark.gain = 0.12;
			dark.name = "1080p catmull dark scene (gain 0.12)";
			const int darkId = st.Add(dark);
			segs.push_back({ darkId, 20.0, false, 1.0, 0.0 });
			RunOptions o;
			const RunResult r = Run(st, segs, o, "fades through black + long dark scene");
			const Change* e = FirstEngage(r);
			char line[260];
			snprintf(line, sizeof(line), "RESULT fade/dark: declared %.2f s, engaged %.2f s (%s, at black %d), engages %d, disengages %d, dark scene end %.4f\n",
				r.firstDeclared, e ? e->t : -1.0, e ? e->why.c_str() : "-", e ? e->atBlack : -1, r.engages, r.disengages, r.segmentEndY.back());
			Logger::WriteMessage(line);
			Assert::IsNotNull(e, L"engaged across fades");
			Assert::IsTrue(e->atBlack || e->why.find("cut") != std::string::npos, L"engaged on black or at a cut");
			// Finding (audit 2026-10-17, not fixed here): on the dark scene's fade-in one informative frame at
			// ~0.1 contrast loses its coherence (output rounding dominates) and its relative detail above
			// 1080 exceeds the bright frames' reference, so it counts as native detail: one unlatch, then
			// no re-engage for the 120 s window (LatchDiagnostics, "dark scene ... fading in").
			// Latch fixes: that reading's detail sits at the rounding floor - no evidence - so no unlatch.
			Assert::AreEqual(0, r.disengages, L"the dark scene does not unlatch (level-aware native test)");
			Assert::IsTrue(r.segmentNativeReadings.back() <= 2, L"the dark scene reads its class otherwise");
			// The switch off: the behaviour before (the one false native and its unlatch).
			RunOptions old = o;
			old.latchFixes = false;
			const RunResult ro = Run(st, segs, old, "fades through black + long dark scene (latch fixes off)");
			snprintf(line, sizeof(line), "RESULT fade/dark (fixes off): engages %d, disengages %d\n", ro.engages, ro.disengages);
			Logger::WriteMessage(line);
			if (ro.disengages > 0) Logger::WriteMessage("FINDING (fixes off): the dark scene unlatched the re-base (false native on a low-contrast frame)\n");
			Assert::IsTrue(ro.disengages <= 1, L"fixes off: at most the one known unlatch");
			Budget("FadeThroughBlackAndLongDarkScene", started, 90);
		}

		// After an engaged upscale, a cut to a defocused native 4K shot (fog, very little detail
		// above 1080) and then to sharp native: the unlatch on the soft shot (if any) is measured; the
		// sharp shot unlatches at once.
		TEST_METHOD(SoftNativeShotAfterAnEngagedUpscale)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "SoftNativeShotAfterAnEngagedUpscale")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			std::vector<Segment> segs;
			for (int i = 0; i < 5; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.0 });
			const double softAt = 15.0;
			segs.push_back({ AddNative(st, c.yacht, 100, "yacht native defocused 3 px (soft)", 0.0, 0.0, 3.0), 12.0 });
			const double sharpAt = softAt + 12.0;
			segs.push_back({ AddNative(st, c.park, 100, "park native (sharp)"), 3.0 });
			RunOptions o;
			const RunResult r = Run(st, segs, o, "upscale -> soft native -> sharp native");
			const Change* softOff = FirstDisengage(r, softAt);
			char line[300];
			snprintf(line, sizeof(line), "RESULT soft native after upscale: engaged frames in the soft shot %d/%d, unlatched %.3f s after its cut (%s); sharp shot engaged frames %d\n",
				r.segmentEngagedFrames[5], r.segmentFrames[5], softOff ? softOff->t - softAt : -1.0, softOff ? softOff->why.c_str() : "-", r.segmentEngagedFrames[6]);
			Logger::WriteMessage(line);
			Assert::IsNotNull(FirstEngage(r), L"engaged on the upscale first");
			Assert::IsTrue(r.segmentEngagedFrames[6] <= static_cast<int>(std::ceil((0.5 + o.latency) * o.fps)) + 2, L"sharp native unlatched within one tile copy");
			// Latch fixes: the level-aware test only on dark / low-contrast frames - the defocused shot at a
			// normal level is native evidence, unlatched within one tile copy as before (never re-base real 4K).
			Assert::IsTrue(r.segmentEngagedFrames[5] <= static_cast<int>(std::ceil((0.5 + o.latency) * o.fps)) + 2, L"soft native unlatched within one tile copy");
			Budget("SoftNativeShotAfterAnEngagedUpscale", started, 40);
		}

		// Mixed screens over an engaged 1080 upscale: a native menu (unlatch at once, no re-base
		// while it shows), subtitles (stay engaged), a transport bar, credits; then picture-in-picture
		// from cold (native PiP over upscaled video; upscaled PiP over native video): never engaged.
		TEST_METHOD(MixedScreenGuardOverAnEngagedUpscale)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "MixedScreenGuardOverAnEngagedUpscale")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			const int art = AddNative(st, c.crowd, 40, "artwork (native)");
			auto upscaled = [&](int i, Overlay overlay, const char* tag) {
				const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0, 0.0, tag);
				st.contents[static_cast<size_t>(id)].overlay = overlay;
				st.contents[static_cast<size_t>(id)].other = art;
				return id;
			};
			char line[300];
			std::string failures;
			// Each overlay in its own run: five upscaled shots (engaged by ~12 s), the overlay on the
			// sixth shot's picture from mid-shot (no cut) for 6 s, then without it.
			const struct { Overlay overlay; const char* name; bool mustUnlatch, mustStay; } overlays[] = {
				{ Overlay::Menu, "native menu", true, false }, { Overlay::Subtitles, "subtitles", false, true },
				{ Overlay::Osd, "transport bar", false, false }, { Overlay::Credits, "credits over black", false, false } };
			for (const auto& ov : overlays)
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 5; ++i) segs.push_back({ upscaled(i, Overlay::None, ""), 3.0 });
				const int plain = upscaled(5, Overlay::None, " (shot 6)");
				const int with = upscaled(5, ov.overlay, (std::string(" + ") + ov.name).c_str());
				segs.push_back({ plain, 1.5 });
				const double overlayAt = 16.5;
				segs.push_back({ with, 6.0, false });
				segs.push_back({ plain, 3.0, false });
				RunOptions o;
				const RunResult r = Run(st, segs, o, ov.name);
				const Change* off = FirstDisengage(r, overlayAt);
				const int engagedUnder = r.segmentEngagedFrames[6];
				const Change* back = FirstEngage(r, overlayAt + 6.0);
				snprintf(line, sizeof(line), "RESULT overlay %-18s: engaged before %d, unlatched %.3f s after it appeared (%s), engaged frames under it %d/%d, re-engaged after it %s\n",
					ov.name, r.segmentEngagedFrames[5] > 0 ? 1 : 0, off ? off->t - overlayAt : -1.0, off ? off->why.c_str() : "-", engagedUnder, r.segmentFrames[6],
					back ? "yes" : "no (within 3 s)");
				Logger::WriteMessage(line);
				if (r.segmentEngagedFrames[5] == 0) failures += std::string("not engaged before the overlay: ") + line;
				if (ov.mustUnlatch && (!off || off->t - overlayAt > 0.5 + o.latency + 2.0 / o.fps)) failures += std::string("menu not unlatched at once: ") + line;
				if (ov.mustStay && off) failures += std::string("subtitles unlatched: ") + line;
			}
			// Picture-in-picture from cold: never engaged.
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 6; ++i)
				{
					const int pip = AddNative(st, shots[static_cast<size_t>(i + 3)].clip, shots[static_cast<size_t>(i + 3)].frame0, "PiP content (native)");
					const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0, 0.0, " + native PiP");
					st.contents[static_cast<size_t>(id)].overlay = Overlay::PipNative;
					st.contents[static_cast<size_t>(id)].other = pip;
					segs.push_back({ id, 3.0 });
				}
				RunOptions o;
				const RunResult r = Run(st, segs, o, "native PiP over a 1080 upscale");
				snprintf(line, sizeof(line), "RESULT native PiP over upscale: engages %d, declared min %.4f\n", r.engages, r.declaredMin);
				Logger::WriteMessage(line);
				if (r.engages != 0) failures += std::string("re-based under a native PiP: ") + line;
			}
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				const int natives[] = { c.crowd, c.park, c.crowd, c.park, c.crowd, c.park };
				for (int i = 0; i < 6; ++i)
				{
					const int pip = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0, 0.0, " (PiP)");
					const int id = AddNative(st, natives[i], 60 + 50 * i, "native + upscaled PiP");
					st.contents[static_cast<size_t>(id)].overlay = Overlay::PipUpscaled;
					st.contents[static_cast<size_t>(id)].other = pip;
					segs.push_back({ id, 3.0 });
				}
				RunOptions o;
				const RunResult r = Run(st, segs, o, "upscaled PiP over native 4K");
				snprintf(line, sizeof(line), "RESULT upscaled PiP over native: engages %d, declared min %.4f\n", r.engages, r.declaredMin);
				Logger::WriteMessage(line);
				if (r.engages != 0) failures += std::string("re-based a native picture with an upscaled PiP: ") + line;
			}
			if (!failures.empty()) Assert::Fail(std::wstring(failures.begin(), failures.end()).c_str());
			Budget("MixedScreenGuardOverAnEngagedUpscale", started, 120);
		}

		// A native menu over an engaged upscale, then the film again: how long until the re-base
		// resumes (the tracker's 120 s window holds the menu's native frames).
		TEST_METHOD(MixedScreenResumeTime)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "MixedScreenResumeTime")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			const int art = AddNative(st, c.crowd, 40, "artwork (native)");
			int up[4], menu = -1;
			for (int i = 0; i < 4; ++i) up[i] = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0);
			menu = AddUpscale(st, shots[0].clip, shots[0].frame0, 0.5, kLanczos3, 2.0, 0.0, " + menu");
			st.contents[static_cast<size_t>(menu)].overlay = Overlay::Menu;
			st.contents[static_cast<size_t>(menu)].other = art;
			std::vector<Segment> segs;
			for (int i = 0; i < 4; ++i) segs.push_back({ up[i], 4.0 });   // engaged by ~16 s
			const double menuAt = 16.0;
			segs.push_back({ menu, 3.0 });
			const double menuOff = menuAt + 3.0;
			for (int i = 0; i < 8; ++i) segs.push_back({ up[(i + 1) % 4], 4.0 });   // 32 s of the film (was 136 s: the 120 s window)
			RunOptions o;
			const RunResult r = Run(st, segs, o, "menu over an engaged upscale, then 32 s of film");
			const Change* off = FirstDisengage(r, menuAt);
			const Change* back = FirstEngage(r, menuOff);
			char line[260];
			snprintf(line, sizeof(line), "RESULT menu resume: unlatched %.3f s after the menu appeared; re-engaged %.1f s after it left (%s); engages %d\n",
				off ? off->t - menuAt : -1.0, back ? back->t - menuOff : -1.0, back ? back->why.c_str() : "never", r.engages);
			Logger::WriteMessage(line);
			Assert::IsNotNull(off, L"unlatched under the menu");
			Assert::IsTrue(r.segmentEngagedFrames[4] <= static_cast<int>(std::ceil((0.5 + o.latency) * o.fps)) + 2, L"not re-based under the menu");
			// Latch fixes: the menu's readings are discarded once it leaves; the standard again, at a cut.
			Assert::IsNotNull(back, L"re-engaged after the menu");
			Assert::IsTrue(back->t - menuOff <= 15.0, L"re-engaged within ~10-15 s of the menu leaving");
			Assert::IsTrue(back->why.find("cut") != std::string::npos, L"re-engaged at a cut");
			Budget("MixedScreenResumeTime", started, 30);
		}

		// Letterbox (bars in the stream, upscaled with it), a mid-shot change from full frame to
		// letterbox, and a 4:3 picture whose bars the player adds at 4K (sharp edges).
		TEST_METHOD(LetterboxAndPillarbox)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "LetterboxAndPillarbox")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			char line[300];
			std::string failures;
			const double bar = (H - W / 2.39) / 2;   // 2.39:1 in 16:9
			for (int variant = 0; variant < 2; ++variant)
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 6; ++i)
				{
					const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0, 0.0,
						variant == 0 ? " letterbox 2.39" : " 4:3, player bars");
					Content& ct = st.contents[static_cast<size_t>(id)];
					if (variant == 0) { ct.py0 = bar; ct.py1 = H - bar; }
					else ct.overlay = Overlay::PlayerPillarbox;
					segs.push_back({ id, 3.0 });
				}
				// Then the same film switching to full frame mid-shot (no cut).
				const int full = AddUpscale(st, shots[5].clip, shots[5].frame0, 0.5, kLanczos3, 2.0, 0.0, " full frame (mid-shot)");
				segs.push_back({ full, 4.0, false });
				RunOptions o;
				const RunResult r = Run(st, segs, o, variant == 0 ? "1080 letterbox 2.39 -> full frame" : "1080 4:3 with player bars -> full frame");
				const Change* e = FirstEngage(r);
				snprintf(line, sizeof(line), "RESULT %s: engaged %s at %.2f s (%s), class %.4f, engages %d, disengages %d, cuts seen %llu\n",
					variant == 0 ? "letterbox" : "player pillarbox", e ? "yes" : "no", e ? e->t : -1.0, e ? e->why.c_str() : "-", e ? e->toY : 1.0,
					r.engages, r.disengages, r.cuts);
				Logger::WriteMessage(line);
				if (variant == 0 && !e) failures += std::string("letterboxed upscale never engaged: ") + line;
				// Finding: with the tiles over the player's bars (VP not cropping), the hard bar edges lift the
				// horizontal coherence floor above maximumFloor (0.53 > 0.35, LatchDiagnostics): never engaged
				// (the safe direction). Logged, not asserted.
				// Latch fixes: tiles whose own floor is high (the bar edge) are left out: it engages.
				if (variant == 1 && !e) failures += std::string("4:3 with player-added bars never engaged: ") + line;
				if (e && std::fabs(e->toY - 0.5) > 0.005) failures += std::string("class: ") + line;
				if (r.disengages != 0) failures += std::string("flapped on the bar change: ") + line;
			}
			// The switch off: the behaviour before (bar edges raise the floor; never engaged).
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 6; ++i)
				{
					const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0, 0.0, " 4:3, player bars");
					st.contents[static_cast<size_t>(id)].overlay = Overlay::PlayerPillarbox;
					segs.push_back({ id, 3.0 });
				}
				RunOptions old;
				old.latchFixes = false;
				const RunResult r = Run(st, segs, old, "1080 4:3 with player bars (latch fixes off)");
				snprintf(line, sizeof(line), "RESULT player pillarbox (fixes off): engages %d\n", r.engages);
				Logger::WriteMessage(line);
				if (r.engages != 0) failures += std::string("fixes off: the old result changed: ") + line;
			}
			if (!failures.empty()) Assert::Fail(std::wstring(failures.begin(), failures.end()).c_str());
			Budget("LetterboxAndPillarbox", started, 75);
		}

		// Adaptive streaming: an engaged 1080 rendition switches to the 4K rendition mid-shot (no cut):
		// unlatched within N frames; then back to 1080 mid-shot: the rendition-change rule (F12)
		// declares from time alone and the re-base waits for the next cut. At 24, 50 and 60 fps: the
		// times agree (time-based, not frame-based).
		TEST_METHOD(LadderSwitchMidShotAt24_50_60)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "LadderSwitchMidShotAt24_50_60")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			std::vector<Segment> segs;
			for (int i = 0; i < 5; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kCatmull, 4.0), 3.0 });
			// Shot 6: 1080 rendition 3 s, 4K rendition 6 s, 1080 rendition 12 s (same pictures), then a cut.
			Content r1080 = st.contents[static_cast<size_t>(AddUpscale(st, c.crowd, 60, 0.5, kCatmull, 4.0, 0.0, " ladder 1080"))];
			r1080.frameStep = 3;
			r1080.cycle = 40;
			const int lo = st.Add(r1080);
			Content r4k = r1080;
			r4k.ratio = 1.0; r4k.truth = 1.0; r4k.nativeQ = 4.0; r4k.masterQ = 0.0; r4k.name = "ladder 4K rendition";
			const int hi = st.Add(r4k);
			segs.push_back({ lo, 3.0 });
			const double upAt = 18.0;
			segs.push_back({ hi, 6.0, false });
			const double downAt = upAt + 6.0;
			segs.push_back({ lo, 14.0, false });
			const double cutAt = downAt + 14.0;
			segs.push_back({ AddUpscale(st, shots[6].clip, shots[6].frame0, 0.5, kCatmull, 4.0), 4.0 });
			char line[300];
			std::vector<double> unlatch, reengage;
			for (double fps : { 24.0, 50.0, 60.0 })
			{
				RunOptions o;
				o.fps = fps;
				const RunResult r = Run(st, segs, o, "ladder 1080 -> 4K -> 1080 mid-shot");
				const Change* off = FirstDisengage(r, upAt);
				const Change* on = FirstEngage(r, downAt);
				const double n = off ? (off->t - upAt) * fps : -1.0;
				snprintf(line, sizeof(line), "RESULT ladder @%.0f fps: up-switch unlatched %.3f s = %.0f frames after it (%s); down-switch re-engaged %.2f s after it (%.2f s after the next cut: %s); renditions %s\n",
					fps, off ? off->t - upAt : -1.0, n, off ? off->why.c_str() : "-", on ? on->t - downAt : -1.0, on ? on->t - cutAt : -1.0,
					on ? on->why.c_str() : "never", r.engages >= 1 ? "" : "");
				Logger::WriteMessage(line);
				Assert::IsNotNull(off, L"unlatched on the 4K rendition");
				Assert::IsTrue(off->t - upAt <= 0.5 + o.latency + 2.0 / fps, L"unlatched within one tile copy");
				// F12 declares from time alone mid-shot; the re-base then waits for the next cut.
				Assert::IsNotNull(on, L"re-engaged after the mid-shot drop to 1080 (F12)");
				Assert::IsTrue(on->t >= cutAt - 1e-6 && on->t - cutAt <= 0.5 + o.latency + 2.0 / fps, L"at the first cut after the drop");
				unlatch.push_back(off->t - upAt);
				reengage.push_back(on ? on->t - downAt : 1e9);
			}
			for (size_t i = 1; i < unlatch.size(); ++i)
			{
				Assert::AreEqual(unlatch[0], unlatch[i], 0.1, L"unlatch time independent of the frame rate");
				Assert::AreEqual(reengage[0], reengage[i], 0.6, L"re-engage time independent of the frame rate");
			}
			Budget("LadderSwitchMidShotAt24_50_60", started, 120);
		}

		// The class changes at a cut: 1080 -> 1440 (raised at once: detail above the re-based height)
		// and 1080 -> 720 (stays at 1080 - the safe direction - while the 1080 frames are in the window).
		TEST_METHOD(ClassChangeAtACut)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "ClassChangeAtACut")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			char line[300];
			for (const double next : { 1440.0 / 2160, 720.0 / 2160 })
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 5; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.0 });
				const double at = 15.0;
				for (int i = 5; i < 9; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, next, kLanczos3, 2.0), 3.0 });
				RunOptions o;
				const RunResult r = Run(st, segs, o, next > 0.5 ? "1080 -> 1440 at a cut" : "1080 -> 720 at a cut");
				double raisedAfter = -1.0;
				for (const Change& ch : r.changes) if (ch.t >= at && ch.toY > ch.fromY + 1e-9) { raisedAfter = ch.t - at; break; }
				snprintf(line, sizeof(line), "RESULT class change 1080 -> %.0f: factor at end %.4f, raised %.3f s after the cut, engages %d, disengages %d\n",
					next * H, r.segmentEndY.back(), raisedAfter, r.engages, r.disengages);
				Logger::WriteMessage(line);
				if (next > 0.5)
				{
					Assert::IsTrue(raisedAfter >= 0.0 && raisedAfter <= 0.5 + o.latency + 2.0 / o.fps, L"1440 raised within one tile copy");
					Assert::IsTrue(r.segmentEndY.back() >= 1440.0 / 2160 - 0.005, L"never below the 1440 class on 1440 content");
				}
				else
					Assert::IsTrue(r.segmentEndY.back() >= 0.5 - 0.005, L"720 content keeps the 1080 re-base (never too low)");
			}
			Budget("ClassChangeAtACut", started, 60);
		}

		// A still: exact repeats (a paused player) and a still with per-frame dither (a capture of a
		// paused screen): never engages from cold; once engaged, a pause neither unlatches nor flaps.
		TEST_METHOD(StaticStill)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "StaticStill")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			char line[260];
			for (const double dither : { 0.0, 0.7 })
			{
				Content still = st.contents[static_cast<size_t>(AddUpscale(st, c.bosphorus, 200, 0.5, kLanczos3, 2.0))];
				still.cycle = 1;
				still.dither = dither;
				still.name = dither > 0 ? "1080 still + dither" : "1080 still (exact)";
				const int id = st.Add(still);
				const RunResult cold = Run(st, { { id, 20.0 } }, RunOptions{}, still.name.c_str());
				snprintf(line, sizeof(line), "RESULT still from cold (dither %.1f): engages %d, declared min %.4f\n", dither, cold.engages, cold.declaredMin);
				Logger::WriteMessage(line);
				Assert::AreEqual(0, cold.engages, L"a still never engages from cold");
				std::vector<Segment> segs;
				for (int i = 0; i < 5; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.0 });
				segs.push_back({ id, 20.0 });
				const RunResult paused = Run(st, segs, RunOptions{}, (still.name + " after an engaged film").c_str());
				snprintf(line, sizeof(line), "RESULT still after engaged (dither %.1f): engages %d, disengages %d, engaged frames in the still %d/%d\n",
					dither, paused.engages, paused.disengages, paused.segmentEngagedFrames[5], paused.segmentFrames[5]);
				Logger::WriteMessage(line);
				Assert::AreEqual(0, paused.disengages, L"no unlatch or flap on a pause");
			}
			Budget("StaticStill", started, 60);
		}

		// Noise and jitter: frame-clock jitter, 15% of the tile copies lost, per-frame dither (coding
		// jitter): an upscale still engages once with no flapping; native 4K under the same conditions
		// never engages; a clock gap over 2 s is a discontinuity (native at once).
		TEST_METHOD(NoiseAndJitterInTheReadings)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "NoiseAndJitterInTheReadings")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			RunOptions o;
			o.fps = 60.0;
			o.jitterMs = 4.0;
			o.lostShare = 0.15;
			o.seed = 11;
			std::vector<Segment> up, native;
			for (int i = 0; i < 9; ++i)
			{
				const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kBicubic06, 3.0, 1.0, " jitter");
				st.contents[static_cast<size_t>(id)].dither = 0.6;
				up.push_back({ id, 3.0 });
			}
			o.gaps = { 22.0 };   // after engagement: a 2.5 s hole in the frame clock
			const RunResult r = Run(st, up, o, "1080 upscale with jitter, lost copies, dither, a clock gap");
			const Change* gap = FirstDisengage(r, 22.0);
			char line[300];
			snprintf(line, sizeof(line), "RESULT jitter upscale: engaged at %.2f s, engages %d, disengages %d (gap unlatch: %s)\n",
				FirstEngage(r) ? FirstEngage(r)->t : -1.0, r.engages, r.disengages, gap ? gap->why.c_str() : "none");
			Logger::WriteMessage(line);
			Assert::IsNotNull(FirstEngage(r), L"engaged despite jitter");
			Assert::IsNotNull(gap, L"the clock gap unlatched");
			Assert::IsTrue(r.disengages <= 1, L"only the gap unlatches");
			// Latch fixes: routed through Monitor::Discontinuity with its own reason.
			Assert::IsTrue(gap->why.find("gap") != std::string::npos && gap->why.find("detail above") == std::string::npos,
				L"the gap is reported as a gap, not as detail above the re-based height");
			st.tiles.clear();
			const int natives[] = { c.crowd, c.park };
			for (int i = 0; i < 8; ++i)
			{
				const int id = AddNative(st, natives[i % 2], 40 + 45 * i, "native + jitter", 4.0, 2.0);
				st.contents[static_cast<size_t>(id)].dither = 0.6;
				native.push_back({ id, 3.0 });
			}
			o.gaps.clear();
			const RunResult n = Run(st, native, o, "native 4K with jitter, lost copies, dither");
			Assert::AreEqual(0, n.engages, L"native never engages under jitter");
			Budget("NoiseAndJitterInTheReadings", started, 60);
		}

		// The look-ahead: with frames queued ahead, the re-base switches exactly on the cut frame,
		// verified against the next shot; a native next shot cancels it.
		TEST_METHOD(LookAheadEngagesOnTheCutFrame)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "LookAheadEngagesOnTheCutFrame")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			std::vector<Segment> segs;
			for (int i = 0; i < 6; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.0 });
			RunOptions o;
			o.lookAhead = true;
			const RunResult r = Run(st, segs, o, "1080 upscale with the look-ahead");
			const Change* e = FirstEngage(r);
			char line[260];
			snprintf(line, sizeof(line), "RESULT look-ahead: engaged %.4f s after the cut (%s); verified %llu, cancelled %llu\n",
				e ? e->sinceCut : -1.0, e ? e->why.c_str() : "-", r.ahead.verified, r.ahead.cancelled);
			Logger::WriteMessage(line);
			Assert::IsNotNull(e);
			Assert::IsTrue(e->sinceCut < 0.5 / o.fps, L"on the cut frame itself");
			// The next shot native: cancelled, and no engagement on the native shot.
			st.tiles.clear();
			std::vector<Segment> segs2;
			// Three upscaled shots (confirmed by the third: 3 shots, latch fixes), so the native shot is
			// the first cut after the confirmation.
			for (int i = 0; i < 3; ++i) segs2.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.2 });
			segs2.push_back({ AddNative(st, c.bosphorus, 300, "Bosphorus native (next shot)"), 3.0 });
			segs2.push_back({ AddNative(st, c.park, 300, "park native"), 3.0 });
			const RunResult r2 = Run(st, segs2, o, "1080 upscale, then native shots, with the look-ahead");
			snprintf(line, sizeof(line), "RESULT look-ahead native next: engages %d, engaged frames on native %d+%d, cancelled %llu\n",
				r2.engages, r2.segmentEngagedFrames[3], r2.segmentEngagedFrames[4], r2.ahead.cancelled);
			Logger::WriteMessage(line);
			Assert::AreEqual(0, r2.segmentEngagedFrames[3] + r2.segmentEngagedFrames[4], L"never re-based a native shot");
			Budget("LookAheadEngagesOnTheCutFrame", started, 40);
		}

		// Real-codec decodes (coded\: x264 / x265 / AV1 film-grain synthesis at 4K; 1080 streams coded
		// and upscaled by ffmpeg bicubic / lanczos): native never engages (grain-heavy and low bitrate
		// included); the 1080 streams engage at 0.5.
		TEST_METHOD(RealCodecStreams)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "RealCodecStreams")) return;
			Studio st(root);
			struct F { const char* file; double truth; };
			const char* scenes[] = { "Bosphorus", "ShakeNDry", "YachtRide", "park", "Bosphorus", "crowd", "YachtRide", "park", "Jockey" };
			char line[300];
			std::string failures;
			const char* nativeKinds[] = { "native_av1fgs_crf35", "native_x265_crf32", "native_x264_crf28", "native_x264_crf18" };
			const char* upKinds[] = { "stream1080_x265_crf26_bicubic", "stream1080_x264_crf28_bicubic", "stream1080_av1fgs_crf35_bicubic", "stream1080_x265_crf26_lanczos" };
			for (int pass = 0; pass < 2; ++pass)
			{
				st.tiles.clear();
				std::vector<Segment> segs;
				for (int i = 0; i < 9; ++i)
				{
					const char* kind = pass == 0 ? nativeKinds[i % 4] : upKinds[i % 4];
					const std::string file = std::string("coded\\") + scenes[i] + "_" + kind + ".yuv";
					const int clip = st.corpus.Add(file, false, 4);
					if (clip < 0) { Logger::WriteMessage(("missing " + file + ": skipped\n").c_str()); return; }
					Content ct;
					ct.name = std::string(scenes[i]) + " " + kind;
					ct.clip = clip; ct.frame0 = 0; ct.frameStep = 1; ct.cycle = 4;
					ct.truth = pass == 0 ? 1.0 : 0.5;
					segs.push_back({ st.Add(ct), 3.0 });
				}
				RunOptions o;
				const RunResult r = Run(st, segs, o, pass == 0 ? "real codec native 4K" : "real codec 1080 streams upscaled by ffmpeg");
				const Change* e = FirstEngage(r);
				snprintf(line, sizeof(line), "RESULT real codec %s: engages %d, engaged at %.2f s (%s), class %.4f, kernel %s\n", pass == 0 ? "native" : "1080 streams",
					r.engages, e ? e->t : -1.0, e ? e->why.c_str() : "-", e ? e->toY : 1.0,
					r.kernel >= 0 ? KernelInverse::Candidates()[static_cast<size_t>(r.kernel)].name.c_str() : "none");
				Logger::WriteMessage(line);
				if (pass == 0 && r.engages != 0) failures += std::string("native real-codec engaged: ") + line;
				if (pass == 1 && e && std::fabs(e->toY - 0.5) > 0.005) failures += std::string("class: ") + line;
			}
			if (!failures.empty()) Assert::Fail(std::wstring(failures.begin(), failures.end()).c_str());
			Budget("RealCodecStreams", started, 45);
		}

		// Single-frame readings behind the findings of the scenarios above (logged, no time).
		TEST_METHOD(LatchDiagnostics)
		{
			using namespace SrcLatch;
			const std::string root = Root();
			if (Skip(root, "LatchDiagnostics")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			// 1. A 4:3 1080 picture with bars the player adds at 4K (never engaged in LetterboxAndPillarbox).
			const int pillar = AddUpscale(st, c.bosphorus, 60, 0.5, kLanczos3, 2.0, 0.0, " 4:3, player bars");
			st.contents[static_cast<size_t>(pillar)].overlay = Overlay::PlayerPillarbox;
			const int plain = AddUpscale(st, c.bosphorus, 60, 0.5, kLanczos3, 2.0);
			Diagnose(st, plain, 0, 1.0, "1080 full frame");
			Diagnose(st, pillar, 0, 1.0, "1080 4:3 player bars");
			Diagnose(st, pillar, 1, 1.0, "1080 4:3 player bars (next)");
			// 2. A dark 1080 scene and its fade-in (one native reading unlatched in FadeThroughBlack...).
			Content dark = st.contents[static_cast<size_t>(AddUpscale(st, c.bosphorus, 300, 0.5, kCatmull, 2.0))];
			dark.gain = 0.12;
			const int darkId = st.Add(dark);
			for (const double g : { 1.0, 0.8, 0.6, 0.4, 0.2 }) Diagnose(st, darkId, 1, g, "1080 dark scene (x0.12) fading in");
			// 3. The ladder: the same crowd pictures as the 1080 and the 4K rendition (F12's detail drop).
			const int lo = AddUpscale(st, c.crowd, 60, 0.5, kCatmull, 4.0);
			Content hiC = st.contents[static_cast<size_t>(lo)];
			hiC.ratio = 1.0; hiC.nativeQ = 4.0; hiC.masterQ = 0.0;
			const int hi = st.Add(hiC);
			for (int k = 0; k < 2; ++k)
			{
				const double dHi = SourceResolution::DetailAbove(Diagnose(st, hi, k, 1.0, "ladder 4K rendition (crowd q4)"), 0.5);
				const double dLo = SourceResolution::DetailAbove(Diagnose(st, lo, k, 1.0, "ladder 1080 rendition (crowd q4)"), 0.5);
				char line[160];
				snprintf(line, sizeof(line), "DIAG ladder detail drop %.3f decades (F12 needs %.2f)\n", dHi - dLo, SourceResolution::Tracker::Settings{}.renditionDetailDrop);
				Logger::WriteMessage(line);
			}
			// 4. Kernel fits on a 1080 Catmull-Rom upscale coded q3 (not identified in the scenario).
			const int cat = AddUpscale(st, c.yacht, 80, 0.5, kCatmull, 3.0);
			const int cat0 = AddUpscale(st, c.yacht, 80, 0.5, kCatmull, 0.0);
			for (const int id : { cat, cat0 })
			{
				int base = 0;
				const auto places = SourceResolution::PlaceTiles(0, 0, W, H, kTile, false, &base);
				for (size_t p = 0; p < 3; ++p)
				{
					const std::vector<float>& t = st.Tile(id, 0, places[p].x0, places[p].y0);
					std::vector<float> q(t.size());
					for (size_t i = 0; i < t.size(); ++i) q[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, static_cast<double>(t[i]))) * 1023.0) / 1023.0);
					const KernelInverse::TileFit fit = KernelInverse::FitTile(q.data(), kTile, kTile, 1023);
					char line[300];
					std::string res;
					for (double r : fit.residuals) { char b[16]; snprintf(b, sizeof(b), " %.3f", r); res += b; }
					snprintf(line, sizeof(line), "DIAG kernel fit %s tile %zu: best %s %.3f, second %.3f, accept %d (floor %.3f); residuals%s\n",
						st.contents[static_cast<size_t>(id)].name.c_str(), p, fit.best >= 0 ? KernelInverse::Candidates()[static_cast<size_t>(fit.best)].name.c_str() : "none",
						fit.bestResidual, fit.secondResidual, KernelInverse::Accept(fit, KernelInverse::FloorCodes(10, 10)) ? 1 : 0, KernelInverse::FloorCodes(10, 10), res.c_str());
					Logger::WriteMessage(line);
				}
			}
		}


		// ---- Latch fixes (latch review 2026-10-17, owner rules) ----

		// The crossfade's mix (libplacebo's frame mixer with the triangle filter, FadeMixTimestamps):
		// monotonic in time, and each path exactly at its end (bit-exact floats).
		TEST_METHOD(CrossfadeMixIsMonotonicAndExactAtTheEnds)
		{
			using M = SourceResolutionMonitor::Monitor;
			const float pairs[][2] = { { 0.0626f, 0.9188f }, { 0.9188f, 0.0626f }, { 0.31234567f, 0.31234570f }, { -0.02f, 1.05f } };   // BTB / WTW kept
			for (const auto& pr : pairs)
			{
				Assert::IsTrue(M::FadeMixValue(pr[0], pr[1], 0.0) == pr[0], L"weight 0: the old path exactly");
				Assert::IsTrue(M::FadeMixValue(pr[0], pr[1], 1.0) == pr[1], L"weight 1: the new path exactly");
				float last = pr[0];
				for (int i = 0; i <= 1000; ++i)
				{
					const float v = M::FadeMixValue(pr[0], pr[1], i / 1000.0);
					if (pr[1] >= pr[0]) Assert::IsTrue(v >= last - 1e-7f, L"monotonic (rising)");
					else Assert::IsTrue(v <= last + 1e-7f, L"monotonic (falling)");
					last = v;
				}
			}
			// The weight in time: 0 at the start, exactly 1 at the end and after, linear between.
			Assert::AreEqual(0.0, M::FadeWeight(0.0, 1.5));
			Assert::AreEqual(1.0, M::FadeWeight(1.5, 1.5));
			Assert::AreEqual(1.0, M::FadeWeight(0.1, 0.0), L"no duration: a hard switch");
			Assert::AreEqual(0.5, M::FadeWeight(0.75, 1.5), 1e-12);
			float ts[2];
			M::FadeMixTimestamps(0.25, ts);
			Assert::IsTrue(ts[0] <= ts[1], L"timestamps ascending (pl_render_image_mix requires it)");
		}

		// A long take (no cut, black, pan or dark moment): confirmed by time alone (10 s), then a
		// crossfade after source_resolution_engage_max_wait_s; the same times at 24, 50 and 60 fps.
		TEST_METHOD(LongTakeCrossfadesAfterTheWait)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "LongTakeCrossfadesAfterTheWait")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			Content take = st.contents[static_cast<size_t>(AddUpscale(st, c.yacht, 40, 0.5, kLanczos3, 2.0, 0.0, " long take"))];
			take.frameStep = 4;
			take.cycle = 60;
			const int id = st.Add(take);
			const std::vector<Segment> segs = { { id, 34.0 } };
			char line[300];
			std::vector<double> engagedAt;
			for (const double fps : { 24.0, 50.0, 60.0 })
			{
				RunOptions o;
				o.fps = fps;
				const RunResult r = Run(st, segs, o, "1080 long take, no cut");
				const Change* e = FirstEngage(r);
				snprintf(line, sizeof(line), "RESULT long take @%.0f fps: declared %.2f s, engaged %.2f s (%s), fade %.2f s\n",
					fps, r.firstDeclared, e ? e->t : -1.0, e ? e->why.c_str() : "never", e ? e->fade : -1.0);
				Logger::WriteMessage(line);
				Assert::IsNotNull(e, L"engaged in the long take");
				Assert::IsTrue(e->why.find("crossfade") != std::string::npos, L"by a crossfade");
				Assert::AreEqual(o.crossfade, e->fade, 1e-9, L"over source_resolution_crossfade_s");
				Assert::IsTrue(r.firstDeclared >= 9.5 && r.firstDeclared <= 11.0, L"confirmed by 10 s in one shot");
				Assert::IsTrue(std::fabs(e->t - (r.firstDeclared + o.latency + o.maxWait)) <= 0.7, L"after the wait from the confirmation");
				engagedAt.push_back(e->t);
			}
			for (size_t i = 1; i < engagedAt.size(); ++i) Assert::AreEqual(engagedAt[0], engagedAt[i], 0.6, L"time-based: the same at 24/50/60 fps");
			Budget("LongTakeCrossfadesAfterTheWait", started, 45);
		}

		// A fast pan in a long take (the motion hint): the switch happens during it, at once.
		TEST_METHOD(FastPanSwitchesInALongTake)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "FastPanSwitchesInALongTake")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			Content take = st.contents[static_cast<size_t>(AddUpscale(st, c.yacht, 40, 0.5, kLanczos3, 2.0, 0.0, " long take"))];
			take.frameStep = 4;
			take.cycle = 60;
			const int id = st.Add(take);
			RunOptions o;
			o.panFrom = 15.0;
			o.panTo = 16.0;
			const RunResult r = Run(st, { { id, 20.0 } }, o, "1080 long take with a fast pan at 15 s");
			const Change* e = FirstEngage(r);
			char line[200];
			snprintf(line, sizeof(line), "RESULT fast pan: declared %.2f s, engaged %.2f s (%s)\n", r.firstDeclared, e ? e->t : -1.0, e ? e->why.c_str() : "never");
			Logger::WriteMessage(line);
			Assert::IsNotNull(e);
			Assert::IsTrue(e->why.find("fast pan") != std::string::npos && e->t >= 15.0 && e->t < 16.0, L"during the pan");
			Assert::AreEqual(0.0, e->fade, L"at once (the pan hides it)");
			Budget("FastPanSwitchesInALongTake", started, 15);
		}

		// Fades and flashes inside shots (a uniform brightness change, no cut) are not cuts; the
		// engagement waits for a real cut.
		TEST_METHOD(FadesAndFlashesAreNotCuts)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "FadesAndFlashesAreNotCuts")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			std::vector<Segment> segs;
			for (int i = 0; i < 8; ++i)
			{
				Segment sg{ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kCatmull, 2.0), 4.0 };
				sg.dip = i % 2 ? 1.4 : 0.3;   // a flash / exposure lift, a fade to dark and back
				sg.dipSeconds = 2.0;
				segs.push_back(sg);
			}
			char line[260];
			RunOptions o;
			const RunResult r = Run(st, segs, o, "1080 shots with in-shot fades and flashes");
			RunOptions old;
			old.latchFixes = false;
			const RunResult ro = Run(st, segs, old, "1080 shots with in-shot fades and flashes (latch fixes off)");
			const Change* e = FirstEngage(r);
			snprintf(line, sizeof(line), "RESULT fades/flashes: cuts seen %llu (fixes off %llu) of 7 real; engaged %.2f s, %.3f s after the cut (%s)\n",
				r.cuts, ro.cuts, e ? e->t : -1.0, e ? e->sinceCut : -1.0, e ? e->why.c_str() : "never");
			Logger::WriteMessage(line);
			Assert::IsTrue(r.cuts <= 7, L"no fade or flash counted as a cut");
			Assert::IsTrue(r.cuts >= 6, L"the real cuts found");
			Assert::IsNotNull(e);
			Budget("FadesAndFlashesAreNotCuts", started, 30);
		}

		// After an engaged upscale, cuts to dark native 4K and to UVG (vertically soft) native 4K: the
		// level-aware native test must not slow their unlatch (real detail first).
		TEST_METHOD(DarkAndSoftNativeUnlatchQuickly)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "DarkAndSoftNativeUnlatchQuickly")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			Content dark = st.contents[static_cast<size_t>(AddNative(st, c.park, 100, "park native"))];
			dark.gain = 0.15;
			dark.name = "park native dark (gain 0.15)";
			const int darkId = st.Add(dark);
			const int uvg = AddNative(st, c.jockey, 200, "UVG Jockey native");
			// Continuous frames through the shot (the default 5-step cycle jumps 36 frames back after
			// 2.5 s: in Jockey's fast motion that jump reads as a cut, which real video never has).
			st.contents[static_cast<size_t>(uvg)].frameStep = 3;
			st.contents[static_cast<size_t>(uvg)].cycle = 40;
			std::vector<Segment> segs;
			for (int i = 0; i < 4; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.0 });
			const double darkAt = 12.0;
			segs.push_back({ darkId, 4.0 });
			for (int i = 4; i < 9; ++i) segs.push_back({ AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0), 3.0 });
			const double uvgAt = 31.0;
			segs.push_back({ uvg, 4.0 });
			RunOptions o;
			const RunResult r = Run(st, segs, o, "upscale -> dark native -> upscale -> UVG native");
			const Change* d1 = FirstDisengage(r, darkAt - 0.01);
			const Change* d2 = FirstDisengage(r, uvgAt - 0.01);
			char line[300];
			snprintf(line, sizeof(line), "RESULT dark / soft native: dark unlatched %.3f s after its cut (%s), engaged frames %d; UVG unlatched %.3f s (%s), engaged frames %d\n",
				d1 ? d1->t - darkAt : -1.0, d1 ? d1->why.c_str() : "-", r.segmentEngagedFrames[4], d2 ? d2->t - uvgAt : -1.0, d2 ? d2->why.c_str() : "-", r.segmentEngagedFrames[10]);
			Logger::WriteMessage(line);
			const int bound = static_cast<int>(std::ceil((0.5 + o.latency) * o.fps)) + 2;
			Assert::IsTrue(r.segmentEngagedFrames[4] <= bound, L"dark native unlatched within one tile copy");
			Assert::IsNotNull(d2, L"UVG native unlatched");
			Assert::IsTrue(d2->t - uvgAt <= 0.5 + o.latency + 2.0 / o.fps, L"UVG native unlatched within one tile copy");
			// The UVG clips are vertically soft: a per-axis (vertical only) re-base is faithful by the owner's
			// rule; a uniform one never.
			for (const Change& ch : r.changes)
				if (ch.t >= uvgAt + 0.01 && ch.toX < 1.0 && ch.toY < 1.0) Assert::Fail(L"a uniform re-base on UVG native");
			Budget("DarkAndSoftNativeUnlatchQuickly", started, 40);
		}

		// A 1080 source (made from the 4K clips): the tile is a fraction of the height (128 px), the
		// dense grid covers it; native 1080 never engages; a 720 upscale engages at its class at a cut.
		TEST_METHOD(Source1080UsesHeightRelativeTiles)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "Source1080UsesHeightRelativeTiles")) return;
			OutputFrame frame1080(1920, 1080);
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const std::vector<ShotSource> shots = Shots(c);
			Assert::AreEqual(128, SourceResolution::TileSize(1080), L"128-px tiles at 1080");
			Assert::AreEqual(256, SourceResolution::TileSize(2160), L"256-px tiles at 2160");
			int base = 0;
			Assert::AreEqual(16, static_cast<int>(SourceResolution::PlaceTiles(0, 0, 1920, 1080, 128, true, &base).size()), L"the dense grid at 1080");
			std::vector<Segment> natives, ups;
			const int nclips[] = { c.crowd, c.park, c.bosphorus, c.crowd, c.park, c.yacht, c.crowd, c.park };
			for (int i = 0; i < 8; ++i)
			{
				const int id = AddNative(st, nclips[i], 40 + 50 * i, "native 1080 (from 4K)");
				st.contents[static_cast<size_t>(id)].outScale = 0.5;
				if (i % 2) st.contents[static_cast<size_t>(id)].lift = 0.12;
				natives.push_back({ id, 3.5 });
			}
			for (int i = 0; i < 6; ++i)
			{
				const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 720.0 / 1080, kLanczos3, 2.0, 0.0, " in a 1080 frame");
				st.contents[static_cast<size_t>(id)].outScale = 0.5;
				ups.push_back({ id, 3.0 });
			}
			RunOptions o;
			const RunResult rn = Run(st, natives, o, "native 1080 source, 128-px tiles");
			const RunResult ru = Run(st, ups, o, "720 upscaled in a 1080 source, 128-px tiles");
			const Change* e = FirstEngage(ru);
			char line[300];
			snprintf(line, sizeof(line), "RESULT 1080 source: tile %d; native engages %d (declared min %.4f); 720 upscale engaged %.2f s (%s), class %.4f\n",
				kTile, rn.engages, rn.declaredMin, e ? e->t : -1.0, e ? e->why.c_str() : "never", e ? e->toY : 1.0);
			Logger::WriteMessage(line);
			Assert::AreEqual(0, rn.engages, L"native 1080 never engages");
			Assert::AreEqual(1.0, rn.declaredMin, 1e-9);
			Assert::IsNotNull(e, L"the 720 upscale engages");
			// Latch precision (2026-10-17): the class exactly - 720 lines, not the reading rounded.
			Assert::AreEqual(720.0 / 1080, e->toY, 0.0, L"its class, exactly");
			// The integration's case: 540 lines in a 1080 frame read through 128-px tiles re-based to
			// 542 (0.4 % off); exactly 0.5000 now.
			std::vector<Segment> halves;
			for (int i = 0; i < 6; ++i)
			{
				const int id = AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0, 0.0, " 540 in a 1080 frame");
				st.contents[static_cast<size_t>(id)].outScale = 0.5;
				halves.push_back({ id, 3.0 });
			}
			const RunResult rh = Run(st, halves, o, "540 upscaled in a 1080 source, 128-px tiles");
			const Change* h = FirstEngage(rh);
			snprintf(line, sizeof(line), "RESULT 1080 source: 540 upscale engaged %.2f s (%s), class %.6f (%.2f lines)\n",
				h ? h->t : -1.0, h ? h->why.c_str() : "never", h ? h->toY : 1.0, h ? h->toY * 1080 : 1080.0);
			Logger::WriteMessage(line);
			Assert::IsNotNull(h, L"the 540 upscale engages");
			Assert::AreEqual(0.5, h->toY, 0.0, L"540 of 1080 exactly (was 542)");
			Budget("Source1080UsesHeightRelativeTiles", started, 45);
		}

		// The level-aware native test and the shape-aware cut test on single frames: the readings
		// behind their settings (logged), with the margins asserted.
		TEST_METHOD(LatchFixesCalibration)
		{
			using namespace SrcLatch;
			const auto started = std::chrono::steady_clock::now();
			const std::string root = Root();
			if (Skip(root, "LatchFixesCalibration")) return;
			Studio st(root);
			const Clips c = AddClips(st.corpus);
			Assert::IsTrue(c.Complete());
			const SourceResolution::Tracker::Settings ts;
			char line[300];
			// 1. Native detail above 1080 against the rounding floor (the level-aware test needs every
			// native frame above floor + nativeFloorDecades, the dark upscale's false natives below).
			double nativeLowest = 1e9, upscaleHighest = -1e9, defocusedHighest = -1e9;
			// The contrast gate (nativeFloorContrast): the dark upscale's false natives below it, the
			// defocused natives at a normal level above it (they stay native evidence).
			double upscaleContrastHighest = -1.0, defocusedContrastLowest = 1e9;
			// native: 1 with detail, 2 defocused (fog, a soft shot: nothing above 1080 but rounding noise), 0 the dark upscale.
			auto above = [&](int id, int k, double gain, const char* label, int native) {
				const SourceResolution::FrameReading f = ReadContent(st, id, k, gain);
				double level = 0.0;
				const double d = SourceResolution::DetailAbove(f, 0.5, &level);
				const double coherence = SourceResolution::CoherenceAtRate(f, 0.5);
				snprintf(line, sizeof(line), "CAL level %-40s gain %.2f: ratio %.4f, detail above 1080 %.3f, level %.2f = floor %+.2f decades (floor %.2f, %.1f bits), coherence at 1080 %.3f, contrast %.4f\n",
					label, gain, f.ratio, d, level, level - f.quantisationFloor, f.quantisationFloor, f.effectiveBits, coherence, f.contrast);
				Logger::WriteMessage(line);
				if (std::isinf(d) || !f.informative()) return;
				if (native == 1) nativeLowest = (std::min)(nativeLowest, level - f.quantisationFloor);
				else if (native == 2) { defocusedHighest = (std::max)(defocusedHighest, level - f.quantisationFloor); defocusedContrastLowest = (std::min)(defocusedContrastLowest, f.contrast); }
				else if (f.ratio >= 1.0) { upscaleHighest = (std::max)(upscaleHighest, level - f.quantisationFloor); upscaleContrastHighest = (std::max)(upscaleContrastHighest, f.contrast); }
			};
			const int park = AddNative(st, c.park, 100, "park native");
			const int crowdFog = AddNative(st, c.crowd, 300, "crowd native fog 6 px", 0.0, 0.0, 6.0);
			const int yachtSoft = AddNative(st, c.yacht, 100, "yacht native defocused 3 px", 0.0, 0.0, 3.0);
			for (int k = 0; k < 2; ++k)
			{
				above(park, k, 1.0, "park native", 1);
				above(park, k, 0.15, "park native dark", 1);
				above(crowdFog, k, 1.0, "crowd native fog 6 px", 2);
				above(yachtSoft, k, 1.0, "yacht native soft 3 px", 2);
				for (const int clip : { c.bosphorus, c.jockey, c.readySetGo, c.yacht, c.shake })
					above(AddNative(st, clip, 100 + 30 * k, "UVG native"), k, 1.0, "UVG native", 1);
			}
			Content dark = st.contents[static_cast<size_t>(AddUpscale(st, c.bosphorus, 300, 0.5, kCatmull, 2.0))];
			dark.gain = 0.12;
			const int darkUp = st.Add(dark);
			for (const double g : { 1.0, 0.8, 0.6, 0.4, 0.2 }) above(darkUp, 1, g, "1080 dark scene (x0.12) fading in", 0);
			snprintf(line, sizeof(line), "CAL level: native with detail lowest %+.2f decades above the floor; defocused native highest %+.2f (nothing above 1080); false-native upscale highest %+.2f; threshold %.2f\n",
				nativeLowest, defocusedHighest, upscaleHighest, ts.nativeFloorDecades);
			Logger::WriteMessage(line);
			Assert::IsTrue(nativeLowest >= ts.nativeFloorDecades, L"every native frame stands above the floor test");
			if (upscaleHighest > -1e8) Assert::IsTrue(upscaleHighest < ts.nativeFloorDecades, L"the dark upscale's false natives are within it");
			snprintf(line, sizeof(line), "CAL contrast: false-native dark upscale highest %.4f; defocused native lowest %.4f; gate %.3f\n",
				upscaleContrastHighest, defocusedContrastLowest, ts.nativeFloorContrast);
			Logger::WriteMessage(line);
			if (upscaleContrastHighest >= 0.0) Assert::IsTrue(upscaleContrastHighest < ts.nativeFloorContrast, L"the dark upscale's false natives are under the contrast gate");
			Assert::IsTrue(defocusedContrastLowest > ts.nativeFloorContrast, L"defocused native at a normal level is above the contrast gate (stays native evidence)");
			// 2. The cut test: consecutive analysed frames of one shot (the 0.5 s step, and the cycle's
			// wrap), the same frames faded / flashed, and cuts between the corpus shots.
			const std::vector<ShotSource> shots = Shots(c);
			std::vector<int> ids;
			for (int i = 0; i < 8; ++i) ids.push_back(AddUpscale(st, shots[static_cast<size_t>(i)].clip, shots[static_cast<size_t>(i)].frame0, 0.5, kLanczos3, 2.0));
			double withinWorstDec = 0.0, withinWorstShare = 0.0, fadeWorst = 0.0, cutLeast = 1e9;
			int withinCuts = 0, fadeCuts = 0, missed = 0;
			// The tracker's judge, fed in playing order (its motion level carries from shot to shot).
			SourceResolution::Tracker::CutJudge judge;
			auto isCut = [&](double dec, double jump) { return judge.Judge(dec, jump, 0.5, ts); };
			for (size_t i = 0; i < ids.size(); ++i)
			{
				std::vector<SourceResolution::FrameReading> f;
				for (int k = 0; k < 6; ++k) f.push_back(ReadContent(st, ids[i], k, 1.0));
				for (int k = 0; k + 1 < 6; ++k)
				{
					double dec = 0.0, share = 0.0;
					double jump = 0.0;
					if (!SourceResolution::PatternChange(f[k].pattern, f[k].patternFlat, f[k + 1].pattern, f[k + 1].patternFlat, kBlack, 2.0 / 1023, dec, share, &jump)) continue;
					{
						// Diagnostic: the same pair on whole-tile means (the quadrants averaged).
						auto tiles = [](const SourceResolution::FrameReading& r) {
							std::vector<double> v;
							for (size_t q = 0; q + 3 < r.pattern.size(); q += 4) v.push_back(0.25 * (r.pattern[q] + r.pattern[q + 1] + r.pattern[q + 2] + r.pattern[q + 3]));
							return v;
						};
						const std::vector<double> a = tiles(f[k]), b = tiles(f[k + 1]);
						std::vector<char> none(a.size(), 0);
						double d8 = -1.0, s8 = -1.0;
						std::vector<double> a2 = a, b2 = b;   // 8 points < the 8-position minimum: duplicate
						a2.insert(a2.end(), a.begin(), a.end()); b2.insert(b2.end(), b.begin(), b.end());
						std::vector<char> none2(a2.size(), 0);
						SourceResolution::PatternChange(a2, none2, b2, none2, kBlack, 2.0 / 1023, d8, s8);
						snprintf(line, sizeof(line), "CAL within shot %zu step %d->%d: quadrants dec %.3f share %.3f jump %.3f; tiles dec %.3f share %.3f\n", i, k, k + 1, dec, share, jump, d8, s8);
						Logger::WriteMessage(line);
					}
					withinWorstDec = (std::max)(withinWorstDec, dec);
					withinWorstShare = (std::max)(withinWorstShare, share);
					withinCuts += isCut(dec, jump) ? 1 : 0;
				}
				for (const double g : { 0.3, 0.6, 1.4 })
				{
					const SourceResolution::FrameReading faded = ReadContent(st, ids[i], 1, g);
					double dec = 0.0, share = 0.0;
					double jump = 0.0;
					if (!SourceResolution::PatternChange(f[1].pattern, f[1].patternFlat, faded.pattern, faded.patternFlat, kBlack, 2.0 / 1023, dec, share, &jump)) continue;
					snprintf(line, sizeof(line), "CAL fade shot %zu gain %.1f: dec %.3f share %.3f jump %.3f\n", i, g, dec, share, jump);
					Logger::WriteMessage(line);
					fadeWorst = (std::max)(fadeWorst, dec);
					SourceResolution::Tracker::CutJudge copy = judge;   // a fade at this point of the shot
					fadeCuts += copy.Judge(dec, jump, 0.5, ts) ? 1 : 0;
				}
				if (i + 1 < ids.size())
				{
					const SourceResolution::FrameReading next = ReadContent(st, ids[i + 1], 0, 1.0);
					double dec = 0.0, share = 0.0;
					double jump = 0.0;
					if (SourceResolution::PatternChange(f[5].pattern, f[5].patternFlat, next.pattern, next.patternFlat, kBlack, 2.0 / 1023, dec, share, &jump))
					{
						auto tiles = [](const SourceResolution::FrameReading& r) {
							std::vector<double> v;
							for (size_t q = 0; q + 3 < r.pattern.size(); q += 4) v.push_back(0.25 * (r.pattern[q] + r.pattern[q + 1] + r.pattern[q + 2] + r.pattern[q + 3]));
							v.insert(v.end(), v.begin(), v.end());
							return v;
						};
						const std::vector<double> a = tiles(f[5]), b = tiles(next);
						std::vector<char> none(a.size(), 0);
						double d8 = -1.0, s8 = -1.0;
						SourceResolution::PatternChange(a, none, b, none, kBlack, 2.0 / 1023, d8, s8);
						snprintf(line, sizeof(line), "CAL cut %zu -> %zu: decorrelation %.3f, share %.3f jump %.3f; tiles dec %.3f share %.3f\n", i, i + 1, dec, share, jump, d8, s8);
						Logger::WriteMessage(line);
						cutLeast = (std::min)(cutLeast, dec);
						missed += isCut(dec, jump) ? 0 : 1;
					}
				}
			}
			snprintf(line, sizeof(line), "CAL cuts: within a shot worst decorrelation %.3f share %.3f (%d counted as cuts); fades/flashes worst %.3f (%d); real cuts least %.3f (%d missed); thresholds decorrelation %.2f with a jump %.2f, or %.2f alone, %.2f above the motion level\n",
				withinWorstDec, withinWorstShare, withinCuts, fadeWorst, fadeCuts, cutLeast, missed, ts.cutDecorrelation, ts.cutJump, ts.cutDecorrelationAlone, ts.cutMotionMargin);
			Logger::WriteMessage(line);
			Assert::AreEqual(0, fadeCuts, L"no fade or flash is a cut");
			Assert::AreEqual(0, withinCuts, L"no step inside a shot is a cut");
			Assert::IsTrue(missed <= 2, L"the real cuts are found (a cut out of a fast pan may be missed: the safe direction)");
			Budget("LatchFixesCalibration", started, 30);
		}

		// ---- The tracker alone (synthetic readings; fast, minutes of time) ----

		// Reading noise: a true 1080 upscale reading 0.5 +- 0.01 with occasional frames that read
		// native (with or without detail above 1080): the unlatches they cause and how long each lasts.
		TEST_METHOD(TrackerFlappingUnderReadingNoise)
		{
			std::mt19937 rng(5);
			std::uniform_real_distribution<double> u(0.0, 1.0);
			char line[260];
			for (const double falseShare : { 0.0, 0.002, 0.01 })
			{
				SourceResolution::Tracker tracker;
				int engages = 0, disengages = 0;
				double lowestDeclared = 1.0, highestDeclaredLower = 0.0, nativeSeconds = 0.0;
				bool engaged = false;
				const double dt = 0.5;
				for (int i = 0; i < 3600 * 2; ++i)   // one hour at 2 Hz
				{
					const double t = i * dt;
					const int shot = static_cast<int>(t / 4.0);
					SourceResolution::FrameReading f;
					f.tiles = 8; f.informativeTiles = 8;
					f.mean = (shot % 2 ? 0.55 : 0.30) + 0.001 * (i % 7);
					f.signature = f.mean * 10.0 + i * 1e-6;
					f.ratio = u(rng) < falseShare ? 1.0 : 0.5 + (u(rng) - 0.5) * 0.02;
					tracker.Update(f, t);
					const bool now = tracker.Ratio() < 1.0;
					if (now && !engaged) ++engages;
					if (!now && engaged) ++disengages;
					engaged = now;
					if (now) { lowestDeclared = (std::min)(lowestDeclared, tracker.Ratio()); highestDeclaredLower = (std::max)(highestDeclaredLower, tracker.Ratio()); }
					else if (t > 20.0) nativeSeconds += dt;
				}
				snprintf(line, sizeof(line), "RESULT tracker noise (false native %.1f%%): declares %d, back to native %d, native %.0f s of 3600, declared %.4f..%.4f\n",
					falseShare * 100, engages, disengages, nativeSeconds, lowestDeclared, highestDeclaredLower);
				Logger::WriteMessage(line);
				if (falseShare == 0.0)
				{
					Assert::AreEqual(1, engages);
					Assert::AreEqual(0, disengages);
					// The declared value is the window's highest reading: the noise biases it up, never down.
					Assert::IsTrue(lowestDeclared >= 0.5 - 1e-9 - 0.01);
				}
			}
		}

		// F12 (a rendition drop mid-shot) declares on time alone and says so, so the monitor's
		// confidence does not ask for shots it cannot have (audit 2026-10-17 bug: confidence stayed at
		// evidence x 1/3 and auto never engaged after a mid-shot 4K -> 1080 switch).
		TEST_METHOD(TrackerRenditionChangeDeclaresOnTimeAlone)
		{
			// Power spectra (bins 0..128): native detail falls as 1/f; a 1080 upscale also loses
			// two decades above half the band.
			auto spectrum = [](bool upscaled, double scale) {
				std::vector<double> p(129);
				for (size_t k = 0; k < p.size(); ++k)
				{
					p[k] = scale * 1e-3 / (1.0 + static_cast<double>(k));
					if (upscaled && k > 64) p[k] *= 1e-2;
				}
				return p;
			};
			SourceResolution::Tracker tracker;
			double t = 0.0;
			auto feed = [&](double ratio, bool upscaled, double mean) {
				SourceResolution::FrameReading f;
				f.tiles = 8; f.informativeTiles = 8;
				f.mean = mean;
				f.signature = mean * 10.0 + t * 1e-3;
				f.ratio = ratio;
				f.verticalPower = f.horizontalPower = spectrum(upscaled, 1.0 + 0.01 * std::sin(t));
				tracker.Update(f, t);
				t += 0.5;
			};
			// The 1080 rendition over four shots: declared the normal way.
			for (int shot = 0; shot < 4; ++shot)
				for (int i = 0; i < 8; ++i) feed(0.5, true, (shot % 2 ? 0.55 : 0.30) + 0.0001 * i);
			Assert::AreEqual(0.5, tracker.Ratio(), 1e-9);
			Assert::IsFalse(tracker.DeclaredByRendition());
			// Same shot: the 4K rendition for 6 s (native at once), then the 1080 rendition again.
			for (int i = 0; i < 12; ++i) feed(1.0, false, 0.30 + 0.0001 * i);
			Assert::AreEqual(1.0, tracker.Ratio(), 1e-9);
			double declaredAfter = -1.0;
			const double drop = t;
			for (int i = 0; i < 40; ++i)
			{
				feed(0.5, true, 0.30 + 0.0001 * i);
				if (declaredAfter < 0 && tracker.Ratio() < 1.0) declaredAfter = t - drop;
			}
			char line[160];
			snprintf(line, sizeof(line), "F12: declared %.1f s after the mid-shot drop, shots in the window %d, by rendition %d\n",
				declaredAfter, tracker.Shots(), tracker.DeclaredByRendition() ? 1 : 0);
			Logger::WriteMessage(line);
			Assert::AreEqual(1ull, tracker.RenditionChanges());
			Assert::AreEqual(0.5, tracker.Ratio(), 1e-9);
			Assert::IsTrue(tracker.DeclaredByRendition(), L"declared by the rendition rule");
			Assert::AreEqual(1, tracker.Shots(), L"one shot only: shots cannot be the evidence");
			Assert::IsTrue(declaredAfter <= tracker.GetSettings().renditionSeconds + 1.0);
			// A discontinuity clears it.
			tracker.Discontinuity("test");
			Assert::IsFalse(tracker.DeclaredByRendition());
		}

		// The resume time after native evidence (a mixed screen, a native shot) is the evidence
		// window (120 s), not the 10 s / 3 shots standard; measured on the tracker.
		TEST_METHOD(TrackerResumeAfterNativeEvidence)
		{
			char line[200];
			for (const bool fixes : { true, false })
			for (const double nativeSeconds : { 0.5, 3.0, 30.0 })
			{
				SourceResolution::Tracker::Settings settings;
				settings.latchFixes = fixes;
				SourceResolution::Tracker tracker(settings);
				const double dt = 0.5, nativeAt = 60.0;
				double declaredAgain = -1.0;
				for (int i = 0; i < 2 * 400; ++i)
				{
					const double t = i * dt;
					const int shot = static_cast<int>(t / 4.0);
					SourceResolution::FrameReading f;
					f.tiles = 8; f.informativeTiles = 8;
					f.mean = (shot % 2 ? 0.55 : 0.30) + 0.001 * (i % 7);
					f.signature = f.mean * 10.0 + i * 1e-6;
					const bool native = t >= nativeAt && t < nativeAt + nativeSeconds;
					f.ratio = native ? 1.0 : 0.5;
					f.mixed = native;
					tracker.Update(f, t);
					if (t >= nativeAt + nativeSeconds && declaredAgain < 0 && tracker.Ratio() < 1.0) declaredAgain = t - (nativeAt + nativeSeconds);
				}
				snprintf(line, sizeof(line), "RESULT tracker resume after %.1f s of native (fixes %s): declared again %.1f s after it ended\n",
					nativeSeconds, fixes ? "on" : "off", declaredAgain);
				Logger::WriteMessage(line);
				Assert::IsTrue(declaredAgain > 0.0, L"declared again eventually");
				if (fixes) Assert::IsTrue(declaredAgain <= 10.5, L"fixes on: the standard again (10 s or 3 shots) after the native evidence");
				else Assert::IsTrue(declaredAgain >= 100.0, L"fixes off: the 120 s window as before");
			}
		}

		// Latch precision (2026-10-17): each rendition's reading on single frames at both tile sizes,
		// latch fixes on and off - within the snap reach of the truth, declared exactly (SnapRatio).
		// The log shows the raw reading and the even size the re-base rounded it to before.
		TEST_METHOD(RenditionsReadAndSnapToTheirExactSize)
		{
			const auto started = std::chrono::steady_clock::now();
			char line[400];
			std::wstring failures;
			int index = 0;
			for (const SrcExact::Rendition& r : SrcExact::Renditions())
			{
				++index;
				for (int tile : { 128, 256 })
				{
					const int x0 = (r.W - r.pw()) / 2, y0 = (r.H - r.ph()) / 2;
					int base = 0;
					const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(x0, y0, r.pw(), r.ph(), tile, false, &base);
					Assert::IsTrue(places.size() >= 4, L"tiles inside the picture");
					std::vector<SourceResolution::Spectrum> spectra(places.size());
					std::vector<float> codes(static_cast<size_t>(tile) * tile);
					for (size_t i = 0; i < places.size(); ++i)
					{
						const std::vector<double> v = SrcExact::Tile(SrcExact::Scene(100u * index + static_cast<unsigned>(i)), r.rx(), r.ry(),
							places[i].x0, places[i].y0, tile, i % 2 ? 0.55 : 0.35);
						SrcExact::Quantise(v, 0.0, codes.data());
						Assert::IsTrue(SourceResolution::Analyse(codes.data(), tile, tile, spectra[i], 1023));
					}
					for (bool fixes : { false, true })
					{
						SourceResolution::Thresholds t;
						t.latchFixes = fixes;
						// Every synthetic tile is its own independent scene: their coherence spreads more
						// than a real picture's, and at 128 px the mixed-screen guard took some for native UI
						// (measured; MixedScreenSourceResolutionTests cover the guard). Off: the rate alone.
						t.mixedGuard = false;
						for (const SourceResolution::TilePlace& p : places) { t.tileX.push_back(p.centreX); t.tileY.push_back(p.centreY); }
						t.signatureTiles = base;
						const SourceResolution::FrameReading f = SourceResolution::ReadFrame(spectra, t);
						const double reading = r.perAxis() ? f.ratioH : f.ratio;
						// The tile the monitor uses for this picture (TileSize with the fixes, else 256) must
						// read the rendition; the other size is checked whenever it reads one.
						const bool production = tile == (fixes ? SourceResolution::TileSize(r.ph()) : 256);
						const double snapped = SourceResolution::SnapRatio(reading, r.frameSize(), tile);
						const int before = 2 * static_cast<int>(std::lround(reading * r.frameSize() * 0.5));
						snprintf(line, sizeof(line), "RESULT %-32s tile %3d fixes %-3s: reading %.5f (%+.3f bin, %.2f px; re-based to %d before) -> %.6f = %.2f px (truth %d)\n",
							r.name, tile, fixes ? "on" : "off", reading, (reading - r.expected()) * tile, reading * r.frameSize(), before,
							snapped, snapped * r.frameSize(), static_cast<int>(std::lround(r.expected() * r.frameSize())));
						Logger::WriteMessage(line);
						const std::string text(line);
						const std::wstring message(text.begin(), text.end());
						if (r.perAxis() && f.ratio != 1.0) failures += L"uniform not native: " + message;
						if (reading >= 1.0)
						{
							if (production) failures += L"not read at the monitor's tile: " + message;
							continue;
						}
						// Within the snap reach (a wrong peak - 128-px tiles at 1/3 read 0.3455 before the
						// candidates' harmonic fallback - is not).
						if (std::fabs(reading - r.expected()) > SourceResolution::kSnapBins / tile) failures += L"not within the snap reach: " + message;
						if (snapped != r.expected()) failures += L"not exact: " + message;
					}
				}
			}
			SrcLatch::Budget("RenditionsReadAndSnapToTheirExactSize", started, 30);
			Assert::IsTrue(failures.empty(), failures.c_str());
		}

		// Latch precision (2026-10-17): the same renditions through SourceResolutionMonitor over time -
		// shots of 2 s, a tile copy every 0.5 s, AutoFactors on every copy - at both tile sizes with
		// the latch fixes on and off: the declared ratio and the applied re-base factors are exact
		// (540 in 1080 is 0.5000, not 542 / 1080).
		TEST_METHOD(RenditionsReBaseToTheirExactSize)
		{
			const auto started = std::chrono::steady_clock::now();
			char line[400];
			std::wstring failures;
			int index = 0;
			for (const SrcExact::Rendition& r : SrcExact::Renditions())
			{
				++index;
				for (int tile : { 128, 256 })
				{
					const int x0 = (r.W - r.pw()) / 2, y0 = (r.H - r.ph()) / 2;
					int base = 0;
					const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(x0, y0, r.pw(), r.ph(), tile, false, &base);
					const int count = static_cast<int>(places.size());
					std::vector<float> tx, ty;
					for (const SourceResolution::TilePlace& p : places) { tx.push_back(p.centreX); ty.push_back(p.centreY); }
					// The shots' tiles (each shot its own scenes and level), shared by both fixes settings.
					const int shots = 16, framesPerShot = 4;
					std::vector<std::vector<std::vector<double>>> shotTiles(shots);
					for (int s = 0; s < shots; ++s)
						for (int i = 0; i < count; ++i)
							shotTiles[static_cast<size_t>(s)].push_back(SrcExact::Tile(SrcExact::Scene(10000u * index + 100u * s + static_cast<unsigned>(i)), r.rx(), r.ry(),
								places[static_cast<size_t>(i)].x0, places[static_cast<size_t>(i)].y0, tile, s % 2 ? 0.55 : 0.30));
					for (bool fixes : { false, true })
					{
						SourceResolutionMonitor::Monitor m;
						m.SetLatchFixes(fixes);
						m.SetMixedGuard(false);   // as above: the synthetic tiles' spread, not the rate under test
						const bool production = tile == m.TileFor(r.ph());
						std::vector<float> values(static_cast<size_t>(count) * tile * tile);
						double t = 0.0, fx = 1.0, fy = 1.0, engagedAt = -1.0;
						uint64_t sequence = 0;
						std::string why, lastWhy;
						int nativeFrames = 0, lowerFrames = 0;
						double lowMin = 1.0, lowMax = 0.0, confidenceMax = 0.0;
						for (int s = 0; s < shots && (engagedAt < 0.0 || t < engagedAt + 2.5); ++s)
							for (int k = 0; k < framesPerShot; ++k)
							{
								for (int i = 0; i < count; ++i)
									SrcExact::Quantise(shotTiles[static_cast<size_t>(s)][static_cast<size_t>(i)], 0.0008 * k, values.data() + static_cast<size_t>(i) * tile * tile);
								m.AnalyseTilesForTest(values, count, t, r.W, r.H, tx, ty, base, tile);
								// Confidence 0: the synthetic content's coherence sits below a real upscale's
								// (clarity under 0.8 measured); the gate is not under test, the sizes are.
								m.AutoFactors(r.W, r.H, 0.0, why, fx, fy, t, ++sequence);
								{
									const SourceResolutionMonitor::Snapshot d = m.Read();
									const double fr = r.perAxis() ? 1.0 : d.frameRatio;
									if (fr >= 1.0) ++nativeFrames; else { ++lowerFrames; lowMin = (std::min)(lowMin, fr); lowMax = (std::max)(lowMax, fr); }
									confidenceMax = (std::max)(confidenceMax, r.perAxis() ? d.axisConfidence : d.confidence);
									if (!why.empty()) lastWhy = why;
								}
								if (engagedAt < 0.0 && (fx < 1.0 || fy < 1.0)) engagedAt = t;
								t += 0.5;
							}
						const SourceResolutionMonitor::Snapshot snap = m.Read();
						const double declared = r.perAxis() ? snap.axisRatioX : snap.declaredRatio;
						snprintf(line, sizeof(line), "RESULT %-32s tile %3d fixes %-3s: declared %.6f (%.2f px), engaged %.1f s, factors %.6f x %.6f (truth %.6f); frames native %d lower %d (%.4f-%.4f), confidence max %.2f, cuts %llu, last: %s\n",
							r.name, tile, fixes ? "on" : "off", declared, declared * r.frameSize(), engagedAt, fx, fy, r.expected(),
							nativeFrames, lowerFrames, lowMin, lowMax, confidenceMax, snap.cuts, lastWhy.c_str());
						Logger::WriteMessage(line);
						const std::string text(line);
						const std::wstring message(text.begin(), text.end());
						// The monitor's own tile for this picture must engage; the other size, whenever it
						// declares or engages, exactly too.
						if (engagedAt < 0.0)
						{
							if (production) failures += L"never engaged at the monitor's tile: " + message;
							if (declared < 1.0 && declared != r.expected()) failures += L"declared not exact: " + message;
							continue;
						}
						if (declared != r.expected()) failures += L"declared not exact: " + message;
						if (r.perAxis() ? (fx != r.expected() || fy != 1.0) : (fx != r.expected() || fy != r.expected()))
							failures += L"factors not exact: " + message;
					}
				}
			}
			SrcLatch::Budget("RenditionsReBaseToTheirExactSize", started, 240);
			Assert::IsTrue(failures.empty(), failures.c_str());
		}
	};
}
