#include <pch.h>
#include "EntranceSegment.h"

#include <VP-V/core/EntranceEpoch.h>

#include <VP-V/modules/control/diagnostics/DiagnosticsAdapter.h>
#include <DebugLog.h>
#include <GeneralRateLog.h>

#include <libplacebo/shaders/sampling.h>

#include <Windows.h>

#include <algorithm>
#include <cstdlib>

// Commit 1 of S1 (see EntranceSegment.h): the renderer host's entrance code moved here verbatim.
// Names are kept (RunEntrance -> RunStages, RunMainEntrance -> RunMain, EntranceRebase -> Rebase,
// ...) so a reviewer can compare it line by line with the host before the extraction
// (feat/m2-entrance base 2f75d36d, LibplaceboVideoRenderer.cpp ~5597-6575). `host.Gpu()` stands
// for d3d11->gpu and `host.Dispatch()` for EnsureRebaseDispatch() + rebaseDispatch.
namespace EntranceSegment
{
	using PipelineSlots::Id;

	pl_frame OwnedFrame(const OwnedEntrance& e, const pl_frame& raw)
	{
		if (e.identity || e.rawW <= 0 || e.rawH <= 0 || !e.frame.planes[0].texture)
			return raw;
		pl_frame out = e.frame;
		out.color = raw.color;
		const double sx = static_cast<double>(e.frame.planes[0].texture->params.w) / e.rawW;
		const double sy = static_cast<double>(e.frame.planes[0].texture->params.h) / e.rawH;
		out.crop.x0 = static_cast<float>(e.frame.crop.x0 + (raw.crop.x0 - e.rawCrop.x0) * sx);
		out.crop.x1 = static_cast<float>(e.frame.crop.x1 + (raw.crop.x1 - e.rawCrop.x1) * sx);
		out.crop.y0 = static_cast<float>(e.frame.crop.y0 + (raw.crop.y0 - e.rawCrop.y0) * sy);
		out.crop.y1 = static_cast<float>(e.frame.crop.y1 + (raw.crop.y1 - e.rawCrop.y1) * sy);
		return out;
	}

	Stage StageFor(Id id)
	{
		switch (id)
		{
		case Id::ScalingRebase: return Stage::Rebase;
		case Id::RepairEncoding: return Stage::RepairA;
		case Id::RepairDeband: return Stage::Deband;
		case Id::RepairTextureRestore: return Stage::Detail;
		case Id::ScalingRoute4k: return Stage::Route4k;
		case Id::ScalingRefine: return Stage::Refine;
		case Id::ScalingChromaUpscale: return Stage::Chroma;
		default: return Stage::None;
		}
	}

	bool Wired(Stage stage)
	{
		return stage == Stage::Rebase || stage == Stage::Deband || stage == Stage::Detail;
	}

	std::vector<Stage> Plan(const PipelineSlots::Registry& registry)
	{
		std::vector<Stage> plan;
		for (Id id : registry.ActiveEntrance())
		{
			const Stage stage = StageFor(id);
			if (stage != Stage::None)
				plan.push_back(stage);
		}
		return plan;
	}

	std::vector<SegmentRunner::StageInfo> EntranceStageInfo(const PipelineSlots::Registry& registry)
	{
		using SegmentRunner::StageInfo;
		std::vector<StageInfo> s;
		for (Id id : registry.CurrentOrder())
		{
			if (!PipelineSlots::IsEntranceStage(id))
				continue;
			const PipelineSlots::Contract c = PipelineSlots::ContractOf(id);
			StageInfo info;
			info.slot = id;
			info.name = "";
			info.role = SegmentRunner::Role::Vp;
			info.needs.res = c.needRes;
			info.needs.light = PipelineSlots::Light::Codes;   // S1 works on the coded source planes
			info.needs.format = SegmentRunner::Format::Any;
			info.needs.range = SegmentRunner::Range::Any;
			info.needs.vectors = false;     // every S1 stage uses vectors when valid, else runs without
			info.gives = SegmentRunner::StageContract{ c.giveRes, 0, 0, 0, false };   // 0 = unchanged
			info.modifies = c.modifies;
			s.push_back(info);
		}
		return s;
	}

	SegmentRunner::Flow EntranceEntry()
	{
		SegmentRunner::Flow f;
		f.res = PipelineSlots::Res::Delivered;
		f.light = PipelineSlots::Light::Codes;          // the source planes as coded (YCbCr / RGB codes)
		f.format = SegmentRunner::Format::Any;         // capture planes: unorm; re-based: r16 or fp16
		f.range = SegmentRunner::Range::Extended;       // BTB/WTW carried (owner rule)
		f.vectorsValid = false;                         // optional for every S1 stage
		return f;
	}

	// ---- the one call ----------------------------------------------------------------------

