#include <pch.h>
#include "CrossfadeMix.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>

// Hunt 3 F1 (CrossfadeMix.h has the design).
namespace CrossfadeMix
{
	namespace
	{
		constexpr unsigned kOutputStages = PL_HOOK_PRE_OUTPUT | PL_HOOK_OUTPUT;

		// The captured picture at the output pixel's position in its own rectangle (the two
		// renders share the target and its crop, so this is the same pixel; nearest, no
		// filtering), mixed with the new path's colour in the signal domain.
		const char* const MIX_HEADER = R"GLSL(
vec3 cf_from_at(vec2 q)
{
    vec2 u = (q - cf_out_rect.xy) / cf_out_rect.zw;
    vec2 s = cf_from_rect.xy + u * cf_from_rect.zw;
    ivec2 size = textureSize(cf_from, 0);
    return texelFetch(cf_from, clamp(ivec2(floor(s)), ivec2(0), size - 1), 0).rgb;
}
)GLSL";

		const char* const MIX_BODY = R"GLSL(
color.rgb = cf_from_at(gl_FragCoord.xy) * (1.0 - cf_weight) + color.rgb * cf_weight;
)GLSL";

		pl_hook_res Failure()
		{
			pl_hook_res res{};
			res.failed = true;
			return res;
		}
	}

	double MixValue(double from, double to, double weight)
	{
		const double w = (std::min)(1.0, (std::max)(0.0, weight));
		return from * (1.0 - w) + to * w;
	}

	std::vector<const pl_hook*> FromPathHooks(const pl_hook* const* hooks, int count, const pl_hook* lateRestore)
	{
		std::vector<const pl_hook*> out;
		for (int i = 0; i < count; ++i)
		{
			const pl_hook* h = hooks[i];
			if (!h) continue;
			if (h == lateRestore || (static_cast<unsigned>(h->stages) & kOutputStages) == 0)
				out.push_back(h);
		}
		return out;
	}

	size_t MixInsertIndex(const std::vector<const pl_hook*>& hooks, const pl_hook* lateRestore)
	{
		if (lateRestore)
			for (size_t i = 0; i < hooks.size(); ++i)
				if (hooks[i] == lateRestore)
					return i + 1;
		return 0;
	}

	Mixer::Mixer()
	{
		capture.stages = PL_HOOK_PRE_OUTPUT;
		capture.input = PL_HOOK_SIG_TEX;
		capture.priv = this;
		capture.hook = &Mixer::RunCapture;
		capture.signature = 0x5650584643415054ull;   // "VPXFCAPT"
		mix.stages = PL_HOOK_PRE_OUTPUT;
		mix.input = PL_HOOK_SIG_TEX;
		mix.priv = this;
		mix.hook = &Mixer::RunMix;
		mix.signature = 0x565058464D495858ull;   // "VPXFMIXX"
	}

	Mixer::~Mixer()
	{
		// Textures belong to the gpu: Release(gpu) first.
	}

	void Mixer::Release(pl_gpu gpu)
	{
		if (gpu && owner == gpu)
			for (pl_tex* t : { &scratch, &captured, &mixedTex })
				if (*t) pl_tex_destroy(gpu, t);
		scratch = captured = mixedTex = nullptr;
		owner = nullptr;
		capturedValid = false;
	}

	bool Mixer::EnsureFloat(pl_gpu gpu, pl_tex& tex, int w, int h)
	{
		if (tex && tex->params.w == w && tex->params.h == h)
			return true;
		const pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE));
		if (!fmt)
			return false;
		pl_tex_params tp{};
		tp.w = w;
		tp.h = h;
		tp.format = fmt;
		tp.sampleable = true;
		tp.renderable = true;
		return pl_tex_recreate(gpu, &tex, &tp);
	}

	// The path being left's PRE_OUTPUT picture into VP's rgba32f texture (a plain copy pass);
	// the stage is left as it was (the scratch target's output is never shown).
	pl_hook_res Mixer::RunCapture(void* priv, const pl_hook_params* params)
	{
		Mixer* self = static_cast<Mixer*>(priv);
		if (!params->tex)
			return Failure();
		if (self->owner != params->gpu)
			return Failure();
		if (!self->EnsureFloat(params->gpu, self->captured, params->tex->params.w, params->tex->params.h))
			return Failure();
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = 4;
		if (!pl_shader_sample_direct(sh, &src))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return Failure();
		}
		pl_dispatch_params dp{};
		dp.shader = &sh;
		dp.target = self->captured;
		if (!pl_dispatch_finish(params->dispatch, &dp))
			return Failure();
		self->capturedRect = params->rect;
		self->capturedValid = true;
		return pl_hook_res{};
	}

	// The new path's PRE_OUTPUT picture mixed with the captured one into VP's float texture
	// (the texture form: libplacebo rejects an inline PRE_OUTPUT hook, ExtendedRange.cpp).
	pl_hook_res Mixer::RunMix(void* priv, const pl_hook_params* params)
	{
		Mixer* self = static_cast<Mixer*>(priv);
		if (!params->tex)
			return Failure();
		// Nothing captured for this frame: leave the stage untouched rather than fail (a failure
		// disables the hook for the renderer's life).
		if (!self->capturedValid || !self->captured || self->owner != params->gpu)
			return pl_hook_res{};
		if (!self->EnsureFloat(params->gpu, self->mixedTex, params->tex->params.w, params->tex->params.h))
			return Failure();
		const pl_rect2df& r = params->rect;
		const pl_rect2df& f = self->capturedRect;
		const float outRect[4] = { r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0 };
		const float fromRect[4] = { f.x0, f.y0, f.x1 - f.x0, f.y1 - f.y0 };
		const float weight = self->weight;
		const pl_shader_var variables[3] = {
			{ pl_var_vec4("cf_out_rect"), outRect, true },
			{ pl_var_vec4("cf_from_rect"), fromRect, true },
			{ pl_var_float("cf_weight"), &weight, true } };
		pl_shader_desc from{};
		from.desc.name = "cf_from";
		from.desc.type = PL_DESC_SAMPLED_TEX;
		from.binding.object = self->captured;
		from.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		from.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		pl_custom_shader custom{};
		custom.description = "VP crossfade: signal-domain mix";
		custom.header = MIX_HEADER;
		custom.input = PL_SHADER_SIG_COLOR;
		custom.output = PL_SHADER_SIG_COLOR;
		custom.descriptors = &from;
		custom.num_descriptors = 1;
		custom.variables = variables;
		custom.num_variables = 3;
		custom.body = MIX_BODY;
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = 4;
		if (!pl_shader_sample_direct(sh, &src) || !pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return Failure();
		}
		pl_dispatch_params dp{};
		dp.shader = &sh;
		dp.target = self->mixedTex;
		if (!pl_dispatch_finish(params->dispatch, &dp))
			return Failure();
		++self->mixed;
		self->capturedValid = false;   // used: once per RenderFrom
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = self->mixedTex;
		res.repr = params->repr;
		res.color = params->color;
		res.components = params->components;
		res.rect = params->rect;
		return res;
	}

	bool Mixer::RenderFrom(pl_renderer renderer, pl_gpu gpu, const pl_frame& from, const pl_frame& destination,
		const pl_render_params& params, const pl_hook* lateRestore)
	{
		capturedValid = false;
		if (!renderer || !gpu || destination.num_planes != 1 || !destination.planes[0].texture)
		{
			++failures;
			return false;
		}
		if (owner && owner != gpu)
			Release(owner);
		owner = gpu;
		const pl_tex dst = destination.planes[0].texture;
		if (!scratch || scratch->params.w != dst->params.w || scratch->params.h != dst->params.h ||
			scratch->params.format != dst->params.format)
		{
			pl_tex_params tp{};
			tp.w = dst->params.w;
			tp.h = dst->params.h;
			tp.format = dst->params.format;
			tp.renderable = true;
			// libplacebo clears a target outside its crop (pl_tex_clear needs blit_dst).
			tp.blit_dst = (dst->params.format->caps & PL_FMT_CAP_BLITTABLE) != 0;
			if (!pl_tex_recreate(gpu, &scratch, &tp))
			{
				++failures;
				return false;
			}
		}
		pl_frame target = destination;
		target.planes[0].texture = scratch;
		target.overlays = nullptr;
		target.num_overlays = 0;
		std::vector<const pl_hook*> hooks = FromPathHooks(params.hooks, params.num_hooks, lateRestore);
		hooks.push_back(&capture);
		pl_render_params p = params;
		p.hooks = hooks.data();
		p.num_hooks = static_cast<int>(hooks.size());
		p.info_callback = nullptr;
		p.info_priv = nullptr;
		if (!pl_render_image(renderer, &from, &target, &p) || !capturedValid)
		{
			capturedValid = false;
			++failures;
			return false;
		}
		return true;
	}

	const pl_hook* Mixer::MixHook(double w)
	{
		weight = static_cast<float>((std::min)(1.0, (std::max)(0.0, w)));
		return &mix;
	}

	std::vector<const pl_hook*> Mixer::WithMix(const pl_hook* const* hooks, int count, const pl_hook* lateRestore, double w)
	{
		std::vector<const pl_hook*> out(hooks, hooks + (hooks ? count : 0));
		out.insert(out.begin() + static_cast<std::ptrdiff_t>(MixInsertIndex(out, lateRestore)), MixHook(w));
		return out;
	}
}
