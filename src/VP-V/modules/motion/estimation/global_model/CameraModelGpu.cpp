// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the GPU label pass (see CameraModelGpu.h). The GLSL follows the branch order of
// GlobalModelFit.cpp Labels() exactly, in float arithmetic; LabelBlockMirror is its CPU twin.
#include "CameraModelGpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <libplacebo/shaders/custom.h>

namespace CameraModel
{
	namespace
	{
		inline bool FiniteF(float v) { return IsFiniteBits(v); }

		// One pass: one thread per block. Uniforms (pl_shader_var):
		//   vec4 cm_a = (h0 - 1, h1, h2, h3)   vec4 cm_b = (h4 - 1, h5, h6, h7)   vec4 cm_c = (h8 - 1, cx, cy, blockSize)
		//   vec4 cm_t = (absThr, rel, pixMin, flat)
		//   int cm_confident, cm_have_res, cm_have_tex, cm_persp; ivec2 cm_grid
		// The displacement is formed directly, (num - x*e) / (1 + e), instead of u/w - x, which loses precision
		// to cancellation in float; for an affine model e = 0 and the displacement is num.
		const char* const kLabelBody = R"GLSL(
#define CM_FINITE(v) ((floatBitsToUint(v) & 0x7F800000u) != 0x7F800000u)
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < cm_grid.x && blk.y < cm_grid.y) {
    vec4 fv = texelFetch(cm_field, blk, 0);
    vec4 ax = texelFetch(cm_aux, blk, 0);
    float vx = fv.x;
    float vy = fv.y;
    float vt = fv.z;
    bool ex = ax.x != 0.0;
    float bs = cm_c.w;
    precise float x = (float(blk.x) + 0.5) * bs - cm_c.y;
    precise float y = (float(blk.y) + 0.5) * bs - cm_c.z;
    precise float nx = cm_a.x * x + cm_a.y * y + cm_a.z;
    precise float ny = cm_a.w * x + cm_b.x * y + cm_b.y;
    float dx;
    float dy;
    if (cm_persp != 0) {
        precise float e = cm_b.z * x + cm_b.w * y + cm_c.x;
        precise float w = 1.0 + e;
        if (w != 0.0) {
            precise float iw = 1.0 / w;
            dx = (nx - x * e) * iw;
            dy = (ny - y * e) * iw;
        } else {
            dx = -x;
            dy = -y;
        }
    } else {
        dx = nx;
        dy = ny;
    }
    if (!CM_FINITE(dx) || !CM_FINITE(dy)) {
        dx = 0.0;
        dy = 0.0;
    }
    precise float mm2 = dx * dx + dy * dy;
    precise float abs2 = cm_t.x * cm_t.x;
    precise float rel2 = cm_t.y * cm_t.y;
    float thr2 = max(abs2, rel2 * mm2);
    bool tr = !ex && vt > 0.0 && CM_FINITE(vt) && CM_FINITE(vx) && CM_FINITE(vy);
    float rx = 0.0;
    float ry = 0.0;
    float zc = 0.0;
    if (tr) {
        rx = vx - dx;
        ry = vy - dy;
        precise float vm2 = vx * vx + vy * vy;
        if (cm_confident != 0 && vm2 <= abs2 && mm2 > thr2)
            zc = 8.0;
    }
    precise float r2 = rx * rx + ry * ry;
    float pres = ax.y;
    float tex = ax.z;
    int l;
    int t = 0;
    if (ex) {
        l = 3;
    } else if (cm_have_res != 0 && CM_FINITE(pres) && pres >= cm_t.z) {
        l = 2;
        t = 1;
    } else if (tr) {
        l = (r2 <= thr2) ? 1 : 2;
    } else {
        bool cam = cm_have_tex != 0 && CM_FINITE(tex) && tex <= cm_t.w;
        if (!cam && CM_FINITE(vx) && CM_FINITE(vy)) {
            precise float ux = vx - dx;
            precise float uy = vy - dy;
            cam = ux * ux + uy * uy <= thr2;
        }
        l = cam ? 1 : 0;
    }
    imageStore(cm_out, blk, vec4(dx, dy, sqrt(r2), float(l + 4 * t) + zc));
}
)GLSL";

		const char* const kClusterBody = R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < cm_grid.x && blk.y < cm_grid.y) {
    vec4 c = texelFetch(cm_in, blk, 0);
    int code = int(c.w + 0.5);
    if ((code & 8) != 0) {
        int n = 0;
        if (blk.x > 0 && (int(texelFetch(cm_in, blk + ivec2(-1, 0), 0).w + 0.5) & 8) != 0) n++;
        if (blk.x + 1 < cm_grid.x && (int(texelFetch(cm_in, blk + ivec2(1, 0), 0).w + 0.5) & 8) != 0) n++;
        if (blk.y > 0 && (int(texelFetch(cm_in, blk + ivec2(0, -1), 0).w + 0.5) & 8) != 0) n++;
        if (blk.y + 1 < cm_grid.y && (int(texelFetch(cm_in, blk + ivec2(0, 1), 0).w + 0.5) & 8) != 0) n++;
        if (n >= cm_overlay_min)
            code = 3 + 8;
    }
    imageStore(cm_out, blk, vec4(c.x, c.y, c.z, float(code)));
}
)GLSL";

		pl_shader_desc Sampled(const char* name, pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_SAMPLED_TEX;
			desc.binding.object = tex;
			desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
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
	}

	const char* LabelPassGlsl() { return kLabelBody; }
	const char* ZeroClusterGlsl() { return kClusterBody; }

	LabelConstants MakeLabelConstants(const CameraMotion& rec, const Config& cfg, const PairInput& in)
	{
		LabelConstants k;
		const Params& P = rec.params;
		for (int i = 0; i < 9; ++i)
			k.h[i] = static_cast<float>(P.h[i]);
		k.dh[0] = static_cast<float>(P.h[0] - 1.0);
		k.dh[1] = static_cast<float>(P.h[4] - 1.0);
		k.dh[2] = static_cast<float>(P.h[8] - 1.0);
		k.cx = static_cast<float>(in.picture.CentreX());
		k.cy = static_cast<float>(in.picture.CentreY());
		k.blockSize = static_cast<float>(in.blockSize);
		const double prec = in.precisionPx > 0 ? in.precisionPx : 0.25 * in.blockSize / 16.0;
		k.absThr = static_cast<float>(cfg.absThresholdSigmas * prec);
		k.rel = static_cast<float>(cfg.relThreshold);
		k.confident = rec.confidence >= 0.5 ? 1 : 0;
		k.pixMin = static_cast<float>(cfg.pixelResidualMinShare);
		k.flat = static_cast<float>(cfg.flatTexture);
		k.haveResidual = in.pixelResidual != nullptr ? 1 : 0;
		k.haveTexture = in.texture != nullptr ? 1 : 0;
		k.gridW = in.gridW;
		k.gridH = in.gridH;
		k.perspective = !(P.h[6] == 0.0 && P.h[7] == 0.0 && P.h[8] == 1.0) ? 1 : 0;
		return k;
	}

	void PackLabelInputs(const PairInput& in, const BlockData& blocks, std::vector<float>& field, std::vector<float>& aux)
	{
		const size_t n = static_cast<size_t>(in.gridW) * static_cast<size_t>(in.gridH);
		field.assign(n * 4, 0.0f);
		aux.assign(n * 4, 0.0f);
		for (size_t i = 0; i < n; ++i)
		{
			field[i * 4 + 0] = in.fwdX[i];
			field[i * 4 + 1] = in.fwdY[i];
			field[i * 4 + 2] = in.fwdTrust[i];
			aux[i * 4 + 0] = (blocks.excluded.size() > i && blocks.excluded[i] != 0) ? 1.0f : 0.0f;
			aux[i * 4 + 1] = in.pixelResidual ? in.pixelResidual[i] : -1.0f;
			aux[i * 4 + 2] = in.texture ? in.texture[i] : -1.0f;
		}
	}

	// The shader's arithmetic, operation for operation, in float. Products that feed a sum go through a
	// volatile temporary so the compiler cannot fuse them (the shader's variables are `precise`).
	void LabelBlockMirror(const LabelConstants& k, int bx, int by, float fx, float fy, float trust, float excluded,
		float pres, float tex, float out[4])
	{
		const float vx = fx, vy = fy, vt = trust;
		const bool ex = excluded != 0.0f;
		const float bs = k.blockSize;
		volatile float cxs = (static_cast<float>(bx) + 0.5f) * bs;
		volatile float cys = (static_cast<float>(by) + 0.5f) * bs;
		const float x = cxs - k.cx;
		const float y = cys - k.cy;
		volatile float t1 = k.dh[0] * x, t2 = k.h[1] * y;
		const float nx = (t1 + t2) + k.h[2];
		volatile float t3 = k.h[3] * x, t4 = k.dh[1] * y;
		const float ny = (t3 + t4) + k.h[5];
		float dx, dy;
		if (k.perspective != 0)
		{
			volatile float e1 = k.h[6] * x, e2 = k.h[7] * y;
			const float e = (e1 + e2) + k.dh[2];
			const float w = 1.0f + e;
			if (w != 0.0f)
			{
				const float iw = 1.0f / w;
				volatile float xe = x * e, ye = y * e;
				dx = (nx - xe) * iw;
				dy = (ny - ye) * iw;
			}
			else
			{
				dx = -x;
				dy = -y;
			}
		}
		else
		{
			dx = nx;
			dy = ny;
		}
		if (!FiniteF(dx) || !FiniteF(dy))
		{
			dx = 0.0f;
			dy = 0.0f;
		}
		volatile float m1 = dx * dx, m2 = dy * dy;
		const float mm2 = m1 + m2;
		const float abs2 = k.absThr * k.absThr;
		const float rel2 = k.rel * k.rel;
		const float rm = rel2 * mm2;
		const float thr2 = abs2 > rm ? abs2 : rm;
		const bool tr = !ex && vt > 0.0f && FiniteF(vt) && FiniteF(vx) && FiniteF(vy);
		float rx = 0.0f, ry = 0.0f, zc = 0.0f;
		if (tr)
		{
			rx = vx - dx;
			ry = vy - dy;
			volatile float v1 = vx * vx, v2 = vy * vy;
			const float vm2 = v1 + v2;
			if (k.confident != 0 && vm2 <= abs2 && mm2 > thr2)
				zc = 8.0f;
		}
		volatile float q1 = rx * rx, q2 = ry * ry;
		const float r2 = q1 + q2;
		int l, t = 0;
		if (ex)
			l = 3;
		else if (k.haveResidual != 0 && FiniteF(pres) && pres >= k.pixMin)
		{
			l = 2;
			t = 1;
		}
		else if (tr)
			l = (r2 <= thr2) ? 1 : 2;
		else
		{
			bool cam = k.haveTexture != 0 && FiniteF(tex) && tex <= k.flat;
			if (!cam && FiniteF(vx) && FiniteF(vy))
			{
				const float ux = vx - dx, uy = vy - dy;
				volatile float u1 = ux * ux, u2 = uy * uy;
				cam = (u1 + u2) <= thr2;
			}
			l = cam ? 1 : 0;
		}
		out[0] = dx;
		out[1] = dy;
		out[2] = std::sqrt(r2);
		out[3] = static_cast<float>(l + 4 * t) + zc;
	}

	void ZeroClusterMirror(int gridW, int gridH, int overlayMin, const float* in4, float* out4)
	{
		auto code = [&](int x, int y) { return static_cast<int>(in4[(static_cast<size_t>(y) * gridW + x) * 4 + 3] + 0.5f); };
		for (int by = 0; by < gridH; ++by)
			for (int bx = 0; bx < gridW; ++bx)
			{
				const size_t i = (static_cast<size_t>(by) * gridW + bx) * 4;
				int c = code(bx, by);
				if ((c & 8) != 0)
				{
					int n = 0;
					if (bx > 0 && (code(bx - 1, by) & 8) != 0) ++n;
					if (bx + 1 < gridW && (code(bx + 1, by) & 8) != 0) ++n;
					if (by > 0 && (code(bx, by - 1) & 8) != 0) ++n;
					if (by + 1 < gridH && (code(bx, by + 1) & 8) != 0) ++n;
					if (n >= overlayMin)
						c = 3 + 8;
				}
				out4[i + 0] = in4[i + 0];
				out4[i + 1] = in4[i + 1];
				out4[i + 2] = in4[i + 2];
				out4[i + 3] = static_cast<float>(c);
			}
	}

	void LabelGridMirror(const LabelConstants& k, const float* field, const float* aux, int overlayMin, std::vector<float>& out4)
	{
		const size_t n = static_cast<size_t>(k.gridW) * static_cast<size_t>(k.gridH);
		std::vector<float> first(n * 4);
		for (int by = 0; by < k.gridH; ++by)
			for (int bx = 0; bx < k.gridW; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * k.gridW + bx;
				LabelBlockMirror(k, bx, by, field[i * 4], field[i * 4 + 1], field[i * 4 + 2], aux[i * 4], aux[i * 4 + 1], aux[i * 4 + 2],
					&first[i * 4]);
			}
		out4.assign(n * 4, 0.0f);
		ZeroClusterMirror(k.gridW, k.gridH, overlayMin, first.data(), out4.data());
	}

	LabelPass::LabelPass(pl_log log, pl_gpu gpu, bool hostReadable) : m_log(log), m_gpu(gpu), m_readable(hostReadable)
	{
		m_dispatch = pl_dispatch_create(log, gpu);
	}

	LabelPass::~LabelPass()
	{
		for (pl_tex& t : m_tex)
			if (t)
				pl_tex_destroy(m_gpu, &t);
		if (m_dispatch)
			pl_dispatch_destroy(&m_dispatch);
	}

	bool LabelPass::Ensure(int w, int h)
	{
		pl_fmt fmt = pl_find_fmt(m_gpu, PL_FMT_FLOAT, 4, 32, 32,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
		if (!fmt)
		{
			m_error = "no storable rgba32f format";
			return false;
		}
		for (pl_tex& t : m_tex)
		{
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = true;
			params.blit_src = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;   // K2: the camera link copies the pass output into its P8 slot
			params.host_readable = m_readable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
			if (!pl_tex_recreate(m_gpu, &t, &params))
			{
				m_error = "cannot create the label textures";
				return false;
			}
		}
		return true;
	}

	bool LabelPass::Dispatch(const char* description, const char* body, const pl_shader_desc* desc, int numDesc,
		const pl_shader_var* vars, int numVars, int w, int h)
	{
		const int group = 8;
		pl_custom_shader custom{};
		custom.description = description;
		custom.body = body;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = numDesc;
		custom.variables = vars;
		custom.num_variables = numVars;
		custom.compute = true;
		custom.compute_group_size[0] = group;
		custom.compute_group_size[1] = group;
		pl_shader sh = pl_dispatch_begin(m_dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(m_dispatch, &sh);
			m_error = std::string("shader rejected: ") + description;
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (w + group - 1) / group;
		compute.dispatch_size[1] = (h + group - 1) / group;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(m_dispatch, &compute))
		{
			m_error = std::string("dispatch failed: ") + description;
			return false;
		}
		return true;
	}

	bool LabelPass::Run(const LabelConstants& k, pl_tex field, pl_tex aux, int overlayMin)
	{
		m_error.clear();
		if (!m_dispatch || !field || !aux || k.gridW <= 0 || k.gridH <= 0)
		{
			m_error = "no input";
			return false;
		}
		if (!m_tex[0] || m_tex[0]->params.w != k.gridW || m_tex[0]->params.h != k.gridH)
			if (!Ensure(k.gridW, k.gridH))
				return false;
		const float a[4] = { k.dh[0], k.h[1], k.h[2], k.h[3] };
		const float b[4] = { k.dh[1], k.h[5], k.h[6], k.h[7] };
		const float c[4] = { k.dh[2], k.cx, k.cy, k.blockSize };
		const float t[4] = { k.absThr, k.rel, k.pixMin, k.flat };
		const int confident = k.confident, haveRes = k.haveResidual, haveTex = k.haveTexture, persp = k.perspective;
		const int grid[2] = { k.gridW, k.gridH };
		const pl_shader_var vars[9] = {
			{ pl_var_vec4("cm_a"), a, true }, { pl_var_vec4("cm_b"), b, true }, { pl_var_vec4("cm_c"), c, true },
			{ pl_var_vec4("cm_t"), t, true }, { pl_var_int("cm_confident"), &confident, true },
			{ pl_var_int("cm_have_res"), &haveRes, true }, { pl_var_int("cm_have_tex"), &haveTex, true },
			{ pl_var_int("cm_persp"), &persp, true }, { pl_var_ivec2("cm_grid"), grid, true } };
		const pl_shader_desc first[3] = { Sampled("cm_field", field), Sampled("cm_aux", aux), Storage("cm_out", m_tex[0]) };
		if (!Dispatch("VideoProcessor camera model: block labels", kLabelBody, first, 3, vars, 9, k.gridW, k.gridH))
			return false;
		const int overlay = overlayMin;
		const pl_shader_var vars2[2] = { { pl_var_ivec2("cm_grid"), grid, true }, { pl_var_int("cm_overlay_min"), &overlay, true } };
		const pl_shader_desc second[2] = { Sampled("cm_in", m_tex[0]), Storage("cm_out", m_tex[1]) };
		return Dispatch("VideoProcessor camera model: zero-motion clusters", kClusterBody, second, 2, vars2, 2, k.gridW, k.gridH);
	}
}
