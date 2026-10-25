#pragma once

#include <VP-V/core/EntranceStages.h>
#include <VP-V/core/SegmentRunner.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/repair/deband/VpDebander.h>
#include <VP-V/modules/repair/texture_restore/DetailPulse.h>
#include <VP-V/modules/repair/texture_restore/TextureRestore.h>
#include <VP-V/modules/scaling/rebase/SourceRebase.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>
#include <vector>

// S1, the ENTRANCE SEGMENT (central-migration-needs.md §6.1 S1; integration-batch-plan.md WP-8;
// M2 steps 10-13). The input side at source size, once per source frame:
//
//   re-base -> [repair A: placeholder until the net runner is wired] -> deband
//      -> texture restore (+ detail pulse) -> [4K route] -> [refine] -> [chroma]   ([..] = placeholder)
//
// One object owns what the renderer host used to carry for it (about 900 lines of
// LibplaceboVideoRenderer.cpp): the stage walk in the slot registry's order, the per-path rules
// (main, extra, crossfade's path being left, B as its own A), the per-source-frame cache, the B3
// ring of processed pictures (pipeline_entrance_once, ADR-0024), the counters and the "entrance
// once" fallback. VP-owned textures (each module's output planes) pass straight from stage to
// stage; the host sees ONE call per picture: Run(request, frame) (plus ProcessedA/ProcessedB for
// the motion consumers, which read the ring).
//
// COMMIT 1 (this file): a behaviour-free extraction. The stages, their order, slots, formats and
// every decision are the host's code moved verbatim; the output is byte-identical by
// construction. The modules and their per-frame parameters stay OWNED by the host for now
// (Bindings points at them), because the host's settings, status and source-resolution code
// read and write them in ~150 other places; moving that ownership is a separate change.
//
// Still in the host (hand-backs by design, S0/motion, WP-9): the motion estimate ahead
// (FieldAhead), the field B takes (FieldOf), the next frame's upload (NextUpload), and the
// motion consumers that interpolate/warp the processed A and B (EntranceOncePhasePicture,
// EntranceOnceExtra). They reach the segment through Host and ProcessedA/ProcessedB.
namespace EntranceSegment
{
	// Which call site a picture comes from (each keeps its own module slots and rules).
	enum class Path { Main, Extra, CrossfadeFrom };

	// The per-picture state the stages share while one picture runs (the host's former EntranceRun).
	struct StageRun
	{
		Path path = Path::Main;
		double factorX = 1.0, factorY = 1.0;   // the re-base factors this picture takes
		uint64_t sequence = 0;                  // its source frame
		// Main only: A's own planes (the debander's hold and the detail history advance once per
		// source frame on A, processed by the stages before them), this draw's situation, and
		// the picture the look-ahead reads.
		const pl_frame* source = nullptr;
		bool phasePicture = false;
		bool cadenceRepeat = false;
		uint64_t nextSequence = 0;
		const pl_frame* image = nullptr;
		bool aNeeded = false;      // A goes through the stages alongside (hold / history)
		bool aReady = true;
		pl_frame aFrame{};
		bool ready = true;         // CrossfadeFrom: false when its re-base failed (no fade)
	};

	// B3: a processed picture as the ring's OWN copy.
	struct OwnedEntrance
	{
		pl_tex tex[4] = {};
		pl_frame frame{};
		bool identity = false;   // no stage changed the picture: the raw planes are the processed ones
		pl_rect2df rawCrop{};    // the raw frame's crop and luma size it was made from (audit MED 4)
		int rawW = 0, rawH = 0;
	};

	// Audit MED 3/4: a ring picture as drawn for THIS frame - the owned planes, but the caller's own
	// descriptor (crop, colour incl. per-frame HDR metadata); the crop mapped to the processed size
	// like the stages map it. An identity entry is the caller's raw frame itself.
	pl_frame OwnedFrame(const OwnedEntrance& e, const pl_frame& raw);

