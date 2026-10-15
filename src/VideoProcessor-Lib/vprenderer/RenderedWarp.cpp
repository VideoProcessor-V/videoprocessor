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
#define RW_SOURCE_GLSL R"GLSL(
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
// The output displacement whose source moves by d (source pixels): the
// sample is at p - rw_moved(...). Without NLS the map is linear and the local
// slope is exact (d / slope, as before). With NLS (owner 2026-10-14) two
// Newton steps on the separable map make it exact near the sides, where the
// slope changes across a large displacement.
vec2 rw_moved(vec2 p, vec2 slope, vec2 d)
{
    vec2 q = p - d / slope;
    if (nls_active > 0.5) {
        vec2 target = rw_source(p) - d;
        vec2 q0 = q;
        for (int k = 0; k < 2; k++) {
            vec2 sl = max(rw_source(q + vec2(0.5)) - rw_source(q - vec2(0.5)), vec2(1e-4));
            q -= (rw_source(q) - target) / sl;
        }
        if (any(isnan(q)) || any(isinf(q)) || any(greaterThan(abs(q - q0), abs(d / slope) + vec2(2.0))))
            q = q0;
        return p - q;
    }
    return d / slope;
}
)GLSL"
		const char* const WARP_FUNCTIONS_SOURCE_ONLY = RW_SOURCE_GLSL;
		// The held rectangles (Geometry::holds), source pixels (x0, y0, x1, y1).
		const char* const HOLDS_GLSL = R"GLSL(
#define HOLDS 1
bool rw_in(vec2 s, vec4 r) { return r.x < r.z && s.x >= r.x && s.y >= r.y && s.x < r.z && s.y < r.w; }
bool rw_held(vec2 s) { return rw_in(s, hold0) || rw_in(s, hold1) || rw_in(s, hold2) || rw_in(s, hold3); }
)GLSL";
		const char* const WARP_FUNCTIONS = RW_SOURCE_GLSL R"GLSL(
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
    // Planner D2: the displacement this pixel's content was moved by (output
    // pixels from A), for the sigma-delta's per-pixel carry; zero = held.
    vec2 moved = vec2(0.0);
#ifdef WRITE_DISP
#define STORE_DISP() imageStore(disp, q, vec4(moved * step_fraction, any(notEqual(moved, vec2(0.0))) ? 1.0 : 0.0, 0.0))
#else
#define STORE_DISP()
#endif
#ifdef PIXEL_TRUST
    // M6: the vector where the pixel comes from (two fixed-point steps),
    // blended with the hold by A's pixel weight there; a cut holds.
    if (shift != 0.0 && texelFetch(frame_trust, ivec2(0), 0).r >= min_trusted_share) {
        vec2 slope = max(rw_source(p + vec2(0.5)) - rw_source(p - vec2(0.5)), vec2(1e-4));
        vec2 sp = rw_source(p);
        float sc = shift * strength_scale();
        vec2 va = field_vector_all(sp);
        va = field_vector_all(sp - sc * va);
        va = field_vector_all(sp - sc * va);
#ifdef OBJECT
        va = object_scale(va);   // delta 14 R-B (audit 2026-10-14)
#endif
        vec2 d = sc * va;
#ifdef HOLDS
        if (rw_held(sp) || rw_held(sp - d))
            o = texelFetch(cached, q, 0);
        else
#endif
        {
            float m = texture(pix_mask, (sp - d) / vec2(textureSize(pix_mask, 0) * 2)).r;
#ifdef LAND_A
            {
                // The landing check (as the source warp): the pixel taken
                // must be carried back here by its own motion.
                vec2 vl = field_vector_all(sp - d);
#ifdef OBJECT
                vl = object_scale(vl);
#endif
                if (length(sc * vl - d) > LAND_A) m = 0.0;
            }
#endif
#ifdef PIXEL_MOTION
            moved = rw_moved(p, slope, m * d);
            o = rw_cubic(p - moved);   // the weight's share of the motion
#else
            vec2 dm_ = rw_moved(p, slope, d);
            o = mix(texelFetch(cached, q, 0), rw_cubic(p - dm_), m);
            moved = m > 0.5 ? dm_ : vec2(0.0);   // a crossfade: the dominant part
#endif
        }
    } else
        o = texelFetch(cached, q, 0);
    imageStore(dst, q, o);
    STORE_DISP();
    return;
