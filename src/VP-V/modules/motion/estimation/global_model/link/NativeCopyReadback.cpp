// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "NativeCopyReadback.h"

#include <libplacebo/shaders/custom.h>

namespace CameraLink
{
	namespace
	{
		// Two variants. The same texture is never bound to two sampled descriptors: on D3D11/WARP a
		// pass with a duplicated binding wrote nothing (the readback came back zero).
		const char* kCopyBody1 = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 d = ivec2(nc_dim);
if (p.x < d.x && p.y < d.y)
    imageStore(nc_dst, p, texelFetch(nc_a, p, 0));
)GLSL";
		const char* kCopyBody2 = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 d = ivec2(nc_dim);
if (p.x < d.x && p.y < d.y) {
    imageStore(nc_dst, p, texelFetch(nc_a, p, 0));
    imageStore(nc_dst, p + ivec2(0, d.y), texelFetch(nc_b, p, 0));
}
)GLSL";

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
	}

	bool NativeCopyReadback::Create(pl_log log, pl_gpu gpu, int slots, int w, int h, int planes)
	{
		Release();
		if (!gpu || w <= 0 || h <= 0 || (planes != 1 && planes != 2))
			return false;
		std::unique_ptr<LedDrive::NativeTexReadback> n(new LedDrive::NativeTexReadback());
		if (!n->Create(gpu, slots, w, h * planes, DXGI_FORMAT_R32G32B32A32_FLOAT))
			return false;
		m_dispatch = pl_dispatch_create(log, gpu);
		if (!m_dispatch)
		{
			n->Release(gpu);
			return false;
		}
		m_native = std::move(n);
		m_gpu = gpu;
		m_w = w;
		m_h = h;
		m_planes = planes;
		return true;
	}

	bool NativeCopyReadback::Start(int slot, pl_tex a, pl_tex b)
	{
		if (!Active() || !a)
			return false;
		const bool two = m_planes == 2 && b && b != a;   // a single plane leaves the second half of the target unread
		pl_tex dst = m_native->Target(slot);
		if (!dst)
			return false;
		pl_shader_desc dstDesc{};
		dstDesc.desc.name = "nc_dst";
		dstDesc.desc.type = PL_DESC_STORAGE_IMG;
		dstDesc.desc.access = PL_DESC_ACCESS_WRITEONLY;
		dstDesc.binding.object = dst;
		const pl_shader_desc desc[3] = { Sampled("nc_a", a), dstDesc, Sampled("nc_b", two ? b : a) };
		// Float variables only, as SourceResolutionMonitor (the proven native-readback pass).
		const float dim[2] = { static_cast<float>(m_w), static_cast<float>(m_h) };
		const pl_shader_var vars[1] = { { pl_var_vec2("nc_dim"), dim, true } };
		pl_custom_shader custom{};
		custom.description = "VideoProcessor camera link: readback copy";
		custom.body = two ? kCopyBody2 : kCopyBody1;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = two ? 3 : 2;
		custom.variables = vars;
		custom.num_variables = 1;
		custom.compute = true;
		custom.compute_group_size[0] = 8;
		custom.compute_group_size[1] = 8;
		pl_shader sh = pl_dispatch_begin(m_dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(m_dispatch, &sh);
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (m_w + 7) / 8;
		compute.dispatch_size[1] = (m_h + 7) / 8;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(m_dispatch, &compute))
			return false;
		m_native->Queue(slot);
		pl_gpu_flush(m_gpu);   // submit the pass and the copy; never waits
		return true;
	}

	bool NativeCopyReadback::Read(int slot, float* dst) const
	{
		return Active() && m_native->Read(slot, dst, static_cast<size_t>(m_w) * 16u, m_h * m_planes);
	}

	void NativeCopyReadback::Release()
	{
		if (m_native)
		{
			m_native->Release(m_gpu);
			m_native.reset();
		}
		if (m_dispatch)
			pl_dispatch_destroy(&m_dispatch);
		m_gpu = nullptr;
		m_w = m_h = 0;
	}
}
