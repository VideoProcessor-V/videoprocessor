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
// and full-range YUV stay on the CPU path (refs/105 s5a). The rule lives HERE (FormatCause / Supports); every reason the
// twin does not run is a Cause, counted per cause in FallbackCounters; the caller (5b) only displays them, never silent.
// Phase 5a ships the kernel and its tests only; the producer switches to it in 5b.
//
// Threading: one Kernel per device; Open / Run / Close on the thread that owns that device's immediate context.
#pragma once

#include <VideoFrameEncoding.h>

#include <d3d11.h>
#include <atlbase.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// The lane source's frame view (AnalysisLumaSource.h). Declared, not included: that header pulls windows.h and
// std::min / std::max code, which a TU without NOMINMAX (this unit has no pch) cannot take.
enum class AnalysisLumaFormat;
enum class AnalysisLumaRange : uint8_t;

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

	// Why a frame's engine luma did NOT come from the twin (None: it did). One counter each (FallbackCounters).
	enum class Cause : uint32_t
	{
		None = 0,
		FormatRgb,            // an RGB capture encoding (ARGB / BGRA / r210 / R10b / R10l / R12B / R12L): CPU (O2)
		FormatDolbyVision,    // decoded or packed Dolby Vision 4:2:2: CPU (O2)
		FormatSdrTunnel,      // the SDR tunnel RGB: CPU (O2)
		FullRange,            // full-range YUV: CPU (O2; the twin's law is the limited domain)
		FormatOther,          // anything else (NV12, P016, compressed, unknown): CPU
		KernelOpenFailed,     // Kernel::Open: no device, shader compile or create, constants
		ReaderAttachFailed,   // EarlyUpload::AttachReader failed (no shared fences / device of another adapter)
		NoSlot,               // this publish carried no EarlyUpload slot (capture_upload render, pool full, closed)
		SharedOpenFailed,     // EarlyUpload::AcquireReader returned null (shared texture open failed)
		RunFailed,            // Kernel::Run refused (texture / size mismatch, views)
		DeviceRemoved,        // the producer device was removed / reset
		DetachTimeout,        // DetachReader timed out; the twin stays off for this source until the next Open
		Count
	};
	// A short ASCII name for the status line, the notice and the dump ("rgb", "dolby vision", "full range", ...).
	const char* CauseName(Cause cause);

	// O2 / C9: does the twin decode this frame, and with which packing? None (and *packing set) only for limited-range
	// P010 / P210 / v210 / UYVY / HDYC. `fullRange` is the source's range; Unknown counts as limited (the CPU path's
	// historical reading). The lane-format overload adds the decoded / packed DV and SDR-tunnel views.
	Cause FormatCause(VideoFrameEncoding encoding, bool fullRange, Packing* packing = nullptr);
	Cause FormatCause(AnalysisLumaFormat format, VideoFrameEncoding encoding, AnalysisLumaRange range, Packing* packing = nullptr);
	inline bool Supports(VideoFrameEncoding encoding, bool fullRange) { return FormatCause(encoding, fullRange) == Cause::None; }

	// Per-cause counters (any thread). Count(None) counts a twin frame.
	class FallbackCounters
	{
	public:
		FallbackCounters() { for (auto& c : counts) c.store(0, std::memory_order_relaxed); last.store(0, std::memory_order_relaxed); }
		void Count(Cause cause)
		{
			const uint32_t i = static_cast<uint32_t>(cause);
			if (i >= static_cast<uint32_t>(Cause::Count))
				return;
			counts[i].fetch_add(1, std::memory_order_relaxed);
			if (cause != Cause::None)
				last.store(i, std::memory_order_relaxed);
		}
		uint64_t Of(Cause cause) const
		{
			const uint32_t i = static_cast<uint32_t>(cause);
			return i < static_cast<uint32_t>(Cause::Count) ? counts[i].load(std::memory_order_relaxed) : 0;
		}
		uint64_t Fallbacks() const
		{
			uint64_t n = 0;
			for (uint32_t i = 1; i < static_cast<uint32_t>(Cause::Count); ++i)
				n += counts[i].load(std::memory_order_relaxed);
			return n;
		}
		// The most recent fallback cause (None before the first).
		Cause Last() const { return static_cast<Cause>(last.load(std::memory_order_relaxed)); }

	private:
		std::atomic<uint64_t> counts[static_cast<uint32_t>(Cause::Count)];
		std::atomic<uint32_t> last;
	};

	class Kernel
	{
	public:
		Kernel() = default;
		~Kernel() { Close(); }
		Kernel(const Kernel&) = delete;
		Kernel& operator=(const Kernel&) = delete;

		// Compiles the shader (cs_5_0) and makes its constant buffer on `device`. False with LastError and
		// LastCause() == KernelOpenFailed on failure.
		bool Open(ID3D11Device* device);
		void Close();
		bool IsOpen() const { return shader != nullptr; }

		// Records the decode on `context` (the device's immediate context; the caller orders it after any GPU wait for
		// the upload and flushes). `raw`: the frame bytes as R8G8B8A8_UNORM (rowBytes / 4 texels x at least `height`
		// rows) with shader-resource binding. `out`: R8_UNORM with unordered-access binding, at least
		// OutDim(width, quarter) x OutDim(height, quarter). Leaves no state bound. False with LastError on a mismatch
		// (LastCause RunFailed, or DeviceRemoved when the device is gone). Views are cached per texture (a few).
		bool Run(ID3D11DeviceContext* context, ID3D11Texture2D* raw, Packing packing, uint32_t width, uint32_t height,
			bool quarter, ID3D11Texture2D* out);

		const std::string& LastError() const { return error; }
		Cause LastCause() const { return cause; }
		// Drops the cached views (and the references they hold to the textures), e.g. when the slot pool is re-made.
		void ReleaseViews();

	private:
		bool Fail(Cause c, const char* message);
		ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* tex);
		ID3D11UnorderedAccessView* UavFor(ID3D11Texture2D* tex);

		static constexpr int kViewCache = 8;
		struct SrvEntry { CComPtr<ID3D11Texture2D> tex; CComPtr<ID3D11ShaderResourceView> view; };
		struct UavEntry { CComPtr<ID3D11Texture2D> tex; CComPtr<ID3D11UnorderedAccessView> view; };
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11ComputeShader> shader;
		CComPtr<ID3D11Buffer> params;
		SrvEntry srvs[kViewCache];
		UavEntry uavs[kViewCache];
		int nextSrv = 0, nextUav = 0;
		std::string error;
		Cause cause = Cause::None;
	};
}
}
