#include <pch.h>
#include "DetailPulse.h"

#include <vprenderer/GrainPreserve.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// detail_pulse_stabilise (DetailPulse.h has the maths). Three compute passes per new source
// frame, two of them at the plane's resolution:
//  1. measure (one 16 x 16 workgroup per 32 x 32 tile): the plane's codes around the tile and
//     the 2x2 boxes around its half-resolution block go into shared memory; the fine Gaussian
//     and the wide half-resolution low band are done there (separable); the tile's band
//     energies, low-band mean and spread, untrusted share and veto weight are written, and
//     its 16 x 16 half-resolution low band (the apply pass reads it back).
//  2. reduce (one workgroup): the frame's energies from the tiles, the cut and fade checks,
//     the reference from the ring, the gains; the frame's entry into the ring.
//  3. apply (one workgroup per 32 x 32 block): the bands again (shared memory), the gains
//     times the tile weight (bilinear between tile centres), added to the source sample.
// A cadence repeat runs the apply pass alone, with the stored decision. The CPU keeps only
// which ring slot is whose and how many consecutive frames stand behind it; every value
// lives on the GPU (no readback).
namespace DetailPulse
{
	namespace
	{
		// Shared by the measure and apply passes: SW x SW codes around the 32 x 32 block
		// (fine band), RB x RB half-resolution boxes around its 16 x 16 half block and the
		// separable low band on them, HL x HL low-band texels (the half block and a one-texel
		// border: the bilinear read-back of every pixel of the block).
		const char* const BAND_HEADER = R"GLSL(
#define BLK 32
#define HB 16
#define SW (BLK + 2 * R1)
#define HL (HB + 2)
#define RB (HL + 2 * GP_R)
shared float sy[SW * SW];
shared float sh[SW * BLK];
shared float slow[HL * HL];
shared float rd[1024];

// The fine band's vertical half on this block's pixel o (sh holds the horizontal half).
float fine_at(ivec2 o)
{
    float a = 0.0;
    for (int k = -R1; k <= R1; k++)
        a += fw(k) * sh[(o.y + R1 + k) * BLK + o.x];
    return a;
}

// The low band at the block's pixel o: bilinear in slow at (o + 0.5) / 2 (GrainModel::LowAt).
float low_at(ivec2 o)
{
    vec2 g = (vec2(o) + 0.5) * 0.5 - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    ivec2 j = i0 + ivec2(1);
    float top = slow[j.y * HL + j.x] * (1.0 - f.x) + slow[j.y * HL + j.x + 1] * f.x;
    float bottom = slow[(j.y + 1) * HL + j.x] * (1.0 - f.x) + slow[(j.y + 1) * HL + j.x + 1] * f.x;
    return top * (1.0 - f.y) + bottom * f.y;
}

// The sum over the workgroup (every invocation gets it).
vec4 reduce4(vec4 v)
{
    uint li = gl_LocalInvocationIndex;
    barrier();
    rd[li * 4u] = v.x;
    rd[li * 4u + 1u] = v.y;
    rd[li * 4u + 2u] = v.z;
    rd[li * 4u + 3u] = v.w;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) {
            rd[li * 4u] += rd[(li + s) * 4u];
            rd[li * 4u + 1u] += rd[(li + s) * 4u + 1u];
            rd[li * 4u + 2u] += rd[(li + s) * 4u + 2u];
            rd[li * 4u + 3u] += rd[(li + s) * 4u + 3u];
        }
        barrier();
    }
    return vec4(rd[0], rd[1], rd[2], rd[3]);
}

// The block's codes (and their horizontal fine half) into shared memory.
void load_block(ivec2 origin, ivec2 hi)
{
    uint li = gl_LocalInvocationIndex;
    for (int i = int(li); i < SW * SW; i += 256)
        sy[i] = code_scale * texelFetch(src, clamp(origin - ivec2(R1) + ivec2(i % SW, i / SW), ivec2(0), hi), 0).r;
    barrier();
    for (int i = int(li); i < SW * BLK; i += 256) {
        int r = i / BLK, c = i % BLK;
        float a = 0.0;
        for (int k = -R1; k <= R1; k++)
            a += fw(k) * sy[r * SW + c + R1 + k];
        sh[i] = a;
    }
}
)GLSL";

		// Measure only: the half-resolution boxes and the low band's horizontal half.
		const char* const MEASURE_HEADER = R"GLSL(
shared float sbox[RB * RB];
shared float shor[RB * HL];
)GLSL";

		const char* const MEASURE_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 hsz = (sz + ivec2(1)) / 2;
