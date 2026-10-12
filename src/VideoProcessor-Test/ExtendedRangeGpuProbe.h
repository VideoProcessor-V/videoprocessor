#pragma once

// Stage 10 GPU probe, shared by the native tests (libplacebo 7.360) and the
// Beta test program (libplacebo 377): 10-bit limited 4:2:2 YCbCr planes
// rendered by pl_render_image into a 10-bit limited RGB target, with
// `preserve_extended_range` on (ExtendedRange::Decide / Prepare, as the
// renderer calls them) or off (libplacebo's own path). Returns RGB codes.
// No test framework here; the callers assert.

#include <vprenderer/ExtendedRange.h>

#include <libplacebo/renderer.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ExtendedRangeGpuProbe
{
	struct Case
	{
		enum pl_color_primaries sourcePrimaries = PL_COLOR_PRIM_BT_709;
		enum pl_color_primaries outputPrimaries = PL_COLOR_PRIM_DISPLAY_P3;
		enum pl_color_transfer sourceTransfer = PL_COLOR_TRC_GAMMA22;
		enum pl_color_transfer outputTransfer = PL_COLOR_TRC_GAMMA22;
		double scale = 1.0;
		bool on = true;
		bool sigmoid = true;
		bool linear = true;
		// The owner's upscaler (ewa_lanczossharp); contrast recovery is HDR-only.
		bool ownerScaling = false;
		// A further hook (e.g. a user shader that writes alpha), after VP's.
		const pl_hook* extraHook = nullptr;
	};

	struct Planes
	{
		int width = 0, height = 0;
		std::vector<uint16_t> luma;     // 10-bit codes << 6
		std::vector<uint16_t> chroma;   // Cb, Cr interleaved, half width
	};

	inline uint16_t Code(int code) { return static_cast<uint16_t>(code << 6); }

	// Columns of neutral grey, one code each.
	inline Planes Greys(const std::vector<int>& codes, int columnWidth = 32, int height = 16)
	{
		Planes p;
		p.width = static_cast<int>(codes.size()) * columnWidth;
		p.height = height;
		p.luma.resize(static_cast<size_t>(p.width) * height);
		p.chroma.assign(static_cast<size_t>(p.width / 2) * height * 2, Code(512));
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < p.width; ++x)
				p.luma[static_cast<size_t>(y) * p.width + x] = Code(codes[x / columnWidth]);
		return p;
	}

	// Hard in-range edges: 1-, 2- and 7-pixel bars, black/white on top and
	// two greys below.
	inline Planes Edges()
	{
		Planes p;
		p.width = 256;
		p.height = 32;
		p.luma.resize(static_cast<size_t>(p.width) * p.height);
		p.chroma.assign(static_cast<size_t>(p.width / 2) * p.height * 2, Code(512));
		for (int y = 0; y < p.height; ++y)
			for (int x = 0; x < p.width; ++x)
			{
				const int period = x < 64 ? 2 : x < 128 ? 4 : 14;
				const bool on = (x / (period / 2)) % 2 == 0;
				const int code = y < 16 ? (on ? 940 : 64) : (on ? 600 : 200);
				p.luma[static_cast<size_t>(y) * p.width + x] = Code(code);
			}
		return p;
	}

	struct Patch { int y, cb, cr; };

	// Coloured patches (BT.709 YCbCr), one column each.
	inline Planes Patches(const std::vector<Patch>& patches, int columnWidth = 32, int height = 16)
	{
		Planes p;
		p.width = static_cast<int>(patches.size()) * columnWidth;
		p.height = height;
		p.luma.resize(static_cast<size_t>(p.width) * height);
		p.chroma.resize(static_cast<size_t>(p.width / 2) * height * 2);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < p.width; ++x)
				p.luma[static_cast<size_t>(y) * p.width + x] = Code(patches[x / columnWidth].y);
			for (int x = 0; x < p.width / 2; ++x)
			{
				const Patch& patch = patches[(x * 2) / columnWidth];
				p.chroma[(static_cast<size_t>(y) * (p.width / 2) + x) * 2] = Code(patch.cb);
				p.chroma[(static_cast<size_t>(y) * (p.width / 2) + x) * 2 + 1] = Code(patch.cr);
			}
		}
		return p;
	}

	struct Result
	{
		bool ok = false;
		std::string error;
		int width = 0, height = 0;
		std::vector<uint32_t> pixels;   // R10G10B10A2
		ExtendedRange::Decision decision;
		unsigned long long restores = 0;

		int Channel(int x, int y, int channel) const
		{
			return static_cast<int>((pixels[static_cast<size_t>(y) * width + x] >> (10 * channel)) & 1023u);
		}
	};

	inline Result Render(pl_gpu gpu, pl_renderer renderer, const Planes& planes, const Case& c)
	{
		Result result;
		const auto sampleCaps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
		pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, sampleCaps);
		pl_fmt rg16 = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, sampleCaps);
		pl_fmt t10 = nullptr;
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
		for (int i = 0; i < gpu->num_formats; ++i)
		{
			pl_fmt f = gpu->formats[i];
			if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
				f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10) { t10 = f; break; }
		}
		if (!r16 || !rg16 || !t10) { result.error = "formats"; return result; }
		pl_tex_params lp{}; lp.w = planes.width; lp.h = planes.height; lp.format = r16; lp.sampleable = true;
		lp.initial_data = planes.luma.data();
		pl_tex_params cp{}; cp.w = planes.width / 2; cp.h = planes.height; cp.format = rg16; cp.sampleable = true;
		cp.initial_data = planes.chroma.data();
		result.width = static_cast<int>(planes.width * c.scale);
		result.height = static_cast<int>(planes.height * c.scale);
		pl_tex_params tp{}; tp.w = result.width; tp.h = result.height; tp.format = t10; tp.renderable = true;
		tp.host_readable = true;
		pl_tex lt = pl_tex_create(gpu, &lp), ct = pl_tex_create(gpu, &cp), tt = pl_tex_create(gpu, &tp);
		if (lt && ct && tt)
		{
			pl_frame image{};
			image.num_planes = 2;
			image.planes[0].texture = lt; image.planes[0].components = 1;
			image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			image.planes[1].texture = ct; image.planes[1].components = 2;
			image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			image.repr.sys = PL_COLOR_SYSTEM_BT_709;
			image.repr.levels = PL_COLOR_LEVELS_LIMITED;
			image.repr.bits.sample_depth = 16; image.repr.bits.color_depth = 10; image.repr.bits.bit_shift = 6;
			image.color.primaries = c.sourcePrimaries;
			image.color.transfer = c.sourceTransfer;
			pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
			pl_frame out{};
			out.num_planes = 1;
			out.planes[0].texture = tt;
			out.planes[0].components = 3;
			for (int i = 0; i < 3; ++i) out.planes[0].component_mapping[i] = i;
			out.repr.sys = PL_COLOR_SYSTEM_RGB;
			out.repr.levels = PL_COLOR_LEVELS_LIMITED;
			out.repr.bits.sample_depth = 10; out.repr.bits.color_depth = 10;
			out.color.primaries = c.outputPrimaries;
			out.color.transfer = c.outputTransfer;
			pl_render_params params = pl_render_default_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.sigmoid_params = c.sigmoid ? &pl_sigmoid_default_params : nullptr;
			params.disable_linear_scaling = !c.linear;
			if (c.ownerScaling)
				params.upscaler = &pl_filter_ewa_lanczossharp;
			ExtendedRange::Hooks hooks;
			std::vector<const pl_hook*> list;
			result.decision = ExtendedRange::Decide(c.on, image.color, out.color, false);
			ExtendedRange::Prepare(result.decision, hooks, image.color, out.color, list);
			if (c.extraHook)
				list.push_back(c.extraHook);
			params.hooks = list.empty() ? nullptr : list.data();
			params.num_hooks = static_cast<int>(list.size());
			if (pl_render_image(renderer, &image, &out, &params))
			{
				pl_gpu_finish(gpu);
				result.pixels.resize(static_cast<size_t>(result.width) * result.height);
				pl_tex_transfer_params d{}; d.tex = tt; d.ptr = result.pixels.data();
				result.ok = pl_tex_download(gpu, &d);
				if (!result.ok) result.error = "download";
			}
			else
				result.error = "pl_render_image";
			result.restores = hooks.Applied();
		}
		else
			result.error = "textures";
		pl_tex_destroy(gpu, &lt);
		pl_tex_destroy(gpu, &ct);
		pl_tex_destroy(gpu, &tt);
		return result;
	}

	// The green channel at the middle of each grey column.
	inline std::vector<int> Columns(const Result& r, int columns, double scale, int columnWidth = 32)
	{
		std::vector<int> codes;
		for (int col = 0; col < columns; ++col)
			codes.push_back(r.Channel(static_cast<int>((col * columnWidth + columnWidth / 2) * scale), r.height / 2, 1));
		return codes;
	}

	// 64 + 876 * sign(v)|v|^(source/output), v = (code - 64) / 876.
	inline int Ideal(int code, double sourceGamma, double outputGamma)
	{
		const double v = (code - 64) / 876.0;
		const double o = (v < 0 ? -1.0 : 1.0) * std::pow(std::abs(v), sourceGamma / outputGamma);
		return static_cast<int>(std::lround(64 + 876 * o));
	}

	struct Difference { int max = 0; double mean = 0.0; };

	inline Difference Compare(const Result& a, const Result& b)
	{
		Difference d;
		if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) { d.max = 1 << 20; return d; }
		double sum = 0.0;
		for (size_t i = 0; i < a.pixels.size(); ++i)
			for (int ch = 0; ch < 3; ++ch)
			{
				const int diff = std::abs(static_cast<int>((a.pixels[i] >> (10 * ch)) & 1023u) -
					static_cast<int>((b.pixels[i] >> (10 * ch)) & 1023u));
				d.max = diff > d.max ? diff : d.max;
				sum += diff;
			}
		d.mean = sum / (a.pixels.size() * 3.0);
		return d;
	}

	inline std::wstring ToWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	inline std::string Join(const std::vector<int>& codes)
	{
		std::string s;
		for (int c : codes) s += (s.empty() ? "" : " ") + std::to_string(c);
		return s;
	}
}
