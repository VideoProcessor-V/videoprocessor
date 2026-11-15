// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs\105 s3.5, phase 1b): the bookkeeping of the engine-luma ring - which texture index holds which
// frame, keyed (generation, sequence). It replaces the producer's previous/current ping-pong: a pair is (Find(N-1), slot of N) and
// the engine's input frames are looked up by key, never "the previous one". The ring holds `capacity` recent frames (3 now: N-1, N,
// N+1 of the pipeline; 16 when the long pair lands) plus up to `pinCapacity` PINNED frames (anchors the long pair needs after the
// GOP outlived the ring). Pure CPU, no D3D11: the owner allocates the textures per index (up to TotalSlots(), on first use) and
// declares their VRAM. Not thread safe: one owner (the producer's engine thread). Header only, C++14.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MotionAhead
{
	class EngineLumaRing
	{
	public:
		enum : int { kNoSlot = -1 };

		explicit EngineLumaRing(size_t capacity = 3, size_t pinCapacity = 0)
			: capacity_(capacity < 1 ? 1 : capacity), pinCapacity_(pinCapacity)
		{
			entries_.reserve(capacity_ + pinCapacity_);
		}

		size_t Capacity() const { return capacity_; }
		size_t PinCapacity() const { return pinCapacity_; }
		size_t TotalSlots() const { return capacity_ + pinCapacity_; }   // texture indices handed out are < TotalSlots()
		size_t Live() const { return entries_.size(); }
		size_t Pinned() const
		{
			size_t n = 0;
			for (const Entry& e : entries_) n += e.pinned ? 1 : 0;
			return n;
		}

		// The slot holding the frame, or kNoSlot.
		int Find(uint64_t generation, uint64_t sequence) const
		{
			for (const Entry& e : entries_)
				if (e.generation == generation && e.sequence == sequence) return e.slot;
			return kNoSlot;
		}

		// A slot for the frame (the caller uploads its luma into it). A frame already held keeps its slot and counts as newest.
		// Otherwise the oldest UNPINNED frame is evicted when `capacity` unpinned frames are held. kNoSlot = no room (cannot
		// happen while pins <= pinCapacity); `evicted` (optional) receives the evicted frame's slot, or kNoSlot.
		int Acquire(uint64_t generation, uint64_t sequence, int* evicted = nullptr)
		{
			if (evicted) *evicted = kNoSlot;
			for (size_t i = 0; i < entries_.size(); ++i)
				if (entries_[i].generation == generation && entries_[i].sequence == sequence)
				{
					const Entry e = entries_[i];
					entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
					entries_.push_back(e);   // newest
					return e.slot;
				}
			size_t unpinned = 0;
			for (const Entry& e : entries_) unpinned += e.pinned ? 0 : 1;
			while (unpinned >= capacity_)
			{
				for (size_t i = 0; i < entries_.size(); ++i)
					if (!entries_[i].pinned)
					{
						if (evicted) *evicted = entries_[i].slot;
						entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
						--unpinned;
						break;
					}
			}
			const int slot = FreeSlot();
			if (slot == kNoSlot) return kNoSlot;
			entries_.push_back(Entry{ generation, sequence, slot, false });
			return slot;
		}

		// Keep the frame until Unpin / Clear (at most pinCapacity at once). False = not held, or the pins are used up.
		bool Pin(uint64_t generation, uint64_t sequence)
		{
			if (Pinned() >= pinCapacity_) return false;
			for (Entry& e : entries_)
				if (e.generation == generation && e.sequence == sequence)
				{
					e.pinned = true;
					return true;
				}
			return false;
		}
		bool Unpin(uint64_t generation, uint64_t sequence)
		{
			for (Entry& e : entries_)
				if (e.generation == generation && e.sequence == sequence && e.pinned)
				{
					e.pinned = false;
					return true;
				}
			return false;
		}

		// A new continuity generation: every frame of another generation goes (pins too: a pin never outlives its generation).
		// Returns how many frames were released.
		size_t ClearGenerationsOtherThan(uint64_t generation)
		{
			size_t removed = 0;
			for (size_t i = 0; i < entries_.size();)
			{
				if (entries_[i].generation != generation) { entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i)); ++removed; }
				else ++i;
			}
			return removed;
		}
		void Clear() { entries_.clear(); }

	private:
		struct Entry { uint64_t generation, sequence; int slot; bool pinned; };

		int FreeSlot() const
		{
			for (size_t s = 0; s < TotalSlots(); ++s)
			{
				bool used = false;
				for (const Entry& e : entries_)
					if (e.slot == static_cast<int>(s)) { used = true; break; }
				if (!used) return static_cast<int>(s);
			}
			return kNoSlot;
		}

		size_t capacity_, pinCapacity_;
		std::vector<Entry> entries_;   // oldest first
	};

	// The walk pair's first frame (105 s3.5). `chained` = the chain says (N-1, N) is a pair (it holds N-1 of this generation);
	// `slot` = the ring's slot of N-1, or kNoSlot. Lost() = chained but the ring no longer holds N-1: the caller counts and
	// names it (Producer::NoteRingMiss with PairStartLostText(); register 115 #13 / #35), never a silent skip.
	struct PairStart
	{
		int slot = EngineLumaRing::kNoSlot;
		bool chained = false;
		bool Lost() const { return chained && slot == EngineLumaRing::kNoSlot; }
	};
	inline PairStart FindPairStart(const EngineLumaRing& ring, bool havePrevious, uint64_t previousGeneration, uint64_t previousSequence,
		uint64_t generation, uint64_t sequence)
	{
		PairStart start;
		start.chained = havePrevious && previousGeneration == generation && previousSequence + 1 == sequence;
		if (start.chained)
			start.slot = ring.Find(previousGeneration, previousSequence);
		return start;
	}
	inline const char* PairStartLostText() { return "the engine-luma ring lost the pair's first frame"; }
}
