// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The source frame tap: the VP-owned seam between the frame-analysis worker (VpFrameAnalysis::Host)
// and the skeleton's analysis lane. A module registers a sink for one renderer (the owner key is the
// renderer identity `host` of every hub callback; VpFrameAnalysis::Host::Owner(this) gives the Host the
// same key). The Host then decodes the shared luma plane for that renderer even when no other reader
// wants it, and the worker hands every decoded plane to the sink as a VpSkeleton::SourceFrameView.
//
// What the view carries. The luma codes are the 10-bit sampler codes of AnalysisLumaSource (studio codes
// when the VP-V limited domain is on, which is always the case outside the Original pathway); a sample
// scaled to the full 16-bit range is (code << 6), so msbShift is 6. strideSamples == width (the plane is
// tightly packed). keepAlive owns the shared plane: while any copy of the view lives, VpFrameAnalysis's
// plane pool cannot recycle the plane (it recycles only a plane nobody else holds, use_count == 1).
//
// Threading. Register / Unregister / SetStore / ClearStore: any thread (lifecycle). Wants / AnyWants:
// the conversion thread (OnArrival) and the worker; AnyWants is one atomic load, so with no sink
// registered nothing locks and nothing changes. Deliver runs on the frame-analysis worker. The registry
// mutex is never held while a sink runs: Deliver copies a shared_ptr to the sink out first.
//
// Header-only: the registry is one function-local static per module (the renderer DLL, where the Host
// and the skeleton modules live together). Pure C++14.

#include <VP-V/core/LaneInterfaces.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace FrameAnalysis { class Store; }

namespace VpFrameAnalysis
{
	namespace SourceFrameTap
	{
		using Sink = std::function<void(const VpSkeleton::SourceFrameView&)>;

		struct Entry
		{
			Sink sink;
			std::atomic<uint64_t> delivered{ 0 };
			std::atomic<uint64_t> skipped{ 0 };   // arrivals whose luma could not be decoded (never delivered)
		};

		struct Registry
		{
			std::mutex mutex;
			std::map<const void*, std::shared_ptr<Entry>> sinks;
			std::map<const void*, FrameAnalysis::Store*> stores;
			std::atomic<int> count{ 0 };
		};

		inline Registry& Reg()
		{
			static Registry registry;
			return registry;
		}

		// Registers (or replaces) the sink of `owner`. False for a null owner or sink.
		inline bool Register(const void* owner, Sink sink)
		{
			if (!owner || !sink)
				return false;
			std::shared_ptr<Entry> entry = std::make_shared<Entry>();
			entry->sink = std::move(sink);
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			r.sinks[owner] = std::move(entry);
			r.count.store(static_cast<int>(r.sinks.size()), std::memory_order_release);
			return true;
		}

		inline void Unregister(const void* owner)
		{
			Registry& r = Reg();
			std::shared_ptr<Entry> old;   // destroyed after the lock is released
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			if (it != r.sinks.end())
			{
				old = std::move(it->second);
				r.sinks.erase(it);
			}
			r.count.store(static_cast<int>(r.sinks.size()), std::memory_order_release);
		}

		// Any sink registered at all (one atomic load: the fast path of every arrival).
		inline bool AnyWants() { return Reg().count.load(std::memory_order_acquire) > 0; }

		inline bool Wants(const void* owner)
		{
			if (!owner || !AnyWants())
				return false;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			return r.sinks.find(owner) != r.sinks.end();
		}

		inline std::shared_ptr<Entry> EntryOf(const void* owner)
		{
			if (!owner || !AnyWants())
				return nullptr;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			return it == r.sinks.end() ? nullptr : it->second;
		}

		// Hands `view` to the sink of `owner` (the sink runs on the calling thread, outside the lock).
		inline bool Deliver(const void* owner, const VpSkeleton::SourceFrameView& view)
		{
			const std::shared_ptr<Entry> entry = EntryOf(owner);
			if (!entry || !entry->sink)
				return false;
			entry->sink(view);
			entry->delivered.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// An arrival the sink wanted but whose luma plane could not be produced.
		inline void NoteSkipped(const void* owner)
		{
			if (const std::shared_ptr<Entry> entry = EntryOf(owner))
				entry->skipped.fetch_add(1, std::memory_order_relaxed);
		}
		inline uint64_t Delivered(const void* owner)
		{
			const std::shared_ptr<Entry> entry = EntryOf(owner);
			return entry ? entry->delivered.load(std::memory_order_relaxed) : 0u;
		}
		inline uint64_t Skipped(const void* owner)
		{
			const std::shared_ptr<Entry> entry = EntryOf(owner);
			return entry ? entry->skipped.load(std::memory_order_relaxed) : 0u;
		}

		// The renderer's frame-analysis store, registered by VpFrameAnalysis::Host::Owner(): the camera
		// extension reads it. Null when the owner has no Host (or it is gone).
		inline void SetStore(const void* owner, FrameAnalysis::Store* store)
		{
			if (!owner)
				return;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			if (store)
				r.stores[owner] = store;
			else
				r.stores.erase(owner);
		}
		inline FrameAnalysis::Store* StoreFor(const void* owner)
		{
			if (!owner)
				return nullptr;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.stores.find(owner);
			return it == r.stores.end() ? nullptr : it->second;
		}
	}
}
