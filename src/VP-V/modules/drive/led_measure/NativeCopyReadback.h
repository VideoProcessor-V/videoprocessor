// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared non-blocking D3D11 GPU -> CPU texture copy readback (moved from motion/estimation/global_model/link to
// drive/led_measure, S#2 / R2; originally K1-4 finding on D3D11):
// libplacebo's D3D11 backend has NO buffer transfers (limits.buf_transfer is false: pl_tex_download into a pl_buf is
// refused), its pl_tex_download is a blocking Map, and pl_buf_poll always says "ready" (see drive/led_measure/NativeReadback.h).
// So the non-blocking GPU -> CPU readback on D3D11 is the shared LedDrive::NativeTexReadback: a VP-owned texture a shader
// writes (imageStore), a native copy into a staging texture, read with Map(DO_NOT_WAIT) once the D3D11 fence completes.
//
// NativeCopyReadback adds the copy pass: it copies one or two sampled textures (Rgba32F, Rgba16F, or R32F) into a slot,
// stacked vertically (second plane below the first), and reads it back without ever waiting. Not D3D11 -> Create returns
// false and the caller keeps its fallback path.
//
// NativeCopyReadbackDevice wraps NativeCopyReadback as the first production VpSkeleton::IReadbackDevice (LaneInterfaces.h /
// LaneReadback.h) with pre-allocated per-slot CPU read buffers (zero steady-state reallocs) and visible failure tracking.
// Render thread only.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX   // d3d11.h pulls windows.h: keep min / max usable
#endif

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/modules/drive/led_measure/NativeReadback.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace LedDrive
{
	// Pixel format of the NativeCopyReadback target / staging textures.
	//   Rgba32F: DXGI_FORMAT_R32G32B32A32_FLOAT (4 floats = 16 B/texel)
	//   Rgba16F: DXGI_FORMAT_R16G16B16A16_FLOAT (4 halfs = 8 B/texel, unpacked losslessly to 4 floats in Read())
	//   R32F:    DXGI_FORMAT_R32_FLOAT (1 float = 4 B/texel, single-channel copy shader)
	enum class CopyFormat : uint8_t { Rgba32F, Rgba16F, R32F };

	class NativeCopyReadback
	{
	public:
		using CopyFormat = LedDrive::CopyFormat;

		NativeCopyReadback() = default;
		NativeCopyReadback(const NativeCopyReadback&) = delete;
		NativeCopyReadback& operator=(const NativeCopyReadback&) = delete;
		~NativeCopyReadback() { Release(); }

		// `slots` slots of w x (h * planes) in `fmt`. False: not D3D11 / invalid dims / no storable format.
		bool Create(pl_log log, pl_gpu gpu, int slots, int w, int h, int planes, CopyFormat fmt);
		// Legacy overload: `fp16 = true` (GEM-143: ONLY when source planes are 16-bit float) selects CopyFormat::Rgba16F;
		// default `fp16 = false` keeps CopyFormat::Rgba32F bit-identically.
		bool Create(pl_log log, pl_gpu gpu, int slots, int w, int h, int planes, bool fp16 = false);

		bool Active() const { return m_native && m_native->Active(); }
		int Width() const { return m_w; }
		int Height() const { return m_h; }
		int Planes() const { return m_planes; }
		CopyFormat Format() const { return m_format; }
		bool IsFp16() const { return m_format == CopyFormat::Rgba16F; }
		int Channels() const { return m_format == CopyFormat::R32F ? 1 : 4; }
		int Slots() const { return m_native ? m_native->Slots() : 0; }
		// Tightly packed float output buffer size in bytes for one slot (w * h * planes * Channels() * sizeof(float); 0 when inactive).
		size_t TightBytes() const;
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
		ReadState Poll(int slot);
		bool Ready(int slot) { return Poll(slot) == ReadState::Ready; }
		// Drops a Ready slot's mapping without reading it (used when draining cleared slots).
		void Discard(int slot) { if (m_native) m_native->Discard(slot); }
		HRESULT LastError() const { return m_native ? m_native->LastError() : S_OK; }
		// dst: w * h * planes * Channels() floats. False while the GPU still holds the slot, and on an error.
		bool Read(int slot, float* dst);
		void Release();

	private:
		std::unique_ptr<NativeTexReadback> m_native;
		pl_gpu m_gpu = nullptr;
		pl_dispatch m_dispatch = nullptr;
		int m_w = 0, m_h = 0, m_planes = 1;
		CopyFormat m_format = CopyFormat::Rgba32F;
		bool m_pendingFlush = false;
		uint64_t m_flushCount = 0;
	};

	// First production VpSkeleton::IReadbackDevice (S#2 / R2): wraps NativeCopyReadback for VpLaneProducts::ReadbackRing.
	// Pre-allocates one tightly packed CPU float buffer per staging slot at Create() (zero steady-state reallocs) and
	// tracks readback HRESULT failures visibly (LastError / FailedCount; never silently swallowed). Render thread only.
	class NativeCopyReadbackDevice : public VpSkeleton::IReadbackDevice
	{
	public:
		using CopyFormat = LedDrive::CopyFormat;
		using SourceResolver = std::function<pl_tex(VpSkeleton::ProductId, uint64_t)>;

		NativeCopyReadbackDevice() = default;
		NativeCopyReadbackDevice(const NativeCopyReadbackDevice&) = delete;
		NativeCopyReadbackDevice& operator=(const NativeCopyReadbackDevice&) = delete;
		~NativeCopyReadbackDevice() override { Release(); }

		// Creates `slots` single-plane staging buffers of w x h in `fmt` (default R32F) with optional `resolver`.
		bool Create(pl_log log, pl_gpu gpu, int slots, int w, int h, CopyFormat fmt = CopyFormat::R32F,
			SourceResolver resolver = nullptr);
		void SetResolver(SourceResolver resolver);
		// Default false: CopyToStaging flushes immediately unless caller sets defer = true and batches FlushPending().
		void SetDeferFlush(bool defer) { m_deferFlush = defer; }
		bool FlushPending() { return m_copy.FlushPending(); }

		// VpSkeleton::IReadbackDevice
		bool CopyToStaging(VpSkeleton::ProductId product, uint64_t seq, int stagingIndex) override;
		bool QueryDone(int stagingIndex) override;
		bool QueryFailed(int stagingIndex) const override;
		void Discard(int stagingIndex) override;
		const void* MapNoWait(int stagingIndex, size_t* bytes) override;
		void Unmap(int stagingIndex) override;

		bool Active() const { return m_copy.Active(); }
		uint64_t AllocatedBytes() const { return m_copy.AllocatedBytes(); }
		int Width() const { return m_copy.Width(); }
		int Height() const { return m_copy.Height(); }
		CopyFormat Format() const { return m_copy.Format(); }
		int Slots() const { return m_copy.Slots(); }
		size_t TightBytes() const { return m_copy.TightBytes(); }
		HRESULT LastError() const { return m_lastError; }
		uint64_t FailedCount() const { return m_failedCount; }
		void Release();

	private:
		bool ValidSlot(int stagingIndex) const
		{
			return stagingIndex >= 0 && static_cast<size_t>(stagingIndex) < m_slotBufs.size();
		}
		void RecordSlotFailure(int stagingIndex);

		NativeCopyReadback m_copy;
		SourceResolver m_resolver;
		std::vector<std::vector<float>> m_slotBufs;
		std::vector<uint8_t> m_slotMapped;
		std::vector<uint8_t> m_slotFailed;
		bool m_deferFlush = false;
		HRESULT m_lastError = S_OK;
		uint64_t m_failedCount = 0;
	};
}

namespace CameraLink
{
	using LedDrive::CopyFormat;
	using LedDrive::NativeCopyReadback;
	using LedDrive::NativeCopyReadbackDevice;
}
