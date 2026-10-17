#include <pch.h>
#include "VpDebander.h"

#include <vprenderer/GrainPreserve.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>

namespace VpDebander
{
	namespace
	{
		// One invocation per plane pixel. `src` is sampled bilinearly (a texel
		// centre reads the texel itself); four samples at a fixed random angle
		// and distance (a hash of the position); the mean replaces the pixel by
		// the soft decision w (1 at 0.6 x threshold, 0 at the threshold).
		const char* const BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 sz = imageSize(dst);
if (p.x < sz.x && p.y < sz.y) {
    vec2 inv = 1.0 / vec2(textureSize(src, 0));
    vec2 pos = vec2(p) + vec2(0.5);
    vec4 c = texture(src, pos * inv);
    uint h = uint(p.x) * 1973u + uint(p.y) * 9277u + 26699u;
    h = (h ^ (h >> 13u)) * 1540483477u;
    h ^= h >> 15u;
    float r1 = float(h & 65535u) / 65535.0;
    float r2 = float(h >> 16u) / 65535.0;
    float d = mix(1.0, radius, r1);
    vec2 o = vec2(cos(r2 * 6.2831853), sin(r2 * 6.2831853)) * d;
    vec2 q = vec2(-o.y, o.x);
    vec4 avg = 0.25 * (texture(src, (pos + o) * inv) + texture(src, (pos - o) * inv) +
                       texture(src, (pos + q) * inv) + texture(src, (pos - q) * inv));
    vec4 diff = abs(c - avg);
    float m = diff.x;
#if NCOMP > 1
    m = max(m, diff.y);
#endif
#if NCOMP > 2
    m = max(m, diff.z);
#endif
    float w = 1.0 - smoothstep(0.6 * thr, thr, m);
#if defined(HOLD) || defined(GRAIN)
    // The plane position in the de-judder field's source pixels.
    vec2 lp = pos * to_field;
    ivec2 fs = textureSize(field, 0);
    vec4 f = texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fs - ivec2(1)), 0);
#endif
#ifdef GRAIN
    if (f.z > 0.5) {
        ivec2 gs = textureSize(grain, 0);
        float g = texelFetch(grain, clamp(ivec2(floor(lp / 16.0)), ivec2(0), gs - ivec2(1)), 0).r;
        if (g > grain_codes) w = 0.0;   // texture or grain: never blurred
    }
#endif
#ifdef HOLD
    // Held only inside this frame's own blur band (below the threshold): the
    // hold steadies the near-threshold decisions that flicker, and never
    // blurs a pixel this frame sees as texture (block vectors are not
    // pixel-exact in fine grain).
    if (f.z > 0.5 && m < thr) {
        // The previous frame's weight where this content was (constant motion).
        vec2 prev = (lp - f.xy) / to_field;
        float wp = texture(prev_w, prev / vec2(textureSize(prev_w, 0))).r;
        w = mix(wp, w, hold_alpha);
    }
#endif
#ifdef SPATIAL_GUARD
    // deband_vp_grain_guard (owner 2026-10-17): this frame's own grain estimate, no
    // vectors needed - the share of pixels per 16x16 block whose Laplacian exceeds
    // lap_min (a band is flat but for its edges; grain is busy
    // everywhere), bilinear between block centres; blur vetoed from veto_lo to veto_hi.
    {
        float share = texture(grain_spatial, pos / 16.0 / vec2(textureSize(grain_spatial, 0))).r;
        w *= 1.0 - smoothstep(veto_lo, veto_hi, share);
    }
#endif
    imageStore(dst, p, mix(c, avg, w));
#ifdef WRITE_W
    imageStore(w_out, p, vec4(w, 0.0, 0.0, 1.0));
#endif
}
)GLSL";

		// The spatial grain estimate per 16x16 block (one workgroup): the share of the
		// block's pixels whose Laplacian |4c - n - s - e - w| (largest channel) exceeds
		// lap_min (GrainPreserve::GuardLaplacianCodes: a third of the threshold's
		// content-step part, never raised by the low-bitrate allowance).
		const char* const GUARD_HEADER = R"GLSL(
shared vec2 gg_sum[256];
)GLSL";
		const char* const GUARD_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
