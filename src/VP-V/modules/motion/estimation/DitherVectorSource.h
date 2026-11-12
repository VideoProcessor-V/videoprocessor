// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// motion.estimation / DITHER VECTOR SOURCE (W2-C8; doc 93 W2-C8, refs/110 BRIEF C8): where the output sigma-delta's block vectors
// for a source frame's first present come from, as ONE visible chain (doc 91 D: one motion source, consumers ask the engine;
// rule 25 addenda 2-3: the fallback stays, each step down is visible, the fallback lives in the owning module):
//   1 SharedPair    the engine's verified field of the pair (seq, next) (IVectorEngine::FieldOf(next), pairFrom == seq) and
//                   the pair serves (MotionValidity::PairServes = PairValid && PairValidAt; PairValidAt holds the Indicator test
//                   SpanStatus != Invalid), the geometry valid -> mapped to output blocks by DejudderRendered::VectorField::Run.
//                   Coordinator 2026-11-11 Q1: SpanStatus == Unknown (resync_event off: no stamps) still reuses, VISIBLY
//                   (spanUnknown: a counter, the status segment and the "dither vectors unvalidated" fallback entry).
//   2 MatcherField  no engine field vouches for (seq, next), but the matcher's live outputs are this frame's (the P3 view's
//                   LiveValidFor = the pre-C8 rule DejudderLiveFieldFor) and the stamps do not refuse the span.
//   3 OnDemand      step 1's field, whose optical-flow pair the engine made ON DEMAND (W2-C5b; VectorEngine::OnDemandServed):
//                   no pair was computed ahead. The request itself is the engine's (the walk stages it, the estimate claims it
//                   within the frame budget): the dither never asks a second time for the same pair (compute once).
//   4 OwnSearch     the output-domain search (fallback/OutputDomainSearchGlsl.h, the passes moved out of the sigma-delta)
//                   measures the motion on the presented picture: dither_vectors own (by choice), or every step above absent
//                   (the reason, incl. an on-demand miss, is named).
//   5 Static        no motion compensation (the sigma-delta rule's motion_compensation is 0, the shader default: the plain
//                   motion gate; no vectors are mapped then - the field mapping would be read by nothing).
// Pure policy + counters (no renderer, no D3D, no clock): the renderer's Impl fills Facts once per decision
// (DitherVectorSourceHost.inl) and records the Choice; DitherVectorSourceTests drive it directly. C++14.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace VpMotion
{
namespace DitherVectors
{
	enum class Step : unsigned char
	{
		None = 0,           // the sigma-delta does not run (its slot is off): nothing to decide
		SharedPair = 1,
		MatcherField = 2,
		OnDemand = 3,
		OwnSearch = 4,
		Static = 5,
	};
	constexpr int kSteps = 6;

	// Why step 4 (or 5) was taken. OwnSetting / NotServed / NoPatch are choices or covered elsewhere (no fallback entry of
	// this chain); the others are automatic downgrades (the "dither vectors own search" entry names them).
	enum class Why : unsigned char
	{
		None,
		OwnSetting,        // dither_vectors own
		NoPatch,           // this libplacebo cannot bind VPVEC (no fork patch 0004): the sigma-delta always searches
		NotServed,         // the vector engine does not serve the sigma-delta (vector_demand off / not the full engine: the
		                   // engine's own "motion engine" entry says why)
		NoFrames,          // no next frame known yet (end of the queue)
		TimelineDrew,      // the timeline / hiccup repair drew refreshes: the last picture was not at shift 0
		NoMatcher,         // no matcher (the engine holds no field at all)
		GeometryInvalid,   // the de-judder geometry is not valid for this frame
		PairInvalid,       // a field exists but the pair does not serve (epoch, adjacency or the stamps refuse it)
		NoField,           // no field covers (seq, next)
		OnDemandMissed,    // no field, and the engine's on-demand pair for it was late / unavailable / not asked (cause named)
		MapFailed,         // the field -> block mapping (VectorField::Run) failed or there is no GPU
		MotionCompensationOff,   // step 5: the rule's motion_compensation is 0
	};

	inline const char* StepName(Step s)
	{
		switch (s)
		{
		case Step::None: return "not running";
		case Step::SharedPair: return "shared pair";
		case Step::MatcherField: return "matcher field (this frame)";
		case Step::OnDemand: return "on-demand pair";
		case Step::OwnSearch: return "own search";
		case Step::Static: return "static (no motion compensation)";
		}
		return "?";
	}
	inline const char* WhyName(Why w)
	{
		switch (w)
		{
		case Why::None: return "";
		case Why::OwnSetting: return "dither_vectors own";
		case Why::NoPatch: return "this libplacebo cannot bind the vectors (no fork patch 0004)";
		case Why::NotServed: return "the vector engine does not serve the sigma-delta";
		case Why::NoFrames: return "no next frame yet";
		case Why::TimelineDrew: return "the timeline drew the refreshes";
		case Why::NoMatcher: return "no motion field";
		case Why::GeometryInvalid: return "the picture geometry changed";
		case Why::PairInvalid: return "the pair is not valid (a discontinuity or a stale field)";
		case Why::NoField: return "no field for this pair";
		case Why::OnDemandMissed: return "no field: the on-demand pair was missed";
		case Why::MapFailed: return "the vector mapping failed";
		case Why::MotionCompensationOff: return "motion_compensation 0";
		}
		return "?";
	}

	// The facts of one decision (render thread; the renderer fills them, the tests set them).
	struct Facts
	{
		bool slotRuns = false;             // the sigma-delta runs (output.dither slot, DitherSlot::VectorsWanted's first term)
		bool patch = false;                // VPVEC can be bound (SigmaDeltaVectors::Supported)
		bool ownSetting = false;           // dither_vectors own
		bool motionCompensation = false;   // the sigma-delta hook's motion_compensation >= 0.5 (shader default 0.0)
		bool served = false;               // VectorDemand::Serves(Consumer::SigmaDelta)
		bool sequences = false;            // seq != 0 && next != 0
		bool timelineDrew = false;         // interpolating || the VP timeline drew this frame's refreshes
		bool matcher = false;              // a matcher (the engine's field source) exists
		bool geometryValid = false;
		bool engineField = false;          // IVectorEngine::FieldOf(next) returned the pair (seq, next)
		bool pairServes = false;           // MotionValidity::PairServes(seq, next, field epoch, field epoch)
		MotionValidity::SpanState span = MotionValidity::SpanState::Unknown;   // SpanStatus(seq, next)
		bool onDemandServed = false;       // VectorEngine::OnDemandServed(seq, next)
		FallbackCatalog::NoPairCause onDemandCause = FallbackCatalog::NoPairCause::None;   // VectorEngine::OnDemandLastCause
		bool matcherLive = false;          // the P3 view's LiveValidFor(seq) (the matcher ran this frame)
	};

	struct Choice
	{
		Step step = Step::None;
		Why why = Why::None;
		bool spanUnknown = false;          // the reuse (steps 1-3) rests on the epoch form only: SpanStatus Unknown (Q1: visible)
		FallbackCatalog::NoPairCause onDemandCause = FallbackCatalog::NoPairCause::None;
		bool Reuses() const { return step == Step::SharedPair || step == Step::MatcherField || step == Step::OnDemand; }
	};

	inline bool OnDemandMiss(FallbackCatalog::NoPairCause c)
	{
		using C = FallbackCatalog::NoPairCause;
		return c == C::OnDemandLate || c == C::OnDemandUnavailable || c == C::OnDemandNoBudget || c == C::OnDemandUncached;
	}

	inline Choice Own(Why why, FallbackCatalog::NoPairCause cause = FallbackCatalog::NoPairCause::None)
	{
		Choice c;
		c.step = Step::OwnSearch;
		c.why = why;
		c.onDemandCause = cause;
		return c;
	}

	// The chain. Every return names its step; no step is reachable without a status (DitherVectorSourceTests).
	inline Choice Choose(const Facts& f)
	{
		using S = MotionValidity::SpanState;
		if (!f.slotRuns)
			return Choice();
		if (!f.motionCompensation)
		{
			Choice c;
			c.step = Step::Static;
			c.why = Why::MotionCompensationOff;
			return c;
		}
		if (f.ownSetting)
			return Own(Why::OwnSetting);
		if (!f.patch)
			return Own(Why::NoPatch);
		if (!f.served)
			return Own(Why::NotServed);
		if (!f.sequences)
			return Own(Why::NoFrames);
		if (f.timelineDrew)
			return Own(Why::TimelineDrew);
		if (!f.matcher)
			return Own(Why::NoMatcher);
		if (!f.geometryValid)
			return Own(Why::GeometryInvalid);
		if (f.engineField)
		{
			// PairServes already holds SpanStatus != Invalid (PairValidAt); the span is checked again so a caller that passes
			// pairServes from the epoch form alone can never reuse across a refused span.
			if (!f.pairServes || f.span == S::Invalid)
				return Own(Why::PairInvalid);
			Choice c;
			c.step = f.onDemandServed ? Step::OnDemand : Step::SharedPair;
			c.spanUnknown = f.span == S::Unknown;
			return c;
		}
		if (f.matcherLive)
		{
			if (f.span == S::Invalid)
				return Own(Why::PairInvalid);
			Choice c;
			c.step = Step::MatcherField;
			c.spanUnknown = f.span == S::Unknown;
			return c;
		}
		if (OnDemandMiss(f.onDemandCause))
			return Own(Why::OnDemandMissed, f.onDemandCause);
		return Own(Why::NoField);
	}

	// Automatic downgrades (an entry in "Fallbacks engaged"): not a choice (own, no patch) and not the engine's own entry
	// (NotServed: "motion engine"), not a plain end of the queue.
	inline bool Automatic(const Choice& c)
	{
		if (c.step == Step::MatcherField || c.step == Step::OnDemand)
			return true;
		if (c.step != Step::OwnSearch)
			return false;
		return c.why != Why::OwnSetting && c.why != Why::NoPatch && c.why != Why::NotServed && c.why != Why::NoFrames;
	}

	// Counters + the status segment + the four fallback entries of one renderer (render thread; the status reader takes a copy
	// under the host's own status mutex). Entries are held for holdMs after their last use so a per-frame flip does not flicker.
	class Tracker
	{
	public:
		static constexpr unsigned long long kHoldMs = 3000;

		// Records one decision; mapped = steps 1-3 produced the vectors (false turns the decision into step 4 MapFailed).
		Choice Note(Choice c, bool mapped, unsigned long long nowMs)
		{
			if (c.Reuses() && !mapped)
				c = Own(Why::MapFailed);
			++counts_[static_cast<int>(c.step)];
			if (c.spanUnknown)
			{
				++spanUnknown_;
				lastUnknownMs_ = nowMs;
			}
			if (Automatic(c))
				lastAutomaticMs_[static_cast<int>(c.step)] = nowMs;
			changed_ = changed_ || c.step != last_.step || c.why != last_.why || c.spanUnknown != last_.spanUnknown;
			last_ = c;
			lastMs_ = nowMs;
			return c;
		}
		const Choice& Last() const { return last_; }
		unsigned long long Count(Step s) const { return counts_[static_cast<int>(s)]; }
		unsigned long long SpanUnknownUses() const { return spanUnknown_; }
		// True once after the step (or its reason) changed: the host logs one line then (never per frame).
		bool TakeChanged()
		{
			const bool c = changed_;
			changed_ = false;
			return c;
		}

		// "dither vectors: shared pair (shared 120, matcher 0, on-demand 4, own 3, static 0; unvalidated 124)". Empty before the
		// first decision.
		std::string StatusSegment() const
		{
			if (lastMs_ == 0 && Total() == 0)
				return std::string();
			std::string s = std::string("dither vectors: ") + StepName(last_.step);
			if (last_.why != Why::None)
				s += std::string(" (") + WhyName(last_.why) + OnDemandNote(last_) + ")";
			else if (last_.spanUnknown)
				s += " (validity unknown: resync_event off)";
			char counts[200];
			std::snprintf(counts, sizeof(counts), "; shared %llu, matcher %llu, on-demand %llu, own %llu, static %llu",
				Count(Step::SharedPair), Count(Step::MatcherField), Count(Step::OnDemand), Count(Step::OwnSearch), Count(Step::Static));
			s += counts;
			if (spanUnknown_ > 0)
				s += ", unvalidated " + std::to_string(spanUnknown_);
			return s;
		}

		// Every entry of this chain released (the host's release / teardown).
		static std::vector<FallbackCatalog::Decision> Released()
		{
			return { FallbackCatalog::DitherVectorsMatcherField(false), FallbackCatalog::DitherVectorsOnDemand(false),
				FallbackCatalog::DitherVectorsOwnSearch(false, std::string()), FallbackCatalog::DitherVectorsUnvalidated(false) };
		}

		// The four entries, in a fixed order (stable ids: FallbackCatalog DitherVectors*). Step 5 (static) is the configuration
		// motion_compensation 0 - the shipped default of the sigma-delta rule - so it is in the status segment only.
		std::vector<FallbackCatalog::Decision> Decisions(unsigned long long nowMs) const
		{
			std::vector<FallbackCatalog::Decision> out;
			out.push_back(FallbackCatalog::DitherVectorsMatcherField(Recent(Step::MatcherField, nowMs)));
			out.push_back(FallbackCatalog::DitherVectorsOnDemand(Recent(Step::OnDemand, nowMs)));
			out.push_back(FallbackCatalog::DitherVectorsOwnSearch(Recent(Step::OwnSearch, nowMs),
				last_.step == Step::OwnSearch ? std::string(WhyName(last_.why)) + OnDemandNote(last_) : std::string()));
			out.push_back(FallbackCatalog::DitherVectorsUnvalidated(spanUnknown_ > 0 && nowMs - lastUnknownMs_ < kHoldMs));
			return out;
		}

	private:
		static std::string OnDemandNote(const Choice& c)
		{
			if (c.why != Why::OnDemandMissed)
				return std::string();
			const std::string note = FallbackCatalog::NoPairNote(c.onDemandCause);
			return note.empty() ? std::string() : ": " + note + ")";
		}
		unsigned long long Total() const
		{
			unsigned long long t = 0;
			for (int i = 0; i < kSteps; ++i)
				t += counts_[i];
			return t;
		}
		bool Recent(Step s, unsigned long long nowMs) const
		{
			const unsigned long long at = lastAutomaticMs_[static_cast<int>(s)];
			return at != 0 && nowMs - at < kHoldMs;
		}

		unsigned long long counts_[kSteps] = {};
		unsigned long long lastAutomaticMs_[kSteps] = {};
		unsigned long long spanUnknown_ = 0, lastUnknownMs_ = 0, lastMs_ = 0;
		Choice last_;
		bool changed_ = false;
	};

	// Per-host state (S8 state anchor, refs/107 s1: no renderer member): one Tracker per renderer, keyed by its Impl. Render thread
	// for the host's own entry; Forget at the host's release (ReleaseDejudder / teardown).
	class Registry
	{
	public:
		static Tracker& ForHost(const void* host)
		{
			std::lock_guard<std::mutex> lock(Mutex());
			std::unique_ptr<Tracker>& t = Map()[host];
			if (!t)
				t.reset(new Tracker());
			return *t;
		}
		// The host's tracker if it decided at least once (the status reader never creates one).
		static Tracker* Find(const void* host)
		{
			std::lock_guard<std::mutex> lock(Mutex());
			const auto it = Map().find(host);
			return it == Map().end() ? nullptr : it->second.get();
		}
		static void Forget(const void* host)
		{
			std::lock_guard<std::mutex> lock(Mutex());
			Map().erase(host);
		}
		static size_t Hosts()
		{
			std::lock_guard<std::mutex> lock(Mutex());
			return Map().size();
		}

	private:
		static std::mutex& Mutex() { static std::mutex m; return m; }
		static std::map<const void*, std::unique_ptr<Tracker>>& Map() { static std::map<const void*, std::unique_ptr<Tracker>> m; return m; }
	};
}
}
