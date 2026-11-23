// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale / basic GPU pass (BasicChromaGpu.h). Mirrors BasicChroma::UpsamplePlane step by step in int32: both are
// ScalerPass's Q14 fixed-point mode (scaling/shared/ScalerPassFixed.h, CF-2 B5a-2 step 2), the GLSL generated with the "BC" prefix.
#include <pch.h>

#include <VP-V/modules/scaling/chroma_upscale/basic/BasicChromaGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/core/ComputeOnce.h>

#include <algorithm>
#include <string>

namespace BasicChroma
{
	namespace Gpu
	{
		std::string BuildHeader(const Geometry& g)
		{
			ScalerPass::FixedQ14::Axis ax, ay;
			AxesOf(g, ax, ay);
			return ScalerPass::FixedQ14::GlslHeader("BC", "bc", ax, ay, LanczosSpec());
		}

		const char* Body()
		{
			static const std::string body = ScalerPass::FixedQ14::GlslBody("BC", "bc", LanczosSpec());
			return body.c_str();
		}

		bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, pl_tex src, const Geometry& g, pl_tex& out)
		{
			m_error.clear();
			VPV_COMPUTE_ONCE("U20", ::ComputeOnce::Variant(src ? src->params.w : 0, src ? src->params.h : 0));
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
			// MV7: the kept output texture is declared to the governor with its real size (inert: no call site makes it yet).
			const uint64_t held = VpSkeleton::TexBytesOf(m_out);
			if (held != m_declared)   // Apply is per frame: the registry is touched only when the texture size moved
			{
				VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserBasicChroma, true);
				VpSkeleton::VramUsers::Instance().Declare(this, DeclaredUser(g.lumaW, g.lumaH), held);
				m_declared = held;
			}
			const pl_shader_desc d[2] = { VpvGpu::Sampled("bc_src", src), VpvGpu::Storage("bc_dst", m_out) };
			if (!VpvGpu::Pass(dispatch, "VideoProcessor basic chroma upscale", BuildHeader(g), Body(), d, 2, nullptr, 0,
				(g.lumaW + 15) / 16, (g.lumaH + 15) / 16, 0))
				return fail("the basic chroma pass failed");
			out = m_out;
			return true;
		}

		void Module::Release(pl_gpu gpu)
		{
			VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserBasicChroma);   // MV7
			VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserBasicChroma, false);
			m_declared = ~uint64_t(0);
			if (m_out) pl_tex_destroy(gpu, &m_out);
		}
	}
}
