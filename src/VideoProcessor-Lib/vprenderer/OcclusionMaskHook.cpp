#include <pch.h>
#include "OcclusionMaskHook.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cmath>
#include <string>

namespace OcclusionMask
{
	namespace
	{
		// One compute pass: the picture unchanged, the mask in alpha. Each
		// output pixel maps back to the source block it shows; outside the
		// destination rectangle (bars, blanking) alpha is 1.
		// The NLS / NLS+ per-axis map (shaders/NLS.glsl, NLSPlus.glsl): the
		// source position, in the hooked texture's normalised coordinates, that
		// an NLS output position shows.
		const char* const MASK_HEADER = R"GLSL(
float nls_radius(float radius, float scale, float protect)
{
    if (nls_geometry < 0.5 || protect <= 0.0)
        return scale * radius - (scale - 1.0) * pow(radius, nls_curve);
    if (radius <= protect)
        return radius * scale;
    float span = 1.0 - protect;
    float t = clamp((radius - protect) / span, 0.0, 1.0);
    float t2 = t * t;
    float t3 = t2 * t;
    float t4 = t3 * t;
    float t5 = t4 * t;
    float h00 = 1.0 - 10.0 * t3 + 15.0 * t4 - 6.0 * t5;
    float h10 = t - 6.0 * t3 + 8.0 * t4 - 3.0 * t5;
    float h01 = 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
    float h11 = -4.0 * t3 + 7.0 * t4 - 3.0 * t5;
    float edge_slope = max(0.20, 1.0 - 0.5 * (scale - 1.0) * nls_curve);
    return h00 * protect * scale + h10 * span * scale + h01 + h11 * span * edge_slope;
}

float nls_coordinate(float c, float scale, float protect)
{
    float centered = c * 2.0 - 1.0;
    float r = nls_radius(abs(centered), scale, protect);
    return (centered < 0.0 ? -r : r) * 0.5 + 0.5;
}
)GLSL";

