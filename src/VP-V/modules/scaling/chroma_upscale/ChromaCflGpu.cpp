// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale GPU passes (ChromaCflGpu.h). Mirrors ChromaUpscale::Upsample (ChromaCfl.h) pass by pass; the model's
// steps are named in the comments (A grain guard, B luma at the sites, C regression, D upsample, E bars).
// SA7: with Params::arena the chroma-grid temporaries (lo, ab, rho, lohi, avg) and the own splits' intermediates are
// arena temporaries (one scope per Apply, opened after the own splits, whose scope is their own).
#include <pch.h>

#include <VP-V/modules/scaling/chroma_upscale/ChromaCflGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ChromaUpscale
{
	namespace Gpu
	{
		namespace
		{
			// Compute workgroup edge (16 x 16 invocations) and Catmull-Rom 4-tap footprint (taps floor(u)-1 .. floor(u)+2).
			constexpr int kWg = 16;
			constexpr int kCrTaps = 4;

			// A bit-exact copy of a rectangle of a plane (so every pass treats the picture's border as the plane edge).
			const char* const CROP_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < cfl_crop_size.x && p.y < cfl_crop_size.y) imageStore(crop_out, p, texelFetch(src, p + cfl_crop_org, 0));
)GLSL";

			// B: luma at the chroma sites (separable tent, rows inner then columns); A: the clean chroma.
			// GEM-074: chroma grain (raw - cc) is computed inline in UP_BODY instead of writing/storing an intermediate gc texture.
			const char* const LO_BODY = R"GLSL(
ivec2 j = ivec2(gl_GlobalInvocationID.xy);
if (j.x < cfl_csize.x && j.y < cfl_csize.y) {
    float px = (float(j.x + cfl_corg.x) + 0.5) * cfl_scale.x + cfl_shift.x - 0.5;
    float py = (float(j.y + cfl_corg.y) + 0.5) * cfl_scale.y + cfl_shift.y - 0.5;
    int x0 = int(ceil(px - cfl_scale.x)), x1 = int(floor(px + cfl_scale.x));
    int y0 = int(ceil(py - cfl_scale.y)), y1 = int(floor(py + cfl_scale.y));
    float wsx = 0.0, wsy = 0.0;
    for (int x = x0; x <= x1; x++) wsx += max(0.0, 1.0 - abs(float(x) - px) / cfl_scale.x);
    for (int y = y0; y <= y1; y++) wsy += max(0.0, 1.0 - abs(float(y) - py) / cfl_scale.y);
    float acc = 0.0;
    for (int y = y0; y <= y1; y++) {
        float wy = max(0.0, 1.0 - abs(float(y) - py) / cfl_scale.y);
        float rs = 0.0;
        for (int x = x0; x <= x1; x++) {
            float wx = max(0.0, 1.0 - abs(float(x) - px) / cfl_scale.x);
            rs += wx * cfl_yc(ivec2(clamp(x - cfl_lrect.x, 0, cfl_lrect.z - 1), clamp(y - cfl_lrect.y, 0, cfl_lrect.w - 1)));
        }
        acc += (wy / wsy) * (rs / wsx);
    }
    vec2 cc = cfl_cc(j);
    imageStore(o_lo, j, vec4(acc, cc.x, cc.y, 0.0));
}
)GLSL";

			// C: fused Cb/Cr regression in (2*CFL_R + 1)^2 (centred two-pass moments, shared mY/vY) and neighbour range in
			// (2*CFL_CLAMPR + 1)^2, tiled into shared memory sh_lo[CFL_ST_T][CFL_ST_T] (20x20 for CFL_R=2, CFL_CLAMPR=1).
			const char* const STATS_HEADER = R"GLSL(
shared vec4 sh_lo[CFL_ST_T][CFL_ST_T];
)GLSL";

			const char* const STATS_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * CFL_WG;
for (uint idx = li; idx < uint(CFL_ST_T * CFL_ST_T); idx += uint(CFL_WG * CFL_WG)) {
    int ty = int(idx / uint(CFL_ST_T));
    int tx = int(idx % uint(CFL_ST_T));
    ivec2 q = ivec2(clamp(org.x - CFL_ST_H + tx, 0, cfl_csize.x - 1), clamp(org.y - CFL_ST_H + ty, 0, cfl_csize.y - 1));
    sh_lo[ty][tx] = texelFetch(t_lo, q, 0);
}
barrier();
ivec2 j = org + lid;
if (j.x < cfl_csize.x && j.y < cfl_csize.y) {
    int cx = lid.x + CFL_ST_H;
    int cy = lid.y + CFL_ST_H;
    float sy = 0.0, scB = 0.0, scR = 0.0;
    for (int jj = -CFL_R; jj <= CFL_R; jj++)
        for (int ii = -CFL_R; ii <= CFL_R; ii++) {
            vec4 t = sh_lo[cy + jj][cx + ii];
            sy += t.x;
            scB += t.y;
            scR += t.z;
        }
    float n = float((2 * CFL_R + 1) * (2 * CFL_R + 1));
    float mY = sy / n, mCB = scB / n, mCR = scR / n;
    float syy = 0.0, sccB = 0.0, sycB = 0.0, sccR = 0.0, sycR = 0.0;
    for (int jj = -CFL_R; jj <= CFL_R; jj++)
        for (int ii = -CFL_R; ii <= CFL_R; ii++) {
            vec4 t = sh_lo[cy + jj][cx + ii];
            float dy = t.x - mY;
            float dcB = t.y - mCB, dcR = t.z - mCR;
            syy += dy * dy;
            sccB += dcB * dcB;
            sycB += dy * dcB;
            sccR += dcR * dcR;
            sycR += dy * dcR;
        }
    float vY = max(syy / n, 0.0);
    float vCB = max(sccB / n, 0.0), covB = sycB / n;
    float vCR = max(sccR / n, 0.0), covR = sycR / n;
    float aB = covB / (vY + cfl_eps), bB = mCB - aB * mY;
    float aR = covR / (vY + cfl_eps), bR = mCR - aR * mY;
    float rho2B = clamp(covB * covB / ((vY + cfl_eps) * (vCB + cfl_eps)), 0.0, 1.0);
    float rho2R = clamp(covR * covR / ((vY + cfl_eps) * (vCR + cfl_eps)), 0.0, 1.0);
    float mnB = 1e30, mxB = -1e30, mnR = 1e30, mxR = -1e30;
    for (int jj = -CFL_CLAMPR; jj <= CFL_CLAMPR; jj++)
        for (int ii = -CFL_CLAMPR; ii <= CFL_CLAMPR; ii++) {
            vec4 t = sh_lo[cy + jj][cx + ii];
            mnB = min(mnB, t.y);
            mxB = max(mxB, t.y);
            mnR = min(mnR, t.z);
            mxR = max(mxR, t.z);
        }
    imageStore(o_ab, j, vec4(aB, bB, aR, bR));
    imageStore(o_rho, j, vec4(rho2B, rho2R, 0.0, 0.0));
    imageStore(o_lohi, j, vec4(mnB, mxB, mnR, mxR));
}
)GLSL";

			// The guided filter's second box average of a and b, tiled into sh_ab[CFL_AVG_T][CFL_AVG_T] (20x20 for CFL_R=2).
			const char* const AVG_HEADER = R"GLSL(