	// What the host hands back to the segment (implemented by a small adapter in the host).
	class Host
	{
	public:
		virtual ~Host() = default;
		virtual pl_gpu Gpu() const = 0;
		// The host's entrance dispatch (created on first use; the host also uses it elsewhere).
		virtual pl_dispatch Dispatch() = 0;
		virtual void EngageFallback(const std::string& id, const std::string& text) = 0;
		virtual void ReleaseFallback(const std::string& id) = 0;
		// S0 hand-backs (B3, stateful stages only): B's own vector field estimated ahead; false and
		// `why` when it is not there.
		virtual bool FieldAhead(uint64_t aSequence, uint64_t bSequence, std::string& why) = 0;
		// The live field when it is B's and current (both null otherwise).
		struct Field { pl_tex field = nullptr; pl_tex grainError = nullptr; };
		virtual Field FieldOf(uint64_t sequence) const = 0;
		// The next source frame's planes when the de-judder uploaded them (the detail look-ahead).
		struct NextUpload { uint64_t sequence = 0; pl_tex luma = nullptr, chroma = nullptr; bool rgbLuma = false; };
		virtual NextUpload Next() const = 0;
	};

	// The host-owned modules and per-frame parameters the stages read (commit 1: not moved).
	struct Bindings
	{
		SourceRebase::Module* sourceRebase = nullptr;
		VpDebander::Module* debander = nullptr;
		DetailPulse::Module* detailPulse = nullptr;
		TextureRestore::Module* textureRestore = nullptr;
		const PipelineSlots::Registry* slots = nullptr;
		const bool* entranceOnceSetting = nullptr;    // pipeline_entrance_once
		const double* rebaseFactor = nullptr;
		const double* rebaseFactorX = nullptr;
		const double* rebaseFromFactor = nullptr;     // the crossfade's path being left
		const double* rebaseFromFactorX = nullptr;
		const double* debandThreshold = nullptr;      // 0 = VP deband off on this frame
		VpDebander::Params* vpDebandParams = nullptr;
		DetailPulse::Params* detailPulseParams = nullptr;
		TextureRestore::Params* textureRestoreParams = nullptr;
		const bool* detailPulseRuns = nullptr;
		const bool* textureRestoreRuns = nullptr;
		std::string* detailPulseInactive = nullptr;   // the status's "inactive (...)" reasons
		std::string* textureRestoreInactive = nullptr;
		uint64_t* detailPulseAheadSequence = nullptr;
	};

	// The ONE call per picture.
	struct Request
	{
		Path path = Path::Main;
		uint64_t sequence = 0;          // the picture's source frame
		// Main:
		const pl_frame* source = nullptr;   // A's own planes
		const pl_frame* image = nullptr;    // the picture the look-ahead reads
		uint64_t nextSequence = 0;
		bool phasePicture = false;
		bool cadenceRepeat = false;
		// Main, B already processed as its own A (audit HIGH 1): the per-picture stages (the extra's
		// path), never advancing any stage's state.
		bool perPicture = false;
	};

	// The S1 stage list for the segment runner's order/contract checks, in the registry's CURRENT
	// order (validated by OrderValidator when it was set; nothing here hard-codes an order), with each
	// slot's contract from SlotContracts (source size, coded planes, BTB/WTW carried). Every entrance
	// slot is listed, placeholders included (repair A, 4K route, refine, chroma).
	std::vector<SegmentRunner::StageInfo> EntranceStageInfo(const PipelineSlots::Registry& registry);
	SegmentRunner::Flow EntranceEntry();
	constexpr const char* kS1FallbackId = "entrance segment S1";

	// The stage a slot runs inside S1 (None: a slot outside S1).
	enum class Stage : uint8_t { None, Rebase, RepairA, Deband, Detail, Route4k, Refine, Chroma };
	Stage StageFor(PipelineSlots::Id id);
	// Whether the stage has a component wired in commit 1 (repair A, 4K route, refine, chroma: not yet).
	bool Wired(Stage stage);
	// The stages the walk calls for this registry, in order: the registry's active entrance slots
	// (enabled, with a component, Original pathway and pipeline_slots_off applied) in its validated
	// order. A switched-off slot is absent: no pass, the picture passes unchanged (zero cost).
	std::vector<Stage> Plan(const PipelineSlots::Registry& registry);
	// A placeholder stage (repair A until the net runner is wired, 4K route, refine, chroma): reached
	// only if a component registered its slot without a stage; passes the picture through unchanged
	// (no pass, no texture) and reports that it did not apply.
	inline bool Placeholder(const pl_frame& in, pl_frame& out) { out = in; return false; }

	class Segment
	{
	public:
		Segment(Host& host, const Bindings& bindings) : host(host), b(bindings) {}

