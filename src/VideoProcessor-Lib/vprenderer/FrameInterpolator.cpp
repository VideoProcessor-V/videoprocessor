#include <pch.h>
#include "FrameInterpolator.h"

#include <vprenderer/GrainPreserve.h>

#include <libplacebo/shaders/custom.h>

#include <cstdio>
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

		// interpolation_grain preserve (owner 2026-10-17; docs/audit/grain-audit-2026-10-17.md).
		// The same decisions as INTERPOLATE_BODY (cut, trusted warp, occlusion, fallback,
		// disocclusion), but a synthesised pixel is
		//     low band:  the warped A / B low bands (low_a / low_b, bilinear) mixed by phase
		//                (wb; 0 where B's landing is occluded, as today);
		//     high band: the frame the low band leans on (the nearer frame; A where wb is
		//                forced to 0) sampled with Catmull-Rom, minus its low band there,
		//                times one gain per 16x16 output block (this workgroup) and channel
		//                that matches the block's high-band energy to the source frame's own
		//                for the same pixels (src - low squared at the texel nearest each
		//                sample: at a whole-pixel position the two sums are the same numbers
		//                and the gain is exactly 1) plus the grain the low-band blend loses
		//                (wb (1 - wb) (la - lb)^2, at most 4x the pixel's own), clamped to [1, 2].
		// The grain is thus one frame's at full strength at every phase and everywhere in the
		// picture (an occluded pixel's A band is energy-matched like the rest), and two
		// consecutive outputs share grain only where a hold would. The fallback blend
		// (untrusted, interpolation_fallback blend) splits the same way at the pixel itself.
		// Pixels that are a source pixel (the ends, a cut, the hold fallback, a disocclusion)
		// stay exact and take no part in the block's gain.
		const char* const PRESERVE_HEADER = R"GLSL(
shared vec4 gp_hf[256];
shared vec4 gp_tg[256];
shared float gp_n[256];
#define DEFINE_LOWBI(NAME, TEX) \
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
DEFINE_LOWBI(low_half_a, low_a)
DEFINE_LOWBI(low_half_b, low_b)
// The low band at p (this plane's pixels): the half-resolution band at p / 2.
#define LOW_A(P) low_half_a((P) * 0.5)
#define LOW_B(P) low_half_b((P) * 0.5)
DEFINE_CATMULL(cat_a, src_a)
DEFINE_CATMULL(cat_b, src_b)
// The source frame's own high band at the texel nearest p, squared (the energy target).
#define HF2_AT(SRC, LOWF, P) gp_sq(texelFetch(SRC, clamp(ivec2(floor(P)), ivec2(0), textureSize(SRC, 0) - ivec2(1)), 0) - \
                               LOWF(floor(P) + vec2(0.5)))
vec4 gp_sq(vec4 x) { return x * x; }
)GLSL";

		const char* const PRESERVE_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
