// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale GPU passes (ChromaCflGpu.h). Mirrors ChromaUpscale::Upsample (ChromaCfl.h) pass by pass; the model's
// steps are named in the comments (A grain guard, B luma at the sites, C regression, D upsample, E bars).
#include <pch.h>

#include <VP-V/modules/scaling/chroma_upscale/ChromaCflGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ChromaUpscale
{
	namespace Gpu
	{
		namespace
		{
			// A bit-exact copy of a rectangle of a plane (so every pass treats the picture's border as the plane edge).
			const char* const CROP_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < CFL_CROP_W && p.y < CFL_CROP_H) imageStore(crop_out, p, texelFetch(src, p + ivec2(CFL_OX, CFL_OY), 0));
)GLSL";

			// B: luma at the chroma sites (separable tent, rows inner then columns); A: the clean chroma and its grain.
			const char* const LO_BODY = R"GLSL(
ivec2 j = ivec2(gl_GlobalInvocationID.xy);
if (j.x < CFL_CW && j.y < CFL_CH) {
    float px = (float(j.x + CFL_CX0) + 0.5) * CFL_FX + CFL_SX - 0.5;
    float py = (float(j.y + CFL_CY0) + 0.5) * CFL_FY + CFL_SY - 0.5;
    int x0 = int(ceil(px - CFL_FX)), x1 = int(floor(px + CFL_FX));
    int y0 = int(ceil(py - CFL_FY)), y1 = int(floor(py + CFL_FY));
    float wsx = 0.0, wsy = 0.0;
    for (int x = x0; x <= x1; x++) wsx += max(0.0, 1.0 - abs(float(x) - px) / CFL_FX);
    for (int y = y0; y <= y1; y++) wsy += max(0.0, 1.0 - abs(float(y) - py) / CFL_FY);
    float acc = 0.0;
    for (int y = y0; y <= y1; y++) {
        float wy = max(0.0, 1.0 - abs(float(y) - py) / CFL_FY);
        float rs = 0.0;
        for (int x = x0; x <= x1; x++) {
            float wx = max(0.0, 1.0 - abs(float(x) - px) / CFL_FX);
            rs += wx * cfl_yc(ivec2(clamp(x - CFL_LEFT, 0, CFL_RW - 1), clamp(y - CFL_TOP, 0, CFL_RH - 1)));
        }
        acc += (wy / wsy) * (rs / wsx);
    }
    vec2 raw = cfl_craw(j);
    vec2 cc = cfl_cc(j);
    imageStore(o_lo, j, vec4(acc, cc.x, cc.y, 0.0));
    imageStore(o_gc, j, vec4(raw - cc, 0.0, 0.0));
}
)GLSL";

			// C: the regression of one channel (c = 0: Cb, 1: Cr) in the box (2r + 1)^2, centred two-pass moments.
			const char* const STATS_FUNCS = R"GLSL(
vec4 cfl_reg(ivec2 j, int c)
{
    float sy = 0.0, sc = 0.0;
    for (int jj = -CFL_R; jj <= CFL_R; jj++)
        for (int ii = -CFL_R; ii <= CFL_R; ii++) {
            vec4 t = texelFetch(t_lo, ivec2(clamp(j.x + ii, 0, CFL_CW - 1), clamp(j.y + jj, 0, CFL_CH - 1)), 0);
            sy += t.x;
            sc += (c == 0 ? t.y : t.z);
        }
    float n = float((2 * CFL_R + 1) * (2 * CFL_R + 1));
    float mY = sy / n, mC = sc / n;
    float syy = 0.0, scc = 0.0, syc = 0.0;
    for (int jj = -CFL_R; jj <= CFL_R; jj++)
        for (int ii = -CFL_R; ii <= CFL_R; ii++) {
            vec4 t = texelFetch(t_lo, ivec2(clamp(j.x + ii, 0, CFL_CW - 1), clamp(j.y + jj, 0, CFL_CH - 1)), 0);
            float dy = t.x - mY, dc = (c == 0 ? t.y : t.z) - mC;
            syy += dy * dy;
            scc += dc * dc;
            syc += dy * dc;
        }
    float vY = max(syy / n, 0.0), vC = max(scc / n, 0.0), cov = syc / n;
    float a = cov / (vY + CFL_EPS);
    float b = mC - a * mY;
    float rho2 = clamp(cov * cov / ((vY + CFL_EPS) * (vC + CFL_EPS)), 0.0, 1.0);
    return vec4(a, b, rho2, 0.0);
}

