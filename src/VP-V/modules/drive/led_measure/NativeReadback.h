// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/gpu.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// A non-blocking GPU-to-CPU readback for D3D11 (bug hunt 34 item 1, 2026-10-31), shared by the
// adaptive scaling statistics, the source-resolution tiles, the LED measurement and the camera link.
//
// Why: on D3D11 libplacebo's pl_buf_poll always reports "ready" and pl_buf_read is a
// CopyResource followed by a blocking Map, so a "polled" readback makes the render thread wait
// for the GPU frame (ReadbackRing.h). libplacebo's D3D11 backend also has no buffer transfers
// (limits.buf_transfer is false: pl_tex_download into a host-readable pl_buf is refused, and
// pl_tex_download itself is a blocking Map), so the readiness cannot come from pl_buf_poll. Here
// each slot is a VP-owned D3D11 texture wrapped for libplacebo (a shader writes it with
// imageStore) and copied natively into a staging texture after the pass.
//
// Readiness (fx/dv-frame-quiet): a real D3D11 FENCE (ID3D11DeviceContext4::Signal after the copy,
// ID3D11Fence::GetCompletedValue on the CPU). No D3D11 event query anywhere (the ablation record of
// fx/dv-determinism implicated the End(D3D11_QUERY_EVENT) / GetData pattern: renders that followed
// it varied from run to run). A fence read is a CPU-side value; Poll maps the staging copy with
// DO_NOT_WAIT only once the fence says the copy is complete, so there is one Map per copy.
// Poll: Pending (copy not issued yet, or the fence has not reached it, or DXGI_ERROR_WAS_STILL_DRAWING),
// Ready (mapped; Read returns it) or Failed (any other HRESULT: LastError; the caller releases the
// slot and shows the failure through its own status). A device-removed fence reads as
// complete, so the Map reports the removal as Failed (visible, 55791fff).
//
// No fence (a device or runtime without ID3D11Device5 / ID3D11DeviceContext4 / CreateFence): the
// readiness falls back to the DO_NOT_WAIT Map probe alone (the behaviour of fx/dv-determinism),
// VISIBLY: FenceUnavailableReadbacks() counts the live readbacks without a fence and the renderer
// shows FallbackCatalog::NativeReadback (never a log-only downgrade).
//
// Deferred copies (QueueDeferred): a shader pass that runs INSIDE libplacebo's frame (a hook) must
// not be followed by native D3D11 work there (the render of the frame varied run to run with the
// copy and its query issued between libplacebo's passes). While the host has Armed the deferral
// (DeferredCopies::Arm, once per rendered frame) QueueDeferred only records the slot; the host's
// DeferredCopies::Flush after the frame's submit issues every recorded copy and its fence signal.
// Not armed (tests, other hosts): QueueDeferred is Queue, immediately.
//
// Header-only (no project changes). Render thread only, like libplacebo's immediate context.
// Not D3D11 (or a format that cannot be stored): Create returns false and the caller keeps
// libplacebo's readback, where pl_buf_poll does report "busy".
namespace LedDrive
{
	// The outcome of polling one slot (a DO_NOT_WAIT Map: it does not wait for the GPU).
	enum class ReadState
	{
		Pending,   // DXGI_ERROR_WAS_STILL_DRAWING: the GPU still holds the copy; poll again later
		Ready,     // the copy is complete and mapped: Read returns it without waiting
		Failed     // any other HRESULT: an error (LastError); release the slot and surface it
	};

	// The readiness rule, pure (no D3D): what Poll does for a slot, given the fence. CPU-tested.
	struct ReadyGate
	{
		enum class Step
		{
			NotIssued,      // the copy has not been issued yet (never queued, or deferred): Pending
			FenceWaiting,   // issued, the fence has not reached its signal: Pending, no Map is attempted
			MapProbe        // the copy is complete (or there is no fence): the DO_NOT_WAIT Map decides
		};
		static Step Decide(bool copyIssued, bool haveFence, uint64_t signalValue, uint64_t completedValue)
		{
			if (!copyIssued) return Step::NotIssued;
			if (haveFence && completedValue < signalValue) return Step::FenceWaiting;
			return Step::MapProbe;
		}
	};