uint li = gl_LocalInvocationIndex;
ivec2 size = imageSize(dst);
bool inside = q.x < size.x && q.y < size.y;
vec4 outv = vec4(0.0);
vec4 lo = vec4(0.0);
vec4 hf = vec4(0.0);
vec4 tg = vec4(0.0);
bool synth = false;
if (inside) {
    vec4 a0 = texelFetch(src_a, q, 0);
    vec4 b0 = texelFetch(src_b, q, 0);
    if (phase <= 0.0) {
        outv = a0;
    } else if (phase >= 1.0) {
        outv = b0;
    } else if (texelFetch(frame_trust, ivec2(0), 0).r < min_trusted_share) {
        outv = phase < 0.5 ? a0 : b0;
    } else {
        vec2 lp = (vec2(q) + vec2(0.5)) * plane_scale;
        vec2 v;
        if (field_vector(lp, v)) {
            vec2 v1;
            if (field_vector(lp - phase * v, v1))
                v = v1;
            vec2 pa = (lp - phase * v) / plane_scale;
            vec2 pb = (lp + (1.0 - phase) * v) / plane_scale;
            float wb = phase;
#ifdef OCCLUSION
            vec2 lb = lp + (1.0 - phase) * v;
            ivec2 osize = textureSize(occ_b, 0);
            if (texelFetch(occ_b, clamp(ivec2(floor(lb / 16.0)), ivec2(0), osize - ivec2(1)), 0).r < 0.5)
                wb = 0.0;
#endif
            vec4 la = LOW_A(pa);
            vec4 lbv = LOW_B(pb);
            lo = mix(la, lbv, wb);
            if (wb >= 0.5) {
                hf = cat_b(pb) - lbv;
                tg = HF2_AT(src_b, LOW_B, pb);
            } else {
                hf = cat_a(pa) - la;
                tg = HF2_AT(src_a, LOW_A, pa);
            }
            // The grain the low-band blend loses (A's and B's are independent: the
            // difference of the warped low bands is theirs; at most 4x the pixel's own
            // high-band energy, so a vector error is not read as grain) is made up in
            // the high band's target.
            vec4 dl = la - lbv;
            tg += wb * (1.0 - wb) * min(dl * dl, 4.0 * tg);
            synth = true;
        } else {
#ifdef FALLBACK_BLEND
            vec4 la = LOW_A(vec2(q) + vec2(0.5));
            vec4 lbv = LOW_B(vec2(q) + vec2(0.5));
            lo = mix(la, lbv, phase);
            hf = phase < 0.5 ? a0 - la : b0 - lbv;
            tg = hf * hf;
            synth = true;
#else
            outv = phase < 0.5 ? a0 : b0;
#endif
#ifdef OCCLUSION
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
                    if (uncovered) {
                        outv = b0;
                        synth = false;
                    }
                }
            }
#endif
        }
    }
}
gp_hf[li] = synth ? hf * hf : vec4(0.0);
gp_tg[li] = synth ? tg : vec4(0.0);
gp_n[li] = synth ? 1.0 : 0.0;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s) {
        gp_hf[li] += gp_hf[li + s];
        gp_tg[li] += gp_tg[li + s];
        gp_n[li] += gp_n[li + s];
    }
    barrier();
}
if (inside) {
    if (synth) {
        vec4 g = vec4(1.0);
        if (gp_n[0] >= gain_min_pixels) {
            vec4 sh = gp_hf[0];
            vec4 st = gp_tg[0];
            for (int c = 0; c < 4; c++)
                if (sh[c] > 1e-12)
                    g[c] = clamp(sqrt(st[c] / sh[c]), gain_lo, gain_hi);
        }
        outv = lo + g * hf;
    }
    imageStore(dst, q, outv);
}
)GLSL";

		// The low band at half resolution (GrainPreserve): a 2x2 box average, then the
		// Gaussian one axis per pass.
		const char* const LOW_HALF_BODY = R"GLSL(
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

		const char* const LOW_H_BODY = R"GLSL(
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

		const char* const LOW_V_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(tmp, 0) - ivec2(1);
    vec4 acc = vec4(0.0);
    for (int i = -GP_R; i <= GP_R; i++)
        acc += gp_w(i) * texelFetch(tmp, clamp(q + ivec2(0, i), ivec2(0), hi), 0);
    imageStore(dst, q, acc);
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

		// gp_w(i), i in -GP_R..GP_R (GrainPreserve::kLowRadius = 5: -5..5): GrainPreserve's
		// low-band weights.
		const std::string& LowWeightsGlsl()
		{
			static const std::string text = []()
			{
				const std::string taps = std::to_string(2 * GrainPreserve::kLowRadius + 1);
				std::string s = "#define GP_R " + std::to_string(GrainPreserve::kLowRadius) + "\nfloat gp_w(int i)\n{\n    float w[" +
					taps + "] = float[" + taps + "](";
				for (int i = -GrainPreserve::kLowRadius; i <= GrainPreserve::kLowRadius; ++i)
				{
					char number[32];
					snprintf(number, sizeof(number), "%.9g", GrainPreserve::LowWeight(i));
					s += number;
					if (i < GrainPreserve::kLowRadius)
						s += ", ";
				}
				return s + ");\n    return w[i + GP_R];\n}\n";
			}();
			return text;
		}

		bool Compute(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
			const std::vector<pl_shader_desc>& desc, const pl_shader_var* vars, int numVars, int groupsX, int groupsY)
		{
			pl_custom_shader custom{};
			custom.description = description;
			custom.header = header.c_str();
			custom.body = body;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc.data();
			custom.num_descriptors = static_cast<int>(desc.size());
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
			for (GrainBands& bands : grainBands)
				for (pl_tex* tex : { &bands.half, &bands.tmp, &bands.lowA, &bands.lowB })
					pl_tex_destroy(gpu, tex);
		}
		outLuma = outChroma = noField = noTrust = nullptr;
		for (GrainBands& bands : grainBands)
			bands = GrainBands();
	}

	bool FrameInterpolator::EnsureFloat(pl_tex& tex, int w, int h, int components)
	{
		if (tex && tex->params.w == w && tex->params.h == h && tex->params.format->num_components == components)
			return true;
		pl_tex_destroy(gpu, &tex);
		const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
		pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, components, 32, 32, caps);
		if (!fmt)
			fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);   // wider is fine (unused channels)
		if (!fmt)
		{
			error = "no storable 32-bit float format for the grain bands";
			return false;
		}
		pl_tex_params params{};
		params.w = w;
		params.h = h;
		params.format = fmt;
		params.sampleable = true;
		params.storable = true;
		params.host_readable = hostReadable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
		tex = pl_tex_create(gpu, &params);
		if (!tex)
			error = "cannot create a grain band texture";
		return tex != nullptr;
	}

	bool FrameInterpolator::LowBand(const char* description, pl_tex src, pl_tex half, pl_tex tmp, pl_tex low)
	{
		const int w = half->params.w, h = half->params.h;
		const int gx = (w + 15) / 16, gy = (h + 15) / 16;
		const std::string header = LowWeightsGlsl();
		const std::vector<pl_shader_desc> passHalf = { Sampled("src", src, false), Storage("dst", half) };
		if (!Compute(dispatch, description, std::string(), LOW_HALF_BODY, passHalf, nullptr, 0, gx, gy))
		{
			error = std::string("grain low band (half) failed: ") + description;
			return false;
		}
		const std::vector<pl_shader_desc> passH = { Sampled("src", half, false), Storage("dst", tmp) };
		if (!Compute(dispatch, description, header, LOW_H_BODY, passH, nullptr, 0, gx, gy))
		{
			error = std::string("grain low band (across) failed: ") + description;
			return false;
		}
		const std::vector<pl_shader_desc> passV = { Sampled("tmp", tmp, false), Storage("dst", low) };
		if (!Compute(dispatch, description, header, LOW_V_BODY, passV, nullptr, 0, gx, gy))
		{
			error = std::string("grain low band (down) failed: ") + description;
			return false;
		}
		return true;
	}

	bool FrameInterpolator::PrepareBands(int index, pl_tex srcA, pl_tex srcB)
	{
		GrainBands& bands = grainBands[index];
		const int w = (srcA->params.w + 1) / 2, h = (srcA->params.h + 1) / 2, n = srcA->params.format->num_components;
		if (!EnsureFloat(bands.half, w, h, n) || !EnsureFloat(bands.tmp, w, h, n) || !EnsureFloat(bands.lowA, w, h, n) ||
			(srcB != srcA && !EnsureFloat(bands.lowB, w, h, n)))
			return false;
		if (!LowBand("VideoProcessor interpolator: low band A", srcA, bands.half, bands.tmp, bands.lowA))
			return false;
		return srcB == srcA || LowBand("VideoProcessor interpolator: low band B", srcB, bands.half, bands.tmp, bands.lowB);
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
		float t, const float planeScale[2], const std::string& header, const GrainBands* bands)
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
		// interpolation_grain preserve: the bands of A and B (aliased like the planes).
		if (bands)
		{
			desc.push_back(Sampled("low_a", bands->lowA, false));
			if (!same)
				desc.push_back(Sampled("low_b", bands->lowB, false));
		}
		const float minShare = motion.minTrustedShare;
		const float gainLo = static_cast<float>(GrainPreserve::kGainMin);
		const float gainHi = static_cast<float>(GrainPreserve::kGainMax);
		const float gainMinPixels = static_cast<float>(GrainPreserve::kGainMinPixels);
		const pl_shader_var vars[] = {
			{ pl_var_float("phase"), &t, true },
			{ pl_var_float("min_trusted_share"), &minShare, true },
			{ pl_var_vec2("plane_scale"), planeScale, true },
			{ pl_var_float("gain_lo"), &gainLo, true },
			{ pl_var_float("gain_hi"), &gainHi, true },
			{ pl_var_float("gain_min_pixels"), &gainMinPixels, true } };
		pl_custom_shader custom{};
		custom.description = description;
		const std::string fullHeader = std::string(same ? "#define src_b src_a\n" : "") +
			(bands && same ? "#define low_b low_a\n" : "") + header +
			(bands ? std::string(GrainPreserve::CatmullGlsl()) + PRESERVE_HEADER : std::string());
		custom.header = fullHeader.c_str();
		custom.body = bands ? PRESERVE_BODY : INTERPOLATE_BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc.data();
		custom.num_descriptors = static_cast<int>(desc.size());
		custom.variables = vars;
		custom.num_variables = bands ? 6 : 3;
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
		// interpolation_grain preserve: each plane's low band of A and B
		// first (blend: today's pass alone, byte for byte).
		const bool preserve = grain == InterpolationPolicy::Grain::Preserve;
		const float lumaScale[2] = { 1.0f, 1.0f };
		if (preserve && !PrepareBands(0, a.luma, b.luma))
			return false;
		if (!Plane(preserve ? "VideoProcessor interpolator: luma (grain preserved)" : "VideoProcessor interpolator: luma",
			a.luma, b.luma, outLuma, use, t, lumaScale, header, preserve ? &grainBands[0] : nullptr))
			return false;
		out.luma = outLuma;
		if (a.chroma)
		{
			const float chromaScale[2] = {
				static_cast<float>(1 << a.chromaShiftX), static_cast<float>(1 << a.chromaShiftY) };
			if (preserve && !PrepareBands(1, a.chroma, b.chroma))
				return false;
			if (!Plane(preserve ? "VideoProcessor interpolator: chroma (grain preserved)" : "VideoProcessor interpolator: chroma",
				a.chroma, b.chroma, outChroma, use, t, chromaScale, header, preserve ? &grainBands[1] : nullptr))
				return false;
			out.chroma = outChroma;
		}
		++runs;
		if (preserve) ++grainPreservedRuns;
		return true;
	}
}
