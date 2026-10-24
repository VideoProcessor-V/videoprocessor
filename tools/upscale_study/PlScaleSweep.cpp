// PlScaleSweep: one libplacebo upscale of a luma plane, for the upscale quality study
// (tools/upscale_study, docs/design/upscale-study.md).
//
// Why: VP's renderer exposes a fixed upscaler list and no anti-ringing strength for the main
// upscaler. The study also needs spline36 and an anti-ringing sweep, so this harness runs the
// SAME libplacebo sampling code VP's scalers use (pl_shader_sample_ortho2 twice, H then V, for the
// separable filters; pl_shader_sample_polar for EWA) on a headless D3D11 device, with a pl_timer.
// Intermediates are r32f (the best case; VP uses rgba16f / rgba32f with fbo_float32 on).
// Gamma-domain luma only. VP's linear-light option and its hybrid blends are measured through
// VpScaleRender instead.
//
//   PlScaleSweep --in <raw> W H [--in-format gray10|gray16] --size OW OH --filter <name>
//                [--antiring A] [--adapter NVIDIA] [--repeat N] --out <raw16>
// Writes <out> (gray16le, value = round(clamp(y) * 65535), y normalised as code / 1023 for gray10)
// and <out>.json {width, height, filter, antiring, polar, gpu_ms_median, device}.
// Exit 0 ok, 2 bad arguments, 3 device / shader failure.
//
// Build (no project in the solution): tools/upscale_study/build_sweep.bat.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/sampling.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	struct Args
	{
		std::string in, out, filter, adapter = "NVIDIA", inFormat = "gray10";
		int w = 0, h = 0, ow = 0, oh = 0, repeat = 1;
		float antiring = 0.0f;
	};

	bool Parse(int argc, char** argv, Args& a)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string k = argv[i];
			auto need = [&](int n) { return i + n < argc; };
			if (k == "--in" && need(3)) { a.in = argv[++i]; a.w = std::atoi(argv[++i]); a.h = std::atoi(argv[++i]); }
			else if (k == "--in-format" && need(1)) a.inFormat = argv[++i];
			else if (k == "--size" && need(2)) { a.ow = std::atoi(argv[++i]); a.oh = std::atoi(argv[++i]); }
			else if (k == "--filter" && need(1)) a.filter = argv[++i];
			else if (k == "--antiring" && need(1)) a.antiring = static_cast<float>(std::atof(argv[++i]));
			else if (k == "--adapter" && need(1)) a.adapter = argv[++i];
			else if (k == "--repeat" && need(1)) a.repeat = std::max(1, std::atoi(argv[++i]));
			else if (k == "--out" && need(1)) a.out = argv[++i];
			else return false;
		}
		return !a.in.empty() && !a.out.empty() && !a.filter.empty() && a.w > 0 && a.h > 0 && a.ow > 0 && a.oh > 0;
	}

	pl_d3d11_params Device(const std::string& want, std::string& name)
	{
		pl_d3d11_params p{};
		name = "default adapter";
		IDXGIFactory1* factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
		{
			IDXGIAdapter1* ad = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				DXGI_ADAPTER_DESC1 d{};
				ad->GetDesc1(&d);
				ad->Release();
				char n[128] = {};
				WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof(n) - 1, nullptr, nullptr);
				if (std::string(n).find(want) != std::string::npos && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				{
					p.adapter_luid = d.AdapterLuid;
					name = n;
					break;
				}
			}
			factory->Release();
		}
		return p;
	}
}