ivec2 hhi = hsz - ivec2(1);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 origin = blk * BLK;
ivec2 qb = blk * HB;
// The 2x2 boxes at the clamped half-resolution positions around the half block
// (GrainModel::Low: box, then the separable Gaussian, edges clamped at each step).
ivec2 ob = qb - ivec2(1 + GP_R);
for (int i = int(li); i < RB * RB; i += 256) {
    ivec2 b = clamp(ob + ivec2(i % RB, i / RB), ivec2(0), hhi) * 2;
    sbox[i] = 0.25 * code_scale * (texelFetch(src, clamp(b, ivec2(0), hi), 0).r + texelFetch(src, clamp(b + ivec2(1, 0), ivec2(0), hi), 0).r +
                                   texelFetch(src, clamp(b + ivec2(0, 1), ivec2(0), hi), 0).r + texelFetch(src, clamp(b + ivec2(1, 1), ivec2(0), hi), 0).r);
}
load_block(origin, hi);
for (int i = int(li); i < RB * HL; i += 256) {
    int r = i / HL, c = i % HL;
    int x = clamp(qb.x - 1 + c, 0, hhi.x) - ob.x;
    float a = 0.0;
    for (int k = -GP_R; k <= GP_R; k++)
        a += gp_w(k) * sbox[r * RB + x + k];
    shor[i] = a;
}
barrier();
for (int i = int(li); i < HL * HL; i += 256) {
    int r = i / HL, c = i % HL;
    int y = clamp(qb.y - 1 + r, 0, hhi.y) - ob.y;
    float a = 0.0;
    for (int k = -GP_R; k <= GP_R; k++)
        a += gp_w(k) * shor[(y + k) * HL + c];
    slow[i] = a;
}
barrier();
// The bands' energies over this invocation's 2 x 2 pixels.
vec4 acc = vec4(0.0);   // D1^2, D2^2, pixels, untrusted pixels
for (int q = 0; q < 4; q++) {
    ivec2 o = lid * 2 + ivec2(q & 1, q >> 1);
    ivec2 p = origin + o;
    if (p.x < sz.x && p.y < sz.y) {
        float g1 = fine_at(o);
        float d1 = sy[(o.y + R1) * SW + o.x + R1] - g1;
        float d2 = g1 - low_at(o);
        acc += vec4(d1 * d1, d2 * d2, 1.0, 0.0);
    }
}
#ifdef FIELD
// The de-judder's block under the centre of this invocation's 2 x 2 cell (by its pixels).
if (acc.z > 0.0) {
    vec2 lp = vec2(origin + lid * 2 + ivec2(1)) * to_field;
    ivec2 fs = textureSize(field, 0);
    vec4 f = texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fs - ivec2(1)), 0);
    if (f.z <= trust_min)
        acc.w = acc.z;
}
#endif
// The low band over the half block: mean and spread (about the block's centre texel).
ivec2 hl = ivec2(int(li % 16u), int(li / 16u));
ivec2 hq = qb + hl;
float shift = slow[9 * HL + 9];
vec4 hacc = vec4(0.0);
if (hq.x < hsz.x && hq.y < hsz.y) {
    float l = slow[(hl.y + 1) * HL + hl.x + 1];
    hacc = vec4(l - shift, (l - shift) * (l - shift), 1.0, 0.0);
#ifndef AHEAD
    imageStore(half_out, hq, vec4(l, 0.0, 0.0, 1.0));
#endif
}
vec4 tot = reduce4(acc);
vec4 htot = reduce4(hacc);
if (li == 0u) {
    float n = max(tot.z, 1.0);
    float hn = max(htot.z, 1.0);
    float hm = htot.x / hn;
    float mean_l = shift + hm;
    float spread = sqrt(max(htot.y / hn - hm * hm, 0.0));
    imageStore(stats_out, blk, vec4(tot.x / n, tot.y / n, mean_l, spread));
    float u = tot.w / n;
    float change = 0.0;
    float w = 1.0;
    float measurable = 1.0;
#ifdef PREV
    vec4 pv = texelFetch(prev_stats, blk, 0);
    change = max(abs(mean_l - pv.z), abs(spread - pv.w));
    // A tile whose band energy jumped (either way) since the previous frame shows new content
    // (an edge moving in, a flash): left out of the frame's energy.
    float en = (tot.x + tot.y) / n, ep = pv.x + pv.y;
    if (en > energy_jump * ep || ep > energy_jump * en)
        measurable = 0.0;
#endif
#ifdef FIELD
    w = 1.0 - smoothstep(0.0, untrusted_share, u);
#elif defined(PREV)
    w = 1.0 - smoothstep(veto_codes, 2.0 * veto_codes, change);
#endif
    imageStore(weight_out, blk, vec4(w, u, change, measurable));
}
)GLSL";

		// One workgroup: the frame from the tiles, then (thread 0) the decision.
		const char* const REDUCE_HEADER = R"GLSL(
