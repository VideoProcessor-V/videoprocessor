// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// A fixed host-keyed table for hub modules (the pattern of ResyncExtension's slot table, for state a
// module creates and destroys itself instead of owning it through the renderer). Every lookup is
// lock-free on our side: a key compare and one shared_ptr load. Writers are the lifecycle callbacks
// (GpuCreated / SettingsApplied / GpuReleased / Shutdown) of ONE renderer each; a retiring renderer's
// release can run on another thread while a replacement renderer registers, so slots are claimed with a
// compare-exchange on the key and the state is handed over as a shared_ptr: a status reader on the GUI
// thread that already holds the pointer keeps the state alive while the lifecycle thread removes it.
//
// `aux` is a raw per-host pointer the module may keep without owning anything (the renderer's D3D11
// device between GpuCreated and GpuReleased: not reference counted, cleared by the module at release).

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

namespace Vpv
{
	namespace Hub
	{
		template<class T, size_t kSlots = 16>
		class HostSlots
		{
		public:
			// The state of `host`, or null. Safe from any thread.
			std::shared_ptr<T> Find(const void* host) const
			{
				if (!host)
					return nullptr;
				for (const Slot& s : slots_)
					if (s.key.load(std::memory_order_acquire) == host)
					{
						std::shared_ptr<T> st = std::atomic_load(&s.state);
						return st && st->host == host ? st : nullptr;   // a slot re-claimed between the loads is not ours
					}
				return nullptr;
			}

			// T needs a `host` member (the key the state was created for).
			// Claims (or finds) the slot of `host` without a state: for `aux`. False when the table is full.
			bool Claim(const void* host)
			{
				return host && ClaimSlot(host) != nullptr;
			}

			// Stores the state of `host` (replacing none: an existing state is kept and false returned).
			bool Put(const void* host, std::shared_ptr<T> state)
			{
				Slot* s = host ? ClaimSlot(host) : nullptr;
				if (!s || std::atomic_load(&s->state))
					return false;
				std::atomic_store(&s->state, std::move(state));
				return true;
			}

			// Drops the state of `host` (returned so the caller destroys it outside any lock) and, when
			// `release` is set, the slot itself (aux cleared).
			std::shared_ptr<T> Remove(const void* host, bool release)
			{
				std::shared_ptr<T> old;
				if (!host)
					return old;
				for (Slot& s : slots_)
					if (s.key.load(std::memory_order_acquire) == host)
					{
						old = std::atomic_exchange(&s.state, std::shared_ptr<T>());
						if (release)
						{
							s.aux.store(nullptr, std::memory_order_release);
							s.key.store(nullptr, std::memory_order_release);
						}
						break;
					}
				return old;
			}

			void SetAux(const void* host, void* aux)
			{
				if (Slot* s = host ? ClaimSlot(host) : nullptr)
					s->aux.store(aux, std::memory_order_release);
			}
			void* Aux(const void* host) const
			{
				if (!host)
					return nullptr;
				for (const Slot& s : slots_)
					if (s.key.load(std::memory_order_acquire) == host)
						return s.aux.load(std::memory_order_acquire);
				return nullptr;
			}

			// Calls f(host, state) for every slot holding a state (module destruction / diagnostics).
			template<class F>
			void ForEach(F&& f) const
			{
				for (const Slot& s : slots_)
				{
					const void* key = s.key.load(std::memory_order_acquire);
					if (!key)
						continue;
					std::shared_ptr<T> st = std::atomic_load(&s.state);
					if (st)
						f(key, st);
				}
			}

			// Slots with a key (tests / diagnostics).
			size_t Used() const
			{
				size_t n = 0;
				for (const Slot& s : slots_)
					if (s.key.load(std::memory_order_acquire) != nullptr)
						++n;
				return n;
			}
			size_t States() const
			{
				size_t n = 0;
				for (const Slot& s : slots_)
					if (s.key.load(std::memory_order_acquire) != nullptr && std::atomic_load(&s.state))
						++n;
				return n;
			}

		private:
			struct Slot
			{
				std::atomic<const void*> key{ nullptr };
				std::atomic<void*> aux{ nullptr };
				mutable std::shared_ptr<T> state;   // accessed with std::atomic_load / atomic_store only
			};

			Slot* ClaimSlot(const void* host)
			{
				for (Slot& s : slots_)
					if (s.key.load(std::memory_order_acquire) == host)
						return &s;
				for (Slot& s : slots_)
				{
					const void* expected = nullptr;
					if (s.key.compare_exchange_strong(expected, host, std::memory_order_acq_rel))
						return &s;
				}
				return nullptr;
			}

			std::array<Slot, kSlots> slots_;
		};
	}
}
