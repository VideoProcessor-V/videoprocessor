#include <pch.h>
#include "RenderedWarp.h"

#include <vprenderer/DejudderWarp.h>
#include <vprenderer/OcclusionMaskHook.h>

#include <libplacebo/shaders/custom.h>

#include <cmath>
#include <string>

namespace DejudderRendered
{
	namespace
	{
		const char* const COPY_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
if (q.x < imageSize(dst).x && q.y < imageSize(dst).y)
    imageStore(dst, q, texelFetch(src, q, 0));
)GLSL";

		// The source position an output (cache) pixel shows, and a 4-tap
		// Catmull-Rom sample of the cached picture (edge clamp, no value
		// clamp).
		const char* const WARP_FUNCTIONS = R"GLSL(
vec2 rw_source(vec2 p)
{
    vec2 s = src_origin + (p - dst_origin) * src_per_dst;
    if (nls_active > 0.5) {
        vec2 u = (s - nls_domain.xy) / nls_domain.zw;
        u = clamp(vec2(nls_coordinate(u.x, nls_scale.x, nls_protect.x),
                       nls_coordinate(u.y, nls_scale.y, nls_protect.y)), vec2(0.0), vec2(1.0));
        s = nls_domain.xy + u * nls_domain.zw;
    }
    return s;
}

vec4 rw_cubic(vec2 pos)
{
    ivec2 size = textureSize(cached, 0);
    vec2 f = pos - vec2(0.5);
    vec2 i = floor(f);
    vec2 t = f - i;
    vec2 w0 = t * (-0.5 + t * (1.0 - 0.5 * t));
    vec2 w1 = 1.0 + t * t * (-2.5 + 1.5 * t);
    vec2 w2 = t * (0.5 + t * (2.0 - 1.5 * t));
    vec2 w3 = t * t * (-0.5 + 0.5 * t);
    vec2 wx[4] = vec2[4](vec2(w0.x, 0.0), vec2(w1.x, 0.0), vec2(w2.x, 0.0), vec2(w3.x, 0.0));
    float ky[4] = float[4](w0.y, w1.y, w2.y, w3.y);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        int yy = clamp(int(i.y) + y - 1, 0, size.y - 1);
        for (int x = 0; x < 4; x++) {
            int xx = clamp(int(i.x) + x - 1, 0, size.x - 1);
            sum += wx[x].x * ky[y] * texelFetch(cached, ivec2(xx, yy), 0);
        }
    }
    return sum;
}
)GLSL";

		const char* const WARP_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec2 p = vec2(q) + vec2(0.5);
    vec2 v;
    vec4 o;
    if (shift != 0.0 && field_vector(rw_source(p), v)) {
        // The source displacement in output pixels: the map's local slope
        // per axis (the maps are separable).
        vec2 slope = max(rw_source(p + vec2(0.5)) - rw_source(p - vec2(0.5)), vec2(1e-4));
        o = rw_cubic(p - shift * strength_scale() * v / slope);
    } else {
        o = texelFetch(cached, q, 0);   // hard hold of A
    }
    imageStore(dst, q, o);
}
)GLSL";

		pl_fmt StorageFormat(pl_gpu gpu)
		{
			return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_STORABLE | PL_FMT_CAP_SAMPLEABLE));
		}

		bool Ensure(pl_gpu gpu, pl_tex& tex, int w, int h, bool hostReadable = false)
		{
			if (tex && tex->params.w == w && tex->params.h == h)
				return true;
			pl_fmt fmt = StorageFormat(gpu);
			if (!fmt)
				return false;
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = hostReadable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
			return pl_tex_recreate(gpu, &tex, &params);
		}

		pl_shader_desc Sampled(const char* name, pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_SAMPLED_TEX;
			desc.binding.object = tex;
			desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			return desc;
		}

		pl_shader_desc Storage(pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = "dst";
			desc.desc.type = PL_DESC_STORAGE_IMG;
			desc.desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc.binding.object = tex;
			return desc;
		}

		bool Compute(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
			const pl_shader_desc* descriptors, int numDescriptors, const pl_shader_var* variables,
			int numVariables, int w, int h)
		{
			pl_custom_shader custom{};
			custom.description = description;
			custom.header = header.empty() ? nullptr : header.c_str();
			custom.body = body;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = descriptors;
			custom.num_descriptors = numDescriptors;
			custom.variables = variables;
			custom.num_variables = numVariables;
			custom.compute = true;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (w + 15) / 16;
			compute.dispatch_size[1] = (h + 15) / 16;
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
		}
	}

	Cache::Cache()
	{
		hook.stages = PL_HOOK_PRE_OUTPUT;
		hook.input = PL_HOOK_SIG_TEX;
		hook.priv = this;
		hook.hook = &Cache::Run;
		hook.signature = 0x5650444A43414348ull;   // "VPDJCACH"
	}

	Cache::~Cache()
	{
		Release();
	}

	void Cache::Release()
	{
		if (gpu && tex)
			pl_tex_destroy(gpu, &tex);
		tex = nullptr;
		gpu = nullptr;
		armed = 0;
		valid = 0;
	}

	Rects Rounded(const pl_rect2df& source, const pl_rect2df& target, float framebufferW, float framebufferH)
	{
		Rects r;
		const float w = framebufferW > 0.0f ? framebufferW : 1e9f;
		const float h = framebufferH > 0.0f ? framebufferH : 1e9f;
		const float x0 = std::fmin(target.x0, target.x1), x1 = std::fmax(target.x0, target.x1);
		const float y0 = std::fmin(target.y0, target.y1), y1 = std::fmax(target.y0, target.y1);
		const float rx0 = std::roundf(std::fmin(std::fmax(x0, 0.0f), w));
		const float ry0 = std::roundf(std::fmin(std::fmax(y0, 0.0f), h));
		const float rx1 = std::roundf(std::fmin(std::fmax(x1, 0.0f), w));
		const float ry1 = std::roundf(std::fmin(std::fmax(y1, 0.0f), h));
		const float scaleX = x1 > x0 ? (source.x1 - source.x0) / (x1 - x0) : 1.0f;
		const float scaleY = y1 > y0 ? (source.y1 - source.y0) / (y1 - y0) : 1.0f;
		r.source = { source.x0 + (rx0 - x0) * scaleX, source.y0 + (ry0 - y0) * scaleY,
			source.x0 + (rx1 - x0) * scaleX, source.y0 + (ry1 - y0) * scaleY };
		r.target = { rx0, ry0, rx1, ry1 };
		return r;
	}

	pl_hook_res Cache::Run(void* priv, const pl_hook_params* params)
	{
		return static_cast<Cache*>(priv)->Apply(params);
	}

	pl_hook_res Cache::Apply(const pl_hook_params* params)
	{
		pl_hook_res none{};
		none.output = PL_HOOK_SIG_NONE;
		if (armed == 0 || !params->tex)
			return none;
		if (gpu && gpu != params->gpu && tex)
			pl_tex_destroy(gpu, &tex);
		gpu = params->gpu;
		const int w = params->tex->params.w, h = params->tex->params.h;
		if (!Ensure(gpu, tex, w, h))
		{
			++failed;
			armed = 0;
			return none;
		}
		const pl_shader_desc all[2] = { Sampled("src", params->tex), Storage(tex) };
		if (!Compute(params->dispatch, "VideoProcessor de-judder: cache A at PRE_OUTPUT", std::string(),
			COPY_BODY, all, 2, nullptr, 0, w, h))
		{
			++failed;
			armed = 0;
			return none;
		}
		repr = params->repr;
		color = params->color;
		valid = armed;
		armed = 0;
		++copies;
		// The picture goes on unchanged (SIG_NONE: handing a pooled texture
		// back would count as resizing a non-resizable stage).
		return none;
	}

	Warp::Warp(pl_log log, pl_gpu gpuIn, bool hostReadableIn)
		: gpu(gpuIn), hostReadable(hostReadableIn)
	{
		dispatch = pl_dispatch_create(log, gpu);
	}

	Warp::~Warp()
	{
		if (warped)
			pl_tex_destroy(gpu, &warped);
		if (dispatch)
			pl_dispatch_destroy(&dispatch);
	}

	bool Warp::Run(pl_tex cached, pl_tex field, pl_tex frameTrust, float minTrustedShare,
		float shift, const Geometry& g, pl_tex& out)
	{
		out = nullptr;
		if (!dispatch || !cached || !field || !frameTrust)
		{
			error = "no cached picture or motion field";
			return false;
		}
		const int w = cached->params.w, h = cached->params.h;
		if (!Ensure(gpu, warped, w, h, hostReadable))
		{
			error = "no storable rgba16f texture";
			return false;
		}
		// The cache is either the destination rectangle itself or the whole
		// target with the picture at the destination rectangle.
		const float targetW = std::fabs(g.targetCrop.x1 - g.targetCrop.x0);
		const float targetH = std::fabs(g.targetCrop.y1 - g.targetCrop.y0);
		const bool cropSized = std::fabs(targetW - static_cast<float>(w)) < 1.0f &&
			std::fabs(targetH - static_cast<float>(h)) < 1.0f;
		const float dstOrigin[2] = { cropSized ? 0.0f : g.targetCrop.x0, cropSized ? 0.0f : g.targetCrop.y0 };
		const float srcOrigin[2] = { g.sourceCrop.x0, g.sourceCrop.y0 };
		const float srcPerDst[2] = {
			targetW > 0.0f ? (g.sourceCrop.x1 - g.sourceCrop.x0) / targetW : 1.0f,
			targetH > 0.0f ? (g.sourceCrop.y1 - g.sourceCrop.y0) / targetH : 1.0f };
		const float nlsActive = g.nls.valid ? 1.0f : 0.0f;
		const float nlsCurve = static_cast<float>(g.nls.curve);
		const float nlsGeometry = static_cast<float>(g.nls.geometry);
		const float nlsScale[2] = { static_cast<float>(g.nls.xScale), static_cast<float>(g.nls.yScale) };
		const float nlsProtect[2] = { static_cast<float>(g.nls.xProtect), static_cast<float>(g.nls.yProtect) };
		const float domain[4] = { g.nlsDomain.x0, g.nlsDomain.y0,
			std::fabs(g.nlsDomain.x1 - g.nlsDomain.x0) > 0.0f ? g.nlsDomain.x1 - g.nlsDomain.x0 : 1.0f,
			std::fabs(g.nlsDomain.y1 - g.nlsDomain.y0) > 0.0f ? g.nlsDomain.y1 - g.nlsDomain.y0 : 1.0f };
		const pl_shader_var variables[11] = {
			{ pl_var_vec2("dst_origin"), dstOrigin, true },
			{ pl_var_vec2("src_origin"), srcOrigin, true },
			{ pl_var_vec2("src_per_dst"), srcPerDst, true },
			{ pl_var_float("nls_active"), &nlsActive, true },
			{ pl_var_float("nls_curve"), &nlsCurve, true },
			{ pl_var_float("nls_geometry"), &nlsGeometry, true },
			{ pl_var_vec2("nls_scale"), nlsScale, true },
			{ pl_var_vec2("nls_protect"), nlsProtect, true },
			{ pl_var_vec4("nls_domain"), domain, true },
			{ pl_var_float("shift"), &shift, true },
			{ pl_var_float("min_trusted_share"), &minTrustedShare, true } };
		const pl_shader_desc descriptors[4] = {
			Sampled("cached", cached), Sampled("field", field), Sampled("frame_trust", frameTrust),
			Storage(warped) };
		const std::string header = std::string(OcclusionMask::NlsMapGlsl()) + Dejudder::FieldVectorGlsl() +
			WARP_FUNCTIONS;
		if (!Compute(dispatch, "VideoProcessor de-judder: warp the rendered picture (6d)", header, WARP_BODY,
			descriptors, 4, variables, 11, w, h))
		{
			error = "rendered warp rejected or failed";
			return false;
		}
		out = warped;
		return true;
	}

	pl_frame Frame(pl_tex tex, const pl_color_repr& repr, const pl_color_space& color)
	{
		pl_frame frame{};
		frame.num_planes = 1;
		frame.planes[0].texture = tex;
		frame.planes[0].components = 3;
		for (int i = 0; i < 3; ++i)
			frame.planes[0].component_mapping[i] = i;
		frame.repr = repr;
		frame.repr.alpha = PL_ALPHA_NONE;
		frame.repr.bits = pl_bit_encoding{};
		frame.color = color;
		frame.crop.x1 = static_cast<float>(tex ? tex->params.w : 0);
		frame.crop.y1 = static_cast<float>(tex ? tex->params.h : 0);
		return frame;
	}

	pl_render_params OutputOnly(const pl_render_params& a, const std::vector<const pl_hook*>& hooks,
		std::vector<const pl_hook*>& kept)
	{
		kept.clear();
		for (const pl_hook* hook : hooks)
			if (hook && hook->stages != 0 && (hook->stages & ~static_cast<unsigned>(PL_HOOK_OUTPUT)) == 0)
				kept.push_back(hook);
		pl_render_params p = a;
		p.hooks = kept.empty() ? nullptr : kept.data();
		p.num_hooks = static_cast<int>(kept.size());
		p.deband_params = nullptr;
		p.color_adjustment = nullptr;
		p.peak_detect_params = nullptr;
		p.cone_params = nullptr;
		p.distort_params = nullptr;
		p.lut = nullptr;
		p.lut_type = PL_LUT_UNKNOWN;
		return p;
	}
}
