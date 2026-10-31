// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3d11.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/gpu.h>

#include <cstddef>
#include <cstring>
#include <vector>

// A non-blocking GPU-to-CPU readback for D3D11 (bug hunt 34 item 1, 2026-10-31), shared by the
// adaptive scaling statistics and the source-resolution tiles; the LED measurement's own copy
// (LedMeasurementHook) follows the same pattern.
//
// Why: on D3D11 libplacebo's pl_buf_poll always reports "ready" and pl_buf_read is a
// CopyResource followed by a blocking Map, so a "polled" readback makes the render thread wait
// for the GPU frame (ReadbackRing.h). Here each slot is a VP-owned D3D11 texture wrapped for
// libplacebo (a shader writes it with imageStore), copied natively into a staging texture with
// an event query after the pass. A slot is read only once its query reports done (GetData with
// DO_NOT_FLUSH) and is mapped with DO_NOT_WAIT, so the render thread never waits.
//
// Header-only (no project changes). Render thread only, like libplacebo's immediate context.
// Not D3D11 (or a format that cannot be stored): Create returns false and the caller keeps
// libplacebo's readback, where pl_buf_poll does report "busy".
namespace LedDrive
{
	class NativeTexReadback
	{
	public:
		NativeTexReadback() = default;
		NativeTexReadback(const NativeTexReadback&) = delete;
		NativeTexReadback& operator=(const NativeTexReadback&) = delete;
		~NativeTexReadback() { ReleaseNative(); }   // the pl_tex wrappers need Release(gpu) first

		// `slots` textures of w x h in `format` (DXGI_FORMAT_R32_FLOAT or R32G32B32A32_FLOAT:
		// typed UAV stores every D3D11 feature level 11 GPU supports).
		bool Create(pl_gpu gpu, int slots, int w, int h, DXGI_FORMAT format)
		{
			Release(owner);
			const pl_d3d11 d3d = gpu ? pl_d3d11_get(gpu) : nullptr;
			if (!d3d || !d3d->device || slots <= 0 || w <= 0 || h <= 0)
				return false;
			d3d->device->GetImmediateContext(&immediate);
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(w);
			desc.Height = static_cast<UINT>(h);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			D3D11_TEXTURE2D_DESC staging = desc;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.BindFlags = 0;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			D3D11_QUERY_DESC query{};
			query.Query = D3D11_QUERY_EVENT;
			bool ok = immediate != nullptr;
			slot.resize(static_cast<size_t>(slots));
			for (Slot& s : slot)
			{
				if (!ok) break;
				ok = SUCCEEDED(d3d->device->CreateTexture2D(&desc, nullptr, &s.tex)) &&
					SUCCEEDED(d3d->device->CreateTexture2D(&staging, nullptr, &s.staging)) &&
					SUCCEEDED(d3d->device->CreateQuery(&query, &s.done));
				if (ok)
				{
					pl_d3d11_wrap_params wrap{};
					wrap.tex = s.tex;
					s.wrapped = pl_d3d11_wrap(gpu, &wrap);
					ok = s.wrapped && s.wrapped->params.storable;
				}
			}
			owner = gpu;
			if (!ok)
			{
				Release(gpu);
				return false;
			}
			return true;
		}

		bool Active() const { return !slot.empty() && immediate != nullptr; }
		int Slots() const { return static_cast<int>(slot.size()); }
		// The storage image a pass writes slot `i` into.
		pl_tex Target(int i) const { return Valid(i) ? slot[static_cast<size_t>(i)].wrapped : nullptr; }

		// After the pass that wrote Target(i) has been dispatched: libplacebo issues its D3D11
		// work on the immediate context as it goes, so the copy follows the pass on the GPU.
		void Queue(int i)
		{
			if (!Valid(i) || !immediate) return;
			Slot& s = slot[static_cast<size_t>(i)];
			immediate->CopyResource(s.staging, s.tex);
			immediate->End(s.done);
		}

		// Never blocks: the copy's event query, without a flush.
		bool Ready(int i) const
		{
			if (!Valid(i) || !immediate) return false;
			return immediate->GetData(slot[static_cast<size_t>(i)].done, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
		}

		// Copies `rows` rows of `rowBytes` each into dst (tightly packed). False while the GPU
		// still holds it (DXGI_ERROR_WAS_STILL_DRAWING): try again later.
		bool Read(int i, void* dst, size_t rowBytes, int rows) const
		{
			if (!Valid(i) || !immediate || !dst) return false;
			const Slot& s = slot[static_cast<size_t>(i)];
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(immediate->Map(s.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
				return false;
			const unsigned char* from = static_cast<const unsigned char*>(mapped.pData);
			unsigned char* to = static_cast<unsigned char*>(dst);
			for (int r = 0; r < rows; ++r)
				std::memcpy(to + static_cast<size_t>(r) * rowBytes, from + static_cast<size_t>(r) * mapped.RowPitch, rowBytes);
			immediate->Unmap(s.staging, 0);
			return true;
		}

		// While the GPU that created it is alive (the wrappers are destroyed through that GPU,
		// whatever `gpu` says; `gpu` is only a check).
		void Release(pl_gpu gpu)
		{
			(void)gpu;
			for (Slot& s : slot)
				if (s.wrapped && owner)
					pl_tex_destroy(owner, &s.wrapped);
			ReleaseNative();
		}

	private:
		struct Slot
		{
			ID3D11Texture2D* tex = nullptr;
			ID3D11Texture2D* staging = nullptr;
			ID3D11Query* done = nullptr;
			pl_tex wrapped = nullptr;
		};

		bool Valid(int i) const { return i >= 0 && static_cast<size_t>(i) < slot.size(); }

		void ReleaseNative()
		{
			for (Slot& s : slot)
			{
				if (s.tex) s.tex->Release();
				if (s.staging) s.staging->Release();
				if (s.done) s.done->Release();
			}
			slot.clear();
			if (immediate) { immediate->Release(); immediate = nullptr; }
			owner = nullptr;
		}

		std::vector<Slot> slot;
		ID3D11DeviceContext* immediate = nullptr;
		pl_gpu owner = nullptr;
	};
}