shared float rd[1024];
vec4 reduce4(vec4 v)
{
    uint li = gl_LocalInvocationIndex;
    barrier();
    rd[li * 4u] = v.x;
    rd[li * 4u + 1u] = v.y;
    rd[li * 4u + 2u] = v.z;
    rd[li * 4u + 3u] = v.w;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) {
            rd[li * 4u] += rd[(li + s) * 4u];
            rd[li * 4u + 1u] += rd[(li + s) * 4u + 1u];
            rd[li * 4u + 2u] += rd[(li + s) * 4u + 2u];
            rd[li * 4u + 3u] += rd[(li + s) * 4u + 3u];
        }
        barrier();
    }
    return vec4(rd[0], rd[1], rd[2], rd[3]);
}
float gain_law(float r, float e)
{
    if (!(e > 0.0))
        return 1.0;
    float q = r / e;
    if (q <= 1.0 + dead_zone)
        return 1.0;
    return clamp(sqrt(q / (1.0 + dead_zone)), 1.0, cap);
}
)GLSL";

		const char* const REDUCE_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 nt = textureSize(stats_in, 0);
int n = nt.x * nt.y;
vec4 a1 = vec4(0.0), a2 = vec4(0.0);
for (int i = int(li); i < n; i += 256) {
    ivec2 t = ivec2(i % nt.x, i / nt.x);
    vec4 s = texelFetch(stats_in, t, 0);
    vec4 b = texelFetch(weight_in, t, 0);
    if (s.x + s.y >= floor_energy && b.x >= 0.5 && b.w > 0.5)
        a1 += vec4(s.x, s.y, 1.0, 0.0);
    a2 += vec4(s.z, b.z > cut_codes ? 1.0 : 0.0, b.y > 0.5 ? 1.0 : 0.0, 1.0);
}
vec4 t1 = reduce4(a1);
vec4 t2 = reduce4(a2);
float m1 = t1.x / max(t1.z, 1.0), m2 = t1.y / max(t1.z, 1.0);
vec4 a3 = vec4(0.0);
for (int i = int(li); i < n; i += 256) {
    ivec2 t = ivec2(i % nt.x, i / nt.x);
    vec4 s = texelFetch(stats_in, t, 0);
    vec4 b = texelFetch(weight_in, t, 0);
    if (s.x + s.y >= floor_energy && b.x >= 0.5 && b.w > 0.5)
        a3 += vec4(min(s.x, winsorise * m1), min(s.y, winsorise * m2), 0.0, 0.0);
}
vec4 t3 = reduce4(a3);
if (li == 0u) {
    bool measured = t1.z > 0.0;
    float e1 = measured ? t3.x / t1.z : 0.0;
    float e2 = measured ? t3.y / t1.z : 0.0;
    float my = t2.x / max(t2.w, 1.0);
    float cut_share = t2.y / max(t2.w, 1.0);
    float u_share = t2.z / max(t2.w, 1.0);
#ifdef AHEAD
    imageStore(ring, ivec2(RING + 1, 0), vec4(e1, e2, my, 1.0));
#else
    bool cut = n_valid > 0 && cut_share > cut_share_min;
#ifdef FIELD
    cut = cut && u_share > 0.5;
#endif
    bool fade = false;
    if (n_valid >= 2) {
        float p1 = imageLoad(ring, ivec2((cur + RING - 1) % RING, 0)).z;
        float p2 = imageLoad(ring, ivec2((cur + RING - 2) % RING, 0)).z;
        float d0 = my - p1, d1 = p1 - p2;
        fade = abs(d0) >= fade_codes && abs(d1) >= fade_codes && d0 * d1 > 0.0;
    }
    bool restart = n_valid == 0 || cut || fade;
    vec2 e = vec2(e1, e2);
    vec2 r = e;
    if (!restart) {
        float dec = 1.0;
        for (int j = 1; j <= n_use; j++) {
            vec4 s = imageLoad(ring, ivec2((cur + RING - j) % RING, 0));
            dec *= decay;
            if (s.w < 0.5)
                break;
            r = max(r, s.xy * dec);
            if (s.w > 1.5)
                break;
        }
        if (ahead_valid > 0) {
            vec4 a = imageLoad(ring, ivec2(RING + 1, 0));
            float ra = (a.x + a.y) / max(e1 + e2, 1e-20);
            if (abs(a.z - my) <= cut_codes && ra <= ahead_ratio && ra * ahead_ratio >= 1.0)
                r = max(r, a.xy * decay);
        }
    }
    vec2 g = vec2(1.0);
    if (!restart && measured)
        g = vec2(gain_law(r.x, e.x), gain_law(r.y, e.y));
    imageStore(ring, ivec2(cur, 0), vec4(e1, e2, my, restart ? 2.0 : 1.0));
    imageStore(ring, ivec2(RING, 0), vec4(g, restart ? 1.0 : 0.0, cut_share));
#endif
}
)GLSL";

		const char* const APPLY_HEADER = R"GLSL(
