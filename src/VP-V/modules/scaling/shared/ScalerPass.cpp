// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: ScalerPass (see ScalerPass.h). The libplacebo calls mirror today's users exactly
// (SourceRebase's chroma passes: across first, rect = the whole texture; AdaptiveDownscale's faithful ortho
// branch: down first, integer window on the unscaled axis, offset rect in the second pass) so the identity
// tests can compare bit for bit.
#include <pch.h>

#include <VP-V/modules/scaling/shared/ScalerPass.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ScalerPass
{
	namespace
	{
		constexpr unsigned kRenderCaps = PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR;

		const char* KindName(ScalerKernels::Kind k)
		{
			switch (k)
			{
			case ScalerKernels::Kind::Lanczos: return "lanczos";
			case ScalerKernels::Kind::CatmullRom: return "catmull_rom";
			case ScalerKernels::Kind::Bicubic: return "bicubic";
			case ScalerKernels::Kind::Hermite: return "hermite";
			case ScalerKernels::Kind::Bilinear: return "bilinear";
			default: return "ewa_lanczossharp";
			}
		}

		bool Ortho(pl_dispatch dispatch, pl_tex from, pl_tex to, float x0, float y0, float x1, float y1, int newW, int newH,
			const pl_filter_config& cfg, float antiring, pl_shader_obj* lut)
		{
			pl_shader sh = pl_dispatch_begin(dispatch);
			pl_sample_src s{};
			s.tex = from;
			s.rect = { x0, y0, x1, y1 };
			s.new_w = newW;
			s.new_h = newH;
			s.components = from->params.format->num_components;
			pl_sample_filter_params f{};
			f.filter = cfg;
			f.antiring = antiring;
			f.lut = lut;
			pl_dispatch_params d{};
			d.shader = &sh;
			d.target = to;
			if (!pl_shader_sample_ortho2(sh, &s, &f) || !pl_dispatch_finish(dispatch, &d))
			{
				if (sh) pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			return true;
		}

		bool Polar(pl_dispatch dispatch, pl_tex from, pl_tex to, float x0, float y0, float x1, float y1, int newW, int newH,
			const pl_filter_config& cfg, float antiring, pl_shader_obj* lut)
		{
			pl_shader sh = pl_dispatch_begin(dispatch);
			pl_sample_src s{};
			s.tex = from;
			s.rect = { x0, y0, x1, y1 };
			s.new_w = newW;
			s.new_h = newH;
			s.components = from->params.format->num_components;
			pl_sample_filter_params f{};
			f.filter = cfg;
			f.antiring = antiring;
			f.lut = lut;
			pl_dispatch_params d{};
			d.shader = &sh;
			d.target = to;
			if (!pl_shader_sample_polar(sh, &s, &f) || !pl_dispatch_finish(dispatch, &d))
			{
				if (sh) pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			return true;
		}
	}

	pl_filter_config FilterConfig(const ScalerKernels::Spec& spec)
	{
		pl_filter_config c{};
		switch (spec.kind)
		{
		case ScalerKernels::Kind::Lanczos:
			c = pl_filter_lanczos;
			c.radius = static_cast<float>(spec.radius);
			break;
		case ScalerKernels::Kind::CatmullRom: c = pl_filter_catmull_rom; break;
		case ScalerKernels::Kind::Bicubic:
			c = pl_filter_bicubic;
			c.params[0] = static_cast<float>(spec.B);
			c.params[1] = static_cast<float>(spec.C);
			break;
		case ScalerKernels::Kind::Hermite: c = pl_filter_hermite; break;
		case ScalerKernels::Kind::Bilinear: c = pl_filter_bilinear; break;
		default: c = pl_filter_ewa_lanczossharp; break;
		}
		return c;
	}

	const char* Name(Axis a)
	{
		return a == Axis::X ? "x" : a == Axis::Y ? "y" : "both";
	}

	void Module::Release(pl_gpu gpu)
	{
		for (int s = 0; s < kSlots; ++s)
		{
			if (textures[s] && gpu) pl_tex_destroy(gpu, &textures[s]);
			textures[s] = nullptr;
			if (temps[s] && gpu) pl_tex_destroy(gpu, &temps[s]);
			temps[s] = nullptr;
			pl_shader_obj_destroy(&luts[s][0]);
			pl_shader_obj_destroy(&luts[s][1]);
		}
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const Source& src, int dstW, int dstH, const Settings& st, pl_tex& out, int slot)
	{
		error.clear();
		auto fail = [this](const std::string& why) {
			error = "ScalerPass: " + why;
			++failures;
			return false;
		};
		if (!gpu || !dispatch || !src.tex || !src.tex->params.sampleable) return fail("no sampleable source");
		if (st.light == Light::Linear) return fail("linear light is not built (B0 scales in the signal domain)");
		slot = (std::max)(0, (std::min)(slot, kSlots - 1));

		const float sx0 = (src.x1 > src.x0 && src.y1 > src.y0) ? src.x0 : 0.0f;
		const float sy0 = (src.x1 > src.x0 && src.y1 > src.y0) ? src.y0 : 0.0f;
		const float sx1 = (src.x1 > src.x0 && src.y1 > src.y0) ? src.x1 : static_cast<float>(src.tex->params.w);
		const float sy1 = (src.x1 > src.x0 && src.y1 > src.y0) ? src.y1 : static_cast<float>(src.tex->params.h);
		const bool polar = ScalerKernels::IsPolar(st.filter.kind);
		if (polar && st.axis != Axis::Both) return fail("ewa_lanczossharp is a both-axes (polar) filter");

		// The unscaled axis of a single-axis pass keeps its integer window (as the faithful ortho branch).
		const ScalerGeometry::Window wx = ScalerGeometry::IntegerWindow(sx0, sx1), wy = ScalerGeometry::IntegerWindow(sy0, sy1);
		int outW = dstW, outH = dstH;
		if (st.axis == Axis::X) outH = wy.size;
		if (st.axis == Axis::Y) outW = wx.size;
		if ((st.axis == Axis::X && dstH != 0 && dstH != wy.size) || (st.axis == Axis::Y && dstW != 0 && dstW != wx.size))
			return fail("the unscaled axis must keep its window size");
		if (outW < 1 || outH < 1) return fail("no destination size");

		// The span, with the siting phase on each scaled axis.
		const bool scaleX = st.axis != Axis::Y, scaleY = st.axis != Axis::X;
		const float px = scaleX ? st.sitingX : 0.0f, py = scaleY ? st.sitingY : 0.0f;
		const float ax0 = sx0 + px, ax1 = sx1 + px, ay0 = sy0 + py, ay1 = sy1 + py;

		// The output format: the target's own (Target), else 32-bit float (default) or half float (visible).
		const pl_fmt srcFmt = src.tex->params.format;
		pl_fmt fmt = nullptr;
		std::string newNote;
		if (st.precision == Precision::Target)
			fmt = st.targetFormat ? st.targetFormat : srcFmt;
		else
		{
			const int bits = st.precision == Precision::F32 ? 32 : 16;
			fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, srcFmt->num_components, bits, bits, static_cast<enum pl_fmt_caps>(kRenderCaps));
			if (st.precision == Precision::F16)
				newNote = "scaler precision: half float (f16 requested)";
		}
		if (!fmt || !(fmt->caps & PL_FMT_CAP_RENDERABLE)) return fail("no renderable output format");

		auto ensure = [&](pl_tex& t, int w, int h) {
			pl_tex_params p{};
			p.w = w;
			p.h = h;
			p.format = fmt;
			p.sampleable = true;
			p.renderable = true;
			p.blit_src = true;
			return pl_tex_recreate(gpu, &t, &p);
		};
		if (!ensure(textures[slot], outW, outH)) return fail("texture allocation failed");

		const pl_filter_config cfg = FilterConfig(st.filter);
		bool ok = true;
		if (polar)
			ok = Polar(dispatch, src.tex, textures[slot], ax0, ay0, ax1, ay1, outW, outH, cfg, st.antiring, &luts[slot][0]);
		else if (st.axis == Axis::X)
			ok = Ortho(dispatch, src.tex, textures[slot], ax0, static_cast<float>(wy.origin), ax1, static_cast<float>(wy.origin + wy.size),
				outW, wy.size, cfg, st.antiring, &luts[slot][0]);
		else if (st.axis == Axis::Y)
			ok = Ortho(dispatch, src.tex, textures[slot], static_cast<float>(wx.origin), ay0, static_cast<float>(wx.origin + wx.size), ay1,
				wx.size, outH, cfg, st.antiring, &luts[slot][0]);
		else if (st.order == Order::AcrossFirst)
		{
			// Across into a temporary (rows of the integer window), then down.
			const ScalerGeometry::Window w = ScalerGeometry::IntegerWindow(ay0, ay1);
			if (!ensure(temps[slot], outW, w.size)) return fail("texture allocation failed");
			ok = Ortho(dispatch, src.tex, temps[slot], ax0, static_cast<float>(w.origin), ax1, static_cast<float>(w.origin + w.size),
					outW, w.size, cfg, st.antiring, &luts[slot][0]) &&
				Ortho(dispatch, temps[slot], textures[slot], 0.0f, ay0 - w.origin, static_cast<float>(outW), ay1 - w.origin,
					outW, outH, cfg, st.antiring, &luts[slot][1]);
		}
		else
		{
			// Down into a temporary (columns of the integer window), then across.
			const ScalerGeometry::Window w = ScalerGeometry::IntegerWindow(ax0, ax1);
			if (!ensure(temps[slot], w.size, outH)) return fail("texture allocation failed");
			ok = Ortho(dispatch, src.tex, temps[slot], static_cast<float>(w.origin), ay0, static_cast<float>(w.origin + w.size), ay1,
					w.size, outH, cfg, st.antiring, &luts[slot][0]) &&
				Ortho(dispatch, temps[slot], textures[slot], ax0 - w.origin, 0.0f, ax1 - w.origin, static_cast<float>(outH),
					outW, outH, cfg, st.antiring, &luts[slot][1]);
		}
		if (!ok) return fail("sampling pass failed");

		out = textures[slot];
		note = newNote;
		char line[200];
		std::snprintf(line, sizeof line, "ScalerPass: %s %s %s, %.4gx%.4g (derived ratios), %dx%d -> %dx%d, %s", KindName(st.filter.kind),
			Name(st.axis), st.plane == Plane::Chroma ? "chroma" : "luma", (static_cast<double>(sx1) - sx0) / outW,
			(static_cast<double>(sy1) - sy0) / outH, src.tex->params.w, src.tex->params.h, outW, outH, fmt->name ? fmt->name : "?");
		status = line;
		return true;
	}
}