	// Live readbacks that have no fence (their readiness is the Map probe alone). Process-wide per
	// module; the renderer shows it through FallbackCatalog::NativeReadback.
	inline std::atomic<unsigned>& FenceUnavailableReadbacks()
	{
		static std::atomic<unsigned> live{ 0 };
		return live;
	}

	// Native copies recorded inside a frame and issued after it (pure bookkeeping, no D3D: the entries
	// carry the issue function). Per render thread. Arm() once per rendered frame BEFORE its hooks run;
	// Flush() after the frame's submit issues every recorded copy in order and disarms. Copies recorded
	// in an earlier armed frame and issued by a later Flush (the flush of their own frame did not run)
	// are counted in LateIssued(): shown, never silent. An owner that was destroyed meanwhile is skipped.
	class DeferredCopies
	{
	public:
		typedef void (*IssueFn)(void* owner, int slot);

		static DeferredCopies& ForThisThread()
		{
			static thread_local DeferredCopies instance;
			return instance;
		}

		void Arm() { armed = true; ++epoch; }
		bool Armed() const { return armed; }

		// False when not armed (the caller issues the copy now). A slot already recorded is not recorded twice.
		bool Defer(const std::shared_ptr<int>& alive, void* owner, int slot, IssueFn fn)
		{
			if (!armed) return false;
			for (const Entry& e : entries)
				if (e.owner == owner && e.slot == slot)
					return true;
			Entry e;
			e.alive = alive;
			e.owner = owner;
			e.slot = slot;
			e.fn = fn;
			e.epoch = epoch;
			entries.push_back(e);
			return true;
		}

		// Issues the recorded copies (FIFO) and disarms. Returns how many were issued.
		size_t Flush()
		{
			std::vector<Entry> work;
			work.swap(entries);
			armed = false;
			size_t issued = 0;
			for (const Entry& e : work)
			{
				if (e.alive.expired()) continue;
				if (e.epoch != epoch) ++lateIssued;
				e.fn(e.owner, e.slot);
				++issued;
			}
			return issued;
		}

		size_t Pending() const { return entries.size(); }
		unsigned long long LateIssued() const { return lateIssued; }