	bool Segment::Run(const Request& request, pl_frame& frame)
	{
		StageRun run;
		run.sequence = request.sequence;
		if (request.path == Path::CrossfadeFrom)
		{
			// Hunt 3 (crossfade lead): the crossfade's path being left carries the stages the path
			// being entered has, from its own (un-re-based) picture at the factors faded from.
			run.path = Path::CrossfadeFrom;
			run.factorX = *b.rebaseFromFactorX;
			run.factorY = *b.rebaseFromFactor;
			RunStages(run, frame);
			return run.ready;
		}
		run.factorX = rebaseFactorX();
		run.factorY = rebaseFactor();
		if (request.path == Path::Extra)
		{
			// Pipeline skeleton (B3/R2): a de-judder extra runs the same entrance stage list as the
			// main frame (re-base, VP deband per frame, the detail stages' decision of its source
			// frame), with the extra's own module slots.
			run.path = Path::Extra;
			RunStages(run, frame);
			LogCounters();
			return run.ready;
		}
		if (request.perPicture)
		{
			// The per-picture stages, never advancing (the extra's path; the detail stages keep B's
			// history - they skip this picture rather than restart).
			run.path = Path::Extra;
			RunStages(run, frame);
			return run.ready;
		}
		run.path = Path::Main;
		run.source = request.source;
		run.phasePicture = request.phasePicture;
		run.cadenceRepeat = request.cadenceRepeat;
		run.nextSequence = request.nextSequence;
		run.image = request.image;
		run.aFrame = *request.source;
		// The same decision as before; the slot switches are tested first, so a switched-off slot's
		// module is not even asked (pure getters: the result is unchanged).
		auto historyNeeded = [&]()
		{
			return (detailPulseRuns() && b.detailPulse->HistorySequence() != request.sequence) ||
				(textureRestoreRuns() && b.textureRestore->HistorySequence() != request.sequence);
		};
		run.aNeeded = request.phasePicture && !request.cadenceRepeat &&
			((debandThreshold() > 0.0 && slots().Runs(Id::RepairDeband)) ||
			 (slots().Runs(Id::RepairTextureRestore) && historyNeeded()));
		RunMain(run, frame);
		return run.ready;
	}

	// ---- the stage walk ----------------------------------------------------------------------

	// Everything a cached entrance result depends on (VP-V/core/EntranceEpoch.h; audit blocker 1):
	// the frame itself (planes, crop, representation, colour), every stage's parameters, the
	// detail stages' vector fields and bitrate quality, and the generation moved by any other use
	// of the modules' output textures.
	// withGeneration false: the B3 ring's key (its pictures are owned copies, so a module writing
	// its textures again does not change them).
	uint64_t Segment::EpochFor(const pl_frame& source, bool withGeneration) const
	{
		const VpDebander::Params& vpDebandParams = *b.vpDebandParams;
		const DetailPulse::Params& detailPulseParams = *b.detailPulseParams;
		const TextureRestore::Params& textureRestoreParams = *b.textureRestoreParams;
		EntranceEpoch::Key key;
		key.Add(withGeneration ? entranceGeneration : 0);
		// The ring (withGeneration false) keys by the unique source sequence and content, not by the
		// texture the frame was uploaded into (its pictures are owned copies).
		EntranceEpoch::AddFrame(key, source, withGeneration);
		key.Add(rebaseFactor()).Add(rebaseFactorX());
		key.Add(debandThreshold()).Add(vpDebandParams.thresholdCodes).Add(vpDebandParams.radiusScale);
		key.Add(static_cast<const void*>(vpDebandParams.field)).Add(static_cast<const void*>(vpDebandParams.grainError));
		key.Add(vpDebandParams.fieldScale).Add(vpDebandParams.fieldScaleX).Add(vpDebandParams.sequence);
		key.Add((detailPulseRuns() ? 1 : 0) | (textureRestoreRuns() ? 2 : 0));
		key.Add(detailPulseParams.sequence).Add(static_cast<const void*>(detailPulseParams.field)).Add(detailPulseParams.bitrateQuality)
			.Add(detailPulseParams.fieldScale).Add(detailPulseParams.fieldScaleX).Add(detailPulseParams.planeLines).Add(detailPulseParams.cap);
		key.Add(textureRestoreParams.sequence).Add(static_cast<const void*>(textureRestoreParams.field)).Add(textureRestoreParams.bitrateQuality)
			.Add(textureRestoreParams.fieldScale).Add(textureRestoreParams.fieldScaleX).Add(textureRestoreParams.planeLines).Add(textureRestoreParams.cap);
		// The registry's entrance (slot switches, order) changes the result as well.
		for (Id id : slots().ActiveEntrance())
			key.Add(static_cast<int>(id));
		return key.Value();
	}

	void Segment::NoteSourceFrame(uint64_t sequence)
	{
		if (sequence != 0 && sequence != entranceLastSourceSequence)
		{
			entranceLastSourceSequence = sequence;
			++entranceSourceFrames;
		}
	}