#endif
    if (shift != 0.0 && field_vector(rw_source(p), v)) {
        // The source displacement in output pixels: the map's local slope
        // per axis (the maps are separable).
        vec2 slope = max(rw_source(p + vec2(0.5)) - rw_source(p - vec2(0.5)), vec2(1e-4));
        vec2 sp = rw_source(p);
        vec2 d = shift * strength_scale() * block_strength(sp) * v;
#ifdef HOLDS
        // A moved subtitle (and its fill) stays still, and nothing around it
        // samples from it.
        if (rw_held(sp) || rw_held(sp - d)) {
            o = texelFetch(cached, q, 0);
        } else
#endif
        {
            moved = rw_moved(p, slope, d);
            o = rw_cubic(p - moved);
#ifdef FEATHER
            // Delta 9 H1: the feather is in output pixels, the field in source.
            float hw = hold_weight(sp, feather_px * max(slope.x, slope.y));
            o = mix(texelFetch(cached, q, 0), o, hw);
            moved = hw > 0.5 ? moved : vec2(0.0);   // a crossfade: the dominant part
#endif
        }
    } else {
        o = texelFetch(cached, q, 0);   // hard hold of A
    }
    imageStore(dst, q, o);
    STORE_DISP();
}
)GLSL";

		// Delta 8 R1: one invocation per 16x16 output block (VectorField).
		const char* const FIELD_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (b.x < size.x && b.y < size.y) {
    vec2 p = vec2(b * 16) + vec2(8.0);
    vec4 o = vec4(0.0, 0.0, 1.0, 0.0);   // held or outside the picture: no motion
    if (all(greaterThanEqual(p, crop_min)) && all(lessThan(p, crop_max))) {
        vec2 v;
        bool held = false;
#ifdef HOLDS
        held = rw_held(rw_source(p));
#endif
#ifdef TO_NEXT
        // B's first present: B is at the full vector, the last picture of A
        // was at shift x the strength scale.
        if (!held && field_vector(rw_source(p), v)) {
            vec2 slope = max(rw_source(p + vec2(0.5)) - rw_source(p - vec2(0.5)), vec2(1e-4));
            o.xy = rw_moved(p, slope, (1.0 - shift * strength_scale() * block_strength(rw_source(p))) * v);
        }
#else
        if (!held && shift != 0.0 && field_vector(rw_source(p), v)) {
            vec2 slope = max(rw_source(p + vec2(0.5)) - rw_source(p - vec2(0.5)), vec2(1e-4));
            o.xy = rw_moved(p, slope, shift * strength_scale() * block_strength(rw_source(p)) * v);
        }
#endif
    }
    if (any(isnan(o.xy)) || any(isinf(o.xy)))
        o = vec4(0.0);
    imageStore(dst, b, o);
}
)GLSL";

		// picture: the cached rendered picture and the warped extra built from
		// it, rgba32f (2026-10-15: rgba16f resolved ~1/2048 near white, about 11
		// bits; rgba16f where 32-bit is not storable). The motion field and the
		// displacement stay rgba16f.
		pl_fmt StorageFormat(pl_gpu gpu, bool picture = false)
		{
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_STORABLE | PL_FMT_CAP_SAMPLEABLE);
			if (picture)
				if (pl_fmt f32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(caps | PL_FMT_CAP_LINEAR)))
					return f32;
			return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16, caps);
		}

		bool Ensure(pl_gpu gpu, pl_tex& tex, int w, int h, bool hostReadable = false, bool picture = false)
		{
			pl_fmt fmt = StorageFormat(gpu, picture);
			if (tex && tex->params.w == w && tex->params.h == h && tex->params.format == fmt)
				return true;
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
		if (!Ensure(gpu, tex, w, h, false, true))
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
		if (displacement)
			pl_tex_destroy(gpu, &displacement);   // D2 (audit 2026-10-14b: leaked)
		if (dispatch)
			pl_dispatch_destroy(&dispatch);
	}

	bool Warp::Run(pl_tex cached, pl_tex field, pl_tex frameTrust, float minTrustedShare,
		float shift, const Geometry& g, pl_tex& out, pl_tex subField, const Dejudder::Settings* matcher, pl_tex camera,
		pl_tex pixelMask, pl_tex vectorOk, pl_tex* displacementOut, float stepFraction)
	{
		out = nullptr;
		if (!dispatch || !cached || !field || !frameTrust)
		{
			error = "no cached picture or motion field";
			return false;
		}
		const int w = cached->params.w, h = cached->params.h;
		if (!Ensure(gpu, warped, w, h, hostReadable, true))
		{
			error = "no storable rgba32f texture";
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
		const Dejudder::Settings none;
		const Dejudder::Settings& ms = matcher ? *matcher : none;
		const float featherPx = ms.holdFeatherPx;
		const float taperTol = ms.matchTolerance;
		const float taperSad = ms.taperSad > 0.0f ? ms.taperSad : 2.0f * ms.matchTolerance;
		float holds[4][4] = {};
		for (int i = 0; i < 4 && i < g.numHolds; ++i)
		{
			const pl_rect2df& r = g.holds[i];
			holds[i][0] = std::min(r.x0, r.x1);
			holds[i][1] = std::min(r.y0, r.y1);
			holds[i][2] = std::max(r.x0, r.x1);
			holds[i][3] = std::max(r.y0, r.y1);
		}
		const pl_shader_var variables[14] = {
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
			{ pl_var_float("min_trusted_share"), &minTrustedShare, true },
			{ pl_var_float("feather_px"), &featherPx, true },
			{ pl_var_float("taper_tol"), &taperTol, true },
			{ pl_var_float("taper_sad"), &taperSad, true } };
		// The variables in use, in order (feather, then taper).
		pl_shader_var used[24];   // 11 + feather 1 + taper 2 + holds 4 + object 1 + step 1
		int n = 0;
		for (int i = 0; i < 11; ++i) used[n++] = variables[i];
		if (ms.holdFeatherPx > 0.0f) used[n++] = variables[11];
		if (ms.blockTaper) { used[n++] = variables[12]; used[n++] = variables[13]; }
		if (g.numHolds > 0)
		{
			used[n++] = { pl_var_vec4("hold0"), holds[0], true };
			used[n++] = { pl_var_vec4("hold1"), holds[1], true };
			used[n++] = { pl_var_vec4("hold2"), holds[2], true };
			used[n++] = { pl_var_vec4("hold3"), holds[3], true };
		}
		// Delta 14 R-B: object_strength, and the camera when given.
		const float objectStrength = ms.objectStrength;
		const bool object = ms.objectStrength < 1.0f;
		if (object) used[n++] = { pl_var_float("object_strength"), &objectStrength, true };
		std::vector<pl_shader_desc> descriptors = {
			Sampled("cached", cached), Sampled("field", field), Sampled("frame_trust", frameTrust), Storage(warped) };
		if (subField) descriptors.push_back(Sampled("sub_field", subField));
		if (object && camera) descriptors.push_back(Sampled("camera", camera));
		// M6: the pixel weights (bilinear).
		const bool pixel = ms.pixelTrust && pixelMask;
		if (pixel)
		{
			pl_shader_desc mask = Sampled("pix_mask", pixelMask);
			mask.binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
			descriptors.push_back(mask);
			if (vectorOk)
				descriptors.push_back(Sampled("vector_ok", vectorOk));
		}
		// Planner D2: the per-pixel displacement (rgba16f, the cache's size).
		const bool writeDisp = displacementOut != nullptr;
		if (writeDisp)
		{
			*displacementOut = nullptr;
			if (!Ensure(gpu, displacement, w, h, hostReadable))
			{
				error = "no storable rgba16f texture for the displacement";
				return false;
			}
			pl_shader_desc d{};
			d.desc.name = "disp";
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = PL_DESC_ACCESS_WRITEONLY;
			d.binding.object = displacement;
			descriptors.push_back(d);
			used[n++] = { pl_var_float("step_fraction"), &stepFraction, true };
		}
		const std::string header = std::string(pixel ? "#define PIXEL_TRUST\n" : "") + (writeDisp ? "#define WRITE_DISP 1\n" : "") +
			(pixel && ms.pixelTrustedVectors ? "#define PIXEL_TRUSTED_VECTORS 1\n" : "") +
			(pixel && vectorOk ? "#define PIXEL_OWN_VECTORS 1\n" : "") +
			(pixel && ms.pixelTrustMotion ? "#define PIXEL_MOTION 1\n" : "") +
			(pixel && ms.landingCheck > 0.0f ? "#define LAND_A " + std::to_string(ms.landingCheck) + "\n" : std::string()) +
			OcclusionMask::NlsMapGlsl() +
			Dejudder::WarpHeader(ms, subField != nullptr, object && camera) + (g.numHolds > 0 ? HOLDS_GLSL : "") +
			(pixel ? Dejudder::FieldAllGlsl() : "") + WARP_FUNCTIONS;
		if (!Compute(dispatch, "VideoProcessor de-judder: warp the rendered picture (6d)", header, WARP_BODY,
			descriptors.data(), static_cast<int>(descriptors.size()), used, n, w, h))
		{
			error = "rendered warp rejected or failed";
			return false;
		}
		out = warped;
		if (writeDisp)
			*displacementOut = displacement;
		return true;
	}

	VectorField::VectorField(pl_log log, pl_gpu gpuIn, bool hostReadableIn)
		: gpu(gpuIn), hostReadable(hostReadableIn)
	{
		dispatch = pl_dispatch_create(log, gpu);
	}

	VectorField::~VectorField()
	{
		if (vectors)
			pl_tex_destroy(gpu, &vectors);
		if (dispatch)
			pl_dispatch_destroy(&dispatch);
	}

	bool VectorField::Run(pl_tex field, pl_tex frameTrust, float minTrustedShare, float shiftStep, const Geometry& g,
		int w, int h, pl_tex& out, pl_tex subField, const Dejudder::Settings* matcher, pl_tex camera, bool toNext)
	{
		out = nullptr;
		if (!dispatch || !field || !frameTrust || w <= 0 || h <= 0)
		{
			error = "no motion field";
			return false;
		}
		const int bw = Blocks(w), bh = Blocks(h);
		if (!Ensure(gpu, vectors, bw, bh, hostReadable))
		{
			error = "no storable rgba16f texture";
			return false;
		}
		// The OUTPUT picture is the cache's size: the destination rectangle
		// itself or the whole target (as in Warp::Run).
		const float targetW = std::fabs(g.targetCrop.x1 - g.targetCrop.x0);
		const float targetH = std::fabs(g.targetCrop.y1 - g.targetCrop.y0);
		const bool cropSized = std::fabs(targetW - static_cast<float>(w)) < 1.0f &&
			std::fabs(targetH - static_cast<float>(h)) < 1.0f;
		const float dstOrigin[2] = { cropSized ? 0.0f : g.targetCrop.x0, cropSized ? 0.0f : g.targetCrop.y0 };
		const float cropMin[2] = { dstOrigin[0], dstOrigin[1] };
		const float cropMax[2] = { dstOrigin[0] + targetW, dstOrigin[1] + targetH };
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
		const Dejudder::Settings none;
		const Dejudder::Settings& ms = matcher ? *matcher : none;
		const float taperTol = ms.matchTolerance;
		const float taperSad = ms.taperSad > 0.0f ? ms.taperSad : 2.0f * ms.matchTolerance;
		// The feather is the warp's blend toward A; the field uses each
		// block's own vector (the per-pixel gate checks the rest), so it
		// is left out (no FEATHER define).
		Dejudder::Settings fieldSettings = ms;
		fieldSettings.holdFeatherPx = 0.0f;
		float holds[4][4] = {};
		for (int i = 0; i < 4 && i < g.numHolds; ++i)
		{
			const pl_rect2df& r = g.holds[i];
			holds[i][0] = std::min(r.x0, r.x1);
			holds[i][1] = std::min(r.y0, r.y1);
			holds[i][2] = std::max(r.x0, r.x1);
			holds[i][3] = std::max(r.y0, r.y1);
		}
		pl_shader_var used[20];
		int n = 0;
		used[n++] = { pl_var_vec2("dst_origin"), dstOrigin, true };
		used[n++] = { pl_var_vec2("src_origin"), srcOrigin, true };
		used[n++] = { pl_var_vec2("src_per_dst"), srcPerDst, true };
		used[n++] = { pl_var_float("nls_active"), &nlsActive, true };
		used[n++] = { pl_var_float("nls_curve"), &nlsCurve, true };
		used[n++] = { pl_var_float("nls_geometry"), &nlsGeometry, true };
		used[n++] = { pl_var_vec2("nls_scale"), nlsScale, true };
		used[n++] = { pl_var_vec2("nls_protect"), nlsProtect, true };
		used[n++] = { pl_var_vec4("nls_domain"), domain, true };
		used[n++] = { pl_var_float("shift"), &shiftStep, true };
		used[n++] = { pl_var_float("min_trusted_share"), &minTrustedShare, true };
		used[n++] = { pl_var_vec2("crop_min"), cropMin, true };
		used[n++] = { pl_var_vec2("crop_max"), cropMax, true };
		if (fieldSettings.blockTaper)
		{
			used[n++] = { pl_var_float("taper_tol"), &taperTol, true };
			used[n++] = { pl_var_float("taper_sad"), &taperSad, true };
		}
		if (g.numHolds > 0)
		{
			used[n++] = { pl_var_vec4("hold0"), holds[0], true };
			used[n++] = { pl_var_vec4("hold1"), holds[1], true };
			used[n++] = { pl_var_vec4("hold2"), holds[2], true };
			used[n++] = { pl_var_vec4("hold3"), holds[3], true };
		}
		// Delta 14 R-B: the extra's picture moved by the split vector, so its
		// motion for the sigma-delta is the split vector too.
		const float objectStrength = fieldSettings.objectStrength;
		const bool object = fieldSettings.objectStrength < 1.0f;
		if (object) used[n++] = { pl_var_float("object_strength"), &objectStrength, true };
		std::vector<pl_shader_desc> descriptors = {
			Sampled("field", field), Sampled("frame_trust", frameTrust), Storage(vectors) };
		if (subField) descriptors.push_back(Sampled("sub_field", subField));
		if (object && camera) descriptors.push_back(Sampled("camera", camera));
		const std::string header = std::string(toNext ? "#define TO_NEXT 1\n" : "") + OcclusionMask::NlsMapGlsl() +
			Dejudder::WarpHeader(fieldSettings, subField != nullptr, object && camera) + (g.numHolds > 0 ? HOLDS_GLSL : "") +
			WARP_FUNCTIONS_SOURCE_ONLY;
		if (!Compute(dispatch, toNext ? "VideoProcessor sigma-delta vectors to the next frame (de-judder field)" :
			"VideoProcessor sigma-delta vectors from the de-judder field (delta 8 R1)", header,
			FIELD_BODY, descriptors.data(), static_cast<int>(descriptors.size()), used, n, bw, bh))
		{
			error = "vector field rejected or failed";
			return false;
		}
		out = vectors;
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
