// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split GPU passes (GrainSplitGpu.h). Mirrors GrainSplit::Compute (GrainSplit.h), the v3 law (refs/116 s6):
// the two low bands at half resolution and the full-resolution work in shared-memory tiles, the pass structure of
// texture_restore (TextureRestore.cpp: HALF / LOWH / LOWV / STATS / GRAIN / APPLY). No full-resolution intermediates.
#include <pch.h>

#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace GrainSplit
{
	namespace Gpu
	{
		namespace
		{
			// Geometry shared by the passes (defines): GS_W/GS_H plane, GS_HW/GS_HH half plane, GS_B block, GS_BW/GS_BH grid.
			// Weights: w1/R1 full-resolution fine band (sigmaFine s), wb/RB and w6/R6 the half-resolution base and low bands
			// (V3HalfWeight of sigmaBase s and sigmaLow s), RL = max(RB, R6).

			// 1. HALF + ACROSS: the half plane by the shared U3 accessor vpu_half_luma_comp (VpUnit::HalfLuma::GlslComponent,
			// channel GS_COMP) in codes (GS_INV_CU) into shared memory (GrainSplit::V3Half), then the base and low Gaussians
			// across (half-resolution edges clamped). Tile TW x TH half px per workgroup (64 x 4).
			const char* const HALF_HEADER = R"GLSL(
#define TW 64
#define TH 4
#define SW (TW + 2 * RL)
shared float s_r[TH * SW];
)GLSL";
			const char* const HALF_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * ivec2(TW, TH);
for (uint i = li; i < uint(TH * SW); i += uint(TW * TH)) {
    int r = int(i / uint(SW)), c = int(i % uint(SW));
    ivec2 q = ivec2(clamp(org.x - RL + c, 0, GS_HW - 1), clamp(org.y + r, 0, GS_HH - 1));
    s_r[i] = vpu_half_luma_comp(src, q, GS_COMP) * GS_INV_CU;
}
barrier();
ivec2 q = org + lid;
if (q.x < GS_HW && q.y < GS_HH) {
    int c0 = lid.y * SW + lid.x + RL;
    float ab = 0.0, a6 = 0.0;
    for (int k = -RB; k <= RB; k++) ab += wb(k) * s_r[c0 + k];
    for (int k = -R6; k <= R6; k++) a6 += w6(k) * s_r[c0 + k];
    imageStore(tmp_out, q, vec4(ab, a6, 0.0, 0.0));
}
)GLSL";

			// 2. DOWN: the base and low Gaussians down (half resolution) -> low = (Gb, G6). Tile TC x TR half px (16 x 64).
			const char* const DOWN_HEADER = R"GLSL(
#define TC 16
#define TR 64
#define SH (TR + 2 * RL)
shared vec2 s_c[SH * TC];
)GLSL";
			const char* const DOWN_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * ivec2(TC, TR);
for (uint i = li; i < uint(SH * TC); i += 256u) {
    int r = int(i / uint(TC)), c = int(i % uint(TC));
    ivec2 q = ivec2(clamp(org.x + c, 0, GS_HW - 1), clamp(org.y - RL + r, 0, GS_HH - 1));
    s_c[i] = texelFetch(tmp, q, 0).xy;
}
barrier();
for (int s = 0; s < TR / 16; s++) {
    int ry = lid.y + 16 * s;
    ivec2 q = org + ivec2(lid.x, ry);
    if (q.x < GS_HW && q.y < GS_HH) {
        float ab = 0.0, a6 = 0.0;
        for (int k = -RB; k <= RB; k++) ab += wb(k) * s_c[(ry + RL + k) * TC + lid.x].x;
        for (int k = -R6; k <= R6; k++) a6 += w6(k) * s_c[(ry + RL + k) * TC + lid.x].y;
        imageStore(low_out, q, vec4(ab, a6, 0.0, 0.0));
    }
}
)GLSL";

			// The half-resolution bands at a full-resolution pixel from the shared tile s_l (half indices lo .. lo + LA - 1 with
			// lo = org / 2 - 1, loaded clamped): bilinear at (p + 0.5) / 2 - 0.5, the arithmetic of GrainSplit::V3LowAt. Any
			// origin parity: for p in [org, org + n - 1] the taps span lo + 1 .. lo + (n + 1) / 2 + 1 <= lo + n / 2 + 2.
			const char* const LOW_S_GLSL = R"GLSL(
