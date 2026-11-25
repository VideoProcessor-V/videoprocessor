#include <pch.h>
#include "VpDebander.h"

#include <vprenderer/GrainPreserve.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdgeGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>
#include <VP-V/modules/repair/grain_split/GrainSplitVariantOwner.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
    // Shared grain guard (15b A3): reads P10/P10c via gs_fetch (when P10_GRAIN) and P11
    // structure tensor re_s1 = (jxx, jyy, jxy, lap_e) (when P11_TENSOR); visibly falls back
    // to kBandMemo / low-band and local low-band gradient covariance when absent. BTB/WTW
    // values pass unclamped via additive c + w * (avg - base).
    ivec2 hi = sz - ivec2(1);
#ifdef P10_GRAIN
    vec4 lowc = vec4(gs_fetch(p).w * p10_cu, 0.0, 0.0, 0.0);
    vec4 avg = (p10_base_at(pos + o, hi, p10_cu) + p10_base_at(pos - o, hi, p10_cu) +
                p10_base_at(pos + q, hi, p10_cu) + p10_base_at(pos - q, hi, p10_cu)) / 4.0;
#else
    vec2 linv = 0.5 / vec2(textureSize(split_low, 0));
    vec4 lowc = texture(split_low, pos * linv);
    vec4 avg = 0.25 * (texture(split_low, (pos + o) * linv) + texture(split_low, (pos - o) * linv) +
                       texture(split_low, (pos + q) * linv) + texture(split_low, (pos - q) * linv));
#endif
#ifdef P11_TENSOR
    // P11 re_s1 stores (jxx, jyy, jxy, lap_e) per block in codes^2.
    vec4 J = texture(p11_tensor, pos * p11_scale);
#else
    vec4 tmask = vec4(1.0, NCOMP > 1 ? 1.0 : 0.0, NCOMP > 2 ? 1.0 : 0.0, 0.0);
    vec3 Jacc = vec3(0.0);
    float hSpan = 0.25 * tensor_px;
    for (int sy = -1; sy <= 1; sy += 2)
        for (int sx = -1; sx <= 1; sx += 2) {
            vec2 sp = pos + vec2(float(sx), float(sy)) * hSpan;
#ifdef P10_GRAIN
            vec4 gx = 0.5 * code_scale * tmask * (p10_base_at(sp + vec2(2.0, 0.0), hi, p10_cu) - p10_base_at(sp - vec2(2.0, 0.0), hi, p10_cu));
            vec4 gy = 0.5 * code_scale * tmask * (p10_base_at(sp + vec2(0.0, 2.0), hi, p10_cu) - p10_base_at(sp - vec2(0.0, 2.0), hi, p10_cu));
#else
            vec2 stepH = vec2(2.0, 0.0) * linv;
            vec2 stepV = vec2(0.0, 2.0) * linv;
            vec2 qpos = sp * linv;
            vec4 gx = 0.5 * code_scale * tmask * (texture(split_low, qpos + stepH) - texture(split_low, qpos - stepH));
            vec4 gy = 0.5 * code_scale * tmask * (texture(split_low, qpos + stepV) - texture(split_low, qpos - stepV));
#endif
            Jacc += vec3(dot(gx, gx), dot(gy, gy), dot(gx, gy));
        }
    vec4 J = vec4(0.25 * Jacc, 0.0);