		// ---- the one call ----
		// Main: the cached entrance result for a picture drawn as itself, else the stages (A in the
		// ring when B3 is on). Extra: the per-picture stages of a de-judder extra. CrossfadeFrom:
		// the path being left at the factors faded from; false = its re-base failed (no fade).
		bool Run(const Request& request, pl_frame& frame);

		// ---- B3 (the motion consumers, still in the host, read the ring) ----
		bool Once() const { return *b.entranceOnceSetting && !b.slots->OriginalPathway(); }
		const OwnedEntrance* ProcessedA(const pl_frame& rawA, uint64_t sequence, uint64_t nextSequence, const pl_frame* image);
		const OwnedEntrance* ProcessedB(const pl_frame& rawB, uint64_t nextSequence, uint64_t aSequence);
		void SetCurrentA(uint64_t sequence) { entranceOnceCurrentA = sequence; }
		uint64_t BAsAFor() const { return entranceOnceBAsAFor; }
		void OnceFallback(const std::string& why);
		void OnceWorking();
		bool ForcedFallback();   // test only: VP_TEST_ENTRANCE_ONCE_FALLBACK_EVERY
		void OnceSwitchedOff();  // settings: B3 off (the note cleared; the textures go on the render thread)

		// ---- lifetime ----
		void NoteSourceFrame(uint64_t sequence);
		void Invalidate();                 // settings applied (slots, order)
		void ReleaseDevice(pl_gpu gpu);    // the device goes: cache, ring, generation
		void LogCounters();

	private:
		using Role = EntranceStages::Role;
		void RunStages(StageRun& run, pl_frame& frame);
		void RunMain(StageRun& run, pl_frame& frame);
		bool StatelessNow() const;
		uint64_t EpochFor(const pl_frame& source, bool withGeneration = true) const;
		uint64_t RingKey(const pl_frame& raw) const;
		void NoteStatefulRun(uint64_t sequence);
		bool CopyEntrance(const pl_frame& in, OwnedEntrance& out);
		bool StoreOwned(uint64_t sequence, uint64_t key, Role role, const pl_frame& raw, const pl_frame& result);
		void ReleaseRing(pl_gpu gpu);
		const OwnedEntrance* ProcessBAsA(const pl_frame& rawB, uint64_t bSequence, uint64_t key);
		void Rebase(StageRun& run, pl_frame& frame);
		void Deband(StageRun& run, pl_frame& frame);
		void Detail(StageRun& run, pl_frame& frame);
		void DetailLookAhead(const StageRun& run);

		// Bound host state (commit 1).
		double rebaseFactor() const { return *b.rebaseFactor; }
		double rebaseFactorX() const { return *b.rebaseFactorX; }
		double debandThreshold() const { return *b.debandThreshold; }
		bool detailPulseRuns() const { return *b.detailPulseRuns; }
		bool textureRestoreRuns() const { return *b.textureRestoreRuns; }
		const PipelineSlots::Registry& slots() const { return *b.slots; }

		Host& host;
		Bindings b;

		EntranceStages::Cache<pl_frame> entranceCache;
		uint64_t entranceGeneration = 1;
		unsigned long long entranceRuns = 0, entranceReuses = 0;
		EntranceStages::Ring<OwnedEntrance, 3> entranceRing;
		unsigned long long entranceOnceA = 0, entranceOnceB = 0, entranceOnceReused = 0, entranceOncePictures = 0,
			entranceOnceFallbacks = 0;
		std::string entranceOnceNote;   // the fallback shown under "Fallbacks engaged" (empty: none)
		size_t entranceRingBytesLogged = 0;
		unsigned long long entranceStageRuns = 0;     // every call (any path): the input side's cost unit
		unsigned long long entranceSourceFrames = 0;  // distinct source frames drawn
		unsigned long long entranceCostLoggedAt = 0;
		uint64_t entranceLastSourceSequence = 0;
		uint64_t entranceOnceCurrentA = 0;     // the source frame whose pictures are being made
		uint64_t entranceOnceFallbackFor = 0;  // the source frame that fell back (sticky until the next clean one)
		uint64_t entranceOnceBAsAFor = 0;      // A whose B was processed as B's A
		unsigned long long entranceStatefulBreaks = 0;
		uint64_t entranceStatefulLast = 0;
		unsigned entranceOnceForcedEvery = 0, entranceOnceForcedCount = 0;
		bool entranceOnceForcedRead = false;
		std::string rebaseFormatNoteLogged;
		std::string detailPulseFailLogged, textureRestoreFailLogged;   // each reason logged once
	};
}
