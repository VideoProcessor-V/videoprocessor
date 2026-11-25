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

		// Hermite (cubic B=C=0): 1 - 3x^2 + 2x^3 on [0,1], zero beyond. libplacebo's
		// ortho sampler turns an all-positive kernel (radius == radius_zero) into
		// half-as-many hardware-bilinear fetches whose sub-texel fraction is only
		// ~8-bit, costing ~1 code at 10-bit. This kernel gives the identical
		// weights but a -1e-12 tail on (1,1.5): that is far below fp16/fp32 LUT
		// resolution (the filter LUT stores it as a ~zero weight; it is not removed,
		// it is just negligible), and it makes radius > radius_zero so the exact
		// one-tap-per-texel path runs (about 4x the texture reads of the 2-fetch path).
		// ExactTapsTaken() checks this on an EQUIVALENT filter (same kernel, window and
		// radius; the sampler's own filter has 256 LUT entries and blur = inv_scale, and blur
		// scales radius and radius_zero alike, so the comparison is the same); it is checked,
		// not assumed. The real fix is an explicit fork flag (102b UPSC1-EXACT-TAPS-FLAG).
		double HermiteExactWeight(const pl_filter_ctx*, double x)
		{
			x = x < 0 ? -x : x;
			if (x < 1.0) return 1.0 - 3.0 * x * x + 2.0 * x * x * x;
			return x < 1.5 ? -1e-12 : 0.0;
		}
		const pl_filter_function kHermiteExact = { "hermite_exact", 2.0f, false, {}, {}, HermiteExactWeight };

		// Bilinear (triangle, 1 - |x| on [0,1]) is all-positive too, so it had the same ~1 code
		// hardware-bilinear error (C4 union WARP TRX: 1.0084 codes at 2.22x, upsc1 SHOULD 6). The same
		// -1e-12 tail on (1,1.5) keeps the weights and makes the exact one-tap-per-texel path run.
		double BilinearExactWeight(const pl_filter_ctx*, double x)
		{
			x = x < 0 ? -x : x;
			if (x < 1.0) return 1.0 - x;
			return x < 1.5 ? -1e-12 : 0.0;
		}
		const pl_filter_function kBilinearExact = { "bilinear_exact", 2.0f, false, {}, {}, BilinearExactWeight };

		// True when libplacebo's ortho sampler will NOT use the half-fetch hardware-bilinear
		// path for this filter (it does so exactly when radius == radius_zero).
		bool ExactTapsTaken(const pl_filter_config& cfg)
		{
			pl_filter_params fp{};
			fp.config = cfg;
			fp.lut_entries = 64;
			fp.row_stride_align = 4;
			pl_filter f = pl_filter_generate(nullptr, &fp);
			if (!f) return false;
			const bool exact = f->radius != f->radius_zero;
			pl_filter_free(&f);
			return exact;
		}

		// The Hermite config is a constant (FilterConfig ignores the spec's parameters for it), so
		// the filter is generated once, not on every Apply on the render thread.
		bool HermiteTakesExactTaps(const pl_filter_config& cfg)
		{
			static const bool exact = ExactTapsTaken(cfg);
			return exact;
		}

		// The Bilinear config is a constant too (no spec parameters).
		bool BilinearTakesExactTaps(const pl_filter_config& cfg)
		{
			static const bool exact = ExactTapsTaken(cfg);
			return exact;
		}

		// Sub-LSB floating-point identity guard for bypassing an unscaled separable axis:
		// 1e-5f < 1/65536 (16-bit LSB ~= 1.53e-5) and << 1/256 (libplacebo's 256-entry LUT phase step ~= 3.91e-3),
		// so any coordinate within this tolerance of an integer texel grid has sub-LSB shift even on a full-scale
		// step edge, while any real subpixel siting phase or non-unit ratio is rejected.
		constexpr float kAxisIdentityEps = 1e-5f;

		// True when [a0, a1] is an exact integer-aligned 1:1 span completely inside [0, srcDim] (zero fractional
		// offset, exact unit scale, and no out-of-bounds clamping), returning its integer start index in `originOut`.
		bool IsExactIdentityAxis(float a0, float a1, int outDim, int srcDim, int& originOut)
		{
			if (!std::isfinite(a0) || !std::isfinite(a1) || outDim < 1 || outDim > srcDim)
				return false;
			const float r0 = std::round(a0);
			if (std::fabs(a0 - r0) > kAxisIdentityEps)
				return false;
			if (std::fabs((static_cast<double>(a1) - static_cast<double>(a0)) - static_cast<double>(outDim)) > static_cast<double>(kAxisIdentityEps))
				return false;
			if (a0 < 0.0f || a1 > static_cast<float>(srcDim))
				return false;
			const int i0 = static_cast<int>(r0);
			if (i0 < 0 || i0 > srcDim - outDim)
				return false;
			originOut = i0;
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
		case ScalerKernels::Kind::Hermite:
			c = pl_filter_hermite;
			c.kernel = &kHermiteExact;
			break;
		case ScalerKernels::Kind::Bilinear:
			c = pl_filter_bilinear;
			c.kernel = &kBilinearExact;
			break;
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

	size_t Module::TextureBytes() const
	{
		size_t total = 0;
		auto add = [&](pl_tex t) {
			if (!t || !t->params.format || t->params.w <= 0 || t->params.h <= 0)
				return;
			const size_t bpp = t->params.format->texel_size ? t->params.format->texel_size : t->params.format->internal_size;
			total += static_cast<size_t>(t->params.w) * static_cast<size_t>(t->params.h) * bpp;
		};
		for (int s = 0; s < kSlots; ++s)
		{
			add(textures[s]);
			add(temps[s]);
		}
		return total;
	}

	pl_fmt Module::SelectFloatFormat(pl_gpu gpu, int comps, Precision precision, bool preferStorable, bool& fellBackTo16)
	{
		fellBackTo16 = false;
		if (!gpu || comps < 1)
			return nullptr;
		auto findFloatBits = [&](int bits, unsigned caps) -> pl_fmt {
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, comps, bits, bits, static_cast<enum pl_fmt_caps>(caps));
			if (!f && comps == 3)
				f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, bits, bits, static_cast<enum pl_fmt_caps>(caps));
			return f;
		};
		// POD2-M6: preserve 32-bit float precision before falling back to 16-bit half-float:
		// F32+storable -> F32+renderable -> F16+storable -> F16+renderable.
		if (precision == Precision::F32)
		{
			if (preferStorable)
				if (pl_fmt f = findFloatBits(32, kRenderCaps | PL_FMT_CAP_STORABLE))
					return f;
			if (pl_fmt f = findFloatBits(32, kRenderCaps))
				return f;
			pl_fmt f16 = preferStorable ? findFloatBits(16, kRenderCaps | PL_FMT_CAP_STORABLE) : nullptr;
			if (!f16)
				f16 = findFloatBits(16, kRenderCaps);
			if (f16)
				fellBackTo16 = true;
			return f16;
		}
		if (preferStorable)
			if (pl_fmt f = findFloatBits(16, kRenderCaps | PL_FMT_CAP_STORABLE))
				return f;
		return findFloatBits(16, kRenderCaps);
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
		if (slot < 0 || slot >= kSlots) return fail("slot out of range (0..2)");

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
		const bool preferStorable = polar && gpu->glsl.compute && gpu->glsl.max_shmem_size > 0;
		if (st.precision == Precision::Target)
		{
			fmt = st.targetFormat ? st.targetFormat : srcFmt;
			if (preferStorable && fmt && !(fmt->caps & PL_FMT_CAP_STORABLE))
			{
				const int reqBits = fmt->component_depth[0] > 0 ? fmt->component_depth[0] : 16;
				if (pl_fmt f = pl_find_fmt(gpu, fmt->type, fmt->num_components, reqBits, reqBits,
					static_cast<enum pl_fmt_caps>(kRenderCaps | PL_FMT_CAP_STORABLE)))
					fmt = f;
				else if (fmt->num_components == 3)
					if (pl_fmt f4 = pl_find_fmt(gpu, fmt->type, 4, reqBits, reqBits,
						static_cast<enum pl_fmt_caps>(kRenderCaps | PL_FMT_CAP_STORABLE)))
						fmt = f4;
			}
		}
		else
		{
			bool fellBackTo16 = false;
			fmt = SelectFloatFormat(gpu, srcFmt->num_components, st.precision, preferStorable, fellBackTo16);
			if (fellBackTo16)
				newNote = "scaler precision: half float (f32 not renderable)";
			else if (st.precision == Precision::F16)
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
			p.storable = (fmt->caps & PL_FMT_CAP_STORABLE) != 0;
			p.blit_src = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			p.blit_dst = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			return pl_tex_recreate(gpu, &t, &p);
		};
		if (!ensure(textures[slot], outW, outH)) return fail("texture allocation failed");

		const pl_filter_config cfg = FilterConfig(st.filter);
		if (st.filter.kind == ScalerKernels::Kind::Hermite && !HermiteTakesExactTaps(cfg))
			return fail("Hermite did not take the exact-tap ortho path (hardware-bilinear half fetches would cost ~1 code)");
		if (st.filter.kind == ScalerKernels::Kind::Bilinear && !BilinearTakesExactTaps(cfg))
			return fail("Bilinear did not take the exact-tap ortho path (hardware-bilinear half fetches would cost ~1 code)");

		auto dropTemp = [&]() {
			if (temps[slot])
			{
				pl_tex_destroy(gpu, &temps[slot]);
				temps[slot] = nullptr;
			}
		};

		bool ok = true;
		const char* pathTag = "";
		if (polar)
		{
			dropTemp();
			ok = Polar(dispatch, src.tex, textures[slot], ax0, ay0, ax1, ay1, outW, outH, cfg, st.antiring, &luts[slot][0]);
		}
		else if (st.axis == Axis::X)
		{
			dropTemp();
			ok = Ortho(dispatch, src.tex, textures[slot], ax0, static_cast<float>(wy.origin), ax1, static_cast<float>(wy.origin + wy.size),
				outW, wy.size, cfg, st.antiring, &luts[slot][0]);
		}
		else if (st.axis == Axis::Y)
		{
			dropTemp();
			ok = Ortho(dispatch, src.tex, textures[slot], static_cast<float>(wx.origin), ay0, static_cast<float>(wx.origin + wx.size), ay1,
				wx.size, outH, cfg, st.antiring, &luts[slot][0]);
		}
		else
		{
			int ix0 = 0, iy0 = 0;
			const bool idX = IsExactIdentityAxis(ax0, ax1, outW, src.tex->params.w, ix0);
			const bool idY = IsExactIdentityAxis(ay0, ay1, outH, src.tex->params.h, iy0);
			if (idX && idY)
			{
				dropTemp();
				const bool canBlit = srcFmt == fmt && src.tex != textures[slot] && src.tex->params.d == 0 &&
					src.tex->params.blit_src && textures[slot]->params.blit_dst &&
					(srcFmt->caps & PL_FMT_CAP_BLITTABLE) != 0 && (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
				if (canBlit)
				{
					pl_tex_blit_params bp{};
					bp.src = src.tex;
					bp.dst = textures[slot];
					bp.src_rc = { ix0, iy0, 0, ix0 + outW, iy0 + outH, 1 };
					bp.dst_rc = { 0, 0, 0, outW, outH, 1 };
					bp.sample_mode = PL_TEX_SAMPLE_NEAREST;
					pl_tex_blit(gpu, &bp);
					pathTag = " [1:1 blit]";
				}
				else
				{
					ok = Ortho(dispatch, src.tex, textures[slot], static_cast<float>(ix0), static_cast<float>(iy0),
						static_cast<float>(ix0 + outW), static_cast<float>(iy0 + outH), outW, outH, cfg, st.antiring, &luts[slot][0]);
					pathTag = " [1:1 single-pass]";
				}
			}
			else if (idX)
			{
				dropTemp();
				ok = Ortho(dispatch, src.tex, textures[slot], static_cast<float>(ix0), ay0, static_cast<float>(ix0 + outW), ay1,
					outW, outH, cfg, st.antiring, &luts[slot][0]);
				pathTag = " [y-only bypass]";
			}
			else if (idY)
			{
				dropTemp();
				ok = Ortho(dispatch, src.tex, textures[slot], ax0, static_cast<float>(iy0), ax1, static_cast<float>(iy0 + outH),
					outW, outH, cfg, st.antiring, &luts[slot][0]);
				pathTag = " [x-only bypass]";
			}
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
		}
		if (!ok) return fail("sampling pass failed");

		out = textures[slot];
		note = newNote;
		char line[256];
		std::snprintf(line, sizeof line, "ScalerPass: %s %s %s, %.4gx%.4g (derived ratios), %dx%d -> %dx%d, %s%s", KindName(st.filter.kind),
			Name(st.axis), st.plane == Plane::Chroma ? "chroma" : "luma", scaleX ? (static_cast<double>(sx1) - sx0) / outW : 1.0,
			scaleY ? (static_cast<double>(sy1) - sy0) / outH : 1.0, src.tex->params.w, src.tex->params.h, outW, outH,
			fmt->name ? fmt->name : "?", pathTag);
		status = line;
		return true;
	}
}
