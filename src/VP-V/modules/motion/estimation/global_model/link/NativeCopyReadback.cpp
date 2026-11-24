// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "NativeCopyReadback.h"
#include <VP-V/core/GpuAttributionPl.h>

#include <libplacebo/shaders/custom.h>

#include <cstring>
#if defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#include <intrin.h>
#endif

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

#if defined(_M_X64) || defined(_M_IX86)
		bool HasF16C()
		{
			static const bool supported = []() -> bool
			{
				int regs[4] = {};
				__cpuid(regs, 1);
				// Bit 27 = OSXSAVE, bit 28 = AVX, bit 29 = F16C.
				if ((regs[2] & ((1 << 27) | (1 << 28) | (1 << 29))) != ((1 << 27) | (1 << 28) | (1 << 29)))
					return false;
				return (_xgetbv(0) & 0x6ull) == 0x6ull;
			}();
			return supported;
		}

		__declspec(noinline) void UnpackHalfRowF16C(const uint16_t* rowSrc, float* rowDst, size_t rowElems)
		{
			size_t c = 0;
			for (; c + 8 <= rowElems; c += 8)
			{
				const __m128i h8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rowSrc + c));
				const __m256 f8 = _mm256_cvtph_ps(h8);
				_mm256_storeu_ps(rowDst + c, f8);
			}
			for (; c < rowElems; ++c)
				rowDst[c] = NativeCopyReadback::HalfToFloat(rowSrc[c]);
		}
#endif
	}

	float NativeCopyReadback::HalfToFloat(uint16_t h)
	{
		const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
		const uint32_t exp = static_cast<uint32_t>(h >> 10) & 0x1Fu;
		uint32_t mant = static_cast<uint32_t>(h & 0x03FFu);
		uint32_t bits = 0;
		if (exp == 0)
		{
			if (mant == 0)
				bits = sign;
			else
			{
				int e = -14;
				while ((mant & 0x0400u) == 0)
				{
					mant <<= 1;
					--e;
				}
				mant &= 0x03FFu;
				bits = sign | (static_cast<uint32_t>(e + 127) << 23) | (mant << 13);
			}
		}
		else if (exp == 31u)
			bits = sign | 0x7F800000u | (mant ? (0x00400000u | (mant << 13)) : 0u);
		else
			bits = sign | ((exp + 112u) << 23) | (mant << 13);
		float out = 0.0f;
		std::memcpy(&out, &bits, sizeof(out));
		return out;
	}

	bool NativeCopyReadback::Create(pl_log log, pl_gpu gpu, int slots, int w, int h, int planes, bool fp16)
	{
		Release();
		if (!gpu || w <= 0 || h <= 0 || (planes != 1 && planes != 2))
			return false;
		const DXGI_FORMAT fmt = fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
		std::unique_ptr<LedDrive::NativeTexReadback> n(new LedDrive::NativeTexReadback());
		if (!n->Create(gpu, slots, w, h * planes, fmt))
			return false;
		m_dispatch = pl_dispatch_create(log, gpu);
		GpuAttribution::Watch(m_dispatch, "camera_native_readback");
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
		m_fp16 = fp16;
		return true;
	}

	uint64_t NativeCopyReadback::AllocatedBytes() const
	{
		if (!Active())
			return 0;
		// Exact allocation derivation (NativeTexReadback::Create): each slot owns 1 D3D11_USAGE_DEFAULT target texture
		// and 1 D3D11_USAGE_STAGING staging texture of w x (h * planes), with 4 channels of 16-bit or 32-bit float.
		constexpr uint64_t kTexturesPerSlot = 2ull;
		const uint64_t texelBytes = m_fp16 ? (4ull * sizeof(uint16_t)) : (4ull * sizeof(float));
		return static_cast<uint64_t>(Slots()) * static_cast<uint64_t>(m_w) * static_cast<uint64_t>(m_h)
			* static_cast<uint64_t>(m_planes) * texelBytes * kTexturesPerSlot;
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
		// GEM-135 (AB1#3): defer flush so multiple Start() calls in a frame batch into ONE pl_gpu_flush(m_gpu).
		m_pendingFlush = true;
		return true;
	}

	bool NativeCopyReadback::FlushPending()
	{
		if (!m_pendingFlush || !m_gpu)
			return false;
		pl_gpu_flush(m_gpu);   // submit all queued passes and copies in one batch; never waits
		m_pendingFlush = false;
		++m_flushCount;
		return true;
	}

	LedDrive::ReadState NativeCopyReadback::Poll(int slot)
	{
		if (!m_native)
			return LedDrive::ReadState::Pending;
		if (m_pendingFlush)
			FlushPending();
		return m_native->Poll(slot);
	}

	bool NativeCopyReadback::Read(int slot, float* dst)
	{
		if (!Active() || !dst)
			return false;
		if (m_pendingFlush)
			FlushPending();
		const int rows = m_h * m_planes;
		if (!m_fp16)
			return m_native->Read(slot, dst, static_cast<size_t>(m_w) * 16u, rows);
		const size_t rowElems = static_cast<size_t>(m_w) * 4u;
		return m_native->ReadRows(slot, rows, [dst, rowElems](const unsigned char* src, size_t pitch, int nRows)
		{
#if defined(_M_X64) || defined(_M_IX86)
			const bool f16c = HasF16C();
#endif
			for (int r = 0; r < nRows; ++r)
			{
				const uint16_t* rowSrc = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(r) * pitch);
				float* rowDst = dst + static_cast<size_t>(r) * rowElems;
#if defined(_M_X64) || defined(_M_IX86)
				if (f16c)
				{
					UnpackHalfRowF16C(rowSrc, rowDst, rowElems);
					continue;
				}
#endif
				for (size_t c = 0; c < rowElems; ++c)
					rowDst[c] = HalfToFloat(rowSrc[c]);
			}
		});
	}

	void NativeCopyReadback::Release()
	{
		if (m_pendingFlush && m_gpu)
			pl_gpu_finish(m_gpu);
		m_pendingFlush = false;
		if (m_native)
		{
			m_native->Release(m_gpu);
			m_native.reset();
		}
		if (m_dispatch)
			pl_dispatch_destroy(&m_dispatch);
		m_gpu = nullptr;
		m_w = m_h = 0;
		m_fp16 = false;
	}
}