	private:
		struct Entry
		{
			std::weak_ptr<int> alive;
			void* owner = nullptr;
			int slot = 0;
			IssueFn fn = nullptr;
			uint64_t epoch = 0;
		};
		std::vector<Entry> entries;
		bool armed = false;
		uint64_t epoch = 0;
		unsigned long long lateIssued = 0;
	};

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
			alive = std::make_shared<int>(0);
			CreateFence(d3d->device);
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
			bool ok = immediate != nullptr;
			slot.resize(static_cast<size_t>(slots));
			for (Slot& s : slot)
			{
				if (!ok) break;
				ok = SUCCEEDED(d3d->device->CreateTexture2D(&desc, nullptr, &s.tex)) &&
					SUCCEEDED(d3d->device->CreateTexture2D(&staging, nullptr, &s.staging));
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
		// work on the immediate context as it goes, so the copy follows the pass on the GPU. Issues the
		// copy and its fence signal NOW: for a pass that ran outside libplacebo's frame.
		// Starts a new round for the slot: a mapping or failure left from the last round is dropped.
		void Queue(int i)
		{
			if (!Valid(i) || !immediate) return;
			Slot& s = slot[static_cast<size_t>(i)];
			Unmap(s);
			s.failed = false;
			s.deferred = false;
			IssueCopy(s);
		}

		// The same for a pass that ran INSIDE libplacebo's frame (a hook): while the host has armed the
		// deferral the copy is only recorded and issued by DeferredCopies::Flush after the frame's
		// submit; otherwise it is issued now, as Queue.
		void QueueDeferred(int i)
		{
			if (!Valid(i) || !immediate) return;
			Slot& s = slot[static_cast<size_t>(i)];
			Unmap(s);
			s.failed = false;
			if (!DeferredCopies::ForThisThread().Defer(alive, this, i, &IssueDeferredThunk))
			{
				s.deferred = false;
				IssueCopy(s);
				return;
			}
			s.issued = false;
			s.deferred = true;
		}

		// Does not wait for the GPU and maps nothing until the copy is complete: Pending while the copy
		// is not issued or the fence has not reached it, then a DO_NOT_WAIT map of the staging copy.
		// Ready keeps the mapping open for the Read that follows (one Map per copy, not a probe and a
		// second Map); Failed latches the HRESULT in LastError() until the slot is queued again.
		ReadState Poll(int i)
		{
			if (!Valid(i) || !immediate) return ReadState::Pending;
			Slot& s = slot[static_cast<size_t>(i)];
			if (s.failed) return ReadState::Failed;
			if (s.mapped) return ReadState::Ready;
			const bool haveFence = FenceAvailable();
			const ReadyGate::Step step = ReadyGate::Decide(s.issued, haveFence, s.signalValue, haveFence ? fence->GetCompletedValue() : 0);
			if (step != ReadyGate::Step::MapProbe)
			{
				// An issued copy and its signal sit in the immediate context's batch until a flush submits them. The
				// renderer flushes right after it issues them (FrameQuiet End), and Poll then runs inside its frame
				// (armed): no flush here. A host that does not flush (tests, other hosts) gets one flush per copy.
				if (step == ReadyGate::Step::FenceWaiting && !s.flushed && !DeferredCopies::ForThisThread().Armed())
				{
					immediate->Flush();
					s.flushed = true;
				}
				return ReadState::Pending;
			}
			const HRESULT hr = immediate->Map(s.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &s.map);
			const ReadState state = ClassifyMapResult(hr);
			if (state == ReadState::Ready)
				s.mapped = true;
			else if (state == ReadState::Failed)
			{
				s.failed = true;
				lastError = hr;
			}
			return state;
		}

		// True when the readiness comes from a D3D11 fence; false: the Map probe alone (shown by the host).
		bool FenceAvailable() const { return fence != nullptr && context4 != nullptr; }

		// The one rule for a DO_NOT_WAIT Map: success is Ready, DXGI_ERROR_WAS_STILL_DRAWING is the
		// only Pending, every other HRESULT (device removed, invalid call, out of memory...) is Failed.
		static ReadState ClassifyMapResult(HRESULT hr)
		{
			if (SUCCEEDED(hr)) return ReadState::Ready;
			return hr == DXGI_ERROR_WAS_STILL_DRAWING ? ReadState::Pending : ReadState::Failed;
		}

		bool Ready(int i) { return Poll(i) == ReadState::Ready; }

		// "Ever failed": the HRESULT of the most recent non-pending failure of any slot since Create
		// (S_OK when none). A later successful Poll or Queue does not clear it; the per-slot state
		// (Poll returning Failed) is what Queue resets.
		HRESULT LastError() const { return lastError; }
		bool Failed() const { return FAILED(lastError); }

		// Copies `rows` rows of `rowBytes` each into dst (tightly packed) from the copy Poll mapped
		// (or maps it now). False while the GPU still holds it (still pending), and on an error
		// (LastError / Poll report Failed). The slot is unmapped either way once it was read.
		bool Read(int i, void* dst, size_t rowBytes, int rows)
		{
			if (!Valid(i) || !immediate || !dst) return false;
			Slot& s = slot[static_cast<size_t>(i)];
			if (Poll(i) != ReadState::Ready) return false;
			const unsigned char* from = static_cast<const unsigned char*>(s.map.pData);
			unsigned char* to = static_cast<unsigned char*>(dst);
			for (int r = 0; r < rows; ++r)
				std::memcpy(to + static_cast<size_t>(r) * rowBytes, from + static_cast<size_t>(r) * s.map.RowPitch, rowBytes);
			Unmap(s);
			return true;
		}

		// Drops a Ready slot's mapping without reading it (the slot is queued again later).
		void Discard(int i)
		{
			if (Valid(i)) Unmap(slot[static_cast<size_t>(i)]);
		}

		// TESTS ONLY (induces a real Map failure): replaces slot i's staging texture with `staging`, taking over the
		// caller's reference; the previous staging texture is released and the slot's mapping dropped (a Failed latch stays until Queue).
		void ReplaceStagingForTest(int i, ID3D11Texture2D* staging)
		{
			if (!Valid(i)) { if (staging) staging->Release(); return; }
			Slot& s = slot[static_cast<size_t>(i)];
			Unmap(s);
			if (s.staging) s.staging->Release();
			s.staging = staging;
		}

		// Diagnostics and tests: the native textures behind slot i (not owned, no reference taken).
		ID3D11Texture2D* Texture(int i) const { return Valid(i) ? slot[static_cast<size_t>(i)].tex : nullptr; }
		ID3D11Texture2D* Staging(int i) const { return Valid(i) ? slot[static_cast<size_t>(i)].staging : nullptr; }

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
			pl_tex wrapped = nullptr;
			D3D11_MAPPED_SUBRESOURCE map{};
			bool mapped = false;   // Poll mapped the staging copy; Read / Discard / Queue unmap it
			bool failed = false;   // Poll got an error HRESULT (until the next Queue)
			bool issued = false;   // the copy (and its fence signal) is on the immediate context
			bool deferred = false; // recorded in DeferredCopies, not issued yet
			bool flushed = false;  // a flush has been issued since the copy (Poll's one-shot, see Poll)
			uint64_t signalValue = 0;   // the fence value this slot's copy signals (0: no fence)
		};