static int RunOne(const Args& a, pl_gpu gpu, pl_dispatch dispatch, const std::string& device)
{
	const pl_filter_config* base = pl_find_filter_config(a.filter.c_str(), PL_FILTER_UPSCALING);
	if (!base)
	{
		std::fprintf(stderr, "unknown filter %s\n", a.filter.c_str());
		return 2;
	}
	std::vector<uint16_t> raw(static_cast<size_t>(a.w) * a.h);
	{
		std::ifstream f(a.in, std::ios::binary);
		if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2)))
		{
			std::fprintf(stderr, "cannot read %s\n", a.in.c_str());
			return 2;
		}
	}
	const float norm = a.inFormat == "gray16" ? 1.0f / 65535.0f : 1.0f / 1023.0f;
	std::vector<float> plane(raw.size());
	for (size_t i = 0; i < raw.size(); ++i) plane[i] = raw[i] * norm;

	pl_fmt r32 = pl_find_named_fmt(gpu, "r32f");
	pl_tex_params tp{};
	tp.w = a.w; tp.h = a.h; tp.format = r32; tp.sampleable = true; tp.initial_data = plane.data();
	pl_tex src = pl_tex_create(gpu, &tp);
	pl_tex_params mp{};
	mp.w = a.ow; mp.h = a.h; mp.format = r32; mp.sampleable = true; mp.renderable = true;
	pl_tex mid = pl_tex_create(gpu, &mp);
	pl_tex_params op{};
	op.w = a.ow; op.h = a.oh; op.format = r32; op.renderable = true; op.blit_src = true; op.sampleable = true;
	pl_tex dst = pl_tex_create(gpu, &op);
	if (!src || !mid || !dst) { std::fprintf(stderr, "texture creation failed\n"); return 3; }

	pl_filter_config cfg = *base;
	cfg.antiring = a.antiring;
	const bool polar = cfg.polar;
	pl_shader_obj lut = nullptr;
	pl_timer timer = pl_timer_create(gpu), timer2 = pl_timer_create(gpu);   // one per pass (pl_timer rule)
	std::vector<double> ms;
	for (int r = 0; r < a.repeat; ++r)
	{
		bool ok;
		if (polar)
		{
			pl_shader sh = pl_dispatch_begin(dispatch);
			pl_sample_src s{};
			s.tex = src; s.components = 1; s.new_w = a.ow; s.new_h = a.oh;
			pl_sample_filter_params fp{};
			fp.filter = cfg; fp.antiring = a.antiring; fp.lut = &lut;
			ok = pl_shader_sample_polar(sh, &s, &fp);
			pl_dispatch_params d{}; d.shader = &sh; d.target = dst; d.timer = timer;
			ok = ok && pl_dispatch_finish(dispatch, &d);
		}
		else
		{
			pl_shader sh = pl_dispatch_begin(dispatch);   // horizontal first (VP's order: H then V)
			pl_sample_src s{};
			s.tex = src; s.components = 1; s.new_w = a.ow; s.new_h = a.h;
			pl_sample_filter_params fp{};
			fp.filter = cfg; fp.antiring = a.antiring; fp.lut = &lut;
			ok = pl_shader_sample_ortho2(sh, &s, &fp);
			pl_dispatch_params d{}; d.shader = &sh; d.target = mid; d.timer = timer;
			ok = ok && pl_dispatch_finish(dispatch, &d);
			pl_shader sh2 = pl_dispatch_begin(dispatch);
			pl_sample_src s2{};
			s2.tex = mid; s2.components = 1; s2.new_w = a.ow; s2.new_h = a.oh;
			ok = ok && pl_shader_sample_ortho2(sh2, &s2, &fp);
			pl_dispatch_params d2{}; d2.shader = &sh2; d2.target = dst; d2.timer = timer2;
			ok = ok && pl_dispatch_finish(dispatch, &d2);
		}
		if (!ok) { std::fprintf(stderr, "sampling shader failed (%s)\n", a.filter.c_str()); return 3; }
		pl_gpu_finish(gpu);
		uint64_t ns = 0, q;
		while ((q = pl_timer_query(gpu, timer)) != 0) ns += q;   // the pass(es) of this repeat
		while ((q = pl_timer_query(gpu, timer2)) != 0) ns += q;
		if (ns && r > 0) ms.push_back(ns * 1e-6);              // repeat 0 includes shader compilation
	}
	// Read back (r32f) and write gray16.
	pl_tex_params rp{};
	rp.w = a.ow; rp.h = a.oh; rp.format = r32; rp.host_readable = true; rp.blit_dst = true;
	pl_tex readable = pl_tex_create(gpu, &rp);
	pl_tex_blit_params bp{}; bp.src = dst; bp.dst = readable;
	pl_tex_blit(gpu, &bp);
	std::vector<float> outp(static_cast<size_t>(a.ow) * a.oh);
	pl_tex_transfer_params tr{}; tr.tex = readable; tr.ptr = outp.data();
	if (!pl_tex_download(gpu, &tr)) { std::fprintf(stderr, "download failed\n"); return 3; }
	std::vector<uint16_t> o16(outp.size());
	for (size_t i = 0; i < outp.size(); ++i)
		o16[i] = static_cast<uint16_t>(std::lround(std::min(std::max(outp[i], 0.0f), 1.0f) * 65535.0f));
	{
		std::ofstream f(a.out, std::ios::binary);
		f.write(reinterpret_cast<const char*>(o16.data()), static_cast<std::streamsize>(o16.size() * 2));
	}
	std::sort(ms.begin(), ms.end());
	const double median = ms.empty() ? -1.0 : ms[ms.size() / 2];
	{
		std::ofstream j(a.out + ".json");
		j << "{\"width\": " << a.ow << ", \"height\": " << a.oh << ", \"filter\": \"" << a.filter << "\", \"antiring\": " << a.antiring
		  << ", \"polar\": " << (polar ? "true" : "false") << ", \"gpu_ms_median\": " << (median < 0 ? std::string("null") : std::to_string(median))
		  << ", \"device\": \"" << device << "\"}\n";
	}
	pl_timer_destroy(gpu, &timer);
	pl_timer_destroy(gpu, &timer2);
	pl_shader_obj_destroy(&lut);
	pl_tex_destroy(gpu, &readable);
	pl_tex_destroy(gpu, &src);
	pl_tex_destroy(gpu, &mid);
	pl_tex_destroy(gpu, &dst);
	return 0;
}