vec2 low_s(ivec2 p, ivec2 lo)
{
    vec2 h = (vec2(p) + vec2(0.5)) * 0.5 - vec2(0.5);
    ivec2 i0 = ivec2(floor(h));
    vec2 f = h - vec2(i0);
    ivec2 a = i0 - lo;
    vec2 v00 = s_l[a.y * LA + a.x], v10 = s_l[a.y * LA + a.x + 1];
    vec2 v01 = s_l[(a.y + 1) * LA + a.x], v11 = s_l[(a.y + 1) * LA + a.x + 1];
    vec2 top = v00 + (v10 - v00) * f.x, bot = v01 + (v11 - v01) * f.x;
    return top + (bot - top) * f.y;
}
)GLSL";

			// 3. STATS: one workgroup per block, in chunks of CH x CH px (CH = min(B, 16)): the chunk and its R1 apron in
			// shared memory, G1 separably, the bands per pixel; per sub-block (one thread each, fixed order) the sums of
			// (D^2, d1^2, d2^2, d3^2) and the pixel count. Then one thread: the quietest sub-block energies (sorted) x CAL and
			// the block means -> quiet = (quiet, e3, e1, e2). Deterministic: no atomics.
			const char* const STATS_HEADER = R"GLSL(
#define AP (CH + 2 * R1)
#define LA (CH / 2 + 3)
#define NSUB (GS_SUB * GS_SUB)
shared float s_y[AP * AP];
shared float s_hh[AP * CH];
shared vec2 s_l[LA * LA];
shared vec4 s_px[CH * CH];
shared vec4 s_sum[NSUB];
shared float s_cnt[NSUB];
)GLSL";
			const char* const STATS_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 borg = blk * GS_B;
if (li < uint(NSUB)) { s_sum[li] = vec4(0.0); s_cnt[li] = 0.0; }
for (int cy = 0; cy < GS_B; cy += CH) {
    for (int cx = 0; cx < GS_B; cx += CH) {
        ivec2 org = borg + ivec2(cx, cy);
        ivec2 lo = org / 2 - ivec2(1);
        barrier();
        for (uint i = li; i < uint(AP * AP); i += 256u) {
            int r = int(i / uint(AP)), c = int(i % uint(AP));
            s_y[i] = gs_y(clamp(org.x - R1 + c, 0, GS_W - 1), clamp(org.y - R1 + r, 0, GS_H - 1));
        }
        for (uint i = li; i < uint(LA * LA); i += 256u) {
            int r = int(i / uint(LA)), c = int(i % uint(LA));
            s_l[i] = texelFetch(low, clamp(lo + ivec2(c, r), ivec2(0), ivec2(GS_HW - 1, GS_HH - 1)), 0).xy;
        }
        barrier();
        for (uint i = li; i < uint(AP * CH); i += 256u) {
            int r = int(i / uint(CH)), c = int(i % uint(CH));
            float acc = 0.0;
            for (int k = -R1; k <= R1; k++) acc += w1(k) * s_y[r * AP + c + R1 + k];
            s_hh[i] = acc;
        }
        barrier();
        if (lid.x < CH && lid.y < CH) {
            ivec2 p = org + lid;
            float g1 = 0.0;
            for (int k = -R1; k <= R1; k++) g1 += w1(k) * s_hh[(lid.y + R1 + k) * CH + lid.x];
            float y = s_y[(lid.y + R1) * AP + lid.x + R1];
            vec2 l = low_s(p, lo);
            float d1 = y - g1, d2 = g1 - l.x, d3 = l.x - l.y, dd = d1 + d2;
            bool inside = p.x < GS_W && p.y < GS_H;
            s_px[lid.y * CH + lid.x] = inside ? vec4(dd * dd, d1 * d1, d2 * d2, d3 * d3) : vec4(-1.0);
        }
        barrier();
        if (li < uint(NSUB)) {
            int si = int(li) % GS_SUB, sj = int(li) / GS_SUB;
            int x0 = max((si * GS_B) / GS_SUB, cx), x1 = min(((si + 1) * GS_B) / GS_SUB, cx + CH);
            int y0 = max((sj * GS_B) / GS_SUB, cy), y1 = min(((sj + 1) * GS_B) / GS_SUB, cy + CH);
            vec4 acc = s_sum[li];
            float n = s_cnt[li];
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    vec4 v = s_px[(yy - cy) * CH + (xx - cx)];
                    if (v.x >= 0.0) { acc += v; n += 1.0; }
                }
            s_sum[li] = acc;
            s_cnt[li] = n;
        }
    }
}
barrier();
if (li == 0u) {
    float e[NSUB];
    int n = 0;
    vec4 tot = vec4(0.0);
    float cnt = 0.0;
    for (int i = 0; i < NSUB; i++) {
        if (s_cnt[i] > 0.0) { e[n] = s_sum[i].x / s_cnt[i]; n++; }
        tot += s_sum[i];
        cnt += s_cnt[i];
    }
    for (int i = 1; i < n; i++) {
        float v = e[i];
        int j = i - 1;
        while (j >= 0 && e[j] > v) { e[j + 1] = e[j]; j--; }
        e[j + 1] = v;
    }
    int nq = min(n, max(1, GS_QUIET));
    float m = 0.0;
    for (int i = 0; i < nq; i++) m += e[i];
    float quiet = n > 0 ? GS_CAL * m / float(nq) : 0.0;
    float c = max(cnt, 1.0);
    imageStore(quiet_out, blk, vec4(quiet, tot.w / c, tot.y / c, tot.z / c));
}
)GLSL";

			// 4. NMIN: per block, the minimum of quiet over (2 n + 1)^2 blocks -> blocks = (N, e3, e1, e2).
			const char* const NMIN_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < GS_BW && b.y < GS_BH) {
    float m = 1e30;
    for (int j = -GS_NG; j <= GS_NG; j++)
        for (int i = -GS_NG; i <= GS_NG; i++)
            m = min(m, texelFetch(quiet, ivec2(clamp(b.x + i, 0, GS_BW - 1), clamp(b.y + j, 0, GS_BH - 1)), 0).x);
    vec4 q = texelFetch(quiet, b, 0);
    imageStore(blocks_out, b, vec4(m, q.y, q.z, q.w));
}
)GLSL";

			// 5. APPLY: 16 x 16 output px per workgroup; the plane with a 1 px border and the R1 apron in shared memory,
			// G1 for the 16 x 16 centre, D = Y - Gb over the 18 x 18 (positions clamped, as GrainSplit's d.get), the 3 x 3
			// energy, f = clamp(1 - kappa N / max(e, floor), 0, 1) -> the product (Full: split rgba32f; Compact: A, B).
			const char* const APPLY_HEADER = R"GLSL(