vec2 cfl_range(ivec2 j, int c)
{
    float mn = 1e30, mx = -1e30;
    for (int jj = -CFL_CLAMPR; jj <= CFL_CLAMPR; jj++)
        for (int ii = -CFL_CLAMPR; ii <= CFL_CLAMPR; ii++) {
            vec4 t = texelFetch(t_lo, ivec2(clamp(j.x + ii, 0, CFL_CW - 1), clamp(j.y + jj, 0, CFL_CH - 1)), 0);
            float v = (c == 0 ? t.y : t.z);
            mn = min(mn, v);
            mx = max(mx, v);
        }
    return vec2(mn, mx);
}
)GLSL";

			const char* const STATS_BODY = R"GLSL(
ivec2 j = ivec2(gl_GlobalInvocationID.xy);
if (j.x < CFL_CW && j.y < CFL_CH) {
    vec4 rb = cfl_reg(j, 0), rr = cfl_reg(j, 1);
    vec2 gb = cfl_range(j, 0), gr = cfl_range(j, 1);
    imageStore(o_ab, j, vec4(rb.x, rb.y, rr.x, rr.y));
    imageStore(o_rho, j, vec4(rb.z, rr.z, 0.0, 0.0));
    imageStore(o_lohi, j, vec4(gb.x, gb.y, gr.x, gr.y));
}
)GLSL";

			// The guided filter's second box average of a and b.
			const char* const AVG_BODY = R"GLSL(
ivec2 j = ivec2(gl_GlobalInvocationID.xy);
if (j.x < CFL_CW && j.y < CFL_CH) {
    vec4 s = vec4(0.0);
    for (int jj = -CFL_R; jj <= CFL_R; jj++)
        for (int ii = -CFL_R; ii <= CFL_R; ii++)
            s += texelFetch(t_ab, ivec2(clamp(j.x + ii, 0, CFL_CW - 1), clamp(j.y + jj, 0, CFL_CH - 1)), 0);
    float n = float((2 * CFL_R + 1) * (2 * CFL_R + 1));
    imageStore(o_avg, j, s / n);
}
)GLSL";

			const char* const UP_FUNCS = R"GLSL(