int main(int argc, char** argv)
{
	// --batch <file> [--adapter NAME]: one argument line per job, one device for all (the study
	// runs thousands of small upscales; a device + shader compile per process would dominate).
	std::vector<std::vector<std::string>> jobs;
	std::string adapter = "NVIDIA";
	if (argc >= 3 && std::string(argv[1]) == "--batch")
	{
		if (argc >= 5 && std::string(argv[3]) == "--adapter") adapter = argv[4];
		std::ifstream f(argv[2]);
		std::string line;
		while (std::getline(f, line))
		{
			std::vector<std::string> t;
			size_t i = 0;
			while (i < line.size())
			{
				while (i < line.size() && line[i] == ' ') ++i;
				size_t j = i;
				while (j < line.size() && line[j] != ' ' && line[j] != '\r') ++j;
				if (j > i) t.push_back(line.substr(i, j - i));
				i = j + 1;
			}
			if (!t.empty()) jobs.push_back(t);
		}
	}
	else
	{
		std::vector<std::string> t;
		for (int i = 1; i < argc; ++i) t.push_back(argv[i]);
		jobs.push_back(t);
	}
	std::vector<Args> parsed;
	for (const auto& t : jobs)
	{
		std::vector<char*> av{ argv[0] };
		for (const std::string& x : t) av.push_back(const_cast<char*>(x.c_str()));
		Args a;
		if (!Parse(static_cast<int>(av.size()), av.data(), a))
		{
			std::fprintf(stderr, "usage: PlScaleSweep [--batch <file> [--adapter NAME]] | --in <raw> W H [--in-format gray10|gray16] --size OW OH --filter <name> [--antiring A] [--adapter NVIDIA] [--repeat N] --out <raw16>\n");
			return 2;
		}
		parsed.push_back(a);
	}
	if (!parsed.empty() && jobs.size() == 1 && argc >= 2 && std::string(argv[1]) != "--batch") adapter = parsed[0].adapter;
	pl_log_params lp{};
	lp.log_level = PL_LOG_ERR;
	lp.log_cb = [](void*, enum pl_log_level, const char* msg) { std::fprintf(stderr, "libplacebo: %s\n", msg); };
	pl_log log = pl_log_create(PL_API_VER, &lp);
	std::string device;
	const pl_d3d11_params dp = Device(adapter, device);
	pl_d3d11 d3d = pl_d3d11_create(log, &dp);
	if (!d3d) { std::fprintf(stderr, "no D3D11 device\n"); return 3; }
	pl_dispatch dispatch = pl_dispatch_create(log, d3d->gpu);
	int rc = 0;
	for (const Args& a : parsed)
	{
		rc = RunOne(a, d3d->gpu, dispatch, device);
		if (rc) break;
	}
	pl_dispatch_destroy(&dispatch);
	pl_d3d11_destroy(&d3d);
	pl_log_destroy(&log);
	return rc;
}
