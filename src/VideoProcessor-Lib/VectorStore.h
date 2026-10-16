#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

// Motion vectors computed ahead of display (docs/VP-nvof-ahead.md; owner
// 2026-10-16: "start working on the frame 4 or so ahead and save the vector
// sets ... obviously always have to default back to the original gpu path").
//
// A producer (the optical-flow engine on the early-upload device) stores the
// field for each source-frame pair (from, to) as frames arrive; consumers (the
// de-judder, detail accumulation) ask for their pair at render time and run the
// matcher on a miss, exactly as today. Thread-safe: one producer thread, one
// consumer thread.
//
// Validity ([[vp-vector-validity]]): every pair carries the continuity
// generation it was computed in; a discontinuity (resync, seek, format change,
// drop) moves the generation and clears the store, and a stale pair is never
// handed out. Pairs that end before the frame being rendered are pruned; the
// store is bounded (oldest first out).
template <typename Payload>
class VectorStore
{
public:
	enum class Source { Engine, Matcher };

	struct Entry
	{
		uint64_t from = 0, to = 0;
		uint64_t generation = 0;
		Source source = Source::Engine;
		Payload payload{};
	};

	struct Stats
	{
		unsigned long long stored = 0, hits = 0, misses = 0, stale = 0, evicted = 0, cleared = 0;
		uint64_t newestTo = 0;   // the furthest frame with a stored pair
	};

	explicit VectorStore(size_t capacity = 16) : capacity((std::max)(size_t{ 1 }, capacity)) {}

	// The current continuity generation; a new one clears the store.
	void SetGeneration(uint64_t value)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (value == generation) return;
		generation = value;
		if (!entries.empty()) ++stats.cleared;
		entries.clear();
		stats.newestTo = 0;
	}
	uint64_t Generation() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return generation;
	}

	// Store a pair computed in `computedGeneration`; refused (false) when the
	// generation has moved on since (a discontinuity while it was computed).
	bool Put(uint64_t from, uint64_t to, uint64_t computedGeneration, Payload payload, Source source = Source::Engine)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (computedGeneration != generation || to <= from)
		{
			++stats.stale;
			return false;
		}
		Entry& e = entries[std::make_pair(from, to)];
		e.from = from;
		e.to = to;
		e.generation = computedGeneration;
		e.source = source;
		e.payload = std::move(payload);
		++stats.stored;
		stats.newestTo = (std::max)(stats.newestTo, to);
		while (entries.size() > capacity)
		{
			entries.erase(entries.begin());   // the oldest pair (smallest from)
			++stats.evicted;
		}
		return true;
	}

	// The pair if it is stored for the current generation (a hit); else false
	// (a miss: the caller runs the matcher).
	bool Get(uint64_t from, uint64_t to, Entry& out)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = entries.find(std::make_pair(from, to));
		if (it == entries.end() || it->second.generation != generation)
		{
			++stats.misses;
			return false;
		}
		out = it->second;
		++stats.hits;
		return true;
	}
	bool Has(uint64_t from, uint64_t to) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = entries.find(std::make_pair(from, to));
		return it != entries.end() && it->second.generation == generation;
	}

	// Drop pairs that end before `oldestNeeded` (the frame being rendered, or
	// the previous one for the causal consumers).
	void Prune(uint64_t oldestNeeded)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = entries.begin(); it != entries.end();)
			it = it->second.to < oldestNeeded ? entries.erase(it) : std::next(it);
	}

	// What the producer should compute next: for the queued (non-repeated)
	// source frames in display order, the consecutive pairs (f-1 -> f) not yet
	// stored, nearest first, up to `ahead` frames past `current`; with `span2`
	// also (f-2 -> f) (detail accumulation v2).
	std::vector<std::pair<uint64_t, uint64_t>> Plan(uint64_t current, const std::vector<uint64_t>& queued, size_t ahead, bool span2) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<std::pair<uint64_t, uint64_t>> plan;
		std::vector<uint64_t> frames;
		frames.push_back(current);
		for (uint64_t q : queued)
			if (q > frames.back() && frames.size() <= ahead) frames.push_back(q);
		for (size_t i = 1; i < frames.size(); ++i)
		{
			const auto one = std::make_pair(frames[i - 1], frames[i]);
			if (!HasLocked(one)) plan.push_back(one);
			if (span2 && i >= 2)
			{
				const auto two = std::make_pair(frames[i - 2], frames[i]);
				if (!HasLocked(two)) plan.push_back(two);
			}
		}
		return plan;
	}

	Stats GetStats() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return stats;
	}
	size_t Size() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return entries.size();
	}

private:
	bool HasLocked(const std::pair<uint64_t, uint64_t>& key) const
	{
		const auto it = entries.find(key);
		return it != entries.end() && it->second.generation == generation;
	}

	mutable std::mutex mutex;
	size_t capacity;
	uint64_t generation = 0;
	std::map<std::pair<uint64_t, uint64_t>, Entry> entries;
	Stats stats;
};