#define AC 16
#define AW (AC + 2)
#define AA (AW + 2 * R1)
#define LA (AC / 2 + 2)
shared float s_y[AA * AA];
shared float s_hh[AA * AC];
shared vec2 s_l[LA * LA];
shared float s_d[AW * AW];
)GLSL";
			const char* const APPLY_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * AC;
ivec2 so = org - ivec2(1 + R1);
ivec2 lo = org / 2 - ivec2(1);
for (uint i = li; i < uint(AA * AA); i += 256u) {
    int r = int(i / uint(AA)), c = int(i % uint(AA));
    s_y[i] = gs_y(clamp(so.x + c, 0, GS_W - 1), clamp(so.y + r, 0, GS_H - 1));
}
for (uint i = li; i < uint(LA * LA); i += 256u) {
    int r = int(i / uint(LA)), c = int(i % uint(LA));
    s_l[i] = texelFetch(low, clamp(lo + ivec2(c, r), ivec2(0), ivec2(GS_HW - 1, GS_HH - 1)), 0).xy;
}
barrier();
for (uint i = li; i < uint(AA * AC); i += 256u) {
    int r = int(i / uint(AC)), c = int(i % uint(AC));
    float acc = 0.0;
    for (int k = -R1; k <= R1; k++) acc += w1(k) * s_y[r * AA + c + 1 + R1 + k];
    s_hh[i] = acc;
}
for (uint i = li; i < uint(AW * AW); i += 256u) {
    int r = int(i / uint(AW)), c = int(i % uint(AW));
    ivec2 q = clamp(org - ivec2(1) + ivec2(c, r), ivec2(0), ivec2(GS_W - 1, GS_H - 1));
    ivec2 sq = q - so;
    s_d[i] = s_y[sq.y * AA + sq.x] - low_s(q, lo).x;
}
barrier();
ivec2 p = org + lid;
if (p.x < GS_W && p.y < GS_H) {
    float g1 = 0.0;
    for (int k = -R1; k <= R1; k++) g1 += w1(k) * s_hh[(lid.y + 1 + R1 + k) * AC + lid.x];
    int ci = (lid.y + 1) * AW + lid.x + 1;
    float e = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            float v = s_d[ci + j * AW + i];
            e += v * v;
        }
    e /= 9.0;
    float y = s_y[(lid.y + 1 + R1) * AA + lid.x + 1 + R1];
    float gb = low_s(p, lo).x;
    float d1 = y - g1, d2 = g1 - gb;
    float nn = texelFetch(blocks, p / GS_B, 0).x;
    float f = clamp(1.0 - GS_KAPPA * nn / max(e, GS_FLOOR), 0.0, 1.0);
#ifdef GS_COMPACT
    imageStore(split_a, p, vec4(f * d1, f * d2, 0.0, 0.0));
    imageStore(split_b, p, vec4(f, gb * GS_CU, 0.0, 0.0));
