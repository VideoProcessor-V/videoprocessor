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
// Several sinks per owner (SID-C2b). Register(owner, name, sink[, wants]) keeps one sink per (owner, name); the
// old Register(owner, sink) is the name "" (the skeleton's analysis lane is unchanged and byte-identical). Deliver
// hands the frame to every sink of the owner in a fixed order (by name, "" first), each outside the registry lock.
// A sink with a `wants` predicate is only handed a frame while the predicate is true (the chain estimator wants a
// plane only when an evaluation is due). Wants(owner) is true when any sink of the owner wants a frame now;
// AnyWants() is true while any sink at all is registered (one atomic load, the predicates are not evaluated there:
// an over-estimate, which only decides whether a shared plane is attached). Unregister(owner) and
// Unregister(owner, name) remove the named sink (the old call: ""); UnregisterAll(owner) removes every sink of the
// owner (the Host, whose store is going away: no sink may outlive it).
//
// Threading. Register / Unregister / SetStore / ClearStore: any thread (lifecycle). Wants / AnyWants:
// the conversion thread (OnArrival) and the worker; AnyWants is one atomic load, so with no sink
// registered nothing locks and nothing changes. Deliver runs on the frame-analysis worker. The registry
// mutex is never held while a sink runs: Deliver copies shared_ptrs to the sinks out first.
//
// Header-only: the registry is one function-local static per module (the renderer DLL, where the Host
// and the skeleton modules live together). Pure C++14.

#include <VP-V/core/LaneInterfaces.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FrameAnalysis { class Store; }

namespace VpFrameAnalysis
{
	namespace SourceFrameTap
	{
		using Sink = std::function<void(const VpSkeleton::SourceFrameView&)>;
		// Optional: true while the sink wants a frame (cheap, any thread; evaluated by Wants and Deliver).
		using WantsFn = std::function<bool()>;

		struct Entry
		{
			Sink sink;
			WantsFn wants;                        // empty = always wants
			std::atomic<uint64_t> delivered{ 0 };
			std::atomic<uint64_t> skipped{ 0 };   // arrivals whose luma could not be decoded (never delivered)
		};

		struct Registry
		{
			std::mutex mutex;
			std::map<const void*, std::map<std::string, std::shared_ptr<Entry>>> sinks;   // per owner, ordered by name
			std::map<const void*, FrameAnalysis::Store*> stores;
			std::map<const void*, uint64_t> generations;   // per owner: bumped whenever its store registration changes
			std::atomic<int> count{ 0 };                   // sinks registered, all owners and names
			std::atomic<uint64_t> epoch{ 0 };              // bumped on any generation change (a cheap "look again")

			void Recount()   // with `mutex` held
			{
				int n = 0;
				for (const auto& o : sinks)
					n += static_cast<int>(o.second.size());
				count.store(n, std::memory_order_release);
			}
		};

		inline Registry& Reg()
		{
			static Registry registry;
			return registry;
		}

		// Registers (or replaces) the sink `name` of `owner`. False for a null owner or sink.
		inline bool Register(const void* owner, const std::string& name, Sink sink, WantsFn wants = WantsFn())
		{
			if (!owner || !sink)
				return false;
			std::shared_ptr<Entry> entry = std::make_shared<Entry>();
			entry->sink = std::move(sink);
			entry->wants = std::move(wants);
			Registry& r = Reg();
			std::shared_ptr<Entry> old;   // destroyed after the lock is released
			{
				std::lock_guard<std::mutex> lock(r.mutex);
				std::shared_ptr<Entry>& slot = r.sinks[owner][name];
				old = std::move(slot);
				slot = std::move(entry);
				r.Recount();
			}
			return true;
		}

		// The old single-sink form: the sink named "".
		inline bool Register(const void* owner, Sink sink)
		{
			return Register(owner, std::string(), std::move(sink), WantsFn());
		}

		inline void Unregister(const void* owner, const std::string& name)
		{
			Registry& r = Reg();
			std::shared_ptr<Entry> old;   // destroyed after the lock is released
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			if (it != r.sinks.end())
			{
				const auto e = it->second.find(name);
				if (e != it->second.end())
				{
					old = std::move(e->second);
					it->second.erase(e);
				}
				if (it->second.empty())
					r.sinks.erase(it);
			}
			r.Recount();
		}

		// The old form: the sink named "" only (other named sinks of the owner stay).
		inline void Unregister(const void* owner) { Unregister(owner, std::string()); }

		// Every sink of `owner` (the Host going away).
		inline void UnregisterAll(const void* owner)
		{
			Registry& r = Reg();
			std::map<std::string, std::shared_ptr<Entry>> old;   // destroyed after the lock is released
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			if (it != r.sinks.end())
			{
				old = std::move(it->second);
				r.sinks.erase(it);
			}
			r.Recount();
		}

		// Any sink registered at all (one atomic load: the fast path of every arrival).
		inline bool AnyWants() { return Reg().count.load(std::memory_order_acquire) > 0; }