shared vec4 sh_ab[CFL_AVG_T][CFL_AVG_T];
)GLSL";

			const char* const AVG_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * CFL_WG;
for (uint idx = li; idx < uint(CFL_AVG_T * CFL_AVG_T); idx += uint(CFL_WG * CFL_WG)) {
    int ty = int(idx / uint(CFL_AVG_T));
    int tx = int(idx % uint(CFL_AVG_T));
    ivec2 q = ivec2(clamp(org.x - CFL_AVG_H + tx, 0, cfl_csize.x - 1), clamp(org.y - CFL_AVG_H + ty, 0, cfl_csize.y - 1));
    sh_ab[ty][tx] = texelFetch(t_ab, q, 0);
}
barrier();
ivec2 j = org + lid;
if (j.x < cfl_csize.x && j.y < cfl_csize.y) {
    int cx = lid.x + CFL_AVG_H;
    int cy = lid.y + CFL_AVG_H;
    vec4 s = vec4(0.0);
    for (int jj = -CFL_R; jj <= CFL_R; jj++)
        for (int ii = -CFL_R; ii <= CFL_R; ii++)
            s += sh_ab[cy + jj][cx + ii];
    float n = float((2 * CFL_R + 1) * (2 * CFL_R + 1));
    imageStore(o_avg, j, s / n);
}
)GLSL";

			const char* const UP_FUNCS = R"GLSL(
shared vec4 sh_avg[CFL_UP_TH][CFL_UP_TW];
shared vec4 sh_lohi[CFL_UP_TH][CFL_UP_TW];
shared vec4 sh_rp[CFL_UP_TH][CFL_UP_TW];
shared vec4 sh_bg[CFL_UP_TH][CFL_UP_TW];

vec4 cfl_cr(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
)GLSL";

			// D inside the active picture (CfL on the crop grid, tiled into shared memory), E outside (bar-side Catmull-Rom of raw chroma).
			const char* const UP_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * CFL_WG;
