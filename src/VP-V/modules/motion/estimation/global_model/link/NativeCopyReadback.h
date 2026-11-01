// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-4, finding on D3D11): libplacebo's D3D11 backend has NO buffer transfers (limits.buf_transfer is false:
// pl_tex_download into a pl_buf is refused), its pl_tex_download is a blocking Map, and pl_buf_poll always says "ready"
// (see drive/led_measure/NativeReadback.h). So the non-blocking GPU -> CPU readback on D3D11 is the existing shared
// LedDrive::NativeTexReadback: a VP-owned texture a shader writes (imageStore), a native copy into a staging texture and an
// event query; read with Map(DO_NOT_WAIT). This class adds the copy pass: it copies one or two sampled rgba32f textures
// (the P3 forward / backward field, or the 1x1 guidance counts) into a slot, stacked vertically (second plane below the first),
// and reads it back without ever waiting. Not D3D11 -> Create returns false and the caller keeps the pl_buf path.
// Render thread only.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX   // d3d11.h pulls windows.h: keep min / max usable
#endif

#include <VP-V/modules/drive/led_measure/NativeReadback.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include <cstddef>
#include <memory>

namespace CameraLink
{
	class NativeCopyReadback
	{
	public:
		NativeCopyReadback() = default;
		NativeCopyReadback(const NativeCopyReadback&) = delete;
		NativeCopyReadback& operator=(const NativeCopyReadback&) = delete;
		~NativeCopyReadback() { Release(); }

		// `slots` slots of w x (h * planes) rgba32f. False: not D3D11 / no storable format (use the buffer path).
		bool Create(pl_log log, pl_gpu gpu, int slots, int w, int h, int planes);
		bool Active() const { return m_native && m_native->Active(); }
		int Width() const { return m_w; }
		int Height() const { return m_h; }
		int Planes() const { return m_planes; }
		int Slots() const { return m_native ? m_native->Slots() : 0; }

		// Dispatches the copy of a (and b when planes == 2) into slot i and queues the native copy. False: dispatch failed.
		bool Start(int slot, pl_tex a, pl_tex b);
		bool Ready(int slot) const { return m_native && m_native->Ready(slot); }
		// dst: w * h * planes * 4 floats. False while the GPU still holds the slot.
		bool Read(int slot, float* dst) const;
		void Release();

	private:
		std::unique_ptr<LedDrive::NativeTexReadback> m_native;
		pl_gpu m_gpu = nullptr;
		pl_dispatch m_dispatch = nullptr;
		int m_w = 0, m_h = 0, m_planes = 1;
	};
}