	// Runs the active entrance stages on `frame` for one call site (the registry's order; a
	// placeholder slot has no stage and never runs: zero cost).
	void Segment::RunStages(StageRun& run, pl_frame& frame)
	{
		if (!slots().ActiveEntrance().empty())
		{
			++entranceStageRuns;
			host.OnStagesRun();   // entrance_gpu_drain (group E): the drain sees every entrance run
		}
		if (run.path == Path::Main && !StatelessNow())
		{
			const unsigned long long before = entranceStatefulBreaks;
			const uint64_t last = entranceStatefulLast;
			NoteStatefulRun(run.sequence);
			if (entranceStatefulBreaks != before && entranceStatefulBreaks <= 3)
				DebugLog::Log("Entrance once (B3): stateful run out of sequence (source %llu after %llu; phase picture %d, A alongside %d, repeat %d)",
					static_cast<unsigned long long>(run.sequence), static_cast<unsigned long long>(last), run.phasePicture ? 1 : 0,
					run.aNeeded ? 1 : 0, run.cadenceRepeat ? 1 : 0);
		}
		if (run.path != Path::Main || run.phasePicture)
			++entranceGeneration;   // the modules' output textures are about to change
		// The re-base slot switched off (pipeline_slots_off, Original pathway): its float-intermediate
		// note no longer applies (audit F3; Rebase() is not called then).
		if (run.path == Path::Main && !rebaseFormatNoteLogged.empty() && !slots().Runs(Id::ScalingRebase))
		{
			rebaseFormatNoteLogged.clear();
			host.ReleaseFallback("re-base precision");
		}
		for (Id id : slots().ActiveEntrance())
		{
			const int span = host.BeginStageSpan(PipelineSlots::Get(id).name);   // efficiency timers T2 (WP-5)
			switch (StageFor(id))
			{
			case Stage::Rebase: Rebase(run, frame); break;
			case Stage::Deband: Deband(run, frame); break;
			case Stage::Detail: Detail(run, frame); break;
			case Stage::RepairA:   // the held repair (A) slot: a placeholder until the net runner is wired
			case Stage::Route4k:
			case Stage::Refine:
			case Stage::Chroma:
			{
				// Placeholders: never active without a component (zero cost); if one registers its slot
				// before its stage exists, the picture passes through unchanged (byte-identical).
				pl_frame passed{};
				(void)Placeholder(frame, passed);
				frame = passed;
				break;
			}
			default: break;
			}
			host.EndStageSpan(span);
			if (!run.ready)
				return;
		}
	}

	// The main draw: the cached entrance result for a picture drawn as itself, else the stages.
	void Segment::RunMain(StageRun& run, pl_frame& frame)
	{
		const bool cacheable = !run.phasePicture && run.sequence != 0;
		// B3 switched off live: the ring's textures go (render thread).
		if (!Once() && entranceRingBytesLogged > 0 && host.Gpu())
			ReleaseRing(host.Gpu());
		// B3: a picture drawn as itself is A's processed picture, kept (owned) in the ring.
		const bool once = cacheable && Once();
		const uint64_t ringKey = once ? RingKey(frame) : 0;
		if (once)
			if (const OwnedEntrance* owned = entranceRing.Find(run.sequence, ringKey, Role::A))
			{
				frame = OwnedFrame(*owned, frame);
				++entranceReuses;
				DetailLookAhead(run);
				return;
			}
		const pl_frame rawFrame = frame;
		const uint64_t epoch = cacheable ? EpochFor(frame) : 0;
		if (cacheable)
			if (const pl_frame* hit = entranceCache.Find(run.sequence, epoch))
			{
				frame = *hit;
				++entranceReuses;
				DetailLookAhead(run);
				return;
			}
		RunStages(run, frame);
		++entranceRuns;
		if (cacheable)
			entranceCache.Store(run.sequence, epoch, frame);
		else
			entranceCache.Invalidate();
		if (once)
		{
			++entranceOnceA;
			(void)StoreOwned(run.sequence, ringKey, Role::A, rawFrame, frame);
		}
		LogCounters();
	}

	void Segment::LogCounters()
	{
		// The "Entrance cost" / "Entrance stages" lines and their cadence live in the diagnostics adapter.
		VpDiagnostics::EntranceCounters c = {
			entranceStageRuns, entranceSourceFrames, entranceRuns, entranceReuses, entranceOnceA, entranceOnceB,
			entranceOnceReused, entranceOncePictures, entranceOnceFallbacks, entranceStatefulBreaks, Once() };
		VpDiagnostics::LogEntranceCounters(c, entranceCostLoggedAt);
	}

	void Segment::Invalidate()
	{
		entranceCache.Invalidate();
		entranceRing.Invalidate();
	}

	void Segment::ReleaseDevice(pl_gpu gpu)
	{
		entranceCache.Invalidate();         // the cached entrance result pointed at those textures
		ReleaseRing(gpu);                   // B3: the processed pictures' own copies
		++entranceGeneration;
	}

	void Segment::OnceSwitchedOff()
	{
		entranceOnceNote.clear();
		host.ReleaseFallback("entrance once");
		host.ReleaseFallback("entrance once fill");
		// Its textures are freed on the render thread (RunMain), never from here.
	}

	// ---- B3: the input side once per source frame (pipeline_entrance_once, ADR-0024) ----

	// No stage that runs on this frame keeps state across frames (the re-base alone; the debander
	// idle; the detail stages off): then B processed ahead IS what B's own draw computes, and the
	// ring keeps it as B's A - the input side runs exactly once per source frame. A stage not
	// listed here counts as stateful (a new module states it explicitly).
	bool Segment::StatelessNow() const
	{
		for (Id id : slots().ActiveEntrance())
			switch (id)
			{
			case Id::ScalingRebase: break;
			case Id::RepairDeband: if (debandThreshold() > 0.0) return false; break;
			case Id::RepairTextureRestore: if (detailPulseRuns() || textureRestoreRuns()) return false; break;
			default: return false;
			}
		return true;
	}

