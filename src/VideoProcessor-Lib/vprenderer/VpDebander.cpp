#include <pch.h>
#include "VpDebander.h"

#include <vprenderer/GrainPreserve.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
#ifdef SPLIT
    // deband_vp_grain_guard split (GrainPreserve::kSplit*): the low band (half
    // resolution, bilinear at pos / 2) is debanded, the residual c - low passes. The
    // ring mean is the low band's; the pixel's own low band gets back its step part:
    // the residual averaged along the contour (the structure tensor's direction),
    // weighted by the contour's coherence and the residual's consistency along it.
    vec2 linv = 0.5 / vec2(textureSize(split_low, 0));
    vec4 lowc = texture(split_low, pos * linv);
    vec4 avg = 0.25 * (texture(split_low, (pos + o) * linv) + texture(split_low, (pos - o) * linv) +
                       texture(split_low, (pos + q) * linv) + texture(split_low, (pos - q) * linv));
    vec4 J = texture(split_tensor, pos / tensor_px / vec2(textureSize(split_tensor, 0)));
    float jd = J.x - J.z;
    float coh = sqrt(jd * jd + 4.0 * J.y * J.y) / (J.x + J.z + coh_eps);
    float theta = (abs(jd) + abs(J.y) > 0.0) ? 0.5 * atan(2.0 * J.y, jd) : 0.0;
    vec2 along = vec2(-sin(theta), cos(theta)) * along_step;
    vec4 ra = vec4(0.0), ra2 = vec4(0.0);
    for (int k = -ALONG_N; k <= ALONG_N; k++) {
        vec2 sp = pos + float(k) * along;
        vec4 rv = texture(src, sp * inv) - texture(split_low, sp * linv);
        ra += along_w(k) * rv;
        ra2 += along_w(k) * rv * rv;
    }
    // The mean's significance: |mean| over its standard error (GrainPreserve::SplitConsistency).
    vec4 rho = abs(ra) / sqrt(max(ra2 - ra * ra, vec4(0.0)) * along_inv_neff + rho_eps);
    vec4 base = lowc + ra * (smoothstep(coh_lo, coh_hi, coh) * smoothstep(rho_lo, rho_hi, rho));
    vec4 diff = abs(base - avg);
#else
    vec4 avg = 0.25 * (texture(src, (pos + o) * inv) + texture(src, (pos - o) * inv) +
                       texture(src, (pos + q) * inv) + texture(src, (pos - q) * inv));
    vec4 diff = abs(c - avg);
#endif
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
    // vectors needed - the share of busy pixels per 16x16 block (GUARD_BODY: Laplacian
    // above lap_min or a local extremum; a band is flat but for its edges, grain is busy
    // everywhere), bilinear between block centres; blur vetoed from veto_lo to veto_hi.
    {
        float share = texture(grain_spatial, pos / 16.0 / vec2(textureSize(grain_spatial, 0))).r;
        w *= 1.0 - smoothstep(veto_lo, veto_hi, share);
    }
#endif
#ifdef SPLIT
    imageStore(dst, p, c + w * (avg - base));
#else
    imageStore(dst, p, mix(c, avg, w));
#endif
#ifdef WRITE_W
    imageStore(w_out, p, vec4(w, 0.0, 0.0, 1.0));
#endif
}
)GLSL";

		// The spatial grain estimate per 16x16 block (one workgroup): the share of the
		// block's busy pixels (GrainPreserve::GuardLaplacianCodes / GuardExtremumCodes):
		// Laplacian |4c - n - s - e - w| (largest channel) above lap_min, or a strict
		// local extremum along x, y or a diagonal by more than ext_min on both sides (a
		// quantised smooth gradient is monotone along every line: never). The block and
		// its one-pixel border are read once into shared memory (1.27 fetches a pixel).
		const char* const GUARD_HEADER = R"GLSL(
shared vec2 gg_sum[256];
shared vec4 gg_tile[324];
)GLSL";
		const char* const GUARD_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
