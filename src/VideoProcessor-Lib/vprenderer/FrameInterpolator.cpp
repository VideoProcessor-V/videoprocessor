#include <pch.h>
#include "FrameInterpolator.h"

#include <libplacebo/shaders/custom.h>

#include <string>
#include <vector>

namespace Interpolator
{
	namespace
	{
		// One plane at phase t (see FrameInterpolator.h). Positions in this plane's
		// pixels; plane_scale converts them to source-luma pixels (the field's
		// space): (1, 1) for luma, the chroma subsampling for CbCr.
		const char* const INTERPOLATE_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec4 a0 = texelFetch(src_a, q, 0);
    vec4 b0 = texelFetch(src_b, q, 0);
    vec4 outv;
    if (phase <= 0.0) {
        outv = a0;
    } else if (phase >= 1.0) {
        outv = b0;
    } else if (texelFetch(frame_trust, ivec2(0), 0).r < min_trusted_share) {
        // A cut between A and B: the nearest frame whole (never a blend of two scenes).
        outv = phase < 0.5 ? a0 : b0;
    } else {
        vec2 lp = (vec2(q) + vec2(0.5)) * plane_scale;   // source-luma pixels
        vec2 v;
        if (field_vector(lp, v)) {
            // The vector of the content that is here at the phase (one fixed-point
            // step: at a moving edge the output position's own block is the wrong one).
            vec2 v1;
            if (field_vector(lp - phase * v, v1))
                v = v1;
            vec2 la = lp - phase * v;
            vec2 lb = lp + (1.0 - phase) * v;
            vec4 sa = bilerp_a(la / plane_scale);
            vec4 sb = bilerp_b(lb / plane_scale);
            float wb = phase;
#ifdef OCCLUSION
            // B's content where the B sample lands is new (not seen in A): not the
            // same content as the A sample - the A sample alone.
            ivec2 osize = textureSize(occ_b, 0);
            if (texelFetch(occ_b, clamp(ivec2(floor(lb / 16.0)), ivec2(0), osize - ivec2(1)), 0).r < 0.5)
                wb = 0.0;
#endif
            outv = mix(sa, sb, wb);
        } else {
#ifdef FALLBACK_BLEND
            outv = mix(a0, b0, phase);
#else
            outv = phase < 0.5 ? a0 : b0;   // hold the nearest frame's own pixel
#endif
#ifdef OCCLUSION
            // Disocclusion: no trusted A vector here, B's own block is content A did not have,
            // and the uncover test passes - a trusted neighbouring block moves away from this
            // position, by at least 2 source pixels at this phase (its content has left: the
            // background behind it is showing). Only B has that content, at any phase. A bad
            // match (new in B, but nothing moving away) keeps the fallback above.
            {
                ivec2 osize = textureSize(occ_b, 0);
                if (texelFetch(occ_b, clamp(ivec2(floor(lp / 16.0)), ivec2(0), osize - ivec2(1)), 0).r < 0.5) {
                    ivec2 fsize = textureSize(field, 0);
                    ivec2 own = clamp(ivec2(floor(lp / 16.0)), ivec2(0), fsize - ivec2(1));
                    bool uncovered = false;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            ivec2 c = own + ivec2(dx, dy);
                            if ((dx == 0 && dy == 0) || c.x < 0 || c.y < 0 || c.x >= fsize.x || c.y >= fsize.y)
                                continue;
                            vec4 f = texelFetch(field, c, 0);
                            vec2 centre = (vec2(c) + vec2(0.5)) * 16.0;
                            if (f.z > 0.5 && dot(f.xy, centre - lp) > 0.0 && phase * length(f.xy) >= 2.0)
                                uncovered = true;
                        }
                    if (uncovered)
                        outv = b0;
                }
            }
#endif
        }
    }
    imageStore(dst, q, outv);
}
)GLSL";

		// Bilinear sampling done in the shader from texelFetch (fp32 weights, edge clamped):
		// exact at a texel centre (a whole-pixel move or no motion gives the source codes
		// back) and full precision for 16-bit planes, which the hardware's filtering of
		// UNORM16 does not guarantee. Positions in this plane's pixels.
		const char* const BILERP_HEADER = R"GLSL(