	// The ring's key for a source frame: the frame (crop, range, colour - not the texture it was
	// uploaded into), the re-base and the active stages. The stages' per-frame parameters (the
	// readings, the vector field) are deliberately NOT in it: a source frame's processed picture,
	// once made (as A, or ahead as B with B's own field), is that frame's picture for its whole
	// time - the input side once per source frame, B ahead identical to B drawn as A. Any settings
	// apply invalidates the ring (Invalidate).
	uint64_t Segment::RingKey(const pl_frame& raw) const
	{
		EntranceEpoch::Key key;
		key.Add(0x5354415445ull);
		// The planes' layout, the codes' meaning and the crop (what the stages read); the per-frame
		// HDR metadata is applied from the drawn frame (OwnedFrame), so B made ahead with A's
		// descriptor is found by B's own draw (audit MED 4).
		key.Add(raw.num_planes);
		for (int i = 0; i < raw.num_planes && i < 4; ++i)
		{
			const pl_tex t = raw.planes[i].texture;
			key.Add(t ? t->params.w : 0).Add(t ? t->params.h : 0).Add(static_cast<const void*>(t ? t->params.format : nullptr));
			key.Add(raw.planes[i].components);
			for (int c = 0; c < 4; ++c)
				key.Add(raw.planes[i].component_mapping[c]);
		}
		key.Add(static_cast<int>(raw.repr.sys)).Add(static_cast<int>(raw.repr.levels)).Add(static_cast<int>(raw.repr.alpha));
		key.Add(raw.repr.bits.sample_depth).Add(raw.repr.bits.color_depth).Add(raw.repr.bits.bit_shift);
		key.Add(static_cast<int>(raw.color.primaries)).Add(static_cast<int>(raw.color.transfer));
		// The crop IS in the key: the stages read it (re-base geometry, the active picture), and a
		// start-up crop hold releasing must re-process a repeated frame (2026-10-24, still-frame
		// identity with B3 on vs off). Only the per-frame HDR metadata stays out.
		key.Add(raw.crop.x0).Add(raw.crop.y0).Add(raw.crop.x1).Add(raw.crop.y1);
		key.Add(rebaseFactor()).Add(rebaseFactorX());
		for (Id id : slots().ActiveEntrance())
			key.Add(static_cast<int>(id));
		return key.Value();
	}

	// Visible, never silent (owner rule): which pictures took the per-picture stages and why.
	// Audit HIGH 2: a fallback stays shown for its whole source frame (a picture made with the
	// fallback in the same call never releases it); released only by a source frame that runs
	// without one.
	void Segment::OnceFallback(const std::string& why)
	{
		++entranceOnceFallbacks;
		entranceOnceFallbackFor = entranceOnceCurrentA;
		if (why != entranceOnceNote)
		{
			entranceOnceNote = why;
			host.EngageFallback("entrance once", why + "; those pictures take the per-picture stages");
		}
	}

	void Segment::OnceWorking()
	{
		++entranceOncePictures;
		if (!entranceOnceNote.empty() && entranceOnceFallbackFor != entranceOnceCurrentA)
		{
			entranceOnceNote.clear();
			host.ReleaseFallback("entrance once");
		}
	}

	// A stateful main run for an EARLIER source frame than one already run (A after B was processed
	// as B's A: the hold chain and the history would restart). A forward skip is a capture gap.
	void Segment::NoteStatefulRun(uint64_t sequence)
	{
		if (sequence == 0)
			return;
		if (entranceStatefulLast != 0 && sequence < entranceStatefulLast)
			++entranceStatefulBreaks;
		entranceStatefulLast = (std::max)(entranceStatefulLast, sequence);
	}

	// Test only (VP_TEST_ENTRANCE_ONCE_FALLBACK_EVERY=N): every Nth picture between frames takes
	// the late fallback, so the harness proves the stateful stages never restart (HIGH 1).
	bool Segment::ForcedFallback()
	{
		if (!entranceOnceForcedRead)
		{
			entranceOnceForcedRead = true;
			// The process environment (the harness sets it with SetEnvironmentVariable).
			char v[32] = {};
			if (GetEnvironmentVariableA("VP_TEST_ENTRANCE_ONCE_FALLBACK_EVERY", v, sizeof(v)) > 0)
				entranceOnceForcedEvery = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
		}
		return entranceOnceForcedEvery > 0 && (++entranceOnceForcedCount % entranceOnceForcedEvery) == 0;
	}

	namespace
	{
		bool SamePlanes(const pl_frame& a, const pl_frame& b)
		{
			if (a.num_planes != b.num_planes)
				return false;
			for (int p = 0; p < a.num_planes; ++p)
				if (a.planes[p].texture != b.planes[p].texture)
					return false;
			return true;
		}
	}