#else
    imageStore(split_out, p, vec4(f * d1, f * d2, f, gb));
#endif
}
)GLSL";

			constexpr const char* kArenaOwner = "grain_split";
			constexpr int kHalfTw = 64, kHalfTh = 4, kDownTc = 16, kDownTr = 64, kApplyTile = 16;

			// Opens the arena scope of one Compute and closes it on every exit.
			struct ScopeGuard
			{
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* a;
				bool opened = false;   // a refused (nested) scope is not ours to close
				explicit ScopeGuard(VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena) : a(arena) { if (a) opened = a->BeginScope(kArenaScope); }
				~ScopeGuard() { if (a && opened) a->EndScope(); }
				ScopeGuard(const ScopeGuard&) = delete;
				ScopeGuard& operator=(const ScopeGuard&) = delete;
			};

			// float NAME(int k), k in -R..R, with the weights `weight(k)` and radius r.
			template <typename F>
			std::string WeightsGlsl(const char* name, const char* radiusName, int r, F weight)
			{
				return VpvGpu::DefineInt(radiusName, r) + "float " + name + "(int k)\n{\n    " +
					VpvGpu::ConstArray("w", 2 * r + 1, [&](int i) { return weight(i - r); }) + "    return w[k + " + radiusName + "];\n}\n";
			}

			int HalfOf(int v) { return (v + 1) / 2; }
			int ChunkOf(int B) { return (std::min)(B, 16); }
		}

		int GaussRadius(double sigma) { return VpUnit::BandFromBase::Radius(sigma, 3.0, true, 0.0); }

		double GaussWeight(int k, double sigma)
		{
			return VpUnit::BandFromBase::Weight(k, sigma, GaussRadius(sigma));
		}

		void Splitter::Release(pl_gpu gpu)
		{
			if (gpu)
				for (pl_tex* t : { &m_tmp, &m_low, &m_quiet, &m_blocks, &m_outFull, &m_outA, &m_outB })
					if (*t) pl_tex_destroy(gpu, t);
			m_out = Frame();
		}

		namespace
		{
			std::string ViewGlslBody(GrainSplit::Storage storage, double codeUnit);
		}

		// The unit version is embedded in every prelude (a mismatch is detectable in the GLSL itself).
		std::string ViewGlsl(GrainSplit::Storage storage, double codeUnit)
		{
			return std::string("#define GS_UNIT_VERSION ") + std::to_string(GrainSplit::kUnitVersion) + "\n" + ViewGlslBody(storage, codeUnit);
		}

		namespace
		{
		std::string ViewGlslBody(GrainSplit::Storage storage, double codeUnit)
		{
			if (storage != GrainSplit::Storage::Compact)
				return "vec4 gs_fetch(ivec2 p) { return texelFetch(split, p, 0); }\n"
					"vec2 gs_blocks(ivec2 b) { return texelFetch(gblocks, b, 0).xy; }\n"
					"#define GS_HAS_ENERGIES 0\n"   // Full is read as before: no band energies through the view
					"vec2 gs_energies(ivec2 b) { return vec2(-1.0); }\n"
					"float gs_tilt(ivec2 b) { return 0.0; }\n";
			return "const float gs_cu = " + VpvGpu::Num(codeUnit) + ";\n"
				"vec4 gs_fetch(ivec2 p)\n{\n    vec2 a = texelFetch(gs_a, p, 0).xy;\n    vec2 b = texelFetch(gs_b, p, 0).xy;\n"
				"    return vec4(a.x, a.y, b.x, b.y / gs_cu);\n}\n"
				"vec2 gs_blocks(ivec2 b) { return texelFetch(gblocks, b, 0).xy; }\n"
				"#define GS_HAS_ENERGIES 1\n"
				"vec2 gs_energies(ivec2 b) { return texelFetch(gblocks, b, 0).zw; }\n"   // (e1, e2): block means of d1^2, d2^2 (codes^2)
				"float gs_tilt(ivec2 b)\n{\n    vec2 e = texelFetch(gblocks, b, 0).zw;\n    return log2((e.x + 1e-3) / (e.y + 1e-3));\n}\n";   // derived, not stored
		}
		}

		uint64_t FullShaderFingerprint()
		{
			uint64_t h = 14695981039346656037ull;
			for (const char* body : { HALF_HEADER, HALF_BODY, DOWN_HEADER, DOWN_BODY, LOW_S_GLSL, STATS_HEADER, STATS_BODY, NMIN_BODY, APPLY_HEADER, APPLY_BODY })
				for (const char* c = body; *c; ++c)
					if (*c != '\r')
					{
						h ^= static_cast<unsigned char>(*c);
						h *= 1099511628211ull;
					}
			return h;
		}

		void BindView(const Frame& f, std::vector<pl_shader_desc>& out)
		{
			if (Compact(&f))
			{
				out.push_back(VpvGpu::Sampled("gs_a", f.split));
				out.push_back(VpvGpu::Sampled("gs_b", f.splitB));
			}
			else
				out.push_back(VpvGpu::Sampled("split", f.split));
			out.push_back(VpvGpu::Sampled("gblocks", f.blocks));
		}

		std::vector<VpSkeleton::ArenaRequest> Splitter::ArenaRequests(pl_gpu gpu, int w, int h, int B, GrainSplit::Storage storage)
		{
			(void)storage;   // v3: the same intermediates for both layouts
			std::vector<VpSkeleton::ArenaRequest> r;
			VpSkeleton::TexFormat t2 = VpSkeleton::TexFormat::RG32F, t4 = VpSkeleton::TexFormat::RGBA32F;
			if (!gpu || w < 1 || h < 1 || B < 1 || !VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 2), &t2) ||
				!VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 4), &t4))
				return r;
			const int bw = (w + B - 1) / B, bh = (h + B - 1) / B;
			VpSkeleton::ArenaRequest a;
			a.scope = kArenaScope;
			a.owner = kArenaOwner;
			a.tier = VpSkeleton::Tier::T0;
			a.shape.w = static_cast<uint32_t>(HalfOf(w)); a.shape.h = static_cast<uint32_t>(HalfOf(h)); a.shape.fmt = t2;
			a.count = 2;   // tmp (across) + low (Gb, G6) at half resolution
			r.push_back(a);
			a.shape.w = static_cast<uint32_t>(bw); a.shape.h = static_cast<uint32_t>(bh); a.shape.fmt = t4;
			a.count = 1;   // quiet = (quiet, e3, e1, e2) per block
			r.push_back(a);
			return r;
		}

		bool Splitter::Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params)
		{
			return Compute(gpu, dispatch, plane, component, codeUnit, scale, B, params, Options());
		}

		bool Splitter::Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params,
			const Options& options)
		{
			// One arena scope across the five steps (they share the arena's temporaries), closed on every exit.
			ScopeGuard scope(options.arena);
			for (uint32_t i = 0; i < StepCount(); ++i)
				if (!Step(gpu, dispatch, plane, component, codeUnit, scale, B, params, options, i))
					return false;
			return true;
		}

		namespace
		{
			struct PassText
			{
				std::string geo, w1, wlow, yread;
				int r1 = 1, rb = 1, r6 = 1;
			};
			PassText MakePassText(int w, int h, int B, int bw, int bh, int component, double codeUnit, double scale, const Params& params)
			{
				using namespace VpvGpu;
				static const char* const kComp[4] = { "x", "y", "z", "w" };
				PassText t;
				t.geo = DefineInt("GS_W", w) + DefineInt("GS_H", h) + DefineInt("GS_HW", HalfOf(w)) + DefineInt("GS_HH", HalfOf(h)) +
					DefineInt("GS_B", B) + DefineInt("GS_BW", bw) + DefineInt("GS_BH", bh) + DefineInt("GS_UNIT_VERSION", GrainSplit::kUnitVersion);
				const double s1 = params.sigmaFine * scale, sb = params.sigmaBase * scale, s6 = params.sigmaLow * scale;
				t.r1 = GaussRadius(s1);
				t.rb = GrainSplit::V3HalfRadius(sb);
				t.r6 = GrainSplit::V3HalfRadius(s6);
				t.w1 = WeightsGlsl("w1", "R1", t.r1, [&](int k) { return GaussWeight(k, s1); });
				t.wlow = WeightsGlsl("wb", "RB", t.rb, [&](int k) { return GrainSplit::V3HalfWeight(k, sb); }) +
					WeightsGlsl("w6", "R6", t.r6, [&](int k) { return GrainSplit::V3HalfWeight(k, s6); }) +
					DefineInt("RL", (std::max)(t.rb, t.r6));
				t.yread = "float gs_y(int x, int y) { return texelFetch(src, ivec2(x, y), 0)." + std::string(kComp[component]) + " * " +
					Num(1.0 / codeUnit) + "; }\n";
				return t;
			}
		}

		bool Splitter::Step(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params,
			const Options& options, uint32_t step)
		{
			using namespace VpvGpu;
			if (step >= StepCount())
			{
				m_error = "grain split: step out of range";
				m_run = Run();
				return false;
			}
			if (step == 0)
			{
				m_run = Run();
				if (!Begin(gpu, dispatch, plane, component, codeUnit, B, params, options, scale))
					return false;
			}
			else if (!m_run.active || m_run.next != step)
			{
				m_error = "grain split: step out of order";
				m_run = Run();
				m_out = Frame();
				return false;
			}
			const Run r = m_run;
			const int w = r.w, h = r.h, bw = r.bw, bh = r.bh, hw = HalfOf(w), hh = HalfOf(h);
			const bool compact = r.compact;
			const PassText txt = MakePassText(w, h, B, bw, bh, component, codeUnit, scale, params);
			const int rl = (std::max)(txt.rb, txt.r6);
			auto fail = [&](const char* why) {
				m_error = why;
				m_run = Run();
				m_out = Frame();
				return false;
			};
			switch (step)
			{
			case 0:
			{
				const pl_shader_desc d[2] = { Sampled("src", plane), VpvGpu::Storage("tmp_out", r.tmp) };
				const size_t shmem = sizeof(float) * size_t(kHalfTh) * size_t(kHalfTw + 2 * rl);
				const std::string hdr = txt.geo + txt.wlow + Define("GS_INV_CU", 1.0 / codeUnit) + DefineInt("GS_COMP", component) +
					VpUnit::HalfLuma::GlslComponent() + HALF_HEADER;
				if (!Pass(dispatch, "VideoProcessor grain split: half + across", hdr, HALF_BODY, d, 2, nullptr, 0,
					(hw + kHalfTw - 1) / kHalfTw, (hh + kHalfTh - 1) / kHalfTh, shmem, kHalfTw, kHalfTh, options.timer))
					return fail("grain split: half pass failed");
				break;
			}
			case 1:
			{
				const pl_shader_desc d[2] = { Sampled("tmp", r.tmp), VpvGpu::Storage("low_out", r.low) };
				const size_t shmem = 2 * sizeof(float) * size_t(kDownTc) * size_t(kDownTr + 2 * rl);
				if (!Pass(dispatch, "VideoProcessor grain split: low bands down", txt.geo + txt.wlow + DOWN_HEADER, DOWN_BODY, d, 2, nullptr, 0,
					(hw + kDownTc - 1) / kDownTc, (hh + kDownTr - 1) / kDownTr, shmem, 16, 16, options.timer))
					return fail("grain split: low pass failed");
				break;
			}
			case 2:
			{
				const int ch = ChunkOf(B), ap = ch + 2 * txt.r1, la = ch / 2 + 3, nsub = params.subBlocks * params.subBlocks;
				const std::string hdr = txt.geo + txt.w1 + txt.yread + DefineInt("CH", ch) + DefineInt("GS_SUB", params.subBlocks) +
					DefineInt("GS_QUIET", params.quietSubBlocks) + Define("GS_CAL", params.grainCal) + STATS_HEADER + LOW_S_GLSL;
				const size_t shmem = sizeof(float) * (size_t(ap) * ap + size_t(ap) * ch + 2u * size_t(la) * la + 4u * size_t(ch) * ch + 5u * size_t(nsub));
				const pl_shader_desc d[3] = { Sampled("src", plane), Sampled("low", r.low), VpvGpu::Storage("quiet_out", r.quiet) };
				if (!Pass(dispatch, "VideoProcessor grain split: grain level", hdr, STATS_BODY, d, 3, nullptr, 0, bw, bh, shmem, 16, 16, options.timer))
					return fail("grain split: grain level pass failed");
				break;
			}
			case 3:
			{
				const pl_shader_desc d[2] = { Sampled("quiet", r.quiet), VpvGpu::Storage("blocks_out", r.blocks) };
				if (!Pass(dispatch, "VideoProcessor grain split: neighbourhood minimum", txt.geo + DefineInt("GS_NG", params.neighbourhood), NMIN_BODY, d, 2, nullptr, 0,
					(bw + 15) / 16, (bh + 15) / 16, 0, 16, 16, options.timer))
					return fail("grain split: minimum pass failed");
				break;
			}
			default:
			{
				const int aw = kApplyTile + 2, aa = aw + 2 * txt.r1, la = kApplyTile / 2 + 2;
				const size_t shmem = sizeof(float) * (size_t(aa) * aa + size_t(aa) * kApplyTile + 2u * size_t(la) * la + size_t(aw) * aw);
				std::string hdr = txt.geo + txt.w1 + txt.yread + Define("GS_KAPPA", params.kappa) + Define("GS_FLOOR", params.energyFloor) +
					APPLY_HEADER + LOW_S_GLSL;
				const int gx = (w + kApplyTile - 1) / kApplyTile, gy = (h + kApplyTile - 1) / kApplyTile;
				if (compact)
				{
					hdr = DefineInt("GS_COMPACT", 1) + Define("GS_CU", codeUnit) + hdr;
					const pl_shader_desc d[5] = { Sampled("src", plane), Sampled("low", r.low), Sampled("blocks", r.blocks),
						VpvGpu::Storage("split_a", r.outA), VpvGpu::Storage("split_b", r.outB) };
					if (!Pass(dispatch, "VideoProcessor grain split: Wiener share (compact)", hdr, APPLY_BODY, d, 5, nullptr, 0, gx, gy, shmem, 16, 16, options.timer))
						return fail("grain split: Wiener pass failed");
					m_out.split = r.outA;
					m_out.splitB = r.outB;
				}
				else
				{
					const pl_shader_desc d[4] = { Sampled("src", plane), Sampled("low", r.low), Sampled("blocks", r.blocks),
						VpvGpu::Storage("split_out", r.outFull) };
					if (!Pass(dispatch, "VideoProcessor grain split: Wiener share", hdr, APPLY_BODY, d, 4, nullptr, 0, gx, gy, shmem, 16, 16, options.timer))
						return fail("grain split: Wiener pass failed");
					m_out.split = r.outFull;
					m_out.splitB = nullptr;
				}
				m_out.blocks = r.blocks;
				m_out.w = w; m_out.h = h; m_out.bw = bw; m_out.bh = bh; m_out.B = B; m_out.scale = scale;
				m_out.codeUnit = codeUnit; m_out.component = component;
				m_out.storage = r.storage;
				m_out.seq = options.seq;
				m_out.version = GrainSplit::kUnitVersion;
				break;
			}
			}
			m_run.next = step + 1;
			if (step + 1 == StepCount())
				m_run = Run();
			return true;
		}

		bool Splitter::Begin(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, int B, const Params& params,
			const Options& options, double scale)
		{
			using namespace VpvGpu;
			m_error.clear();
			m_scratchShort = false;
			m_out = Frame();   // a failed Compute never leaves the previous frame's product looking valid
			if (!gpu || !dispatch || !plane || !plane->params.sampleable || component < 0 || component > 3 || !(codeUnit > 0.0) || B < 1)
			{
				m_error = "grain split: invalid input";
				return false;
			}
			if (!(params.sigmaFine > 0.0) || !(params.sigmaBase * scale >= GrainSplit::kV3MinLowSigma) || !(params.sigmaLow * scale >= GrainSplit::kV3MinLowSigma) ||
				params.subBlocks < 1 || params.subBlocks > 16)
			{
				m_error = "grain split: invalid law";
				return false;
			}
			// The passes' shared memory against the device limit (a visible refusal, never a silent fallback).
			{
				const double s1 = params.sigmaFine * scale;
				const int r1 = GaussRadius(s1), rl = (std::max)(GrainSplit::V3HalfRadius(params.sigmaBase * scale), GrainSplit::V3HalfRadius(params.sigmaLow * scale));
				const int ch = ChunkOf(B), ap = ch + 2 * r1, la = ch / 2 + 3, nsub = params.subBlocks * params.subBlocks;
				const int aw = kApplyTile + 2, aa = aw + 2 * r1, lapp = kApplyTile / 2 + 2;
				const size_t need[4] = { sizeof(float) * size_t(kHalfTh) * size_t(kHalfTw + 2 * rl), 2 * sizeof(float) * size_t(kDownTc) * size_t(kDownTr + 2 * rl),
					sizeof(float) * (size_t(ap) * ap + size_t(ap) * ch + 2u * size_t(la) * la + 4u * size_t(ch) * ch + 5u * size_t(nsub)),
					sizeof(float) * (size_t(aa) * aa + size_t(aa) * kApplyTile + 2u * size_t(lapp) * lapp + size_t(aw) * aw) };
				const size_t most = (std::max)((std::max)(need[0], need[1]), (std::max)(need[2], need[3]));
				if (most > gpu->glsl.max_shmem_size)
				{
					m_error = "grain split: the passes need " + std::to_string(most) + " B of shared memory, the device offers " +
						std::to_string(gpu->glsl.max_shmem_size) + " B";
					return false;
				}
			}
			const StorageChoice choice = ChooseStorage(options.storage, plane->params.format, codeUnit);
			m_storageNote = choice.why;
			const bool compact = choice.storage == GrainSplit::Storage::Compact;
			const Targets& tg = options.targets;
			if (choice.downgraded && tg.splitB)
			{
				m_error = "grain split: targets are for the compact layout but storage fell back to full (" + choice.why + ")";
				return false;
			}
			const int w = plane->params.w, h = plane->params.h;
			const int bw = (w + B - 1) / B, bh = (h + B - 1) / B, hw = HalfOf(w), hh = HalfOf(h);
			const pl_fmt f4 = FloatFormat(gpu, 4), f2 = FloatFormat(gpu, 2);
			pl_fmt fA = nullptr, fB = nullptr, fBlocks = f4;
			if (compact)
			{
				fA = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG16F);
				fB = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG16);
				if (!fA || !fB || !f4)
				{
					m_error = "grain split: no compact storage formats (rg16f, rg16, rgba32f)";
					return false;
				}
			}
			if (!f4 || !f2)
			{
				m_error = "grain split: no rgba32f / rg32f storage texture";
				return false;
			}
			auto usable = [&](pl_tex t, int tw, int th, pl_fmt fmt) { return t && t->params.w == tw && t->params.h == th && t->params.format == fmt && t->params.sampleable && t->params.storable; };
			if ((tg.split && !usable(tg.split, w, h, compact ? fA : f4)) || (tg.blocks && !usable(tg.blocks, bw, bh, fBlocks)) ||
				(compact && tg.splitB && !usable(tg.splitB, w, h, fB)))
			{
				m_error = "grain split: unusable target texture";
				return false;
			}

			// Intermediates (half-resolution tmp + low, block-grid quiet): from the arena (scope closed on every exit), else private.
			pl_tex tmp = nullptr, low = nullptr, quiet = nullptr;
			if (options.arena)
			{
				// The private temporaries of the non-arena path are not used any more: free them.
				for (pl_tex* t : { &m_tmp, &m_low, &m_quiet })
					if (*t) pl_tex_destroy(gpu, t);
				VpSkeleton::TexFormat t2 = VpSkeleton::TexFormat::RG32F, t4 = VpSkeleton::TexFormat::RGBA32F;
				if (!VpSkeleton::PlTexBackend::FormatOf(f2, &t2) || !VpSkeleton::PlTexBackend::FormatOf(f4, &t4))
				{
					m_error = "grain split: rg32f / rgba32f is not an arena format";
					return false;
				}
				VpSkeleton::TexShape half, qshape;
				half.w = static_cast<uint32_t>(hw); half.h = static_cast<uint32_t>(hh); half.fmt = t2;
				qshape.w = static_cast<uint32_t>(bw); qshape.h = static_cast<uint32_t>(bh); qshape.fmt = t4;
				tmp = options.arena->Acquire(kArenaOwner, half);
				low = options.arena->Acquire(kArenaOwner, half);
				quiet = options.arena->Acquire(kArenaOwner, qshape);
				if (!tmp || !low || !quiet)
				{
					m_scratchShort = true;
					m_error = std::string("scratch memory short (grain split): ") + (!tmp ? "tmp " : "") + (!low ? "low " : "") +
						(!quiet ? "quiet " : "") + "unavailable";
					return false;
				}
			}
			else
			{
				if (!Ensure(gpu, m_tmp, hw, hh, f2) || !Ensure(gpu, m_low, hw, hh, f2) || !Ensure(gpu, m_quiet, bw, bh, f4))
				{
					m_error = "grain split: no rg32f / rgba32f storage texture";
					return false;
				}
				tmp = m_tmp;
				low = m_low;
				quiet = m_quiet;
			}
			// Outputs: the caller's targets, else the Splitter's own.
			pl_tex outA = nullptr, outB = nullptr, outFull = nullptr, blocksTex = tg.blocks;
			if (compact)
			{
				outA = tg.split;
				outB = tg.splitB;
				if ((!outA && !Ensure(gpu, m_outA, w, h, fA)) || (!outB && !Ensure(gpu, m_outB, w, h, fB)))
				{
					m_error = "grain split: no compact output textures";
					return false;
				}
				if (!outA) outA = m_outA;
				if (!outB) outB = m_outB;
			}
			else
			{
				outFull = tg.split;
				if (!outFull && !Ensure(gpu, m_outFull, w, h, f4))
				{
					m_error = "grain split: no rgba32f storage texture";
					return false;
				}
				if (!outFull) outFull = m_outFull;
			}
			if (!blocksTex)
			{
				if (!Ensure(gpu, m_blocks, bw, bh, fBlocks))
				{
					m_error = "grain split: no rgba32f storage texture";
					return false;
				}
				blocksTex = m_blocks;
			}
			m_run.active = true;
			m_run.next = 0;
			m_run.compact = compact;
			m_run.storage = choice.storage;
			m_run.w = w; m_run.h = h; m_run.bw = bw; m_run.bh = bh;
			m_run.tmp = tmp; m_run.low = low; m_run.quiet = quiet; m_run.blocks = blocksTex;
			m_run.outFull = outFull; m_run.outA = outA; m_run.outB = outB;
			return true;
		}
	}
}
