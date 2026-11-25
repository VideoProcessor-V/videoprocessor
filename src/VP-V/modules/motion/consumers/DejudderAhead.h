#pragma once

#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>

#include <array>
#include <cstdint>
#include <functional>

// Owner 2026-10-13 (estimate ahead): the bookkeeping of the de-judder's
// motion estimate moved into an extra's spare refresh. After the first extra
// of A, with C (the frame after B) queued, (B, C) is estimated while A's
// remaining extras read a copy of (A, B)'s field. At B's frame, if the next
// frame is still C, the estimate is already there; anything else is
// estimated before the frame as before. Z1 (N#4): backed by VpMotion::LaneAhead::AheadRing
// as the ONE chain ring arbiter + physical slot tracker (default capacity 1 preserves
// single-pair behaviour; SetCapacity(depth) enables multi-slot ahead scheduling).
namespace Dejudder
{
	class AheadSchedule
	{
	public:
		// Why a frame was (or was not) estimated ahead.
		enum class Reason
		{
			Ahead,             // estimated ahead
			Off,               // dejudder_estimate_ahead off
			SourceWarp,        // dejudder_warp_stage: source (warps the matcher's own planes)
			NotActive,         // the de-judder is not running with extras
			NoNext,            // B not known
			NoAfter,           // C not queued (or not converted) yet: a short queue
			AfterNotNew,       // C is B or A
			NotEstimated,      // the matcher does not hold (A, B)
			Layout,            // C does not upload in B's layout
			SnapshotFailed,
			UploadFailed,
			EstimateFailed,
			AfterGap,          // C is not queued because a gap follows B (inert until the host sets afterGap)
			Count
		};

		struct Conditions
		{
			bool enabled = true;
			bool renderedWarp = true;
			bool active = true;
			uint64_t source = 0;      // A, showing
			uint64_t next = 0;        // B
			uint64_t after = 0;       // C (0 = not available)
			bool afterGap = false;    // C is absent because the frame after next follows a gap
			uint64_t estimated = 0;   // the matcher holds (estimated, its next)
			bool layoutMatches = true;
		};

		struct Steps
		{
			std::function<bool()> snapshot;                              // copy (A, B)'s warp inputs
			std::function<bool()> uploadAfter;                           // C's planes
			std::function<bool(uint64_t a, uint64_t b)> estimate;        // the matcher on (a, b)
		};

		AheadSchedule() : ring_(1) {}

		// After A's first extra. True when the remaining extras must read the
		// copy (the matcher's own outputs may now be (B, C)'s or partly so).
		bool TryAhead(const Conditions& c, const Steps& steps);

		// Z1 multi-slot ring arbiter: picks FreeSlot(), validates conditions + AheadRing chain/capacity/epoch invariants,
		// runs snapshot/upload/estimate, and pushes EntryKey{ c.next, c.after, epochNow } into that physical slot.
		bool TryAheadSlot(const Conditions& c, const Steps& steps, int* outSlot = nullptr);

		// At a frame's estimate: true when (source, next) was estimated ahead
		// (nothing to do but take its planes). False: estimate it now.
		bool Consume(uint64_t source, uint64_t next);

		// E12 + Z1: epoch-checked consume from the front of the chain ring, returning its physical slot in *outSlot (-1 on miss).
		bool Consume(uint64_t source, uint64_t next, uint64_t epoch, int* outSlot = nullptr);

		// Z1 ring arbiter controls and read-only views.
		void SetCapacity(int depth);
		int Capacity() const;
		int Held() const;
		const VpMotion::LaneAhead::AheadRing& Ring() const { return ring_; }
		int FreeSlot() const { return ring_.FreeSlot(); }
		int SlotAt(int i) const;
		const VpMotion::LaneAhead::EntryKey& EntryAt(int i) const;
		void Discard(VpMotion::LaneAhead::DiscardCause cause);
		unsigned long long Discarded(VpMotion::LaneAhead::DiscardCause cause) const { return ring_.Discarded(cause); }

		// E12: the epoch of the estimate the matcher holds now (the render-time estimate's, or a consumed entry's).
		// TryAhead / TryAheadSlot records it with the entry it makes.
		void NoteEpoch(uint64_t epoch) { epochNow = epoch; }
		uint64_t ConsumedEpoch() const { return consumedEpoch; }
		unsigned long long EpochMisses() const { return epochMisses; }

		void Reset();

		Reason LastReason() const { return last; }
		unsigned long long Count(Reason r) const { return counts[static_cast<size_t>(r)]; }
		unsigned long long Hits() const { return hits; }
		unsigned long long Misses() const { return misses; }
		unsigned long long Sequential() const { return sequential; }
		bool Pending() const { return !ring_.Empty() || legacyFrom_ != 0; }

		static const char* Name(Reason r);

	private:
		void Note(Reason r);
		bool CheckConditions(const Conditions& c);

		VpMotion::LaneAhead::AheadRing ring_;
		uint64_t legacyFrom_ = 0, legacyTo_ = 0, legacyEpoch_ = 0;   // depth-1 entry when NoteEpoch==0 or after<=next
		uint64_t epochNow = 0, consumedEpoch = 0;   // E12
		unsigned long long epochMisses = 0;
		Reason last = Reason::Off;
		std::array<unsigned long long, static_cast<size_t>(Reason::Count)> counts{};
		unsigned long long hits = 0, misses = 0, sequential = 0;
	};
}