#endif
    float jd = J.x - J.y;
    float coh = sqrt(jd * jd + 4.0 * J.z * J.z) / (J.x + J.y + coh_eps);
    float theta = (abs(jd) + abs(J.z) > 0.0) ? 0.5 * atan(2.0 * J.z, jd) : 0.0;
    vec2 along = vec2(-sin(theta), cos(theta)) * along_step;
    vec4 ra = vec4(0.0), ra2 = vec4(0.0);
    for (int k = -ALONG_N; k <= ALONG_N; k++) {
        vec2 sp = pos + float(k) * along;
#ifdef P10_GRAIN
        vec4 rv = vec4(texture(src, sp * inv).x, 0.0, 0.0, 0.0) - p10_base_at(sp, hi, p10_cu);
#else
        vec4 rv = texture(src, sp * inv) - texture(split_low, sp * linv);
#endif
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
    // vectors needed - the share of busy pixels per 16x16 block (the regions busy share:
    // Laplacian above bz_lap or a local extremum; a band is flat but for its edges, grain is busy
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
    if (write_w != 0)
        imageStore(w_out, p, vec4(w, 0.0, 0.0, 1.0));
}
)GLSL";

		// The spatial grain estimate per 16x16 block (one workgroup) is the owner's busy share
		// (input.source_analysis/regions, RegionEvidence::BusyShareHeaderGlsl / BusyShareBodyGlsl,
		// CPU oracle BusyEdge::BusyShare): the share of the block's busy pixels, Laplacian
		// |4c - n - s - e - w| (largest channel) above bz_lap, or a strict local extremum along x,
		// y or a diagonal by more than bz_ext on both sides (GrainPreserve::GuardLaplacianCodes /
		// GuardExtremumCodes; a quantised smooth gradient is monotone along every line: never).
		// The guard reads the debander's own input planes (any depth, float or a shifted
		// container), so the taps stay in the plane's normalised units (bz_scale 1) and the
		// thresholds are passed in those units: the comparisons are the ones the private copy made.

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
if (q.x < size.x && q.y < size.y)
    imageStore(dst, q, vpu_band_from_base_across(src, q));
)GLSL";
		const char* const SPLIT_V_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y)
    imageStore(dst, q, vpu_band_from_base_down(src, q));
)GLSL";
		const char* const P10_BASE_HELPER_GLSL = R"GLSL(