float pu0 = (float(org.x) + 0.5 - cfl_shift.x) / cfl_scale.x - 0.5;
float pv0 = (float(org.y) + 0.5 - cfl_shift.y) / cfl_scale.y - 0.5;
ivec2 tileOrg = ivec2(int(floor(pu0 - float(cfl_crect.x))) - 1, int(floor(pv0 - float(cfl_crect.y))) - 1);
bool wgInside = org.x < cfl_active.z && (org.x + CFL_WG) > cfl_active.x && org.y < cfl_active.w && (org.y + CFL_WG) > cfl_active.y;
if (wgInside) {
    for (uint idx = li; idx < uint(CFL_UP_TW * CFL_UP_TH); idx += uint(CFL_WG * CFL_WG)) {
        int ty = int(idx / uint(CFL_UP_TW));
        int tx = int(idx % uint(CFL_UP_TW));
        ivec2 q = ivec2(clamp(tileOrg.x + tx, 0, cfl_csize.x - 1), clamp(tileOrg.y + ty, 0, cfl_csize.y - 1));
        vec4 av = texelFetch(t_avg, q, 0);
        vec4 lo = texelFetch(t_lo, q, 0);
        vec2 rho = texelFetch(t_rho, q, 0).xy;
        vec4 lohi = texelFetch(t_lohi, q, 0);
        vec2 pred = vec2(av.x * lo.x + av.y, av.z * lo.x + av.w);
#if CFL_GUARD
        vec2 gc = cfl_csrc(q + cfl_crect.xy) - lo.yz;
#else
        vec2 gc = vec2(0.0);
#endif
        sh_avg[ty][tx] = av;
        sh_lohi[ty][tx] = lohi;
        sh_rp[ty][tx] = vec4(rho, pred);
        sh_bg[ty][tx] = vec4(lo.yz, gc);
    }
}
barrier();
ivec2 p = org + lid;
if (p.x < cfl_size.x && p.y < cfl_size.y) {
    float pu = (float(p.x) + 0.5 - cfl_shift.x) / cfl_scale.x - 0.5;
    float pv = (float(p.y) + 0.5 - cfl_shift.y) / cfl_scale.y - 0.5;
    vec2 res = vec2(0.0);
    bool inside = p.x >= cfl_active.x && p.x < cfl_active.z && p.y >= cfl_active.y && p.y < cfl_active.w;
    if (inside) {
        float u = pu - float(cfl_crect.x), v = pv - float(cfl_crect.y);
        int iu = int(floor(u)), iv = int(floor(v));
        vec4 wu4 = cfl_cr(u - float(iu)), wv4 = cfl_cr(v - float(iv));
        float wu[4] = float[4](wu4.x, wu4.y, wu4.z, wu4.w);
        float wv[4] = float[4](wv4.x, wv4.y, wv4.z, wv4.w);
        int bx = iu - 1 - tileOrg.x;
        int by = iv - 1 - tileOrg.y;
        vec4 sA = vec4(0.0), sL = vec4(0.0);
        vec2 sR = vec2(0.0), sG = vec2(0.0), sB = vec2(0.0), sP = vec2(0.0);
        for (int m = 0; m < 4; m++) {
            int ty = by + m;
            vec4 rA = vec4(0.0), rL = vec4(0.0);
            vec2 rR = vec2(0.0), rG = vec2(0.0), rB = vec2(0.0), rP = vec2(0.0);
            for (int n = 0; n < 4; n++) {
                int tx = bx + n;
                vec4 av = sh_avg[ty][tx];
                vec4 lh = sh_lohi[ty][tx];
                vec4 rp = sh_rp[ty][tx];
                vec4 bg = sh_bg[ty][tx];
                rA += wu[n] * av;
                rP += wu[n] * rp.zw;
                rR += wu[n] * rp.xy;
                rL += wu[n] * lh;
                rB += wu[n] * bg.xy;
                rG += wu[n] * bg.zw;
            }
            sA += wv[m] * rA;
            sL += wv[m] * rL;
            sR += wv[m] * rR;
            sB += wv[m] * rB;
            sG += wv[m] * rG;
            sP += wv[m] * rP;
        }
        vec2 am = vec2(sA.x, sA.z), bm = vec2(sA.y, sA.w);
        vec2 mm = clamp(sR, 0.0, 1.0);
        float yv = cfl_yc(p - cfl_active.xy);
        vec2 lo2 = min(vec2(sL.x, sL.z), vec2(sL.y, sL.w));
        vec2 hi2 = max(vec2(sL.x, sL.z), vec2(sL.y, sL.w));
        res = sB + mm * (clamp(am * yv + bm, lo2, hi2) - sP) + sG;
    } else {
        int xlo = 0, xhi = cfl_psize.x - 1, ylo = 0, yhi = cfl_psize.y - 1;
        if (p.x < cfl_active.x) xhi = max(cfl_crect.x - 1, 0);
        else if (p.x >= cfl_active.z) xlo = min(cfl_crect.z, cfl_psize.x - 1);
        if (p.y < cfl_active.y) yhi = max(cfl_crect.y - 1, 0);
        else if (p.y >= cfl_active.w) ylo = min(cfl_crect.w, cfl_psize.y - 1);
        int iu = int(floor(pu)), iv = int(floor(pv));
        vec4 wu4 = cfl_cr(pu - float(iu)), wv4 = cfl_cr(pv - float(iv));
        float wu[4] = float[4](wu4.x, wu4.y, wu4.z, wu4.w);
        float wv[4] = float[4](wv4.x, wv4.y, wv4.z, wv4.w);
        vec2 sr = vec2(0.0);
        for (int m = 0; m < 4; m++) {
            int yy = clamp(iv - 1 + m, ylo, yhi);
            vec2 rs = vec2(0.0);
            for (int n = 0; n < 4; n++) rs += wu[n] * cfl_csrc(ivec2(clamp(iu - 1 + n, xlo, xhi), yy));
            sr += wv[m] * rs;
        }
        res = sr;
    }
    imageStore(o_up, p, vec4(res * cfl_cu_out, 0.0, 0.0));
}
)GLSL";

			// vec2 fn(ivec2 p): (Cb, Cr) in codes from the descriptor(s) d0 / d1 (d1 unused when one texture holds both).
			std::string RawReader(const char* fn, const char* d0, const char* d1, bool same, int comp0, int comp1)
			{
				static const char* const kComp = "xyzw";
				return std::string("vec2 ") + fn + "(ivec2 p)\n{\n    return vec2(texelFetch(" + d0 + ", p, 0)." + kComp[comp0] + ", texelFetch(" +
					(same ? d0 : d1) + ", p, 0)." + kComp[comp1] + ") * cfl_inv_cu_c;\n}\n";
			}

			int Groups(int n) { return (n + kWg - 1) / kWg; }

			// Derived worst-case chroma-site tile edge for a kWg-pixel workgroup under 4-tap Catmull-Rom, independent of siting
			// shift and active crop so varying siting, crop, or same-class plane size compiles zero new shaders.
			// Over lid in [0, kWg - 1], exact u(lid) - u(0) = span = (kWg - 1) / ratio. With arbitrary phase f0 in [0, 1) and
			// float32 rounding across at most 4 coordinate ops at GPU texture dimensions <= 32768 (2^15 px, >= D3D11 16384 max),
			// the endpoint difference error is bounded by eta = 2 * 4 * 0.5 * 32768 * FLT_EPSILON = 2^-6. Because f0 < 1:
			//   floor(u(lid)) - floor(u(0)) = floor(f0 + span + err) <= ceil(span + eta),
			// so bx + (kCrTaps - 1) < ceil(span + eta) + kCrTaps unconditionally:
			//   ratio = 2.0 (4:2:0): ceil( 7.5 + 2^-6) + 4 = 12  (12 x 12 x 64 B =  9,216 B < 16 KB)
			//   ratio = 1.0 (4:2:2): ceil(15.0 + 2^-6) + 4 = 20  (12 x 20 x 64 B = 15,360 B < 16 KB)
			//   ratio = 4.0 (4:1:0): ceil( 3.75 + 2^-6) + 4 = 8  ( 8 x  8 x 64 B =  4,096 B < 16 KB)
			int UpTileDim(double ratio)
			{
				constexpr double kMaxCoord = 32768.0; // 2^15 px (>= D3D11 16384 max texture edge)
				constexpr double kCoordOps = 4.0;     // +0.5, -shift, /ratio, -0.5-cx0 (both endpoints -> 2 * 4 * 0.5 ULP)
				const double eta = kCoordOps * kMaxCoord * static_cast<double>(std::numeric_limits<float>::epsilon());
				const double span = static_cast<double>(kWg - 1) / ratio;
				return static_cast<int>(std::ceil(span + eta)) + kCrTaps;
			}

			// SA7: opens the arena scope of one Apply and closes it on every exit path. A refused (nested) scope is not ours to close;
			// its Acquires then fail visibly (the arena counts it).
			struct ArenaScopeGuard
			{
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena;
				bool opened = false;
				ArenaScopeGuard(VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* a, uint16_t scope) : arena(a) { if (arena) opened = arena->BeginScope(scope); }
				~ArenaScopeGuard() { if (arena && opened) arena->EndScope(); }
				ArenaScopeGuard(const ArenaScopeGuard&) = delete;
				ArenaScopeGuard& operator=(const ArenaScopeGuard&) = delete;
			};

			// One plane's grain accessor under its own names (the luma, Cb and Cr frames may differ in layout, and one shader
			// binds several). The ONE read accessor (GrainSplit::Gpu::ViewGlsl / BindView) with every name prefixed per plane:
			// plane 0 "gsL_", 1 "gsB_", 2 "gsR_" (not "gl_": reserved in GLSL). Appends the prelude to `glsl`, the plane's
			// sampled descriptors to `descs`, and (for Compact storage) replaces the baked `const float gs<P>_cu = ...;`
			// declaration with a dynamic pl_shader_var backed by `cuStorage` so code-unit changes compile zero new shaders.
			void AddGrainView(const GrainSplit::Gpu::Frame& f, int plane, std::string& glsl, std::vector<pl_shader_desc>& descs,
				std::vector<pl_shader_var>& vars, float& cuStorage)
			{
				static const char* const kNames[3][4] = {
					{ "gsL_split", "gsL_gblocks", "gsL_a", "gsL_b" },
					{ "gsB_split", "gsB_gblocks", "gsB_a", "gsB_b" },
					{ "gsR_split", "gsR_gblocks", "gsR_a", "gsR_b" } };
				static const char* const kPfx[3] = { "gsL_", "gsB_", "gsR_" };
				static const char* const kCuNames[3] = { "gsL_cu", "gsB_cu", "gsR_cu" };
				auto replaceAll = [](std::string& t, const std::string& from, const std::string& to) {
					for (size_t at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size()))
						t.replace(at, from.size(), to);
				};
				std::string v = GrainSplit::Gpu::ViewGlsl(f.storage, f.codeUnit);
				replaceAll(v, "gs_", kPfx[plane]);                                        // gs_fetch, gs_blocks, gs_a, gs_b, gs_cu
				replaceAll(v, "texelFetch(split,", std::string("texelFetch(") + kNames[plane][0] + ",");   // Full: the sampler names
				replaceAll(v, "texelFetch(gblocks,", std::string("texelFetch(") + kNames[plane][1] + ",");
				if (f.storage == GrainSplit::Storage::Compact)
				{
					const std::string cuDecl = std::string("const float ") + kCuNames[plane] + " = ";
					const size_t pos = v.find(cuDecl);
					const size_t end = pos != std::string::npos ? v.find(";\n", pos) : std::string::npos;
					if (pos != std::string::npos && end != std::string::npos)
					{
						v.erase(pos, end - pos + 2);
						cuStorage = static_cast<float>(f.codeUnit);
						vars.push_back({ pl_var_float(kCuNames[plane]), &cuStorage, true });
					}
				}
				glsl += v;
				std::vector<pl_shader_desc> bound;
				GrainSplit::Gpu::BindView(f, bound);
				for (pl_shader_desc& d : bound)
				{
					const char* n = d.desc.name;
					if (!std::strcmp(n, "split")) d.desc.name = kNames[plane][0];
					else if (!std::strcmp(n, "gblocks")) d.desc.name = kNames[plane][1];
					else if (!std::strcmp(n, "gs_a")) d.desc.name = kNames[plane][2];
					else if (!std::strcmp(n, "gs_b")) d.desc.name = kNames[plane][3];
					descs.push_back(d);
				}
			}
		}

		struct CflModule::Impl
		{
			GrainSplit::Gpu::Splitter splitL, splitB, splitR;
			pl_tex cropL = nullptr, cropB = nullptr, cropR = nullptr;
			pl_tex lo = nullptr, ab = nullptr, rho = nullptr, lohi = nullptr, avg = nullptr, out = nullptr;
		};

		CflModule::~CflModule() { delete m_impl; }

		void CflModule::Release(pl_gpu gpu)
		{
			if (m_impl && gpu)
			{
				Impl& m = *m_impl;
				m.splitL.Release(gpu);
				m.splitB.Release(gpu);
				m.splitR.Release(gpu);
				for (pl_tex* t : { &m.cropL, &m.cropB, &m.cropR, &m.lo, &m.ab, &m.rho, &m.lohi, &m.avg, &m.out })
					if (*t) pl_tex_destroy(gpu, t);
			}
			delete m_impl;
			m_impl = nullptr;
		}

		uint64_t CflModule::TextureBytes(int W, int H, int cw, int ch, bool tempsInArena)
		{
			const uint64_t px = static_cast<uint64_t>(W) * static_cast<uint64_t>(H);
			const uint64_t cpx = static_cast<uint64_t>(cw) * static_cast<uint64_t>(ch);
			const uint64_t nbL = static_cast<uint64_t>((W + 15) / 16) * static_cast<uint64_t>((H + 15) / 16);
			const uint64_t nbC = static_cast<uint64_t>((cw + 15) / 16) * static_cast<uint64_t>((ch + 15) / 16);
			uint64_t b = 0;
			b += px * (2 + 32 + 4);                       // luma crop (16-bit), luma split (rows/split + bands, rgba32f), output (rg16)
			b += nbL * 32;                                // luma split block grids (2 x rgba32f)
			b += cpx * (4 + 2 * 32);                      // chroma crop (rg16), two chroma splits
			b += nbC * 2 * 32;                            // chroma split block grids
			b += cpx * (16 + 16 + 8 + 16 + 16);           // lo, ab, rho, lohi, avg (GEM-074: gc removed -> -8 B/cpx)
			// SA7 + GEM-074: lo..avg (72 B per chroma pixel) and the splits' bands (16 per luma pixel, 2 x 16 per chroma pixel) are the arena's.
			if (tempsInArena) b -= 16 * px + 104 * cpx;
			return b;
		}

		std::vector<VpSkeleton::ArenaRequest> CflModule::GridRequests(VpSkeleton::TexFormat t4, VpSkeleton::TexFormat t2, int pcw, int pch)
		{
			std::vector<VpSkeleton::ArenaRequest> r;
			auto add = [&](VpSkeleton::TexFormat f, int count) {
				VpSkeleton::ArenaRequest q;
				q.scope = kArenaScope;
				q.owner = kArenaOwner;
				q.tier = VpSkeleton::Tier::T1;
				q.shape.w = static_cast<uint32_t>(pcw);
				q.shape.h = static_cast<uint32_t>(pch);
				q.shape.fmt = f;
				q.count = static_cast<uint8_t>(count);
				for (VpSkeleton::ArenaRequest& have : r)
					if (have.shape == q.shape)   // same scope and owner by construction: held together, so the counts add
					{
						have.count = static_cast<uint8_t>(have.count + count);
						return;
					}
				r.push_back(q);
			};
			add(t4, 4);   // lo, ab, lohi, avg
			add(t2, 1);   // rho (GEM-074: gc removed, grain computed inline as raw - cc in UP_BODY)
			return r;
		}

		std::vector<VpSkeleton::ArenaRequest> CflModule::ArenaRequests(pl_gpu gpu, int rw, int rh, int pcw, int pch, double lumaLines,
			double chromaLines, bool ownSplit)
		{
			std::vector<VpSkeleton::ArenaRequest> r;
			VpSkeleton::TexFormat t4{}, t2{};
			if (!gpu || rw < 1 || rh < 1 || pcw < 1 || pch < 1 ||
				!VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 4), &t4) || !VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 2), &t2))
				return r;
			r = GridRequests(t4, t2, pcw, pch);
			if (ownSplit)
			{
				// The three splits run one after the other, each in its own scope: one request per distinct shape is enough.
				std::vector<VpSkeleton::ArenaRequest> splits = GrainSplit::Gpu::Splitter::ArenaRequests(gpu, rw, rh, BlockPx(lumaLines), GrainSplit::Storage::Full);
				const std::vector<VpSkeleton::ArenaRequest> chroma = GrainSplit::Gpu::Splitter::ArenaRequests(gpu, pcw, pch, BlockPx(chromaLines), GrainSplit::Storage::Full);
				splits.insert(splits.end(), chroma.begin(), chroma.end());
				for (const VpSkeleton::ArenaRequest& s : splits)
				{
					bool merged = false;
					for (VpSkeleton::ArenaRequest& have : r)
						if (have.scope == s.scope && have.shape == s.shape && std::string(have.owner) == std::string(s.owner))
						{
							have.count = (std::max)(have.count, s.count);
							merged = true;
						}
					if (!merged) r.push_back(s);
				}
			}
			return r;
		}

		bool CflModule::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
		{
			using namespace VpvGpu;
			m_error.clear();
			m_scratchShort = false;
			if (params.settings.mode == Mode::Off)
			{
				m_status = "off";
				m_grainStatus = GrainSplit::GrainStatus::Ring;   // not reading the grain: no fallback to show
				result = source;
				return true;
			}
			auto fail = [&](const std::string& why) {
				m_error = "chroma upscale: " + why;
				m_status = m_error;
				++m_failures;
				return false;
			};
			if (!gpu || !dispatch || source.num_planes < 2 || source.num_planes > PL_MAX_PLANES) return fail("a luma plane and chroma plane(s) are needed");
			if (!pl_color_system_is_ycbcr_like(source.repr.sys)) return fail("the frame is not YCbCr");
			const pl_plane& lp = source.planes[0];
			const pl_tex luma = lp.texture;
			if (!luma || !luma->params.sampleable || lp.components != 1 || lp.component_mapping[0] != PL_CHANNEL_Y)
				return fail("plane 0 is not a sampleable luma plane");

			// The chroma layout: one plane with CB and CR, or one plane each.
			pl_tex cbTex = nullptr, crTex = nullptr;
			int cbComp = 0, crComp = 0;
			const pl_plane* cbPlane = nullptr;
			const pl_plane* crPlane = nullptr;
			for (int i = 1; i < source.num_planes; ++i)
			{
				const pl_plane& pp = source.planes[i];
				if (!pp.texture) continue;
				for (int c = 0; c < pp.components && c < 4; ++c)
				{
					if (pp.component_mapping[c] == PL_CHANNEL_CB && !cbTex) { cbTex = pp.texture; cbComp = c; cbPlane = &pp; }
					else if (pp.component_mapping[c] == PL_CHANNEL_CR && !crTex) { crTex = pp.texture; crComp = c; crPlane = &pp; }
				}
			}
			if (!cbTex || !crTex) return fail("the chroma planes (CB, CR) were not found");
			if (!cbTex->params.sampleable || !crTex->params.sampleable) return fail("a chroma plane is not sampleable");
			if (cbTex->params.w != crTex->params.w || cbTex->params.h != crTex->params.h || cbTex->params.format != crTex->params.format)
				return fail("the Cb and Cr planes differ in size or format");
			if (std::fabs(cbPlane->shift_x - crPlane->shift_x) > 1e-6 || std::fabs(cbPlane->shift_y - crPlane->shift_y) > 1e-6)
				return fail("the Cb and Cr planes have different siting");

			const int W = luma->params.w, H = luma->params.h, cw = cbTex->params.w, ch = cbTex->params.h;
			if (cw == W && ch == H)
			{
				m_status = "4:4:4: nothing to upsample";
				m_grainStatus = GrainSplit::GrainStatus::Ring;   // not reading the grain: no fallback to show
				result = source;
				return true;
			}
			if (cw > W || ch > H || cw < 1 || ch < 1) return fail("the chroma plane is larger than the luma plane");
			const double fx = static_cast<double>(W) / cw, fy = static_cast<double>(H) / ch;
			const double sx = cbPlane->shift_x, sy = cbPlane->shift_y;
			const ActiveRect r = params.active.Resolve(W, H);
			const bool whole = r.left == 0 && r.top == 0 && r.right == W && r.bottom == H;
			const Rect cRect = ChromaRect(r, fx, fy, cw, ch);
			const bool chromaWhole = cRect.x0 == 0 && cRect.y0 == 0 && cRect.x1 == cw && cRect.y1 == ch;
			const int rw = r.Width(), rh = r.Height(), pcw = cRect.W(), pch = cRect.H();
			const bool guard = params.cfl.grainGuard;
			const bool sameTex = crTex == cbTex;

			if (!m_impl) m_impl = new Impl();
			Impl& m = *m_impl;

			// --- Textures ---
			const pl_fmt f4 = FloatFormat(gpu, 4), f2 = FloatFormat(gpu, 2), outFmt = StorableLike(gpu, cbTex->params.format, 2);
			if (!f4 || !f2) return fail("no float storage texture");
			if (!outFmt) return fail("no 16-bit two-component output format");
			const bool useArena = params.arena != nullptr;   // SA7 + GEM-074: lo, ab, rho, lohi, avg and the own splits' intermediates
			if (!(useArena || (Ensure(gpu, m.lo, pcw, pch, f4) && Ensure(gpu, m.ab, pcw, pch, f4) &&
				Ensure(gpu, m.rho, pcw, pch, f2) && Ensure(gpu, m.lohi, pcw, pch, f4) && Ensure(gpu, m.avg, pcw, pch, f4))) ||
				!Ensure(gpu, m.out, W, H, outFmt, true))
				return fail("no storage texture for the module");
			if (useArena)
				for (pl_tex* t : { &m.lo, &m.ab, &m.rho, &m.lohi, &m.avg })   // the private temporaries of the non-arena path: free them
					if (*t) pl_tex_destroy(gpu, t);

			// --- A: the grain guard (the shared grain split, three planes) ---
			const double cuL = CodeUnit(luma, source.repr.bits, params.colorDepth);
			const double cuC = CodeUnit(cbTex, source.repr.bits, params.colorDepth);
			// The output carries the frame's one repr: write chroma in the input chroma's units (as the input would decode).
			if (m.out->params.format->type == PL_FMT_FLOAT && cbTex->params.format->type != PL_FMT_FLOAT && source.repr.bits.bit_shift != 0)
				return fail("chroma upscale: no storable unorm output for a bit-shifted chroma plane");
			const double cuOut = cuC;
			const double lines = params.lumaLines > 0.0 ? params.lumaLines : static_cast<double>(H);
			const double linesC = lines / fy;
			// The shared grain.split products when they fit (one producer, many readers), else this module's own (visible).
			const GrainSplit::Gpu::Frame* gL = nullptr;
			const GrainSplit::Gpu::Frame* gB = nullptr;
			const GrainSplit::Gpu::Frame* gR = nullptr;
			bool ownSplit = false, misfit = false;
			bool usedCropL = false, usedCropB = false, usedCropR = false;
			m_grainStatus = GrainSplit::GrainStatus::Ring;   // recomputed every frame: a fitting product is read again at once
			if (guard)
			{
				// Active-picture crop into a plane's own format, lazily dispatched ONLY when an own-split fallback runs on a cropped plane.
				auto crop = [&](pl_tex src, pl_tex& dst, int x0, int y0, int w, int h, const char* what) -> bool {
					if (!(src->params.format->caps & PL_FMT_CAP_STORABLE))
					{
						fail(std::string("the ") + what + " format is not storable (active-picture crop)");
						return false;
					}
					if (!Ensure(gpu, dst, w, h, src->params.format))
					{
						fail(std::string("no texture for the ") + what + " crop");
						return false;
					}
					const int cropSize[2] = { w, h };
					const int cropOrg[2] = { x0, y0 };
					const pl_shader_desc cd[2] = { Sampled("src", src), Storage("crop_out", dst) };
					const pl_shader_var cv[2] = {
						{ pl_var_ivec2("cfl_crop_size"), cropSize, true },
						{ pl_var_ivec2("cfl_crop_org"), cropOrg, true } };
					if (!Pass(dispatch, "VideoProcessor chroma upscale: crop", "", CROP_BODY, cd, 2, cv, 2, Groups(w), Groups(h), 0))
					{
						fail(std::string(what) + " crop pass failed");
						return false;
					}
					return true;
				};
				bool stale = false, shedNow = false;
				auto pick = [&](const GrainSplit::Gpu::Frame* shared, GrainSplit::Gpu::Splitter& own, pl_tex srcTex, pl_tex& cropDst,
					bool& usedCrop, bool isWhole, int x0, int y0, int tw, int th, const char* what, int comp, double cu, double ln,
					bool isChroma, const GrainSplit::Gpu::Frame*& outF) -> bool {
					const GrainSplit::Gpu::FitResult fit = GrainSplit::Gpu::CheckFit(shared, tw, th, BlockPx(ln), Scale(ln), cu, comp,
						params.grainSeq);
					if (fit == GrainSplit::Gpu::FitResult::Fits && !params.grainShed) { outF = shared; return true; }
					ownSplit = true;
					if (params.grainShed) shedNow = true;
					if (shared && !params.grainShed) misfit = true;
					if (fit == GrainSplit::Gpu::FitResult::Stale) stale = true;
					m_grainStatus = shedNow ? GrainSplit::GrainStatus::Shed : stale ? GrainSplit::GrainStatus::Stale : (misfit ? GrainSplit::GrainStatus::WrongSize : GrainSplit::GrainStatus::Missing);
					pl_tex tex = srcTex;
					if (!isWhole)
					{
						if (!usedCrop)
						{
							if (!crop(srcTex, cropDst, x0, y0, tw, th, what)) return false;
							usedCrop = true;
						}
						tex = cropDst;
					}
					// W23-NEON-H5: pass actual colorDepth into GrainSplit::Params and isChroma for Cb/Cr fallback splits.
					GrainSplit::Params gp;
					gp.sourceBits = (params.colorDepth >= 8 && params.colorDepth <= 16) ? params.colorDepth : 10;
					GrainSplit::Gpu::Options splitOptions;
					splitOptions.arena = params.arena;
					splitOptions.isChroma = isChroma;
					const bool splitOk = own.Compute(gpu, dispatch, tex, comp, cu, Scale(ln), BlockPx(ln), gp, splitOptions);
					if (!splitOk)
					{
						if (own.ScratchShort()) m_scratchShort = true;
						return fail(own.LastError());
					}
					outF = &own.Out();
					return true;
				};
				using RegionEvidence::GrainPlane;
				if (!pick(params.grain.Gpu(GrainPlane::Luma), m.splitL, luma, m.cropL, usedCropL, whole, r.left, r.top, rw, rh, "luma", 0, cuL, lines, false, gL)) return false;
				if (!pick(params.grain.Gpu(GrainPlane::Cb), m.splitB, cbTex, m.cropB, usedCropB, chromaWhole, cRect.x0, cRect.y0, pcw, pch, "chroma", cbComp, cuC, linesC, true, gB)) return false;
				if (!pick(params.grain.Gpu(GrainPlane::Cr), m.splitR, crTex, sameTex ? m.cropB : m.cropR, sameTex ? usedCropB : usedCropR, chromaWhole, cRect.x0, cRect.y0, pcw, pch, "chroma", crComp, cuC, linesC, true, gR)) return false;
			}
			if (!usedCropL && m.cropL) pl_tex_destroy(gpu, &m.cropL);
			if (!usedCropB && m.cropB) pl_tex_destroy(gpu, &m.cropB);
			if (!usedCropR && m.cropR) pl_tex_destroy(gpu, &m.cropR);

			// --- The per-frame temporaries: the arena's (SA7) or this module's own ---
			// One scope for the rest of the Apply, opened only now: the own splits' scope above is closed, so they never nest.
			ArenaScopeGuard arenaScope(params.arena, kArenaScope);
			pl_tex loTex = m.lo, abTex = m.ab, rhoTex = m.rho, lohiTex = m.lohi, avgTex = m.avg;
			if (useArena)
			{
				loTex = abTex = rhoTex = lohiTex = avgTex = nullptr;
				VpSkeleton::TexFormat t4{}, t2{};
				if (VpSkeleton::PlTexBackend::FormatOf(f4, &t4) && VpSkeleton::PlTexBackend::FormatOf(f2, &t2))
				{
					const VpSkeleton::TexShape s4{ static_cast<uint32_t>(pcw), static_cast<uint32_t>(pch), t4 };
					const VpSkeleton::TexShape s2{ static_cast<uint32_t>(pcw), static_cast<uint32_t>(pch), t2 };
					loTex = params.arena->Acquire(kArenaOwner, s4);   // fixed order per shape: lo, ab, lohi, avg / rho
					abTex = params.arena->Acquire(kArenaOwner, s4);
					lohiTex = params.arena->Acquire(kArenaOwner, s4);
					avgTex = params.arena->Acquire(kArenaOwner, s4);
					rhoTex = params.arena->Acquire(kArenaOwner, s2);
				}
				if (!loTex || !abTex || !lohiTex || !avgTex || !rhoTex)
				{
					m_scratchShort = true;
					return fail("scratch memory short (the chroma-grid temporaries)");
				}
			}

			// --- Headers & dynamic uniforms (GEM-076: per-frame geometry/crop/siting/eps/scale are pl_shader_var uniforms) ---
			double peak = 1.0;
			for (int i = 0; i < params.colorDepth; ++i) peak *= 2.0;
			const double eps = (params.cfl.epsFrac * (peak - 1.0)) * (params.cfl.epsFrac * (peak - 1.0));
			const int stHalo = (std::max)(0, (std::max)(params.cfl.radius, params.cfl.clampR));
			const int stTile = kWg + 2 * stHalo;
			const int avgHalo = (std::max)(0, params.cfl.radius);
			const int avgTile = kWg + 2 * avgHalo;
			const int upTileW = UpTileDim(fx);
			const int upTileH = UpTileDim(fy);
			const size_t stShmem = static_cast<size_t>(stTile) * static_cast<size_t>(stTile) * 4u * sizeof(float);
			const size_t avgShmem = static_cast<size_t>(avgTile) * static_cast<size_t>(avgTile) * 4u * sizeof(float);
			const size_t upShmem = static_cast<size_t>(upTileW) * static_cast<size_t>(upTileH) * 4u * 4u * sizeof(float);
			const int uSize[2] = { W, H };
			const int uPsize[2] = { cw, ch };
			const int uActive[4] = { r.left, r.top, r.right, r.bottom };
			const int uLrect[4] = { r.left, r.top, rw, rh };
			const int uCrect[4] = { cRect.x0, cRect.y0, cRect.x1, cRect.y1 };
			const int uLorg[2] = { r.left, r.top };
			const int uCorg[2] = { cRect.x0, cRect.y0 };
			const int uCsize[2] = { pcw, pch };
			const float uScale[2] = { static_cast<float>(fx), static_cast<float>(fy) };
			const float uShift[2] = { static_cast<float>(sx), static_cast<float>(sy) };
			const float uEps = static_cast<float>(eps);
			const float uInvCuL = static_cast<float>(1.0 / cuL);
			const float uInvCuC = static_cast<float>(1.0 / cuC);
			const float uCuOut = static_cast<float>(cuOut);
			float uGsCu[3] = { 0.0f, 0.0f, 0.0f };
			// With the guard every grain read goes through the accessor of its plane's frame (prelude + BindView, per-plane names).
			std::string lumaView, chromaView;
			std::vector<pl_shader_desc> lumaDescs, chromaDescs;
			std::vector<pl_shader_var> lumaVars, chromaVars;
			if (guard)
			{
				AddGrainView(*gL, 0, lumaView, lumaDescs, lumaVars, uGsCu[0]);
				AddGrainView(*gB, 1, chromaView, chromaDescs, chromaVars, uGsCu[1]);
				AddGrainView(*gR, 2, chromaView, chromaDescs, chromaVars, uGsCu[2]);
			}
			const std::string lumaReader = guard ?
				lumaView + "float cfl_yc(ivec2 p)\n{\n    vec4 s = gsL_fetch(p);\n    return s.w + s.x + s.y;\n}\n" :
				"float cfl_yc(ivec2 p)\n{\n    return texelFetch(ysrc, p + cfl_lorg, 0).x * cfl_inv_cu_l;\n}\n";

			const int cgx = Groups(pcw), cgy = Groups(pch);

			// --- B + A (chroma): luma at the sites, clean chroma ---
			{
				std::string hdr = lumaReader;
				std::vector<pl_shader_desc> d;
				std::vector<pl_shader_var> v = {
					{ pl_var_ivec2("cfl_csize"), uCsize, true },
					{ pl_var_ivec2("cfl_corg"), uCorg, true },
					{ pl_var_ivec4("cfl_lrect"), uLrect, true },
					{ pl_var_vec2("cfl_scale"), uScale, true },
					{ pl_var_vec2("cfl_shift"), uShift, true } };
				if (guard)
				{
					hdr += chromaView + "vec2 cfl_cc(ivec2 j)\n{\n    vec4 b = gsB_fetch(j), r = gsR_fetch(j);\n    return vec2(b.w + b.x + b.y, r.w + r.x + r.y);\n}\n";
					d.insert(d.end(), lumaDescs.begin(), lumaDescs.end());
					d.insert(d.end(), chromaDescs.begin(), chromaDescs.end());
					v.insert(v.end(), lumaVars.begin(), lumaVars.end());
					v.insert(v.end(), chromaVars.begin(), chromaVars.end());
				}
				else
				{
					hdr += RawReader("cfl_craw", "craw0", "craw1", sameTex, cbComp, crComp) + "vec2 cfl_cc(ivec2 j)\n{\n    return cfl_craw(j + cfl_corg);\n}\n";
					d.push_back(Sampled("ysrc", luma));
					d.push_back(Sampled("craw0", cbTex));
					if (!sameTex) d.push_back(Sampled("craw1", crTex));
					v.push_back({ pl_var_ivec2("cfl_lorg"), uLorg, true });
					v.push_back({ pl_var_float("cfl_inv_cu_l"), &uInvCuL, true });
					v.push_back({ pl_var_float("cfl_inv_cu_c"), &uInvCuC, true });
				}
				d.push_back(Storage("o_lo", loTex));
				if (!Pass(dispatch, "VideoProcessor chroma upscale: luma at the sites", hdr, LO_BODY, d.data(), static_cast<int>(d.size()),
					v.data(), static_cast<int>(v.size()), cgx, cgy, 0, kWg, kWg))
					return fail("luma-at-sites pass failed");
			}
			// --- C: fused regression and neighbour range (tiled sh_lo[CFL_ST_T][CFL_ST_T]) ---
			{
				const std::string hdr = DefineInt("CFL_WG", kWg) + DefineInt("CFL_R", params.cfl.radius) +
					DefineInt("CFL_CLAMPR", params.cfl.clampR) + DefineInt("CFL_ST_H", stHalo) + DefineInt("CFL_ST_T", stTile) + STATS_HEADER;
				const pl_shader_desc d[4] = { Sampled("t_lo", loTex), Storage("o_ab", abTex), Storage("o_rho", rhoTex), Storage("o_lohi", lohiTex) };
				const pl_shader_var v[2] = {
					{ pl_var_ivec2("cfl_csize"), uCsize, true },
					{ pl_var_float("cfl_eps"), &uEps, true } };
				if (!Pass(dispatch, "VideoProcessor chroma upscale: regression", hdr, STATS_BODY, d, 4, v, 2, cgx, cgy, stShmem, kWg, kWg))
					return fail("regression pass failed");
			}
			// --- C2: box average of a and b (tiled sh_ab[CFL_AVG_T][CFL_AVG_T]) ---
			{
				const std::string hdr = DefineInt("CFL_WG", kWg) + DefineInt("CFL_R", params.cfl.radius) +
					DefineInt("CFL_AVG_H", avgHalo) + DefineInt("CFL_AVG_T", avgTile) + AVG_HEADER;
				const pl_shader_desc d[2] = { Sampled("t_ab", abTex), Storage("o_avg", avgTex) };
				const pl_shader_var v[1] = { { pl_var_ivec2("cfl_csize"), uCsize, true } };
				if (!Pass(dispatch, "VideoProcessor chroma upscale: averaging", hdr, AVG_BODY, d, 2, v, 1, cgx, cgy, avgShmem, kWg, kWg))
					return fail("averaging pass failed");
			}
			// --- D + E: the upsample (tiled active-picture chroma neighborhood + inline grain raw - cc) ---
			{
				std::string hdr = DefineInt("CFL_WG", kWg) + DefineInt("CFL_UP_TW", upTileW) + DefineInt("CFL_UP_TH", upTileH) +
					DefineInt("CFL_GUARD", guard ? 1 : 0) + lumaReader + UP_FUNCS + RawReader("cfl_csrc", "csrc0", "csrc1", sameTex, cbComp, crComp);
				std::vector<pl_shader_desc> d;
				std::vector<pl_shader_var> v = {
					{ pl_var_ivec2("cfl_size"), uSize, true },
					{ pl_var_ivec2("cfl_psize"), uPsize, true },
					{ pl_var_ivec4("cfl_active"), uActive, true },
					{ pl_var_ivec4("cfl_crect"), uCrect, true },
					{ pl_var_ivec2("cfl_csize"), uCsize, true },
					{ pl_var_vec2("cfl_scale"), uScale, true },
					{ pl_var_vec2("cfl_shift"), uShift, true },
					{ pl_var_float("cfl_inv_cu_c"), &uInvCuC, true },
					{ pl_var_float("cfl_cu_out"), &uCuOut, true } };
				if (guard)
				{
					d.insert(d.end(), lumaDescs.begin(), lumaDescs.end());
					v.insert(v.end(), lumaVars.begin(), lumaVars.end());
				}
				else
				{
					d.push_back(Sampled("ysrc", luma));
					v.push_back({ pl_var_ivec2("cfl_lorg"), uLorg, true });
					v.push_back({ pl_var_float("cfl_inv_cu_l"), &uInvCuL, true });
				}
				d.push_back(Sampled("t_avg", avgTex));
				d.push_back(Sampled("t_rho", rhoTex));
				d.push_back(Sampled("t_lohi", lohiTex));
				d.push_back(Sampled("t_lo", loTex));
				d.push_back(Sampled("csrc0", cbTex));
				if (!sameTex) d.push_back(Sampled("csrc1", crTex));
				d.push_back(Storage("o_up", m.out));
				if (!Pass(dispatch, "VideoProcessor chroma upscale: upsample", hdr, UP_BODY, d.data(), static_cast<int>(d.size()),
					v.data(), static_cast<int>(v.size()), Groups(W), Groups(H), upShmem, kWg, kWg))
					return fail("upsample pass failed");
			}

			// --- The frame for libplacebo: luma unchanged, full-size CbCr (shifts 0), extra non-Cb/Cr planes preserved ---
			pl_frame frame = source;
			for (int i = 1; i < PL_MAX_PLANES; ++i) frame.planes[i] = pl_plane{};
			pl_plane up{};
			up.texture = m.out;
			up.components = 2;
			up.component_mapping[0] = PL_CHANNEL_CB;
			up.component_mapping[1] = PL_CHANNEL_CR;
			up.component_mapping[2] = PL_CHANNEL_NONE;
			up.component_mapping[3] = PL_CHANNEL_NONE;
			up.shift_x = 0.0f;
			up.shift_y = 0.0f;
			frame.planes[1] = up;
			int outIdx = 2;
			for (int i = 1; i < source.num_planes; ++i)
			{
				const pl_plane& sp = source.planes[i];
				bool hasExtra = false;
				bool hasChromaOrLuma = false;
				for (int c = 0; c < sp.components && c < 4; ++c)
				{
					const int cm = sp.component_mapping[c];
					if (cm == PL_CHANNEL_CB || cm == PL_CHANNEL_CR || cm == PL_CHANNEL_Y) hasChromaOrLuma = true;
					else if (cm != PL_CHANNEL_NONE) hasExtra = true;
				}
				if (!hasExtra) continue;
				if (outIdx >= PL_MAX_PLANES) return fail("too many planes to preserve extra channels");
				pl_plane extra = sp;
				if (hasChromaOrLuma)
				{
					for (int c = 0; c < extra.components && c < 4; ++c)
					{
						const int cm = extra.component_mapping[c];
						if (cm == PL_CHANNEL_CB || cm == PL_CHANNEL_CR || cm == PL_CHANNEL_Y)
							extra.component_mapping[c] = PL_CHANNEL_NONE;
					}
				}
				frame.planes[outIdx++] = extra;
			}
			frame.num_planes = outIdx;
			result = frame;
			m_status = !ownSplit ? "ok" : (misfit ? "ok; own grain split (the shared product does not fit)" : "ok; own grain split");
			return true;
		}
	}
}
