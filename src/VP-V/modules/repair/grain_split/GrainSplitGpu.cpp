// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split GPU passes (GrainSplitGpu.h). Mirrors GrainSplit::Compute (GrainSplit.h).
#include <pch.h>

#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace GrainSplit
{
	namespace Gpu
	{
		namespace
		{
			// Rows: the three Gaussians across, edges replicated; the summation order of GrainSplit::Gauss.
			const char* const GH_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < GS_W && p.y < GS_H) {
    float a1 = 0.0, ab = 0.0, a6 = 0.0;
    for (int k = -R1; k <= R1; k++) a1 += w1(k) * gs_y(clamp(p.x + k, 0, GS_W - 1), p.y);
    for (int k = -RB; k <= RB; k++) ab += wb(k) * gs_y(clamp(p.x + k, 0, GS_W - 1), p.y);
    for (int k = -R6; k <= R6; k++) a6 += w6(k) * gs_y(clamp(p.x + k, 0, GS_W - 1), p.y);
    imageStore(rows_out, p, vec4(a1, ab, a6, gs_y(p.x, p.y)));
}
)GLSL";

			// Columns: -> d1 = Y - G1, d2 = G1 - Gb, d3 = Gb - G6, base = Gb.
			const char* const GV_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < GS_W && p.y < GS_H) {
    float g1 = 0.0, gb = 0.0, g6 = 0.0;
    for (int k = -R1; k <= R1; k++) g1 += w1(k) * texelFetch(rows, ivec2(p.x, clamp(p.y + k, 0, GS_H - 1)), 0).x;
    for (int k = -RB; k <= RB; k++) gb += wb(k) * texelFetch(rows, ivec2(p.x, clamp(p.y + k, 0, GS_H - 1)), 0).y;
    for (int k = -R6; k <= R6; k++) g6 += w6(k) * texelFetch(rows, ivec2(p.x, clamp(p.y + k, 0, GS_H - 1)), 0).z;
    float y = texelFetch(rows, p, 0).w;
    imageStore(bands_out, p, vec4(y - g1, g1 - gb, gb - g6, gb));
}
)GLSL";

			// One thread per block: sub-block energies of D (sub-block (si, sj) spans [b B + si B / SUB, b B + (si + 1) B / SUB)),
			// sorted, the quietest QUIET averaged x CAL; and the block mean of d3^2 (raster order).
			const char* const QUIET_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < GS_BW && b.y < GS_BH) {
    float e[GS_SUB * GS_SUB];
    int n = 0;
    for (int sj = 0; sj < GS_SUB; sj++)
        for (int si = 0; si < GS_SUB; si++) {
            int x0 = b.x * GS_B + (si * GS_B) / GS_SUB, x1 = min(GS_W, b.x * GS_B + ((si + 1) * GS_B) / GS_SUB);
            int y0 = b.y * GS_B + (sj * GS_B) / GS_SUB, y1 = min(GS_H, b.y * GS_B + ((sj + 1) * GS_B) / GS_SUB);
            if (x1 > x0 && y1 > y0) {
                float sum = 0.0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        vec4 c = texelFetch(bands, ivec2(xx, yy), 0);
                        float d = c.x + c.y;
                        sum += d * d;
                    }
                e[n] = sum / float((x1 - x0) * (y1 - y0));
                n++;
            }
        }
    // insertion sort, ascending
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
    float s3 = 0.0;
    int y1b = min(GS_H, (b.y + 1) * GS_B), x1b = min(GS_W, (b.x + 1) * GS_B);
    for (int yy = b.y * GS_B; yy < y1b; yy++)
        for (int xx = b.x * GS_B; xx < x1b; xx++) {
            float d3 = texelFetch(bands, ivec2(xx, yy), 0).z;
            s3 += d3 * d3;
        }
    float cnt = float((x1b - b.x * GS_B) * (y1b - b.y * GS_B));
    imageStore(quiet_out, b, vec4(quiet, s3 / cnt, 0.0, 0.0));
}
)GLSL";

			// COMPACT block passes: the same N and e3 as the Full ones, plus the block means of d1^2 and d2^2 (the fine and
			// mid band energies, raster order, no atomics) as two more modifier channels: quiet = (quiet, e3, e1, e2).
			const char* const QUIET_BODY_COMPACT = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < GS_BW && b.y < GS_BH) {
    float e[GS_SUB * GS_SUB];
    int n = 0;
    for (int sj = 0; sj < GS_SUB; sj++)
        for (int si = 0; si < GS_SUB; si++) {
            int x0 = b.x * GS_B + (si * GS_B) / GS_SUB, x1 = min(GS_W, b.x * GS_B + ((si + 1) * GS_B) / GS_SUB);
            int y0 = b.y * GS_B + (sj * GS_B) / GS_SUB, y1 = min(GS_H, b.y * GS_B + ((sj + 1) * GS_B) / GS_SUB);
            if (x1 > x0 && y1 > y0) {
                float sum = 0.0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        vec4 c = texelFetch(bands, ivec2(xx, yy), 0);
                        float d = c.x + c.y;
                        sum += d * d;
                    }
                e[n] = sum / float((x1 - x0) * (y1 - y0));
                n++;
            }
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
    float s1q = 0.0, s2q = 0.0, s3 = 0.0;
    int y1b = min(GS_H, (b.y + 1) * GS_B), x1b = min(GS_W, (b.x + 1) * GS_B);
    for (int yy = b.y * GS_B; yy < y1b; yy++)
        for (int xx = b.x * GS_B; xx < x1b; xx++) {
            vec4 c = texelFetch(bands, ivec2(xx, yy), 0);
            s1q += c.x * c.x;
            s2q += c.y * c.y;
            s3 += c.z * c.z;
        }
    float cnt = float((x1b - b.x * GS_B) * (y1b - b.y * GS_B));
    imageStore(quiet_out, b, vec4(quiet, s3 / cnt, s1q / cnt, s2q / cnt));
}
)GLSL";

			const char* const NMIN_BODY_COMPACT = R"GLSL(
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

			const char* const NMIN_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < GS_BW && b.y < GS_BH) {
    float m = 1e30;
    for (int j = -GS_NG; j <= GS_NG; j++)
        for (int i = -GS_NG; i <= GS_NG; i++)
            m = min(m, texelFetch(quiet, ivec2(clamp(b.x + i, 0, GS_BW - 1), clamp(b.y + j, 0, GS_BH - 1)), 0).x);
    imageStore(blocks_out, b, vec4(m, texelFetch(quiet, b, 0).y, 0.0, 0.0));
}
)GLSL";

			const char* const F_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < GS_W && p.y < GS_H) {
    float e = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec4 c = texelFetch(bands, ivec2(clamp(p.x + i, 0, GS_W - 1), clamp(p.y + j, 0, GS_H - 1)), 0);
            float dv = c.x + c.y;
            e += dv * dv;
        }
    e /= 9.0;
    float N = texelFetch(blocks, ivec2(p.x / GS_B, p.y / GS_B), 0).x;
    float f = clamp(1.0 - GS_KAPPA * N / max(e, GS_FLOOR), 0.0, 1.0);
    vec4 c = texelFetch(bands, p, 0);
    imageStore(split_out, p, vec4(f * c.x, f * c.y, f, c.w));
}
)GLSL";

			// The Compact last pass: the same arithmetic as F_BODY; A = (s1, s2) in codes, B = (f, base x code unit).
			const char* const F_BODY_COMPACT = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < GS_W && p.y < GS_H) {
    float e = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec4 c = texelFetch(bands, ivec2(clamp(p.x + i, 0, GS_W - 1), clamp(p.y + j, 0, GS_H - 1)), 0);
            float dv = c.x + c.y;
            e += dv * dv;
        }
    e /= 9.0;
    float N = texelFetch(blocks, ivec2(p.x / GS_B, p.y / GS_B), 0).x;
    float f = clamp(1.0 - GS_KAPPA * N / max(e, GS_FLOOR), 0.0, 1.0);
    vec4 c = texelFetch(bands, p, 0);
    imageStore(split_a, p, vec4(f * c.x, f * c.y, 0.0, 0.0));
    imageStore(split_b, p, vec4(f, c.w * GS_CU, 0.0, 0.0));
}
)GLSL";

			constexpr const char* kArenaOwner = "grain_split";

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

			std::string WeightsGlsl(const char* name, const char* radiusName, double sigma)
			{
				const int r = GaussRadius(sigma);
				return VpvGpu::DefineInt(radiusName, r) + "float " + name + "(int k)\n{\n    " +
					VpvGpu::ConstArray("w", 2 * r + 1, [&](int i) { return GaussWeight(i - r, sigma); }) + "    return w[k + " + radiusName + "];\n}\n";
			}
		}

		int GaussRadius(double sigma) { return VpUnit::BandFromBase::Radius(sigma, 3.0, true, 0.0); }

		double GaussWeight(int k, double sigma)
		{
			return VpUnit::BandFromBase::Weight(k, sigma, GaussRadius(sigma));
		}

		void Splitter::Release(pl_gpu gpu)
		{
			if (gpu)
				for (pl_tex* t : { &m_rows, &m_bands, &m_quiet, &m_blocks, &m_outA, &m_outB })
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
					"#define GS_HAS_ENERGIES 0\n"   // Full carries no band energies: a defined 'unavailable' value, readers derive their own
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
			for (const char* body : { GH_BODY, GV_BODY, QUIET_BODY, NMIN_BODY, F_BODY })
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
			std::vector<VpSkeleton::ArenaRequest> r;
			VpSkeleton::TexFormat tf = VpSkeleton::TexFormat::RGBA32F;
			if (!gpu || w < 1 || h < 1 || B < 1 || !VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 4), &tf)) return r;
			const int bw = (w + B - 1) / B, bh = (h + B - 1) / B;
			VpSkeleton::ArenaRequest a;
			a.scope = kArenaScope;
			a.owner = kArenaOwner;
			a.tier = VpSkeleton::Tier::T0;
			a.shape.w = static_cast<uint32_t>(w); a.shape.h = static_cast<uint32_t>(h); a.shape.fmt = tf;
			a.count = storage == GrainSplit::Storage::Compact ? 2 : 1;   // Compact: rows + bands; Full: bands (rows is the product)
			r.push_back(a);
			a.shape.w = static_cast<uint32_t>(bw); a.shape.h = static_cast<uint32_t>(bh);
			a.count = 1;
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
				std::string geo, weights, yread;
			};
			PassText MakePassText(int w, int h, int B, int bw, int bh, int component, double codeUnit, double scale, const Params& params)
			{
				using namespace VpvGpu;
				static const char* const kComp[4] = { "x", "y", "z", "w" };
				PassText t;
				t.geo = DefineInt("GS_W", w) + DefineInt("GS_H", h) + DefineInt("GS_B", B) + DefineInt("GS_BW", bw) + DefineInt("GS_BH", bh) +
					DefineInt("GS_UNIT_VERSION", GrainSplit::kUnitVersion);
				t.weights = WeightsGlsl("w1", "R1", params.sigmaFine * scale) + WeightsGlsl("wb", "RB", params.sigmaBase * scale) +
					WeightsGlsl("w6", "R6", params.sigmaLow * scale);
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
				if (!Begin(gpu, dispatch, plane, component, codeUnit, B, params, options))
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
			const int w = r.w, h = r.h, bw = r.bw, bh = r.bh;
			const bool compact = r.compact;
			const PassText txt = MakePassText(w, h, B, bw, bh, component, codeUnit, scale, params);
			const int gx = (w + 15) / 16, gy = (h + 15) / 16, bgx = (bw + 15) / 16, bgy = (bh + 15) / 16;
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
				const pl_shader_desc d[2] = { Sampled("src", plane), VpvGpu::Storage("rows_out", r.rows) };
				if (!Pass(dispatch, "VideoProcessor grain split: rows", txt.geo + txt.weights + txt.yread, GH_BODY, d, 2, nullptr, 0, gx, gy, 0, 16, 16, options.timer))
					return fail("grain split: rows pass failed");
				break;
			}
			case 1:
			{
				const pl_shader_desc d[2] = { Sampled("rows", r.rows), VpvGpu::Storage("bands_out", r.bands) };
				if (!Pass(dispatch, "VideoProcessor grain split: columns", txt.geo + txt.weights, GV_BODY, d, 2, nullptr, 0, gx, gy, 0, 16, 16, options.timer))
					return fail("grain split: columns pass failed");
				break;
			}
			case 2:
			{
				const std::string hdr = txt.geo + DefineInt("GS_SUB", params.subBlocks) + DefineInt("GS_QUIET", params.quietSubBlocks) + Define("GS_CAL", params.grainCal);
				const pl_shader_desc d[2] = { Sampled("bands", r.bands), VpvGpu::Storage("quiet_out", r.quiet) };
				if (!Pass(dispatch, "VideoProcessor grain split: grain level", hdr, compact ? QUIET_BODY_COMPACT : QUIET_BODY, d, 2, nullptr, 0, bgx, bgy, 0, 16, 16, options.timer))
					return fail("grain split: grain level pass failed");
				break;
			}
			case 3:
			{
				const pl_shader_desc d[2] = { Sampled("quiet", r.quiet), VpvGpu::Storage("blocks_out", r.blocks) };
				if (!Pass(dispatch, "VideoProcessor grain split: neighbourhood minimum", txt.geo + DefineInt("GS_NG", params.neighbourhood), compact ? NMIN_BODY_COMPACT : NMIN_BODY, d, 2, nullptr, 0, bgx, bgy, 0, 16, 16, options.timer))
					return fail("grain split: minimum pass failed");
				break;
			}
			default:
			{
				if (compact)
				{
					const std::string hdr = txt.geo + Define("GS_KAPPA", params.kappa) + Define("GS_FLOOR", params.energyFloor) + DefineInt("GS_COMPACT", 1) + Define("GS_CU", codeUnit);
					const pl_shader_desc d[4] = { Sampled("bands", r.bands), Sampled("blocks", r.blocks), VpvGpu::Storage("split_a", r.outA), VpvGpu::Storage("split_b", r.outB) };
					if (!Pass(dispatch, "VideoProcessor grain split: Wiener share (compact)", hdr, F_BODY_COMPACT, d, 4, nullptr, 0, gx, gy, 0, 16, 16, options.timer))
						return fail("grain split: Wiener pass failed");
					m_out.split = r.outA;
					m_out.splitB = r.outB;
				}
				else
				{
					const std::string hdr = txt.geo + Define("GS_KAPPA", params.kappa) + Define("GS_FLOOR", params.energyFloor);
					const pl_shader_desc d[3] = { Sampled("bands", r.bands), Sampled("blocks", r.blocks), VpvGpu::Storage("split_out", r.rows) };
					if (!Pass(dispatch, "VideoProcessor grain split: Wiener share", hdr, F_BODY, d, 3, nullptr, 0, gx, gy, 0, 16, 16, options.timer))
						return fail("grain split: Wiener pass failed");
					m_out.split = r.rows;
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
			const Options& options)
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
			if (!(params.sigmaFine > 0.0) || !(params.sigmaBase > 0.0) || !(params.sigmaLow > 0.0) || params.subBlocks < 1)
			{
				m_error = "grain split: invalid law";
				return false;
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
			const int bw = (w + B - 1) / B, bh = (h + B - 1) / B;
			const pl_fmt f4 = FloatFormat(gpu, 4);
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
			if (!f4)
			{
				m_error = "grain split: no rgba32f storage texture";
				return false;
			}
			auto usable = [&](pl_tex t, int tw, int th, pl_fmt fmt) { return t && t->params.w == tw && t->params.h == th && t->params.format == fmt && t->params.sampleable && t->params.storable; };
			if ((tg.split && !usable(tg.split, w, h, compact ? fA : f4)) || (tg.blocks && !usable(tg.blocks, bw, bh, fBlocks)) ||
				(compact && tg.splitB && !usable(tg.splitB, w, h, fB)))
			{
				m_error = "grain split: unusable target texture";
				return false;
			}

			// Intermediates: from the arena (fixed order rows, bands, quiet; scope closed on every exit), else private.
			pl_tex rows = nullptr, bands = nullptr, quiet = nullptr;
						if (options.arena)
			{
				// The private temporaries of the non-arena path are not used any more: free them.
				for (pl_tex* t : { &m_bands, &m_quiet })
					if (*t) pl_tex_destroy(gpu, t);
				if (compact && m_rows) pl_tex_destroy(gpu, &m_rows);
				VpSkeleton::TexFormat tf = VpSkeleton::TexFormat::RGBA32F;
				if (!VpSkeleton::PlTexBackend::FormatOf(f4, &tf))
				{
					m_error = "grain split: rgba32f is not an arena format";
					return false;
				}
				VpSkeleton::TexShape full, qshape;
				full.w = static_cast<uint32_t>(w); full.h = static_cast<uint32_t>(h); full.fmt = tf;
				qshape.w = static_cast<uint32_t>(bw); qshape.h = static_cast<uint32_t>(bh); qshape.fmt = tf;
				if (compact) rows = options.arena->Acquire(kArenaOwner, full);
				bands = options.arena->Acquire(kArenaOwner, full);
				quiet = options.arena->Acquire(kArenaOwner, qshape);
				if ((compact && !rows) || !bands || !quiet)
				{
					m_scratchShort = true;
					m_error = std::string("scratch memory short (grain split): ") + ((compact && !rows) ? "rows " : "") + (!bands ? "bands " : "") +
						(!quiet ? "quiet " : "") + "unavailable";
					return false;
				}
			}
			else
			{
				if (!Ensure(gpu, m_bands, w, h, f4) || !Ensure(gpu, m_quiet, bw, bh, f4) || (compact && !Ensure(gpu, m_rows, w, h, f4)))
				{
					m_error = "grain split: no rgba32f storage texture";
					return false;
				}
				bands = m_bands;
				quiet = m_quiet;
				if (compact) rows = m_rows;
			}
			// Outputs: the caller's targets, else the Splitter's own.
			pl_tex outA = nullptr, outB = nullptr, blocksTex = tg.blocks;
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
				// Full: the final pass writes the split into the rows texture, which therefore outlives the call.
				rows = tg.split;
				if (!rows && !Ensure(gpu, m_rows, w, h, f4))
				{
					m_error = "grain split: no rgba32f storage texture";
					return false;
				}
				if (!rows) rows = m_rows;
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
			m_run.rows = rows; m_run.bands = bands; m_run.quiet = quiet; m_run.blocks = blocksTex;
			m_run.outA = outA; m_run.outB = outB;
			return true;
		}
	}
}
