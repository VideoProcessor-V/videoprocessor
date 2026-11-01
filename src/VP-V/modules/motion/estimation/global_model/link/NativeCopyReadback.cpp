// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "NativeCopyReadback.h"

#include <libplacebo/shaders/custom.h>

namespace CameraLink
{
	namespace
	{
		const char* kCopyBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < nc_dim.x && p.y < nc_dim.y) {
    imageStore(nc_dst, p, texelFetch(nc_a, p, 0));
    if (nc_planes == 2)
        imageStore(nc_dst, p + ivec2(0, nc_dim.y), texelFetch(nc_b, p, 0));
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
		if (!Active() || !a || (m_planes == 2 && !b))
			return false;
		pl_tex dst = m_native->Target(slot);
		if (!dst)
			return false;
		const pl_shader_desc desc[3] = { Sampled("nc_a", a), Sampled("nc_b", m_planes == 2 ? b : a), [&] {
			pl_shader_desc d{};
			d.desc.name = "nc_dst";
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = PL_DESC_ACCESS_WRITEONLY;
			d.binding.object = dst;
			return d;
		}() };
		int dim[2] = { m_w, m_h };
		int planes = m_planes;
		const pl_shader_var vars[2] = { { pl_var_ivec2("nc_dim"), dim, true }, { pl_var_int("nc_planes"), &planes, true } };
		pl_custom_shader custom{};
		custom.description = "VideoProcessor camera link: readback copy";
		custom.body = kCopyBody;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = 3;
		custom.variables = vars;
		custom.num_variables = 2;
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