	// The ring's own copy of a processed picture (a plain copy pass per plane, exact for the
	// 16-bit and float planes the stages write).
	bool Segment::CopyEntrance(const pl_frame& in, OwnedEntrance& out)
	{
		const pl_dispatch dispatch = host.Dispatch();
		const pl_gpu gpu = host.Gpu();
		out.frame = in;
		for (int p = 0; p < in.num_planes; ++p)
		{
			const pl_tex src = in.planes[p].texture;
			if (!src || !src->params.sampleable || !(src->params.format->caps & PL_FMT_CAP_RENDERABLE))
				return false;
			bool shared = false;
			for (int q = 0; q < p; ++q)
				if (in.planes[q].texture == src)
				{
					out.frame.planes[p].texture = out.frame.planes[q].texture;
					shared = true;
				}
			if (shared)
				continue;
			pl_tex_params tp{};
			tp.w = src->params.w;
			tp.h = src->params.h;
			tp.format = src->params.format;
			tp.sampleable = true;
			tp.renderable = true;
			tp.storable = (src->params.format->caps & PL_FMT_CAP_STORABLE) != 0;
			if (!pl_tex_recreate(gpu, &out.tex[p], &tp))
				return false;
			pl_shader sh = pl_dispatch_begin(dispatch);
			pl_sample_src sample{};
			sample.tex = src;
			sample.components = src->params.format->num_components;
			if (!pl_shader_sample_direct(sh, &sample))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_params dp{};
			dp.shader = &sh;
			dp.target = out.tex[p];
			if (!pl_dispatch_finish(dispatch, &dp))
				return false;
			out.frame.planes[p].texture = out.tex[p];
		}
		return true;
	}