uint li = gl_LocalInvocationIndex;
ivec2 sz = textureSize(src, 0);
vec2 v = vec2(0.0);
if (p.x < sz.x && p.y < sz.y) {
    ivec2 hi = sz - ivec2(1);
    vec4 c = texelFetch(src, p, 0);
    vec4 l = abs(4.0 * c - texelFetch(src, clamp(p + ivec2(1, 0), ivec2(0), hi), 0)
                         - texelFetch(src, clamp(p - ivec2(1, 0), ivec2(0), hi), 0)
                         - texelFetch(src, clamp(p + ivec2(0, 1), ivec2(0), hi), 0)
                         - texelFetch(src, clamp(p - ivec2(0, 1), ivec2(0), hi), 0));
    float m = l.x;
#if NCOMP > 1
    m = max(m, l.y);
#endif
#if NCOMP > 2
    m = max(m, l.z);
#endif
    v = vec2(m > lap_min ? 1.0 : 0.0, 1.0);
}
gg_sum[li] = v;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        gg_sum[li] += gg_sum[li + s];
    barrier();
}
if (li == 0u)
    imageStore(guard_out, ivec2(gl_WorkGroupID.xy), vec4(gg_sum[0].x / max(gg_sum[0].y, 1.0), 0.0, 0.0, 1.0));
)GLSL";

		pl_shader_desc Sampled(const char* name, pl_tex tex, bool linear)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = tex;
			d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
			return d;
		}

		pl_shader_desc Storage(const char* name, pl_tex tex)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = PL_DESC_ACCESS_WRITEONLY;
			d.binding.object = tex;
			return d;
		}

		pl_fmt StorableLike(pl_gpu gpu, pl_fmt like)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
			// At least 16 bits: an 8-bit output would round the debanded gradient back onto its
			// band steps (audit 2026-10-16).
			if (like && (like->caps & caps) == caps && like->component_depth[0] >= 16) return like;
			const int n = like ? like->num_components : 1;
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_UNORM, n, 16, 16, caps);
			return f ? f : pl_find_fmt(gpu, PL_FMT_FLOAT, n, 16, 16, caps);
		}
	}

	double Strength(double effectiveBits, int colorDepth, double bitrateQuality, bool useQuality)
	{
		double threshold = 0.0;
		// The content's own level step: a band is one step high.
		if (effectiveBits > 0.0 && effectiveBits < colorDepth - 0.25)
			threshold = 1.5 * std::pow(2.0, colorDepth - effectiveBits);
		// Low bitrate (spec 2b): up to three 8-bit steps at quality 0, from
		// nothing at 0.8 and above.
		if (useQuality && bitrateQuality < 0.8)
		{
			const double eightBitStep = std::pow(2.0, (std::max)(0, colorDepth - 8));
			threshold = (std::max)(threshold, 3.0 * eightBitStep * (0.8 - (std::max)(0.0, bitrateQuality)) / 0.8);
		}
		return threshold;
	}

	void Module::Release(pl_gpu gpu)
	{
		for (int s = 0; s < kSlots; ++s)
			for (int p = 0; p < PL_MAX_PLANES; ++p)
			{
				if (out[s][p] && gpu) pl_tex_destroy(gpu, &out[s][p]);
				out[s][p] = nullptr;
			}
		for (int i = 0; i < 2; ++i)
			for (int p = 0; p < PL_MAX_PLANES; ++p)
			{
				if (weight[i][p] && gpu) pl_tex_destroy(gpu, &weight[i][p]);
				weight[i][p] = nullptr;
			}
		for (int s = 0; s < kSlots; ++s)
			for (int p = 0; p < PL_MAX_PLANES; ++p)
			{
				if (guard[s][p] && gpu) pl_tex_destroy(gpu, &guard[s][p]);
				guard[s][p] = nullptr;
			}
		weightSequence = 0;
		prevValid = false;
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result, int slot)
	{
		error.clear();
		if (!gpu || !dispatch || source.num_planes <= 0 || !(params.thresholdCodes > 0.0))
		{
			error = "nothing to deband";
			return false;
		}
		slot = (std::max)(0, (std::min)(slot, kSlots - 1));
		const bool mainFrame = slot == 0;
		const pl_tex luma = source.planes[0].texture;
		if (!luma)
		{
			error = "no luma plane";
			++failures;
			return false;
		}
		// Holding needs this frame's field and the previous source frame's
		// weights. A source frame presented again (a cadence repeat) is
		// recomputed against the same previous weights, never advancing.
		const bool haveField = params.field != nullptr;
		const bool repeat = mainFrame && params.sequence != 0 && params.sequence == weightSequence;
		const bool consecutive = mainFrame && weightSequence != 0 && params.sequence == weightSequence + 1;
		int readIndex = -1, writeIndex = 1 - curIndex;
		if (repeat) { writeIndex = curIndex; readIndex = prevValid ? 1 - curIndex : -1; }
		else if (consecutive) readIndex = curIndex;
		const bool hold = haveField && readIndex >= 0;
		const int next = writeIndex;
		// One code of the signal in the plane texture's normalised units.
		const int sampleDepth = source.repr.bits.sample_depth > 0 ? source.repr.bits.sample_depth : luma->params.format->component_depth[0];
		const double codeUnit = luma->params.format->type == PL_FMT_FLOAT ?
			1.0 / (std::pow(2.0, params.colorDepth) - 1.0) :
			std::pow(2.0, source.repr.bits.bit_shift) / (std::pow(2.0, sampleDepth) - 1.0);
		const float thr = static_cast<float>(params.thresholdCodes * codeUnit);

		pl_frame frame = source;
		for (int plane = 0; plane < source.num_planes; ++plane)
		{
			const pl_tex in = source.planes[plane].texture;
			if (!in || !in->params.sampleable)
			{
				error = "plane " + std::to_string(plane) + " is not sampleable";
				++failures;
				return false;
			}
			const int w = in->params.w, h = in->params.h;
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = StorableLike(gpu, in->params.format);
			tp.sampleable = true;
			tp.storable = true;
			tp.blit_src = true;
			if (!tp.format || !pl_tex_recreate(gpu, &out[slot][plane], &tp))
			{
				error = "no storable texture for plane " + std::to_string(plane);
				++failures;
				return false;
			}
			pl_tex weightOut = nullptr, weightIn = nullptr;
			if (mainFrame)
			{
				pl_tex_params wp{};
				wp.w = w;
				wp.h = h;
				wp.format = StorableLike(gpu, pl_find_named_fmt(gpu, "r16f"));
				wp.sampleable = true;
				wp.storable = true;
				if (!wp.format || !pl_tex_recreate(gpu, &weight[next][plane], &wp))
				{
					error = "no weight texture";
					++failures;
					return false;
				}
				weightOut = weight[next][plane];
				if (hold && weight[readIndex][plane] && weightW[plane] == w && weightH[plane] == h)
					weightIn = weight[readIndex][plane];
			}
			const bool useHold = weightIn != nullptr;
			const bool useGrain = haveField && params.grainError != nullptr;
			// The plane's pixels in field (native luma) pixels.
			const float toField[2] = {
				static_cast<float>((params.fieldScaleX > 0.0 ? params.fieldScaleX : params.fieldScale) * luma->params.w / (std::max)(1, w)),
				static_cast<float>(params.fieldScale * luma->params.h / (std::max)(1, h)) };
			const float radius = static_cast<float>((std::max)(2.0, (std::min)(64.0, 16.0 * params.radiusScale * h / (std::max)(1, luma->params.h))));
			const float alpha = static_cast<float>(params.holdAlpha);
			const float grainCodes = static_cast<float>(params.grainCodes);
			const int ncomp = (std::max)(1, (std::min)(3, source.planes[plane].components));

			// deband_vp_grain_guard: the plane's spatial grain estimate first.
			const bool useGuard = params.grainGuard;
			const float lapMin = static_cast<float>(GrainPreserve::GuardLaplacianCodes(params.thresholdCodes, params.colorDepth) * codeUnit);
			const float vetoLo = static_cast<float>(GrainPreserve::kVetoLo);
			const float vetoHi = static_cast<float>(GrainPreserve::kVetoHi);
			if (useGuard)
			{
				pl_tex_params gp{};
				gp.w = (w + 15) / 16;
				gp.h = (h + 15) / 16;
				gp.format = StorableLike(gpu, pl_find_named_fmt(gpu, "r16f"));
				gp.sampleable = true;
				gp.storable = true;
				if (!gp.format || !pl_tex_recreate(gpu, &guard[slot][plane], &gp))
				{
					error = "no grain guard texture";
					++failures;
					return false;
				}
				const pl_shader_desc gd[2] = { Sampled("src", in, false), Storage("guard_out", guard[slot][plane]) };
				const pl_shader_var gv[1] = { { pl_var_float("lap_min"), &lapMin, true } };
				const std::string guardHeader = "#define NCOMP " + std::to_string(ncomp) + "\n" + GUARD_HEADER;
				pl_custom_shader gc{};
				gc.description = "VideoProcessor deband: grain guard";
				gc.header = guardHeader.c_str();
				gc.body = GUARD_BODY;
				gc.input = PL_SHADER_SIG_NONE;
				gc.output = PL_SHADER_SIG_NONE;
				gc.descriptors = gd;
				gc.num_descriptors = 2;
				gc.variables = gv;
				gc.num_variables = 1;
				gc.compute = true;
				gc.compute_group_size[0] = 16;
				gc.compute_group_size[1] = 16;
				pl_shader gsh = pl_dispatch_begin(dispatch);
				if (!pl_shader_custom(gsh, &gc))
				{
					pl_dispatch_abort(dispatch, &gsh);
					error = "the grain guard shader was refused";
					++failures;
					return false;
				}
				pl_dispatch_compute_params gcompute{};
				gcompute.shader = &gsh;
				gcompute.dispatch_size[0] = gp.w;
				gcompute.dispatch_size[1] = gp.h;
				gcompute.dispatch_size[2] = 1;
				if (!pl_dispatch_compute(dispatch, &gcompute))
				{
					error = "the grain guard pass failed";
					++failures;
					return false;
				}
			}

			pl_shader_desc d[7];
			int nd = 0;
			d[nd++] = Sampled("src", in, true);
			d[nd++] = Storage("dst", out[slot][plane]);
			if (weightOut) d[nd++] = Storage("w_out", weightOut);
			if (useHold || useGrain) d[nd++] = Sampled("field", params.field, false);
			if (useHold) d[nd++] = Sampled("prev_w", weightIn, true);
			if (useGrain) d[nd++] = Sampled("grain", params.grainError, false);
			if (useGuard) d[nd++] = Sampled("grain_spatial", guard[slot][plane], true);
			pl_shader_var v[7];
			int nv = 0;
			v[nv++] = { pl_var_float("thr"), &thr, true };
			v[nv++] = { pl_var_float("radius"), &radius, true };
			if (useHold || useGrain) v[nv++] = { pl_var_vec2("to_field"), toField, true };
			if (useHold) v[nv++] = { pl_var_float("hold_alpha"), &alpha, true };
			if (useGrain) v[nv++] = { pl_var_float("grain_codes"), &grainCodes, true };
			if (useGuard)
			{
				v[nv++] = { pl_var_float("veto_lo"), &vetoLo, true };
				v[nv++] = { pl_var_float("veto_hi"), &vetoHi, true };
			}
			std::string header = "#define NCOMP " + std::to_string(ncomp) + "\n";
			if (useHold) header += "#define HOLD 1\n";
			if (useGrain) header += "#define GRAIN 1\n";
			if (weightOut) header += "#define WRITE_W 1\n";
			if (useGuard) header += "#define SPATIAL_GUARD 1\n";
			pl_custom_shader custom{};
			custom.description = useHold ? "VideoProcessor deband (held along the vectors)" : "VideoProcessor deband";
			custom.header = header.c_str();
			custom.body = BODY;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = d;
			custom.num_descriptors = nd;
			custom.variables = v;
			custom.num_variables = nv;
			custom.compute = true;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				error = "the deband shader was refused";
				++failures;
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (w + 15) / 16;
			compute.dispatch_size[1] = (h + 15) / 16;
			compute.dispatch_size[2] = 1;
			if (!pl_dispatch_compute(dispatch, &compute))
			{
				error = "the deband pass failed";
				++failures;
				return false;
			}
			frame.planes[plane].texture = out[slot][plane];
			if (mainFrame)
			{
				weightW[plane] = w;
				weightH[plane] = h;
			}
		}
		if (mainFrame)
		{
			if (!repeat)
			{
				prevValid = consecutive;
				curIndex = next;
				weightSequence = params.sequence;
			}
			if (hold) ++held;
		}
		result = frame;
		return true;
	}
}
