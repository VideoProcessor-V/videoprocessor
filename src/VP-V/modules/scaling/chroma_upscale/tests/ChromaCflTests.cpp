// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale: the CfL chroma upsampler. ChromaCflModelTests exercise the CPU reference model (ChromaCfl.h);
// ChromaCflGpuTests compare the GPU passes (ChromaCflGpu) with it on a libplacebo device: WARP (software, runs in the CPU
// tier) unless VP_TEST_GPU=hardware. The timing test is hardware only (category GpuHardware).
#include "pch.h"
#include "CppUnitTest.h"
#include "ExtendedRangeGpuProbe.h"

#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCflGpu.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ChromaCflTestsNs
{
	using ChromaUpscale::ActiveRect;
	using ChromaUpscale::Plane;
	constexpr double kPi = 3.14159265358979323846;

	void LogF(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0)
	{
		char line[256];
		std::snprintf(line, sizeof line, fmt, a, b, c, d, e);
		Logger::WriteMessage(line);
	}
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	struct Noise
	{
		std::mt19937 g;
		explicit Noise(uint32_t seed) : g(seed) {}
		double Uniform() { return (static_cast<double>(g()) + 0.5) / 4294967296.0; }
		double Next() { return std::sqrt(-2.0 * std::log(Uniform())) * std::cos(2.0 * kPi * Uniform()); }
	};

	// 10-bit codes at 1/64-code resolution: what a 16-bit plane with bit_shift 6 holds.
	void Quantise(Plane& p)
	{
		for (double& v : p.v) v = (std::min)(65535.0, (std::max)(0.0, std::round(v * 64.0))) / 64.0;
	}

	double TruthLuma(double x, double y)
	{
		return 420.0 + 150.0 * std::sin(2 * kPi * x / 23.0 + 0.4) * std::cos(2 * kPi * y / 31.0 + 0.2) +
			120.0 * std::tanh((0.8 * x + 0.6 * y - 70.0) / 1.0) + 60.0 * std::tanh((x - 0.7 * y - 90.0) / 1.3);
	}

	struct Scene
	{
		Plane y, cb, cr;   // codes, quantised
	};

	// A textured 4:2:x scene: luma with edges and texture, chroma = luma-correlated structure + a smooth term, downsampled with
	// the siting's tent, plus grain; everything quantised to 1/64 code.
	Scene MakeScene(int w, int h, int cw, int ch, double sx, double sy, uint32_t seed, double lumaSigma, double chromaSigma)
	{
		Plane Y(w, h), tcb(w, h), tcr(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const double L = TruthLuma(x, y);
				Y.at(x, y) = L;
				tcb.at(x, y) = 520.0 + 0.35 * (L - 420.0) + 25.0 * std::sin(2 * kPi * x / 61.0);
				tcr.at(x, y) = 500.0 - 0.25 * (L - 420.0) + 20.0 * std::cos(2 * kPi * y / 47.0);
			}
		const double fx = static_cast<double>(w) / cw, fy = static_cast<double>(h) / ch;
		Scene s;
		s.y = Y;
		s.cb = ChromaUpscale::TentAtSites(tcb, 0, 0, cw, ch, fx, fy, sx, sy, 0, 0);
		s.cr = ChromaUpscale::TentAtSites(tcr, 0, 0, cw, ch, fx, fy, sx, sy, 0, 0);
		Noise n(seed);
		for (double& v : s.y.v) v += lumaSigma * n.Next();
		for (double& v : s.cb.v) v += chromaSigma * n.Next();
		for (double& v : s.cr.v) v += chromaSigma * n.Next();
		Quantise(s.y); Quantise(s.cb); Quantise(s.cr);
		return s;
	}

	// The picture inside bars: pad* in luma pixels (multiples of the subsampling factors), bar luma and chroma codes.
	Scene Embed(const Scene& pic, int padL, int padT, int padR, int padB, int fx, int fy, double barY, double barC)
	{
		Scene s;
		const int w = pic.y.w, h = pic.y.h, cw = pic.cb.w, ch = pic.cb.h;
		const int cpl = padL / fx, cpt = padT / fy;
		s.y = Plane(w + padL + padR, h + padT + padB, barY);
		s.cb = Plane(cw + cpl + padR / fx, ch + cpt + padB / fy, barC);
		s.cr = s.cb;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) s.y.at(padL + x, padT + y) = pic.y.at(x, y);
		for (int y = 0; y < ch; ++y)
			for (int x = 0; x < cw; ++x)
			{
				s.cb.at(cpl + x, cpt + y) = pic.cb.at(x, y);
				s.cr.at(cpl + x, cpt + y) = pic.cr.at(x, y);
			}
		return s;
	}

	double Mse(const Plane& a, const Plane& b)
	{
		double s = 0.0;
		for (size_t i = 0; i < a.v.size(); ++i) s += (a.v[i] - b.v[i]) * (a.v[i] - b.v[i]);
		return s / static_cast<double>(a.v.size());
	}

	double RmsDiff(const Plane& a, const Plane& b) { return std::sqrt(Mse(a, b)); }

	double MaxDiff(const Plane& a, const Plane& b)
	{
		double m = 0.0;
		for (size_t i = 0; i < a.v.size(); ++i) m = (std::max)(m, std::fabs(a.v[i] - b.v[i]));
		return m;
	}

	// --- GPU side ---
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
			Logger::WriteMessage(("chroma upscale GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	uint16_t Raw(double codes) { return static_cast<uint16_t>((std::min)(65535L, (std::max)(0L, std::lround(codes * 64.0)))); }

	pl_tex UploadRaw(pl_gpu gpu, int w, int h, const char* format, const std::vector<uint16_t>& data)
	{
		pl_tex_params p{};
		p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, format); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	pl_tex UploadR16(pl_gpu gpu, const Plane& y)
	{
		std::vector<uint16_t> data(y.v.size());
		for (size_t i = 0; i < data.size(); ++i) data[i] = Raw(y.v[i]);
		return UploadRaw(gpu, y.w, y.h, "r16", data);
	}

	pl_tex UploadRg16(pl_gpu gpu, const Plane& cb, const Plane& cr)
	{
		std::vector<uint16_t> data(cb.v.size() * 2);
		for (size_t i = 0; i < cb.v.size(); ++i) { data[i * 2] = Raw(cb.v[i]); data[i * 2 + 1] = Raw(cr.v[i]); }
		return UploadRaw(gpu, cb.w, cb.h, "rg16", data);
	}

	// A 16-bit unorm texture (1 or more components) read back as uint16; false for any other format.
	bool DownloadRaw(pl_gpu gpu, pl_tex tex, std::vector<uint16_t>& raw, int& comps)
	{
		const pl_fmt fmt = tex->params.format;
		if (fmt->type != PL_FMT_UNORM || fmt->component_depth[0] != 16) return false;
		comps = fmt->num_components;
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = fmt; rp.host_readable = true; rp.blit_dst = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return false;
		pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
		pl_tex_blit(gpu, &bp);
		raw.assign(static_cast<size_t>(rp.w) * rp.h * comps, 0);
		pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
		const bool ok = pl_tex_download(gpu, &d);
		pl_tex_destroy(gpu, &readable);
		return ok;
	}

	void SetRepr(pl_frame& f)
	{
		f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
		f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
		f.color = pl_color_space_bt709;
	}

	// Plane 1 = Cb, Cr interleaved (NV12-like layout, 16-bit).
	pl_frame Frame2(pl_tex luma, pl_tex chroma, double sx, double sy)
	{
		pl_frame f{};
		f.num_planes = 2;
		f.planes[0].texture = luma; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
		f.planes[1].texture = chroma; f.planes[1].components = 2;
		f.planes[1].component_mapping[0] = PL_CHANNEL_CB; f.planes[1].component_mapping[1] = PL_CHANNEL_CR;
		f.planes[1].shift_x = static_cast<float>(sx); f.planes[1].shift_y = static_cast<float>(sy);
		SetRepr(f);
		return f;
	}

	// Planes 1 and 2 = Cb and Cr (I420-like layout).
	pl_frame Frame3(pl_tex luma, pl_tex cb, pl_tex cr, double sx, double sy)
	{
		pl_frame f{};
		f.num_planes = 3;
		f.planes[0].texture = luma; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
		f.planes[1].texture = cb; f.planes[1].components = 1; f.planes[1].component_mapping[0] = PL_CHANNEL_CB;
		f.planes[2].texture = cr; f.planes[2].components = 1; f.planes[2].component_mapping[0] = PL_CHANNEL_CR;
		for (int i = 1; i < 3; ++i) { f.planes[i].shift_x = static_cast<float>(sx); f.planes[i].shift_y = static_cast<float>(sy); }
		SetRepr(f);
		return f;
	}

	struct GpuOut
	{
		bool ok = false;
		std::string error;
		Plane cb, cr;
		int numPlanes = 0, texW = 0, texH = 0, comps = 0, map0 = -9, map1 = -9;
		float shiftX = -1.0f, shiftY = -1.0f;
		bool lumaUnchanged = false;
		std::string status;
	};

	// One Apply on a fresh module. threePlane: Cb and Cr as separate planes.
	GpuOut RunGpu(GpuDevice& dev, const Scene& s, double sx, double sy, const ActiveRect& rect, bool threePlane = false, bool guard = true)
	{
		GpuOut o;
		pl_gpu gpu = dev.Gpu();
		pl_tex luma = UploadR16(gpu, s.y), chroma = nullptr, cbT = nullptr, crT = nullptr;
		pl_frame src;
		if (threePlane)
		{
			cbT = UploadR16(gpu, s.cb); crT = UploadR16(gpu, s.cr);
			src = Frame3(luma, cbT, crT, sx, sy);
		}
		else
		{
			chroma = UploadRg16(gpu, s.cb, s.cr);
			src = Frame2(luma, chroma, sx, sy);
		}
		ChromaUpscale::Gpu::CflModule module;
		ChromaUpscale::Gpu::Params gp;
		gp.settings.mode = ChromaUpscale::Mode::Cfl;
		gp.active = rect;
		gp.cfl.grainGuard = guard;
		pl_frame out{};
		o.ok = module.Apply(gpu, dev.dispatch, src, gp, out);
		o.error = module.LastError();
		o.status = module.Status();
		if (o.ok)
		{
			o.numPlanes = out.num_planes;
			o.comps = out.planes[1].components;
			o.map0 = out.planes[1].component_mapping[0]; o.map1 = out.planes[1].component_mapping[1];
			o.shiftX = out.planes[1].shift_x; o.shiftY = out.planes[1].shift_y;
			o.lumaUnchanged = out.planes[0].texture == luma;
			pl_tex ct = out.planes[1].texture;
			o.texW = ct->params.w; o.texH = ct->params.h;
			std::vector<uint16_t> raw;
			int nc = 0;
			if (DownloadRaw(gpu, ct, raw, nc))
			{
				o.cb = Plane(o.texW, o.texH); o.cr = Plane(o.texW, o.texH);
				for (size_t i = 0; i < o.cb.v.size(); ++i)
				{
					o.cb.v[i] = raw[i * nc] / 64.0;
					o.cr.v[i] = raw[i * nc + 1] / 64.0;
				}
			}
			else
			{
				o.ok = false;
				o.error = "the output texture is not a 16-bit unorm format (test readback)";
			}
		}
		module.Release(gpu);
		for (pl_tex* t : { &luma, &chroma, &cbT, &crT })
			if (*t) pl_tex_destroy(gpu, t);
		return o;
	}

	ChromaUpscale::Result RunModel(const Scene& s, double sx, double sy, const ActiveRect& rect)
	{
		return ChromaUpscale::Upsample(s.y, s.cb, s.cr, sx, sy, rect, 10, 0.0);
	}

	// Edge scene: luma step at an odd column, chroma = 0.5 luma + 100 made by the siting's tent. MSE of CfL and of the plain
	// Catmull-Rom against the full-resolution truth.
	void EdgeCase(int w, int h, int cw, int ch, double sx, double sy, double& mseCfl, double& msePlain)
	{
		Plane Y(w, h), truth(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				Y.at(x, y) = x < 21 ? 200.0 : 700.0;
				truth.at(x, y) = 0.5 * Y.at(x, y) + 100.0;
			}
		const double fx = static_cast<double>(w) / cw, fy = static_cast<double>(h) / ch;
		const Plane c = ChromaUpscale::TentAtSites(truth, 0, 0, cw, ch, fx, fy, sx, sy, 0, 0);
		const ChromaUpscale::Result r = ChromaUpscale::Upsample(Y, c, c, sx, sy, ActiveRect(), 10, 0.0);
		mseCfl = Mse(r.cb, truth);
		msePlain = Mse(ChromaUpscale::PlainUpsample(c, w, h, sx, sy), truth);
	}
}

namespace VideoProcessorTest
{
	using namespace ChromaCflTestsNs;

	TEST_CLASS(ChromaCflModelTests)
	{
	public:
		TEST_METHOD(ChromaCflFlatChromaStaysExact)
		{
			// A flat chroma plane is flat at every pixel, whatever the luma does, including the blacker-than-black (40) and
			// whiter-than-white (1000) codes: no range clamp, no ringing, no leakage from the luma.
			const double sites[3][2] = { { -0.5, -0.5 }, { -0.5, 0.0 }, { 0.0, 0.0 } };
			for (const auto& s : sites)
				for (int guard = 0; guard < 2; ++guard)
				{
					Scene sc = MakeScene(64, 48, 32, 24, s[0], s[1], 5u, 1.5, 0.0);
					std::fill(sc.cb.v.begin(), sc.cb.v.end(), 40.0);
					std::fill(sc.cr.v.begin(), sc.cr.v.end(), 1000.0);
					ChromaUpscale::CflParams p;
					p.grainGuard = guard != 0;
					const ChromaUpscale::Result r = ChromaUpscale::Upsample(sc.y, sc.cb, sc.cr, s[0], s[1], ActiveRect(), 10, 0.0, p);
					Assert::AreEqual(std::string("ok"), r.status);
					Assert::IsTrue(r.cb.w == 64 && r.cb.h == 48 && r.cr.w == 64 && r.cr.h == 48, L"full luma size");
					double worst = 0.0;
					for (size_t i = 0; i < r.cb.v.size(); ++i)
						worst = (std::max)(worst, (std::max)(std::fabs(r.cb.v[i] - 40.0), std::fabs(r.cr.v[i] - 1000.0)));
					LogF("flat chroma (shift %.1f, %.1f, guard %.0f): worst deviation %.3e codes", s[0], s[1], guard, worst);
					Assert::IsTrue(worst < 1e-9, L"flat chroma stays exactly its code");
				}
		}

		TEST_METHOD(ChromaCflSitingMatchesChromaSamplesAtSites)
		{
			// Left-sited (shift -0.5, 0): luma columns 2i sit exactly on chroma column i; top-left co-sited (-0.5, -0.5): luma
			// (2i, 2j) sits on chroma (i, j). With a constant luma the prediction is off (no luma structure to follow) and
			// the output at a site is the chroma sample itself.
			{
				Scene sc = MakeScene(64, 48, 32, 24, -0.5, 0.0, 6u, 0.0, 0.0);
				std::fill(sc.y.v.begin(), sc.y.v.end(), 300.0);
				for (int y = 0; y < 24; ++y)
					for (int x = 0; x < 32; ++x)
					{
						sc.cb.at(x, y) = 300.0 + 120.0 * std::sin(2 * kPi * x / 13.0);   // varies along x only
						sc.cr.at(x, y) = 700.0 - 5.0 * x;
					}
				const ChromaUpscale::Result r = RunModel(sc, -0.5, 0.0, ActiveRect());
				double worst = 0.0;
				for (int y = 0; y < 48; ++y)
					for (int x = 0; x < 64; x += 2)
						worst = (std::max)(worst, (std::max)(std::fabs(r.cb.at(x, y) - sc.cb.at(x / 2, y / 2)), std::fabs(r.cr.at(x, y) - sc.cr.at(x / 2, y / 2))));
				LogF("left-sited, luma columns on chroma sites: worst %.3e codes", worst);
				Assert::IsTrue(worst < 1e-9, L"left-sited: the output at a site is the chroma sample");
			}
			{
				Scene sc = MakeScene(64, 48, 32, 24, -0.5, -0.5, 7u, 0.0, 0.0);
				std::fill(sc.y.v.begin(), sc.y.v.end(), 300.0);
				for (int y = 0; y < 24; ++y)
					for (int x = 0; x < 32; ++x)
					{
						sc.cb.at(x, y) = 300.0 + 90.0 * std::sin(2 * kPi * x / 11.0) * std::cos(2 * kPi * y / 9.0);
						sc.cr.at(x, y) = 600.0 + 3.0 * x - 2.0 * y;
					}
				const ChromaUpscale::Result r = RunModel(sc, -0.5, -0.5, ActiveRect());
				double worst = 0.0;
				for (int y = 0; y < 48; y += 2)
					for (int x = 0; x < 64; x += 2)
						worst = (std::max)(worst, (std::max)(std::fabs(r.cb.at(x, y) - sc.cb.at(x / 2, y / 2)), std::fabs(r.cr.at(x, y) - sc.cr.at(x / 2, y / 2))));
				LogF("top-left co-sited: worst %.3e codes", worst);
				Assert::IsTrue(worst < 1e-9, L"co-sited: the output at a site is the chroma sample");
			}
		}

		TEST_METHOD(ChromaCflReconstructsLumaCorrelatedEdges)
		{
			// Chroma = 0.5 luma + 100 with a luma edge at an odd column, chroma made by the tent downsample: the CfL output is
			// closer to the full-resolution truth than the plain Catmull-Rom.
			double cfl = 0.0, plain = 0.0;
			EdgeCase(64, 48, 32, 24, -0.5, -0.5, cfl, plain);
			LogF("4:2:0 co-sited edge: MSE CfL %.3f, plain Catmull-Rom %.3f", cfl, plain);
			Assert::IsTrue(plain > 1.0, L"the plain upsample has an error at the edge (test premise)");
			Assert::IsTrue(cfl < 0.5 * plain, L"CfL reconstructs the edge better than the plain upsample");
			EdgeCase(64, 48, 32, 24, -0.5, 0.0, cfl, plain);
			LogF("4:2:0 left-sited edge: MSE CfL %.3f, plain Catmull-Rom %.3f", cfl, plain);
			Assert::IsTrue(cfl < 0.5 * plain, L"left-sited: CfL beats the plain upsample");
		}

		TEST_METHOD(ChromaCfl422Works)
		{
			// 4:2:2 (fx = 2, fy = 1): flat chroma exact, correlated edge reconstructed, output full size.
			Scene sc = MakeScene(64, 48, 32, 48, -0.5, 0.0, 8u, 1.0, 0.0);
			std::fill(sc.cb.v.begin(), sc.cb.v.end(), 40.0);
			std::fill(sc.cr.v.begin(), sc.cr.v.end(), 1000.0);
			const ChromaUpscale::Result r = RunModel(sc, -0.5, 0.0, ActiveRect());
			Assert::AreEqual(std::string("ok"), r.status);
			Assert::IsTrue(r.cb.w == 64 && r.cb.h == 48, L"4:2:2 output is full size");
			double worst = 0.0;
			for (size_t i = 0; i < r.cb.v.size(); ++i)
				worst = (std::max)(worst, (std::max)(std::fabs(r.cb.v[i] - 40.0), std::fabs(r.cr.v[i] - 1000.0)));
			Assert::IsTrue(worst < 1e-9, L"4:2:2 flat chroma stays exact");
			double cfl = 0.0, plain = 0.0;
			EdgeCase(64, 48, 32, 48, -0.5, 0.0, cfl, plain);
			LogF("4:2:2 edge: MSE CfL %.3f, plain Catmull-Rom %.3f", cfl, plain);
			Assert::IsTrue(cfl < 0.5 * plain, L"4:2:2: CfL beats the plain upsample");
		}

		TEST_METHOD(ChromaCfl444IsNothing)
		{
			Scene sc = MakeScene(32, 24, 32, 24, 0.0, 0.0, 9u, 1.0, 1.0);
			const ChromaUpscale::Result r = RunModel(sc, 0.0, 0.0, ActiveRect());
			Assert::AreEqual(std::string("4:4:4: nothing to upsample"), r.status);
			Assert::IsTrue(r.cb.v == sc.cb.v && r.cr.v == sc.cr.v, L"4:4:4 returns the input");
		}

		TEST_METHOD(ChromaCflGrainIsPassedNotSmoothed)
		{
			// Owner principle: subtract the grain, process, add the exact grain back. Chroma = the same smooth structure plus
			// white grain (sigma 2) under two seeds; the grain isolate out_a - out_b has the RMS of the Original kernel's
			// (the plain Catmull-Rom passes grain; CfL must not smooth it away nor amplify it).
			const int W = 96, H = 64, cw = 48, ch = 32;
			const Scene base = MakeScene(W, H, cw, ch, -0.5, 0.0, 11u, 1.5, 0.0);   // luma and chroma structure, no chroma grain yet
			Scene a2 = base, b = base;
			Noise na(77u), nb(99u);
			for (double& v : a2.cb.v) v += 2.0 * na.Next();
			for (double& v : a2.cr.v) v += 2.0 * na.Next();
			for (double& v : b.cb.v) v += 2.0 * nb.Next();
			for (double& v : b.cr.v) v += 2.0 * nb.Next();
			const ChromaUpscale::Result ra = RunModel(a2, -0.5, 0.0, ActiveRect()), rb = RunModel(b, -0.5, 0.0, ActiveRect());
			const double cflCb = RmsDiff(ra.cb, rb.cb), cflCr = RmsDiff(ra.cr, rb.cr);
			const double plainCb = RmsDiff(ChromaUpscale::PlainUpsample(a2.cb, W, H, -0.5, 0.0), ChromaUpscale::PlainUpsample(b.cb, W, H, -0.5, 0.0));
			const double plainCr = RmsDiff(ChromaUpscale::PlainUpsample(a2.cr, W, H, -0.5, 0.0), ChromaUpscale::PlainUpsample(b.cr, W, H, -0.5, 0.0));
			LogF("grain isolate RMS: Cb CfL %.4f vs plain %.4f, Cr CfL %.4f vs plain %.4f", cflCb, plainCb, cflCr, plainCr);
			Assert::IsTrue(plainCb > 0.5 && plainCr > 0.5, L"the plain upsample passes the grain (test premise)");
			Assert::IsTrue(std::fabs(cflCb / plainCb - 1.0) < 0.05, L"Cb grain isolate within 5% of the Original kernel's");
			Assert::IsTrue(std::fabs(cflCr / plainCr - 1.0) < 0.05, L"Cr grain isolate within 5% of the Original kernel's");
		}

		TEST_METHOD(ChromaCflDeterministic)
		{
			const Scene sc = MakeScene(64, 48, 32, 24, -0.5, 0.0, 12u, 1.5, 1.5);
			const ChromaUpscale::Result r1 = RunModel(sc, -0.5, 0.0, ActiveRect()), r2 = RunModel(sc, -0.5, 0.0, ActiveRect());
			Assert::IsTrue(r1.cb.v == r2.cb.v && r1.cr.v == r2.cr.v, L"two runs are bit-identical");
		}

		TEST_METHOD(ChromaCflActivePictureKeepsBars)
		{
			// Letterbox and pillarbox: bar pixels are exactly the bar code (a flat bar stays flat), and the picture's output does
			// not depend on the bar level (no tap crosses the boundary).
			const Scene pic = MakeScene(64, 32, 32, 16, -0.5, 0.0, 13u, 1.5, 1.5);
			struct Case { const char* name; int padL, padT, padR, padB; };
			const Case cases[] = { { "letterbox", 0, 8, 0, 8 }, { "pillarbox", 8, 0, 8, 0 } };
			for (const Case& c : cases)
			{
				const double barLevels[2] = { 512.0, 64.0 };
				ChromaUpscale::Result res[2];
				ActiveRect rect;
				Scene scenes[2];
				for (int k = 0; k < 2; ++k)
				{
					scenes[k] = Embed(pic, c.padL, c.padT, c.padR, c.padB, 2, 2, 64.0, barLevels[k]);
					rect.left = c.padL; rect.top = c.padT; rect.right = c.padL + 64; rect.bottom = c.padT + 32;
					res[k] = RunModel(scenes[k], -0.5, 0.0, rect);
					Assert::AreEqual(std::string("ok"), res[k].status);
					double worstBar = 0.0;
					const int W = scenes[k].y.w, H = scenes[k].y.h;
					for (int y = 0; y < H; ++y)
						for (int x = 0; x < W; ++x)
						{
							const bool inside = x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
							if (!inside) worstBar = (std::max)(worstBar, (std::max)(std::fabs(res[k].cb.at(x, y) - barLevels[k]), std::fabs(res[k].cr.at(x, y) - barLevels[k])));
						}
					LogF("bar level %.0f, worst bar deviation %.3e codes", barLevels[k], worstBar);
					Logger::WriteMessage(c.name);
					Assert::IsTrue(worstBar < 1e-9, L"bar pixels are exactly the bar code");
				}
				bool identical = true;
				for (int y = rect.top; y < rect.bottom; ++y)
					for (int x = rect.left; x < rect.right; ++x)
						identical = identical && res[0].cb.at(x, y) == res[1].cb.at(x, y) && res[0].cr.at(x, y) == res[1].cr.at(x, y);
				Assert::IsTrue(identical, L"the picture does not depend on the bar level");
			}
		}

		TEST_METHOD(ChromaCflSettingsParse)
		{
			ChromaUpscale::Settings s;
			std::vector<std::string> errors;
			Assert::IsTrue(s.mode == ChromaUpscale::Mode::Off, L"default off");
			Assert::IsTrue(ChromaUpscale::ParseSettings({}, s, &errors) && errors.empty() && s.mode == ChromaUpscale::Mode::Off, L"absent key keeps the default");
			Assert::IsTrue(ChromaUpscale::ParseSettings({ { "chroma_upscale_vp", "cfl" } }, s, &errors) && s.mode == ChromaUpscale::Mode::Cfl, L"cfl");
			Assert::IsTrue(ChromaUpscale::ParseSettings({ { "chroma_upscale_vp", "off" } }, s, &errors) && s.mode == ChromaUpscale::Mode::Off, L"off");
			Assert::IsTrue(ChromaUpscale::ParseSettings({ { "chroma_upscale_vp", "CFL" } }, s, &errors) && s.mode == ChromaUpscale::Mode::Cfl, L"CFL (case-insensitive)");
			Assert::IsTrue(errors.empty(), L"no errors so far");
			Assert::IsFalse(ChromaUpscale::ParseSettings({ { "chroma_upscale_vp", "bogus" } }, s, &errors), L"invalid value fails");
			Assert::IsTrue(s.mode == ChromaUpscale::Mode::Cfl, L"an invalid value keeps the previous mode");
			Assert::IsTrue(errors.size() == 1, L"one error message");
			Assert::IsTrue(ChromaUpscale::ParseSettings({ { "other_key", "x" } }, s, &errors), L"unknown keys are ignored");
		}
	};

	TEST_CLASS(ChromaCflGpuTests)
	{
	public:
		static void CompareWithModel(GpuDevice& dev, const Scene& sc, double sx, double sy, const ActiveRect& rect, const char* name)
		{
			const GpuOut g = RunGpu(dev, sc, sx, sy, rect);
			Assert::IsTrue(g.ok, Wide(g.error).c_str());
			Assert::IsTrue(g.status == "ok" || g.status == "ok; own grain split", Wide(g.status).c_str());
			const ChromaUpscale::Result m = RunModel(sc, sx, sy, rect);
			Assert::IsTrue(g.texW == sc.y.w && g.texH == sc.y.h, L"output chroma is luma size");
			const double wcb = MaxDiff(g.cb, m.cb), wcr = MaxDiff(g.cr, m.cr);
			LogF("chroma upscale GPU vs model: Cb worst %.4f, Cr worst %.4f codes", wcb, wcr);
			Logger::WriteMessage(name);
			Assert::IsTrue(wcb <= 0.02 && wcr <= 0.02, L"GPU within 0.02 codes of the model");
		}

		TEST_METHOD(ChromaCflGpu420MatchesModel)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 21u, 1.5, 2.0);
			CompareWithModel(dev, sc, -0.5, 0.0, ActiveRect(), "4:2:0 left-sited");
			const Scene co = MakeScene(128, 96, 64, 48, -0.5, -0.5, 22u, 1.5, 2.0);
			CompareWithModel(dev, co, -0.5, -0.5, ActiveRect(), "4:2:0 co-sited");
		}

		TEST_METHOD(ChromaCflGpu422MatchesModel)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 96, -0.5, 0.0, 23u, 1.5, 2.0);
			CompareWithModel(dev, sc, -0.5, 0.0, ActiveRect(), "4:2:2");
		}

		TEST_METHOD(ChromaCflGpuLetterboxMatchesModelAndKeepsBars)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene pic = MakeScene(128, 96, 64, 48, -0.5, 0.0, 24u, 1.5, 2.0);
			const Scene sc = Embed(pic, 0, 16, 0, 16, 2, 2, 64.0, 512.0);
			ActiveRect rect;
			rect.left = 0; rect.top = 16; rect.right = 128; rect.bottom = 112;
			CompareWithModel(dev, sc, -0.5, 0.0, rect, "letterbox");
			const GpuOut g = RunGpu(dev, sc, -0.5, 0.0, rect);
			Assert::IsTrue(g.ok, Wide(g.error).c_str());
			for (int y = 0; y < sc.y.h; ++y)
				for (int x = 0; x < sc.y.w; ++x)
					if (y < rect.top || y >= rect.bottom)
						Assert::IsTrue(g.cb.at(x, y) == 512.0 && g.cr.at(x, y) == 512.0, L"bar pixels bit-exact on the GPU");
			// pillarbox as well
			const Scene pic2 = MakeScene(96, 96, 48, 48, -0.5, 0.0, 25u, 1.5, 2.0);
			const Scene sc2 = Embed(pic2, 16, 0, 16, 0, 2, 2, 64.0, 900.0);
			ActiveRect rect2;
			rect2.left = 16; rect2.top = 0; rect2.right = 112; rect2.bottom = 96;
			CompareWithModel(dev, sc2, -0.5, 0.0, rect2, "pillarbox");
			const GpuOut g2 = RunGpu(dev, sc2, -0.5, 0.0, rect2);
			Assert::IsTrue(g2.ok, Wide(g2.error).c_str());
			for (int y = 0; y < sc2.y.h; ++y)
				for (int x = 0; x < sc2.y.w; ++x)
					if (x < rect2.left || x >= rect2.right)
						Assert::IsTrue(g2.cb.at(x, y) == 900.0 && g2.cr.at(x, y) == 900.0, L"pillarbox bar pixels bit-exact on the GPU");
		}

		TEST_METHOD(ChromaCflGpuOffReturnsTheSameFrame)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Scene sc = MakeScene(64, 48, 32, 24, -0.5, 0.0, 26u, 1.0, 1.0);
			pl_tex luma = UploadR16(gpu, sc.y), chroma = UploadRg16(gpu, sc.cb, sc.cr);
			const pl_frame src = Frame2(luma, chroma, -0.5, 0.0);
			ChromaUpscale::Gpu::CflModule module;
			ChromaUpscale::Gpu::Params gp;   // mode Off
			pl_frame out{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
			Assert::AreEqual(std::string("off"), module.Status());
			Assert::IsTrue(out.num_planes == src.num_planes && out.planes[0].texture == luma && out.planes[1].texture == chroma, L"the same planes");
			Assert::IsTrue(out.planes[1].shift_x == src.planes[1].shift_x, L"the same siting");
			module.Release(gpu);
			pl_tex_destroy(gpu, &luma);
			pl_tex_destroy(gpu, &chroma);
		}

		TEST_METHOD(ChromaCflGpuOutputFrameIsFullSizeWithZeroShifts)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(64, 48, 32, 24, -0.5, 0.0, 27u, 1.0, 1.0);
			const GpuOut g = RunGpu(dev, sc, -0.5, 0.0, ActiveRect());
			Assert::IsTrue(g.ok, Wide(g.error).c_str());
			Assert::IsTrue(g.numPlanes == 2, L"2 planes");
			Assert::IsTrue(g.comps == 2 && g.map0 == PL_CHANNEL_CB && g.map1 == PL_CHANNEL_CR, L"CbCr, 2 components");
			Assert::IsTrue(g.texW == 64 && g.texH == 48, L"full-size chroma");
			Assert::IsTrue(g.shiftX == 0.0f && g.shiftY == 0.0f, L"zero shifts: libplacebo does not upsample again");
			Assert::IsTrue(g.lumaUnchanged, L"the luma plane passes through");
		}

		TEST_METHOD(ChromaCflGpuThreePlaneSourceMatchesTwoPlane)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(64, 48, 32, 24, -0.5, 0.0, 28u, 1.5, 1.5);
			const GpuOut two = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false);
			const GpuOut three = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), true);
			Assert::IsTrue(two.ok && three.ok, Wide(two.error + three.error).c_str());
			Assert::IsTrue(two.cb.v == three.cb.v && two.cr.v == three.cr.v, L"planar and semi-planar chroma give the same output");
		}

		TEST_METHOD(ChromaCflGpuDeterministic)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 29u, 1.5, 2.0);
			const GpuOut a = RunGpu(dev, sc, -0.5, 0.0, ActiveRect()), b = RunGpu(dev, sc, -0.5, 0.0, ActiveRect());
			Assert::IsTrue(a.ok && b.ok, Wide(a.error + b.error).c_str());
			Assert::IsTrue(a.cb.v == b.cb.v && a.cr.v == b.cr.v, L"two runs are bit-identical");
		}

		TEST_METHOD(ChromaCflGpu444IsNothing)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Scene sc = MakeScene(32, 24, 32, 24, 0.0, 0.0, 30u, 1.0, 1.0);
			pl_tex luma = UploadR16(gpu, sc.y), chroma = UploadRg16(gpu, sc.cb, sc.cr);
			const pl_frame src = Frame2(luma, chroma, 0.0, 0.0);
			ChromaUpscale::Gpu::CflModule module;
			ChromaUpscale::Gpu::Params gp;
			gp.settings.mode = ChromaUpscale::Mode::Cfl;
			pl_frame out{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
			Assert::AreEqual(std::string("4:4:4: nothing to upsample"), module.Status());
			Assert::IsTrue(out.planes[1].texture == chroma, L"the input frame");
			module.Release(gpu);
			pl_tex_destroy(gpu, &luma);
			pl_tex_destroy(gpu, &chroma);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ChromaCflGpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChromaCflGpuTiming)
		{
			// Hardware only (VP_TEST_GPU=hardware): Apply at 3840x2160 and 1920x1080 4:2:0, median wall time over 40 frames with
			// pl_gpu_finish per frame.
			if (!ExtendedRangeGpuProbe::OnHardwareGpu())
			{
				Logger::WriteMessage("ChromaCflGpuTiming: VP_TEST_GPU=hardware only; not run");
				return;
			}
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int sizes[2][2] = { { 3840, 2160 }, { 1920, 1080 } };
			for (const auto& sz : sizes)
			{
				const int w = sz[0], h = sz[1], cw = w / 2, ch = h / 2;
				std::vector<uint16_t> yd(static_cast<size_t>(w) * h), cd(static_cast<size_t>(cw) * ch * 2);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						yd[static_cast<size_t>(y) * w + x] = Raw(300.0 + 200.0 * (((x >> 6) + (y >> 6)) & 1) + ((x * 7 + y * 13) & 3));
				for (int y = 0; y < ch; ++y)
					for (int x = 0; x < cw; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * cw + x) * 2;
						cd[i] = Raw(500.0 + 60.0 * (((x >> 5) + (y >> 5)) & 1) + ((x * 5 + y * 3) & 3));
						cd[i + 1] = Raw(520.0 - 40.0 * (((x >> 5) + (y >> 5)) & 1) + ((x * 3 + y * 7) & 3));
					}
				pl_tex luma = UploadRaw(gpu, w, h, "r16", yd), chroma = UploadRaw(gpu, cw, ch, "rg16", cd);
				const pl_frame src = Frame2(luma, chroma, -0.5, 0.0);
				ChromaUpscale::Gpu::CflModule module;
				ChromaUpscale::Gpu::Params gp;
				gp.settings.mode = ChromaUpscale::Mode::Cfl;
				std::vector<double> ms;
				for (int k = 0; k < 40; ++k)
				{
					pl_frame out{};
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
					pl_gpu_finish(gpu);
					const auto t1 = std::chrono::steady_clock::now();
					ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				char text[200];
				std::snprintf(text, sizeof text, "chroma upscale CfL %dx%d 4:2:0: median %.3f ms over %zu frames (first %.3f ms); textures %.0f MB", w, h,
					ms[ms.size() / 2], ms.size(), ms.front(), ChromaUpscale::Gpu::CflModule::TextureBytes(w, h, cw, ch) / 1048576.0);
				Logger::WriteMessage(text);
				module.Release(gpu);
				pl_tex_destroy(gpu, &luma);
				pl_tex_destroy(gpu, &chroma);
			}
		}
	};
}