uint li = gl_LocalInvocationIndex;
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 origin = ivec2(gl_WorkGroupID.xy) * 16 - ivec2(1);
for (uint i = li; i < 324u; i += 256u)
    gg_tile[i] = texelFetch(src, clamp(origin + ivec2(int(i % 18u), int(i / 18u)), ivec2(0), hi), 0);
barrier();
vec2 v = vec2(0.0);
if (p.x < sz.x && p.y < sz.y) {
    int t = (int(gl_LocalInvocationID.y) + 1) * 18 + int(gl_LocalInvocationID.x) + 1;
    vec4 c = gg_tile[t];
    vec4 dw = c - gg_tile[t - 1], de = c - gg_tile[t + 1];
    vec4 dn = c - gg_tile[t - 18], ds = c - gg_tile[t + 18];
    vec4 d1 = c - gg_tile[t - 19], d2 = c - gg_tile[t + 19];
    vec4 d3 = c - gg_tile[t - 17], d4 = c - gg_tile[t + 17];
    vec4 l = abs(dw + de + dn + ds);
    // Extremum along a line: both differences above ext_min, or both below -ext_min.
    vec4 x = max(max(max(min(dw, de), -max(dw, de)), max(min(dn, ds), -max(dn, ds))),
                 max(max(min(d1, d2), -max(d1, d2)), max(min(d3, d4), -max(d3, d4))));
    float m = l.x;
    float e = x.x;
#if NCOMP > 1
    m = max(m, l.y);
    e = max(e, x.y);
#endif
#if NCOMP > 2
    m = max(m, l.z);
    e = max(e, x.z);
#endif
    v = vec2((m > lap_min || e > ext_min) ? 1.0 : 0.0, 1.0);
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

		// deband_vp_grain_guard split: the low band at half resolution (GrainPreserve, as
		// the interpolator's): a 2x2 box average, then the separable Gaussian (gp_w).
		const char* const SPLIT_HALF_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    ivec2 p = q * 2;
    vec4 acc = texelFetch(src, clamp(p, ivec2(0), hi), 0) + texelFetch(src, clamp(p + ivec2(1, 0), ivec2(0), hi), 0) +
               texelFetch(src, clamp(p + ivec2(0, 1), ivec2(0), hi), 0) + texelFetch(src, clamp(p + ivec2(1, 1), ivec2(0), hi), 0);
    imageStore(dst, q, 0.25 * acc);
}
)GLSL";
		const char* const SPLIT_H_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    vec4 acc = vec4(0.0);
    for (int i = -GP_R; i <= GP_R; i++)
        acc += gp_w(i) * texelFetch(src, clamp(q + ivec2(i, 0), ivec2(0), hi), 0);
    imageStore(dst, q, acc);
}
)GLSL";
		const char* const SPLIT_V_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    vec4 acc = vec4(0.0);
    for (int i = -GP_R; i <= GP_R; i++)
        acc += gp_w(i) * texelFetch(src, clamp(q + ivec2(0, i), ivec2(0), hi), 0);
    imageStore(dst, q, acc);
}
)GLSL";
		// The structure tensor of the low band per TB x TB block of its (half-resolution)
		// pixels: the mean of (gx^2, gx gy, gy^2), central differences, summed over the
		// plane's components, in codes^2 (code_scale = 1 / one code).
		const char* const SPLIT_TENSOR_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (b.x < size.x && b.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    vec4 mask = vec4(1.0, NCOMP > 1 ? 1.0 : 0.0, NCOMP > 2 ? 1.0 : 0.0, 0.0);
    vec3 acc = vec3(0.0);
    float n = 0.0;
    for (int j = 0; j < TB; j++)
        for (int i = 0; i < TB; i++) {
            ivec2 q = b * TB + ivec2(i, j);
            if (q.x > hi.x || q.y > hi.y)
                continue;
            vec4 gx = 0.5 * code_scale * mask * (texelFetch(src, clamp(q + ivec2(1, 0), ivec2(0), hi), 0) - texelFetch(src, clamp(q - ivec2(1, 0), ivec2(0), hi), 0));
            vec4 gy = 0.5 * code_scale * mask * (texelFetch(src, clamp(q + ivec2(0, 1), ivec2(0), hi), 0) - texelFetch(src, clamp(q - ivec2(0, 1), ivec2(0), hi), 0));
            acc += vec3(dot(gx, gx), dot(gx, gy), dot(gy, gy));
            n += 1.0;
        }
    imageStore(dst, b, vec4(acc / max(n, 1.0), 1.0));
}
)GLSL";

		pl_fmt FloatFormat(pl_gpu gpu, int components, bool linear)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | (linear ? PL_FMT_CAP_LINEAR : 0));
			for (int n = (std::max)(1, components); n <= 4; ++n)
				if (pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, n, 32, 32, caps))
					return f;
			for (int n = (std::max)(1, components); n <= 4; ++n)
				if (pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, n, 16, 16, caps))
					return f;
			return nullptr;
		}

		bool Pass(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
			const pl_shader_desc* desc, int numDesc, const pl_shader_var* vars, int numVars, int groupsX, int groupsY)
		{
			pl_custom_shader custom{};
			custom.description = description;
			custom.header = header.c_str();
			custom.body = body;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc;
			custom.num_descriptors = numDesc;
			custom.variables = vars;
			custom.num_variables = numVars;
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

		// along_w(k), k in -ALONG_N..ALONG_N: GrainPreserve::SplitAlongWeight.
		std::string AlongWeightsGlsl()
		{
			const int n = GrainPreserve::kSplitAlongTaps;
			const std::string taps = std::to_string(2 * n + 1);
			std::string s = "#define ALONG_N " + std::to_string(n) + "\nfloat along_w(int k)\n{\n    float w[" + taps + "] = float[" + taps + "](";
			for (int k = -n; k <= n; ++k)
			{
				char number[32];
				snprintf(number, sizeof(number), "%.9g", GrainPreserve::SplitAlongWeight(k));
				s += number;
				if (k < n)
					s += ", ";
			}
			return s + ");\n    return w[k + ALONG_N];\n}\n";
		}

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

	double Strength(double effectiveBits, int colorDepth, double bitrateQuality, bool useQuality, bool snapLevels)
	{
		double threshold = 0.0;
		// The content's own level step: a band is one step high.
		if (snapLevels)
		{
			if (effectiveBits > 0.0 && effectiveBits < colorDepth - 0.5)
				threshold = 1.5 * std::pow(2.0, colorDepth - std::floor(effectiveBits + 0.1));
		}
		else if (effectiveBits > 0.0 && effectiveBits < colorDepth - 0.25)
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
		for (int s = 0; s < kSlots; ++s)
			for (int p = 0; p < PL_MAX_PLANES; ++p)
				for (pl_tex* tex : { &splitHalf[s][p], &splitTmp[s][p], &splitLow[s][p], &splitTensor[s][p] })
				{
					if (*tex && gpu) pl_tex_destroy(gpu, tex);
					*tex = nullptr;
				}
		weightSequence = 0;
		prevValid = false;
	}

	bool Module::SplitLowBand(pl_gpu gpu, pl_dispatch dispatch, pl_tex in, int slot, int plane, int ncomp, double scale, double codeUnit)
	{
		const int hw = (in->params.w + 1) / 2, hh = (in->params.h + 1) / 2;
		const int n = in->params.format->num_components;
		const int tb = GrainPreserve::SplitTensorHalfBlock(scale);
		const int tw = (hw + tb - 1) / tb, th = (hh + tb - 1) / tb;
		auto ensure = [&](pl_tex& tex, int w, int h, pl_fmt fmt) {
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = fmt;
			tp.sampleable = true;
			tp.storable = true;
			return fmt && pl_tex_recreate(gpu, &tex, &tp);
		};
		if (!ensure(splitHalf[slot][plane], hw, hh, FloatFormat(gpu, n, false)) ||
			!ensure(splitTmp[slot][plane], hw, hh, FloatFormat(gpu, n, false)) ||
			!ensure(splitLow[slot][plane], hw, hh, FloatFormat(gpu, n, true)) ||
			!ensure(splitTensor[slot][plane], tw, th, FloatFormat(gpu, 4, true)))
		{
			error = "no float texture for the grain split";
			++failures;
			return false;
		}
		const int gx = (hw + 15) / 16, gy = (hh + 15) / 16;
		const std::string weights = GrainPreserve::LowWeightsGlsl(GrainPreserve::kSplitSigma * scale);
		const pl_shader_desc dh[2] = { Sampled("src", in, false), Storage("dst", splitHalf[slot][plane]) };
		const pl_shader_desc dx[2] = { Sampled("src", splitHalf[slot][plane], false), Storage("dst", splitTmp[slot][plane]) };
		const pl_shader_desc dy[2] = { Sampled("src", splitTmp[slot][plane], false), Storage("dst", splitLow[slot][plane]) };
		const pl_shader_desc dt[2] = { Sampled("src", splitLow[slot][plane], false), Storage("dst", splitTensor[slot][plane]) };
		const float codeScale = static_cast<float>(1.0 / codeUnit);
		const pl_shader_var vt[1] = { { pl_var_float("code_scale"), &codeScale, true } };
		const std::string tensorHeader = "#define NCOMP " + std::to_string(ncomp) + "\n#define TB " + std::to_string(tb) + "\n";
		if (!Pass(dispatch, "VideoProcessor deband: grain split (half)", std::string(), SPLIT_HALF_BODY, dh, 2, nullptr, 0, gx, gy) ||
			!Pass(dispatch, "VideoProcessor deband: grain split (across)", weights, SPLIT_H_BODY, dx, 2, nullptr, 0, gx, gy) ||
			!Pass(dispatch, "VideoProcessor deband: grain split (down)", weights, SPLIT_V_BODY, dy, 2, nullptr, 0, gx, gy) ||
			!Pass(dispatch, "VideoProcessor deband: grain split (contours)", tensorHeader, SPLIT_TENSOR_BODY, dt, 2, vt, 1, (tw + 15) / 16, (th + 15) / 16))
		{
			error = "the grain split passes failed";
			++failures;
			return false;
		}
		return true;
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
			// deband_vp_grain_guard split: grain passes by construction, so neither veto runs
			// (the vector grain veto read band pumping on a starved stream as grain: H1).
			const bool useSplit = params.grainGuard && params.grainSplit;
			const bool useGrain = haveField && params.grainError != nullptr && !useSplit;
			// The plane's pixels in field (native luma) pixels.
			const float toField[2] = {
				static_cast<float>((params.fieldScaleX > 0.0 ? params.fieldScaleX : params.fieldScale) * luma->params.w / (std::max)(1, w)),
				static_cast<float>(params.fieldScale * luma->params.h / (std::max)(1, h)) };
			const float radius = static_cast<float>((std::max)(2.0, (std::min)(64.0, 16.0 * params.radiusScale * h / (std::max)(1, luma->params.h))));
			const float alpha = static_cast<float>(params.holdAlpha);
			const float grainCodes = static_cast<float>(params.grainCodes);
			const int ncomp = (std::max)(1, (std::min)(3, source.planes[plane].components));

			// deband_vp_grain_guard: the plane's spatial grain estimate first.
			const bool useGuard = params.grainGuard && !useSplit;
			// The split's distances: grain scale (GrainPreserve::GrainScale) of this plane's
			// lines in the picture (radiusScale = the source's lines / 1080).
			const double planeLines = 1080.0 * params.radiusScale * h / (std::max)(1, luma->params.h);
			const double splitScale = GrainPreserve::GrainScale(planeLines);
			const float tensorPx = static_cast<float>(2 * GrainPreserve::SplitTensorHalfBlock(splitScale));
			const float alongStep = static_cast<float>(GrainPreserve::kSplitAlongStep * splitScale);
			const float cohEps = static_cast<float>(GrainPreserve::kSplitTensorEps);
			const float rhoEps = static_cast<float>(1e-12 * codeUnit * codeUnit);
			const float cohLo = static_cast<float>(GrainPreserve::kSplitCoherenceLo), cohHi = static_cast<float>(GrainPreserve::kSplitCoherenceHi);
			const float rhoLo = static_cast<float>(GrainPreserve::kSplitConsistencyLo), rhoHi = static_cast<float>(GrainPreserve::kSplitConsistencyHi);
			const float alongInvNeff = static_cast<float>(GrainPreserve::SplitAlongInvNeff());
			if (useSplit && !SplitLowBand(gpu, dispatch, in, slot, plane, ncomp, splitScale, codeUnit))
				return false;
			const float lapMin = static_cast<float>(GrainPreserve::GuardLaplacianCodes(params.thresholdCodes, params.colorDepth) * codeUnit);
			const float extMin = static_cast<float>(GrainPreserve::GuardExtremumCodes(params.thresholdCodes, params.colorDepth) * codeUnit);
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
				const pl_shader_var gv[2] = { { pl_var_float("lap_min"), &lapMin, true }, { pl_var_float("ext_min"), &extMin, true } };
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
				gc.num_variables = 2;
				gc.compute = true;
				gc.compute_shmem = sizeof(float) * (256 * 2 + 324 * 4);   // gg_sum + gg_tile
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

			pl_shader_desc d[9];
			int nd = 0;
			d[nd++] = Sampled("src", in, true);
			d[nd++] = Storage("dst", out[slot][plane]);
			if (weightOut) d[nd++] = Storage("w_out", weightOut);
			if (useHold || useGrain) d[nd++] = Sampled("field", params.field, false);
			if (useHold) d[nd++] = Sampled("prev_w", weightIn, true);
			if (useGrain) d[nd++] = Sampled("grain", params.grainError, false);
			if (useGuard) d[nd++] = Sampled("grain_spatial", guard[slot][plane], true);
			if (useSplit)
			{
				d[nd++] = Sampled("split_low", splitLow[slot][plane], true);
				d[nd++] = Sampled("split_tensor", splitTensor[slot][plane], true);
			}
			pl_shader_var v[20];
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
			if (useSplit)
			{
				v[nv++] = { pl_var_float("tensor_px"), &tensorPx, true };
				v[nv++] = { pl_var_float("along_step"), &alongStep, true };
				v[nv++] = { pl_var_float("coh_eps"), &cohEps, true };
				v[nv++] = { pl_var_float("rho_eps"), &rhoEps, true };
				v[nv++] = { pl_var_float("coh_lo"), &cohLo, true };
				v[nv++] = { pl_var_float("coh_hi"), &cohHi, true };
				v[nv++] = { pl_var_float("rho_lo"), &rhoLo, true };
				v[nv++] = { pl_var_float("rho_hi"), &rhoHi, true };
				v[nv++] = { pl_var_float("along_inv_neff"), &alongInvNeff, true };
			}
			std::string header = "#define NCOMP " + std::to_string(ncomp) + "\n";
			if (useHold) header += "#define HOLD 1\n";
			if (useGrain) header += "#define GRAIN 1\n";
			if (weightOut) header += "#define WRITE_W 1\n";
			if (useGuard) header += "#define SPATIAL_GUARD 1\n";
			if (useSplit) header += "#define SPLIT 1\n" + AlongWeightsGlsl();
			pl_custom_shader custom{};
			custom.description = useHold ? (useSplit ? "VideoProcessor deband, grain split (held along the vectors)" : "VideoProcessor deband (held along the vectors)") :
				(useSplit ? "VideoProcessor deband, grain split" : "VideoProcessor deband");
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
