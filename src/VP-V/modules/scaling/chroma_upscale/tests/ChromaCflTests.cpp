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
				Assert::IsTrue(arena.Acquired() >= 6, L"the arena was really used (six chroma-grid temporaries)");
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
			// SA7: on a backend where the 2-component float format falls back to the 4-component one, the six temporaries have one
			// shape: one request of 6, not two requests the plan could count as 4 + 2 of different pools. Distinct formats: 4 and 2.
			const auto same = ChromaUpscale::Gpu::CflModule::GridRequests(VpSkeleton::TexFormat::RGBA32F, VpSkeleton::TexFormat::RGBA32F, 64, 48);
			Assert::IsTrue(same.size() == 1 && same[0].count == 6, L"equal formats: one request of six");
			const auto split = ChromaUpscale::Gpu::CflModule::GridRequests(VpSkeleton::TexFormat::RGBA32F, VpSkeleton::TexFormat::RG32F, 64, 48);
			Assert::IsTrue(split.size() == 2 && split[0].count == 4 && split[1].count == 2, L"distinct formats: 4 + 2");
			const VpSkeleton::ArenaPlan plan = VpSkeleton::PlanArena(same, VpSkeleton::Tier::T2);
			Assert::IsTrue(plan.pools.size() == 1 && plan.pools[0].capacity == 6, L"the plan reserves all six");
			Assert::IsTrue(plan.OwnerCount(ChromaUpscale::Gpu::kArenaScope, ChromaUpscale::Gpu::kArenaOwner, same[0].shape) == 6, L"the owner may hold six");
		}

		TEST_METHOD(ChromaCflGpuTextureBytesCountsTheArenaTemporaries)
		{
			// SA7: with the temporaries in the arena the module's own bytes drop by 16 per luma pixel (the luma split's bands) and 112
			// per chroma pixel (lo..avg 80 + two chroma splits' bands 32); the default form is the private-texture form, unchanged.
			const int W = 3840, H = 2160, cw = 1920, ch = 1080;
			const uint64_t priv = ChromaUpscale::Gpu::CflModule::TextureBytes(W, H, cw, ch), inArena = ChromaUpscale::Gpu::CflModule::TextureBytes(W, H, cw, ch, true);
			Assert::IsTrue(priv == ChromaUpscale::Gpu::CflModule::TextureBytes(W, H, cw, ch, false), L"the default is the private form");
			Assert::IsTrue(priv - inArena == uint64_t(16) * W * H + uint64_t(112) * cw * ch, L"the temporaries move to the arena");
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
	};
}
