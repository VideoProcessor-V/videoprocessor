// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-4, finding on D3D11): libplacebo's D3D11 backend has NO buffer transfers (limits.buf_transfer is false:
// pl_tex_download into a pl_buf is refused), its pl_tex_download is a blocking Map, and pl_buf_poll always says "ready"
// (see drive/led_measure/NativeReadback.h). So the non-blocking GPU -> CPU readback on D3D11 is the existing shared
// LedDrive::NativeTexReadback: a VP-owned texture a shader writes (imageStore), a native copy into a staging texture, read
// with Map(DO_NOT_WAIT) (that map is the readiness probe; no event query). This class adds the copy pass: it copies one or two sampled rgba32f textures
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
#include <cstdint>
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

		// `slots` slots of w x (h * planes). When `fp16` is true (GEM-143: ONLY when source planes are 16-bit float),
		// staging uses DXGI_FORMAT_R16G16B16A16_FLOAT and Read() unpacks IEEE-754 half -> float losslessly; default
		// false keeps DXGI_FORMAT_R32G32B32A32_FLOAT (used by GuidanceCountsRing and rgba32f sources). False: not D3D11 / no storable format.
		bool Create(pl_log log, pl_gpu gpu, int slots, int w, int h, int planes, bool fp16 = false);
		bool Active() const { return m_native && m_native->Active(); }
		int Width() const { return m_w; }
		int Height() const { return m_h; }
		int Planes() const { return m_planes; }
		bool IsFp16() const { return m_fp16; }
		int Slots() const { return m_native ? m_native->Slots() : 0; }
		// Exact allocated GPU + staging texture bytes across all slots (0 when inactive).
		uint64_t AllocatedBytes() const;

		// Exact IEEE-754 binary16 -> binary32 conversion (handles normals, subnormals, +/-0, +/-Inf, NaN).
		static float HalfToFloat(uint16_t h);

		// Dispatches the copy of a (and b when planes == 2) into slot i and queues the native copy without flushing;
		// call FlushPending() once after a batch of Start() calls (Poll/Read also flush once if still pending). False: dispatch failed.
		bool Start(int slot, pl_tex a, pl_tex b);
		// Submits all queued copies since the last flush with ONE pl_gpu_flush(m_gpu). False when nothing is pending.
		bool FlushPending();
		bool HasPendingFlush() const { return m_pendingFlush; }
		uint64_t FlushCount() const { return m_flushCount; }
		// Never blocks. Flushes pending queued copies once if needed. Pending: the GPU still holds the copy. Ready: mapped,
		// Read returns it. Failed: an error HRESULT (LastError), not "still drawing": release the slot and show the failure.
		LedDrive::ReadState Poll(int slot);
		bool Ready(int slot) { return Poll(slot) == LedDrive::ReadState::Ready; }
		// Drops a Ready slot's mapping without reading it (used when draining cleared slots).
		void Discard(int slot) { if (m_native) m_native->Discard(slot); }
		HRESULT LastError() const { return m_native ? m_native->LastError() : S_OK; }
		// dst: w * h * planes * 4 floats. False while the GPU still holds the slot, and on an error.
		bool Read(int slot, float* dst);
		void Release();

	private:
		std::unique_ptr<LedDrive::NativeTexReadback> m_native;
		pl_gpu m_gpu = nullptr;
		pl_dispatch m_dispatch = nullptr;
		int m_w = 0, m_h = 0, m_planes = 1;
		bool m_fp16 = false;
		bool m_pendingFlush = false;
		uint64_t m_flushCount = 0;
	};
}
