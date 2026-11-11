// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 5a (owner O2 / O3): the GPU twin of the optical-flow producer's engine luma. On the producer's OWN
// D3D11 device it reads the ONE uploaded raw frame (an EarlyUpload slot opened there, EarlyUpload::AcquireReader) and
// writes the 8-bit engine view the NVOF engine takes - byte-identical to the CPU path (MotionAhead::HalfLuma8 and the
// producer's quarter step) by construction: the same integer law (VpUnit::HalfLuma U3 taps, the engine-8 finish, the
// quarter rounding), pinned by EngineLumaGpuTwinTests.
//
// Formats (O2): limited-range P010 / P210 (the luma plane), v210, UYVY / HDYC. RGB, decoded / packed DV, the SDR tunnel
// and full-range YUV stay on the CPU path (refs/105 s5a; counted and shown by the caller, never silent).
// Phase 5a ships the kernel and its tests only; the producer switches to it in 5b.
//
// Threading: one Kernel per device; Open / Run / Close on the thread that owns that device's immediate context.
#pragma once

#include <d3d11.h>
#include <atlbase.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace VpUnit
{
namespace EngineLumaGpu
{
	// Bump on ANY change of the decode, the taps, the rounding or the output format.
	constexpr uint32_t kUnitVersion = 1;

	// The raw layouts the twin decodes (the shader's `packing`).
	enum class Packing : uint32_t
	{
		Planar16 = 0,   // P010 / P210 luma plane: 16-bit words, code = word >> 6
		V210 = 1,       // 10-bit 4:2:2, six pixels per 16 bytes
		Uyvy8 = 2       // UYVY / HDYC, 8-bit, code = byte << 2
	};

	// The bytes one row of `width` pixels needs (the raw texture row is rowBytes / 4 texels, rowBytes >= this).
	inline size_t MinRowBytes(Packing packing, uint32_t width)
	{
		switch (packing)
		{
		case Packing::Planar16: return size_t(width) * 2u;
		case Packing::V210: return ((size_t(width) + 5u) / 6u) * 16u;
		default: return size_t(width) * 2u;
		}
	}
	// The engine view's size: the half grid ((w + 1) / 2), or half again for the quarter view.
	inline uint32_t HalfDim(uint32_t v) { return (v + 1u) / 2u; }
	inline uint32_t OutDim(uint32_t v, bool quarter) { return quarter ? HalfDim(HalfDim(v)) : HalfDim(v); }

	// The kernel source (EngineLumaGpu.hlsl).
	const char* Source();

	class Kernel
	{
	public:
		Kernel() = default;
		~Kernel() { Close(); }
		Kernel(const Kernel&) = delete;
		Kernel& operator=(const Kernel&) = delete;

		// Compiles the shader (cs_5_0) and makes its constant buffer on `device`. False with LastError on failure.
		bool Open(ID3D11Device* device);
		void Close();
		bool IsOpen() const { return shader != nullptr; }

		// Records the decode on `context` (the device's immediate context; the caller orders it after any GPU wait for
		// the upload and flushes). `raw`: the frame bytes as R8G8B8A8_UNORM (rowBytes / 4 texels x at least `height`
		// rows) with shader-resource binding. `out`: R8_UNORM with unordered-access binding, at least
		// OutDim(width, quarter) x OutDim(height, quarter). Leaves no state bound. False with LastError on a mismatch.
		bool Run(ID3D11DeviceContext* context, ID3D11Texture2D* raw, Packing packing, uint32_t width, uint32_t height,
			bool quarter, ID3D11Texture2D* out);

		const std::string& LastError() const { return error; }

	private:
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11ComputeShader> shader;
		CComPtr<ID3D11Buffer> params;
		std::string error;
	};
}
}
