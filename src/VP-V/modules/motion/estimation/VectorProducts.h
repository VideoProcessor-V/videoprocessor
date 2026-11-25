// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV (doc 83, MV1/MV3): the vector products the motion pass publishes, and the read-only views the consumers use.
//
//   P2  (kP2VectorPair)     the NVOF pair: forward + backward + cost + whole-frame motion on the 4x4 grid of half luma.
//                           Made ONLY by MotionAhead::Producer (the one NvOpticalFlow session per device), which PUBLISHES it into
//                           a P2Slot owned by the vector engine; the consumers read it through P2View. The producer owns no store.
//   P2m (kP2mMatcherPair)   the matcher pair (no NVOF pair for this estimate): it has NO payload of its own here - the matcher's block
//                           searches are the P3 field itself (the segment's Source::Matcher says the pair was the P2m path).
//   P3  (kP3AnalysisField)  the verified block field of a pair (P2 or P2m plus the matcher's verification): published as
//                           MotionShadowField (VectorEngineState.h) through the MotionSegment, read through MotionSegment::Fetch.
//                           Only the LIVE outputs (field, frame trust, sub-field, camera) are published. The Snap* textures
//                           (SnapField / SnapFrameTrust / SnapSubField / SnapCamera: the copy extras and the entrance snapshot read
//                           while the next pair is estimated) are NOT published; MV1 (the renderer-side reader switch) needs a second
//                           publication for them. The Backward publication (low-latency statistics) carries no textures by design.
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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace MotionAhead
{
	struct Flow;   // MotionAheadProducer.h
	using FlowPtr = std::shared_ptr<const Flow>;
}

namespace VpMotion
{
	// Y1b: one published P2 pair as the publish sink sees it (the skeleton's P2 feed claims its lane slot from it).
	struct P2Published
	{
		uint64_t from = 0, to = 0;
		uint64_t generation = 0;       // the continuity generation the pair was computed (and accepted) under
		uint64_t epoch = 0;            // the motion-validity epoch at publish (0 = unknown / not set by the slot)
		bool globalUsable = false;     // whole-frame motion of the pair usable (a FACT, never a publish gate)
	};

	// The P2 slot: the pairs the producer computed, per (from, to), under the continuity generation (a new generation clears it; a pair
	// computed in an older generation is refused). Bounded (oldest first out). The ONLY VectorStore of the P2 product.
	class P2Slot
	{
	public:
		using Store = VectorStore<MotionAhead::FlowPtr>;
		using Stats = Store::Stats;
		// Y1b: what one successful publish tells the (single) observer. epoch / globalUsable are filled by the engine's wrapper of the observer
		// (the slot knows neither); the slot fills from / to / generation.
		using Observer = std::function<void(const P2Published&, const MotionAhead::FlowPtr&)>;

		explicit P2Slot(size_t capacity = 16) : store_(capacity) {}
		P2Slot(const P2Slot&) = delete;
		P2Slot& operator=(const P2Slot&) = delete;

		// ---- publisher side (the producer's worker) ----
		uint64_t Generation() const { return store_.Generation(); }
		void SetGeneration(uint64_t value) { store_.SetGeneration(value); }
		// False when the generation moved on while the pair was computed (a discontinuity): nothing is stored.
		// On success the observer (if any) runs AFTER store_.Put, OUTSIDE the store's mutex, under observerMutex_.
		bool Publish(uint64_t from, uint64_t to, uint64_t computedGeneration, MotionAhead::FlowPtr flow) { MotionAhead::FlowPtr k = flow; if (!store_.Put(from, to, computedGeneration, std::move(flow))) return false; std::lock_guard<std::mutex> c(observerMutex_); if (observer_) { P2Published i; i.from = from; i.to = to; i.generation = computedGeneration; observer_(i, k); } return true; }
		// ONE observer; an empty function clears it and never returns while a call is in flight.
		void SetObserver(Observer observer) { std::lock_guard<std::mutex> c(observerMutex_); observer_ = std::move(observer); }
		// Every pair goes (a producer was started: the slot of an older producer must not be read as its pairs).
		void Clear() { store_.Prune(UINT64_MAX); }

