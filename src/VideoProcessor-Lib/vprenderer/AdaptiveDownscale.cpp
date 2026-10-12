#include <pch.h>
#include "AdaptiveDownscale.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>

#include <vprenderer/OcclusionMaskHook.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AdaptiveDownscale
{
	namespace
	{
		// Luma in 8-bit codes of the encoded picture (a linear PREKERNEL is
		// re-encoded with 1/2.2 so the code thresholds mean the same in both
		// light domains), edges, and per-workgroup sums for the grain term.
		const char* const MEASURE_HEADER = R"GLSL(
shared vec4 ad_sum[256];
float ad_luma(ivec2 p)
{
    ivec2 sz = textureSize(src, 0);
    vec3 c = texelFetch(src, clamp(p, ivec2(0), sz - ivec2(1)), 0).rgb;
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    if (linear_in > 0.5)
        y = sign(y) * pow(abs(y), 1.0 / 2.2);
    return y * 255.0;
}
)GLSL";

		const char* const MEASURE_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
uint li = gl_LocalInvocationIndex;
vec4 v = vec4(0.0);
if (q.x < int(region.x) && q.y < int(region.y)) {
    ivec2 p = ivec2(origin) + q;
    float a = ad_luma(p + ivec2(-1, -1)), b = ad_luma(p + ivec2(0, -1)), c = ad_luma(p + ivec2(1, -1));
    float d = ad_luma(p + ivec2(-1, 0)), e = ad_luma(p), f = ad_luma(p + ivec2(1, 0));
    float g = ad_luma(p + ivec2(-1, 1)), h = ad_luma(p + ivec2(0, 1)), k = ad_luma(p + ivec2(1, 1));
    float gx = (c + 2.0 * f + k) - (a + 2.0 * d + g);
    float gy = (g + 2.0 * h + k) - (a + 2.0 * b + c);
    float grad = length(vec2(gx, gy)) * 0.25;
    float ed = smoothstep(edge_lo, edge_hi, grad);
    imageStore(edge_out, q, vec4(ed));
    float lap = abs(4.0 * e - b - d - f - h);
    float flat_px = grad < edge_lo ? 1.0 : 0.0;
    float wgt = max(mix(1.0, 2.0 * (1.0 - clamp(e / 255.0, 0.0, 1.0)), shadow_bias), 0.0);
    v = vec4(lap * wgt * flat_px, flat_px, 1.0 - ed, e);
}
ad_sum[li] = v;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        ad_sum[li] += ad_sum[li + s];
    barrier();
}
if (li == 0u)
    imageStore(partial_out, ivec2(gl_WorkGroupID.xy), ad_sum[0]);
)GLSL";

		// One workgroup: the frame's totals, the ramp, the smoothing and the
		// cut snap, into the persistent state (r grain_scale, g mean(1 - edge),
		// b mean luma, a initialised).
		const char* const FINAL_HEADER = R"GLSL(
shared vec4 ad_total[256];
)GLSL";

		const char* const FINAL_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 n = textureSize(partial_in, 0);
vec4 acc = vec4(0.0);
for (int idx = int(li); idx < n.x * n.y; idx += 256)
    acc += texelFetch(partial_in, ivec2(idx % n.x, idx / n.x), 0);
