#pragma once

#include "SlotRegistry.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// The input side as ONE stage list, run ONCE per source frame (barrier scan B3, 2026-10-24).
//
// Why: the renderer carried three copies of the front-end chain (main draw, de-judder extras,
// the source-resolution crossfade) and re-ran re-base + deband for every refresh of a source
// frame, plus once more for the hold. A net placed in that chain would run ~2.5x per source
// frame at 24p in 60. The design (VP-pipeline-final.md 3.6) is instead:
//
//   re-base -> repair -> deband -> texture restore -> 4K route -> refine -> chroma
//   run once per source sequence into the entrance cache; the motion consumers warp or read the
//   cached result (vectors delivered per axis), never re-run the front end.
//
// StageList is generic over the frame type so the ordering, skip and fallback rules are tested
// on the CPU (PipelineSkeletonTests). The renderer follows exactly the same rule - walk
// Registry::ActiveEntrance() and call each slot's stage - in LibplaceboVideoRenderer's
// RunEntrance (a switch over the slot ids, because each call site there keeps its own module
// slots), and keeps its per-source-frame result in Cache<pl_frame>.
//
// Zero-cost rule (PipelineSlots::Registry::Runs): a slot that is off or has no component is not
// called at all, and the frame descriptor passes through unchanged (no pass, no texture copy).
namespace EntranceStages
{
	using PipelineSlots::Id;

	template <class Frame>
	class StageList
	{
	public:
		// A stage reads `in` and writes `out`; false = it did not apply this frame (its own
		// fallback is reported through "Fallbacks engaged" by the module), and the picture
		// continues unchanged.
		using Stage = std::function<bool(const Frame& in, Frame& out)>;

		void Set(Id id, Stage stage) { stages[static_cast<size_t>(id)] = std::move(stage); }
		void Clear(Id id) { stages[static_cast<size_t>(id)] = nullptr; }
		bool Has(Id id) const { return static_cast<bool>(stages[static_cast<size_t>(id)]); }

		struct Result
		{
			int applied = 0;              // stages that produced a new picture
			std::vector<Id> ran;          // stages called, in order
			std::vector<Id> declined;     // stages called that returned false
		};

		// Runs the registry's active entrance stages in effective order. Slots with no callable
		// are skipped like placeholders, so a missing wire-up never costs a pass.
		Result Run(const PipelineSlots::Registry& registry, const Frame& in, Frame& out) const
		{
			Result result;
			out = in;
			for (Id id : registry.ActiveEntrance())
			{
				const Stage& stage = stages[static_cast<size_t>(id)];
				if (!stage)
					continue;
				result.ran.push_back(id);
				Frame next = out;
				if (stage(out, next))
				{
					out = next;
					++result.applied;
				}
				else
					result.declined.push_back(id);
			}
			return result;
		}

	private:
		std::array<Stage, static_cast<size_t>(Id::Count)> stages{};
	};

	// The entrance result of one source frame, keyed by its sequence and by an epoch the
	// renderer bumps on anything that changes the stages' output (settings, re-base factors,
	// processing size, format). A refresh of the same source frame (cadence repeat, de-judder
	// extra, crossfade) reuses it instead of re-running the stages.
	template <class Frame>
	class Cache
	{
	public:
		const Frame* Find(uint64_t sequence, uint64_t epoch) const
		{
			return valid && sequence != 0 && sequence == cachedSequence && epoch == cachedEpoch ? &frame : nullptr;
		}

		void Store(uint64_t sequence, uint64_t epoch, const Frame& value)
		{
			cachedSequence = sequence;
			cachedEpoch = epoch;
			frame = value;
			valid = true;
		}

		void Invalidate() { valid = false; }

		// Runs the stage list for a new (sequence, epoch) or returns the cached picture.
		// `computed` reports which happened (for tests and the once-per-source-frame counter).
		const Frame& Get(const StageList<Frame>& list, const PipelineSlots::Registry& registry,
			uint64_t sequence, uint64_t epoch, const Frame& source, bool* computed = nullptr)
		{
			if (const Frame* hit = Find(sequence, epoch))
			{
				if (computed) *computed = false;
				return *hit;
			}
			Frame result{};
			list.Run(registry, source, result);
			Store(sequence, epoch, result);
			if (computed) *computed = true;
			return frame;
		}

	private:
		Frame frame{};
		uint64_t cachedSequence = 0;
		uint64_t cachedEpoch = 0;
		bool valid = false;
	};

	// B3 (2026-10-24): the processed pictures the motion consumers read, OWNED by the ring (their
	// own copies, so a module writing its output texture again - for another picture - never
	// changes them). Keyed by source sequence, an epoch (the stages' parameters, without the
	// module-texture generation the plain Cache needs) and a role: A (the source frame drawn as
	// itself, the stateful stages advanced on it) or B (the next frame processed AHEAD, per
	// picture, never advancing any stage's state). N entries, least recently used replaced.
	enum class Role { A = 0, B = 1 };

	template <class Payload, size_t N = 3>
	class Ring
	{
	public:
		Payload* Find(uint64_t sequence, uint64_t epoch, Role role)
		{
			for (Entry& e : entries)
				if (e.valid && sequence != 0 && e.sequence == sequence && e.epoch == epoch && e.role == role)
				{
					e.used = ++clock;
					return &e.payload;
				}
			return nullptr;
		}

		// The entry to (re)fill for this key: the same key, else an invalid one, else the least
		// recently used. Its payload is kept (the caller reuses its textures). Valid only after Commit.
		Payload& Claim(uint64_t sequence, uint64_t epoch, Role role)
		{
			Entry* pick = nullptr;
			for (Entry& e : entries)
				if (e.valid && e.sequence == sequence && e.role == role) { pick = &e; break; }
			if (!pick)
				for (Entry& e : entries)
					if (!e.valid) { pick = &e; break; }
			if (!pick)
			{
				pick = &entries[0];
				for (Entry& e : entries)
					if (e.used < pick->used) pick = &e;
			}
			pick->valid = false;
			pick->sequence = sequence;
			pick->epoch = epoch;
			pick->role = role;
			pick->used = ++clock;
			claimed = pick;
			return pick->payload;
		}

		// The last Claim's payload holds the picture now.
		void Commit() { if (claimed) claimed->valid = true; claimed = nullptr; }

		void Invalidate()
		{
			for (Entry& e : entries) e.valid = false;
			claimed = nullptr;
		}

		template <class Fn>
		void ForEachPayload(Fn fn) { for (Entry& e : entries) fn(e.payload); }

		size_t ValidCount() const
		{
			size_t n = 0;
			for (const Entry& e : entries) n += e.valid ? 1 : 0;
			return n;
		}

	private:
		struct Entry
		{
			uint64_t sequence = 0, epoch = 0, used = 0;
			Role role = Role::A;
			bool valid = false;
			Payload payload{};
		};
		std::array<Entry, N> entries{};
		Entry* claimed = nullptr;
		uint64_t clock = 0;
	};
}
