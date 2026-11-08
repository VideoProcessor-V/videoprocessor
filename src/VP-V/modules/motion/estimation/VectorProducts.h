// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV (doc 83, MV1/MV3): the vector products the motion pass publishes, and the read-only views the consumers use.
//
//   P2  (kP2VectorPair)     the NVOF pair: forward + backward + cost + whole-frame motion on the 4x4 grid of half luma.
//                           Made ONLY by MotionAhead::Producer (the one NvOpticalFlow session per device), which PUBLISHES it into
//                           a P2Slot owned by the vector engine; the consumers read it through P2View. The producer owns no store.
//   P2m (kP2mMatcherPair)   the matcher pair (no NVOF pair for this estimate): published by the de-judder's matcher
//   P3  (kP3AnalysisField)  the verified block field of a pair (P2 or P2m plus the matcher's verification): the same publication
//                           MotionShadowField (VectorEngineState.h) through the MotionSegment, read through MotionSegment::Fetch.
//
// The skeleton lane's slots (core/ProductSlots.h) carry the headers (stamp, ancestry) of these products: SkeletonModule::FeedEngine
// publishes them from IVectorEngine's facts. A GPU texture or a shared CPU flow cannot sit in a lane slot's byte buffer, so the payload
// lives here, keyed by the SAME pair (from, to) and the same validity generation, and a reader asks the view, never the producer.
//
// Header only; C++14 (included by VideoProcessor-Lib code). Thread-safe (the store's own mutex): one publisher, any readers.
#pragma once

#include <VP-V/modules/motion/estimation/VectorStore.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace MotionAhead
{
	struct Flow;   // MotionAheadProducer.h
	using FlowPtr = std::shared_ptr<const Flow>;
}

namespace VpMotion
{
	// The P2 slot: the pairs the producer computed, per (from, to), under the continuity generation (a new generation clears it; a pair
	// computed in an older generation is refused). Bounded (oldest first out). The ONLY VectorStore of the P2 product.
	class P2Slot
	{
	public:
		using Store = VectorStore<MotionAhead::FlowPtr>;
		using Stats = Store::Stats;

		explicit P2Slot(size_t capacity = 16) : store_(capacity) {}
		P2Slot(const P2Slot&) = delete;
		P2Slot& operator=(const P2Slot&) = delete;

		// ---- publisher side (the producer's worker) ----
		uint64_t Generation() const { return store_.Generation(); }
		void SetGeneration(uint64_t value) { store_.SetGeneration(value); }
		// False when the generation moved on while the pair was computed (a discontinuity): nothing is stored.
		bool Publish(uint64_t from, uint64_t to, uint64_t computedGeneration, MotionAhead::FlowPtr flow)
		{
			return store_.Put(from, to, computedGeneration, std::move(flow));
		}
		// Every pair goes (a producer was started: the slot of an older producer must not be read as its pairs).
		void Clear() { store_.Prune(UINT64_MAX); }

		// ---- reader side (through P2View) ----
		// The pair if it is stored for the current generation (a hit), else null (a miss: the consumer takes the matcher path).
		MotionAhead::FlowPtr Find(uint64_t from, uint64_t to)
		{
			Store::Entry entry;
			return store_.Get(from, to, entry) ? entry.payload : nullptr;
		}
		bool Has(uint64_t from, uint64_t to) const { return store_.Has(from, to); }
		// Pairs that end before `oldestNeeded` go (the frame being rendered, or the previous one for the causal consumers).
		void Prune(uint64_t oldestNeeded) { store_.Prune(oldestNeeded); }
		Stats GetStats() const { return store_.GetStats(); }

	private:
		Store store_;
	};

	// The read-only handle of the P2 slot. Cheap to copy; an empty view (no slot) finds nothing.
	class P2View
	{
	public:
		P2View() = default;
		explicit P2View(std::shared_ptr<P2Slot> slot) : slot_(std::move(slot)) {}
		bool Bound() const { return static_cast<bool>(slot_); }
		MotionAhead::FlowPtr Find(uint64_t from, uint64_t to) const { return slot_ ? slot_->Find(from, to) : nullptr; }
		bool Has(uint64_t from, uint64_t to) const { return slot_ && slot_->Has(from, to); }
		// The consumer's prune of what it no longer needs (a retention, not a write of a product).
		void Prune(uint64_t oldestNeeded) const { if (slot_) slot_->Prune(oldestNeeded); }
		P2Slot::Stats GetStats() const { return slot_ ? slot_->GetStats() : P2Slot::Stats(); }

	private:
		std::shared_ptr<P2Slot> slot_;
	};
}
