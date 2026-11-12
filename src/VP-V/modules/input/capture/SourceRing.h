// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P10 D5 phase A (refs/118 s3): the P13 "source ring" producer, luma-only first slice of EU-P13. input.capture owns it
// (ProductCatalog.h P13, "VIEWS of the unpacked planes of the queued frames (owned by queue/upload): 0 extra bytes").
//
// What it is: a small seq-keyed list of the frames still QUEUED ahead of the render, each with the hand-off object of its
// early-upload slot (EarlyUploadHandoff) and the raw frame's facts (encoding, row pitch, height). It owns no texture and
// makes no copy: a reader (the P10 tap builder, phase C) finds a frame by (seq, generation), peeks the hand-off
// (EarlyUploadHandoff::PeekAttached: never waits, never abandons) and reads the slot on the render device.
//
// Fed by the existing S2 seam: MotionAhead::CollectQueueAhead raises the CollectAhead phase with QueueView::ahead (the
// queue's frames up to the lane lead); CaptureExtension::OnFrame passes them to Update. Update runs on the render-loop
// thread WITH m_queueMutex held. Lock order: queue mutex -> ring mutex; a reader never takes the queue mutex.
//
// Lifetime law (refs/118 risk 2): nothing is destroyed under the queue lock. An entry that leaves the list is MOVED to the
// graveyard (a move destroys nothing); ReleaseGraveyard runs at the next S2 Begin (render thread, no queue lock) and is the
// only place an entry's reference is dropped, so the last reference to a hand-off, and with it the slot's return to the
// pool, never happens under m_queueMutex.
//
// Off by default: a ring does nothing until a reader declares itself with SetWanted(true) (the skeleton, when the lane is
// active and lane_tap_source = auto). AnyWanted() is one relaxed atomic load: the walk builds no list when nobody wants one.
// Original pathway: the CollectAhead call site is gated off there, and the Original overlay turns the setting to cpu, so the
// ring is never fed. C++14, header-only.

#include <vprenderer/RendererExtension.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Vpv
{
	namespace Capture
	{
		class SourceRing
		{
		public:
			// The lane lead is capped at the ring-slot budget (the skeleton's QueueShape); +1 for the front frame.
			static constexpr size_t kCapacity = 9;
			// The most entries one pass may hand over (the walk's stack array).
			static constexpr size_t kMaxPerPass = kCapacity;

			using Entry = RendererExtension::AheadSlot;

			struct Stats
			{
				uint64_t updates = 0;        // Update calls that carried a list
				uint64_t added = 0;          // entries taken
				uint64_t displaced = 0;      // entries moved to the graveyard (left the queue list)
				uint64_t overCapacity = 0;   // entries refused because the ring was full
				uint64_t released = 0;       // entries dropped by ReleaseGraveyard
				size_t entries = 0;
				size_t graveyard = 0;
			};

			SourceRing()
			{
				entries_.reserve(kCapacity);
				graveyard_.reserve(kCapacity * 2);
			}
			SourceRing(const SourceRing&) = delete;
			SourceRing& operator=(const SourceRing&) = delete;
			~SourceRing() { SetWanted(false); }

			// ---- demand ------------------------------------------------------------------------------------------
			// Any thread. A reader declares itself (the skeleton, at its settings apply); idempotent per ring.
			void SetWanted(bool wanted)
			{
				const bool was = wanted_.exchange(wanted, std::memory_order_acq_rel);
				if (was == wanted)
					return;
				Hosts().fetch_add(wanted ? 1 : -1, std::memory_order_relaxed);
				if (!wanted)
					Clear();
			}
			bool Wanted() const { return wanted_.load(std::memory_order_acquire); }
			// True when some ring (any renderer) is wanted: the walk's one cheap gate before it builds a list.
			static bool AnyWanted() { return Hosts().load(std::memory_order_relaxed) > 0; }

			// ---- producer: render-loop thread, queue lock held ---------------------------------------------------
			// `slots[0..count)`: the queue's frames from the front up to the lane lead. The ring afterwards holds exactly
			// those frames that are present (an entry keeps its place while its frame is listed; a new frame is added; a
			// frame no longer listed is moved to the graveyard). Allocation-free once the vectors are reserved; a pass
			// with no reader is ignored. A frame is keyed by (seq, generation): the same seq in another generation is
			// another entry (the old one leaves).
			void Update(const Entry* slots, size_t count)
			{
				if (!Wanted())
					return;
				std::lock_guard<std::mutex> lock(m_);
				++stats_.updates;
				for (size_t i = 0; i < entries_.size();)
				{
					if (Listed(slots, count, entries_[i].seq, entries_[i].generation))
					{
						++i;
						continue;
					}
					graveyard_.push_back(std::move(entries_[i]));   // a move: nothing is destroyed under the caller's lock
					if (i + 1 < entries_.size())
						entries_[i] = std::move(entries_.back());
					entries_.pop_back();
					++stats_.displaced;
				}
				for (size_t i = 0; i < count; ++i)
				{
					if (Has(slots[i].seq, slots[i].generation))
						continue;
					if (entries_.size() >= kCapacity)
					{
						++stats_.overCapacity;
						continue;
					}
					entries_.push_back(slots[i]);
					++stats_.added;
				}
			}

			// ---- the S2 Begin: render thread, no queue lock -------------------------------------------------------
			// Drops the graveyard's references (the only place they are dropped). Returns how many entries it released.
			size_t ReleaseGraveyard()
			{
				std::vector<Entry> dead;   // destroyed after the lock is released
				{
					std::lock_guard<std::mutex> lock(m_);
					dead.swap(graveyard_);
					graveyard_.reserve(kCapacity * 2);
					stats_.released += dead.size();
				}
				return dead.size();
			}

			// ---- reader: render thread ----------------------------------------------------------------------------
			// A copy of the entry for (seq, generation), or false. The copy holds the hand-off, so the slot cannot be
			// returned to the pool while the reader uses it (the copy's release is the reader's, on the render thread).
			bool Find(uint64_t seq, uint64_t generation, Entry& out) const
			{
				std::lock_guard<std::mutex> lock(m_);
				for (const Entry& e : entries_)
					if (e.seq == seq && e.generation == generation)
					{
						out = e;
						return true;
					}
				return false;
			}

			Stats GetStats() const
			{
				std::lock_guard<std::mutex> lock(m_);
				Stats s = stats_;
				s.entries = entries_.size();
				s.graveyard = graveyard_.size();
				return s;
			}

		private:
			static std::atomic<int>& Hosts()
			{
				static std::atomic<int> hosts(0);
				return hosts;
			}
			static bool Listed(const Entry* slots, size_t count, uint64_t seq, uint64_t generation)
			{
				for (size_t i = 0; i < count; ++i)
					if (slots[i].seq == seq && slots[i].generation == generation)
						return true;
				return false;
			}
			bool Has(uint64_t seq, uint64_t generation) const
			{
				for (const Entry& e : entries_)
					if (e.seq == seq && e.generation == generation)
						return true;
				return false;
			}
			// SetWanted(false): the references go to the graveyard (released at the next Begin, or by the destructor's owner).
			void Clear()
			{
				std::lock_guard<std::mutex> lock(m_);
				for (Entry& e : entries_)
					graveyard_.push_back(std::move(e));
				entries_.clear();
			}

			std::atomic<bool> wanted_{ false };
			mutable std::mutex m_;
			std::vector<Entry> entries_;     // under m_
			std::vector<Entry> graveyard_;   // under m_
			Stats stats_;                    // under m_
		};
	}
}