#define DEFINE_BILERP(NAME, TEX) \
vec4 NAME(vec2 p) \
{ \
    ivec2 hi = textureSize(TEX, 0) - ivec2(1); \
    vec2 g = p - vec2(0.5); \
    ivec2 i0 = ivec2(floor(g)); \
    vec2 f = g - vec2(i0); \
    vec4 c00 = texelFetch(TEX, clamp(i0, ivec2(0), hi), 0); \
    vec4 c10 = texelFetch(TEX, clamp(i0 + ivec2(1, 0), ivec2(0), hi), 0); \
    vec4 c01 = texelFetch(TEX, clamp(i0 + ivec2(0, 1), ivec2(0), hi), 0); \
    vec4 c11 = texelFetch(TEX, clamp(i0 + ivec2(1, 1), ivec2(0), hi), 0); \
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y); \
}
DEFINE_BILERP(bilerp_a, src_a)
DEFINE_BILERP(bilerp_b, src_b)
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

		pl_shader_desc Storage(const char* name, pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_STORAGE_IMG;
			desc.desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc.binding.object = tex;
			return desc;
		}

		bool SameLayout(pl_tex a, pl_tex b)
		{
			return a && b && a->params.w == b->params.w && a->params.h == b->params.h &&
				a->params.format == b->params.format;
		}
	}

	FrameInterpolator::FrameInterpolator(pl_log logIn, pl_gpu gpuIn, bool hostReadableIn)
		: log(logIn), gpu(gpuIn), hostReadable(hostReadableIn)
	{
		dispatch = gpu ? pl_dispatch_create(log, gpu) : nullptr;
	}

	FrameInterpolator::~FrameInterpolator()
	{
		Release();
		if (dispatch)
			pl_dispatch_destroy(&dispatch);
	}

	void FrameInterpolator::Release()
	{
		if (gpu)
		{
			pl_tex_destroy(gpu, &outLuma);
			pl_tex_destroy(gpu, &outChroma);
			pl_tex_destroy(gpu, &noField);
			pl_tex_destroy(gpu, &noTrust);
		}
		outLuma = outChroma = noField = noTrust = nullptr;
	}

	bool FrameInterpolator::EnsureNoVectors()
	{
		if (noField && noTrust)
			return true;
		pl_fmt f32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
		if (!f32)
		{
			error = "no RGBA32F format for the no-vector fallback";
			return false;
		}
		const float untrusted[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // one block, not trusted
		const float notACut[4] = { 1.0f, 1.0f, 0.0f, 1.0f };     // the frame-trust test passes
		pl_tex_params params{};
		params.w = 1;
		params.h = 1;
		params.format = f32;
		params.sampleable = true;
		params.initial_data = untrusted;
		if (!noField) noField = pl_tex_create(gpu, &params);
		params.initial_data = notACut;
		if (!noTrust) noTrust = pl_tex_create(gpu, &params);
		if (!noField || !noTrust)
			error = "cannot create the no-vector fallback's textures";
		return noField && noTrust;
	}

	bool FrameInterpolator::Ensure(pl_tex& tex, pl_tex like)
	{
		if (tex && SameLayout(tex, like))
			return true;
		pl_tex_destroy(gpu, &tex);
		if (!(like->params.format->caps & PL_FMT_CAP_STORABLE))
		{
			error = std::string("plane format not storable: ") + like->params.format->name;
			return false;
		}
		pl_tex_params params{};
		params.w = like->params.w;
		params.h = like->params.h;
		params.format = like->params.format;
		params.sampleable = true;
		params.storable = true;
		params.blit_src = (like->params.format->caps & PL_FMT_CAP_BLITTABLE) != 0;
		params.host_readable = hostReadable && (like->params.format->caps & PL_FMT_CAP_HOST_READABLE);
		tex = pl_tex_create(gpu, &params);
		if (!tex)
			error = "cannot create the interpolated plane";
		return tex != nullptr;
	}

	bool FrameInterpolator::Plane(const char* description, pl_tex srcA, pl_tex srcB, pl_tex dst, const Motion& motion,
		float t, const float planeScale[2], const std::string& header)
	{
		// One texture bound twice in a pass is rejected by libplacebo, which reports it
		// only in its log (the pass then writes nothing): A and B the same texture (a
		// repeated frame, the identity test) bind it once, src_b an alias of src_a.
		const bool same = srcA == srcB;
		std::vector<pl_shader_desc> desc = { Sampled("src_a", srcA, false) };
		if (!same)
			desc.push_back(Sampled("src_b", srcB, false));
		desc.push_back(Sampled("field", motion.field, false));
		desc.push_back(Sampled("frame_trust", motion.frameTrust, false));
		desc.push_back(Storage("dst", dst));
		if (motion.subField)
			desc.push_back(Sampled("sub_field", motion.subField, false));
		if (motion.occlusionB)
			desc.push_back(Sampled("occ_b", motion.occlusionB, false));
		const float minShare = motion.minTrustedShare;
		const pl_shader_var vars[] = {
			{ pl_var_float("phase"), &t, true },
			{ pl_var_float("min_trusted_share"), &minShare, true },
			{ pl_var_vec2("plane_scale"), planeScale, true } };
		pl_custom_shader custom{};
		custom.description = description;
		const std::string fullHeader = std::string(same ? "#define src_b src_a\n" : "") + header;
		custom.header = fullHeader.c_str();
		custom.body = INTERPOLATE_BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc.data();
		custom.num_descriptors = static_cast<int>(desc.size());
		custom.variables = vars;
		custom.num_variables = 3;
		custom.compute = true;
		custom.compute_group_size[0] = 16;
		custom.compute_group_size[1] = 16;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			error = std::string("shader rejected: ") + description;
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (dst->params.w + 15) / 16;
		compute.dispatch_size[1] = (dst->params.h + 15) / 16;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(dispatch, &compute))
		{
			error = std::string("dispatch failed: ") + description;
			return false;
		}
		return true;
	}

	bool FrameInterpolator::Run(const Dejudder::Planes& a, const Dejudder::Planes& b, const Motion& motion, float t,
		InterpolationPolicy::Fallback fallback, Dejudder::Planes& out)
	{
		error.clear();
		out = a;
		if (!(t > 0.0f))
		{
			++passthroughs;
			return true;   // A exactly
		}
		if (!(t < 1.0f))
		{
			out = b;
			++passthroughs;
			return true;   // B exactly
		}
		if (!gpu || !dispatch)
		{
			error = "no GPU";
			return false;
		}
		Motion use = motion;
		if (!use.field || !use.frameTrust)
		{
			// No valid vectors: every position untrusted (the fallback), not a cut.
			if (!EnsureNoVectors())
				return false;
			use = Motion();
			use.field = noField;
			use.frameTrust = noTrust;
			use.minTrustedShare = motion.minTrustedShare;
		}
		if (!SameLayout(a.luma, b.luma) || ((a.chroma || b.chroma) && !SameLayout(a.chroma, b.chroma)))
		{
			error = "A and B differ in size or format";
			return false;
		}
		if (!Ensure(outLuma, a.luma) || (a.chroma && !Ensure(outChroma, a.chroma)))
			return false;
		const std::string header = std::string(fallback == InterpolationPolicy::Fallback::Blend ? "#define FALLBACK_BLEND 1\n" : "") +
			(use.occlusionB ? "#define OCCLUSION 1\n" : "") +
			(use.subField ? Dejudder::FieldVectorGlslSplit() : Dejudder::FieldVectorGlsl()) + BILERP_HEADER;
		const float lumaScale[2] = { 1.0f, 1.0f };
		if (!Plane("VideoProcessor interpolator: luma", a.luma, b.luma, outLuma, use, t, lumaScale, header))
			return false;
		out.luma = outLuma;
		if (a.chroma)
		{
			const float chromaScale[2] = {
				static_cast<float>(1 << a.chromaShiftX), static_cast<float>(1 << a.chromaShiftY) };
			if (!Plane("VideoProcessor interpolator: chroma", a.chroma, b.chroma, outChroma, use, t, chromaScale, header))
				return false;
			out.chroma = outChroma;
		}
		++runs;
		return true;
	}
}
