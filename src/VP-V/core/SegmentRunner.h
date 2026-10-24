#pragma once

#include "FallbackStatus.h"
#include "OrderValidator.h"
#include "PipelineSlots.h"
#include "SlotContracts.h"
#include "SlotRegistry.h"
#include "SlotTiming.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// DRAFT (2026-10-24, not integrated): the SEGMENT RUNNER core (central-migration-needs.md §6).
//
// Owner principle: VP stages hand directly to the next VP stage inside VP's umbrella, and the
// host (libplacebo) sees ONE entry and ONE exit per contiguous VP segment:
//
//   S2 output colour + drive (PRE_OUTPUT): SDR tone shape (+ DV trims) -> Stage 13 appearance
//      -> LED measurement -> LED compensation (adaptive gamma) -> black level -> [user
//      PRE_OUTPUT shaders: a segment edge] -> calibration LUT (VP)
//   S3 output finish (OUTPUT): encoding repair (libplacebo 7.360 workaround) -> occlusion mask
//      -> dither (sigma-delta) LAST
//
// This header is the pure part (no libplacebo, no Windows, C++14): what a stage needs and
// gives (its contract), the checks run when settings change (never per frame), 1-in-N GPU
// timing sampling that never waits (recorded per slot), and "Fallbacks engaged" reporting.
// The libplacebo side: the generic one-hook runner with its own texture pool
// (src/VideoProcessor-Lib/vprenderer/HookSegment.h, upstreamable, no VP-V dependency) and the
// output stage adapters (src/VP-V/segments/OutputSegments.h).
namespace SegmentRunner
{
	using PipelineSlots::Id;

	// ---- contracts --------------------------------------------------------------------------
	// Intermediate formats a stage accepts / produces (bit set).
	namespace Format { constexpr uint8_t Fp16 = 1, Fp32 = 2, Unorm = 4, Float = Fp16 | Fp32, Any = 7; }
	// Signal range (bit set). Extended = values below 0 / above 1 (BTB/WTW, owner rule
	// vp-btb-wtw-preserve) are carried; Clipped = the stage clamps them.
	namespace Range { constexpr uint8_t Clipped = 1, Extended = 2, Any = 3; }

	struct StageContract
	{
		uint8_t res = PipelineSlots::Res::Output;       // size domain (PipelineSlots::Res bits)
		uint8_t light = PipelineSlots::Light::Target;   // colour domain (PipelineSlots::Light bits)
		uint8_t format = Format::Float;
		uint8_t range = Range::Extended;
		bool vectors = false;                           // needs VALID motion vectors (vp-vector-validity)
	};

	enum class Role : uint8_t
	{
		Vp,        // a VP stage: runs inside the segment
		Edge,      // foreign hooks (user shaders) that split the segment here, in this order
		Quantiser  // the dither: must be the last stage of its segment
	};

	struct StageInfo
	{
		Id slot = Id::Count;
		const char* name = "";          // e.g. "dv_trims" (several stages may share one slot)
		Role role = Role::Vp;
		StageContract needs;            // what it requires of its input
		StageContract gives;            // what it hands on (0 / unchanged fields keep the input's)
		bool modifies = true;           // false = measures only (the LED measurement)
	};

	struct Check
	{
		bool ok = true;
		std::string reason;
	};

	// The domain state handed from stage to stage (for the walk).
	struct Flow
	{
		uint8_t res = PipelineSlots::Res::Output;
		uint8_t light = PipelineSlots::Light::Target;
		uint8_t format = Format::Fp32;
		uint8_t range = Range::Extended;
		bool vectorsValid = false;
	};

	// Order check: the registry's whole order must be valid (OrderValidator), the stages must
	// follow the registry's effective order of their slots (several stages of one slot keep
	// their listed order), the quantiser is last and an edge never sits after it.
	Check CheckOrder(const std::vector<StageInfo>& stages, const PipelineSlots::Registry& registry);

	// Contract walk from the segment's entry state: every stage's needs met by what the stage
	// before it gives. A stage that would clip BTB/WTW ahead of the quantiser is rejected (the
	// owner requires both kept in all cases).
	Check CheckContracts(const std::vector<StageInfo>& stages, const Flow& entry);