		// The sinks of `owner` in delivery order (copied out under the lock; empty when none).
		inline std::vector<std::shared_ptr<Entry>> EntriesOf(const void* owner)
		{
			std::vector<std::shared_ptr<Entry>> out;
			if (!owner || !AnyWants())
				return out;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			if (it != r.sinks.end())
				for (const auto& e : it->second)
					out.push_back(e.second);
			return out;
		}

		// True when any sink of `owner` wants a frame now (a sink without a predicate always does).
		inline bool Wants(const void* owner)
		{
			const std::vector<std::shared_ptr<Entry>> entries = EntriesOf(owner);
			for (const std::shared_ptr<Entry>& e : entries)
				if (e && e->sink && (!e->wants || e->wants()))
					return true;
			return false;
		}

		// True when the sink `name` of `owner` is registered (whether or not it wants a frame now): what a module that owns
		// one named sink asks to learn that ITS registration was lost (Wants() is true for any sink of the owner).
		inline bool Registered(const void* owner, const std::string& name)
		{
			if (!owner || !AnyWants())
				return false;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			return it != r.sinks.end() && it->second.find(name) != it->second.end();
		}

		// The first sink of `owner` in delivery order (the one named "" when present): the old single-sink accessor.
		inline std::shared_ptr<Entry> EntryOf(const void* owner)
		{
			const std::vector<std::shared_ptr<Entry>> entries = EntriesOf(owner);
			return entries.empty() ? nullptr : entries.front();
		}

		// Hands `view` to every sink of `owner` that wants it, in name order (each runs on the calling thread, outside the
		// lock). True when at least one sink got it.
		inline bool Deliver(const void* owner, const VpSkeleton::SourceFrameView& view)
		{
			const std::vector<std::shared_ptr<Entry>> entries = EntriesOf(owner);
			bool any = false;
			for (const std::shared_ptr<Entry>& e : entries)
			{
				if (!e || !e->sink)
					continue;
				if (e->wants && !e->wants())
					continue;
				e->sink(view);
				e->delivered.fetch_add(1, std::memory_order_relaxed);
				any = true;
			}
			return any;
		}

		// An arrival the sinks wanted but whose luma plane could not be produced (counted for every sink that wants a frame now).
		inline void NoteSkipped(const void* owner)
		{
			const std::vector<std::shared_ptr<Entry>> entries = EntriesOf(owner);
			for (const std::shared_ptr<Entry>& e : entries)
				if (e && (!e->wants || e->wants()))
					e->skipped.fetch_add(1, std::memory_order_relaxed);
		}
		// Frames delivered to the sinks of `owner`: the sum over the sinks (one sink: its own count, as before).
		inline uint64_t Delivered(const void* owner)
		{
			uint64_t n = 0;
			for (const std::shared_ptr<Entry>& e : EntriesOf(owner))
				if (e)
					n += e->delivered.load(std::memory_order_relaxed);
			return n;
		}
		// Arrivals skipped for lack of a decodable plane: the largest count over the sinks (one sink: its own count).
		inline uint64_t Skipped(const void* owner)
		{
			uint64_t n = 0;
			for (const std::shared_ptr<Entry>& e : EntriesOf(owner))
				if (e)
					n = (std::max)(n, e->skipped.load(std::memory_order_relaxed));
			return n;
		}
		// One named sink of `owner` (0 when it is not registered).
		inline uint64_t Delivered(const void* owner, const std::string& name)
		{
			if (!owner || !AnyWants())
				return 0;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.sinks.find(owner);
			if (it == r.sinks.end())
				return 0;
			const auto e = it->second.find(name);
			return (e == it->second.end() || !e->second) ? 0u : e->second->delivered.load(std::memory_order_relaxed);
		}

		// The renderer's frame-analysis store, registered by VpFrameAnalysis::Host::Owner(): the camera
		// extension reads it. Null when the owner has no Host (or it is gone).
		inline void SetStore(const void* owner, FrameAnalysis::Store* store)
		{
			if (!owner)
				return;
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.stores.find(owner);
			const FrameAnalysis::Store* current = it == r.stores.end() ? nullptr : it->second;
			if (current == store)
				return;   // the same registration (Owner() is called at every settings apply): no new generation
			if (store)
				r.stores[owner] = store;
			else
				r.stores.erase(owner);
			++r.generations[owner];
			r.epoch.fetch_add(1, std::memory_order_release);
		}
		// A renderer that is destroyed (its Host clears the store) and a new one created at the SAME address
		// differ in generation: per-host module state built for the old one is stale (hub modules compare).
		inline uint64_t Generation(const void* owner)
		{
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			const auto it = r.generations.find(owner);
			return it == r.generations.end() ? 0u : it->second;
		}
		inline uint64_t Epoch() { return Reg().epoch.load(std::memory_order_acquire); }
		// The renderer identities that registered a store (Host::Owner): a module that is not told its renderer finds it here when
		// exactly one exists.
		inline std::vector<const void*> StoreOwners()
		{
			Registry& r = Reg();
			std::lock_guard<std::mutex> lock(r.mutex);
			std::vector<const void*> out;
			for (const auto& s : r.stores)
				out.push_back(s.first);
			return out;
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