		// The copy and, with a fence, its signal: the signal follows the copy on the GPU timeline, so
		// GetCompletedValue() >= signalValue means the staging copy is complete.
		void IssueCopy(Slot& s)
		{
			immediate->CopyResource(s.staging, s.tex);
			s.signalValue = 0;
			if (FenceAvailable())
			{
				s.signalValue = ++fenceValue;
				context4->Signal(fence, s.signalValue);
			}
			s.issued = true;
			s.deferred = false;
			s.flushed = false;
		}

		static void IssueDeferredThunk(void* self, int i)
		{
			NativeTexReadback* rb = static_cast<NativeTexReadback*>(self);
			if (!rb->Valid(i) || !rb->immediate) return;
			Slot& s = rb->slot[static_cast<size_t>(i)];
			if (s.deferred) rb->IssueCopy(s);
		}

		void CreateFence(ID3D11Device* device)
		{
			ID3D11Device5* device5 = nullptr;
			if (device && SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5))) && device5)
			{
				if (immediate && SUCCEEDED(immediate->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&context4))) &&
					context4 && SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&fence))) &&
					fence)
				{
					fenceValue = 0;
				}
				else
				{
					if (fence) { fence->Release(); fence = nullptr; }
					if (context4) { context4->Release(); context4 = nullptr; }
				}
				device5->Release();
			}
			if (!FenceAvailable() && !countedWithoutFence)
			{
				countedWithoutFence = true;
				FenceUnavailableReadbacks().fetch_add(1, std::memory_order_relaxed);
			}
		}

		bool Valid(int i) const { return i >= 0 && static_cast<size_t>(i) < slot.size(); }

		void Unmap(Slot& s)
		{
			if (s.mapped && immediate) immediate->Unmap(s.staging, 0);
			s.mapped = false;
		}

		void ReleaseNative()
		{
			for (Slot& s : slot)
			{
				Unmap(s);
				if (s.tex) s.tex->Release();
				if (s.staging) s.staging->Release();
			}
			slot.clear();
			alive.reset();   // recorded deferred copies of this readback are skipped by the next Flush
			if (fence) { fence->Release(); fence = nullptr; }
			if (context4) { context4->Release(); context4 = nullptr; }
			if (countedWithoutFence)
			{
				countedWithoutFence = false;
				FenceUnavailableReadbacks().fetch_sub(1, std::memory_order_relaxed);
			}
			if (immediate) { immediate->Release(); immediate = nullptr; }
			owner = nullptr;
			lastError = S_OK;
		}

		std::vector<Slot> slot;
		ID3D11DeviceContext* immediate = nullptr;
		ID3D11DeviceContext4* context4 = nullptr;
		ID3D11Fence* fence = nullptr;
		uint64_t fenceValue = 0;
		bool countedWithoutFence = false;
		std::shared_ptr<int> alive;   // deferred entries hold a weak reference
		pl_gpu owner = nullptr;
		HRESULT lastError = S_OK;
	};
}
