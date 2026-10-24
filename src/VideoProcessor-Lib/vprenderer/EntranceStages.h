#pragma once

#include "PipelineSlots.h"

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
// on the CPU; the renderer instantiates it with pl_frame and stage callables that wrap the
// existing modules (SourceRebase, VpDebander, DetailPulse/TextureRestore, later the NetRunner).
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
}