	// ---- timing ------------------------------------------------------------------------------
	// 1-in-N sampling of per-stage GPU time. The GPU side attaches a timer only on sampled frames
	// and polls every frame WITHOUT waiting (pl_timer_query returns 0 until the result is
	// there); a ready result is recorded under the stage's slot. Never drains the queue.
	class TimingSampler
	{
	public:
		explicit TimingSampler(uint32_t everyN = 30) : n(everyN ? everyN : 1) {}
		void SetEvery(uint32_t everyN) { n = everyN ? everyN : 1; }
		uint32_t Every() const { return n; }
		// Called once per segment run: true = attach timers this run.
		bool BeginRun() { return (runs++ % n) == 0; }
		uint64_t Runs() const { return runs; }
		// A polled timer result: 0 = not ready yet (ignored, polled again next run).
		void Poll(Id slot, uint64_t nanoseconds)
		{
			if (!nanoseconds)
				return;
			acc.Record(slot, static_cast<double>(nanoseconds) * 1e-6);
		}
		const SlotTiming::Accumulator& Accumulated() const { return acc; }

	private:
		uint32_t n;
		uint64_t runs = 0;
		SlotTiming::Accumulator acc;
	};

	// ---- texture pool ------------------------------------------------------------------------
	// The pool lives with the one-hook runner (vprenderer/HookSegmentPool.h, generic and
	// upstreamable): one pool per merged hook, reused across runs.

	// ---- the segment -------------------------------------------------------------------------
	// Mode: Separate = commit 1 (the segment issues today's hooks, same order, byte-identical);
	// Merged = commit 2 (one libplacebo hook per run of VP stages, internal hand-offs; behind
	// the switch output_segments_merged, default off).
	enum class Mode : uint8_t { Separate, Merged };

	class Segment
	{
	public:
		Segment(std::string fallbackId, std::vector<StageInfo> stageList, Flow entryFlow)
			: id(std::move(fallbackId)), stages(std::move(stageList)), entry(entryFlow) {}

		const std::string& FallbackId() const { return id; }
		const std::vector<StageInfo>& Stages() const { return stages; }

		// Run when settings change (the caller passes a signature of what the checks depend on:
		// registry order, switches, the merged switch); unchanged signature = nothing re-checked.
		// A failed check while Merged is wanted runs the Separate form instead and says so under
		// "Fallbacks engaged"; a failed check in the Separate form is reported too (the hooks
		// still run as today: the separate form is the current behaviour).
		Mode Configure(const PipelineSlots::Registry& registry, bool mergedWanted, uint64_t signature,
			FallbackStatus::Registry& fallbacks)
		{
			if (configured && signature == lastSignature && mergedWanted == lastMergedWanted)
				return mode;
			configured = true;
			lastSignature = signature;
			lastMergedWanted = mergedWanted;
			Check order = CheckOrder(Active(registry), registry);
			Check contracts = order.ok ? CheckContracts(Active(registry), entry) : order;
			last = order.ok ? contracts : order;
			if (!last.ok)
			{
				mode = Mode::Separate;
				fallbacks.Engage(id, std::string(mergedWanted ? "separate hooks: " : "") + last.reason);
				return mode;
			}
			mode = mergedWanted ? Mode::Merged : Mode::Separate;
			fallbacks.Release(id);
			return mode;
		}
		// The GPU side reports a run-time downgrade (a stage failed, the pool could not allocate):
		// the segment drops to the separate form until the next Configure, visibly.
		void Downgrade(const std::string& reason, FallbackStatus::Registry& fallbacks)
		{
			mode = Mode::Separate;
			last.ok = false;
			last.reason = reason;
			fallbacks.Engage(id, "separate hooks: " + reason);
		}
		Mode CurrentMode() const { return mode; }
		const Check& LastCheck() const { return last; }

		// The stages whose slot runs (zero-cost rule); edges and slot-less stages always listed.
		std::vector<StageInfo> Active(const PipelineSlots::Registry& registry) const
		{
			std::vector<StageInfo> out;
			for (const StageInfo& s : stages)
				if (s.role == Role::Edge || s.slot == Id::Count || registry.Runs(s.slot))
					out.push_back(s);
			return out;
		}

		TimingSampler& Timing() { return timing; }
		const TimingSampler& Timing() const { return timing; }

	private:
		std::string id;
		std::vector<StageInfo> stages;
		Flow entry;
		Mode mode = Mode::Separate;
		Check last;
		bool configured = false;
		bool lastMergedWanted = false;
		uint64_t lastSignature = 0;
		TimingSampler timing;
	};

	// The two output segments' stage lists (the designed order; RenderLocked's current hook list).
	std::vector<StageInfo> OutputColourStages();   // S2
	std::vector<StageInfo> OutputFinishStages();   // S3
	Flow OutputColourEntry();   // after libplacebo's colour management: output size, target light
	Flow OutputFinishEntry();   // after the PRE_OUTPUT -> OUTPUT boundary

	// Ids under "Fallbacks engaged".
	constexpr const char* kS2FallbackId = "output segment S2";
	constexpr const char* kS3FallbackId = "output segment S3";
}