		// ---- reader side (through P2View) ----
		// The pair if it is stored for the current generation (a hit), else null (a miss: the consumer takes the matcher path).
		MotionAhead::FlowPtr Find(uint64_t from, uint64_t to)
		{
			Store::Entry entry;
			return store_.Get(from, to, entry) ? entry.payload : nullptr;
		}
		// C2a-F 2: the same hit, with the generation the pair was stored under (0 when it is a miss).
		MotionAhead::FlowPtr Find(uint64_t from, uint64_t to, uint64_t& generation)
		{
			Store::Entry entry;
			generation = 0;
			if (!store_.Get(from, to, entry))
				return nullptr;
			generation = entry.generation;
			return entry.payload;
		}
		// The pair that ends at `to` (newest `from`, current generation): its `from` and generation come back; null on a miss.
		MotionAhead::FlowPtr FindTo(uint64_t to, uint64_t& from, uint64_t& generation)
		{
			Store::Entry entry;
			from = 0;
			generation = 0;
			if (!store_.GetTo(to, entry))
				return nullptr;
			from = entry.from;
			generation = entry.generation;
			return entry.payload;
		}
		bool Has(uint64_t from, uint64_t to) const { return store_.Has(from, to); }
		// Pairs that end before `oldestNeeded` go (the frame being rendered, or the previous one for the causal consumers).
		void Prune(uint64_t oldestNeeded) { store_.Prune(oldestNeeded); }
		Stats GetStats() const { return store_.GetStats(); }

	private:
		Store store_;
		std::mutex observerMutex_;
		Observer observer_;
	};

	// SK-MV: when "no pair ahead" becomes a visible fallback. Not a bare frame count: a run must outlast the look-ahead depth at the
	// measured rate (the start-up fill and a refill take about that many estimates) - threshold = max(kFloor, depth + kMargin) - and
	// a sustained PARTIAL miss ratio shows too (a window of kWindowFactor thresholds with at least half missed), so 7 misses in 8
	// cannot stay hidden under a run counter that keeps resetting. One note per optical-flow estimate. Render thread.
	struct NoPairTracker
	{
		static constexpr unsigned kFloor = 8, kMargin = 4, kWindowFactor = 4;
		unsigned run = 0;                          // consecutive misses
		unsigned windowSeen = 0, windowMisses = 0; // the window in progress
		unsigned lastWindowSeen = 0, lastWindowMisses = 0;   // the last full window
		bool ratioSustained = false;               // the last full window had at least half missed

		static unsigned Threshold(int depthFrames)
		{
			const unsigned depth = depthFrames > 0 ? static_cast<unsigned>(depthFrames) : 0u;
			return depth + kMargin > kFloor ? depth + kMargin : kFloor;
		}
		void Note(bool miss, int depthFrames)
		{
			if (miss)
			{
				if (run < 1000000u) ++run;
				++windowMisses;
			}
			else
				run = 0;
			if (++windowSeen >= Threshold(depthFrames) * kWindowFactor)
			{
				lastWindowSeen = windowSeen;
				lastWindowMisses = windowMisses;
				ratioSustained = windowMisses * 2 >= windowSeen;
				windowSeen = windowMisses = 0;
			}
		}
		bool Sustained(int depthFrames) const { return run >= Threshold(depthFrames) || ratioSustained; }
		// "12 estimates in a row" or "30 of the last 32 estimates" for the status text; empty when not sustained.
		std::string Describe(int depthFrames) const
		{
			if (run >= Threshold(depthFrames))
				return std::to_string(run) + " estimates in a row";
			if (ratioSustained)
				return std::to_string(lastWindowMisses) + " of the last " + std::to_string(lastWindowSeen) + " estimates";
			return std::string();
		}
		void Reset() { *this = NoPairTracker(); }
	};

	// The read-only handle of the P2 slot. Cheap to copy; an empty view (no slot) finds nothing.
	class P2View
	{
	public:
		P2View() = default;
		explicit P2View(std::shared_ptr<P2Slot> slot) : slot_(std::move(slot)) {}
		bool Bound() const { return static_cast<bool>(slot_); }
		MotionAhead::FlowPtr Find(uint64_t from, uint64_t to) const { return slot_ ? slot_->Find(from, to) : nullptr; }
		MotionAhead::FlowPtr Find(uint64_t from, uint64_t to, uint64_t& generation) const
		{
			generation = 0;
			return slot_ ? slot_->Find(from, to, generation) : nullptr;
		}
		MotionAhead::FlowPtr FindTo(uint64_t to, uint64_t& from, uint64_t& generation) const
		{
			from = 0;
			generation = 0;
			return slot_ ? slot_->FindTo(to, from, generation) : nullptr;
		}
		// The slot's current continuity generation (0 when unbound): the decline text of the P2 CPU form names it.
		uint64_t Generation() const { return slot_ ? slot_->Generation() : 0; }
		bool Has(uint64_t from, uint64_t to) const { return slot_ && slot_->Has(from, to); }
		// The consumer's prune of what it no longer needs (a retention, not a write of a product).
		void Prune(uint64_t oldestNeeded) const { if (slot_) slot_->Prune(oldestNeeded); }
		P2Slot::Stats GetStats() const { return slot_ ? slot_->GetStats() : P2Slot::Stats(); }

	private:
		std::shared_ptr<P2Slot> slot_;
	};
}