ad_total[li] = acc;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        ad_total[li] += ad_total[li + s];
    barrier();
}
if (li == 0u && advance > 0.5) {
    vec4 t = ad_total[0];
    float energy = t.x / max(t.y, 1.0);
    float free_mean = t.z / max(total_pixels, 1.0);
    float luma_mean = t.w / max(total_pixels, 1.0);
    float target = mix(grain_floor, 1.0, 1.0 - smoothstep(grain_lo, grain_hi, energy));
    vec4 prev = imageLoad(state_io, ivec2(0));
    bool cut = prev.a < 0.5 || abs(luma_mean - prev.b) > 20.0;
    float scale = cut ? target : mix(prev.r, target, smooth_alpha);
    // a: 1 initialised, 2 = a cut on this frame (the grain map resets too).
    imageStore(state_io, ivec2(0), vec4(scale, free_mean, luma_mean, cut ? 2.0 : 1.0));
    if (write_stats > 0.5)
        ad_out = vec4(scale, free_mean, luma_mean, 1.0);
}
)GLSL";

		// 8-3: the per-block map, once per source frame. Each block of A takes
		// the ramp of its temporal error (shadow-weighted like the scalar) and
		// smooths it with the value the previous map held where this content
		// was (the block centre minus its vector); untrusted blocks keep their
		// value and are marked untrusted; a cut (the scalar's, or too few
		// trusted blocks) starts over.
		const char* const MAP_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(map_out);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = texelFetch(field, b, 0);
    vec4 ge = texelFetch(grain_error, b, 0);
    bool frame_ok = texelFetch(frame_trust, ivec2(0), 0).r >= min_trusted_share;
    bool reset = texelFetch(state, ivec2(0), 0).a > 1.5 || first_map > 0.5;
    bool trusted = frame_ok && f.z > 0.5;
    vec2 c = (vec2(b) + vec2(0.5)) * 16.0;
    vec2 pc = trusted ? c - f.xy : c;
    ivec2 pb = clamp(ivec2(floor(pc / 16.0)), ivec2(0), blocks - ivec2(1));
    vec4 prev = texelFetch(map_in, pb, 0);
    float wgt = max(mix(1.0, 2.0 * (1.0 - clamp(ge.g / 255.0, 0.0, 1.0)), shadow_bias), 0.0);
    float energy = ge.r * temporal_to_spatial * wgt;
    float target = mix(grain_floor, 1.0, 1.0 - smoothstep(grain_lo, grain_hi, energy));
    vec4 o;
    if (!trusted)
        o = vec4(reset ? 1.0 : texelFetch(map_in, b, 0).r, 0.0, 0.0, 1.0);
    else if (reset || prev.a < 0.5)
        o = vec4(target, 1.0, 0.0, 1.0);
    else
        o = vec4(mix(prev.r, target, smooth_alpha), 1.0, 0.0, 1.0);
    imageStore(map_out, b, o);
}
)GLSL";

		// The blend. With the map (AD_USE_MAP), the grain term at the source
		// position the output pixel shows: bilinear between the four nearest
		// block centres, an untrusted block contributing the scalar.
		const char* const BLEND_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec4 fa = texelFetch(faithful, q, 0);
    vec4 ss = texelFetch(hooked, q, 0);
    vec2 s = src_origin + (vec2(q) + vec2(0.5)) * src_per_dst;
    float e = texture(edge, (s - edge_origin) / vec2(textureSize(edge, 0))).r;
    float g = texelFetch(state, ivec2(0), 0).r;
#ifdef AD_USE_MAP
    {
        vec2 u = (s - src_origin) / src_size;   // 0..1 over the source crop
        if (nls_active > 0.5)
            u = clamp(vec2(nls_coordinate(u.x, nls_scale.x, nls_protect.x),
                           nls_coordinate(u.y, nls_scale.y, nls_protect.y)), vec2(0.0), vec2(1.0));
        vec2 sp = crop.xy + u * crop.zw;          // source pixels of the whole frame
        ivec2 msize = textureSize(grain_map, 0);
        vec2 gpos = sp / 16.0 - vec2(0.5);
        ivec2 g0 = ivec2(floor(gpos));
        vec2 t = gpos - vec2(g0);
        float acc = 0.0;
        for (int y = 0; y <= 1; y++)
            for (int x = 0; x <= 1; x++) {
                vec4 m = texelFetch(grain_map, clamp(g0 + ivec2(x, y), ivec2(0), msize - ivec2(1)), 0);
                float wt = (x == 1 ? t.x : 1.0 - t.x) * (y == 1 ? t.y : 1.0 - t.y);
                acc += wt * (m.g > 0.5 ? m.r : g);
            }
        g = acc;
    }