	bool Segment::StoreOwned(uint64_t sequence, uint64_t key, Role role, const pl_frame& raw, const pl_frame& result)
	{
		OwnedEntrance& entry = entranceRing.Claim(sequence, key, role);
		entry.identity = SamePlanes(raw, result);
		entry.rawCrop = raw.crop;
		entry.rawW = raw.planes[0].texture ? raw.planes[0].texture->params.w : 0;
		entry.rawH = raw.planes[0].texture ? raw.planes[0].texture->params.h : 0;
		if (entry.identity)
			entry.frame = result;
		else if (!CopyEntrance(result, entry))
		{
			OnceFallback("cannot copy the processed picture");
			return false;
		}
		entranceRing.Commit();
		// The ring's memory, logged when it changes (3 pictures at the processing size).
		size_t bytes = 0;
		entranceRing.ForEachPayload([&](OwnedEntrance& e)
			{
				for (pl_tex t : e.tex)
					if (t) bytes += static_cast<size_t>(t->params.w) * static_cast<size_t>(t->params.h) * t->params.format->texel_size;
			});
		if (bytes != entranceRingBytesLogged)
		{
			entranceRingBytesLogged = bytes;
			DebugLog::Log("Entrance once (B3): ring of processed pictures %.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
		}
		return true;
	}

	void Segment::ReleaseRing(pl_gpu gpu)
	{
		entranceRing.ForEachPayload([&](OwnedEntrance& e)
			{
				for (pl_tex& t : e.tex)
					if (t) pl_tex_destroy(gpu, &t);
				e = OwnedEntrance{};
			});
		entranceRing.Invalidate();
		entranceRingBytesLogged = 0;
	}

	// The processed A of `sequence`: its entrance result drawn as itself (the stateful stages -
	// the debander's hold, the detail history - advanced on it once), from the ring.
	const OwnedEntrance* Segment::ProcessedA(const pl_frame& rawA, uint64_t sequence, uint64_t nextSequence, const pl_frame* image)
	{
		const uint64_t key = RingKey(rawA);
		if (const OwnedEntrance* hit = entranceRing.Find(sequence, key, Role::A))
		{
			++entranceOnceReused;
			return hit;
		}
		StageRun run;
		run.path = Path::Main;
		run.factorX = rebaseFactorX();
		run.factorY = rebaseFactor();
		run.sequence = sequence;
		run.source = &rawA;
		run.nextSequence = nextSequence;
		run.image = image;
		run.aFrame = rawA;
		pl_frame frame = rawA;
		RunMain(run, frame);   // stores A in the ring (B3 on)
		return entranceRing.Find(sequence, key, Role::A);
	}

	// The processed B (the next source frame) ahead of its own draw: the per-picture stages of an
	// extra (re-base, VP deband per picture, the detail stages' decision of A as a repeat), never
	// advancing any stage's state. Once per next frame (the ring).
	const OwnedEntrance* Segment::ProcessedB(const pl_frame& rawB, uint64_t nextSequence, uint64_t aSequence)
	{
		if (nextSequence == 0)
		{
			OnceFallback("no next frame known");
			return nullptr;
		}
		const uint64_t key = RingKey(rawB);
		// B already processed as its own A (ahead, or stateless).
		if (const OwnedEntrance* hit = entranceRing.Find(nextSequence, key, Role::A))
		{
			++entranceOnceReused;
			return hit;
		}
		const bool stateless = StatelessNow();
		if (!stateless)
		{
			// (i) Stateful stages (the debander's hold, the detail history): B is processed exactly
			// as its own draw will process it - with B's own vector field, estimated ahead (B, C)
			// while A's consumers read the snapshot of (A, B) - and kept as B's A: exact continuity
			// at the source-frame boundary.
			std::string why;
			if (host.FieldAhead(aSequence, nextSequence, why))
			{
				const OwnedEntrance* bA = ProcessBAsA(rawB, nextSequence, key);
				if (bA)
					entranceOnceBAsAFor = aSequence;
				return bA;
			}
			// (ii) The fallback, visible: B per picture (A's decisions as a repeat, never advancing).
			OnceFallback("B's vectors not ready ahead (" + why + "); B per picture");
		}
		const Role role = stateless ? Role::A : Role::B;
		if (!stateless)
			if (const OwnedEntrance* hit = entranceRing.Find(nextSequence, key, role))
			{
				++entranceOnceReused;
				return hit;
			}
		StageRun run;
		run.path = Path::Extra;
		run.factorX = rebaseFactorX();
		run.factorY = rebaseFactor();
		run.sequence = aSequence;   // the detail stages apply A's decision (a repeat, never advancing)
		pl_frame frame = rawB;
		RunStages(run, frame);      // moves entranceGeneration: the plain cache is not reused after it
		++entranceOnceB;
		LogCounters();
		if (!StoreOwned(nextSequence, key, role, rawB, frame))
			return nullptr;
		return entranceRing.Find(nextSequence, key, role);
	}

	// B through the main (stateful) path as its own A: B's field and sequence in the stages'
	// parameters (the readings stay A's: a one-frame lag of the bitrate/level readings), the hold
	// and the history advanced on B once. Kept as B's A in the ring.
	const OwnedEntrance* Segment::ProcessBAsA(const pl_frame& rawB, uint64_t bSequence, uint64_t key)
	{
		VpDebander::Params& vpDebandParams = *b.vpDebandParams;
		DetailPulse::Params& detailPulseParams = *b.detailPulseParams;
		TextureRestore::Params& textureRestoreParams = *b.textureRestoreParams;
		const VpDebander::Params savedDeband = vpDebandParams;
		const DetailPulse::Params savedPulse = detailPulseParams;
		const TextureRestore::Params savedRestore = textureRestoreParams;
		const Host::Field bField = host.FieldOf(bSequence);
		vpDebandParams.field = bField.field;
		vpDebandParams.grainError = bField.grainError;
		vpDebandParams.sequence = bSequence;
		detailPulseParams.field = bField.field;
		detailPulseParams.sequence = bSequence;
		textureRestoreParams.field = bField.field;
		textureRestoreParams.sequence = bSequence;
		StageRun run;
		run.path = Path::Main;
		run.factorX = rebaseFactorX();
		run.factorY = rebaseFactor();
		run.sequence = bSequence;
		run.source = &rawB;
		run.nextSequence = 0;   // the detail look-ahead (optional) is not taken ahead
		run.image = &rawB;
		run.aFrame = rawB;
		pl_frame frame = rawB;
		RunStages(run, frame);
		++entranceGeneration;   // the modules' slot-0 textures now hold B: the plain cache must not reuse A's
		vpDebandParams = savedDeband;
		detailPulseParams = savedPulse;
		textureRestoreParams = savedRestore;
		++entranceOnceB;
		LogCounters();
		if (!StoreOwned(bSequence, key, Role::A, rawB, frame))
			return nullptr;
		return entranceRing.Find(bSequence, key, Role::A);
	}

	// ---- the stages ----------------------------------------------------------------------------

	// scaling.rebase
	void Segment::Rebase(StageRun& run, pl_frame& frame)
	{
		SourceRebase::Module& sourceRebase = *b.sourceRebase;
		if (run.path == Path::Main)
		{
			if (rebaseFactor() < 1.0 || rebaseFactorX() < 1.0)
			{
				const pl_dispatch dispatch = host.Dispatch();
				pl_frame rebased{};
				// The picture being drawn (a Stage B picture between frames, or A).
				if (sourceRebase.Apply(host.Gpu(), dispatch, frame, rebaseFactorX(), rebaseFactor(), rebased, 0))
				{
					frame = rebased;
					if (sourceRebase.FormatNote() != rebaseFormatNoteLogged)
					{
						rebaseFormatNoteLogged = sourceRebase.FormatNote();
						DebugLog::Log("Re-base: %s", rebaseFormatNoteLogged.c_str());
						// A float (fp16) intermediate instead of r16 keeps ~11 bits near white: visible.
						if (rebaseFormatNoteLogged.empty())
							host.ReleaseFallback("re-base precision");
						else
							host.EngageFallback("re-base precision", rebaseFormatNoteLogged);
					}
				}
				else if (sourceRebase.Failures() == 1)
					DebugLog::Log("Re-base failed (%s); the native frame is rendered", sourceRebase.LastError().c_str());
				// A itself, for the hold and the history (its own slot).
				if (run.aNeeded && run.aReady)
				{
					pl_frame rebasedA{};
					run.aReady = sourceRebase.Apply(host.Gpu(), dispatch, run.aFrame, rebaseFactorX(), rebaseFactor(), rebasedA, 1);
					if (run.aReady) run.aFrame = rebasedA;
				}
			}
			else if (!rebaseFormatNoteLogged.empty())
			{
				// No re-base on this picture: its float-intermediate note no longer applies (finding R1).
				rebaseFormatNoteLogged.clear();
				host.ReleaseFallback("re-base precision");
			}
			return;
		}
		if (run.path == Path::Extra)
		{
			// Stage 12: an extra shows the same re-based source as the main frame.
			if (rebaseFactor() < 1.0 || rebaseFactorX() < 1.0)
			{
				const pl_dispatch dispatch = host.Dispatch();
				pl_frame rebased{};
				if (sourceRebase.Apply(host.Gpu(), dispatch, frame, rebaseFactorX(), rebaseFactor(), rebased, 1))
					frame = rebased;
			}
			return;
		}
		// The crossfade's path being left, at the factors faded from.
		if (run.factorY < 1.0 || run.factorX < 1.0)
		{
			const pl_dispatch dispatch = host.Dispatch();
			pl_frame rebasedFrom{};
			run.ready = sourceRebase.Apply(host.Gpu(), dispatch, frame, run.factorX, run.factorY, rebasedFrom,
				SourceRebase::Module::kSlotCrossfade);
			if (run.ready) frame = rebasedFrom;
		}
	}

	// repair.deband (Stage 12 §2b/§2d: VP's debander at source resolution)
	void Segment::Deband(StageRun& run, pl_frame& frame)
	{
		if (!(debandThreshold() > 0.0))
			return;
		VpDebander::Module& debander = *b.debander;
		const VpDebander::Params& vpDebandParams = *b.vpDebandParams;
		const pl_dispatch dispatch = host.Dispatch();
		const pl_gpu gpu = host.Gpu();
		if (run.path == Path::Main)
		{
			// (6) The hold (slot 0) advances once per source frame on A itself, even when every
			// refresh of the frame shows a picture between frames: A's first draw. Hunt 3 F5: A's
			// debanded picture (the hold's draw) is also the picture the detail stages' history
			// advances on, so the history has one domain (debanded).
			if (run.phasePicture && !run.cadenceRepeat && run.aNeeded && run.aReady)
			{
				pl_frame heldA{};
				if (debander.Apply(gpu, dispatch, run.aFrame, vpDebandParams, heldA, 0))
					run.aFrame = heldA;
			}
			// A picture between frames decides per picture (slot 1, no hold along A's field).
			VpDebander::Params phaseParams = vpDebandParams;
			if (run.phasePicture) { phaseParams.field = nullptr; phaseParams.grainError = nullptr; }
			pl_frame debanded{};
			if (debander.Apply(gpu, dispatch, frame, run.phasePicture ? phaseParams : vpDebandParams, debanded,
				run.phasePicture ? 1 : 0))
				frame = debanded;
			else if (debander.Failures() == 1)
				DebugLog::Log("VP deband failed (%s); the frame is rendered without it", debander.LastError().c_str());
			return;
		}
		VpDebander::Params p = vpDebandParams;
		p.field = nullptr;
		p.grainError = nullptr;
		pl_frame debanded{};
		if (run.path == Path::Extra)
		{
			// The same debander per frame on an extra.
			if (debander.Apply(gpu, dispatch, frame, p, debanded, 1))
				frame = debanded;
			return;
		}
		// Hunt 3 (crossfade lead): the path being left keeps VP's debander (its crossfade slot),
		// at its own geometry.
		const double ratio = rebaseFactor() > 0.0 ? run.factorY / rebaseFactor() : 1.0;
		p.radiusScale = vpDebandParams.radiusScale * ratio;
		if (debander.Apply(gpu, dispatch, frame, p, debanded, VpDebander::Module::kSlotCrossfade))
			frame = debanded;
	}

	// repair.texture_restore: detail_pulse_stabilise, then texture_restore (docs/VP-detail-restore.md).
	// Never while smooth motion / deinterlacing runs (those flags are false then).
	void Segment::Detail(StageRun& run, pl_frame& frame)
	{
		if (!detailPulseRuns() && !textureRestoreRuns())
			return;
		DetailPulse::Module& detailPulse = *b.detailPulse;
		TextureRestore::Module& textureRestore = *b.textureRestore;
		const DetailPulse::Params& detailPulseParams = *b.detailPulseParams;
		const TextureRestore::Params& textureRestoreParams = *b.textureRestoreParams;
		const pl_dispatch dispatch = host.Dispatch();
		const pl_gpu gpu = host.Gpu();
		if (run.path == Path::CrossfadeFrom)
		{
			// The decision of this source frame on the picture's own bands (ApplyForeign: another
			// geometry, never advancing), so neither drops out in one step when a fade starts.
			const double ratio = rebaseFactor() > 0.0 ? run.factorY / rebaseFactor() : 1.0;
			if (detailPulseRuns() && run.sequence != 0 && detailPulse.HistorySequence() == run.sequence)
			{
				DetailPulse::Params p = detailPulseParams;
				p.planeLines = detailPulseParams.planeLines * ratio;
				pl_frame pulsed{};
				if (detailPulse.ApplyForeign(gpu, dispatch, frame, p, pulsed))
					frame = pulsed;
			}
			if (textureRestoreRuns() && run.sequence != 0 && textureRestore.HistorySequence() == run.sequence)
			{
				TextureRestore::Params p = textureRestoreParams;
				p.planeLines = textureRestoreParams.planeLines * ratio;
				pl_frame restored{};
				if (textureRestore.ApplyForeign(gpu, dispatch, frame, p, restored))
					frame = restored;
			}
			return;
		}
		if (run.path == Path::Extra)
		{
			// An extra shows its source frame's decision (a repeat of the sequence the history
			// advanced on with A), so the detail level does not change from refresh to refresh.
			// Never advances: skipped when A's own frame was not processed (another sequence).
			if (detailPulseRuns() && detailPulseParams.sequence == run.sequence &&
				detailPulse.HistorySequence() == run.sequence && run.sequence != 0)
			{
				pl_frame pulsed{};
				if (detailPulse.Apply(gpu, dispatch, frame, detailPulseParams, pulsed))
					frame = pulsed;
			}
			if (textureRestoreRuns() && textureRestoreParams.sequence == run.sequence &&
				textureRestore.HistorySequence() == run.sequence && run.sequence != 0)
			{
				pl_frame restored{};
				if (textureRestore.Apply(gpu, dispatch, frame, textureRestoreParams, restored))
					frame = restored;
			}
			return;
		}
		// Main. A Stage B picture between frames takes its frame's decision as a repeat (no
		// refresh-to-refresh detail flicker). The history advances once per source frame on A
		// itself (processed by the stages before this one, as drawn): A's first draw.
		if (run.phasePicture && !run.cadenceRepeat && run.aNeeded && run.aReady &&
			((detailPulseRuns() && detailPulse.HistorySequence() != run.sequence) ||
			 (textureRestoreRuns() && textureRestore.HistorySequence() != run.sequence)))
		{
			pl_frame aFrame = run.aFrame;
			if (detailPulseRuns())
			{
				pl_frame pulsedA{};
				if (detailPulse.Apply(gpu, dispatch, aFrame, detailPulseParams, pulsedA))
					aFrame = pulsedA;
			}
			if (textureRestoreRuns())
			{
				pl_frame restoredA{};
				(void)textureRestore.Apply(gpu, dispatch, aFrame, textureRestoreParams, restoredA);
			}
		}
		if (detailPulseRuns())
		{
			DetailLookAhead(run);
			pl_frame pulsed{};
			if (detailPulse.Apply(gpu, dispatch, frame, detailPulseParams, pulsed))
			{
				frame = pulsed;
				b.detailPulseInactive->clear();
			}
			else
			{
				// Hunt 3 F8: counted and reported ("inactive (...)" in the status), logged
				// whenever the reason changes.
				if (detailPulse.LastError() != detailPulseFailLogged)
					DebugLog::Log("Detail pulse stabilise: not applied (%s; %llu frames so far); the frame is rendered without it",
						detailPulse.LastError().c_str(), detailPulse.Failures());
				detailPulseFailLogged = detailPulse.LastError();
				*b.detailPulseInactive = detailPulse.LastError();
			}
		}
		if (textureRestoreRuns())
		{
			pl_frame restored{};
			if (textureRestore.Apply(gpu, dispatch, frame, textureRestoreParams, restored))
			{
				frame = restored;
				b.textureRestoreInactive->clear();
			}
			else
			{
				if (textureRestore.LastError() != textureRestoreFailLogged)
					DebugLog::Log("Texture restore: not applied (%s; %llu frames so far); the frame is rendered without it",
						textureRestore.LastError().c_str(), textureRestore.Failures());
				textureRestoreFailLogged = textureRestore.LastError();
				*b.textureRestoreInactive = textureRestore.LastError();
			}
		}
	}

	// detail_pulse_stabilise's look-ahead (optional): the next source frame's band energies when
	// it is already uploaded (the de-judder's B) and no re-base is engaged; once per new source
	// frame. Hunt 3 F4: in Apply's domain - B through VP's debander (per picture, its look-ahead
	// slot) when the debander runs on this frame. Also checked on a reused entrance result.
	void Segment::DetailLookAhead(const StageRun& run)
	{
		if (!detailPulseRuns() || !slots().Runs(Id::RepairTextureRestore))
			return;
		uint64_t& detailPulseAheadSequence = *b.detailPulseAheadSequence;
		const Host::NextUpload next = host.Next();
		if (!run.cadenceRepeat && run.nextSequence != 0 && detailPulseAheadSequence != run.nextSequence &&
			next.sequence == run.nextSequence && next.luma && next.chroma &&
			!next.rgbLuma && run.image && run.image->num_planes == 2 && rebaseFactor() == 1.0 && rebaseFactorX() == 1.0)
		{
			const pl_dispatch dispatch = host.Dispatch();
			detailPulseAheadSequence = run.nextSequence;
			pl_frame nextFrame = *run.image;
			nextFrame.planes[0].texture = next.luma;
			nextFrame.planes[1].texture = next.chroma;
			(void)b.detailPulse->MeasureAhead(host.Gpu(), dispatch, nextFrame, next.sequence, *b.detailPulseParams,
				debandThreshold() > 0.0 && slots().Runs(Id::RepairDeband) ? b.debander : nullptr, b.vpDebandParams);
		}
	}
}