		const char* const MASK_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    vec4 c = texelFetch(hooked, p, 0);
    vec2 o = vec2(p) + vec2(0.5);
    float m = 1.0;
    float g = 0.0;
    if (all(greaterThanEqual(o, dst_rect.xy)) && all(lessThan(o, dst_rect.zw))) {
        vec2 s = src_origin + (o - dst_rect.xy) * src_per_dst;
        if (nls_active > 0.5) {
            vec2 u = (s - nls_domain.xy) / nls_domain.zw;
            u = clamp(vec2(nls_coordinate(u.x, nls_scale.x, nls_protect.x),
                           nls_coordinate(u.y, nls_scale.y, nls_protect.y)), vec2(0.0), vec2(1.0));
            s = nls_domain.xy + u * nls_domain.zw;
        }
#ifdef MASK
        ivec2 blk = clamp(ivec2(floor(s / 16.0)), ivec2(0), textureSize(mask, 0) - ivec2(1));
        m = texelFetch(mask, blk, 0).r > 0.5 ? 1.0 : 0.0;
#endif
#ifdef GRAIN
        ivec2 gb = clamp(ivec2(floor(s / 16.0)), ivec2(0), textureSize(grain, 0) - ivec2(1));
        vec4 gm = texelFetch(grain, gb, 0);
        g = gm.g > 0.5 ? clamp((1.0 - gm.r) / max(1.0 - grain_floor, 1e-3), 0.0, 1.0) : 0.0;
#endif
    }
#ifdef GRAIN
    imageStore(dst, p, vec4(c.rgb, m > 0.5 ? 1.0 - 0.49 * g : 0.49 * g));
#else
    imageStore(dst, p, vec4(c.rgb, m));
#endif
}
)GLSL";

		pl_hook_res Failure()
		{
			pl_hook_res res{};
			res.failed = true;
			return res;
		}
	}

	const char* NlsMapGlsl()
	{
		return MASK_HEADER;
	}

	Hook::Hook()
	{
		hook.stages = PL_HOOK_OUTPUT;
		hook.input = PL_HOOK_SIG_TEX;
		hook.priv = this;
		hook.hook = &Hook::Run;
		hook.signature = 0x56504F43434D534Bull;   // "VPOCCMSK"
	}

	void Hook::SetFrame(pl_tex maskIn, const pl_rect2df& sourceCrop, const pl_rect2df& targetCrop)
	{
		mask = maskIn;
		source = sourceCrop;
		target = targetCrop;
		nls = {};
	}

	void Hook::SetNls(const NlsMaskMapping::Mapping& mapping, const pl_rect2df& hookedRect)
	{
		nls = mapping;
		nlsDomain = hookedRect;
	}

	pl_hook_res Hook::Run(void* priv, const pl_hook_params* params)
	{
		return static_cast<Hook*>(priv)->Apply(params);
	}

	pl_hook_res Hook::Apply(const pl_hook_params* params)
	{
		pl_hook_res none{};
		none.output = PL_HOOK_SIG_NONE;
		if (!params->tex || (!mask && !grain))
			return none;
		const int w = params->tex->params.w, h = params->tex->params.h;
		pl_tex out = params->get_tex(params->priv, w, h);
		if (!out || !out->params.storable)
		{
			++failed;
			return Failure();
		}
		// The OUTPUT texture is either the destination rectangle itself or
		// the whole target with the picture at the destination rectangle.
		const float targetW = std::fabs(target.x1 - target.x0), targetH = std::fabs(target.y1 - target.y0);
		const bool cropSized = std::fabs(targetW - static_cast<float>(w)) < 1.0f &&
			std::fabs(targetH - static_cast<float>(h)) < 1.0f;
		const float dstRect[4] = {
			cropSized ? 0.0f : target.x0, cropSized ? 0.0f : target.y0,
			cropSized ? static_cast<float>(w) : target.x1, cropSized ? static_cast<float>(h) : target.y1 };
		const float srcOrigin[2] = { source.x0, source.y0 };
		const float srcPerDst[2] = {
			targetW > 0.0f ? (source.x1 - source.x0) / targetW : 1.0f,
			targetH > 0.0f ? (source.y1 - source.y0) / targetH : 1.0f };

		pl_shader_desc descriptors[4]{};
		int numDescriptors = 0;
		auto sampled = [&](const char* name, pl_tex tex)
		{
			pl_shader_desc& d = descriptors[numDescriptors++];
			d.desc.name = name;
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = tex;
			d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		};
		sampled("hooked", params->tex);
		if (mask)
			sampled("mask", mask);
		if (grain)
			sampled("grain", grain);
		pl_shader_desc& dst = descriptors[numDescriptors++];
		dst.desc.name = "dst";
		dst.desc.type = PL_DESC_STORAGE_IMG;
		dst.desc.access = PL_DESC_ACCESS_WRITEONLY;
		dst.binding.object = out;
		const float nlsActive = nls.valid ? 1.0f : 0.0f;
		const float nlsCurve = static_cast<float>(nls.curve);
		const float nlsGeometry = static_cast<float>(nls.geometry);
		const float nlsScale[2] = { static_cast<float>(nls.xScale), static_cast<float>(nls.yScale) };
		const float nlsProtect[2] = { static_cast<float>(nls.xProtect), static_cast<float>(nls.yProtect) };
		const float domain[4] = { nlsDomain.x0, nlsDomain.y0,
			std::fabs(nlsDomain.x1 - nlsDomain.x0) > 0.0f ? nlsDomain.x1 - nlsDomain.x0 : 1.0f,
			std::fabs(nlsDomain.y1 - nlsDomain.y0) > 0.0f ? nlsDomain.y1 - nlsDomain.y0 : 1.0f };
		const float grainFloor = grainFloorValue;
		const pl_shader_var variables[10] = {
			{ pl_var_vec4("dst_rect"), dstRect, true },
			{ pl_var_vec2("src_origin"), srcOrigin, true },
			{ pl_var_vec2("src_per_dst"), srcPerDst, true },
			{ pl_var_float("nls_active"), &nlsActive, true },
			{ pl_var_float("nls_curve"), &nlsCurve, true },
			{ pl_var_float("nls_geometry"), &nlsGeometry, true },
			{ pl_var_vec2("nls_scale"), nlsScale, true },
			{ pl_var_vec2("nls_protect"), nlsProtect, true },
			{ pl_var_vec4("nls_domain"), domain, true },
			{ pl_var_float("grain_floor"), &grainFloor, true } };

		const std::string header = std::string(mask ? "#define MASK\n" : "") + (grain ? "#define GRAIN\n" : "") +
			MASK_HEADER;
		pl_custom_shader custom{};
		custom.description = grain ? "VideoProcessor occlusion mask and grain for the sigma-delta" :
			"VideoProcessor occlusion mask for the sigma-delta";
		custom.header = header.c_str();
		custom.body = MASK_BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = numDescriptors;
		custom.variables = variables;
		custom.num_variables = grain ? 10 : 9;
		custom.compute = true;
		custom.compute_group_size[0] = 16;
		custom.compute_group_size[1] = 16;
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			++failed;
			return Failure();
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (w + 15) / 16;
		compute.dispatch_size[1] = (h + 15) / 16;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(params->dispatch, &compute))
		{
			++failed;
			return Failure();
		}
		++applied;
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = out;
		res.repr = params->repr;
		res.color = params->color;
		res.components = 4;
		res.rect = params->rect;
		return res;
	}
}