#endif
    float w = clamp(w_base * (1.0 - e) * g, 0.0, 1.0);
    vec4 o;
    if (chroma_ssim > 0.5) {
        o = mix(fa, ss, w);
    } else {
        const vec3 k = vec3(0.2126, 0.7152, 0.0722);
        o.rgb = fa.rgb + vec3(w * (dot(ss.rgb, k) - dot(fa.rgb, k)));
        o.a = fa.a;   // the carry's offset (VP re-injects its own at SCALED anyway)
    }
    imageStore(dst, q, o);
}
)GLSL";

		pl_shader_desc Sampled(const char* name, pl_tex tex, bool linear)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_SAMPLED_TEX;
			desc.binding.object = tex;
			desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
			return desc;
		}

		pl_shader_desc Storage(const char* name, pl_tex tex, enum pl_desc_access access = PL_DESC_ACCESS_WRITEONLY)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_STORAGE_IMG;
			desc.desc.access = access;
			desc.binding.object = tex;
			return desc;
		}

		bool Ensure(pl_gpu gpu, pl_tex& tex, int w, int h, int components, int depth, bool renderable,
			bool linear, bool hostReadable = false)
		{
			if (tex && tex->params.w == w && tex->params.h == h)
				return true;
			enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			if (renderable) caps = static_cast<enum pl_fmt_caps>(caps | PL_FMT_CAP_RENDERABLE);
			if (linear) caps = static_cast<enum pl_fmt_caps>(caps | PL_FMT_CAP_LINEAR);
			if (hostReadable) caps = static_cast<enum pl_fmt_caps>(caps | PL_FMT_CAP_HOST_READABLE);
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, components, depth, depth, caps);
			if (!fmt)
				return false;
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = true;
			params.renderable = renderable;
			params.host_readable = hostReadable;
			return pl_tex_recreate(gpu, &tex, &params);
		}

		bool Compute(pl_dispatch dispatch, const char* description, const char* header, const char* body,
			const pl_shader_desc* descriptors, int numDescriptors, const pl_shader_var* variables,
			int numVariables, int groupsX, int groupsY)
		{
			pl_custom_shader custom{};
			custom.description = description;
			custom.header = header;
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
			compute.dispatch_size[0] = groupsX;
			compute.dispatch_size[1] = groupsY;
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
		}

		// No change: SIG_NONE. Handing libplacebo's own texture back counts as
		// a resize when it is a larger pooled texture (POST/PRE_KERNEL may not
		// be resized).
		pl_hook_res Unchanged(const pl_hook_params*)
		{
			pl_hook_res res{};
			res.output = PL_HOOK_SIG_NONE;
			return res;
		}
	}

	Module::Module()
	{
		measure.stages = PL_HOOK_PRE_KERNEL;
		measure.input = PL_HOOK_SIG_TEX;
		measure.priv = this;
		measure.hook = &Module::RunMeasure;
		measure.signature = 0x5650414453444D45ull;   // "VPADSDME"
		blend.stages = PL_HOOK_POST_KERNEL;
		blend.input = PL_HOOK_SIG_TEX;
		blend.priv = this;
		blend.hook = &Module::RunBlend;
		blend.signature = 0x565041445344424Cull;     // "VPADSDBL"
	}

	Module::~Module()
	{
		Release();
	}

	void Module::Release()
	{
		if (gpu)
		{
			for (pl_tex* tex : { &faithful, &edge, &partial, &state, &map[0], &map[1] })
				if (*tex)
					pl_tex_destroy(gpu, tex);
			if (readback)
				pl_buf_destroy(gpu, &readback);
		}
		if (polarLut)
			pl_shader_obj_destroy(&polarLut);
		faithful = edge = partial = state = map[0] = map[1] = nullptr;
		mapIndex = 0;
		mapReady = false;
		mapSequence = 0;
		mapState = MapState::Off;
		grainField = GrainField();
		readback = nullptr;
		readbackPending = false;
		gpu = nullptr;
		active = false;
		advancedSequence = 0;
		stats = Stats();
	}

	void Module::SetFrame(const Settings& value, uint64_t sourceSequence, float seconds)
	{
		settings = value;
		sequence = sourceSequence;
		frameSeconds = seconds > 0.0f ? seconds : 1.0f / 24.0f;
	}

	pl_hook_res Module::RunMeasure(void* priv, const pl_hook_params* params)
	{
		return static_cast<Module*>(priv)->Measuring(params);
	}

	pl_hook_res Module::RunBlend(void* priv, const pl_hook_params* params)
	{
		return static_cast<Module*>(priv)->Blending(params);
	}

	void Module::PollStats()
	{
		if (!readbackPending || !gpu || !readback || pl_buf_poll(gpu, readback, 0))
			return;
		float values[4] = {};
		if (pl_buf_read(gpu, readback, 0, values, sizeof(values)))
		{
			stats.valid = true;
			stats.grainScale = values[0];
			stats.meanW = settings.wBase * values[1] * values[0];
			stats.meanLuma = values[2];
		}
		readbackPending = false;
	}

	pl_hook_res Module::Measuring(const pl_hook_params* p)
	{
		const pl_hook_res unchanged = Unchanged(p);
		active = false;
		if (!p->tex)
			return unchanged;
		const float sx0 = std::fmin(p->src_rect.x0, p->src_rect.x1), sx1 = std::fmax(p->src_rect.x0, p->src_rect.x1);
		const float sy0 = std::fmin(p->src_rect.y0, p->src_rect.y1), sy1 = std::fmax(p->src_rect.y0, p->src_rect.y1);
		const int dw = std::abs(p->dst_rect.x1 - p->dst_rect.x0), dh = std::abs(p->dst_rect.y1 - p->dst_rect.y0);
		// The SSIM shader's own condition: the source is made shorter (the
		// renderer's NATIVE_CROPPED.h > POSTKERNEL.h); and this picture too.
		if (!sourceDownscales || dw <= 0 || dh <= 0 || !(sy1 - sy0 > static_cast<float>(dh) + 0.5f))
			return unchanged;
		if (gpu && gpu != p->gpu)
			Release();
		gpu = p->gpu;
		PollStats();
		const int ox = static_cast<int>(std::floor(sx0)), oy = static_cast<int>(std::floor(sy0));
		const int w = std::max(1, static_cast<int>(std::ceil(sx1)) - ox);
		const int h = std::max(1, static_cast<int>(std::ceil(sy1)) - oy);
		const int gx = (w + 15) / 16, gy = (h + 15) / 16;
		if (!Ensure(gpu, faithful, dw, dh, 4, 16, true, true) || !Ensure(gpu, edge, w, h, 1, 16, false, true) ||
			!Ensure(gpu, partial, gx, gy, 4, 32, false, false) || !Ensure(gpu, state, 1, 1, 4, 32, false, false))
		{
			++failures;
			return unchanged;
		}
		if (!readback)
		{
			// The final pass writes the statistics straight into this storage
			// buffer (the LED measurement's pattern); polled frames later.
			pl_buf_params bp{};
			bp.size = sizeof(float) * 4;
			bp.storable = true;
			bp.host_readable = true;
			readback = pl_buf_create(gpu, &bp);
		}

		// 1. The faithful branch: libplacebo's polar ewa_lanczossharp.
		{
			pl_shader sh = pl_dispatch_begin(p->dispatch);
			pl_sample_src src{};
			src.tex = p->tex;
			src.rect = { sx0, sy0, sx1, sy1 };
			src.new_w = dw;
			src.new_h = dh;
			src.components = 4;
			pl_sample_filter_params filter{};
			filter.filter = pl_filter_ewa_lanczossharp;
			filter.antiring = settings.antiring;
			filter.lut = &polarLut;
			pl_dispatch_params dp{};
			dp.shader = &sh;
			dp.target = faithful;
			if (!pl_shader_sample_polar(sh, &src, &filter) || !pl_dispatch_finish(p->dispatch, &dp))
			{
				if (sh)
					pl_dispatch_abort(p->dispatch, &sh);
				++failures;
				return unchanged;
			}
		}

		// 2. Edges and the per-workgroup grain sums.
		const float linearIn = p->color.transfer == PL_COLOR_TRC_LINEAR ? 1.0f : 0.0f;
		const float origin[2] = { static_cast<float>(ox), static_cast<float>(oy) };
		const float region[2] = { static_cast<float>(w), static_cast<float>(h) };
		{
			const pl_shader_desc d[3] = { Sampled("src", p->tex, false), Storage("edge_out", edge),
				Storage("partial_out", partial) };
			const pl_shader_var v[6] = {
				{ pl_var_float("linear_in"), &linearIn, true },
				{ pl_var_vec2("origin"), origin, true },
				{ pl_var_vec2("region"), region, true },
				{ pl_var_float("edge_lo"), &settings.edgeLo, true },
				{ pl_var_float("edge_hi"), &settings.edgeHi, true },
				{ pl_var_float("shadow_bias"), &settings.shadowBias, true } };
			if (!Compute(p->dispatch, "VideoProcessor adaptive downscale: measure", MEASURE_HEADER, MEASURE_BODY,
				d, 3, v, 6, gx, gy))
			{
				++failures;
				return unchanged;
			}
		}

		// 3. The frame's grain term into the state (once per source frame).
		{
			const float advance = sequence != advancedSequence ? 1.0f : 0.0f;
			const bool writeStats = advance > 0.5f && readback && !readbackPending;
			const float writeStatsF = writeStats ? 1.0f : 0.0f;
			const float total = static_cast<float>(w) * static_cast<float>(h);
			const float alpha = settings.grainSmoothMs > 0.0f ?
				1.0f - std::exp(-frameSeconds * 1000.0f / settings.grainSmoothMs) : 1.0f;
			pl_buffer_var outVar{};
			outVar.var = pl_var_vec4("ad_out");
			outVar.layout = pl_std430_layout(0, &outVar.var);
			pl_shader_desc d[3] = { Sampled("partial_in", partial, false),
				Storage("state_io", state, PL_DESC_ACCESS_READWRITE), pl_shader_desc{} };
			int numDescriptors = 2;
			if (readback)
			{
				d[2].desc.name = "ad_stats";
				d[2].desc.type = PL_DESC_BUF_STORAGE;
				d[2].desc.access = PL_DESC_ACCESS_WRITEONLY;
				d[2].binding.object = readback;
				d[2].buffer_vars = &outVar;
				d[2].num_buffer_vars = 1;
				d[2].memory = PL_MEMORY_COHERENT;
				numDescriptors = 3;
			}
			const pl_shader_var v[7] = {
				{ pl_var_float("write_stats"), &writeStatsF, true },
				{ pl_var_float("advance"), &advance, true },
				{ pl_var_float("total_pixels"), &total, true },
				{ pl_var_float("grain_lo"), &settings.grainLo, true },
				{ pl_var_float("grain_hi"), &settings.grainHi, true },
				{ pl_var_float("grain_floor"), &settings.grainFloor, true },
				{ pl_var_float("smooth_alpha"), &alpha, true } };
			if (numDescriptors != 3 || !Compute(p->dispatch, "VideoProcessor adaptive downscale: grain",
				FINAL_HEADER, FINAL_BODY, d, numDescriptors, v, 7, 1, 1))
			{
				++failures;
				return unchanged;
			}
			if (advance > 0.5f)
			{
				advancedSequence = sequence;
				if (writeStats)
					readbackPending = true;
			}
			// 4. 8-3: the map, after the scalar (it reads the cut flag).
			if (!UpdateMap(p->dispatch, advance > 0.5f))
				++failures;
		}

		srcRect = { sx0, sy0, sx1, sy1 };
		dstRect = p->dst_rect;
		edgeOrigin[0] = origin[0];
		edgeOrigin[1] = origin[1];
		active = true;
		return unchanged;
	}

	bool Module::UpdateMap(pl_dispatch dispatch, bool advance)
	{
		const GrainField& gf = grainField;
		const bool fieldPresent = settings.grainMap && gf.error && gf.field && gf.frameTrust &&
			gf.error->params.w == gf.field->params.w && gf.error->params.h == gf.field->params.h;
		if (!settings.grainMap)
		{
			mapReady = false;
			mapState = MapState::Off;
			return true;
		}
		if (!fieldPresent)
		{
			// Frames without a field use the scalar; the map starts over when
			// the field comes back (it no longer follows the content).
			if (advance)
			{
				mapReady = false;
				mapSequence = 0;
			}
			mapState = mapReady && mapSequence == sequence ? MapState::Used : MapState::NoField;
			return true;
		}
		if (!advance)
		{
			// A repeat or an extra of the same source frame: the map stands.
			mapState = mapReady && mapSequence == sequence ? MapState::Used : MapState::NoField;
			return true;
		}
		const int bw = gf.field->params.w, bh = gf.field->params.h;
		const bool resized = !map[0] || map[0]->params.w != bw || map[0]->params.h != bh;
		if (!Ensure(gpu, map[0], bw, bh, 4, 32, false, false, hostReadableMap) ||
			!Ensure(gpu, map[1], bw, bh, 4, 32, false, false, hostReadableMap))
		{
			mapReady = false;
			mapState = MapState::NoField;
			return false;
		}
		const bool first = resized || mapSequence == 0;
		const int next = 1 - mapIndex;
		const float firstF = first ? 1.0f : 0.0f;
		const float alpha = settings.grainMapSmoothMs > 0.0f ?
			1.0f - std::exp(-frameSeconds * 1000.0f / settings.grainMapSmoothMs) : 1.0f;
		const pl_shader_desc d[6] = { Sampled("field", gf.field, false), Sampled("grain_error", gf.error, false),
			Sampled("frame_trust", gf.frameTrust, false), Sampled("state", state, false),
			Sampled("map_in", map[mapIndex], false), Storage("map_out", map[next]) };
		const pl_shader_var v[8] = {
			{ pl_var_float("min_trusted_share"), &gf.minTrustedShare, true },
			{ pl_var_float("first_map"), &firstF, true },
			{ pl_var_float("shadow_bias"), &settings.shadowBias, true },
			{ pl_var_float("temporal_to_spatial"), &kTemporalToSpatial, true },
			{ pl_var_float("grain_lo"), &settings.grainLo, true },
			{ pl_var_float("grain_hi"), &settings.grainHi, true },
			{ pl_var_float("grain_floor"), &settings.grainFloor, true },
			{ pl_var_float("smooth_alpha"), &alpha, true } };
		if (!Compute(dispatch, "VideoProcessor adaptive downscale: grain map", nullptr, MAP_BODY, d, 6, v, 8,
			(bw + 15) / 16, (bh + 15) / 16))
		{
			mapReady = false;
			mapState = MapState::NoField;
			return false;
		}
		mapIndex = next;
		mapReady = true;
		mapSequence = sequence;
		mapState = MapState::Used;
		return true;
	}

	pl_hook_res Module::Blending(const pl_hook_params* p)
	{
		const pl_hook_res unchanged = Unchanged(p);
		// The image is the destination rectangle's size; libplacebo may hand a
		// larger pooled texture, and POST_KERNEL may not be resized.
		const int w = std::abs(p->dst_rect.x1 - p->dst_rect.x0), h = std::abs(p->dst_rect.y1 - p->dst_rect.y0);
		if (!active || !p->tex || !faithful || w != faithful->params.w || h != faithful->params.h ||
			p->tex->params.w < w || p->tex->params.h < h)
			return unchanged;
		pl_tex out = p->get_tex(p->priv, w, h);
		if (!out || !out->params.storable)
		{
			++failures;
			return unchanged;
		}
		const float dw = static_cast<float>(std::abs(dstRect.x1 - dstRect.x0));
		const float dh = static_cast<float>(std::abs(dstRect.y1 - dstRect.y0));
		const float srcOrigin[2] = { srcRect.x0, srcRect.y0 };
		const float srcPerDst[2] = { dw > 0.0f ? (srcRect.x1 - srcRect.x0) / dw : 1.0f,
			dh > 0.0f ? (srcRect.y1 - srcRect.y0) / dh : 1.0f };
		const float chroma = settings.chromaSsim ? 1.0f : 0.0f;
		// 8-3: the map for this source frame (else the scalar; the binding
		// then repeats the state texture, never read).
		const bool useMap = mapState == MapState::Used && mapReady && mapSequence == sequence;
		const float srcSize[2] = { srcRect.x1 - srcRect.x0, srcRect.y1 - srcRect.y0 };
		const pl_rect2df& sc = grainField.sourceCrop;
		const float crop[4] = { sc.x0, sc.y0, sc.x1 - sc.x0, sc.y1 - sc.y0 };
		const NlsMaskMapping::Mapping& nls = grainField.nls;
		const float nlsActive = useMap && nls.valid ? 1.0f : 0.0f;
		const float nlsCurve = static_cast<float>(nls.curve);
		const float nlsGeometry = static_cast<float>(nls.geometry);
		const float nlsScale[2] = { static_cast<float>(nls.xScale), static_cast<float>(nls.yScale) };
		const float nlsProtect[2] = { static_cast<float>(nls.xProtect), static_cast<float>(nls.yProtect) };
		// Without the map the pass has no `grain_map` binding and no map code
		// (a texture may be bound only once per pass).
		const pl_shader_desc d[6] = { Sampled("hooked", p->tex, false), Sampled("faithful", faithful, false),
			Sampled("edge", edge, true), Sampled("state", state, false), Storage("dst", out),
			Sampled("grain_map", useMap ? map[mapIndex] : nullptr, false) };
		static const std::string mapHeader = std::string("#define AD_USE_MAP 1\n") + OcclusionMask::NlsMapGlsl();
		const pl_shader_var v[12] = {
			{ pl_var_vec2("src_origin"), srcOrigin, true },
			{ pl_var_vec2("src_per_dst"), srcPerDst, true },
			{ pl_var_vec2("edge_origin"), edgeOrigin, true },
			{ pl_var_float("w_base"), &settings.wBase, true },
			{ pl_var_float("chroma_ssim"), &chroma, true },
			{ pl_var_vec2("src_size"), srcSize, true },
			{ pl_var_vec4("crop"), crop, true },
			{ pl_var_float("nls_active"), &nlsActive, true },
			{ pl_var_float("nls_curve"), &nlsCurve, true },
			{ pl_var_float("nls_geometry"), &nlsGeometry, true },
			{ pl_var_vec2("nls_scale"), nlsScale, true },
			{ pl_var_vec2("nls_protect"), nlsProtect, true } };
		if (!Compute(p->dispatch, "VideoProcessor adaptive downscale: blend", useMap ? mapHeader.c_str() : nullptr,
			BLEND_BODY, d, useMap ? 6 : 5, v, useMap ? 12 : 5, (w + 15) / 16, (h + 15) / 16))
		{
			++failures;
			return unchanged;
		}
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = out;
		res.repr = p->repr;
		res.color = p->color;
		res.components = p->components;
		res.rect = p->rect;
		return res;
	}
}
