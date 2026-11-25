// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale: the CfL chroma upsampler. ChromaCflModelTests exercise the CPU reference model (ChromaCfl.h);
// ChromaCflGpuTests compare the GPU passes (ChromaCflGpu) with it on a libplacebo device: WARP (software, runs in the CPU
// tier) unless VP_TEST_GPU=hardware. The timing test is hardware only (category GpuHardware).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCflGpu.h>

#include <microsoft_directshow/MadVRShaderLoader.h>

#include <libplacebo/cache.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ChromaCflTestsNs
{
	using ChromaUpscale::ActiveRect;
	using ChromaUpscale::Plane;
	constexpr double kPi = 3.14159265358979323846;
	// Compact vs Full shared grain product: worst output difference allowed, in codes (WARP-measured value + margin).
	constexpr double kCompactVsFullMaxCodes = 0.05;

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
	// shared: pass the grain.split products computed once outside the module (one producer, many readers).
	GpuOut RunGpu(GpuDevice& dev, const Scene& s, double sx, double sy, const ActiveRect& rect, bool threePlane = false, bool guard = true, bool shared = false,
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr)
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
		gp.arena = arena;   // SA7: null = private textures
		GrainSplit::Gpu::Splitter pL, pB, pR;
		RegionEvidence::GrainProduct sharedProduct;   // the one grain.split node, GPU form per plane
		if (shared && !threePlane)
		{
			const double cu = 64.0 / 65535.0, lines = s.y.h, linesC = lines / (static_cast<double>(s.y.h) / s.cb.h);
			const bool made = pL.Compute(gpu, dev.dispatch, luma, 0, cu, ChromaUpscale::Scale(lines), ChromaUpscale::BlockPx(lines)) &&
				pB.Compute(gpu, dev.dispatch, chroma, 0, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC)) &&
				pR.Compute(gpu, dev.dispatch, chroma, 1, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
			Assert::IsTrue(made, L"the shared products are made");
			sharedProduct.gpu[0] = &pL.Out(); sharedProduct.gpu[1] = &pB.Out(); sharedProduct.gpu[2] = &pR.Out();
			gp.grain = RegionEvidence::GrainView(&sharedProduct);
		}
		pl_frame out{};
		if (arena) arena->BeginFrame();
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
		pL.Release(gpu); pB.Release(gpu); pR.Release(gpu);
		for (pl_tex* t : { &luma, &chroma, &cbT, &crT })
			if (*t) pl_tex_destroy(gpu, t);
		return o;
	}

	ChromaUpscale::Result RunModel(const Scene& s, double sx, double sy, const ActiveRect& rect)
	{
		return ChromaUpscale::Upsample(s.y, s.cb, s.cr, sx, sy, rect, 10, 0.0);
	}

	// --- The shared grain products: absent, misfit and Compact (SA2 B3) ---
	enum class CflShared { Absent, EmptyProduct, Full, Compact, WrongBlock, WrongUnit, WrongComponent, CbMisfitOnly, StaleSeq, StaleVersion, Shed };

	struct CflFrameOut
	{
		Plane cb, cr;
		std::string status, id;
		GrainSplit::GrainStatus grain = GrainSplit::GrainStatus::Ring;
		bool engaged = false;
	};

	// One module, one scene (two-plane layout), one Apply per entry of `seq` with that kind of shared product (built on the input
	// planes, as the host's node would).
	std::vector<CflFrameOut> RunCflShared(GpuDevice& dev, const Scene& s, const std::vector<CflShared>& seq)
	{
		std::vector<CflFrameOut> res;
		pl_gpu gpu = dev.Gpu();
		pl_tex luma = UploadR16(gpu, s.y), chroma = UploadRg16(gpu, s.cb, s.cr);
		const pl_frame src = Frame2(luma, chroma, -0.5, 0.0);
		ChromaUpscale::Gpu::CflModule module;
		GrainSplit::Gpu::Splitter fl, fb, fr, cl, cb, cr, wl, wb, wr;   // Full, Compact, wrong
		const double cu = 64.0 / 65535.0, lines = s.y.h, linesC = lines / (static_cast<double>(s.y.h) / s.cb.h);
		const double sL = ChromaUpscale::Scale(lines), sC = ChromaUpscale::Scale(linesC);
		const int bL = ChromaUpscale::BlockPx(lines), bC = ChromaUpscale::BlockPx(linesC);
		GrainSplit::Gpu::Options compact;
		compact.storage = GrainSplit::Storage::Compact;
		GrainSplit::Gpu::Frame tamperedL, tamperedB, tamperedR;
		GrainSplit::Gpu::Options stale7;   // computed for source frame 7 (the frame the reader asks for)
		stale7.seq = 7;
		GrainSplit::Gpu::Options stale100;   // computed for another frame
		stale100.seq = 100;
		for (const CflShared kind : seq)
		{
			RegionEvidence::GrainProduct product;
			bool made = true;
			switch (kind)
			{
			case CflShared::StaleSeq:
			case CflShared::StaleVersion:
			{
				const GrainSplit::Gpu::Options& o = kind == CflShared::StaleSeq ? stale100 : stale7;
				made = fl.Compute(gpu, dev.dispatch, luma, 0, cu, sL, bL, GrainSplit::Params(), o) &&
					fb.Compute(gpu, dev.dispatch, chroma, 0, cu, sC, bC, GrainSplit::Params(), o) &&
					fr.Compute(gpu, dev.dispatch, chroma, 1, cu, sC, bC, GrainSplit::Params(), o);
				product.gpu[0] = &fl.Out(); product.gpu[1] = &fb.Out(); product.gpu[2] = &fr.Out();
				if (kind == CflShared::StaleVersion)
				{
					tamperedL = fl.Out(); tamperedB = fb.Out(); tamperedR = fr.Out();
					tamperedL.version = tamperedB.version = tamperedR.version = GrainSplit::kUnitVersion + 1;
					product.gpu[0] = &tamperedL; product.gpu[1] = &tamperedB; product.gpu[2] = &tamperedR;
				}
				break;
			}
			case CflShared::Shed:
			case CflShared::Full:
			case CflShared::CbMisfitOnly:
				made = fl.Compute(gpu, dev.dispatch, luma, 0, cu, sL, bL) && fb.Compute(gpu, dev.dispatch, chroma, 0, cu, sC, bC) &&
					fr.Compute(gpu, dev.dispatch, chroma, 1, cu, sC, bC);
				product.gpu[0] = &fl.Out(); product.gpu[1] = &fb.Out(); product.gpu[2] = &fr.Out();
				if (kind == CflShared::CbMisfitOnly)
				{
					made = made && wb.Compute(gpu, dev.dispatch, chroma, 0, cu * 2.0, sC, bC);
					product.gpu[1] = &wb.Out();
				}
				break;
			case CflShared::Compact:
				made = cl.Compute(gpu, dev.dispatch, luma, 0, cu, sL, bL, GrainSplit::Params(), compact) &&
					cb.Compute(gpu, dev.dispatch, chroma, 0, cu, sC, bC, GrainSplit::Params(), compact) &&
					cr.Compute(gpu, dev.dispatch, chroma, 1, cu, sC, bC, GrainSplit::Params(), compact);
				product.gpu[0] = &cl.Out(); product.gpu[1] = &cb.Out(); product.gpu[2] = &cr.Out();
				break;
			case CflShared::WrongBlock:
				made = wl.Compute(gpu, dev.dispatch, luma, 0, cu, sL, bL * 2) && wb.Compute(gpu, dev.dispatch, chroma, 0, cu, sC, bC * 2) &&
					wr.Compute(gpu, dev.dispatch, chroma, 1, cu, sC, bC * 2);
				product.gpu[0] = &wl.Out(); product.gpu[1] = &wb.Out(); product.gpu[2] = &wr.Out();
				break;
			case CflShared::WrongUnit:
				made = wl.Compute(gpu, dev.dispatch, luma, 0, cu * 2.0, sL, bL) && wb.Compute(gpu, dev.dispatch, chroma, 0, cu * 2.0, sC, bC) &&
					wr.Compute(gpu, dev.dispatch, chroma, 1, cu * 2.0, sC, bC);
				product.gpu[0] = &wl.Out(); product.gpu[1] = &wb.Out(); product.gpu[2] = &wr.Out();
				break;
			case CflShared::WrongComponent:   // Cb and Cr swapped
				made = fl.Compute(gpu, dev.dispatch, luma, 0, cu, sL, bL) && wb.Compute(gpu, dev.dispatch, chroma, 1, cu, sC, bC) &&
					wr.Compute(gpu, dev.dispatch, chroma, 0, cu, sC, bC);
				product.gpu[0] = &fl.Out(); product.gpu[1] = &wb.Out(); product.gpu[2] = &wr.Out();
				break;
			default:
				break;
			}
			Assert::IsTrue(made, L"the shared products are made");
			ChromaUpscale::Gpu::Params gp;
			gp.settings.mode = ChromaUpscale::Mode::Cfl;
			if (kind != CflShared::Absent) gp.grain = RegionEvidence::GrainView(&product);
			if (kind == CflShared::StaleSeq || kind == CflShared::StaleVersion) gp.grainSeq = 7;
			if (kind == CflShared::Shed) gp.grainShed = true;
			pl_frame out{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
			CflFrameOut o;
			std::vector<uint16_t> raw;
			int nc = 0;
			Assert::IsTrue(DownloadRaw(gpu, out.planes[1].texture, raw, nc), L"the output is readable");
			o.cb = Plane(out.planes[1].texture->params.w, out.planes[1].texture->params.h);
			o.cr = o.cb;
			for (size_t i = 0; i < o.cb.v.size(); ++i) { o.cb.v[i] = raw[i * nc] / 64.0; o.cr.v[i] = raw[i * nc + 1] / 64.0; }
			o.status = module.Status();
			o.grain = module.LastGrainStatus();
			const FallbackCatalog::Decision d = module.GrainFallbackDecision();
			o.engaged = d.engaged;
			o.id = d.id;
			res.push_back(o);
		}
		module.Release(gpu);
		for (GrainSplit::Gpu::Splitter* sp : { &fl, &fb, &fr, &cl, &cb, &cr, &wl, &wb, &wr }) sp->Release(gpu);
		pl_tex_destroy(gpu, &luma);
		pl_tex_destroy(gpu, &chroma);
		return res;
	}

	double CflWorstDiff(const CflFrameOut& a, const CflFrameOut& b) { return (std::max)(MaxDiff(a.cb, b.cb), MaxDiff(a.cr, b.cr)); }

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

	// --- Q#7 (GEM-066): dynamic CfL_Prediction.glsl chroma siting offset helpers ---
	struct DummyPlFrame
	{
		pl_tex_t lumaTex;
		pl_tex_t chromaTex;
		pl_tex_t crTex;
		pl_frame frame;

		DummyPlFrame()
		{
			std::memset(&lumaTex, 0, sizeof(lumaTex));
			std::memset(&chromaTex, 0, sizeof(chromaTex));
			std::memset(&crTex, 0, sizeof(crTex));
			std::memset(&frame, 0, sizeof(frame));
		}

		DummyPlFrame(const DummyPlFrame&) = delete;
		DummyPlFrame& operator=(const DummyPlFrame&) = delete;

		void Init2Plane(int lw, int lh, int cw, int ch, float sx, float sy)
		{
			std::memset(&lumaTex, 0, sizeof(lumaTex));
			std::memset(&chromaTex, 0, sizeof(chromaTex));
			std::memset(&frame, 0, sizeof(frame));
			lumaTex.params.w = lw;
			lumaTex.params.h = lh;
			chromaTex.params.w = cw;
			chromaTex.params.h = ch;

			frame.num_planes = 2;
			frame.planes[0].texture = &lumaTex;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.planes[1].texture = &chromaTex;
			frame.planes[1].components = 2;
			frame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			frame.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			frame.planes[1].shift_x = sx;
			frame.planes[1].shift_y = sy;
			SetRepr(frame);
		}

		void Init3Plane(int lw, int lh, int cw, int ch, float sx, float sy)
		{
			std::memset(&lumaTex, 0, sizeof(lumaTex));
			std::memset(&chromaTex, 0, sizeof(chromaTex));
			std::memset(&crTex, 0, sizeof(crTex));
			std::memset(&frame, 0, sizeof(frame));
			lumaTex.params.w = lw;
			lumaTex.params.h = lh;
			chromaTex.params.w = cw;
			chromaTex.params.h = ch;
			crTex.params.w = cw;
			crTex.params.h = ch;

			frame.num_planes = 3;
			frame.planes[0].texture = &lumaTex;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.planes[1].texture = &chromaTex;
			frame.planes[1].components = 1;
			frame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			frame.planes[1].shift_x = sx;
			frame.planes[1].shift_y = sy;
			frame.planes[2].texture = &crTex;
			frame.planes[2].components = 1;
			frame.planes[2].component_mapping[0] = PL_CHANNEL_CR;
			frame.planes[2].shift_x = sx;
			frame.planes[2].shift_y = sy;
			SetRepr(frame);
		}

		void InitRgb1Plane(int w, int h)
		{
			std::memset(&lumaTex, 0, sizeof(lumaTex));
			std::memset(&frame, 0, sizeof(frame));
			lumaTex.params.w = w;
			lumaTex.params.h = h;

			frame.num_planes = 1;
			frame.planes[0].texture = &lumaTex;
			frame.planes[0].components = 3;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_R;
			frame.planes[0].component_mapping[1] = PL_CHANNEL_G;
			frame.planes[0].component_mapping[2] = PL_CHANNEL_B;
			frame.repr.sys = PL_COLOR_SYSTEM_RGB;
			frame.repr.levels = PL_COLOR_LEVELS_FULL;
			frame.color = pl_color_space_bt709;
		}
	};

	struct DummyCflHook
	{
		pl_var_data valX;
		pl_var_data valY;
		pl_hook_par pars[2];
		pl_hook hook;

		DummyCflHook(float initX, float initY)
		{
			std::memset(&valX, 0, sizeof(valX));
			std::memset(&valY, 0, sizeof(valY));
			std::memset(&pars, 0, sizeof(pars));
			std::memset(&hook, 0, sizeof(hook));

			valX.f = initX;
			valY.f = initY;

			pars[0].name = "chroma_offset_x";
			pars[0].type = PL_VAR_FLOAT;
			pars[0].mode = PL_HOOK_PAR_DYNAMIC;
			pars[0].data = &valX;
			pars[0].initial.f = -0.5f;
			pars[0].minimum.f = -FLT_MAX;
			pars[0].maximum.f = FLT_MAX;

			pars[1].name = "chroma_offset_y";
			pars[1].type = PL_VAR_FLOAT;
			pars[1].mode = PL_HOOK_PAR_DYNAMIC;
			pars[1].data = &valY;
			pars[1].initial.f = 0.0f;
			pars[1].minimum.f = -FLT_MAX;
			pars[1].maximum.f = FLT_MAX;

			hook.stages = PL_HOOK_CHROMA_INPUT;
			hook.parameters = pars;
			hook.num_parameters = 2;
		}

		DummyCflHook(const DummyCflHook&) = delete;
		DummyCflHook& operator=(const DummyCflHook&) = delete;
	};

	std::string LoadBundledCflShader()
	{
		std::string dir = __FILE__;
		for (int level = 0; level < 10; ++level)
		{
			const size_t sep = dir.find_last_of("\\/");
			if (sep == std::string::npos)
				break;
			dir.resize(sep);
			std::ifstream in(dir + "\\shaders\\CfL_Prediction.glsl", std::ios::binary);
			if (in)
				return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		return {};
	}

	bool HasDynamicFloatParam(const std::string& src, const char* paramName)
	{
		const std::string needle = std::string("//!PARAM ") + paramName;
		const size_t pos = src.find(needle);
		if (pos == std::string::npos)
			return false;
		const size_t nextParam = src.find("//!PARAM ", pos + needle.size());
		const size_t nextHook = src.find("//!HOOK ", pos + needle.size());
		size_t blockEnd = src.size();
		if (nextParam != std::string::npos) blockEnd = (std::min)(blockEnd, nextParam);
		if (nextHook != std::string::npos) blockEnd = (std::min)(blockEnd, nextHook);
		const std::string block = src.substr(pos, blockEnd - pos);
		return block.find("//!TYPE DYNAMIC float") != std::string::npos;
	}

	const pl_hook_par* FindHookParam(const pl_hook* hook, const char* name)
	{
		if (!hook || !hook->parameters || !name)
			return nullptr;
		for (int i = 0; i < hook->num_parameters; ++i)
		{
			if (hook->parameters[i].name && std::strcmp(hook->parameters[i].name, name) == 0)
				return &hook->parameters[i];
		}
		return nullptr;
	}

	// --- Q#8 (GEM-074): unquantized float32 GPU parity, tiled workgroup seam & inline-grain helpers ---
	constexpr double kFloat32CodePeak = 1023.0;

	void QuantiseFloat32(Plane& p)
	{
		for (double& v : p.v)
		{
			const double clamped = (std::min)(kFloat32CodePeak, (std::max)(0.0, v));
			const float norm = static_cast<float>(clamped / kFloat32CodePeak);
			v = static_cast<double>(norm) * kFloat32CodePeak;
		}
	}

	pl_fmt FindFloat32PlaneFormat(pl_gpu gpu, int comps)
	{
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
		return pl_find_fmt(gpu, PL_FMT_FLOAT, comps, 32, 32, caps);
	}

	pl_tex UploadR32f(pl_gpu gpu, const Plane& y)
	{
		pl_fmt fmt = FindFloat32PlaneFormat(gpu, 1);
		if (!fmt) return nullptr;
		std::vector<float> data(y.v.size());
		for (size_t i = 0; i < data.size(); ++i)
			data[i] = static_cast<float>(y.v[i] / kFloat32CodePeak);
		pl_tex_params p{};
		p.w = y.w; p.h = y.h; p.format = fmt; p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	pl_tex UploadRg32f(pl_gpu gpu, const Plane& cb, const Plane& cr)
	{
		pl_fmt fmt = FindFloat32PlaneFormat(gpu, 2);
		if (!fmt) return nullptr;
		std::vector<float> data(cb.v.size() * 2);
		for (size_t i = 0; i < cb.v.size(); ++i)
		{
			data[i * 2] = static_cast<float>(cb.v[i] / kFloat32CodePeak);
			data[i * 2 + 1] = static_cast<float>(cr.v[i] / kFloat32CodePeak);
		}
		pl_tex_params p{};
		p.w = cb.w; p.h = cb.h; p.format = fmt; p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	bool DownloadFloat32(pl_gpu gpu, pl_tex tex, std::vector<float>& out, int& comps)
	{
		if (!tex) return false;
		const pl_fmt fmt = tex->params.format;
		if (!fmt || fmt->type != PL_FMT_FLOAT || fmt->component_depth[0] != 32) return false;
		comps = fmt->num_components;
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = fmt; rp.host_readable = true; rp.blit_dst = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return false;
		pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
		pl_tex_blit(gpu, &bp);
		out.assign(static_cast<size_t>(rp.w) * rp.h * comps, 0.0f);
		pl_tex_transfer_params d{}; d.tex = readable; d.ptr = out.data();
		const bool ok = pl_tex_download(gpu, &d);
		pl_tex_destroy(gpu, &readable);
		return ok;
	}

	void SetReprFloat32(pl_frame& f)
	{
		f.repr.sys = PL_COLOR_SYSTEM_BT_709;
		f.repr.levels = PL_COLOR_LEVELS_LIMITED;
		f.repr.bits.sample_depth = 32;
		f.repr.bits.color_depth = 10;
		f.repr.bits.bit_shift = 0;
		f.color = pl_color_space_bt709;
	}

	pl_frame Frame2Float32(pl_tex luma, pl_tex chroma, double sx, double sy)
	{
		pl_frame f{};
		f.num_planes = 2;
		f.planes[0].texture = luma; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
		f.planes[1].texture = chroma; f.planes[1].components = 2;
		f.planes[1].component_mapping[0] = PL_CHANNEL_CB; f.planes[1].component_mapping[1] = PL_CHANNEL_CR;
		f.planes[1].shift_x = static_cast<float>(sx); f.planes[1].shift_y = static_cast<float>(sy);
		SetReprFloat32(f);
		return f;
	}

	GpuOut RunGpuFloat32(GpuDevice& dev, const Scene& s, double sx, double sy, const ActiveRect& rect, bool guard = true)
	{
		GpuOut o;
		pl_gpu gpu = dev.Gpu();
		pl_tex luma = UploadR32f(gpu, s.y);
		pl_tex chroma = UploadRg32f(gpu, s.cb, s.cr);
		ChromaUpscale::Gpu::CflModule module;
		struct FloatCleanup
		{
			pl_gpu gpu;
			ChromaUpscale::Gpu::CflModule* mod;
			pl_tex* luma;
			pl_tex* chroma;
			FloatCleanup(pl_gpu g, ChromaUpscale::Gpu::CflModule* m, pl_tex* l, pl_tex* c)
				: gpu(g), mod(m), luma(l), chroma(c) {}
			~FloatCleanup()
			{
				if (mod) mod->Release(gpu);
				if (chroma && *chroma) pl_tex_destroy(gpu, chroma);
				if (luma && *luma) pl_tex_destroy(gpu, luma);
			}
			FloatCleanup(const FloatCleanup&) = delete;
			FloatCleanup& operator=(const FloatCleanup&) = delete;
		} cleanup(gpu, &module, &luma, &chroma);

		if (!luma || !chroma)
		{
			o.error = "could not allocate r32f/rg32f source textures";
			return o;
		}
		const pl_frame src = Frame2Float32(luma, chroma, sx, sy);
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
			std::vector<float> raw;
			int nc = 0;
			if (DownloadFloat32(gpu, ct, raw, nc))
			{
				o.cb = Plane(o.texW, o.texH);
				o.cr = Plane(o.texW, o.texH);
				for (size_t i = 0; i < o.cb.v.size(); ++i)
				{
					o.cb.v[i] = static_cast<double>(raw[i * nc]) * kFloat32CodePeak;
					o.cr.v[i] = static_cast<double>(raw[i * nc + 1]) * kFloat32CodePeak;
				}
			}
			else
			{
				o.ok = false;
				o.error = "the float32 output texture could not be read back";
			}
		}
		return o;
	}

	// Rich 4:2:0 scene crossing multiple 16x16 luma & chroma workgroup seams (x=15/16, 31/32, 63/64; y=15/16, 31/32, 47/48),
	// non-zero slopes right up to all 4 image borders, sharp step edges, diagonal gradients, independent chroma transitions,
	// and high-frequency grain (raw != clean). Quantised to exact float32 normalized inputs so CPU and GPU see identical bits.
	Scene MakeTiledEdgeScene(int w, int h, int cw, int ch, double sx, double sy, uint32_t seed, double lumaSigma, double chromaSigma)
	{
		Plane Y(w, h), tcb(w, h), tcr(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const double L = 440.0
					+ 150.0 * std::tanh((x - 31.5) / 0.8)
					- 120.0 * std::tanh((y - 31.5) / 0.8)
					+ 85.0 * std::tanh((0.75 * x + 0.65 * y - 64.0) / 1.0)
					+ 55.0 * std::sin(2.0 * kPi * x / 19.0 + 0.3) * std::cos(2.0 * kPi * y / 23.0 + 0.5)
					+ 0.7 * x - 0.5 * y;
				const double Lc = (std::min)(900.0, (std::max)(100.0, L));
				Y.at(x, y) = Lc;
				tcb.at(x, y) = 520.0 + 0.38 * (Lc - 440.0) + 45.0 * std::tanh((x - 63.5) / 1.1) + 28.0 * std::cos(2.0 * kPi * (x + 2.0 * y) / 37.0);
				tcr.at(x, y) = 500.0 - 0.32 * (Lc - 440.0) - 40.0 * std::tanh((y - 47.5) / 1.1) + 24.0 * std::sin(2.0 * kPi * (2.0 * x - y) / 41.0);
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
		QuantiseFloat32(s.y); QuantiseFloat32(s.cb); QuantiseFloat32(s.cr);
		return s;
	}

	// Broken CPU baseline A/C (in-test negative control): same CfL pipeline, except chroma grain add-back is either
	// dropped (wrongCropOrigin == false: G = 0, simulating omitting inline raw - cc after m.gc removal) or fetched at
	// the un-shifted crop index (wrongCropOrigin == true: raw(i, j) - cc(i, j) instead of raw(cx0 + i, cy0 + j) - cc(i, j)).
	ChromaUpscale::Result UpsampleBrokenInlineGrain(const Plane& Y, const Plane& Cb, const Plane& Cr, double sx, double sy,
		const ActiveRect& activeRect, int colorDepth, bool wrongCropOrigin)
	{
		ChromaUpscale::CflParams p;
		p.grainGuard = true;
		ChromaUpscale::Result res = ChromaUpscale::Upsample(Y, Cb, Cr, sx, sy, activeRect, colorDepth, 0.0, p);
		const int W = Y.w, H = Y.h, cw = Cb.w, ch = Cb.h;
		const double fx = static_cast<double>(W) / cw, fy = static_cast<double>(H) / ch;
		const ActiveRect rc = activeRect.Resolve(W, H);
		const ChromaUpscale::Rect lr = { rc.left, rc.top, rc.right, rc.bottom };
		const ChromaUpscale::Rect cr = ChromaUpscale::ChromaRect(rc, fx, fy, cw, ch);
		const double linesC = static_cast<double>(H) / fy;
		const Plane cbCrop = ChromaUpscale::CropPlane(Cb, cr);
		const Plane crCrop = ChromaUpscale::CropPlane(Cr, cr);
		const GrainSplit::Split sb = GrainSplit::Compute(cbCrop, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
		const GrainSplit::Split sr = GrainSplit::Compute(crCrop, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
		Plane badGB(cbCrop.w, cbCrop.h, 0.0), badGR(crCrop.w, crCrop.h, 0.0);
		if (wrongCropOrigin)
		{
			for (int j = 0; j < cbCrop.h; ++j)
				for (int i = 0; i < cbCrop.w; ++i)
				{
					badGB.at(i, j) = Cb.get(i, j) - sb.clean.at(i, j);
					badGR.at(i, j) = Cr.get(i, j) - sr.clean.at(i, j);
				}
		}
		for (int y = lr.y0; y < lr.y1; ++y)
			for (int x = lr.x0; x < lr.x1; ++x)
			{
				const double u = (x + 0.5 - sx) / fx - 0.5 - cr.x0;
				const double v = (y + 0.5 - sy) / fy - 0.5 - cr.y0;
				const double trueGB = ChromaUpscale::CatmullRomAt(sb.grain, u, v);
				const double trueGR = ChromaUpscale::CatmullRomAt(sr.grain, u, v);
				const double replGB = wrongCropOrigin ? ChromaUpscale::CatmullRomAt(badGB, u, v) : 0.0;
				const double replGR = wrongCropOrigin ? ChromaUpscale::CatmullRomAt(badGR, u, v) : 0.0;
				res.cb.at(x, y) = res.cb.at(x, y) - trueGB + replGB;
				res.cr.at(x, y) = res.cr.at(x, y) - trueGR + replGR;
			}
		return res;
	}

	// Broken CPU baseline B (in-test negative control): omits shared-memory halo aprons across 16x16 workgroup boundaries
	// in STATS (5x5 regression), AVG (5x5 box), and UP (4x4 Catmull-Rom) by clamping each workgroup's reads to its own
	// unpadded local tile. Proves our seam assertions genuinely catch missing/broken shared-memory halos on the exact same input.
	ChromaUpscale::Result UpsampleBrokenHaloOmitted(const Plane& Y, const Plane& Cb, const Plane& Cr, double sx, double sy,
		const ActiveRect& activeRect, int colorDepth)
	{
		ChromaUpscale::CflParams p;
		p.grainGuard = true;
		const int W = Y.w, H = Y.h, cw = Cb.w, ch = Cb.h;
		const double fx = static_cast<double>(W) / cw, fy = static_cast<double>(H) / ch;
		const ActiveRect rc = activeRect.Resolve(W, H);
		const ChromaUpscale::Rect lr = { rc.left, rc.top, rc.right, rc.bottom };
		const ChromaUpscale::Rect cr = ChromaUpscale::ChromaRect(rc, fx, fy, cw, ch);
		const double lines = static_cast<double>(H), linesC = lines / fy;
		const Plane yCrop = ChromaUpscale::CropPlane(Y, lr);
		const Plane cbCrop = ChromaUpscale::CropPlane(Cb, cr), crCrop = ChromaUpscale::CropPlane(Cr, cr);
		const Plane yc = GrainSplit::Compute(yCrop, ChromaUpscale::Scale(lines), ChromaUpscale::BlockPx(lines)).clean;
		const GrainSplit::Split sb = GrainSplit::Compute(cbCrop, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
		const GrainSplit::Split sr = GrainSplit::Compute(crCrop, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
		const Plane ylo = ChromaUpscale::TentAtSites(yc, cr.x0, cr.y0, cr.W(), cr.H(), fx, fy, sx, sy, lr.x0, lr.y0);
		double peak = 1.0;
		for (int i = 0; i < colorDepth; ++i) peak *= 2.0;
		const double eps = (p.epsFrac * (peak - 1.0)) * (p.epsFrac * (peak - 1.0));

		auto regressTileClamped = [&](const Plane& cc) {
			const int w = ylo.w, h = ylo.h;
			const double n = static_cast<double>((2 * p.radius + 1) * (2 * p.radius + 1));
			Plane a(w, h), b(w, h);
			ChromaUpscale::Regression r;
			r.rho2 = Plane(w, h); r.lo = Plane(w, h); r.hi = Plane(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const int tx0 = (x / 16) * 16, tx1 = (std::min)(w - 1, tx0 + 15);
					const int ty0 = (y / 16) * 16, ty1 = (std::min)(h - 1, ty0 + 15);
					double sySum = 0.0, scSum = 0.0;
					for (int j = -p.radius; j <= p.radius; ++j)
						for (int i = -p.radius; i <= p.radius; ++i)
						{
							const int xx = (std::max)(tx0, (std::min)(tx1, x + i));
							const int yy = (std::max)(ty0, (std::min)(ty1, y + j));
							sySum += ylo.at(xx, yy); scSum += cc.at(xx, yy);
						}
					const double mY = sySum / n, mC = scSum / n;
					double syy = 0.0, scc = 0.0, syc = 0.0;
					for (int j = -p.radius; j <= p.radius; ++j)
						for (int i = -p.radius; i <= p.radius; ++i)
						{
							const int xx = (std::max)(tx0, (std::min)(tx1, x + i));
							const int yy = (std::max)(ty0, (std::min)(ty1, y + j));
							const double dy = ylo.at(xx, yy) - mY, dc = cc.at(xx, yy) - mC;
							syy += dy * dy; scc += dc * dc; syc += dy * dc;
						}
					const double vY = (std::max)(syy / n, 0.0), vC = (std::max)(scc / n, 0.0), cov = syc / n;
					const double aa = cov / (vY + eps);
					a.at(x, y) = aa;
					b.at(x, y) = mC - aa * mY;
					r.rho2.at(x, y) = RampMath::Clamp01<double>(cov * cov / ((vY + eps) * (vC + eps)));
					double lo = 1e300, hi = -1e300;
					for (int j = -p.clampR; j <= p.clampR; ++j)
						for (int i = -p.clampR; i <= p.clampR; ++i)
						{
							const int xx = (std::max)(tx0, (std::min)(tx1, x + i));
							const int yy = (std::max)(ty0, (std::min)(ty1, y + j));
							const double v = cc.at(xx, yy);
							lo = (std::min)(lo, v); hi = (std::max)(hi, v);
						}
					r.lo.at(x, y) = lo; r.hi.at(x, y) = hi;
				}
			r.a = Plane(w, h); r.b = Plane(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const int tx0 = (x / 16) * 16, tx1 = (std::min)(w - 1, tx0 + 15);
					const int ty0 = (y / 16) * 16, ty1 = (std::min)(h - 1, ty0 + 15);
					double sa = 0.0, sb2 = 0.0;
					for (int j = -p.radius; j <= p.radius; ++j)
						for (int i = -p.radius; i <= p.radius; ++i)
						{
							const int xx = (std::max)(tx0, (std::min)(tx1, x + i));
							const int yy = (std::max)(ty0, (std::min)(ty1, y + j));
							sa += a.at(xx, yy); sb2 += b.at(xx, yy);
						}
					r.a.at(x, y) = sa / n; r.b.at(x, y) = sb2 / n;
				}
			return r;
		};

		const ChromaUpscale::Regression rB = regressTileClamped(sb.clean), rR = regressTileClamped(sr.clean);
		Plane plB(ylo.w, ylo.h), plR(ylo.w, ylo.h);
		for (size_t i = 0; i < ylo.v.size(); ++i)
		{
			plB.v[i] = rB.a.v[i] * ylo.v[i] + rB.b.v[i];
			plR.v[i] = rR.a.v[i] * ylo.v[i] + rR.b.v[i];
		}

		ChromaUpscale::Result res;
		res.cb = Plane(W, H); res.cr = Plane(W, H);
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
			{
				const double uPlane = (x + 0.5 - sx) / fx - 0.5, vPlane = (y + 0.5 - sy) / fy - 0.5;
				const bool inside = x >= lr.x0 && x < lr.x1 && y >= lr.y0 && y < lr.y1;
				if (!inside)
				{
					res.cb.at(x, y) = Cb.get(static_cast<int>(std::floor(uPlane)), static_cast<int>(std::floor(vPlane)));
					res.cr.at(x, y) = Cr.get(static_cast<int>(std::floor(uPlane)), static_cast<int>(std::floor(vPlane)));
					continue;
				}
				const double u = uPlane - cr.x0, v = vPlane - cr.y0;
				const int lx0 = (x / 16) * 16, lx1 = (std::min)(W - 1, lx0 + 15);
				const int ly0 = (y / 16) * 16, ly1 = (std::min)(H - 1, ly0 + 15);
				const int cxLo = (std::max)(0, (std::min)(cr.W() - 1, static_cast<int>(std::floor((lx0 + 0.5 - sx) / fx - 0.5 - cr.x0))));
				const int cxHi = (std::max)(cxLo, (std::min)(cr.W() - 1, static_cast<int>(std::floor((lx1 + 0.5 - sx) / fx - 0.5 - cr.x0))));
				const int cyLo = (std::max)(0, (std::min)(cr.H() - 1, static_cast<int>(std::floor((ly0 + 0.5 - sy) / fy - 0.5 - cr.y0))));
				const int cyHi = (std::max)(cyLo, (std::min)(cr.H() - 1, static_cast<int>(std::floor((ly1 + 0.5 - sy) / fy - 0.5 - cr.y0))));
				const double yv = yc.at(x - lr.x0, y - lr.y0);
				for (int c = 0; c < 2; ++c)
				{
					const ChromaUpscale::Regression& g = c == 0 ? rB : rR;
					const Plane& cc = c == 0 ? sb.clean : sr.clean;
					const Plane& gr = c == 0 ? sb.grain : sr.grain;
					const double A = ChromaUpscale::CatmullRomClamped(g.a, u, v, cxLo, cxHi, cyLo, cyHi);
					const double Bv = ChromaUpscale::CatmullRomClamped(g.b, u, v, cxLo, cxHi, cyLo, cyHi);
					const double M = RampMath::Clamp01<double>(ChromaUpscale::CatmullRomClamped(g.rho2, u, v, cxLo, cxHi, cyLo, cyHi));
					const double base = ChromaUpscale::CatmullRomClamped(cc, u, v, cxLo, cxHi, cyLo, cyHi);
					const double LO = ChromaUpscale::CatmullRomClamped(g.lo, u, v, cxLo, cxHi, cyLo, cyHi);
					const double HI = ChromaUpscale::CatmullRomClamped(g.hi, u, v, cxLo, cxHi, cyLo, cyHi);
					const double G = ChromaUpscale::CatmullRomClamped(gr, u, v, cxLo, cxHi, cyLo, cyHi);
					const double PL = ChromaUpscale::CatmullRomClamped(c == 0 ? plB : plR, u, v, cxLo, cxHi, cyLo, cyHi);
					const double lo = (std::min)(LO, HI), hi = (std::max)(LO, HI);
					const double pred = (std::max)(lo, (std::min)(hi, A * yv + Bv));
					(c == 0 ? res.cb : res.cr).at(x, y) = base + M * (pred - PL) + G;
				}
			}
		res.status = "ok";
		return res;
	}

	// --- Q#9 (GEM-076): dynamic CfL uniform compile-tracking & test-scene helpers ---
	struct ShaderCompileSnapshot
	{
		int cacheObjects = 0;
		uint64_t cacheSignature = 0;
		int cacheInserts = 0;
		size_t uniqueDispatchSignatures = 0;
		int totalDispatches = 0;
		int compileLogs = 0;
		int errorLogs = 0;
	};

	// Tracks real libplacebo shader compilations across three independent signals (strict RAII restore):
	//   1. pl_cache attached to pl_gpu via pl_gpu_set_cache (pl_cache_objects, pl_cache_signature, and set callback).
	//   2. pl_dispatch_callback tracking unique pl_dispatch_info.signature values and pass descriptions.
	//   3. pl_log_update at PL_LOG_DEBUG counting shader-compile log entries ("compiling shader", "translating SPIR-V",
	//      "compute shader source", "D3DCompile") and PL_LOG_ERR entries.
	struct ShaderCompileTracker
	{
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		pl_log log = nullptr;
		pl_cache cache = nullptr;
		pl_log_params prevLogParams{};
		bool logUpdated = false;

		int cacheInserts = 0;
		int totalDispatches = 0;
		int compileLogs = 0;
		int errorLogs = 0;
		std::set<uint64_t> uniqueSignatures;
		std::vector<std::string> newPassDescriptions;

		ShaderCompileTracker(pl_gpu g, pl_dispatch d, pl_log l) : gpu(g), dispatch(d), log(l)
		{
			pl_cache_params cp{};
			cp.max_object_size = 0;
			cp.max_total_size = 0;
			cp.priv = this;
			cp.set = &ShaderCompileTracker::OnCacheSet;
			cache = pl_cache_create(&cp);
			if (gpu && cache)
				pl_gpu_set_cache(gpu, cache);
			if (dispatch)
				pl_dispatch_callback(dispatch, this, &ShaderCompileTracker::OnDispatch);
			if (log)
			{
				pl_log_params lp{};
				lp.log_level = PL_LOG_DEBUG;
				lp.log_priv = this;
				lp.log_cb = &ShaderCompileTracker::OnLog;
				prevLogParams = pl_log_update(log, &lp);
				logUpdated = true;
			}
		}

		~ShaderCompileTracker()
		{
			if (dispatch)
				pl_dispatch_callback(dispatch, nullptr, nullptr);
			if (gpu)
				pl_gpu_set_cache(gpu, nullptr);
			if (cache)
				pl_cache_destroy(&cache);
			if (log && logUpdated)
				pl_log_update(log, &prevLogParams);
		}

		ShaderCompileTracker(const ShaderCompileTracker&) = delete;
		ShaderCompileTracker& operator=(const ShaderCompileTracker&) = delete;

		ShaderCompileSnapshot Snapshot() const
		{
			ShaderCompileSnapshot s;
			s.cacheObjects = cache ? pl_cache_objects(cache) : 0;
			s.cacheSignature = cache ? pl_cache_signature(cache) : 0;
			s.cacheInserts = cacheInserts;
			s.uniqueDispatchSignatures = uniqueSignatures.size();
			s.totalDispatches = totalDispatches;
			s.compileLogs = compileLogs;
			s.errorLogs = errorLogs;
			return s;
		}

		std::string FormatNewDescriptionsSince(size_t startIdx) const
		{
			std::string out;
			for (size_t i = startIdx; i < newPassDescriptions.size(); ++i)
			{
				if (!out.empty()) out += ", ";
				out += newPassDescriptions[i];
			}
			return out.empty() ? std::string("<none>") : out;
		}

		static void OnCacheSet(void* priv, pl_cache_obj obj)
		{
			if (!priv || obj.size == 0) return;
			++static_cast<ShaderCompileTracker*>(priv)->cacheInserts;
		}

		static void OnDispatch(void* priv, const pl_dispatch_info* info)
		{
			if (!priv || !info) return;
			ShaderCompileTracker* self = static_cast<ShaderCompileTracker*>(priv);
			++self->totalDispatches;
			if (self->uniqueSignatures.insert(info->signature).second)
			{
				const char* desc = (info->shader && info->shader->description) ? info->shader->description : "<unnamed>";
				self->newPassDescriptions.emplace_back(desc);
			}
		}

		static void OnLog(void* priv, enum pl_log_level level, const char* msg)
		{
			if (!priv || !msg) return;
			ShaderCompileTracker* self = static_cast<ShaderCompileTracker*>(priv);
			if (level <= PL_LOG_ERR)
				++self->errorLogs;
			if (std::strstr(msg, "compiling shader") != nullptr ||
				std::strstr(msg, "translating SPIR-V") != nullptr ||
				std::strstr(msg, "compute shader source") != nullptr ||
				std::strstr(msg, "D3DCompile") != nullptr)
			{
				++self->compileLogs;
			}
		}
	};

	// Builds a textured 4:2:0 scene with sharp correlated luma/chroma step edges in the top/bottom margins (y=7, h-7),
	// left/right margins (x=7, w-7), and picture interior (x=55, y=45) so any crop change, siting change, or eps change
	// produces a clear (> 5 code) measurable change in CfL output.
	Scene MakeDynamicUniformScene(int w, int h, int cw, int ch, double sx, double sy, uint32_t seed)
	{
		Plane Y(w, h), tcb(w, h), tcr(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				double L = 420.0
					+ 80.0 * std::sin(2.0 * kPi * x / 19.0 + 0.3) * std::cos(2.0 * kPi * y / 23.0 + 0.2)
					+ 130.0 * std::tanh((x - 55.0) / 0.7)
					- 130.0 * std::tanh((y - 45.0) / 0.7)
					+ 120.0 * std::tanh((y - 7.0) / 0.6)
					- 120.0 * std::tanh((y - static_cast<double>(h - 7)) / 0.6)
					+ 120.0 * std::tanh((x - 7.0) / 0.6)
					- 120.0 * std::tanh((x - static_cast<double>(w - 7)) / 0.6);
				L = (std::min)(900.0, (std::max)(100.0, L));
				Y.at(x, y) = L;
				tcb.at(x, y) = 512.0 + 0.40 * (L - 420.0) + 22.0 * std::sin(2.0 * kPi * x / 53.0);
				tcr.at(x, y) = 500.0 - 0.35 * (L - 420.0) + 18.0 * std::cos(2.0 * kPi * y / 41.0);
			}
		const double fx = static_cast<double>(w) / cw, fy = static_cast<double>(h) / ch;
		Scene s;
		s.y = Y;
		s.cb = ChromaUpscale::TentAtSites(tcb, 0, 0, cw, ch, fx, fy, sx, sy, 0, 0);
		s.cr = ChromaUpscale::TentAtSites(tcr, 0, 0, cw, ch, fx, fy, sx, sy, 0, 0);
		Noise n(seed);
		for (double& v : s.y.v) v += 1.5 * n.Next();
		for (double& v : s.cb.v) v += 1.5 * n.Next();
		for (double& v : s.cr.v) v += 1.5 * n.Next();
		Quantise(s.y); Quantise(s.cb); Quantise(s.cr);
		return s;
	}

	uint16_t RawScaled(double codes, double scale)
	{
		return static_cast<uint16_t>((std::min)(65535L, (std::max)(0L, std::lround(codes * scale))));
	}

	pl_tex UploadR16Scaled(pl_gpu gpu, const Plane& y, double scale)
	{
		std::vector<uint16_t> data(y.v.size());
		for (size_t i = 0; i < data.size(); ++i) data[i] = RawScaled(y.v[i], scale);
		return UploadRaw(gpu, y.w, y.h, "r16", data);
	}

	pl_tex UploadRg16Scaled(pl_gpu gpu, const Plane& cb, const Plane& cr, double scale)
	{
		std::vector<uint16_t> data(cb.v.size() * 2);
		for (size_t i = 0; i < cb.v.size(); ++i)
		{
			data[i * 2] = RawScaled(cb.v[i], scale);
			data[i * 2 + 1] = RawScaled(cr.v[i], scale);
		}
		return UploadRaw(gpu, cb.w, cb.h, "rg16", data);
	}

	bool DownloadChromaScaled(pl_gpu gpu, pl_tex tex, double invScale, Plane& cb, Plane& cr)
	{
		std::vector<uint16_t> raw;
		int nc = 0;
		if (!DownloadRaw(gpu, tex, raw, nc) || nc < 2)
			return false;
		cb = Plane(tex->params.w, tex->params.h);
		cr = Plane(tex->params.w, tex->params.h);
		for (size_t i = 0; i < cb.v.size(); ++i)
		{
			cb.v[i] = raw[i * nc] * invScale;
			cr.v[i] = raw[i * nc + 1] * invScale;
		}
		return true;
	}

	pl_frame Frame2CustomRepr(pl_tex luma, pl_tex chroma, double sx, double sy, enum pl_color_levels levels, int colorDepth, int bitShift)
	{
		pl_frame f = Frame2(luma, chroma, sx, sy);
		f.repr.levels = levels;
		f.repr.bits.sample_depth = 16;
		f.repr.bits.color_depth = colorDepth;
		f.repr.bits.bit_shift = bitShift;
		return f;
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

		TEST_METHOD(CflHookOffsetsFollowSubsampledFrameSiting)
		{
			// Q#7 (GEM-066): CflOffsetsFromFrame extracts exact chroma siting offsets for subsampled YCbCr
			// frames (4:2:0 left, 4:2:0 top-left 2-plane & 3-plane, 4:2:0 center, 4:2:2 left) and rejects
			// non-subsampled 4:4:4 and 1-plane RGB frames.
			DummyPlFrame f;

			// MPEG-2 4:2:0 left-sited (-0.5, 0.0).
			f.Init2Plane(1920, 1080, 960, 540, -0.5f, 0.0f);
			const ChromaUpscale::CflOffsets mpeg2Left = ChromaUpscale::CflOffsetsFromFrame(f.frame);
			Assert::IsTrue(mpeg2Left.valid, L"4:2:0 MPEG-2 left must be valid");
			Assert::AreEqual(-0.5, mpeg2Left.x, 1e-6, L"4:2:0 MPEG-2 left x offset");
			Assert::AreEqual(0.0, mpeg2Left.y, 1e-6, L"4:2:0 MPEG-2 left y offset");

			// BT.2020 / HDR10 4:2:0 top-left (-0.5, -0.5), 2-plane P010 layout.
			f.Init2Plane(3840, 2160, 1920, 1080, -0.5f, -0.5f);
			f.frame.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
			f.frame.color.primaries = PL_COLOR_PRIM_BT_2020;
			f.frame.color.transfer = PL_COLOR_TRC_PQ;
			const ChromaUpscale::CflOffsets hdr10TopLeft = ChromaUpscale::CflOffsetsFromFrame(f.frame);
			Assert::IsTrue(hdr10TopLeft.valid, L"4:2:0 BT.2020/HDR10 top-left (2-plane) must be valid");
			Assert::AreEqual(-0.5, hdr10TopLeft.x, 1e-6, L"4:2:0 top-left x offset");
			Assert::AreEqual(-0.5, hdr10TopLeft.y, 1e-6, L"4:2:0 top-left y offset");

			// BT.2020 / HDR10 4:2:0 top-left (-0.5, -0.5), 3-plane planar layout.
			f.Init3Plane(3840, 2160, 1920, 1080, -0.5f, -0.5f);
			f.frame.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
			const ChromaUpscale::CflOffsets hdr10Planar = ChromaUpscale::CflOffsetsFromFrame(f.frame);
			Assert::IsTrue(hdr10Planar.valid, L"4:2:0 BT.2020/HDR10 top-left (3-plane) must be valid");
			Assert::AreEqual(-0.5, hdr10Planar.x, 1e-6, L"3-plane 4:2:0 top-left x offset");
			Assert::AreEqual(-0.5, hdr10Planar.y, 1e-6, L"3-plane 4:2:0 top-left y offset");

			// MPEG-1 4:2:0 center-sited (0.0, 0.0).
			f.Init2Plane(1920, 1080, 960, 540, 0.0f, 0.0f);
			const ChromaUpscale::CflOffsets mpeg1Center = ChromaUpscale::CflOffsetsFromFrame(f.frame);
			Assert::IsTrue(mpeg1Center.valid, L"4:2:0 MPEG-1 center must be valid");
			Assert::AreEqual(0.0, mpeg1Center.x, 1e-6, L"4:2:0 MPEG-1 center x offset");
			Assert::AreEqual(0.0, mpeg1Center.y, 1e-6, L"4:2:0 MPEG-1 center y offset");

			// 4:2:2 left-sited (w/2, h, -0.5, 0.0).
			f.Init2Plane(1920, 1080, 960, 1080, -0.5f, 0.0f);
			const ChromaUpscale::CflOffsets left422 = ChromaUpscale::CflOffsetsFromFrame(f.frame);
			Assert::IsTrue(left422.valid, L"4:2:2 left must be valid");
			Assert::AreEqual(-0.5, left422.x, 1e-6, L"4:2:2 left x offset");
			Assert::AreEqual(0.0, left422.y, 1e-6, L"4:2:2 left y offset");

			// Negative controls: non-subsampled 4:4:4 (both zero and non-zero shifts), 1-plane RGB,
			// and empty frame must all return valid == false.
			f.Init2Plane(1920, 1080, 1920, 1080, 0.0f, 0.0f);
			Assert::IsFalse(ChromaUpscale::CflOffsetsFromFrame(f.frame).valid,
				L"negative control: 4:4:4 (0, 0) is not subsampled and must return valid == false");
			f.Init2Plane(1920, 1080, 1920, 1080, -0.5f, -0.5f);
			Assert::IsFalse(ChromaUpscale::CflOffsetsFromFrame(f.frame).valid,
				L"negative control: 4:4:4 (-0.5, -0.5) is not subsampled and must return valid == false");

			f.InitRgb1Plane(1920, 1080);
			Assert::IsFalse(ChromaUpscale::CflOffsetsFromFrame(f.frame).valid,
				L"negative control: 1-plane RGB frame must return valid == false");

			pl_frame emptyFrame{};
			Assert::IsFalse(ChromaUpscale::CflOffsetsFromFrame(emptyFrame).valid,
				L"negative control: empty frame must return valid == false");
		}

		TEST_METHOD(CflHookOffsetsSkipUserConfiguredParameters)
		{
			// Q#7 (GEM-066): explicit user-configured chroma_offset_x / chroma_offset_y on a CfL_Prediction.glsl
			// rule must never be overwritten by per-frame siting, while any unset axis still follows the frame.
			DummyPlFrame f;
			f.Init2Plane(3840, 2160, 1920, 1080, -0.5f, -0.5f);

			DummyCflHook cflHook(0.25f, -0.25f);
			std::vector<const pl_hook*> hooks = { &cflHook.hook };

			ConfiguredShaderRule userXRule;
			userXRule.name = "cfl";
			userXRule.label = "CfL_Prediction";
			userXRule.filename = "CfL_Prediction.glsl";
			userXRule.parameters["chroma_offset_x"] = "0.25";
			const std::vector<ConfiguredShaderRule> rulesWithX = { userXRule };

			Assert::IsTrue(ChromaUpscale::RuleSetsCflParam(rulesWithX, "chroma_offset_x"),
				L"RuleSetsCflParam must detect user-configured chroma_offset_x");
			Assert::IsFalse(ChromaUpscale::RuleSetsCflParam(rulesWithX, "chroma_offset_y"),
				L"RuleSetsCflParam must return false for unset chroma_offset_y");

			ChromaUpscale::ApplyCflHookOffsets(f.frame, hooks, rulesWithX, false);
			Assert::AreEqual(0.25f, cflHook.valX.f, 1e-6f,
				L"user-configured chroma_offset_x must stay untouched at 0.25");
			Assert::AreEqual(-0.5f, cflHook.valY.f, 1e-6f,
				L"unset chroma_offset_y must be updated to frame top-left siting (-0.5)");

			// Symmetric check: user sets only chroma_offset_y -> x updates, y stays untouched.
			cflHook.valX.f = 0.25f;
			cflHook.valY.f = 0.125f;
			ConfiguredShaderRule userYRule;
			userYRule.name = "cfl";
			userYRule.label = "CfL_Prediction";
			userYRule.filename = "CfL_Prediction.glsl";
			userYRule.parameters["chroma_offset_y"] = "0.125";
			const std::vector<ConfiguredShaderRule> rulesWithY = { userYRule };
			Assert::IsFalse(ChromaUpscale::RuleSetsCflParam(rulesWithY, "chroma_offset_x"),
				L"RuleSetsCflParam must return false for unset chroma_offset_x");
			Assert::IsTrue(ChromaUpscale::RuleSetsCflParam(rulesWithY, "chroma_offset_y"),
				L"RuleSetsCflParam must detect user-configured chroma_offset_y");
			ChromaUpscale::ApplyCflHookOffsets(f.frame, hooks, rulesWithY, false);
			Assert::AreEqual(-0.5f, cflHook.valX.f, 1e-6f,
				L"unset chroma_offset_x must be updated to frame siting (-0.5)");
			Assert::AreEqual(0.125f, cflHook.valY.f, 1e-6f,
				L"user-configured chroma_offset_y must stay untouched at 0.125");

			// In-test negative control: exact same frame and hook, empty rule.parameters ->
			// ApplyCflHookOffsets must overwrite BOTH chroma_offset_x and chroma_offset_y.
			cflHook.valX.f = 0.25f;
			cflHook.valY.f = -0.25f;
			ConfiguredShaderRule defaultRule;
			defaultRule.name = "cfl";
			defaultRule.label = "CfL_Prediction";
			defaultRule.filename = "CfL_Prediction.glsl";
			const std::vector<ConfiguredShaderRule> emptyParamRules = { defaultRule };
			Assert::IsFalse(ChromaUpscale::RuleSetsCflParam(emptyParamRules, "chroma_offset_x"),
				L"negative control: empty parameters must not claim chroma_offset_x");
			Assert::IsFalse(ChromaUpscale::RuleSetsCflParam(emptyParamRules, "chroma_offset_y"),
				L"negative control: empty parameters must not claim chroma_offset_y");

			ChromaUpscale::ApplyCflHookOffsets(f.frame, hooks, emptyParamRules, false);
			Assert::AreEqual(-0.5f, cflHook.valX.f, 1e-6f,
				L"negative control: without user override, chroma_offset_x must update to -0.5");
			Assert::AreEqual(-0.5f, cflHook.valY.f, 1e-6f,
				L"negative control: without user override, chroma_offset_y must update to -0.5");
		}

		TEST_METHOD(CflHookOffsetsNoOpOnOriginalPathway)
		{
			// Q#7 (GEM-066): on the Original pathway (originalPathway == true), ApplyCflHookOffsets must be
			// a strict no-op, leaving dynamic CfL hook parameters untouched.
			DummyPlFrame f;
			f.Init2Plane(3840, 2160, 1920, 1080, -0.5f, -0.5f);

			DummyCflHook cflHook(0.25f, 0.75f);
			std::vector<const pl_hook*> hooks = { &cflHook.hook };

			ConfiguredShaderRule defaultRule;
			defaultRule.name = "cfl";
			defaultRule.label = "CfL_Prediction";
			defaultRule.filename = "CfL_Prediction.glsl";
			const std::vector<ConfiguredShaderRule> rules = { defaultRule };

			ChromaUpscale::ApplyCflHookOffsets(f.frame, hooks, rules, true);
			Assert::AreEqual(0.25f, cflHook.valX.f, 1e-6f,
				L"originalPathway=true must leave chroma_offset_x untouched");
			Assert::AreEqual(0.75f, cflHook.valY.f, 1e-6f,
				L"originalPathway=true must leave chroma_offset_y untouched");

			// In-test negative control: exact same frame, hook, and rules with originalPathway=false
			// must update both parameters to the frame's top-left (-0.5, -0.5) siting.
			ChromaUpscale::ApplyCflHookOffsets(f.frame, hooks, rules, false);
			Assert::AreEqual(-0.5f, cflHook.valX.f, 1e-6f,
				L"negative control: originalPathway=false must update chroma_offset_x to -0.5");
			Assert::AreEqual(-0.5f, cflHook.valY.f, 1e-6f,
				L"negative control: originalPathway=false must update chroma_offset_y to -0.5");
		}
	};

	TEST_CLASS(ChromaCflGpuTests)
	{
	public:
		static void CompareWithModel(GpuDevice& dev, const Scene& sc, double sx, double sy, const ActiveRect& rect, const char* name)
		{
			const GpuOut g = RunGpu(dev, sc, sx, sy, rect);
			Assert::IsTrue(g.ok, Wide(g.error).c_str());
			Assert::IsTrue(g.status == "ok" || g.status == "ok; own grain split", Wide(g.status).c_str());   // no product passed here
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

		TEST_METHOD(ChromaCflGpuReadsTheSharedGrainSplit)
		{
			// One producer, many readers: with the shared grain.split products (luma, Cb, Cr) the module computes no split of
			// its own and its output is bit-identical to its own-split output.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const GpuOut own = RunGpu(dev, sc, -0.5, 0.0, ActiveRect()), shared = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false, true, true);
			Assert::IsTrue(own.ok && shared.ok, Wide(own.error + shared.error).c_str());
			Assert::AreEqual(std::string("ok; own grain split"), own.status);
			Assert::AreEqual(std::string("ok"), shared.status);
			Assert::IsTrue(own.cb.v == shared.cb.v && own.cr.v == shared.cr.v, L"bit-identical to the module's own split");
		}

		TEST_METHOD(ChromaCflGpuGrainProductAbsentFallsBackVisibly)
		{
			// No shared product (no view, or a view of a product with neither form): all three planes use the module's own splits;
			// status names it, LastGrainStatus is Missing and "grain split (chroma-from-luma)" is engaged.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, { CflShared::Absent, CflShared::EmptyProduct });
			for (const CflFrameOut& o : r)
			{
				Assert::IsTrue(o.grain == GrainSplit::GrainStatus::Missing, L"absent product: Missing");
				Assert::IsTrue(o.engaged && o.id == "grain split (chroma-from-luma)", L"the fallback is engaged and named");
				Assert::AreEqual(std::string("ok; own grain split"), o.status);
			}
			Assert::IsTrue(CflWorstDiff(r[0], r[1]) == 0.0, L"the same own-split output");
		}

		TEST_METHOD(ChromaCflGpuGrainProductMisfitFallsBackAsWrongSize)
		{
			// Another block size, code unit or component (or one misfit plane of three) does not fit: WrongSize, engaged, status
			// "does not fit"; the output equals the own-split output.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflShared> kinds = { CflShared::Absent, CflShared::WrongBlock, CflShared::WrongUnit, CflShared::WrongComponent, CflShared::CbMisfitOnly };
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, kinds);
			for (size_t i = 1; i < r.size(); ++i)
			{
				Assert::IsTrue(r[i].grain == GrainSplit::GrainStatus::WrongSize, L"misfit product: WrongSize");
				Assert::IsTrue(r[i].engaged, L"the fallback is engaged");
				Assert::AreEqual(std::string("ok; own grain split (the shared product does not fit)"), r[i].status);
				Assert::IsTrue(CflWorstDiff(r[0], r[i]) == 0.0, L"a misfit product is not read: the own-split output");
			}
		}

		TEST_METHOD(ChromaCflGpuFittingFullProductIsReadThroughTheAccessor)
		{
			// Fitting Full products on all three planes: Ring, released, status "ok", bit-identical to the own splits.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, { CflShared::Absent, CflShared::Full });
			Assert::IsTrue(r[1].grain == GrainSplit::GrainStatus::Ring && !r[1].engaged, L"fitting product: Ring, released");
			Assert::AreEqual(std::string("ok"), r[1].status);
			Assert::IsTrue(CflWorstDiff(r[0], r[1]) == 0.0, L"bit-identical to the own splits");
		}

		TEST_METHOD(ChromaCflGpuCompactProductsAreReadWithinOneCode)
		{
			// Compact products on all three planes (three distinct accessors in one shader) fit and are read: Ring, released,
			// output within one code of the Full-shared output.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, { CflShared::Full, CflShared::Compact });
			Assert::IsTrue(r[1].grain == GrainSplit::GrainStatus::Ring && !r[1].engaged, L"Compact products: Ring, released");
			Assert::AreEqual(std::string("ok"), r[1].status);
			const double worst = CflWorstDiff(r[0], r[1]);
			LogF("chroma upscale: Compact vs Full shared products, worst output difference %.5f codes", worst);
			// Measured on WARP (finisher, 2026-11-01): 0.0156 codes (one fp16 step of s1/s2 at these magnitudes). Bound = measured
			// plus margin (about 3x), so a real precision regression fails instead of hiding under a one-code bound.
			Assert::IsTrue(worst <= kCompactVsFullMaxCodes, L"Compact output within the measured bound of Full");
		}

		TEST_METHOD(ChromaCflGpuGrainFallbackRecoversAutomatically)
		{
			// absent -> fits -> absent -> misfit -> fits (Compact): engaged, released, engaged, engaged, released.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflShared> seq = { CflShared::Absent, CflShared::Full, CflShared::Absent, CflShared::WrongBlock, CflShared::Compact, CflShared::Full };
			const bool engaged[] = { true, false, true, true, false, false };
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, seq);
			for (size_t i = 0; i < seq.size(); ++i)
				Assert::IsTrue(r[i].engaged == engaged[i], L"engaged exactly while a shared product does not fit");
			Assert::IsTrue(r[0].grain == GrainSplit::GrainStatus::Missing && r[3].grain == GrainSplit::GrainStatus::WrongSize &&
				r[1].grain == GrainSplit::GrainStatus::Ring && r[4].grain == GrainSplit::GrainStatus::Ring, L"statuses follow the products");
			for (size_t i = 1; i < seq.size(); ++i)
				Assert::IsTrue(CflWorstDiff(r[0], r[i]) <= 1.0, L"the output does not change visibly across the switches");
			Assert::IsTrue(CflWorstDiff(r[0], r[4]) <= kCompactVsFullMaxCodes, L"Compact output within measured bound");
			Assert::IsTrue(CflWorstDiff(r[0], r[1]) == 0.0 && CflWorstDiff(r[0], r[2]) == 0.0 &&
				CflWorstDiff(r[0], r[3]) == 0.0 && CflWorstDiff(r[0], r[5]) == 0.0, L"Full and own are exact");
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
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"ChromaCflGpuTiming", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;
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
				const double medianMs = ms[ms.size() / 2];
				Assert::IsTrue(std::isfinite(medianMs) && medianMs > 0.0, L"median CfL GPU timing must be finite and positive");
				char text[200];
				std::snprintf(text, sizeof text, "chroma upscale CfL %dx%d 4:2:0: median %.3f ms over %zu frames (first %.3f ms); textures %.0f MB", w, h,
					medianMs, ms.size(), ms.front(), ChromaUpscale::Gpu::CflModule::TextureBytes(w, h, cw, ch) / 1048576.0);
				Logger::WriteMessage(text);
				module.Release(gpu);
				pl_tex_destroy(gpu, &luma);
				pl_tex_destroy(gpu, &chroma);
			}
		}

		TEST_METHOD(ChromaCflGpuStaleProductFallsBackVisibly)
		{
			// Products computed for another source frame, or by another version of the grain split unit, are STALE: the module
			// uses its own splits (own-split output), the entry is engaged and named, and a fitting product recovers it.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, { CflShared::Absent, CflShared::StaleSeq, CflShared::StaleVersion });
			for (size_t i = 1; i < r.size(); ++i)
			{
				Assert::IsTrue(r[i].grain == GrainSplit::GrainStatus::Stale, L"stale product: Stale");
				Assert::IsTrue(r[i].engaged && r[i].id == "grain split (chroma-from-luma)", L"engaged and named");
				Assert::IsTrue(CflWorstDiff(r[0], r[i]) == 0.0, L"a stale product is not read: the own-split output");
			}
			// Recovery: stale, fitting (a Full product made without a seq stamp is accepted when the reader does not ask), stale.
			const std::vector<CflFrameOut> rec = RunCflShared(dev, sc, { CflShared::StaleSeq, CflShared::Full, CflShared::StaleVersion, CflShared::Full });
			Assert::IsTrue(rec[0].engaged && !rec[1].engaged && rec[2].engaged && !rec[3].engaged, L"engaged exactly while stale: automatic recovery");
			Assert::IsTrue(rec[1].grain == GrainSplit::GrainStatus::Ring && rec[3].grain == GrainSplit::GrainStatus::Ring, L"Ring when it fits again");
		}

		TEST_METHOD(ChromaCflGpuShedProductFallsBackVisibly)
		{
			// The governor dropped the products (Params::grainShed): a fitting product is not read, status Shed, engaged, own-split
			// output; released when the products are back.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 31u, 1.5, 2.0);
			const std::vector<CflFrameOut> r = RunCflShared(dev, sc, { CflShared::Absent, CflShared::Shed, CflShared::Full });
			Assert::IsTrue(r[1].grain == GrainSplit::GrainStatus::Shed && r[1].engaged && r[1].id == "grain split (chroma-from-luma)", L"Shed, engaged, named");
			Assert::IsTrue(CflWorstDiff(r[0], r[1]) == 0.0, L"the shed product is not read");
			Assert::IsTrue(!r[2].engaged && r[2].grain == GrainSplit::GrainStatus::Ring, L"recovers when the products are back");
		}

		TEST_METHOD(ChromaCflGpuArenaIsByteIdentical)
		{
			// SA7: the chroma-grid temporaries and the own splits' intermediates from the scratch arena change nothing: the same scene
			// through a module without an arena and with one gives exactly equal planes (own splits, and the shared products).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 41u, 1.5, 2.0);
			for (int shared = 0; shared < 2; ++shared)
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				const auto requests = ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, 128, 96, 64, 48, 96.0, 48.0, shared == 0);
				Assert::IsFalse(requests.empty(), L"the float formats map to arena formats");
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T2)), L"arena reserve");
				const GpuOut priv = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false, true, shared != 0);
				const GpuOut ar = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false, true, shared != 0, &arena);
				Assert::IsTrue(priv.ok && ar.ok, Wide(priv.error + ar.error).c_str());
				Assert::IsTrue(priv.cb.v == ar.cb.v && priv.cr.v == ar.cr.v, L"arena output equals private output");
				Assert::AreEqual(priv.status, ar.status, L"the same status");
				Assert::AreEqual(uint64_t(0), arena.Exhausted(), L"the arena never ran short");
				Assert::IsFalse(arena.FrameDecision().engaged, L"scratch_arena released");
				Assert::IsTrue(arena.Acquired() >= 5, L"the arena was really used (five chroma-grid temporaries)");
				LogF("%.0f arena acquires, %.0f exhausted (shared products: %.0f)", static_cast<double>(arena.Acquired()), static_cast<double>(arena.Exhausted()), static_cast<double>(shared));
				arena.Release();
			}
		}

		TEST_METHOD(ChromaCflGpuArenaShortageIsVisibleAndRecovers)
		{
			// SA7: no scratch texture (an empty plan, or the tier shed) = the frame fails visibly (Apply false, ScratchShort, the
			// failure counted, scratch_arena engaged); the next frame under a proper plan equals a private module's output.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Scene sc = MakeScene(128, 96, 64, 48, -0.5, 0.0, 42u, 1.5, 2.0);
			pl_tex luma = UploadR16(gpu, sc.y), chroma = UploadRg16(gpu, sc.cb, sc.cr);
			const pl_frame src = Frame2(luma, chroma, -0.5, 0.0);
			const auto requests = ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, 128, 96, 64, 48, 96.0, 48.0, true);
			Assert::IsFalse(requests.empty(), L"the float formats map to arena formats");
			const GpuOut want = RunGpu(dev, sc, -0.5, 0.0, ActiveRect());
			Assert::IsTrue(want.ok, Wide(want.error).c_str());
			for (int tierCase = 0; tierCase < 2; ++tierCase)
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				// 0: an empty plan; 1: a plan whose tier (T0) is below the module's (T1): shed (the own splits' T0 requests are admitted).
				if (tierCase == 0)
					arena.Reserve(VpSkeleton::PlanArena(std::vector<VpSkeleton::ArenaRequest>(), VpSkeleton::Tier::T2));
				else
					arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T0));
				ChromaUpscale::Gpu::CflModule module;
				ChromaUpscale::Gpu::Params gp;
				gp.settings.mode = ChromaUpscale::Mode::Cfl;
				gp.arena = &arena;
				pl_frame out{};
				arena.BeginFrame();
				const unsigned long long before = module.Failures();
				Assert::IsFalse(module.Apply(gpu, dev.dispatch, src, gp, out), L"no scratch texture: Apply fails");
				Assert::IsTrue(module.ScratchShort(), L"ScratchShort");
				Assert::IsTrue(module.LastError().find("scratch memory short") != std::string::npos, Wide(module.LastError()).c_str());
				Assert::IsTrue(module.Failures() == before + 1, L"one failure counted");
				const FallbackCatalog::Decision d = arena.FrameDecision();
				Assert::IsTrue(d.engaged && d.id == "scratch_arena", L"scratch_arena engaged");
				Assert::IsTrue(tierCase == 0 ? arena.Exhausted() > 0 : arena.Shed() > 0,
					tierCase == 0 ? L"empty plan counted as Exhausted" : L"T0 plan counted as Shed");
				// Recovery: a proper plan, a new frame.
				arena.BeginFrame();
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T2)), L"arena reserve");
				Assert::IsFalse(arena.FrameDecision().engaged, L"released after BeginFrame");
				pl_frame out2{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out2), Wide(module.LastError()).c_str());
				Assert::IsFalse(module.ScratchShort(), L"recovered");
				Assert::IsFalse(arena.FrameDecision().engaged, L"no shortage after recovery");
				std::vector<uint16_t> raw;
				int nc = 0;
				Assert::IsTrue(DownloadRaw(gpu, out2.planes[1].texture, raw, nc), L"the output is readable");
				Plane cb(out2.planes[1].texture->params.w, out2.planes[1].texture->params.h), cr = cb;
				for (size_t i = 0; i < cb.v.size(); ++i) { cb.v[i] = raw[i * nc] / 64.0; cr.v[i] = raw[i * nc + 1] / 64.0; }
				Assert::IsTrue(cb.v == want.cb.v && cr.v == want.cr.v, L"the recovered frame equals a private-texture module's");
				Logger::WriteMessage(tierCase == 0 ? "empty plan: visible, recovered" : "tier shed: visible, recovered");
				module.Release(gpu);
				arena.Release();
			}
			pl_tex_destroy(gpu, &luma);
			pl_tex_destroy(gpu, &chroma);
		}

		TEST_METHOD(ChromaCflArenaGridRequestsMergeEqualShapes)
		{
			// SA7 + GEM-074: on a backend where the 2-component float format falls back to the 4-component one, the five temporaries have one
			// shape: one request of 5, not two requests the plan could count as 4 + 1 of different pools. Distinct formats: 4 and 1 (m.gc removed).
			const auto same = ChromaUpscale::Gpu::CflModule::GridRequests(VpSkeleton::TexFormat::RGBA32F, VpSkeleton::TexFormat::RGBA32F, 64, 48);
			Assert::IsTrue(same.size() == 1 && same[0].count == 5, L"equal formats: one request of five");
			const auto split = ChromaUpscale::Gpu::CflModule::GridRequests(VpSkeleton::TexFormat::RGBA32F, VpSkeleton::TexFormat::RG32F, 64, 48);
			Assert::IsTrue(split.size() == 2 && split[0].count == 4 && split[1].count == 1, L"distinct formats: 4 + 1");
			const VpSkeleton::ArenaPlan plan = VpSkeleton::PlanArena(same, VpSkeleton::Tier::T2);
			Assert::IsTrue(plan.pools.size() == 1 && plan.pools[0].capacity == 5, L"the plan reserves all five");
			Assert::IsTrue(plan.OwnerCount(ChromaUpscale::Gpu::kArenaScope, ChromaUpscale::Gpu::kArenaOwner, same[0].shape) == 5, L"the owner may hold five");
		}

		TEST_METHOD(ChromaCflGpuTextureBytesCountsTheArenaTemporaries)
		{
			// SA7 + GEM-074: with the temporaries in the arena the module's own bytes drop by 16 per luma pixel (the luma split's bands) and 104
			// per chroma pixel (lo..avg 72 after m.gc removal + two chroma splits' bands 32); the default form is the private-texture form, unchanged.
			const int W = 3840, H = 2160, cw = 1920, ch = 1080;
			const uint64_t priv = ChromaUpscale::Gpu::CflModule::TextureBytes(W, H, cw, ch), inArena = ChromaUpscale::Gpu::CflModule::TextureBytes(W, H, cw, ch, true);
			Assert::IsTrue(priv == ChromaUpscale::Gpu::CflModule::TextureBytes(W, H, cw, ch, false), L"the default is the private form");
			Assert::IsTrue(priv - inArena == uint64_t(16) * W * H + uint64_t(104) * cw * ch, L"the temporaries move to the arena");
		}

		TEST_METHOD(ChromaCflGpu444EarlyOutResetsGrainStatus)
		{
			// Q#6 (GEM-068): a 4:2:0 frame with grainGuard=true and no shared grain product latches
			// LastGrainStatus() == Missing (engaged). Immediately running a 4:4:4 frame on the same
			// CflModule instance takes the 4:4:4 early-out and must reset LastGrainStatus() to Ring
			// (clearing the stale fallback decision).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Scene sc420 = MakeScene(64, 48, 32, 24, -0.5, 0.0, 51u, 1.5, 1.5);
			const Scene sc444 = MakeScene(32, 24, 32, 24, 0.0, 0.0, 52u, 1.0, 1.0);
			pl_tex luma420 = UploadR16(gpu, sc420.y);
			pl_tex chroma420 = UploadRg16(gpu, sc420.cb, sc420.cr);
			pl_tex luma444 = UploadR16(gpu, sc444.y);
			pl_tex chroma444 = UploadRg16(gpu, sc444.cb, sc444.cr);
			ChromaUpscale::Gpu::CflModule module;
			struct ScopeCleanup
			{
				pl_gpu gpu;
				ChromaUpscale::Gpu::CflModule* mod;
				std::vector<pl_tex*> texes;
				ScopeCleanup(pl_gpu g, ChromaUpscale::Gpu::CflModule* m, std::vector<pl_tex*> ts)
					: gpu(g), mod(m), texes(ts) {}
				~ScopeCleanup()
				{
					if (mod) mod->Release(gpu);
					for (pl_tex* t : texes)
						if (t && *t) pl_tex_destroy(gpu, t);
				}
				ScopeCleanup(const ScopeCleanup&) = delete;
				ScopeCleanup& operator=(const ScopeCleanup&) = delete;
			} cleanup(gpu, &module, { &luma420, &chroma420, &luma444, &chroma444 });
			Assert::IsTrue(luma420 && chroma420 && luma444 && chroma444, L"test textures uploaded");

			const pl_frame src420 = Frame2(luma420, chroma420, -0.5, 0.0);
			const pl_frame src444 = Frame2(luma444, chroma444, 0.0, 0.0);

			ChromaUpscale::Gpu::Params gp;
			gp.settings.mode = ChromaUpscale::Mode::Cfl;
			gp.cfl.grainGuard = true;   // params.grain intentionally empty (no shared grain product)

			// Negative control: 4:2:0 frame without a shared grain product must genuinely latch a non-Ring status.
			pl_frame out420{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, src420, gp, out420), Wide(module.LastError()).c_str());
			Assert::IsTrue(module.LastGrainStatus() != GrainSplit::GrainStatus::Ring,
				L"negative control: 4:2:0 without shared grain product must latch a non-Ring grain status");
			Assert::IsTrue(module.LastGrainStatus() == GrainSplit::GrainStatus::Missing,
				L"negative control: 4:2:0 without shared grain product must latch GrainStatus::Missing");
			Assert::IsTrue(module.GrainFallbackDecision().engaged,
				L"negative control: grain fallback decision must be engaged after 4:2:0 own split");

			// Immediately run a 4:4:4 frame on the EXACT SAME module instance: stale status must be cleared to Ring.
			pl_frame out444{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, src444, gp, out444), Wide(module.LastError()).c_str());
			Assert::AreEqual(std::string("4:4:4: nothing to upsample"), module.Status());
			Assert::IsTrue(out444.planes[0].texture == luma444 && out444.planes[1].texture == chroma444,
				L"4:4:4 early-out passes source planes through untouched");
			Assert::IsTrue(module.LastGrainStatus() == GrainSplit::GrainStatus::Ring,
				L"4:4:4 early-out must reset stale LastGrainStatus() to Ring");
			Assert::IsFalse(module.GrainFallbackDecision().engaged,
				L"4:4:4 early-out must release the stale grain fallback decision");
		}

		TEST_METHOD(ChromaCflGpuPreservesExtraNonChromaAlphaPlane)
		{
			// Q#6 (GEM-068): a 4:2:0 frame carrying an extra non-Cb/Cr plane (Alpha, PL_CHANNEL_A) in either
			// 3-plane (Y, CbCr, A) or 4-plane (Y, Cb, Cr, A) layout must preserve the exact Alpha plane
			// (texture and component mapping) at output plane 2 while still running CfL chroma upsampling.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int W = 64, H = 48, cw = 32, ch = 24;
			const Scene sc = MakeScene(W, H, cw, ch, -0.5, 0.0, 53u, 1.5, 1.5);
			const ChromaUpscale::Result model = RunModel(sc, -0.5, 0.0, ActiveRect());
			Plane alphaPlane(W, H);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					alphaPlane.at(x, y) = 64.0 + static_cast<double>((x * 11 + y * 17) % 876);
			Quantise(alphaPlane);

			pl_tex luma = UploadR16(gpu, sc.y);
			pl_tex chroma = UploadRg16(gpu, sc.cb, sc.cr);
			pl_tex cbTex = UploadR16(gpu, sc.cb);
			pl_tex crTex = UploadR16(gpu, sc.cr);
			pl_tex alphaTex = UploadR16(gpu, alphaPlane);
			ChromaUpscale::Gpu::CflModule module;
			struct ScopeCleanup
			{
				pl_gpu gpu;
				ChromaUpscale::Gpu::CflModule* mod;
				std::vector<pl_tex*> texes;
				ScopeCleanup(pl_gpu g, ChromaUpscale::Gpu::CflModule* m, std::vector<pl_tex*> ts)
					: gpu(g), mod(m), texes(ts) {}
				~ScopeCleanup()
				{
					if (mod) mod->Release(gpu);
					for (pl_tex* t : texes)
						if (t && *t) pl_tex_destroy(gpu, t);
				}
				ScopeCleanup(const ScopeCleanup&) = delete;
				ScopeCleanup& operator=(const ScopeCleanup&) = delete;
			} cleanup(gpu, &module, { &luma, &chroma, &cbTex, &crTex, &alphaTex });
			Assert::IsTrue(luma && chroma && cbTex && crTex && alphaTex, L"test textures uploaded");

			auto makeAlphaPlPlane = [&]() {
				pl_plane a{};
				a.texture = alphaTex;
				a.components = 1;
				a.component_mapping[0] = PL_CHANNEL_A;
				a.component_mapping[1] = PL_CHANNEL_NONE;
				a.component_mapping[2] = PL_CHANNEL_NONE;
				a.component_mapping[3] = PL_CHANNEL_NONE;
				a.shift_x = 0.0f;
				a.shift_y = 0.0f;
				return a;
			};

			// Case 1: 3-plane 4:2:0 source (Y, subsampled CbCr, full-res Alpha).
			pl_frame src3 = Frame2(luma, chroma, -0.5, 0.0);
			src3.num_planes = 3;
			src3.planes[2] = makeAlphaPlPlane();
			src3.repr.alpha = PL_ALPHA_INDEPENDENT;

			// Case 2: 4-plane 4:2:0 source (Y, subsampled Cb, subsampled Cr, full-res Alpha).
			pl_frame src4 = Frame3(luma, cbTex, crTex, -0.5, 0.0);
			src4.num_planes = 4;
			src4.planes[3] = makeAlphaPlPlane();
			src4.repr.alpha = PL_ALPHA_INDEPENDENT;

			ChromaUpscale::Gpu::Params gp;
			gp.settings.mode = ChromaUpscale::Mode::Cfl;

			const pl_frame sources[2] = { src3, src4 };
			Plane outCb[2], outCr[2];
			for (int k = 0; k < 2; ++k)
			{
				const pl_frame& src = sources[k];
				const wchar_t* tag = (k == 0) ? L"3-plane (Y+CbCr+A)" : L"4-plane (Y+Cb+Cr+A)";
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());

				// Output must have 3 planes: Y, upsampled CbCr, preserved Alpha.
				Assert::AreEqual(3, out.num_planes, tag);
				Assert::IsTrue(out.planes[0].texture == luma && out.planes[0].components == 1 &&
					out.planes[0].component_mapping[0] == PL_CHANNEL_Y, L"luma plane preserved unchanged");

				// Negative control: verify chroma upsampling actually ran (not a pass-through of source planes).
				Assert::IsTrue(out.planes[1].texture != nullptr && out.planes[1].texture != src.planes[1].texture,
					L"negative control: chroma upsampling must produce a new upsampled texture");
				if (k == 1)
				{
					Assert::IsTrue(out.planes[1].texture != crTex,
						L"negative control: 4-plane upsampled chroma must not alias source Cr texture");
				}
				Assert::IsTrue(out.planes[1].texture->params.w == W && out.planes[1].texture->params.h == H,
					L"upsampled chroma texture is full luma resolution");
				Assert::IsTrue(out.planes[1].components == 2 &&
					out.planes[1].component_mapping[0] == PL_CHANNEL_CB &&
					out.planes[1].component_mapping[1] == PL_CHANNEL_CR,
					L"upsampled plane 1 maps CB and CR");
				Assert::IsTrue(out.planes[1].shift_x == 0.0f && out.planes[1].shift_y == 0.0f,
					L"upsampled chroma plane has zero shifts");

				// Extra Alpha plane must be preserved untouched at plane 2.
				Assert::IsTrue(out.planes[2].texture == alphaTex,
					L"Alpha plane texture must be preserved untouched");
				Assert::IsTrue(out.planes[2].components == 1 && out.planes[2].component_mapping[0] == PL_CHANNEL_A,
					L"Alpha plane component mapping (PL_CHANNEL_A) must be preserved");
				Assert::IsTrue(out.planes[2].shift_x == 0.0f && out.planes[2].shift_y == 0.0f,
					L"Alpha plane shifts must be preserved");
				Assert::IsTrue(out.repr.alpha == PL_ALPHA_INDEPENDENT,
					L"frame alpha representation must be preserved");
				Assert::IsTrue(out.planes[3].texture == nullptr,
					L"unused plane slot 3 must be cleared");

				// Verify upsampled chroma content matches the independent CPU reference model.
				std::vector<uint16_t> raw;
				int nc = 0;
				Assert::IsTrue(DownloadRaw(gpu, out.planes[1].texture, raw, nc), L"upsampled chroma readable");
				outCb[k] = Plane(W, H);
				outCr[k] = Plane(W, H);
				for (size_t i = 0; i < outCb[k].v.size(); ++i)
				{
					outCb[k].v[i] = raw[i * nc] / 64.0;
					outCr[k].v[i] = raw[i * nc + 1] / 64.0;
				}
				const double wcb = MaxDiff(outCb[k], model.cb);
				const double wcr = MaxDiff(outCr[k], model.cr);
				Assert::IsTrue(wcb <= 0.02 && wcr <= 0.02, L"upsampled chroma matches CPU model within 0.02 codes");
			}
			Assert::IsTrue(outCb[0].v == outCb[1].v && outCr[0].v == outCr[1].v,
				L"3-plane and 4-plane alpha frames produce bit-identical upsampled chroma");
		}

		TEST_METHOD(ChromaCflPredictionShaderDynamicOffsetsShiftChromaOnWarp)
		{
			// Q#7 (GEM-066): verify the real bundled shaders/CfL_Prediction.glsl preserves its MIT license header,
			// declares //!TYPE DYNAMIC float on chroma_offset_x (-0.5) and chroma_offset_y (0.0), parses via
			// pl_mpv_user_shader_parse with PL_HOOK_PAR_DYNAMIC float parameters, and dynamically shifts GPU chroma
			// alignment on a sharp horizontal+vertical Y+CbCr step edge via ApplyCflHookOffsets without re-parsing.
			const std::string shaderSource = LoadBundledCflShader();
			Assert::IsFalse(shaderSource.empty(), L"bundled shaders/CfL_Prediction.glsl must be readable");
			Assert::IsTrue(shaderSource.find("MIT License") != std::string::npos,
				L"MIT License header in shaders/CfL_Prediction.glsl must remain intact");
			Assert::IsTrue(HasDynamicFloatParam(shaderSource, "chroma_offset_x"),
				L"shaders/CfL_Prediction.glsl must declare //!TYPE DYNAMIC float on chroma_offset_x");
			Assert::IsTrue(HasDynamicFloatParam(shaderSource, "chroma_offset_y"),
				L"shaders/CfL_Prediction.glsl must declare //!TYPE DYNAMIC float on chroma_offset_y");

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();

			const pl_hook* cflHook = nullptr;
			pl_renderer renderer = nullptr;
			pl_tex lumaTex = nullptr;
			pl_tex chromaTex = nullptr;
			pl_tex targetTex = nullptr;

			struct WarpCleanup
			{
				pl_gpu gpu;
				pl_renderer* rr;
				const pl_hook** hook;
				pl_tex* luma;
				pl_tex* chroma;
				pl_tex* target;

				WarpCleanup(pl_gpu g, pl_renderer* r, const pl_hook** h, pl_tex* l, pl_tex* c, pl_tex* t)
					: gpu(g), rr(r), hook(h), luma(l), chroma(c), target(t) {}

				~WarpCleanup()
				{
					if (rr && *rr) pl_renderer_destroy(rr);
					if (hook && *hook) pl_mpv_user_shader_destroy(hook);
					if (target && *target) pl_tex_destroy(gpu, target);
					if (chroma && *chroma) pl_tex_destroy(gpu, chroma);
					if (luma && *luma) pl_tex_destroy(gpu, luma);
				}

				WarpCleanup(const WarpCleanup&) = delete;
				WarpCleanup& operator=(const WarpCleanup&) = delete;
			} cleanup(gpu, &renderer, &cflHook, &lumaTex, &chromaTex, &targetTex);

			cflHook = pl_mpv_user_shader_parse(gpu, shaderSource.data(), shaderSource.size());
			Assert::IsNotNull(cflHook, L"CfL_Prediction.glsl must parse cleanly via pl_mpv_user_shader_parse");

			const pl_hook_par* parX = FindHookParam(cflHook, "chroma_offset_x");
			const pl_hook_par* parY = FindHookParam(cflHook, "chroma_offset_y");
			Assert::IsNotNull(parX, L"parsed CfL hook must expose chroma_offset_x parameter");
			Assert::IsNotNull(parY, L"parsed CfL hook must expose chroma_offset_y parameter");
			Assert::IsTrue(parX->type == PL_VAR_FLOAT && parX->mode == PL_HOOK_PAR_DYNAMIC && parX->data != nullptr,
				L"chroma_offset_x must be PL_VAR_FLOAT with PL_HOOK_PAR_DYNAMIC and mutable data pointer");
			Assert::IsTrue(parY->type == PL_VAR_FLOAT && parY->mode == PL_HOOK_PAR_DYNAMIC && parY->data != nullptr,
				L"chroma_offset_y must be PL_VAR_FLOAT with PL_HOOK_PAR_DYNAMIC and mutable data pointer");
			Assert::AreEqual(-0.5f, parX->initial.f, 1e-6f, L"default chroma_offset_x must remain -0.5");
			Assert::AreEqual(0.0f, parY->initial.f, 1e-6f, L"default chroma_offset_y must remain 0.0");
			Assert::AreEqual(-0.5f, parX->data->f, 1e-6f, L"initial live chroma_offset_x value must be -0.5");
			Assert::AreEqual(0.0f, parY->data->f, 1e-6f, L"initial live chroma_offset_y value must be 0.0");

			// Build a 64x64 4:2:0 sharp horizontal+vertical Y+CbCr quadrant step edge whose BT.709 RGB
			// values stay strictly inside [0.11, 0.90] (zero clipping on both sides of the edge).
			const int W = 64, H = 64, cw = 32, ch = 32;
			Plane luma(W, H), cb(cw, ch), cr(cw, ch);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					luma.at(x, y) = (x >= W / 2 && y >= H / 2) ? 680.0 : 240.0;
			for (int cy = 0; cy < ch; ++cy)
				for (int cx = 0; cx < cw; ++cx)
				{
					const bool fg = (cx >= cw / 2 && cy >= ch / 2);
					cb.at(cx, cy) = fg ? 288.0 : 736.0;
					cr.at(cx, cy) = fg ? 624.0 : 464.0;
				}

			lumaTex = UploadR16(gpu, luma);
			chromaTex = UploadRg16(gpu, cb, cr);
			Assert::IsTrue(lumaTex != nullptr && chromaTex != nullptr, L"4:2:0 step edge textures uploaded");

			pl_fmt targetFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(targetFmt, L"RGBA32F renderable+host_readable target format must be available");
			pl_tex_params tp{};
			tp.w = W;
			tp.h = H;
			tp.format = targetFmt;
			tp.renderable = true;
			tp.host_readable = true;
			tp.blit_dst = (targetFmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			targetTex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(targetTex, L"RGBA32F render target created");

			renderer = pl_renderer_create(dev.log, gpu);
			Assert::IsNotNull(renderer, L"libplacebo renderer created");

			pl_frame target{};
			target.num_planes = 1;
			target.planes[0].texture = targetTex;
			target.planes[0].components = 3;
			target.planes[0].component_mapping[0] = PL_CHANNEL_R;
			target.planes[0].component_mapping[1] = PL_CHANNEL_G;
			target.planes[0].component_mapping[2] = PL_CHANNEL_B;
			target.repr.sys = PL_COLOR_SYSTEM_RGB;
			target.repr.levels = PL_COLOR_LEVELS_FULL;
			target.color = pl_color_space_bt709;

			std::vector<const pl_hook*> hooks = { cflHook };
			ConfiguredShaderRule cflRule;
			cflRule.name = "cfl";
			cflRule.label = "CfL_Prediction";
			cflRule.filename = "CfL_Prediction.glsl";
			const std::vector<ConfiguredShaderRule> rules = { cflRule };

			pl_render_params rparams = pl_render_default_params;
			rparams.dither_params = nullptr;
			rparams.error_diffusion = nullptr;
			rparams.hooks = hooks.data();
			rparams.num_hooks = static_cast<int>(hooks.size());

			auto renderFrame = [&](const pl_frame& src) -> std::vector<float>
			{
				const float black[3] = { 0.0f, 0.0f, 0.0f };
				pl_frame_clear(gpu, &target, black);
				Assert::IsTrue(pl_render_image(renderer, &src, &target, &rparams),
					L"pl_render_image with CfL_Prediction.glsl must succeed");
				const pl_render_errors errs = pl_renderer_get_errors(renderer);
				Assert::IsTrue(errs.errors == 0 && errs.num_disabled_hooks == 0,
					L"pl_render_image must execute CfL_Prediction.glsl without errors or disabled hooks");
				pl_gpu_finish(gpu);
				std::vector<float> rgba(static_cast<size_t>(W) * H * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = targetTex;
				dl.ptr = rgba.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"float32 render target download must succeed");
				for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
				{
					Assert::IsTrue(std::isfinite(rgba[i * 4 + 0]) &&
						std::isfinite(rgba[i * 4 + 1]) &&
						std::isfinite(rgba[i * 4 + 2]),
						L"rendered RGB pixels must be finite");
				}
				return rgba;
			};

			auto maxRgbDiffInBox = [&](const std::vector<float>& a, const std::vector<float>& b,
				int x0, int y0, int x1, int y1) -> double
			{
				double worst = 0.0;
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x)
					{
						const size_t base = (static_cast<size_t>(y) * W + x) * 4;
						for (int c = 0; c < 3; ++c)
							worst = (std::max)(worst, std::fabs(static_cast<double>(a[base + c]) - static_cast<double>(b[base + c])));
					}
				return worst;
			};

			// Render 1: top-left (-0.5, -0.5) via ApplyCflHookOffsets on the parsed hook.
			const pl_frame frameTopLeft = Frame2(lumaTex, chromaTex, -0.5, -0.5);
			ChromaUpscale::ApplyCflHookOffsets(frameTopLeft, hooks, rules, false);
			Assert::AreEqual(-0.5f, parX->data->f, 1e-6f, L"ApplyCflHookOffsets must set chroma_offset_x to -0.5 for top-left");
			Assert::AreEqual(-0.5f, parY->data->f, 1e-6f, L"ApplyCflHookOffsets must set chroma_offset_y to -0.5 for top-left");
			const std::vector<float> outTopLeft = renderFrame(frameTopLeft);

			// Render 2: center (0.0, 0.0) on the EXACT SAME parsed pl_hook (no re-parse).
			const pl_frame frameCenter = Frame2(lumaTex, chromaTex, 0.0, 0.0);
			ChromaUpscale::ApplyCflHookOffsets(frameCenter, hooks, rules, false);
			Assert::AreEqual(0.0f, parX->data->f, 1e-6f, L"ApplyCflHookOffsets must set chroma_offset_x to 0.0 for center");
			Assert::AreEqual(0.0f, parY->data->f, 1e-6f, L"ApplyCflHookOffsets must set chroma_offset_y to 0.0 for center");
			const std::vector<float> outCenter = renderFrame(frameCenter);

			// Render 3 (isolated dynamic-uniform proof): render the EXACT SAME frameTopLeft while the same
			// parsed pl_hook still holds (0.0, 0.0) from ApplyCflHookOffsets(frameCenter, ...). Any difference
			// vs outTopLeft is 100% caused by the dynamic GPU shader parameters (not pl_frame.shift_x/y).
			const std::vector<float> outTopLeftWithCenterParams = renderFrame(frameTopLeft);

			const double maxDiffEdge = maxRgbDiffInBox(outTopLeft, outCenter, 24, 24, 56, 56);
			const double maxDiffDynamicX = maxRgbDiffInBox(outTopLeft, outTopLeftWithCenterParams, 28, 44, 36, 52);
			const double maxDiffDynamicY = maxRgbDiffInBox(outTopLeft, outTopLeftWithCenterParams, 44, 28, 52, 36);
			const double maxDiffFlat = maxRgbDiffInBox(outTopLeft, outCenter, 6, 6, 20, 20);

			LogF("CfL_Prediction dynamic offsets WARP: top-left vs center edge %.4f, isolated dynX %.4f, isolated dynY %.4f, flat %.6f",
				maxDiffEdge, maxDiffDynamicX, maxDiffDynamicY, maxDiffFlat);

			// In-test negative control: top-left (-0.5, -0.5) and center (0.0, 0.0) outputs must differ
			// measurably at the edge, and both dynamic shader uniforms must independently shift chroma
			// on the exact same pl_frame without re-parsing the hook.
			Assert::IsTrue(maxDiffEdge > 0.01,
				L"negative control: top-left (-0.5, -0.5) and center (0.0, 0.0) edge outputs must differ by > 0.01");
			Assert::IsTrue(maxDiffDynamicX > 0.01,
				L"negative control: dynamic chroma_offset_x alone must shift vertical-edge chroma by > 0.01 without re-parsing");
			Assert::IsTrue(maxDiffDynamicY > 0.01,
				L"negative control: dynamic chroma_offset_y alone must shift horizontal-edge chroma by > 0.01 without re-parsing");
			Assert::IsTrue(maxDiffFlat < 1e-3,
				L"flat interior must remain unaffected (< 1e-3) across dynamic siting updates");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ChromaCflGpuTiledPassesMatchCpuModelInFloat32)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChromaCflGpuTiledPassesMatchCpuModelInFloat32)
		{
			// Q#8 (GEM-074): WARP parity of tiled STATS/AVG/UP compute passes + inline chroma grain (raw - cc) against
			// the independent double-precision CPU reference model (ChromaUpscale::Upsample) using unquantised r32f/rg32f
			// planes (eliminating 16-bit unorm 1/64-code quantisation). Exercises odd non-multiple-of-16 dimensions
			// (134x98 luma / 67x49 chroma), multiple 16x16 workgroup seams (x=15/16, 31/32, 47/48, 63/64...), all 4 borders,
			// both PL_CHROMA_LEFT (-0.5, 0.0) and PL_CHROMA_TOP_LEFT (-0.5, -0.5), and asymmetric non-zero active crop
			// (odd cx0=7, cy0=5, mid-workgroup picture edges) with high-frequency grain (raw != clean).
			//
			// IEEE-754 float32 parity bounds (Rule 1 derivation):
			//   At 10-bit chroma magnitude ~512 codes, 1 ULP(float32) = 512 * 2^-23 = 2^-14 = 6.1035e-5 codes.
			//   - guard=true  (7-pass GrainSplit + 4-pass CfL in float32 vs CPU double): 0.01 codes (= 9.78e-6 normalised,
			//     < 1e-5 normalised, 2x tighter than CompareWithModel's 0.02 codes on rg16).
			//   - guard=false (4-pass LO/STATS/AVG/UP directly in float32 vs CPU double): 0.002 codes (~32 ULPs at 512,
			//     = 1.96e-6 normalised).
			constexpr double kMaxCodesWithGrain = 0.01;
			constexpr double kMaxCodesNoGrain = 0.002;
			constexpr double kMaxFlatBarCodes = 1e-3;

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");

			auto maxDiffAtSeamAndBorder = [](const Plane& a, const Plane& b, double& seamMax, double& borderMax) {
				seamMax = 0.0;
				borderMax = 0.0;
				for (int y = 0; y < a.h; ++y)
					for (int x = 0; x < a.w; ++x)
					{
						const double d = std::fabs(a.at(x, y) - b.at(x, y));
						if (x % 16 == 0 || x % 16 == 15 || y % 16 == 0 || y % 16 == 15)
							seamMax = (std::max)(seamMax, d);
						if (x == 0 || x == a.w - 1 || y == 0 || y == a.h - 1)
							borderMax = (std::max)(borderMax, d);
					}
			};

			// Case 1: Odd non-multiple-of-16 full frame (134x98 / 67x49), PL_CHROMA_LEFT (-0.5, 0.0), guard=true.
			{
				const int W = 134, H = 98, cw = 67, ch = 49;
				const Scene sc = MakeTiledEdgeScene(W, H, cw, ch, -0.5, 0.0, 801u, 2.0, 3.0);

				// Premise check: verify high-frequency grain is genuinely separated (raw != clean) so inline raw - cc runs.
				const double linesC = static_cast<double>(H) / 2.0;
				const GrainSplit::Split cbSplit = GrainSplit::Compute(sc.cb, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
				const double grainPeak = MaxDiff(sc.cb, cbSplit.clean);
				Assert::IsTrue(grainPeak > 2.0, L"test premise: chroma grain (raw - clean) must exceed 2.0 codes");

				const GpuOut g = RunGpuFloat32(dev, sc, -0.5, 0.0, ActiveRect(), true);
				Assert::IsTrue(g.ok, Wide(g.error).c_str());
				Assert::IsTrue(g.texW == W && g.texH == H, L"float32 output matches odd luma dimensions 134x98");

				ChromaUpscale::CflParams cp;
				cp.grainGuard = true;
				const ChromaUpscale::Result ref = ChromaUpscale::Upsample(sc.y, sc.cb, sc.cr, -0.5, 0.0, ActiveRect(), 10, 0.0, cp);
				Assert::AreEqual(std::string("ok"), ref.status);

				const double maxCb = MaxDiff(g.cb, ref.cb), maxCr = MaxDiff(g.cr, ref.cr);
				double seamCb = 0.0, borderCb = 0.0, seamCr = 0.0, borderCr = 0.0;
				maxDiffAtSeamAndBorder(g.cb, ref.cb, seamCb, borderCb);
				maxDiffAtSeamAndBorder(g.cr, ref.cr, seamCr, borderCr);
				{
					char msg[256];
					std::snprintf(msg, sizeof msg,
						"Q#8 float32 134x98 left (guard=true): max (%.5f, %.5f) codes, seam (%.5f, %.5f), border (%.5f, %.5f)",
						maxCb, maxCr, seamCb, seamCr, borderCb, borderCr);
					Logger::WriteMessage(msg);
				}
				Assert::IsTrue(maxCb <= kMaxCodesWithGrain && maxCr <= kMaxCodesWithGrain,
					L"134x98 left-sited float32 GPU output must match CPU model within 0.01 codes (< 1e-5 normalised)");
				Assert::IsTrue(seamCb <= kMaxCodesWithGrain && seamCr <= kMaxCodesWithGrain,
					L"134x98 left-sited workgroup seams (x/y=15,16,31,32...) must match CPU model within 0.01 codes");
				Assert::IsTrue(borderCb <= kMaxCodesWithGrain && borderCr <= kMaxCodesWithGrain,
					L"134x98 left-sited 4 image borders must match CPU model within 0.01 codes");

				// In-test negative control A (exact same frame): dropping chroma grain (G = 0, simulating omitted inline raw - cc)
				// must violate the parity bound by > 100x (> 1.0 code).
				const ChromaUpscale::Result brokenNoGrain = UpsampleBrokenInlineGrain(sc.y, sc.cb, sc.cr, -0.5, 0.0, ActiveRect(), 10, false);
				const double noGrainGpuErr = (std::max)(MaxDiff(g.cb, brokenNoGrain.cb), MaxDiff(g.cr, brokenNoGrain.cr));
				const double noGrainRefErr = (std::max)(MaxDiff(ref.cb, brokenNoGrain.cb), MaxDiff(ref.cr, brokenNoGrain.cr));
				LogF("Q#8 negative control (grain dropped G=0): vs GPU %.4f codes, vs CPU ref %.4f codes", noGrainGpuErr, noGrainRefErr);
				Assert::IsTrue(noGrainGpuErr > 100.0 * kMaxCodesWithGrain && noGrainRefErr > 100.0 * kMaxCodesWithGrain,
					L"negative control: dropping inline grain (raw - cc) must cause > 1.0 code error (> 100x parity bound)");

				// In-test negative control B (exact same frame): omitting shared-memory halos across 16x16 workgroup tiles
				// in STATS/AVG/UP must violate the seam parity bound by > 100x (> 1.0 code).
				const ChromaUpscale::Result brokenHalo = UpsampleBrokenHaloOmitted(sc.y, sc.cb, sc.cr, -0.5, 0.0, ActiveRect(), 10);
				double haloSeamGpuCb = 0.0, haloBorderGpuCb = 0.0, haloSeamGpuCr = 0.0, haloBorderGpuCr = 0.0;
				double haloSeamRefCb = 0.0, haloBorderRefCb = 0.0, haloSeamRefCr = 0.0, haloBorderRefCr = 0.0;
				maxDiffAtSeamAndBorder(g.cb, brokenHalo.cb, haloSeamGpuCb, haloBorderGpuCb);
				maxDiffAtSeamAndBorder(g.cr, brokenHalo.cr, haloSeamGpuCr, haloBorderGpuCr);
				maxDiffAtSeamAndBorder(ref.cb, brokenHalo.cb, haloSeamRefCb, haloBorderRefCb);
				maxDiffAtSeamAndBorder(ref.cr, brokenHalo.cr, haloSeamRefCr, haloBorderRefCr);
				const double haloSeamGpu = (std::max)(haloSeamGpuCb, haloSeamGpuCr);
				const double haloSeamRef = (std::max)(haloSeamRefCb, haloSeamRefCr);
				LogF("Q#8 negative control (halo omitted at 16x16 seams): vs GPU %.4f codes, vs CPU ref %.4f codes", haloSeamGpu, haloSeamRef);
				Assert::IsTrue(haloSeamGpu > 100.0 * kMaxCodesWithGrain && haloSeamRef > 100.0 * kMaxCodesWithGrain,
					L"negative control: omitting 16x16 shared-memory halos must cause > 1.0 code seam error (> 100x parity bound)");
			}

			// Case 2: Odd non-multiple-of-16 full frame (134x98 / 67x49), PL_CHROMA_TOP_LEFT (-0.5, -0.5),
			// tested both with guard=true (full pipeline) and guard=false (pure LO/STATS/AVG/UP within 0.002 codes).
			{
				const int W = 134, H = 98, cw = 67, ch = 49;
				const Scene sc = MakeTiledEdgeScene(W, H, cw, ch, -0.5, -0.5, 802u, 2.0, 3.0);
				for (int guard = 0; guard < 2; ++guard)
				{
					const bool useGuard = guard != 0;
					const double bound = useGuard ? kMaxCodesWithGrain : kMaxCodesNoGrain;
					const GpuOut g = RunGpuFloat32(dev, sc, -0.5, -0.5, ActiveRect(), useGuard);
					Assert::IsTrue(g.ok, Wide(g.error).c_str());
					ChromaUpscale::CflParams cp;
					cp.grainGuard = useGuard;
					const ChromaUpscale::Result ref = ChromaUpscale::Upsample(sc.y, sc.cb, sc.cr, -0.5, -0.5, ActiveRect(), 10, 0.0, cp);
					const double maxCb = MaxDiff(g.cb, ref.cb), maxCr = MaxDiff(g.cr, ref.cr);
					double seamCb = 0.0, borderCb = 0.0, seamCr = 0.0, borderCr = 0.0;
					maxDiffAtSeamAndBorder(g.cb, ref.cb, seamCb, borderCb);
					maxDiffAtSeamAndBorder(g.cr, ref.cr, seamCr, borderCr);
					LogF("Q#8 float32 134x98 top-left (guard=%.0f): max (%.5f, %.5f) codes, seam (%.5f, %.5f)",
						static_cast<double>(guard), maxCb, maxCr, seamCb, seamCr);
					Assert::IsTrue(maxCb <= bound && maxCr <= bound,
						useGuard ? L"134x98 top-left (guard=true) must match CPU model within 0.01 codes"
						         : L"134x98 top-left (guard=false) pure LO/STATS/AVG/UP must match CPU model within 0.002 codes");
					Assert::IsTrue(seamCb <= bound && seamCr <= bound && borderCb <= bound && borderCr <= bound,
						L"134x98 top-left workgroup seams and borders must match CPU model within bound");
				}
			}

			// Case 3: Asymmetric non-zero active crop (padL=14, padT=10, padR=12, padB=14 -> 160x122 frame, 80x61 chroma,
			// activeRect={14,10,148,108}, odd CFL_CX0=7, CFL_CY0=5, mid-workgroup picture borders) under BOTH
			// PL_CHROMA_LEFT (-0.5, 0.0) and PL_CHROMA_TOP_LEFT (-0.5, -0.5). Catches missing +ivec2(CFL_CX0, CFL_CY0)
			// on inline raw - cc and bar/picture mixed-workgroup shared-memory barrier bugs.
			{
				const double sitings[2][2] = { { -0.5, 0.0 }, { -0.5, -0.5 } };
				const double barCode = static_cast<double>(static_cast<float>(820.0 / kFloat32CodePeak)) * kFloat32CodePeak;
				for (int si = 0; si < 2; ++si)
				{
					const double sx = sitings[si][0], sy = sitings[si][1];
					const Scene pic = MakeTiledEdgeScene(134, 98, 67, 49, sx, sy, 803u + static_cast<uint32_t>(si), 2.0, 3.0);
					Scene sc = Embed(pic, 14, 10, 12, 14, 2, 2, 64.0, 820.0);
					QuantiseFloat32(sc.y); QuantiseFloat32(sc.cb); QuantiseFloat32(sc.cr);
					ActiveRect rect;
					rect.left = 14; rect.top = 10; rect.right = 148; rect.bottom = 108;

					const GpuOut g = RunGpuFloat32(dev, sc, sx, sy, rect, true);
					Assert::IsTrue(g.ok, Wide(g.error).c_str());
					const ChromaUpscale::Result ref = ChromaUpscale::Upsample(sc.y, sc.cb, sc.cr, sx, sy, rect, 10, 0.0);
					Assert::AreEqual(std::string("ok"), ref.status);

					double maxPicCb = 0.0, maxPicCr = 0.0, maxCropEdge = 0.0, maxBarDev = 0.0;
					for (int y = 0; y < sc.y.h; ++y)
						for (int x = 0; x < sc.y.w; ++x)
						{
							const bool inside = x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
							if (inside)
							{
								const double dcb = std::fabs(g.cb.at(x, y) - ref.cb.at(x, y));
								const double dcr = std::fabs(g.cr.at(x, y) - ref.cr.at(x, y));
								maxPicCb = (std::max)(maxPicCb, dcb);
								maxPicCr = (std::max)(maxPicCr, dcr);
								if (x == rect.left || x == rect.right - 1 || y == rect.top || y == rect.bottom - 1)
									maxCropEdge = (std::max)(maxCropEdge, (std::max)(dcb, dcr));
							}
							else
							{
								maxBarDev = (std::max)(maxBarDev, (std::max)(std::fabs(g.cb.at(x, y) - barCode), std::fabs(g.cr.at(x, y) - barCode)));
							}
						}
					{
						char msg[256];
						std::snprintf(msg, sizeof msg,
							"Q#8 float32 cropped 160x122 (cx0=7,cy0=5, siting %.1f,%.1f): pic (%.5f, %.5f), cropEdge %.5f, barDev %.5f",
							sx, sy, maxPicCb, maxPicCr, maxCropEdge, maxBarDev);
						Logger::WriteMessage(msg);
					}
					Assert::IsTrue(maxPicCb <= kMaxCodesWithGrain && maxPicCr <= kMaxCodesWithGrain,
						L"non-zero odd crop picture pixels must match CPU model within 0.01 codes");
					Assert::IsTrue(maxCropEdge <= kMaxCodesWithGrain,
						L"non-zero odd crop border pixels (mid-workgroup) must match CPU model within 0.01 codes");
					Assert::IsTrue(maxBarDev <= kMaxFlatBarCodes,
						L"bar pixels outside active crop must stay flat at barCode within 1e-3 codes");

					if (si == 0)
					{
						// In-test negative control C (exact same cropped frame): forgetting +ivec2(CFL_CX0, CFL_CY0)
						// when computing inline raw - cc reads bar/shifted chroma and causes > 20 codes error.
						const ChromaUpscale::Result wrongOrigin = UpsampleBrokenInlineGrain(sc.y, sc.cb, sc.cr, sx, sy, rect, 10, true);
						const double wrongOriginErr = (std::max)(MaxDiff(g.cb, wrongOrigin.cb), MaxDiff(g.cr, wrongOrigin.cr));
						LogF("Q#8 negative control (inline raw - cc missing cx0/cy0 offset): error %.2f codes", wrongOriginErr);
						Assert::IsTrue(wrongOriginErr > 20.0,
							L"negative control: omitting crop offset (cx0, cy0) on inline raw - cc must produce > 20 codes error");
					}
				}
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ChromaCflGpuArenaDropsGcPlaneAndReducesVram)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChromaCflGpuArenaDropsGcPlaneAndReducesVram)
		{
			// Q#8 (GEM-074 AMEND step 1): removing m.gc (computing raw - cc inline in UP_BODY) drops s2 scratch-arena
			// allocation by 1 full-plane texture (2 -> 1 s2 plane; 6 -> 5 total chroma-grid temporaries) and reduces
			// TextureBytes() by exactly 8 bytes per chroma pixel (one rg32f chroma-grid plane).
			// Pre-fix code (with m.gc still allocated) MUST FAIL this test; post-fix code MUST PASS.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();

			// 1. Contract check on GridRequests & ArenaRequests: s2 count must be 1 (not 2), merged count must be 5 (not 6).
			const auto splitReqs = ChromaUpscale::Gpu::CflModule::GridRequests(VpSkeleton::TexFormat::RGBA32F, VpSkeleton::TexFormat::RG32F, 64, 48);
			Assert::IsTrue(splitReqs.size() == 2 && splitReqs[0].count == 4 && splitReqs[1].count == 1,
				L"distinct float formats: 4 s4 (lo, ab, lohi, avg) + 1 s2 (rho only; m.gc removed)");
			const auto mergedReqs = ChromaUpscale::Gpu::CflModule::GridRequests(VpSkeleton::TexFormat::RGBA32F, VpSkeleton::TexFormat::RGBA32F, 64, 48);
			Assert::IsTrue(mergedReqs.size() == 1 && mergedReqs[0].count == 5,
				L"equal float formats: merged single request of 5 temporaries (4 s4 + 1 s2)");

			const auto floatCaps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			pl_fmt f4 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, floatCaps);
			pl_fmt f2 = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32, floatCaps);
			if (!f2) f2 = f4;
			VpSkeleton::TexFormat t4{}, t2{};
			Assert::IsTrue(VpSkeleton::PlTexBackend::FormatOf(f4, &t4) && VpSkeleton::PlTexBackend::FormatOf(f2, &t2),
				L"float32 formats must map to arena formats");

			const int W = 128, H = 96, cw = 64, ch = 48;
			const auto gridOnly = ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, W, H, cw, ch, static_cast<double>(H), static_cast<double>(ch), false);
			uint32_t totalGridSlots = 0;
			uint32_t s2GridSlots = 0;
			for (const VpSkeleton::ArenaRequest& q : gridOnly)
			{
				totalGridSlots += q.count;
				if (q.shape.fmt == t2) s2GridSlots += q.count;
			}
			Assert::AreEqual(uint32_t(5), totalGridSlots, L"ArenaRequests(ownSplit=false) must request exactly 5 chroma-grid textures");
			Assert::AreEqual(t4 == t2 ? uint32_t(5) : uint32_t(1), s2GridSlots,
				L"ArenaRequests(ownSplit=false) s2 count must drop to 1 (or 5 when merged with t4)");

			// 2. Real GPU runtime enforcement: independently build a tight 1-s2-plane arena (4 s4 + 1 s2 = 5 textures, ZERO gc).
			// On pre-fix code (demanding gc + rho = 2 s2 textures), Apply() MUST FAIL with ScratchShort; post-fix MUST SUCCEED.
			const Scene sc = MakeScene(W, H, cw, ch, -0.5, 0.0, 811u, 1.5, 2.0);
			const ChromaUpscale::Result ref = RunModel(sc, -0.5, 0.0, ActiveRect());

			auto makeCustomGridReqs = [&](uint8_t s4Count, uint8_t s2Count) {
				std::vector<VpSkeleton::ArenaRequest> reqs;
				if (t4 == t2)
				{
					VpSkeleton::ArenaRequest q;
					q.scope = ChromaUpscale::Gpu::kArenaScope;
					q.owner = ChromaUpscale::Gpu::kArenaOwner;
					q.tier = VpSkeleton::Tier::T1;
					q.shape = { static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), t4 };
					q.count = static_cast<uint8_t>(s4Count + s2Count);
					if (q.count > 0) reqs.push_back(q);
				}
				else
				{
					if (s4Count > 0)
					{
						VpSkeleton::ArenaRequest q4;
						q4.scope = ChromaUpscale::Gpu::kArenaScope;
						q4.owner = ChromaUpscale::Gpu::kArenaOwner;
						q4.tier = VpSkeleton::Tier::T1;
						q4.shape = { static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), t4 };
						q4.count = s4Count;
						reqs.push_back(q4);
					}
					if (s2Count > 0)
					{
						VpSkeleton::ArenaRequest q2;
						q2.scope = ChromaUpscale::Gpu::kArenaScope;
						q2.owner = ChromaUpscale::Gpu::kArenaOwner;
						q2.tier = VpSkeleton::Tier::T1;
						q2.shape = { static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), t2 };
						q2.count = s2Count;
						reqs.push_back(q2);
					}
				}
				return reqs;
			};

			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				const auto tightPlan = VpSkeleton::PlanArena(makeCustomGridReqs(4, 1), VpSkeleton::Tier::T2);
				Assert::IsTrue(arena.Reserve(tightPlan), L"reserve tight 1-s2-plane arena (5 textures total)");
				const GpuOut out = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false, true, true, &arena);
				Assert::IsTrue(out.ok, Wide("1-s2-plane arena Apply failed: " + out.error).c_str());
				Assert::AreEqual(uint64_t(0), arena.Exhausted(), L"1-s2-plane arena must never exhaust");
				Assert::IsFalse(arena.FrameDecision().engaged, L"scratch_arena must remain released");
				Assert::AreEqual(uint64_t(5), arena.Acquired(),
					L"shared-grain Apply must acquire exactly 5 arena textures (4 s4 + 1 s2; m.gc removed)");
				Assert::IsTrue(MaxDiff(out.cb, ref.cb) <= 0.02 && MaxDiff(out.cr, ref.cr) <= 0.02,
					L"1-s2-plane arena output with grainGuard=true must still match CPU model within 0.02 codes");
				arena.Release();
			}

			// Own-split active path (shared=false, guard=true) with ArenaRequests(..., true).
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				const auto ownReqs = ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, W, H, cw, ch, static_cast<double>(H), static_cast<double>(ch), true);
				const VpSkeleton::ArenaPlan ownPlan = VpSkeleton::PlanArena(ownReqs, VpSkeleton::Tier::T2);
				const VpSkeleton::TexShape s2Shape{ static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), t2 };
				Assert::AreEqual(t4 == t2 ? uint32_t(5) : uint32_t(1),
					ownPlan.OwnerCount(ChromaUpscale::Gpu::kArenaScope, ChromaUpscale::Gpu::kArenaOwner, s2Shape),
					L"own-split arena plan must reserve only 1 s2 plane for chroma_cfl");
				Assert::IsTrue(arena.Reserve(ownPlan), L"reserve own-split arena");
				const GpuOut out = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false, true, false, &arena);
				Assert::IsTrue(out.ok, Wide("own-split arena Apply failed: " + out.error).c_str());
				Assert::AreEqual(uint64_t(0), arena.Exhausted(), L"own-split arena must never exhaust");
				Assert::IsTrue(MaxDiff(out.cb, ref.cb) <= 0.02 && MaxDiff(out.cr, ref.cr) <= 0.02,
					L"own-split arena output must match CPU model within 0.02 codes");
				arena.Release();
			}

			// In-test negative control 1 (exact same frame): removing the remaining s2 plane (0 s2 planes: 4 textures total)
			// MUST fail Apply visibly, proving the tight arena genuinely enforces exact s2 counts.
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> zeroS2Arena(backend);
				Assert::IsTrue(zeroS2Arena.Reserve(VpSkeleton::PlanArena(makeCustomGridReqs(4, 0), VpSkeleton::Tier::T2)),
					L"reserve 0-s2-plane negative-control arena");
				const GpuOut shortOut = RunGpu(dev, sc, -0.5, 0.0, ActiveRect(), false, true, true, &zeroS2Arena);
				Assert::IsFalse(shortOut.ok, L"negative control: 0-s2-plane arena must fail Apply (rho cannot be acquired)");
				Assert::IsTrue(zeroS2Arena.FrameDecision().engaged,
					L"negative control: 0-s2-plane shortage must engage scratch_arena decision");
				Assert::IsTrue(zeroS2Arena.Shed() > 0 || zeroS2Arena.Exhausted() > 0,
					L"negative control: 0-s2-plane shortage must be counted in Shed or Exhausted");
				zeroS2Arena.Release();
			}

			// 3. VRAM TextureBytes() check + negative control 2: strictly below the 2-s2-plane budget by exact 8 * cw * ch bytes.
			const int vramSizes[2][4] = { { 3840, 2160, 1920, 1080 }, { 128, 96, 64, 48 } };
			for (const auto& sz : vramSizes)
			{
				const int vw = sz[0], vh = sz[1], vcw = sz[2], vch = sz[3];
				const uint64_t px = static_cast<uint64_t>(vw) * static_cast<uint64_t>(vh);
				const uint64_t cpx = static_cast<uint64_t>(vcw) * static_cast<uint64_t>(vch);
				const uint64_t nbL = static_cast<uint64_t>((vw + 15) / 16) * static_cast<uint64_t>((vh + 15) / 16);
				const uint64_t nbC = static_cast<uint64_t>((vcw + 15) / 16) * static_cast<uint64_t>((vch + 15) / 16);
				// Pre-fix 2-s2-plane private budget (lo, gc, ab, rho, lohi, avg = 80 B/chroma px):
				const uint64_t twoS2PrivateBudget =
					px * (2 + 32 + 4) + nbL * 32 + cpx * (4 + 2 * 32) + nbC * 2 * 32 + cpx * (16 + 8 + 16 + 8 + 16 + 16);
				const uint64_t actualPriv = ChromaUpscale::Gpu::CflModule::TextureBytes(vw, vh, vcw, vch, false);
				const uint64_t actualInArena = ChromaUpscale::Gpu::CflModule::TextureBytes(vw, vh, vcw, vch, true);

				{
					char msg[256];
					std::snprintf(msg, sizeof msg,
						"Q#8 VRAM %dx%d: actualPriv %llu B, twoS2Budget %llu B, saved %llu B (expected %llu B)",
						vw, vh,
						static_cast<unsigned long long>(actualPriv),
						static_cast<unsigned long long>(twoS2PrivateBudget),
						static_cast<unsigned long long>(twoS2PrivateBudget - actualPriv),
						static_cast<unsigned long long>(uint64_t(8) * cpx));
					Logger::WriteMessage(msg);
				}
				Assert::IsTrue(actualPriv < twoS2PrivateBudget,
					L"post-fix TextureBytes() must be strictly lower than the 2-s2-plane budget");
				Assert::AreEqual(uint64_t(8) * cpx, twoS2PrivateBudget - actualPriv,
					L"m.gc removal must save exactly 8 bytes per chroma pixel (1 full-plane rg32f texture)");
				Assert::AreEqual(uint64_t(16) * px + uint64_t(104) * cpx, actualPriv - actualInArena,
					L"arena temporaries delta must be 16 B/luma px + 104 B/chroma px (72 B grid + 32 B chroma split bands)");

				// In-test negative control 2: a 2-s2-plane count strictly exceeds TextureBytes() by 8 * cw * ch bytes.
				const uint64_t twoS2Delta = twoS2PrivateBudget - actualInArena;
				Assert::IsTrue(twoS2PrivateBudget > actualPriv && twoS2Delta > (actualPriv - actualInArena),
					L"negative control: 2-plane s2 budget must strictly exceed post-fix TextureBytes()");
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ChromaCflGpuActiveCropChangeCompilesNothing)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChromaCflGpuActiveCropChangeCompilesNothing)
		{
			// Q#9 (GEM-076): dynamic per-frame geometry, active crop rect, chroma siting, eps, and code-unit scales
			// must be runtime uniforms rather than GLSL #defines, so changing crop rect / siting / eps / range scale /
			// frame dimensions (same 4:2:0 ratio) triggers ZERO new libplacebo shader compilations while still
			// producing exact CPU-model parity (<= 0.02 codes) and visibly applying the new dynamic parameters.
			// Pre-fix code (baking CFL_W/H/LEFT/TOP/RIGHT/BOTTOM/CX0/CY0/SX/SY/EPS/CU as #defines) triggers >= 4 new
			// shader compilations per change and MUST FAIL this test; post-fix code MUST PASS.
			constexpr double kParityMaxCodes = 0.02;
			constexpr double kDynamicChangeMinCodes = 1.0;

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();

			ShaderCompileTracker tracker(gpu, dev.dispatch, dev.log);
			Assert::IsNotNull(tracker.cache, L"pl_cache must be created");

			ChromaUpscale::Gpu::CflModule module;
			pl_tex luma160 = nullptr, chroma160 = nullptr;
			pl_tex luma160Full8 = nullptr, chroma160Full8 = nullptr;
			pl_tex luma128 = nullptr, chroma128 = nullptr;
			struct DynamicCleanup
			{
				pl_gpu gpu;
				ChromaUpscale::Gpu::CflModule* mod;
				pl_tex* t0; pl_tex* t1; pl_tex* t2; pl_tex* t3; pl_tex* t4; pl_tex* t5;
				DynamicCleanup(pl_gpu g, ChromaUpscale::Gpu::CflModule* m,
					pl_tex* a, pl_tex* b, pl_tex* c, pl_tex* d, pl_tex* e, pl_tex* f)
					: gpu(g), mod(m), t0(a), t1(b), t2(c), t3(d), t4(e), t5(f) {}
				~DynamicCleanup()
				{
					if (mod) mod->Release(gpu);
					for (pl_tex* t : { t5, t4, t3, t2, t1, t0 })
						if (t && *t) pl_tex_destroy(gpu, t);
				}
				DynamicCleanup(const DynamicCleanup&) = delete;
				DynamicCleanup& operator=(const DynamicCleanup&) = delete;
			} cleanup(gpu, &module, &luma160, &chroma160, &luma160Full8, &chroma160Full8, &luma128, &chroma128);

			// 160x120 4:2:0 scene (10-bit limited, bit_shift=6, 1/64-code resolution).
			const Scene sc160 = MakeDynamicUniformScene(160, 120, 80, 60, -0.5, 0.0, 901u);
			luma160 = UploadR16(gpu, sc160.y);
			chroma160 = UploadRg16(gpu, sc160.cb, sc160.cr);

			// 160x120 4:2:0 full-range 8-bit scene in same r16/rg16 format (bit_shift=8, 4x larger code unit, 1/256-code LSB).
			Scene sc160Full8;
			sc160Full8.y = Plane(160, 120);
			sc160Full8.cb = Plane(80, 60);
			sc160Full8.cr = Plane(80, 60);
			auto to8BitQuant = [](const Plane& in, Plane& out) {
				for (size_t i = 0; i < in.v.size(); ++i)
				{
					const double v = (std::min)(255.0, (std::max)(0.0, in.v[i] * 0.25));
					out.v[i] = std::round(v * 256.0) / 256.0;
				}
			};
			to8BitQuant(sc160.y, sc160Full8.y);
			to8BitQuant(sc160.cb, sc160Full8.cb);
			to8BitQuant(sc160.cr, sc160Full8.cr);
			luma160Full8 = UploadR16Scaled(gpu, sc160Full8.y, 256.0);
			chroma160Full8 = UploadRg16Scaled(gpu, sc160Full8.cb, sc160Full8.cr, 256.0);

			// 128x96 4:2:0 scene (different frame dimensions, same 2x2 subsampling ratio).
			const Scene sc128 = MakeDynamicUniformScene(128, 96, 64, 48, -0.5, 0.0, 902u);
			luma128 = UploadR16(gpu, sc128.y);
			chroma128 = UploadRg16(gpu, sc128.cb, sc128.cr);

			Assert::IsTrue(luma160 && chroma160 && luma160Full8 && chroma160Full8 && luma128 && chroma128,
				L"all dynamic-uniform test source textures must upload cleanly");

			struct FrameRun
			{
				Plane cb, cr;
				ShaderCompileSnapshot snap;
				std::string newPasses;
			};

			auto runFrame = [&](const char* tag, const pl_frame& src, const Scene& sc, const ActiveRect& rect,
				double sx, double sy, int colorDepth, double epsFrac, int radius, double invReadScale) -> FrameRun
			{
				ChromaUpscale::Gpu::Params gp;
				gp.settings.mode = ChromaUpscale::Mode::Cfl;
				gp.colorDepth = colorDepth;
				gp.active = rect;
				gp.cfl.grainGuard = false;   // strictly isolate CflModule passes (no GrainSplit)
				gp.cfl.epsFrac = epsFrac;
				gp.cfl.radius = radius;

				const size_t descStart = tracker.newPassDescriptions.size();
				const ShaderCompileSnapshot before = tracker.Snapshot();
				pl_dispatch_reset_frame(dev.dispatch);
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out),
					Wide(std::string(tag) + " Apply failed: " + module.LastError()).c_str());
				pl_gpu_finish(gpu);
				const ShaderCompileSnapshot after = tracker.Snapshot();
				const std::string addedPasses = tracker.FormatNewDescriptionsSince(descStart);

				Assert::AreEqual(before.errorLogs, after.errorLogs,
					Wide(std::string(tag) + ": libplacebo logged an error during Apply").c_str());
				Assert::IsTrue(after.totalDispatches > before.totalDispatches,
					Wide(std::string(tag) + ": CflModule::Apply must dispatch compute passes").c_str());
				Assert::IsTrue(out.planes[1].texture != nullptr &&
					out.planes[1].texture->params.w == sc.y.w && out.planes[1].texture->params.h == sc.y.h,
					Wide(std::string(tag) + ": output must match full luma size").c_str());

				FrameRun fr;
				fr.snap = after;
				fr.newPasses = addedPasses;
				Assert::IsTrue(DownloadChromaScaled(gpu, out.planes[1].texture, invReadScale, fr.cb, fr.cr),
					Wide(std::string(tag) + ": output chroma download failed").c_str());

				ChromaUpscale::CflParams cp;
				cp.grainGuard = false;
				cp.epsFrac = epsFrac;
				cp.radius = radius;
				const ChromaUpscale::Result ref = ChromaUpscale::Upsample(sc.y, sc.cb, sc.cr, sx, sy, rect, colorDepth, 0.0, cp);
				Assert::AreEqual(std::string("ok"), ref.status, Wide(std::string(tag) + ": CPU reference failed").c_str());

				const double wcb = MaxDiff(fr.cb, ref.cb);
				const double wcr = MaxDiff(fr.cr, ref.cr);
				char msg[320];
				std::snprintf(msg, sizeof msg,
					"Q#9 %s: parity (%.5f, %.5f) codes | sigs=%zu (+%zu) cache=%d (+%d) inserts=%d (+%d) compileLogs=%d (+%d) new=[%s]",
					tag, wcb, wcr,
					after.uniqueDispatchSignatures, after.uniqueDispatchSignatures - before.uniqueDispatchSignatures,
					after.cacheObjects, after.cacheObjects - before.cacheObjects,
					after.cacheInserts, after.cacheInserts - before.cacheInserts,
					after.compileLogs, after.compileLogs - before.compileLogs,
					addedPasses.c_str());
				Logger::WriteMessage(msg);

				Assert::IsTrue(wcb <= kParityMaxCodes && wcr <= kParityMaxCodes,
					Wide(std::string(tag) + ": GPU output must match independent CPU model within 0.02 codes").c_str());
				return fr;
			};

			auto assertZeroCompiles = [&](const char* tag, const ShaderCompileSnapshot& warm, const FrameRun& fr) {
				const std::wstring ctx = Wide(std::string(tag) + " compiled unexpected shader(s): " + fr.newPasses);
				Assert::AreEqual(warm.uniqueDispatchSignatures, fr.snap.uniqueDispatchSignatures,
					(ctx + L" [unique dispatch signatures]").c_str());
				Assert::AreEqual(warm.cacheObjects, fr.snap.cacheObjects,
					(ctx + L" [pl_cache_objects]").c_str());
				Assert::AreEqual(warm.cacheSignature, fr.snap.cacheSignature,
					(ctx + L" [pl_cache_signature]").c_str());
				Assert::AreEqual(warm.cacheInserts, fr.snap.cacheInserts,
					(ctx + L" [pl_cache set inserts]").c_str());
				Assert::AreEqual(warm.compileLogs, fr.snap.compileLogs,
					(ctx + L" [libplacebo shader-compile logs]").c_str());
			};

			// Frame 0 (Warm): 160x120 4:2:0, PL_CHROMA_LEFT (-0.5, 0.0), full active rect {0, 0, 160, 120}.
			ActiveRect full160;
			full160.left = 0; full160.top = 0; full160.right = 160; full160.bottom = 120;
			const pl_frame src160Left = Frame2(luma160, chroma160, -0.5, 0.0);
			const FrameRun f0 = runFrame("Frame0_Warm160Full", src160Left, sc160, full160, -0.5, 0.0, 10, 0.002, 2, 1.0 / 64.0);
			const ShaderCompileSnapshot warm = f0.snap;
			Assert::IsTrue(warm.uniqueDispatchSignatures >= 4 && warm.cacheObjects >= 4 && warm.cacheInserts >= 4,
				L"warm frame must genuinely compile the 4 CfL compute passes into pl_dispatch and pl_cache");
			Assert::IsTrue(warm.compileLogs > 0,
				L"warm frame must produce real libplacebo shader-compile debug log entries");

			// Frame 1: Letterbox active crop {0, 16, 160, 104} (different CFL_TOP/BOTTOM/RH/CY0/CY1/CH).
			ActiveRect letterbox160;
			letterbox160.left = 0; letterbox160.top = 16; letterbox160.right = 160; letterbox160.bottom = 104;
			const FrameRun f1 = runFrame("Frame1_LetterboxCrop", src160Left, sc160, letterbox160, -0.5, 0.0, 10, 0.002, 2, 1.0 / 64.0);
			assertZeroCompiles("Frame1_LetterboxCrop", warm, f1);
			const double f1VsF0 = (std::max)(MaxDiff(f1.cb, f0.cb), MaxDiff(f1.cr, f0.cr));
			LogF("Q#9 Frame1 vs Frame0 max diff: %.3f codes", f1VsF0);
			Assert::IsTrue(f1VsF0 > kDynamicChangeMinCodes,
				L"Frame 1 letterbox crop output must genuinely differ from Frame 0 full output by > 1.0 code");

			// Frame 2: Asymmetric crop {14, 10, 146, 110} (odd CFL_CX0=7, CFL_CY0=5, all 4 crop edges moved).
			ActiveRect asym160;
			asym160.left = 14; asym160.top = 10; asym160.right = 146; asym160.bottom = 110;
			const FrameRun f2 = runFrame("Frame2_AsymCrop", src160Left, sc160, asym160, -0.5, 0.0, 10, 0.002, 2, 1.0 / 64.0);
			assertZeroCompiles("Frame2_AsymCrop", warm, f2);
			const double f2VsF0 = (std::max)(MaxDiff(f2.cb, f0.cb), MaxDiff(f2.cr, f0.cr));
			const double f2VsF1 = (std::max)(MaxDiff(f2.cb, f1.cb), MaxDiff(f2.cr, f1.cr));
			LogF("Q#9 Frame2 vs Frame0 %.3f codes, vs Frame1 %.3f codes", f2VsF0, f2VsF1);
			Assert::IsTrue(f2VsF0 > kDynamicChangeMinCodes && f2VsF1 > kDynamicChangeMinCodes,
				L"Frame 2 asymmetric crop output must genuinely differ from Frame 0 and Frame 1 by > 1.0 code");

			// Frame 3a: Different siting PL_CHROMA_TOP_LEFT (-0.5, -0.5) + 625x larger eps (epsFrac 0.002 -> 0.05).
			const pl_frame src160TopLeft = Frame2(luma160, chroma160, -0.5, -0.5);
			const FrameRun f3a = runFrame("Frame3a_TopLeftAndEps", src160TopLeft, sc160, full160, -0.5, -0.5, 10, 0.05, 2, 1.0 / 64.0);
			assertZeroCompiles("Frame3a_TopLeftAndEps", warm, f3a);
			const double f3aVsF0 = (std::max)(MaxDiff(f3a.cb, f0.cb), MaxDiff(f3a.cr, f0.cr));
			LogF("Q#9 Frame3a (top-left + eps=0.05) vs Frame0 max diff: %.3f codes", f3aVsF0);
			Assert::IsTrue(f3aVsF0 > kDynamicChangeMinCodes,
				L"Frame 3a siting+eps output must genuinely differ from Frame 0 by > 1.0 code");

			// Frame 3b: Different siting (-0.5, -0.5) + full-range 8-bit code scale (bit_shift 6 -> 8, 4x cu, colorDepth 10 -> 8) + crop.
			const pl_frame src160Full8 = Frame2CustomRepr(luma160Full8, chroma160Full8, -0.5, -0.5, PL_COLOR_LEVELS_FULL, 8, 8);
			const FrameRun f3b = runFrame("Frame3b_FullRange8BitScale", src160Full8, sc160Full8, asym160, -0.5, -0.5, 8, 0.01, 2, 1.0 / 256.0);
			assertZeroCompiles("Frame3b_FullRange8BitScale", warm, f3b);

			// Frame 4: Different frame dimensions (128x96, same 4:2:0 subsampling ratio), both full and cropped.
			ActiveRect full128;
			full128.left = 0; full128.top = 0; full128.right = 128; full128.bottom = 96;
			ActiveRect crop128;
			crop128.left = 8; crop128.top = 8; crop128.right = 120; crop128.bottom = 88;
			const pl_frame src128Left = Frame2(luma128, chroma128, -0.5, 0.0);
			const FrameRun f4Full = runFrame("Frame4a_Dim128x96Full", src128Left, sc128, full128, -0.5, 0.0, 10, 0.002, 2, 1.0 / 64.0);
			assertZeroCompiles("Frame4a_Dim128x96Full", warm, f4Full);
			const FrameRun f4Crop = runFrame("Frame4b_Dim128x96Crop", src128Left, sc128, crop128, -0.5, 0.0, 10, 0.002, 2, 1.0 / 64.0);
			assertZeroCompiles("Frame4b_Dim128x96Crop", warm, f4Crop);
			const double f4CropVsFull = (std::max)(MaxDiff(f4Crop.cb, f4Full.cb), MaxDiff(f4Crop.cr, f4Full.cr));
			LogF("Q#9 Frame4b cropped 128x96 vs Frame4a full 128x96 max diff: %.3f codes", f4CropVsFull);
			Assert::IsTrue(f4CropVsFull > kDynamicChangeMinCodes,
				L"Frame 4b cropped 128x96 output must genuinely differ from Frame 4a full 128x96 output by > 1.0 code");

			// Frame 5 (Mandatory in-test negative control on EXACT SAME CflModule, pl_gpu, pl_dispatch, pl_cache):
			// Changing a true compile-time #define parameter (CFL_R: radius 2 -> 3) MUST trigger >= 2 new shader
			// compilations (STATS_BODY + AVG_BODY), proving all 3 compile detectors are live and non-vacuous.
			const FrameRun f5Neg = runFrame("Frame5_NegCtrl_Radius3", src160Left, sc160, full160, -0.5, 0.0, 10, 0.002, 3, 1.0 / 64.0);
			Assert::IsTrue(f5Neg.snap.uniqueDispatchSignatures >= warm.uniqueDispatchSignatures + 2,
				L"negative control: changing compile-time radius (2 -> 3) must produce >= 2 new unique dispatch signatures (STATS + AVG)");
			Assert::IsTrue(f5Neg.snap.cacheObjects >= warm.cacheObjects + 2,
				L"negative control: changing compile-time radius (2 -> 3) must add >= 2 new pl_cache objects");
			Assert::IsTrue(f5Neg.snap.cacheInserts >= warm.cacheInserts + 2,
				L"negative control: changing compile-time radius (2 -> 3) must record >= 2 new pl_cache inserts");
			Assert::AreNotEqual(warm.cacheSignature, f5Neg.snap.cacheSignature,
				L"negative control: changing compile-time radius (2 -> 3) must change pl_cache_signature");
			Assert::IsTrue(f5Neg.snap.compileLogs > warm.compileLogs,
				L"negative control: changing compile-time radius (2 -> 3) must emit new libplacebo compile log entries");
		}
	};

	TEST_CLASS(ChromaCflGpuHardwareTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(ChromaCflGpuHardwareTimingDiagnostic)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChromaCflGpuHardwareTimingDiagnostic)
		{
			// Q#8 (GEM-074): Hardware-GPU-only (VP_TEST_GPU=hardware) timing & parity diagnostic for ChromaCflGpu::Apply
			// at UHD (3840x2160) and HD (1920x1080) with shared grain products and scratch arena (isolating the tiled
			// LO/STATS/AVG/UP passes), plus pl_timer GPU query probe and hardware output sanity parity.
			// Never asserts brittle wall-clock thresholds: checks finite positive GPU duration + real output parity.
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"ChromaCflGpuHardwareTimingDiagnostic", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo hardware test device");
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

				pl_tex luma = UploadRaw(gpu, w, h, "r16", yd);
				pl_tex chroma = UploadRaw(gpu, cw, ch, "rg16", cd);
				ChromaUpscale::Gpu::CflModule module;
				GrainSplit::Gpu::Splitter pL, pB, pR;
				pl_timer timer = pl_timer_create(gpu);
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				struct HwCleanup
				{
					pl_gpu gpu;
					VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena;
					ChromaUpscale::Gpu::CflModule* mod;
					GrainSplit::Gpu::Splitter* sL;
					GrainSplit::Gpu::Splitter* sB;
					GrainSplit::Gpu::Splitter* sR;
					pl_timer* timer;
					pl_tex* luma;
					pl_tex* chroma;
					HwCleanup(pl_gpu g, VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* a, ChromaUpscale::Gpu::CflModule* m,
						GrainSplit::Gpu::Splitter* sl, GrainSplit::Gpu::Splitter* sb, GrainSplit::Gpu::Splitter* sr,
						pl_timer* t, pl_tex* l, pl_tex* c)
						: gpu(g), arena(a), mod(m), sL(sl), sB(sb), sR(sr), timer(t), luma(l), chroma(c) {}
					~HwCleanup()
					{
						if (arena) arena->Release();
						if (mod) mod->Release(gpu);
						if (sL) sL->Release(gpu);
						if (sB) sB->Release(gpu);
						if (sR) sR->Release(gpu);
						if (timer && *timer) pl_timer_destroy(gpu, timer);
						if (chroma && *chroma) pl_tex_destroy(gpu, chroma);
						if (luma && *luma) pl_tex_destroy(gpu, luma);
					}
					HwCleanup(const HwCleanup&) = delete;
					HwCleanup& operator=(const HwCleanup&) = delete;
				} cleanup(gpu, &arena, &module, &pL, &pB, &pR, &timer, &luma, &chroma);

				Assert::IsTrue(luma != nullptr && chroma != nullptr, L"hardware timing source textures uploaded");

				// Build shared grain products once outside the timed loop (also probing pl_timer GPU timestamps).
				const double cu = 64.0 / 65535.0;
				const double lines = static_cast<double>(h), linesC = static_cast<double>(ch);
				GrainSplit::Gpu::Options splitOpts;
				splitOpts.timer = timer;
				const bool splitOk =
					pL.Compute(gpu, dev.dispatch, luma, 0, cu, ChromaUpscale::Scale(lines), ChromaUpscale::BlockPx(lines), GrainSplit::Params(), splitOpts) &&
					pB.Compute(gpu, dev.dispatch, chroma, 0, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC), GrainSplit::Params(), splitOpts) &&
					pR.Compute(gpu, dev.dispatch, chroma, 1, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC), GrainSplit::Params(), splitOpts);
				Assert::IsTrue(splitOk, L"shared grain products computed on hardware GPU");
				pl_gpu_finish(gpu);

				uint64_t plTimerTotalNs = 0;
				if (timer)
				{
					uint64_t q = 0;
					while ((q = pl_timer_query(gpu, timer)) != 0)
						plTimerTotalNs += q;
					Assert::IsTrue(plTimerTotalNs > 0, L"hardware pl_timer must report positive GPU nanoseconds");
				}

				const auto reqs = ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, w, h, cw, ch, lines, linesC, false);
				Assert::IsFalse(reqs.empty(), L"hardware arena requests must be non-empty");
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T2)), L"hardware arena reserve");

				RegionEvidence::GrainProduct sharedProduct;
				sharedProduct.gpu[0] = &pL.Out(); sharedProduct.gpu[1] = &pB.Out(); sharedProduct.gpu[2] = &pR.Out();
				const pl_frame src = Frame2(luma, chroma, -0.5, 0.0);
				ChromaUpscale::Gpu::Params gp;
				gp.settings.mode = ChromaUpscale::Mode::Cfl;
				gp.grain = RegionEvidence::GrainView(&sharedProduct);
				gp.arena = &arena;

				// 3 warm-up frames, then 20 timed frames bracketing CflModule::Apply with pl_gpu_finish.
				for (int k = 0; k < 3; ++k)
				{
					pl_frame out{};
					arena.BeginFrame();
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
				}
				pl_gpu_finish(gpu);

				std::vector<double> ms;
				ms.reserve(20);
				for (int k = 0; k < 20; ++k)
				{
					pl_frame out{};
					arena.BeginFrame();
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
					pl_gpu_finish(gpu);
					const auto t1 = std::chrono::steady_clock::now();
					Assert::AreEqual(std::string("ok"), module.Status(), L"hardware timing must use shared grain products");
					Assert::IsTrue(out.planes[1].texture != nullptr &&
						out.planes[1].texture->params.w == w && out.planes[1].texture->params.h == h,
						L"hardware CfL output must be full luma resolution");
					ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				const double medianMs = ms[ms.size() / 2];
				Assert::IsTrue(std::isfinite(medianMs) && medianMs > 0.0,
					L"hardware CfL median GPU duration must be finite and positive");
				Assert::AreEqual(uint64_t(0), arena.Exhausted(), L"hardware scratch arena must not exhaust");

				char text[256];
				std::snprintf(text, sizeof text,
					"Q#8 HW CfL %dx%d 4:2:0 (shared grain + arena): median %.3f ms (min %.3f ms), split pl_timer %.3f ms, VRAM %.1f MB",
					w, h, medianMs, ms.front(), static_cast<double>(plTimerTotalNs) * 1e-6,
					ChromaUpscale::Gpu::CflModule::TextureBytes(w, h, cw, ch) / 1048576.0);
				Logger::WriteMessage(text);
			}

			// Sanity output parity on hardware GPU + in-test negative control (Mode::Off leaves subsampled chroma untouched).
			{
				const int W = 128, H = 96, cw = 64, ch = 48;
				const Scene sc = MakeScene(W, H, cw, ch, -0.5, 0.0, 819u, 1.5, 2.0);
				const ChromaUpscale::Result ref = RunModel(sc, -0.5, 0.0, ActiveRect());
				const GpuOut hwOut = RunGpu(dev, sc, -0.5, 0.0, ActiveRect());
				Assert::IsTrue(hwOut.ok, Wide(hwOut.error).c_str());
				Assert::IsTrue(hwOut.texW == W && hwOut.texH == H, L"hardware CfL output must be full 128x96 size");
				const double wcb = MaxDiff(hwOut.cb, ref.cb), wcr = MaxDiff(hwOut.cr, ref.cr);
				LogF("Q#8 HW sanity parity 128x96: Cb max %.4f, Cr max %.4f codes", wcb, wcr);
				Assert::IsTrue(wcb <= 0.02 && wcr <= 0.02, L"hardware GPU output must match CPU model within 0.02 codes");

				// In-test negative control on exact same frame: Mode::Off must NOT upsample chroma.
				pl_tex luma = UploadR16(gpu, sc.y);
				pl_tex chroma = UploadRg16(gpu, sc.cb, sc.cr);
				ChromaUpscale::Gpu::CflModule offMod;
				struct OffCleanup
				{
					pl_gpu gpu; ChromaUpscale::Gpu::CflModule* m; pl_tex* l; pl_tex* c;
					OffCleanup(pl_gpu g, ChromaUpscale::Gpu::CflModule* mod, pl_tex* lumaT, pl_tex* chromaT)
						: gpu(g), m(mod), l(lumaT), c(chromaT) {}
					~OffCleanup() { if (m) m->Release(gpu); if (c && *c) pl_tex_destroy(gpu, c); if (l && *l) pl_tex_destroy(gpu, l); }
					OffCleanup(const OffCleanup&) = delete;
					OffCleanup& operator=(const OffCleanup&) = delete;
				} offCleanup(gpu, &offMod, &luma, &chroma);
				Assert::IsTrue(luma != nullptr && chroma != nullptr, L"negative control source textures uploaded");
				const pl_frame src = Frame2(luma, chroma, -0.5, 0.0);
				ChromaUpscale::Gpu::Params offParams;   // Mode::Off
				pl_frame offOut{};
				Assert::IsTrue(offMod.Apply(gpu, dev.dispatch, src, offParams, offOut), Wide(offMod.LastError()).c_str());
				Assert::IsTrue(offOut.planes[1].texture == chroma &&
					offOut.planes[1].texture->params.w == cw && offOut.planes[1].texture->params.h == ch,
					L"negative control: Mode::Off must pass subsampled 64x48 chroma through untouched");
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ChromaCflGpuHardwareCropChangeZeroCompileTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChromaCflGpuHardwareCropChangeZeroCompileTiming)
		{
			// Q#9 (GEM-076): Hardware-GPU-only (VP_TEST_GPU=hardware) verification that toggling active crops between
			// 1920x1080 full frame and 1920x800 letterbox with shared grain products compiles ZERO new shader signatures,
			// records finite positive median GPU frame time via pl_gpu_finish + steady_clock, includes a live negative
			// control (radius 2 -> 3 recompiles >= 2 passes), and maintains exact small-crop CPU parity (<= 0.02 codes).
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"ChromaCflGpuHardwareCropChangeZeroCompileTiming", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo hardware test device");
			pl_gpu gpu = dev.Gpu();

			{
				const int W = 1920, H = 1080, cw = W / 2, ch = H / 2;
				const int cropTop = 140, cropBottom = 940;
				const int cropH = cropBottom - cropTop;   // 800
				const int cTop = cropTop / 2, cH = cropH / 2;   // 70, 400

				std::vector<uint16_t> yFull(static_cast<size_t>(W) * H);
				std::vector<uint16_t> cFull(static_cast<size_t>(cw) * ch * 2);
				for (int y = 0; y < H; ++y)
					for (int x = 0; x < W; ++x)
						yFull[static_cast<size_t>(y) * W + x] = Raw(300.0 + 200.0 * (((x >> 6) + (y >> 6)) & 1) + ((x * 7 + y * 13) & 3));
				for (int y = 0; y < ch; ++y)
					for (int x = 0; x < cw; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * cw + x) * 2;
						cFull[i] = Raw(500.0 + 60.0 * (((x >> 5) + (y >> 5)) & 1) + ((x * 5 + y * 3) & 3));
						cFull[i + 1] = Raw(520.0 - 40.0 * (((x >> 5) + (y >> 5)) & 1) + ((x * 3 + y * 7) & 3));
					}

				std::vector<uint16_t> yCrop(static_cast<size_t>(W) * cropH);
				std::memcpy(yCrop.data(), yFull.data() + static_cast<size_t>(cropTop) * W,
					static_cast<size_t>(W) * cropH * sizeof(uint16_t));
				std::vector<uint16_t> cCrop(static_cast<size_t>(cw) * cH * 2);
				std::memcpy(cCrop.data(), cFull.data() + static_cast<size_t>(cTop) * cw * 2,
					static_cast<size_t>(cw) * cH * 2 * sizeof(uint16_t));

				pl_tex lumaFull = UploadRaw(gpu, W, H, "r16", yFull);
				pl_tex chromaFull = UploadRaw(gpu, cw, ch, "rg16", cFull);
				pl_tex lumaCrop = UploadRaw(gpu, W, cropH, "r16", yCrop);
				pl_tex chromaCrop = UploadRaw(gpu, cw, cH, "rg16", cCrop);

				ChromaUpscale::Gpu::CflModule module;
				GrainSplit::Gpu::Splitter pFL, pFB, pFR, pCL, pCB, pCR;
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);

				struct HwCropCleanup
				{
					pl_gpu gpu;
					VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena;
					ChromaUpscale::Gpu::CflModule* mod;
					GrainSplit::Gpu::Splitter* s0; GrainSplit::Gpu::Splitter* s1; GrainSplit::Gpu::Splitter* s2;
					GrainSplit::Gpu::Splitter* s3; GrainSplit::Gpu::Splitter* s4; GrainSplit::Gpu::Splitter* s5;
					pl_tex* t0; pl_tex* t1; pl_tex* t2; pl_tex* t3;
					HwCropCleanup(pl_gpu g, VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* a, ChromaUpscale::Gpu::CflModule* m,
						GrainSplit::Gpu::Splitter* a0, GrainSplit::Gpu::Splitter* a1, GrainSplit::Gpu::Splitter* a2,
						GrainSplit::Gpu::Splitter* a3, GrainSplit::Gpu::Splitter* a4, GrainSplit::Gpu::Splitter* a5,
						pl_tex* x0, pl_tex* x1, pl_tex* x2, pl_tex* x3)
						: gpu(g), arena(a), mod(m), s0(a0), s1(a1), s2(a2), s3(a3), s4(a4), s5(a5), t0(x0), t1(x1), t2(x2), t3(x3) {}
					~HwCropCleanup()
					{
						if (arena) arena->Release();
						if (mod) mod->Release(gpu);
						for (GrainSplit::Gpu::Splitter* s : { s5, s4, s3, s2, s1, s0 })
							if (s) s->Release(gpu);
						for (pl_tex* t : { t3, t2, t1, t0 })
							if (t && *t) pl_tex_destroy(gpu, t);
					}
					HwCropCleanup(const HwCropCleanup&) = delete;
					HwCropCleanup& operator=(const HwCropCleanup&) = delete;
				} cleanup(gpu, &arena, &module, &pFL, &pFB, &pFR, &pCL, &pCB, &pCR,
					&lumaFull, &chromaFull, &lumaCrop, &chromaCrop);

				Assert::IsTrue(lumaFull && chromaFull && lumaCrop && chromaCrop,
					L"hardware 1920x1080 and 1920x800 source textures uploaded");

				// Pre-compute shared grain products for BOTH 1920x1080 full and 1920x800 letterbox BEFORE attaching
				// ShaderCompileTracker so GrainSplit passes do not mix with CflModule's compile counts.
				const double cu = 64.0 / 65535.0;
				const double lines = static_cast<double>(H), linesC = static_cast<double>(ch);
				const bool splitFullOk =
					pFL.Compute(gpu, dev.dispatch, lumaFull, 0, cu, ChromaUpscale::Scale(lines), ChromaUpscale::BlockPx(lines)) &&
					pFB.Compute(gpu, dev.dispatch, chromaFull, 0, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC)) &&
					pFR.Compute(gpu, dev.dispatch, chromaFull, 1, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
				const bool splitCropOk =
					pCL.Compute(gpu, dev.dispatch, lumaCrop, 0, cu, ChromaUpscale::Scale(lines), ChromaUpscale::BlockPx(lines)) &&
					pCB.Compute(gpu, dev.dispatch, chromaCrop, 0, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC)) &&
					pCR.Compute(gpu, dev.dispatch, chromaCrop, 1, cu, ChromaUpscale::Scale(linesC), ChromaUpscale::BlockPx(linesC));
				Assert::IsTrue(splitFullOk && splitCropOk, L"shared grain products computed for 1920x1080 and 1920x800");
				pl_gpu_finish(gpu);

				std::vector<VpSkeleton::ArenaRequest> reqs =
					ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, W, H, cw, ch, lines, linesC, false);
				const std::vector<VpSkeleton::ArenaRequest> cropReqs =
					ChromaUpscale::Gpu::CflModule::ArenaRequests(gpu, W, cropH, cw, cH, lines, linesC, false);
				reqs.insert(reqs.end(), cropReqs.begin(), cropReqs.end());
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T2)),
					L"hardware arena reserve for both 1920x1080 and 1920x800 grid shapes");

				RegionEvidence::GrainProduct sharedFull, sharedCrop;
				sharedFull.gpu[0] = &pFL.Out(); sharedFull.gpu[1] = &pFB.Out(); sharedFull.gpu[2] = &pFR.Out();
				sharedCrop.gpu[0] = &pCL.Out(); sharedCrop.gpu[1] = &pCB.Out(); sharedCrop.gpu[2] = &pCR.Out();

				const pl_frame src = Frame2(lumaFull, chromaFull, -0.5, 0.0);
				ActiveRect rectFull;
				rectFull.left = 0; rectFull.top = 0; rectFull.right = W; rectFull.bottom = H;
				ActiveRect rectLetterbox;
				rectLetterbox.left = 0; rectLetterbox.top = cropTop; rectLetterbox.right = W; rectLetterbox.bottom = cropBottom;

				// Attach compile tracker strictly around CflModule warmup + crop toggles + negative control.
				ShaderCompileTracker tracker(gpu, dev.dispatch, dev.log);
				Assert::IsNotNull(tracker.cache, L"hardware pl_cache must be created");

				// Warm at 1920x1080 full frame (shared = true).
				for (int w = 0; w < 2; ++w)
				{
					ChromaUpscale::Gpu::Params gp;
					gp.settings.mode = ChromaUpscale::Mode::Cfl;
					gp.active = rectFull;
					gp.grain = RegionEvidence::GrainView(&sharedFull);
					gp.arena = &arena;
					pl_frame out{};
					arena.BeginFrame();
					pl_dispatch_reset_frame(dev.dispatch);
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
					Assert::AreEqual(std::string("ok"), module.Status(), L"hardware warm must use shared grain products");
					Assert::IsTrue(module.LastGrainStatus() == GrainSplit::GrainStatus::Ring,
						L"hardware warm grain status must stay Ring (no own GrainSplit fallback)");
				}
				pl_gpu_finish(gpu);

				const ShaderCompileSnapshot afterWarm = tracker.Snapshot();
				const size_t descAfterWarm = tracker.newPassDescriptions.size();
				Assert::IsTrue(afterWarm.uniqueDispatchSignatures >= 4 && afterWarm.cacheObjects >= 4 && afterWarm.cacheInserts >= 4,
					L"hardware warm must compile CfL passes into pl_dispatch and pl_cache");
				Assert::AreEqual(0, afterWarm.errorLogs, L"hardware warm must emit zero libplacebo error logs");

				// Toggle between letterbox 1920x800 and full 1920x1080 for 10 frames.
				std::vector<double> ms;
				ms.reserve(10);
				for (int k = 0; k < 10; ++k)
				{
					const bool useLetterbox = (k % 2) == 0;
					ChromaUpscale::Gpu::Params gp;
					gp.settings.mode = ChromaUpscale::Mode::Cfl;
					gp.active = useLetterbox ? rectLetterbox : rectFull;
					gp.grain = RegionEvidence::GrainView(useLetterbox ? &sharedCrop : &sharedFull);
					gp.arena = &arena;
					pl_frame out{};
					arena.BeginFrame();
					pl_dispatch_reset_frame(dev.dispatch);
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, gp, out), Wide(module.LastError()).c_str());
					pl_gpu_finish(gpu);
					const auto t1 = std::chrono::steady_clock::now();
					Assert::AreEqual(std::string("ok"), module.Status(), L"hardware toggle must use shared grain products");
					Assert::IsTrue(module.LastGrainStatus() == GrainSplit::GrainStatus::Ring,
						L"hardware toggle grain status must stay Ring (no own GrainSplit fallback)");
					Assert::IsTrue(out.planes[1].texture != nullptr &&
						out.planes[1].texture->params.w == W && out.planes[1].texture->params.h == H,
						L"hardware CfL output must remain full 1920x1080 resolution");
					ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}

				const ShaderCompileSnapshot afterToggle = tracker.Snapshot();
				const std::string unexpectedPasses = tracker.FormatNewDescriptionsSince(descAfterWarm);
				std::sort(ms.begin(), ms.end());
				const double medianMs = ms[ms.size() / 2];

				char msg[320];
				std::snprintf(msg, sizeof msg,
					"Q#9 HW CfL 1920x1080<->1920x800 toggle (10 frames, shared grain): median %.3f ms (min %.3f ms, max %.3f ms) | sigs %zu->%zu cache %d->%d compileLogs %d->%d new=[%s]",
					medianMs, ms.front(), ms.back(),
					afterWarm.uniqueDispatchSignatures, afterToggle.uniqueDispatchSignatures,
					afterWarm.cacheObjects, afterToggle.cacheObjects,
					afterWarm.compileLogs, afterToggle.compileLogs,
					unexpectedPasses.c_str());
				Logger::WriteMessage(msg);

				const std::wstring toggleCtx = Wide("1920x1080<->1920x800 crop toggle compiled unexpected shader(s): " + unexpectedPasses);
				Assert::AreEqual(afterWarm.uniqueDispatchSignatures, afterToggle.uniqueDispatchSignatures,
					(toggleCtx + L" [unique dispatch signatures]").c_str());
				Assert::AreEqual(afterWarm.cacheObjects, afterToggle.cacheObjects,
					(toggleCtx + L" [pl_cache_objects]").c_str());
				Assert::AreEqual(afterWarm.cacheSignature, afterToggle.cacheSignature,
					(toggleCtx + L" [pl_cache_signature]").c_str());
				Assert::AreEqual(afterWarm.cacheInserts, afterToggle.cacheInserts,
					(toggleCtx + L" [pl_cache inserts]").c_str());
				Assert::AreEqual(afterWarm.compileLogs, afterToggle.compileLogs,
					(toggleCtx + L" [libplacebo compile logs]").c_str());
				Assert::AreEqual(afterWarm.errorLogs, afterToggle.errorLogs,
					L"hardware crop toggle must emit zero libplacebo error logs");
				Assert::IsTrue(afterToggle.totalDispatches > afterWarm.totalDispatches,
					L"hardware crop toggle must actually dispatch compute passes");
				Assert::IsTrue(std::isfinite(medianMs) && medianMs > 0.0,
					L"hardware crop-toggle median GPU frame time must be finite and positive");
				Assert::AreEqual(uint64_t(0), arena.Exhausted(), L"hardware scratch arena must not exhaust");

				// Mandatory in-test negative control (exact same CflModule, pl_gpu, pl_dispatch, pl_cache):
				// changing true compile-time radius (2 -> 3) MUST compile >= 2 new shader signatures (STATS + AVG).
				{
					ChromaUpscale::Gpu::Params negGp;
					negGp.settings.mode = ChromaUpscale::Mode::Cfl;
					negGp.active = rectFull;
					negGp.grain = RegionEvidence::GrainView(&sharedFull);
					negGp.arena = &arena;
					negGp.cfl.radius = 3;
					pl_frame negOut{};
					arena.BeginFrame();
					pl_dispatch_reset_frame(dev.dispatch);
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, src, negGp, negOut), Wide(module.LastError()).c_str());
					pl_gpu_finish(gpu);
					const ShaderCompileSnapshot afterNeg = tracker.Snapshot();
					Assert::IsTrue(afterNeg.uniqueDispatchSignatures >= afterWarm.uniqueDispatchSignatures + 2,
						L"hardware negative control: changing radius (2 -> 3) must compile >= 2 new shader signatures");
					Assert::IsTrue(afterNeg.cacheObjects >= afterWarm.cacheObjects + 2,
						L"hardware negative control: changing radius (2 -> 3) must add >= 2 new pl_cache objects");
					Assert::AreNotEqual(afterWarm.cacheSignature, afterNeg.cacheSignature,
						L"hardware negative control: changing radius (2 -> 3) must change pl_cache_signature");
				}
			}

			// Hardware small-crop parity check (160x120 letterbox {0, 16, 160, 104}) vs independent CPU reference.
			{
				const Scene sc = MakeDynamicUniformScene(160, 120, 80, 60, -0.5, 0.0, 909u);
				ActiveRect cropRect;
				cropRect.left = 0; cropRect.top = 16; cropRect.right = 160; cropRect.bottom = 104;
				const GpuOut hwCrop = RunGpu(dev, sc, -0.5, 0.0, cropRect, false, true, false);
				Assert::IsTrue(hwCrop.ok, Wide(hwCrop.error).c_str());
				const ChromaUpscale::Result refCrop = RunModel(sc, -0.5, 0.0, cropRect);
				const ChromaUpscale::Result refFull = RunModel(sc, -0.5, 0.0, ActiveRect());
				const double wcb = MaxDiff(hwCrop.cb, refCrop.cb);
				const double wcr = MaxDiff(hwCrop.cr, refCrop.cr);
				const double vsFull = (std::max)(MaxDiff(hwCrop.cb, refFull.cb), MaxDiff(hwCrop.cr, refFull.cr));
				LogF("Q#9 HW small-crop parity 160x120 {0,16,160,104}: max (%.4f, %.4f) codes, vs full %.3f codes", wcb, wcr, vsFull);
				Assert::IsTrue(wcb <= 0.02 && wcr <= 0.02,
					L"hardware small-crop CfL output must match independent CPU model within 0.02 codes");
				Assert::IsTrue(vsFull > 1.0,
					L"hardware small-crop output must genuinely differ from uncropped full-frame CPU model by > 1.0 code");
			}
		}
	};
}
