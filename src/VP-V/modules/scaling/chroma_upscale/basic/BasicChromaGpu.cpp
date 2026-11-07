// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale / basic GPU pass (BasicChromaGpu.h). Mirrors BasicChroma::UpsamplePlane step by step in int32.
#include <pch.h>

#include <VP-V/modules/scaling/chroma_upscale/basic/BasicChromaGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <algorithm>
#include <string>

namespace BasicChroma
{
	namespace Gpu
	{
		namespace
		{
			int Log2(int v) { int l = 0; while ((1 << l) < v) ++l; return l; }

			std::string IntArray(const char* name, const PhaseTable& t)
			{
				std::string s = std::string("const int ") + name + "[" + std::to_string(kMaxPhases * kTaps) + "] = int[" +
					std::to_string(kMaxPhases * kTaps) + "](";
				for (int p = 0; p < kMaxPhases; ++p)
					for (int k = 0; k < kTaps; ++k)
					{
						s += std::to_string(p < t.phases ? t.w[p][k] : 0);
						if (p + 1 < kMaxPhases || k + 1 < kTaps) s += ", ";
					}
				return s + ");\n";
			}

			// bc_range (in the header: a function cannot sit inside the pass body) mirrors BasicChroma::ClampRange; floor division by
			// 4f is a shift (4f is 4, 8 or 16) and the phase a mask: the same results as FloorDiv for negative n (two's complement).
			const char* const RANGE_FN = R"GLSL(
void bc_range(int x, int L, int R, int cL, int cR, int cn, out int lo, out int hi)
{
    if (x >= L && x < R) { lo = cL; hi = cR - 1; }
    else if (x < L) { lo = 0; hi = max(cL - 1, 0); }
    else { lo = min(cR, cn - 1); hi = cn - 1; }
}
)GLSL";
			const char* const BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < BC_W && p.y < BC_H) {
    int nx = 4 * p.x + 2 - BC_S4X - 2 * BC_FX;
    int ix0 = nx >> BC_LX;
    int phx = nx & (4 * BC_FX - 1);
    int ny = 4 * p.y + 2 - BC_S4Y - 2 * BC_FY;
    int iy0 = ny >> BC_LY;
    int phy = ny & (4 * BC_FY - 1);
    int xlo, xhi, ylo, yhi;
    bc_range(p.x, BC_AX0, BC_AX1, BC_CX0, BC_CX1, BC_CW, xlo, xhi);
    bc_range(p.y, BC_AY0, BC_AY1, BC_CY0, BC_CY1, BC_CH, ylo, yhi);
    ivec2 accV = ivec2(0);
    for (int ty = 0; ty < 6; ty++) {
        int ky = clamp(iy0 - 2 + ty, ylo, yhi);
        ivec2 accH = ivec2(0);
        for (int tx = 0; tx < 6; tx++) {
            int kx = clamp(ix0 - 2 + tx, xlo, xhi);
            vec2 t = texelFetch(bc_src, ivec2(kx, ky), 0).xy;
            ivec2 c = ivec2(floor(t * 65535.0 + 0.5));
            accH += c * BCX_W[phx * 6 + tx];
        }
        ivec2 hv = (accH + ivec2(8192)) >> 14;
        accV += hv * BCY_W[phy * 6 + ty];
    }
    ivec2 o = clamp((accV + ivec2(8192)) >> 14, ivec2(0), ivec2(65535));
    imageStore(bc_dst, p, vec4(vec2(o) / 65535.0, 0.0, 1.0));
}
)GLSL";
		}

		std::string BuildHeader(const Geometry& g)
		{
			const int cw = g.chromaW(), ch = g.chromaH();
			const Rect lr = g.active.Empty() ? Rect{ 0, 0, g.lumaW, g.lumaH } : g.active;
			const Rect cr = ChromaRect(lr, g.fx, g.fy, cw, ch);
			std::string h;
			h += VpvGpu::DefineInt("BC_W", g.lumaW) + VpvGpu::DefineInt("BC_H", g.lumaH);
			h += VpvGpu::DefineInt("BC_CW", cw) + VpvGpu::DefineInt("BC_CH", ch);
			h += VpvGpu::DefineInt("BC_FX", g.fx) + VpvGpu::DefineInt("BC_FY", g.fy);
			h += VpvGpu::DefineInt("BC_LX", Log2(4 * g.fx)) + VpvGpu::DefineInt("BC_LY", Log2(4 * g.fy));
			h += VpvGpu::DefineInt("BC_S4X", g.s4x) + VpvGpu::DefineInt("BC_S4Y", g.s4y);
			h += VpvGpu::DefineInt("BC_AX0", lr.x0) + VpvGpu::DefineInt("BC_AX1", lr.x1);
			h += VpvGpu::DefineInt("BC_AY0", lr.y0) + VpvGpu::DefineInt("BC_AY1", lr.y1);
			h += VpvGpu::DefineInt("BC_CX0", cr.x0) + VpvGpu::DefineInt("BC_CX1", cr.x1);
			h += VpvGpu::DefineInt("BC_CY0", cr.y0) + VpvGpu::DefineInt("BC_CY1", cr.y1);
			h += IntArray("BCX_W", BuildPhaseTable(g.fx));
			h += IntArray("BCY_W", BuildPhaseTable(g.fy));
			h += RANGE_FN;
			return h;
		}

		const char* Body() { return BODY; }

		bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, pl_tex src, const Geometry& g, pl_tex& out)
		{
			m_error.clear();
			auto fail = [&](const std::string& why) { m_error = why; return false; };
			const std::string why = Check(g);
			if (!why.empty()) return fail(why);
			if (!gpu || !dispatch || !src) return fail("no device or source texture");
			if (src->params.w != g.chromaW() || src->params.h != g.chromaH())
				return fail("source texture size does not match the luma size and factors");
			if (!src->params.sampleable || !src->params.format || src->params.format->num_components < 2 ||
				src->params.format->component_depth[0] != 16 || src->params.format->type != PL_FMT_UNORM)
				return fail("source texture is not a sampleable two-component 16-bit unorm format");
			const pl_fmt fmt = VpvGpu::Unorm16(gpu, 2);
			if (!fmt) return fail("no storable two-component 16-bit unorm format");
			if (!VpvGpu::Ensure(gpu, m_out, g.lumaW, g.lumaH, fmt, true)) return fail("no storage texture for the output");
			const pl_shader_desc d[2] = { VpvGpu::Sampled("bc_src", src), VpvGpu::Storage("bc_dst", m_out) };
			if (!VpvGpu::Pass(dispatch, "VideoProcessor basic chroma upscale", BuildHeader(g), BODY, d, 2, nullptr, 0,
				(g.lumaW + 15) / 16, (g.lumaH + 15) / 16, 0))
				return fail("the basic chroma pass failed");
			out = m_out;
			return true;
		}

		void Module::Release(pl_gpu gpu)
		{
			if (m_out) pl_tex_destroy(gpu, &m_out);
		}
	}
}
