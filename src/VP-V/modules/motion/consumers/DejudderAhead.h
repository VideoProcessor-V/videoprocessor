#pragma once

#include <array>
#include <cstdint>
#include <functional>

// Owner 2026-10-13 (estimate ahead): the bookkeeping of the de-judder's
// motion estimate moved into an extra's spare refresh. After the first extra
// of A, with C (the frame after B) queued, (B, C) is estimated while A's
// remaining extras read a copy of (A, B)'s field. At B's frame, if the next
// frame is still C, the estimate is already there; anything else is
// estimated before the frame as before. The renderer supplies the steps; the
// tests drive the same class.
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
			uint64_t estimated = 0;   // the matcher holds (estimated, its next)
			bool layoutMatches = true;
		};

		struct Steps
		{
			std::function<bool()> snapshot;                              // copy (A, B)'s warp inputs
			std::function<bool()> uploadAfter;                           // C's planes
			std::function<bool(uint64_t a, uint64_t b)> estimate;        // the matcher on (a, b)
		};

		// After A's first extra. True when the remaining extras must read the
		// copy (the matcher's own outputs may now be (B, C)'s or partly so).
		bool TryAhead(const Conditions& c, const Steps& steps)
		{
			auto skip = [this](Reason r) { Note(r); return false; };
			if (!c.enabled) return skip(Reason::Off);
			if (!c.renderedWarp) return skip(Reason::SourceWarp);
			if (!c.active) return skip(Reason::NotActive);
			if (c.next == 0) return skip(Reason::NoNext);
			if (c.after == 0) return skip(Reason::NoAfter);
			if (c.after == c.next || c.after == c.source) return skip(Reason::AfterNotNew);
			if (c.estimated != c.source) return skip(Reason::NotEstimated);
			if (!c.layoutMatches) return skip(Reason::Layout);
			if (!steps.snapshot || !steps.snapshot()) return skip(Reason::SnapshotFailed);
			// From here the copy is in use whatever happens.
			if (!steps.uploadAfter || !steps.uploadAfter()) { Note(Reason::UploadFailed); return true; }
			if (!steps.estimate || !steps.estimate(c.next, c.after)) { Note(Reason::EstimateFailed); return true; }
			aheadFor = c.next;
			aheadNext = c.after;
			Note(Reason::Ahead);
			return true;
		}

		// At a frame's estimate: true when (source, next) was estimated ahead
		// (nothing to do but take its planes). False: estimate it now.
		bool Consume(uint64_t source, uint64_t next)
		{
			const bool hit = next != 0 && aheadFor == source && aheadNext == next;
			if (hit)
				++hits;
			else
			{
				if (aheadFor != 0)
					++misses;   // estimated ahead for a pair that did not come
				++sequential;
			}
			aheadFor = aheadNext = 0;
			return hit;
		}

		void Reset()
		{
			aheadFor = aheadNext = 0;
		}

		Reason LastReason() const { return last; }
		unsigned long long Count(Reason r) const { return counts[static_cast<size_t>(r)]; }
		unsigned long long Hits() const { return hits; }
		unsigned long long Misses() const { return misses; }
		unsigned long long Sequential() const { return sequential; }
		bool Pending() const { return aheadFor != 0; }

		static const char* Name(Reason r)
		{
			switch (r)
			{
			case Reason::Ahead: return "estimated ahead";
			case Reason::Off: return "off";
			case Reason::SourceWarp: return "source warp";
			case Reason::NotActive: return "de-judder not running";
			case Reason::NoNext: return "next frame unknown";
			case Reason::NoAfter: return "no frame after next (short queue)";
			case Reason::AfterNotNew: return "frame after next not new";
			case Reason::NotEstimated: return "this pair not estimated";
			case Reason::Layout: return "frame after next in another layout";
			case Reason::SnapshotFailed: return "copy failed";
			case Reason::UploadFailed: return "upload failed";
			case Reason::EstimateFailed: return "estimate failed";
			default: return "?";
			}
		}

	private:
		void Note(Reason r)
		{
			last = r;
			++counts[static_cast<size_t>(r)];
		}

		uint64_t aheadFor = 0, aheadNext = 0;
		Reason last = Reason::Off;
		std::array<unsigned long long, static_cast<size_t>(Reason::Count)> counts{};
		unsigned long long hits = 0, misses = 0, sequential = 0;
	};
}