vec4 cfl_cr(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
)GLSL";

			// D inside the active picture (CfL on the crop grid), E outside (bar-side Catmull-Rom of the raw chroma).
			const char* const UP_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < CFL_W && p.y < CFL_H) {
    float pu = (float(p.x) + 0.5 - CFL_SX) / CFL_FX - 0.5;
    float pv = (float(p.y) + 0.5 - CFL_SY) / CFL_FY - 0.5;
    vec2 res = vec2(0.0);
    bool inside = p.x >= CFL_LEFT && p.x < CFL_RIGHT && p.y >= CFL_TOP && p.y < CFL_BOTTOM;
    if (inside) {
        float u = pu - float(CFL_CX0), v = pv - float(CFL_CY0);
        int iu = int(floor(u)), iv = int(floor(v));
        vec4 wu4 = cfl_cr(u - float(iu)), wv4 = cfl_cr(v - float(iv));
        float wu[4] = float[4](wu4.x, wu4.y, wu4.z, wu4.w);
        float wv[4] = float[4](wv4.x, wv4.y, wv4.z, wv4.w);
        vec4 sA = vec4(0.0), sL = vec4(0.0);
        vec2 sR = vec2(0.0), sG = vec2(0.0), sB = vec2(0.0), sP = vec2(0.0);
        for (int m = 0; m < 4; m++) {
            int yy = clamp(iv - 1 + m, 0, CFL_CH - 1);
            vec4 rA = vec4(0.0), rL = vec4(0.0);
            vec2 rR = vec2(0.0), rG = vec2(0.0), rB = vec2(0.0), rP = vec2(0.0);
            for (int n = 0; n < 4; n++) {
                int xx = clamp(iu - 1 + n, 0, CFL_CW - 1);
                ivec2 q = ivec2(xx, yy);
                vec4 av = texelFetch(t_avg, q, 0);
                float ylo = texelFetch(t_lo, q, 0).x;
                rA += wu[n] * av;
                rP += wu[n] * vec2(av.x * ylo + av.y, av.z * ylo + av.w);
                rR += wu[n] * texelFetch(t_rho, q, 0).xy;
                rL += wu[n] * texelFetch(t_lohi, q, 0);
                rB += wu[n] * texelFetch(t_lo, q, 0).yz;
                rG += wu[n] * texelFetch(t_gc, q, 0).xy;
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
        float yv = cfl_yc(p - ivec2(CFL_LEFT, CFL_TOP));
        vec2 lo2 = min(vec2(sL.x, sL.z), vec2(sL.y, sL.w));
        vec2 hi2 = max(vec2(sL.x, sL.z), vec2(sL.y, sL.w));
        res = sB + mm * (clamp(am * yv + bm, lo2, hi2) - sP) + sG;
    } else {
        int xlo = 0, xhi = CFL_PCW - 1, ylo = 0, yhi = CFL_PCH - 1;
        if (p.x < CFL_LEFT) xhi = max(CFL_CX0 - 1, 0);
        else if (p.x >= CFL_RIGHT) xlo = min(CFL_CX1, CFL_PCW - 1);
        if (p.y < CFL_TOP) yhi = max(CFL_CY0 - 1, 0);
        else if (p.y >= CFL_BOTTOM) ylo = min(CFL_CY1, CFL_PCH - 1);
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
    imageStore(o_up, p, vec4(res * CFL_CU_OUT, 0.0, 0.0));
}
)GLSL";

			// A float constant in parentheses (negative values stay safe inside any expression).
			std::string DefineP(const char* name, double v) { return std::string("#define ") + name + " (" + VpvGpu::Num(v) + ")\n"; }

			// vec2 fn(ivec2 p): (Cb, Cr) in codes from the descriptor(s) d0 / d1 (d1 unused when one texture holds both).
			std::string RawReader(const char* fn, const char* d0, const char* d1, bool same, int comp0, int comp1)
			{
				static const char* const kComp = "xyzw";
				return std::string("vec2 ") + fn + "(ivec2 p)\n{\n    return vec2(texelFetch(" + d0 + ", p, 0)." + kComp[comp0] + ", texelFetch(" +
					(same ? d0 : d1) + ", p, 0)." + kComp[comp1] + ") * CFL_INV_CU_C;\n}\n";
			}

			int Groups(int n) { return (n + 15) / 16; }

			// One plane's grain accessor under its own names (the luma, Cb and Cr frames may differ in layout, and one shader
			// binds several). The ONE read accessor (GrainSplit::Gpu::ViewGlsl / BindView) with every name prefixed per plane:
			// plane 0 "gsL_", 1 "gsB_", 2 "gsR_" (not "gl_": reserved in GLSL). Appends the prelude to `glsl` and the
			// plane's sampled descriptors to `descs`. The prelude defines gs<P>_fetch(ivec2) -> (s1, s2, f, base) in codes.
			void AddGrainView(const GrainSplit::Gpu::Frame& f, int plane, std::string& glsl, std::vector<pl_shader_desc>& descs)
			{
				static const char* const kNames[3][4] = {
					{ "gsL_split", "gsL_gblocks", "gsL_a", "gsL_b" },
					{ "gsB_split", "gsB_gblocks", "gsB_a", "gsB_b" },
					{ "gsR_split", "gsR_gblocks", "gsR_a", "gsR_b" } };
				static const char* const kPfx[3] = { "gsL_", "gsB_", "gsR_" };
				auto replaceAll = [](std::string& t, const std::string& from, const std::string& to) {
					for (size_t at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size()))
						t.replace(at, from.size(), to);
				};
				std::string v = GrainSplit::Gpu::ViewGlsl(f.storage, f.codeUnit);
				replaceAll(v, "gs_", kPfx[plane]);                                        // gs_fetch, gs_blocks, gs_a, gs_b, gs_cu
				replaceAll(v, "texelFetch(split,", std::string("texelFetch(") + kNames[plane][0] + ",");   // Full: the sampler names
				replaceAll(v, "texelFetch(gblocks,", std::string("texelFetch(") + kNames[plane][1] + ",");
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
			pl_tex lo = nullptr, gc = nullptr, ab = nullptr, rho = nullptr, lohi = nullptr, avg = nullptr, out = nullptr;
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
				for (pl_tex* t : { &m.cropL, &m.cropB, &m.cropR, &m.lo, &m.gc, &m.ab, &m.rho, &m.lohi, &m.avg, &m.out })
					if (*t) pl_tex_destroy(gpu, t);
			}
			delete m_impl;
			m_impl = nullptr;
		}

		uint64_t CflModule::TextureBytes(int W, int H, int cw, int ch)
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
			b += cpx * (16 + 8 + 16 + 8 + 16 + 16);       // lo, gc, ab, rho, lohi, avg
			return b;
		}

		bool CflModule::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
		{
			using namespace VpvGpu;
			m_error.clear();
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
			if (!gpu || !dispatch || source.num_planes < 2) return fail("a luma plane and chroma plane(s) are needed");
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
			if (!Ensure(gpu, m.lo, pcw, pch, f4) || !Ensure(gpu, m.gc, pcw, pch, f2) || !Ensure(gpu, m.ab, pcw, pch, f4) ||
				!Ensure(gpu, m.rho, pcw, pch, f2) || !Ensure(gpu, m.lohi, pcw, pch, f4) || !Ensure(gpu, m.avg, pcw, pch, f4) ||
				!Ensure(gpu, m.out, W, H, outFmt, true))
				return fail("no storage texture for the module");

			// --- The active picture, bit-exact in the planes' own formats ---
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
				const std::string hdr = DefineInt("CFL_CROP_W", w) + DefineInt("CFL_CROP_H", h) + DefineInt("CFL_OX", x0) + DefineInt("CFL_OY", y0);
				const pl_shader_desc cd[2] = { Sampled("src", src), Storage("crop_out", dst) };
				if (!Pass(dispatch, "VideoProcessor chroma upscale: crop", hdr, CROP_BODY, cd, 2, nullptr, 0, Groups(w), Groups(h), 0))
				{
					fail(std::string(what) + " crop pass failed");
					return false;
				}
				return true;
			};
			pl_tex lumaIn = luma, cbIn = cbTex, crIn = crTex;
			if (whole)
			{
				if (m.cropL) pl_tex_destroy(gpu, &m.cropL);
			}
			else
			{
				if (!crop(luma, m.cropL, r.left, r.top, rw, rh, "luma")) return false;
				lumaIn = m.cropL;
			}
			if (chromaWhole)
			{
				if (m.cropB) pl_tex_destroy(gpu, &m.cropB);
				if (m.cropR) pl_tex_destroy(gpu, &m.cropR);
			}
			else
			{
				if (!crop(cbTex, m.cropB, cRect.x0, cRect.y0, pcw, pch, "chroma")) return false;
				cbIn = m.cropB;
				if (sameTex)
				{
					crIn = m.cropB;
					if (m.cropR) pl_tex_destroy(gpu, &m.cropR);
				}
				else
				{
					if (!crop(crTex, m.cropR, cRect.x0, cRect.y0, pcw, pch, "chroma")) return false;
					crIn = m.cropR;
				}
			}

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
			m_grainStatus = GrainSplit::GrainStatus::Ring;   // recomputed every frame: a fitting product is read again at once
			if (guard)
			{
				auto pick = [&](const GrainSplit::Gpu::Frame* shared, GrainSplit::Gpu::Splitter& own, pl_tex tex, int comp, double cu, double ln,
					const GrainSplit::Gpu::Frame*& outF) {
					if (GrainSplit::Gpu::Fits(shared, tex->params.w, tex->params.h, BlockPx(ln), Scale(ln), cu, comp)) { outF = shared; return true; }
					ownSplit = true;
					if (shared) misfit = true;
					m_grainStatus = misfit ? GrainSplit::GrainStatus::WrongSize : GrainSplit::GrainStatus::Missing;
					if (!own.Compute(gpu, dispatch, tex, comp, cu, Scale(ln), BlockPx(ln))) return false;
					outF = &own.Out();
					return true;
				};
				using RegionEvidence::GrainPlane;
				if (!pick(params.grain.Gpu(GrainPlane::Luma), m.splitL, lumaIn, 0, cuL, lines, gL)) return fail(m.splitL.LastError());
				if (!pick(params.grain.Gpu(GrainPlane::Cb), m.splitB, cbIn, cbComp, cuC, linesC, gB)) return fail(m.splitB.LastError());
				if (!pick(params.grain.Gpu(GrainPlane::Cr), m.splitR, crIn, crComp, cuC, linesC, gR)) return fail(m.splitR.LastError());
			}

			// --- Headers ---
			double peak = 1.0;
			for (int i = 0; i < params.colorDepth; ++i) peak *= 2.0;
			const double eps = (params.cfl.epsFrac * (peak - 1.0)) * (params.cfl.epsFrac * (peak - 1.0));
			std::string geo;
			geo += DefineInt("CFL_W", W) + DefineInt("CFL_H", H) + DefineInt("CFL_PCW", cw) + DefineInt("CFL_PCH", ch);
			geo += DefineInt("CFL_LEFT", r.left) + DefineInt("CFL_TOP", r.top) + DefineInt("CFL_RIGHT", r.right) + DefineInt("CFL_BOTTOM", r.bottom);
			geo += DefineInt("CFL_RW", rw) + DefineInt("CFL_RH", rh);
			geo += DefineInt("CFL_CX0", cRect.x0) + DefineInt("CFL_CY0", cRect.y0) + DefineInt("CFL_CX1", cRect.x1) + DefineInt("CFL_CY1", cRect.y1);
			geo += DefineInt("CFL_CW", pcw) + DefineInt("CFL_CH", pch);
			geo += DefineP("CFL_FX", fx) + DefineP("CFL_FY", fy) + DefineP("CFL_SX", sx) + DefineP("CFL_SY", sy);
			geo += DefineInt("CFL_R", params.cfl.radius) + DefineInt("CFL_CLAMPR", params.cfl.clampR) + Define("CFL_EPS", eps);
			geo += Define("CFL_INV_CU_L", 1.0 / cuL) + Define("CFL_INV_CU_C", 1.0 / cuC) + Define("CFL_CU_OUT", cuOut);
			// With the guard every grain read goes through the accessor of its plane's frame (prelude + BindView, per-plane names).
			std::string lumaView, chromaView;
			std::vector<pl_shader_desc> lumaDescs, chromaDescs;
			if (guard)
			{
				AddGrainView(*gL, 0, lumaView, lumaDescs);
				AddGrainView(*gB, 1, chromaView, chromaDescs);
				AddGrainView(*gR, 2, chromaView, chromaDescs);
			}
			const std::string lumaReader = guard ?
				lumaView + "float cfl_yc(ivec2 p)\n{\n    vec4 s = gsL_fetch(p);\n    return s.w + s.x + s.y;\n}\n" :
				"float cfl_yc(ivec2 p)\n{\n    return texelFetch(ysrc, p, 0).x * CFL_INV_CU_L;\n}\n";

			const int cgx = Groups(pcw), cgy = Groups(pch);

			// --- B + A (chroma): luma at the sites, clean chroma, chroma grain ---
			{
				std::string hdr = geo + lumaReader + RawReader("cfl_craw", "craw0", "craw1", sameTex, cbComp, crComp);
				hdr += guard ?
					chromaView + "vec2 cfl_cc(ivec2 j)\n{\n    vec4 b = gsB_fetch(j), r = gsR_fetch(j);\n    return vec2(b.w + b.x + b.y, r.w + r.x + r.y);\n}\n" :
					"vec2 cfl_cc(ivec2 j)\n{\n    return cfl_craw(j);\n}\n";
				std::vector<pl_shader_desc> d;
				if (guard)
				{
					d.insert(d.end(), lumaDescs.begin(), lumaDescs.end());
					d.insert(d.end(), chromaDescs.begin(), chromaDescs.end());
				}
				else
					d.push_back(Sampled("ysrc", lumaIn));
				d.push_back(Sampled("craw0", cbIn));
				if (!sameTex) d.push_back(Sampled("craw1", crIn));
				d.push_back(Storage("o_lo", m.lo));
				d.push_back(Storage("o_gc", m.gc));
				if (!Pass(dispatch, "VideoProcessor chroma upscale: luma at the sites", hdr, LO_BODY, d.data(), static_cast<int>(d.size()), nullptr, 0, cgx, cgy, 0))
					return fail("luma-at-sites pass failed");
			}
			// --- C: regression and neighbour range ---
			{
				const pl_shader_desc d[4] = { Sampled("t_lo", m.lo), Storage("o_ab", m.ab), Storage("o_rho", m.rho), Storage("o_lohi", m.lohi) };
				if (!Pass(dispatch, "VideoProcessor chroma upscale: regression", geo + STATS_FUNCS, STATS_BODY, d, 4, nullptr, 0, cgx, cgy, 0))
					return fail("regression pass failed");
			}
			{
				const pl_shader_desc d[2] = { Sampled("t_ab", m.ab), Storage("o_avg", m.avg) };
				if (!Pass(dispatch, "VideoProcessor chroma upscale: averaging", geo, AVG_BODY, d, 2, nullptr, 0, cgx, cgy, 0))
					return fail("averaging pass failed");
			}
			// --- D + E: the upsample ---
			{
				std::string hdr = geo + lumaReader + UP_FUNCS + RawReader("cfl_csrc", "csrc0", "csrc1", sameTex, cbComp, crComp);
				std::vector<pl_shader_desc> d;
				if (guard) d.insert(d.end(), lumaDescs.begin(), lumaDescs.end());
				else d.push_back(Sampled("ysrc", lumaIn));
				d.push_back(Sampled("t_avg", m.avg));
				d.push_back(Sampled("t_rho", m.rho));
				d.push_back(Sampled("t_lohi", m.lohi));
				d.push_back(Sampled("t_lo", m.lo));
				d.push_back(Sampled("t_gc", m.gc));
				d.push_back(Sampled("csrc0", cbTex));
				if (!sameTex) d.push_back(Sampled("csrc1", crTex));
				d.push_back(Storage("o_up", m.out));
				if (!Pass(dispatch, "VideoProcessor chroma upscale: upsample", hdr, UP_BODY, d.data(), static_cast<int>(d.size()), nullptr, 0, Groups(W), Groups(H), 0))
					return fail("upsample pass failed");
			}

			// --- The frame for libplacebo: luma unchanged, full-size CbCr, shifts 0 ---
			pl_frame frame = source;
			frame.num_planes = 2;
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
			result = frame;
			m_status = !ownSplit ? "ok" : (misfit ? "ok; own grain split (the shared product does not fit)" : "ok; own grain split");
			return true;
		}
	}
}