vec4 p10_base_at(vec2 sp, ivec2 hi, float cu)
{
    vec2 q = clamp(sp - vec2(0.5), vec2(0.0), vec2(hi));
    ivec2 i0 = ivec2(floor(q));
    ivec2 i1 = min(i0 + ivec2(1), hi);
    vec2 fr = q - vec2(i0);
    float a = gs_fetch(i0).w;
    float b = gs_fetch(ivec2(i1.x, i0.y)).w;
    float c0 = gs_fetch(ivec2(i0.x, i1.y)).w;
    float d0 = gs_fetch(i1).w;
    return vec4(mix(mix(a, b, fr.x), mix(c0, d0, fr.x), fr.y) * cu, 0.0, 0.0, 0.0);
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

	unsigned long long Module::BandMemoHits() const { return bandMemoHits; }
	unsigned long long Module::BandMemoFallbacks() const { return bandMemoFallbacks; }
	const std::string& Module::LastBandMemoFallback() const { return lastBandMemoFallback; }
	unsigned long long Module::GrainGuardHits() const { return grainGuardHits; }
	unsigned long long Module::GrainGuardFallbacks() const { return grainGuardFallbacks; }
	const std::string& Module::LastGrainGuardFallback() const { return lastGrainGuardFallback; }

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
		if (weightDummy && gpu) pl_tex_destroy(gpu, &weightDummy);
		weightDummy = nullptr;
		for (int s = 0; s < kSlots; ++s)
			for (int p = 0; p < PL_MAX_PLANES; ++p)
			{
				if (guard[s][p] && gpu) pl_tex_destroy(gpu, &guard[s][p]);
				guard[s][p] = nullptr;
			}
		for (int s = 0; s < kSlots; ++s)
			for (int p = 0; p < PL_MAX_PLANES; ++p)
				for (pl_tex* tex : { &splitHalf[s][p], &splitTmp[s][p], &splitLow[s][p] })
				{
					if (*tex && gpu) pl_tex_destroy(gpu, tex);
					*tex = nullptr;
				}
		weightSequence = 0;
		prevValid = false;
		lastFailureError.clear();
	}

	bool Module::SplitLowBand(pl_gpu gpu, pl_dispatch dispatch, pl_tex in, int slot, int plane, double scale,
		pl_tex acquiredLow, pl_tex& splitLowOut)
	{
		splitLowOut = nullptr;
		if (acquiredLow != nullptr)
		{
			splitLowOut = acquiredLow;
			return true;
		}
		const int hw = (in->params.w + 1) / 2, hh = (in->params.h + 1) / 2;
		const int n = in->params.format->num_components;
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
			!ensure(splitLow[slot][plane], hw, hh, FloatFormat(gpu, n, true)))
		{
			error = "no float texture for the grain split";
			lastFailureError = error;
			++failures;
			return false;
		}
		const int gx = (hw + 15) / 16, gy = (hh + 15) / 16;
		VpUnit::BandMemo::BandKey key;
		key.resolution = VpUnit::BandMemo::Stage::Half;
		key.plane = static_cast<uint8_t>(plane);
		key.width = static_cast<uint32_t>(hw);
		key.height = static_cast<uint32_t>(hh);
		key.sigmaPx = VpUnit::BandFromBase::kSigmaLow * scale;
		key.law = VpUnit::BandMemo::Law::Low;
		key.edge = VpUnit::BandFromBase::EdgeMode::Replicate;
		const std::string weights = VpUnit::BandMemo::GlslOf(key);
		const pl_shader_desc dh[2] = { Sampled("src", in, false), Storage("dst", splitHalf[slot][plane]) };
		const pl_shader_desc dx[2] = { Sampled("src", splitHalf[slot][plane], false), Storage("dst", splitTmp[slot][plane]) };
		const pl_shader_desc dy[2] = { Sampled("src", splitTmp[slot][plane], false), Storage("dst", splitLow[slot][plane]) };
		if (!Pass(dispatch, "VideoProcessor deband: grain split (half)", std::string(), SPLIT_HALF_BODY, dh, 2, nullptr, 0, gx, gy) ||
			!Pass(dispatch, "VideoProcessor deband: grain split (across)", weights, SPLIT_H_BODY, dx, 2, nullptr, 0, gx, gy) ||
			!Pass(dispatch, "VideoProcessor deband: grain split (down)", weights, SPLIT_V_BODY, dy, 2, nullptr, 0, gx, gy))
		{
			error = "the grain split passes failed";
			lastFailureError = error;
			++failures;
			return false;
		}
		splitLowOut = splitLow[slot][plane];
		return true;
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result, int slot)
	{
		error.clear();
		tdRead = TdProducts::ReadToday(TdProducts::Consumer::Deband);   // absent in reset 1: nothing below changes
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
			lastFailureError = error;
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
		bool anyHeld = false;   // a plane really read the previous weights (finding D5)
		for (int plane = 0; plane < source.num_planes; ++plane)
		{
			const pl_tex in = source.planes[plane].texture;
			if (!in || !in->params.sampleable)
			{
				error = "plane " + std::to_string(plane) + " is not sampleable";
				lastFailureError = error;
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
				lastFailureError = error;
				++failures;
				return false;
			}
			pl_tex weightOut = nullptr, weightIn = nullptr;
			if (mainFrame)
			{
				pl_tex_params wp{};
				wp.w = w;
				wp.h = h;
				wp.format = StorableLike(gpu, pl_find_named_fmt(gpu, "r32f"));
				wp.sampleable = true;
				wp.storable = true;
				if (!wp.format || !pl_tex_recreate(gpu, &weight[next][plane], &wp))
				{
					error = "no weight texture";
					lastFailureError = error;
					++failures;
					return false;
				}
				weightOut = weight[next][plane];
				if (hold && weight[readIndex][plane] && weightW[plane] == w && weightH[plane] == h)
					weightIn = weight[readIndex][plane];
			}
			else if (!weightDummy)
			{
				pl_tex_params dp{};
				dp.w = 1;
				dp.h = 1;
				dp.format = StorableLike(gpu, pl_find_named_fmt(gpu, "r32f"));
				dp.storable = true;
				if (!dp.format || !pl_tex_recreate(gpu, &weightDummy, &dp))
				{
					error = "no dummy weight texture";
					lastFailureError = error;
					++failures;
					return false;
				}
			}
			const bool useHold = weightIn != nullptr;
			anyHeld = anyHeld || useHold;
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
			VpSkeleton::AcquireResult memoBand, memoGrain, memoP11;
			pl_tex splitLowTex = nullptr;
			const GrainSplit::Gpu::Frame* p10Frame = nullptr;
			pl_tex p11TensorTex = nullptr;
			float p11Scale[2] = { 0.0f, 0.0f };
			float p10Cu = static_cast<float>(codeUnit);
			const float codeScale = static_cast<float>(1.0 / codeUnit);
			if (useSplit)
			{
				const int inComps = in->params.format->num_components;
				const bool ycbcr = pl_color_system_is_ycbcr_like(source.repr.sys);
				std::string guardFbReason;

				// 15b A3: acquire shared P10 (luma) / P10c (chroma) grain split and P11 structure tensor (re_s1).
				if (params.memo != nullptr && params.sequence != 0u && ycbcr)
				{
					if (inComps == 1 && plane <= 2)
					{
						const VpSkeleton::ProductId grainProd = (plane == 0) ? VpSkeleton::kP10GrainSplit : VpSkeleton::kP10cGrainSplitChroma;
						const VpSkeleton::VariantKey gKey = (plane == 0)
							? GrainSplit::Gpu::VariantFor(w, h, splitScale, params.colorDepth)
							: GrainSplit::Gpu::ChromaVariantFor(plane, w, h, splitScale, params.colorDepth);
						memoGrain = params.memo->Acquire(grainProd, params.sequence, VpSkeleton::AcquireForm::Gpu, gKey);
						if (memoGrain.Ok() && memoGrain.gpu.textureSet == nullptr && memoGrain.gpu.resource != nullptr)
						{
							const GrainSplit::Gpu::Frame* candidate = GrainSplit::Gpu::FrameOf(memoGrain);
							if (candidate != nullptr && candidate->split != nullptr && candidate->blocks != nullptr
								&& (candidate->storage != GrainSplit::Storage::Compact || candidate->splitB != nullptr)
								&& candidate->w == w && candidate->h == h && candidate->codeUnit > 0.0)
							{
								p10Frame = candidate;
								p10Cu = static_cast<float>(candidate->codeUnit);
							}
							else
							{
								guardFbReason = std::string(plane == 0 ? "kP10GrainSplit" : "kP10cGrainSplitChroma") + " geometry/frame mismatch";
								memoGrain = VpSkeleton::AcquireResult();
							}
						}
						else
						{
							const std::string detail = params.memo->LastCause(grainProd);
							guardFbReason = std::string(plane == 0 ? "kP10GrainSplit" : "kP10cGrainSplitChroma")
								+ " unavailable (" + VpSkeleton::Token(memoGrain.cause)
								+ (detail.empty() ? ")" : ": " + detail + ")");
							memoGrain = VpSkeleton::AcquireResult();
						}
					}
					else
					{
						guardFbReason = "multi-component/unsupported plane index (P10/P10c serves single-component Y/Cb/Cr planes)";
					}

					memoP11 = params.memo->Acquire(VpSkeleton::kP11RegionBlockStats, params.sequence, VpSkeleton::AcquireForm::Gpu);
					if (memoP11.Ok() && memoP11.gpu.Valid())
					{
						pl_tex s1 = nullptr;
						if (memoP11.gpu.textureSet != nullptr && memoP11.gpu.index >= 0)
						{
							const auto* set11 = static_cast<const VpLaneProducts::GpuProductTextures*>(memoP11.gpu.textureSet);
							s1 = memoP11.gpu.ringSlot
								? set11->Ring(static_cast<size_t>(memoP11.gpu.index), 1)
								: set11->Live(static_cast<size_t>(memoP11.gpu.index), 1);
						}
						else if (memoP11.gpu.view != nullptr)
							s1 = static_cast<pl_tex>(memoP11.gpu.view);
						else if (memoP11.gpu.resource != nullptr)
							s1 = static_cast<pl_tex>(memoP11.gpu.resource);

						if (s1 != nullptr && s1->params.sampleable && s1->params.w > 0 && s1->params.h > 0
							&& s1->params.format != nullptr && s1->params.format->num_components >= 3)
						{
							p11TensorTex = s1;
							const int p11Block = RegionEvidence::BlockSizeFor(luma->params.h);
							const double spanX = static_cast<double>(p11Block) * static_cast<double>(s1->params.w);
							const double spanY = static_cast<double>(p11Block) * static_cast<double>(s1->params.h);
							p11Scale[0] = static_cast<float>(static_cast<double>(luma->params.w) / (spanX * static_cast<double>((std::max)(1, w))));
							p11Scale[1] = static_cast<float>(static_cast<double>(luma->params.h) / (spanY * static_cast<double>((std::max)(1, h))));
						}
						else
						{
							const std::string p11Why = "P11 re_s1 texture missing/unusable";
							guardFbReason = guardFbReason.empty() ? p11Why : (guardFbReason + "; " + p11Why);
							memoP11 = VpSkeleton::AcquireResult();
						}
					}
					else
					{
						const std::string p11Why = std::string("P11 unavailable (") + VpSkeleton::Token(memoP11.cause) + ")";
						guardFbReason = guardFbReason.empty() ? p11Why : (guardFbReason + "; " + p11Why);
						memoP11 = VpSkeleton::AcquireResult();
					}
				}
				else
				{
					if (!ycbcr)
						guardFbReason = "RGB plane (P10/P10c/P11 serve YCbCr planes)";
					else if (params.memo == nullptr)
						guardFbReason = "no skeleton memo bound";
					else
						guardFbReason = "frame sequence 0 (unkeyed)";
				}

				if (p10Frame != nullptr && p11TensorTex != nullptr)
				{
					++grainGuardHits;
				}
				else
				{
					++grainGuardFallbacks;
					lastGrainGuardFallback = guardFbReason.empty() ? "shared P10/P11 grain guard unavailable" : guardFbReason;
				}

				// When P10/P10c is unavailable, fall back to shared kBandMemo (or BandMemo::GlslOf low-band pass).
				if (p10Frame == nullptr)
				{
					pl_tex acquiredLow = nullptr;
					const int hw = (in->params.w + 1) / 2, hh = (in->params.h + 1) / 2;
					if (plane == 0 && inComps == 1 && params.memo != nullptr && params.sequence != 0u && ycbcr)
					{
						VpUnit::BandMemo::BandKey key;
						key.resolution = VpUnit::BandMemo::Stage::Half;
						key.pipelineStage = 0;
						key.plane = 0;
						key.width = static_cast<uint32_t>(hw);
						key.height = static_cast<uint32_t>(hh);
						key.sigmaPx = VpUnit::BandFromBase::kSigmaLow * splitScale;
						key.law = VpUnit::BandMemo::Law::Low;
						key.edge = VpUnit::BandFromBase::EdgeMode::Replicate;
						memoBand = params.memo->Acquire(VpSkeleton::kBandMemo, params.sequence, VpSkeleton::AcquireForm::Gpu,
							VpUnit::BandMemo::ToVariant(key));
						if (memoBand.Ok() && memoBand.gpu.Valid() && memoBand.gpu.resource != nullptr)
						{
							pl_tex t = static_cast<pl_tex>(memoBand.gpu.resource);
							if (t->params.w == hw && t->params.h == hh && t->params.sampleable
								&& (t->params.format->caps & PL_FMT_CAP_LINEAR))
							{
								acquiredLow = t;
								++bandMemoHits;
							}
							else
							{
								++bandMemoFallbacks;
								lastBandMemoFallback = "kBandMemo texture layout/linear-cap mismatch";
								memoBand = VpSkeleton::AcquireResult();
							}
						}
						else
						{
							++bandMemoFallbacks;
							const std::string detail = params.memo->LastCause(VpSkeleton::kBandMemo);
							lastBandMemoFallback = std::string("kBandMemo unavailable (") + VpSkeleton::Token(memoBand.cause)
								+ (detail.empty() ? ")" : ": " + detail + ")");
						}
					}
					else
					{
						++bandMemoFallbacks;
						if (plane != 0 || inComps != 1 || !ycbcr)
							lastBandMemoFallback = "chroma/RGB/multi-component plane (band_memo serves YCbCr luma only)";
						else if (params.memo == nullptr)
							lastBandMemoFallback = "no skeleton memo bound";
						else
							lastBandMemoFallback = "frame sequence 0 (unkeyed)";
					}
					if (!SplitLowBand(gpu, dispatch, in, slot, plane, splitScale, acquiredLow, splitLowTex))
						return false;
				}
			}
			const float lapMin = static_cast<float>(GrainPreserve::GuardLaplacianCodes(params.thresholdCodes, params.colorDepth) * codeUnit);
			const float extMin = static_cast<float>(GrainPreserve::GuardExtremumCodes(params.thresholdCodes, params.colorDepth) * codeUnit);
			const float vetoLo = static_cast<float>(GrainPreserve::kVetoLo);
			const float vetoHi = static_cast<float>(GrainPreserve::kVetoHi);
			if (useGuard)
			{
				pl_tex_params gp{};
				gp.w = (w + 15) / 16;
				gp.h = (h + 15) / 16;
				gp.format = StorableLike(gpu, pl_find_named_fmt(gpu, "r32f"));
				gp.sampleable = true;
				gp.storable = true;
				gp.blit_src = gp.format && (gp.format->caps & PL_FMT_CAP_BLITTABLE) != 0;   // read back by the guard parity test where blittable
				if (!gp.format || !pl_tex_recreate(gpu, &guard[slot][plane], &gp))
				{
					error = "no grain guard texture";
					lastFailureError = error;
					++failures;
					return false;
				}
				const pl_shader_desc gd[2] = { Sampled("bz_src", in, false), Storage("bz_out", guard[slot][plane]) };
				// The owner kernel: one 16 x 16 workgroup per 16 px cell, the plane's components (bz_ncomp),
				// taps in the plane's normalised units (scale 1), thresholds in the same units.
				const int busyBlock = 16;
				const float busyScale = 1.0f;
				const pl_shader_var gv[5] = { { pl_var_int("bz_block"), &busyBlock, true }, { pl_var_int("bz_ncomp"), &ncomp, true },
					{ pl_var_float("bz_scale"), &busyScale, true }, { pl_var_float("bz_lap"), &lapMin, true }, { pl_var_float("bz_ext"), &extMin, true } };
				const std::string guardHeader = RegionEvidence::BusyShareHeaderGlsl();
				const std::string guardBody = RegionEvidence::BusyShareBodyGlsl();
				pl_custom_shader gc{};
				gc.description = "VideoProcessor deband: grain guard (regions busy share)";
				gc.header = guardHeader.c_str();
				gc.body = guardBody.c_str();
				gc.input = PL_SHADER_SIG_NONE;
				gc.output = PL_SHADER_SIG_NONE;
				gc.descriptors = gd;
				gc.num_descriptors = 2;
				gc.variables = gv;
				gc.num_variables = 5;
				gc.compute = true;
				gc.compute_shmem = sizeof(float) * 256 * 2;   // bz_sum
				gc.compute_group_size[0] = 16;
				gc.compute_group_size[1] = 16;
				pl_shader gsh = pl_dispatch_begin(dispatch);
				if (!pl_shader_custom(gsh, &gc))
				{
					pl_dispatch_abort(dispatch, &gsh);
					error = "the grain guard shader was refused";
					lastFailureError = error;
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
					lastFailureError = error;
					++failures;
					return false;
				}
			}

			const bool useP10 = useSplit && p10Frame != nullptr;
			const bool useP11 = useSplit && p11TensorTex != nullptr;
			std::vector<pl_shader_desc> d;
			d.reserve(12);
			d.push_back(Sampled("src", in, true));
			d.push_back(Storage("dst", out[slot][plane]));
			d.push_back(Storage("w_out", weightOut ? weightOut : weightDummy));
			if (useHold || useGrain) d.push_back(Sampled("field", params.field, false));
			if (useHold) d.push_back(Sampled("prev_w", weightIn, true));
			if (useGrain) d.push_back(Sampled("grain", params.grainError, false));
			if (useGuard) d.push_back(Sampled("grain_spatial", guard[slot][plane], true));
			if (useSplit)
			{
				if (useP10)
					GrainSplit::Gpu::BindView(*p10Frame, d);
				else
					d.push_back(Sampled("split_low", splitLowTex, true));
				if (useP11)
					d.push_back(Sampled("p11_tensor", p11TensorTex, (p11TensorTex->params.format->caps & PL_FMT_CAP_LINEAR) != 0));
			}
			const int writeW = weightOut ? 1 : 0;
			pl_shader_var v[24];
			int nv = 0;
			v[nv++] = { pl_var_float("thr"), &thr, true };
			v[nv++] = { pl_var_float("radius"), &radius, true };
			v[nv++] = { pl_var_int("write_w"), &writeW, true };
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
				if (useP10) v[nv++] = { pl_var_float("p10_cu"), &p10Cu, true };
				if (useP11)
				{
					v[nv++] = { pl_var_vec2("p11_scale"), p11Scale, true };
				}
				else
				{
					v[nv++] = { pl_var_float("tensor_px"), &tensorPx, true };
					v[nv++] = { pl_var_float("code_scale"), &codeScale, true };
				}
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
			if (useGuard) header += "#define SPATIAL_GUARD 1\n";
			if (useSplit)
			{
				header += "#define SPLIT 1\n" + AlongWeightsGlsl();
				if (useP10)
				{
					header += "#define P10_GRAIN 1\n" + GrainSplit::Gpu::ViewGlsl(p10Frame->storage, p10Frame->codeUnit) + P10_BASE_HELPER_GLSL;
				}
				if (useP11)
					header += "#define P11_TENSOR 1\n";
			}
			pl_custom_shader custom{};
			custom.description = useHold ? (useSplit ? "VideoProcessor deband, grain split (held along the vectors)" : "VideoProcessor deband (held along the vectors)") :
				(useSplit ? "VideoProcessor deband, grain split" : "VideoProcessor deband");
			custom.header = header.c_str();
			custom.body = BODY;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = d.data();
			custom.num_descriptors = static_cast<int>(d.size());
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
				lastFailureError = error;
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
				lastFailureError = error;
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
			if (anyHeld) ++held;
		}
		result = frame;
		return true;
	}
}