shared float stile[9];
)GLSL";

		const char* const APPLY_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 hhi = textureSize(half_in, 0) - ivec2(1);
ivec2 nt = textureSize(weight_in, 0);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 origin = blk * BLK;
ivec2 qb = blk * HB;
for (int i = int(li); i < HL * HL; i += 256)
    slow[i] = texelFetch(half_in, clamp(qb - ivec2(1) + ivec2(i % HL, i / HL), ivec2(0), hhi), 0).r;
if (li < 9u)
    stile[li] = texelFetch(weight_in, clamp(blk - ivec2(1) + ivec2(int(li % 3u), int(li / 3u)), ivec2(0), nt - ivec2(1)), 0).x;
load_block(origin, hi);
barrier();
vec2 gm = texelFetch(ring, ivec2(RING, 0), 0).xy - vec2(1.0);
for (int q = 0; q < 4; q++) {
    ivec2 o = lid * 2 + ivec2(q & 1, q >> 1);
    ivec2 p = origin + o;
    if (p.x < sz.x && p.y < sz.y) {
        float g1 = fine_at(o);
        float d1 = sy[(o.y + R1) * SW + o.x + R1] - g1;
        float d2 = g1 - low_at(o);
        // The tile weight, bilinear between tile centres (edges clamped).
        vec2 tp = (vec2(p) + 0.5) / float(BLK) - 0.5;
        ivec2 t0 = ivec2(floor(tp));
        vec2 tf = tp - vec2(t0);
        ivec2 a = clamp(t0, ivec2(0), nt - ivec2(1)) - blk + ivec2(1);
        ivec2 b = clamp(t0 + ivec2(1), ivec2(0), nt - ivec2(1)) - blk + ivec2(1);
        float top = stile[a.y * 3 + a.x] * (1.0 - tf.x) + stile[a.y * 3 + b.x] * tf.x;
        float bottom = stile[b.y * 3 + a.x] * (1.0 - tf.x) + stile[b.y * 3 + b.x] * tf.x;
        float w = top * (1.0 - tf.y) + bottom * tf.y;
        float delta = w * (gm.x * d1 + gm.y * d2);
        // Added to the source sample itself: a gain of 1 gives it back exactly.
        imageStore(dst, p, vec4(texelFetch(src, p, 0).r + delta / code_scale, 0.0, 0.0, 1.0));
    }
}
)GLSL";

		std::string WeightsGlsl(const char* name, const char* radiusName, int radius, double (*weight)(int, double), double scale)
		{
			const std::string taps = std::to_string(2 * radius + 1);
			std::string s = std::string("#define ") + radiusName + " " + std::to_string(radius) + "\nfloat " + name + "(int i)\n{\n    float w[" + taps + "] = float[" + taps + "](";
			for (int i = -radius; i <= radius; ++i)
			{
				char number[32];
				snprintf(number, sizeof(number), "%.9g", weight(i, scale));
				s += number;
				if (i < radius)
					s += ", ";
			}
			return s + ");\n    return w[i + " + radiusName + "];\n}\n";
		}

		pl_shader_desc Sampled(const char* name, pl_tex tex)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = tex;
			d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			return d;
		}

		pl_shader_desc Storage(const char* name, pl_tex tex, enum pl_desc_access access = PL_DESC_ACCESS_WRITEONLY)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = access;
			d.binding.object = tex;
			return d;
		}

		pl_fmt StorableLike(pl_gpu gpu, pl_fmt like)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
			// At least 16 bits (as VpDebander's): the stabilised detail is finer than an 8-bit code.
			if (like && (like->caps & caps) == caps && like->component_depth[0] >= 16) return like;
			const int n = like ? like->num_components : 1;
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_UNORM, n, 16, 16, caps);
			return f ? f : pl_find_fmt(gpu, PL_FMT_FLOAT, n, 16, 16, caps);
		}

		// 32-bit float, `components` or the next wider: the low band in codes needs fp32 (fp16
		// would round it by a quarter of a 10-bit code), and the ring is read-write.
		pl_fmt Float32(pl_gpu gpu, int components)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			for (int n = components; n <= 4; ++n)
				if (pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, n, 32, 32, caps))
					return f;
			return nullptr;
		}

		bool Ensure(pl_gpu gpu, pl_tex& tex, int w, int h, pl_fmt fmt, bool blitSrc = false)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = fmt;
			tp.sampleable = true;
			tp.storable = true;
			tp.blit_src = blitSrc;
			return fmt && pl_tex_recreate(gpu, &tex, &tp);
		}

		bool Compute(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
			const pl_shader_desc* desc, int numDesc, const pl_shader_var* vars, int numVars, size_t shmem, int groupsX, int groupsY)
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
			custom.compute_shmem = shmem;
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

		// The plane's geometry and band layout (the history belongs to one).
		struct Geometry
		{
			int w = 0, h = 0;
			double scale = 0.0;
			bool operator==(const Geometry& o) const { return w == o.w && h == o.h && scale == o.scale; }
		};

		struct Constants
		{
			Geometry geometry;
			int r1 = 0, gpR = 0;
			int tilesX = 0, tilesY = 0;
			std::string bandHeader;
			size_t bandShmem = 0;
			float codeScale = 1.0f;
		};
	}

	struct Module::Impl
	{
		pl_tex out = nullptr;            // the stabilised luma
		pl_tex half = nullptr;           // this source frame's half-resolution low band (codes)
		pl_tex stats[2] = {};            // per tile: E1, E2, low mean, low spread (this frame's and the previous one's)
		pl_tex weight = nullptr;         // per tile: veto weight, untrusted share, change
		pl_tex aheadStats = nullptr, aheadWeight = nullptr;
		pl_tex ring = nullptr;           // kRingSlots entries, then the decision, then the look-ahead
		int statsIndex = 0;              // stats[statsIndex] = lastSequence's
		int cur = 0;                     // ring slot of lastSequence
		int behind = 0;                  // consecutive frames in the ring behind lastSequence
		bool decided = false;            // the ring's decision is lastSequence's
		uint64_t lastSequence = 0;
		Geometry geometry;
		uint64_t aheadSequence = 0;
		Geometry aheadGeometry;
		float codeScale = 0.0f;          // the last Apply's (MeasureAhead reads the same signal)
	};

	namespace
	{
		void Prepare(pl_tex luma, const Params& params, Constants& c, const pl_frame* frame)
		{
			c.geometry.w = luma->params.w;
			c.geometry.h = luma->params.h;
			c.geometry.scale = Scale(params.planeLines > 0.0 ? params.planeLines : luma->params.h);
			c.r1 = FineRadius(c.geometry.scale);
			c.gpR = GrainPreserve::LowRadius(WideSigma(c.geometry.scale));
			c.tilesX = (c.geometry.w + kTilePx - 1) / kTilePx;
			c.tilesY = (c.geometry.h + kTilePx - 1) / kTilePx;
			c.bandHeader = "#define R1 " + std::to_string(c.r1) + "\n" + "#define RING " + std::to_string(kRingSlots) + "\n" +
				WeightsGlsl("fw", "FW_R", c.r1, FineWeight, c.geometry.scale) +
				GrainPreserve::LowWeightsGlsl(WideSigma(c.geometry.scale)) + BAND_HEADER;
			const int sw = kTilePx + 2 * c.r1, hl = kTilePx / 2 + 2;
			c.bandShmem = sizeof(float) * (sw * sw + sw * kTilePx + hl * hl + 1024);
			// One code of the signal in the plane texture's normalised units (VpDebander's).
			const int sampleDepth = frame && frame->repr.bits.sample_depth > 0 ? frame->repr.bits.sample_depth : luma->params.format->component_depth[0];
			const int shift = frame ? frame->repr.bits.bit_shift : 0;
			const double codeUnit = luma->params.format->type == PL_FMT_FLOAT ?
				1.0 / (std::pow(2.0, params.colorDepth) - 1.0) :
				std::pow(2.0, shift) / (std::pow(2.0, sampleDepth) - 1.0);
			c.codeScale = static_cast<float>(1.0 / codeUnit);
		}

		size_t MeasureShmem(const Constants& c)
		{
			const int hl = kTilePx / 2 + 2, rb = hl + 2 * c.gpR;
			return c.bandShmem + sizeof(float) * (rb * rb + rb * hl);
		}

		bool EnsureRing(pl_gpu gpu, pl_tex& ring)
		{
			if (ring) return true;
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
			if (!f) return false;
			std::string zero(sizeof(float) * 4 * (kRingSlots + 2), '\0');
			pl_tex_params tp{};
			tp.w = kRingSlots + 2;
			tp.h = 1;
			tp.format = f;
			tp.sampleable = true;
			tp.storable = true;
			tp.initial_data = zero.data();
			ring = pl_tex_create(gpu, &tp);
			return ring != nullptr;
		}

		// Measure (pass 1) and reduce (pass 2). ahead: the look-ahead (no half band, no previous
		// frame, no field; the ring's look-ahead entry).
		bool MeasureAndReduce(pl_dispatch dispatch, pl_tex luma, const Params& params, const Constants& c,
			pl_tex halfOut, pl_tex statsOut, pl_tex weightOut, pl_tex prevStats, pl_tex ring,
			bool ahead, int cur, int nValid, int nUse, bool aheadValid, std::string& error)
		{
			const bool useField = !ahead && params.field != nullptr;
			const bool usePrev = !ahead && prevStats != nullptr;
			const float toField[2] = {
				static_cast<float>(params.fieldScaleX > 0.0 ? params.fieldScaleX : params.fieldScale),
				static_cast<float>(params.fieldScale) };
			const float trustMin = static_cast<float>(kTrustMin);
			const float untrustedShare = static_cast<float>(kVetoUntrustedShare);
			const float vetoCodes = static_cast<float>(VetoCodes(params.bitrateQuality, params.colorDepth));
			const float energyJump = static_cast<float>(kTileEnergyJump);
			{
				pl_shader_desc d[6];
				int nd = 0;
				d[nd++] = Sampled("src", luma);
				d[nd++] = Storage("stats_out", statsOut);
				d[nd++] = Storage("weight_out", weightOut);
				if (!ahead) d[nd++] = Storage("half_out", halfOut);
				if (usePrev) d[nd++] = Sampled("prev_stats", prevStats);
				if (useField) d[nd++] = Sampled("field", params.field);
				pl_shader_var v[8];
				int nv = 0;
				v[nv++] = { pl_var_float("code_scale"), &c.codeScale, true };
				if (usePrev && !useField) v[nv++] = { pl_var_float("veto_codes"), &vetoCodes, true };
				if (usePrev) v[nv++] = { pl_var_float("energy_jump"), &energyJump, true };
				if (useField)
				{
					v[nv++] = { pl_var_vec2("to_field"), toField, true };
					v[nv++] = { pl_var_float("trust_min"), &trustMin, true };
					v[nv++] = { pl_var_float("untrusted_share"), &untrustedShare, true };
				}
				std::string header = c.bandHeader + MEASURE_HEADER;
				if (ahead) header = "#define AHEAD 1\n" + header;
				if (usePrev) header = "#define PREV 1\n" + header;
				if (useField) header = "#define FIELD 1\n" + header;
				if (!Compute(dispatch, ahead ? "VideoProcessor detail pulse: measure (look-ahead)" : "VideoProcessor detail pulse: measure",
					header, MEASURE_BODY, d, nd, v, nv, MeasureShmem(c), c.tilesX, c.tilesY))
				{
					error = "the measure pass failed";
					return false;
				}
			}
			{
				const float floorEnergy = static_cast<float>(FloorEnergy(params.colorDepth));
				const float winsorise = static_cast<float>(kWinsorise);
				const float cutCodes = static_cast<float>(CutCodes(params.bitrateQuality, params.colorDepth));
				const float cutShare = static_cast<float>(kCutShare);
				const float fadeCodes = static_cast<float>(FadeCodes(params.colorDepth));
				const float decay = static_cast<float>(DecayPerFrame(params.sourceHz));
				const float deadZone = static_cast<float>(kDeadZone);
				const float cap = static_cast<float>(ClampCap(params.cap));
				const float aheadRatio = static_cast<float>(kAheadMaxRatio);
				const int aheadOn = aheadValid ? 1 : 0;
				const pl_shader_desc d[3] = { Sampled("stats_in", statsOut), Sampled("weight_in", weightOut),
					Storage("ring", ring, PL_DESC_ACCESS_READWRITE) };
				const pl_shader_var v[14] = {
					{ pl_var_float("floor_energy"), &floorEnergy, true },
					{ pl_var_float("winsorise"), &winsorise, true },
					{ pl_var_float("cut_codes"), &cutCodes, true },
					{ pl_var_float("cut_share_min"), &cutShare, true },
					{ pl_var_float("fade_codes"), &fadeCodes, true },
					{ pl_var_float("decay"), &decay, true },
					{ pl_var_float("dead_zone"), &deadZone, true },
					{ pl_var_float("cap"), &cap, true },
					{ pl_var_float("ahead_ratio"), &aheadRatio, true },
					{ pl_var_int("cur"), &cur, true },
					{ pl_var_int("n_valid"), &nValid, true },
					{ pl_var_int("n_use"), &nUse, true },
					{ pl_var_int("ahead_valid"), &aheadOn, true } };
				std::string header = "#define RING " + std::to_string(kRingSlots) + "\n" + REDUCE_HEADER;
				if (ahead) header = "#define AHEAD 1\n" + header;
				if (useField) header = "#define FIELD 1\n" + header;
				if (!Compute(dispatch, ahead ? "VideoProcessor detail pulse: reduce (look-ahead)" : "VideoProcessor detail pulse: reduce",
					header, REDUCE_BODY, d, 3, v, 13, sizeof(float) * 1024, 1, 1))
				{
					error = "the reduce pass failed";
					return false;
				}
			}
			return true;
		}
	}

	Module::~Module()
	{
		delete impl;
	}

	uint64_t Module::HistorySequence() const { return impl && impl->decided ? impl->lastSequence : 0; }
	int Module::HistoryFrames() const { return impl && impl->decided ? impl->behind : 0; }

	void Module::Forget()
	{
		if (!impl) return;
		impl->decided = false;
		impl->behind = 0;
		impl->aheadSequence = 0;
	}

	void Module::Release(pl_gpu gpu)
	{
		if (impl)
		{
			for (pl_tex* tex : { &impl->out, &impl->half, &impl->stats[0], &impl->stats[1], &impl->weight,
				&impl->aheadStats, &impl->aheadWeight, &impl->ring })
			{
				if (*tex && gpu) pl_tex_destroy(gpu, tex);
				*tex = nullptr;
			}
		}
		delete impl;
		impl = nullptr;
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
	{
		error.clear();
		if (!gpu || !dispatch || source.num_planes <= 0)
		{
			error = "nothing to stabilise";
			return false;
		}
		const pl_tex luma = source.planes[0].texture;
		if (!luma || !luma->params.sampleable)
		{
			error = "no sampleable luma plane";
			++failures;
			return false;
		}
		if (source.planes[0].components != 1 || source.planes[0].component_mapping[0] != PL_CHANNEL_Y)
		{
			error = "the first plane is not a luma plane";
			return false;
		}
		if (!impl) impl = new Impl();
		Impl& m = *impl;
		Constants c;
		Prepare(luma, params, c, &source);
		m.codeScale = c.codeScale;
		const bool sameGeometry = m.decided && c.geometry == m.geometry;
		const bool repeat = sameGeometry && params.sequence != 0 && params.sequence == m.lastSequence;
		const bool consecutive = sameGeometry && m.lastSequence != 0 && params.sequence == m.lastSequence + 1;
		const int hw = (c.geometry.w + 1) / 2, hh = (c.geometry.h + 1) / 2;
		pl_fmt f1 = Float32(gpu, 1), f4 = Float32(gpu, 4);
		if (!Ensure(gpu, m.out, c.geometry.w, c.geometry.h, StorableLike(gpu, luma->params.format), true) ||
			!f1 || !f4 || !EnsureRing(gpu, m.ring))
		{
			error = "no storable textures";
			++failures;
			return false;
		}
		if (!repeat)
		{
			const int next = 1 - m.statsIndex;
			if (!Ensure(gpu, m.half, hw, hh, f1) || !Ensure(gpu, m.stats[next], c.tilesX, c.tilesY, f4) ||
				!Ensure(gpu, m.weight, c.tilesX, c.tilesY, f4))
			{
				error = "no storable textures";
				++failures;
				m.decided = false;
				return false;
			}
			const int slot = m.decided ? (m.cur + 1) % kRingSlots : 0;
			const int nValid = consecutive ? (std::min)(m.behind + 1, kRingSlots - 2) : 0;
			const int nUse = (std::min)(nValid, WindowFrames(params.sourceHz));
			const bool aheadValid = m.aheadSequence != 0 && params.sequence != 0 && m.aheadSequence == params.sequence + 1 &&
				m.aheadGeometry == c.geometry;
			if (!MeasureAndReduce(dispatch, luma, params, c, m.half, m.stats[next], m.weight,
				consecutive && m.stats[m.statsIndex] ? m.stats[m.statsIndex] : nullptr, m.ring,
				false, slot, nValid, nUse, aheadValid, error))
			{
				++failures;
				m.decided = false;
				return false;
			}
			m.statsIndex = next;
			m.cur = slot;
			m.behind = nValid;
			m.lastSequence = params.sequence;
			m.geometry = c.geometry;
			m.decided = true;
			if (aheadValid) ++aheadUsed;
		}
		{
			const pl_shader_desc d[5] = { Sampled("src", luma), Storage("dst", m.out), Sampled("half_in", m.half),
				Sampled("weight_in", m.weight), Sampled("ring", m.ring) };
			const pl_shader_var v[1] = { { pl_var_float("code_scale"), &c.codeScale, true } };
			const std::string header = c.bandHeader + APPLY_HEADER;
			if (!Compute(dispatch, "VideoProcessor detail pulse: apply", header, APPLY_BODY, d, 5, v, 1,
				c.bandShmem + sizeof(float) * 9, c.tilesX, c.tilesY))
			{
				error = "the apply pass failed";
				++failures;
				return false;
			}
		}
		pl_frame frame = source;
		frame.planes[0].texture = m.out;
		result = frame;
		++frames;
		return true;
	}

	void LumaWeights(enum pl_color_system system, enum pl_color_primaries primaries, double out[3])
	{
		double kr = 0.2126, kb = 0.0722;
		switch (system)
		{
		case PL_COLOR_SYSTEM_BT_601: kr = 0.299; kb = 0.114; break;
		case PL_COLOR_SYSTEM_BT_2020_NC:
		case PL_COLOR_SYSTEM_BT_2020_C: kr = 0.2627; kb = 0.0593; break;
		case PL_COLOR_SYSTEM_SMPTE_240M: kr = 0.212; kb = 0.087; break;
		case PL_COLOR_SYSTEM_BT_709: break;
		default:
			if (primaries == PL_COLOR_PRIM_BT_2020) { kr = 0.2627; kb = 0.0593; }
			break;
		}
		out[0] = kr;
		out[1] = 1.0 - kr - kb;
		out[2] = kb;
	}

	bool Module::ApplyForeign(pl_gpu, pl_dispatch, const pl_frame&, const Params&, pl_frame&)
	{
		error = "not implemented";
		return false;
	}

	bool Module::MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& next, uint64_t sequence, const Params& params,
		VpDebander::Module*, const VpDebander::Params*)
	{
		return next.num_planes > 0 && MeasureAhead(gpu, dispatch, next.planes[0].texture, sequence, params);
	}

	bool Module::MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, pl_tex luma, uint64_t sequence, const Params& params)
	{
		error.clear();
		if (!gpu || !dispatch || !luma || !luma->params.sampleable || sequence == 0)
		{
			error = "nothing to measure ahead";
			return false;
		}
		if (!impl) impl = new Impl();
		Impl& m = *impl;
		// The next frame shares the current one's representation: the code scale is the last
		// Apply's (a bare texture does not say where its signal's bits sit).
		if (!(m.codeScale > 0.0f))
		{
			error = "no frame applied yet";
			return false;
		}
		Constants c;
		Prepare(luma, params, c, nullptr);
		c.codeScale = m.codeScale;
		pl_fmt f4 = Float32(gpu, 4);
		if (!f4 || !EnsureRing(gpu, m.ring) || !Ensure(gpu, m.aheadStats, c.tilesX, c.tilesY, f4) ||
			!Ensure(gpu, m.aheadWeight, c.tilesX, c.tilesY, f4))
		{
			error = "no storable textures";
			++failures;
			return false;
		}
		m.aheadSequence = 0;
		if (!MeasureAndReduce(dispatch, luma, params, c, nullptr, m.aheadStats, m.aheadWeight, nullptr, m.ring,
			true, 0, 0, 0, false, error))
		{
			++failures;
			return false;
		}
		m.aheadSequence = sequence;
		m.aheadGeometry = c.geometry;
		return true;
	}
}
