// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Vector engine on demand: which consumers of motion vectors want them, which the engine can
// serve right now, and in what mode it runs. Pure decision logic (no GPU, no Windows, no clock).
//
// Synthesis consumers (de-judder extras, interpolator, hiccup repair, occlusion mask) have their
// own policies; they only count as "wants" here and are never in served / unserved.
//
// DELIBERATE exception: while libplacebo smooth motion runs, the source-domain analysis consumers
// (SourceField, SourceStatistics) keep the VP vectors; the output-domain consumer (sigma-delta)
// falls back visibly. Analysis only: no VP interpolation / de-judder synthesis runs alongside
// smooth motion. Revert with kAnalysisUnderSmoothMotion = false (smooth motion then owns output
// exclusively and every non-synthesis consumer falls back visibly).
namespace VectorDemand
{
	constexpr bool kAnalysisUnderSmoothMotion = true;
	constexpr unsigned kUnitVersion = 1;   // P2/P3 (vector pair, analysis field) unit version (core/ProductCatalog.h)

	enum class Consumer : int
	{
		DejudderExtras, Interpolator, HiccupRepair, OcclusionMask,
		SigmaDelta, GrainMap, DebandHold, DetailPulse, EntranceSegment,
		SourceMonitorPan, MotionIntent,
		// Camera-linking consumers (K0): appended so the indices above never move. All read the source field.
		TextureRestore, ResidualStabilise, TemporalDetail,
		// SK-W stage D: the camera fit (P7) wants the verified field of every pair (P3 and the pair it reads), so declaring it starts the
		// engine like any source-field consumer. Appended: the indices above never move.
		CameraFit
	};
	constexpr int kConsumerCount = 15;

	enum class Domain { Synthesis, Output, SourceField, SourceStatistics };

	inline Domain DomainOf(Consumer c)
	{
		switch (c)
		{
		case Consumer::DejudderExtras:
		case Consumer::Interpolator:
		case Consumer::HiccupRepair:
		case Consumer::OcclusionMask: return Domain::Synthesis;
		case Consumer::SigmaDelta: return Domain::Output;
		case Consumer::GrainMap:
		case Consumer::DebandHold:
		case Consumer::DetailPulse:
		case Consumer::EntranceSegment:
		case Consumer::TextureRestore:
		case Consumer::ResidualStabilise:
		case Consumer::TemporalDetail:
		case Consumer::CameraFit: return Domain::SourceField;
		case Consumer::SourceMonitorPan:
		case Consumer::MotionIntent: return Domain::SourceStatistics;
		}
		return Domain::Synthesis;
	}

	inline const char* Name(Consumer c)
	{
		switch (c)
		{
		case Consumer::DejudderExtras: return "dejudder extras";
		case Consumer::Interpolator: return "interpolator";
		case Consumer::HiccupRepair: return "hiccup repair";
		case Consumer::OcclusionMask: return "occlusion mask";
		case Consumer::SigmaDelta: return "output dither";
		case Consumer::GrainMap: return "grain map";
		case Consumer::DebandHold: return "deband hold";
		case Consumer::DetailPulse: return "detail pulse";
		case Consumer::EntranceSegment: return "entrance segment";
		case Consumer::SourceMonitorPan: return "source monitor";
		case Consumer::MotionIntent: return "intent";
		case Consumer::TextureRestore: return "texture restore";
		case Consumer::ResidualStabilise: return "residual stabilise";
		case Consumer::TemporalDetail: return "temporal detail";
		case Consumer::CameraFit: return "camera fit";
		}
		return "?";
	}

	enum class Mode { Off, Forward, Backward };
	// W2-C1 (B9): NoStarter appended (no index moved): reuse-only consumers want vectors but no starter runs the engine.
	enum class Reason { Disabled, SlotsOff, NoConsumer, Interlaced, SmoothMotionExclusive, Forward, ForwardShallow, BackwardDepth0, NoStarter };

	// W2-C1 (B9, owner 2026-11-10): the only consumers that may START the motion engine (matcher or producer): de-judder standing
	// demand (de-judder extras; the interpolator is de-judder family), hiccup repair and the camera fit. Every other consumer is
	// reuse-only: it reads the vectors a starter made this frame, else takes its non-motion path visibly (Reason::NoStarter).
	inline unsigned StarterMask()
	{
		return (1u << static_cast<int>(Consumer::DejudderExtras)) | (1u << static_cast<int>(Consumer::Interpolator)) |
			(1u << static_cast<int>(Consumer::HiccupRepair)) | (1u << static_cast<int>(Consumer::CameraFit));
	}
	inline bool IsStarter(Consumer c) { return (StarterMask() & (1u << static_cast<int>(c))) != 0; }

	struct Input
	{
		bool enabled = false;
		bool slotsRun = false;
		bool lowLatency = false;
		int queueAheadCapacity = 0;
		bool smoothMotion = false;
		bool interlaced = false;
		bool wants[kConsumerCount] = {};
		// W2-C1: a starter runs the engine this frame (set by the host from the de-judder standing class, the hiccup warm and the
		// camera want). Reuse-only consumers are served only while it is true; a served CameraFit want engages it by itself.
		bool starterEngaged = false;
		// W2-C1 (coordinator 2026-11-11): the motion engine exists while no starter runs it this frame (kept loaded and idle for hiccup
		// repair, or held until its release). Wording only: the status never says "idle (no starter)" while an engine exists, it names
		// why the engine is loaded. Serving is unchanged (an idle engine makes no vectors).
		bool engineLoaded = false;
		bool engineLoadedForHiccupRepair = false;
	};

	struct Decision
	{
		Mode mode = Mode::Off;
		Reason reason = Reason::Disabled;
		unsigned served = 0;     // bit (1 << Consumer)
		unsigned unserved = 0;   // bit (1 << Consumer)
		bool estimate = false;
		bool producer = false;
		bool lowLatencyAllowed = false;
		// Additions to the frozen API (needed for wording): libplacebo smooth motion was running,
		// and only synthesis consumers want vectors (their own policies decide).
		bool smoothMotion = false;
		bool synthesisOnly = false;
		// W2-C1: the starter consumers that want vectors (bit per Consumer, synthesis included), and the reuse-only consumers left
		// unserved because no starter ran (each is a refused start: MotionEngineWork::refusedStarts).
		unsigned starters = 0;
		unsigned refused = 0;
		// W2-C1: copied from Input (wording of the NoStarter status only).
		bool engineLoaded = false;
		bool engineLoadedForHiccupRepair = false;
		bool Serves(Consumer c) const { return (served & (1u << static_cast<int>(c))) != 0; }
	};

	inline Consumer ConsumerAt(int i) { return static_cast<Consumer>(i); }

	inline Decision DecideWith(const Input& in, bool analysisUnderSmoothMotion)
	{
		Decision d;
		d.engineLoaded = in.engineLoaded;
		d.engineLoadedForHiccupRepair = in.engineLoaded && in.engineLoadedForHiccupRepair;
		if (!in.enabled)
			return d;
		if (!in.slotsRun)
		{
			d.reason = Reason::SlotsOff;
			return d;
		}
		bool anyWant = false;
		unsigned wanted = 0;   // non-synthesis wants
		unsigned starterWants = 0;
		unsigned synthesisReuse = 0;   // W2-C1 (C1-4): reuse-only synthesis wants (the occlusion mask): refused like the others without a starter
		for (int i = 0; i < kConsumerCount; ++i)
		{
			if (!in.wants[i])
				continue;
			anyWant = true;
			if (IsStarter(ConsumerAt(i)))
				starterWants |= 1u << i;
			if (DomainOf(ConsumerAt(i)) != Domain::Synthesis)
				wanted |= 1u << i;
			else if (!IsStarter(ConsumerAt(i)))
				synthesisReuse |= 1u << i;
		}
		d.reason = Reason::NoConsumer;
		if (!anyWant)
			return d;
		d.starters = starterWants;
		if (wanted == 0)
		{
			d.synthesisOnly = true;
			// W2-C1 (C1-4): the occlusion mask alone, with no starter running, is a refused start (its own matcher never runs):
			// DejudderPolicy reads the refusal (occlusionMaskServed), and the status names it.
			if (synthesisReuse != 0 && !in.starterEngaged)
			{
				d.refused = synthesisReuse;
				d.unserved = synthesisReuse;
				d.reason = Reason::NoStarter;
			}
			return d;
		}
		d.smoothMotion = in.smoothMotion;
		if (in.interlaced)
		{
			d.unserved = wanted;
			d.reason = Reason::Interlaced;
			return d;
		}
		unsigned served = wanted;
		if (in.smoothMotion)
		{
			if (!analysisUnderSmoothMotion)
			{
				d.unserved = wanted;
				d.reason = Reason::SmoothMotionExclusive;
				return d;
			}
			for (int i = 0; i < kConsumerCount; ++i)
				if ((wanted & (1u << i)) != 0 && DomainOf(ConsumerAt(i)) == Domain::Output)
					served &= ~(1u << i);
			d.unserved = wanted & ~served;
			if (served == 0)
			{
				d.reason = Reason::SmoothMotionExclusive;
				return d;
			}
		}
		// W2-C1: reuse-only consumers are served only while a starter runs the engine this frame (a served camera fit is one).
		const bool engaged = in.starterEngaged || (served & StarterMask()) != 0;
		if (!engaged)
		{
			d.refused = (served & ~StarterMask()) | synthesisReuse;   // C1-4: the occlusion mask too (never in served)
			d.unserved |= d.refused;
			served &= StarterMask();
			if (served == 0)
			{
				d.reason = Reason::NoStarter;
				return d;
			}
		}
		// Low latency (owner 2026-11-01): vectors never ADD queue depth. A mode that queues no frame ahead
		// (competitive, capacity 0) runs backward pairs; a mode whose own queue holds frames ahead (games,
		// capacity >= 1) uses those already-queued frames (ForwardShallow, EngageAhead bounded by the capacity).
		if (in.lowLatency && in.queueAheadCapacity == 0)
		{
			unsigned stats = 0;
			for (int i = 0; i < kConsumerCount; ++i)
				if ((served & (1u << i)) != 0 && DomainOf(ConsumerAt(i)) == Domain::SourceStatistics)
					stats |= 1u << i;
			d.unserved |= served & ~stats;
			d.served = stats;
			d.mode = Mode::Backward;
			d.reason = Reason::BackwardDepth0;
			d.producer = (stats & StarterMask()) != 0;   // W2-C1: statistics are reuse-only (never a starter)
			d.estimate = false;
			d.lowLatencyAllowed = true;
			return d;
		}
		d.served = served;
		d.mode = Mode::Forward;
		d.reason = Reason::Forward;
		if (in.lowLatency && in.queueAheadCapacity >= 1)
		{
			d.reason = Reason::ForwardShallow;
			d.lowLatencyAllowed = true;
		}
		// A render-time estimate only for the starter consumers that need a GPU field (source field, output: the camera fit);
		// the reuse-only consumers read what a starter made and cost no matcher run of their own.
		unsigned fieldMask = 0;
		for (int i = 0; i < kConsumerCount; ++i)
			if (DomainOf(ConsumerAt(i)) == Domain::SourceField || DomainOf(ConsumerAt(i)) == Domain::Output)
				fieldMask |= 1u << i;
		d.estimate = (served & StarterMask() & fieldMask) != 0;
		d.producer = (served & StarterMask()) != 0;
		return d;
	}

	inline Decision Decide(const Input& in) { return DecideWith(in, kAnalysisUnderSmoothMotion); }

	// The look-ahead depth the engine engages at: the configured depth bounded by the queue's
	// capacity, at least one frame.
	inline int EngageAhead(int configured, int queueAheadCapacity)
	{
		if (queueAheadCapacity <= 0)
			return 1;
		const int bounded = configured < queueAheadCapacity ? configured : queueAheadCapacity;
		return bounded > 1 ? bounded : 1;
	}

	// Why a consumer the engine cannot serve goes without vectors.
	inline const char* UnservedWhy(const Decision& d, Consumer c)
	{
		if (d.reason == Reason::Interlaced)
			return "interlaced";
		if ((d.refused & (1u << static_cast<int>(c))) != 0)
			return "no starter runs the motion engine";
		if (d.smoothMotion && (d.reason == Reason::SmoothMotionExclusive || DomainOf(c) == Domain::Output))
			return "libplacebo smooth motion owns output";
		return "needs a frame ahead";
	}

	inline std::string NamesOf(unsigned bits)
	{
		std::string s;
		for (int i = 0; i < kConsumerCount; ++i)
		{
			if ((bits & (1u << i)) == 0)
				continue;
			if (!s.empty())
				s += ", ";
			s += Name(ConsumerAt(i));
		}
		return s;
	}

	// "x, y without vectors (why)" groups for the unserved consumers, grouped by reason.
	inline std::string UnservedText(const Decision& d)
	{
		std::vector<std::string> whys;
		std::vector<unsigned> groups;
		for (int i = 0; i < kConsumerCount; ++i)
		{
			if ((d.unserved & (1u << i)) == 0)
				continue;
			const std::string why = UnservedWhy(d, ConsumerAt(i));
			size_t g = 0;
			while (g < whys.size() && whys[g] != why)
				++g;
			if (g == whys.size())
			{
				whys.push_back(why);
				groups.push_back(0);
			}
			groups[g] |= 1u << i;
		}
		std::string s;
		for (size_t g = 0; g < whys.size(); ++g)
		{
			if (!s.empty())
				s += "; ";
			s += NamesOf(groups[g]) + " without vectors (" + whys[g] + ")";
		}
		return s;
	}

	inline std::string StatusLine(const Decision& d)
	{
		if (d.reason == Reason::Disabled)
			return std::string();
		if (d.reason == Reason::SlotsOff)
			return "Vector engine: off (slot off)";
		if (d.reason == Reason::NoConsumer)
			return d.synthesisOnly ? "Vector engine: idle (served by de-judder/interpolator policies)" :
				"Vector engine: idle (no consumer)";
		std::string s = "Vector engine: ";
		if (d.reason == Reason::NoStarter)
		{
			// W2-C1 (coordinator 2026-11-11): an engine that exists is named with the reason it is loaded, never "idle (no starter)".
			const char* state = d.engineLoadedForHiccupRepair ? "loaded idle for hiccup repair (no starter runs it); " :
				d.engineLoaded ? "loaded, not running (no starter runs it); " : "idle (no starter); ";
			return s + state + UnservedText(d);
		}
		if (d.mode == Mode::Forward)
			s += "on (forward)";
		else if (d.mode == Mode::Backward)
			s += "backward pairs (no frame ahead)";
		else
			s += "off";
		if (d.served != 0)
			s += " for " + NamesOf(d.served);
		if (d.unserved != 0)
			s += "; " + UnservedText(d);
		return s;
	}
	// refs/109 F1: the status line plus the look-ahead cap while it binds. `cappedLead` = MotionAhead::kMaxLead when the lead is held
	// at it (VectorEngine: sustained, forward look-ahead), else 0 (the line above, unchanged).
	inline std::string StatusLine(const Decision& d, int cappedLead)
	{
		std::string s = StatusLine(d);
		if (cappedLead > 0 && d.mode == Mode::Forward && !s.empty())
			s += "; look-ahead lead capped at " + std::to_string(cappedLead) + " frames (more are queued ahead)";
		return s;
	}
	// The whole-frame motion statistics every source-statistics consumer reads (source monitor fast-pan,
	// motion intent), from one engine pair's global vector. One formula for the forward producer hit,
	// a render-time sample and the backward pair. `sourcePixels` = the global vector in source pixels
	// per frame (DejudderPolicy::EngineGlobalSourcePixels); z / w = the ambiguity and validity of
	// the engine's global (usable below 0.9, valid above 0.5).
	struct GlobalSample
	{
		bool usable = false;          // z < 0.9: the source monitor's fast-pan switch may use it
		bool intentValid = false;     // the intent interpreter may use it
		double sourcePixels = 0.0;
		double heightsPerFrame = 0.0; // picture heights per frame
	};
	inline GlobalSample SampleGlobal(double sourcePixels, float z, float w, int pictureHeight)
	{
		GlobalSample g;
		g.usable = z < 0.9f;
		g.sourcePixels = sourcePixels;
		g.intentValid = w > 0.5f && z < 0.9f && pictureHeight > 0;
		g.heightsPerFrame = pictureHeight > 0 ? sourcePixels / pictureHeight : 0.0;
		return g;
	}

	// ---- W2-C1 (B9): the ONE global motion-engine state every consumer reads (IVectorEngine::State) --------------------------
	// Off = no starter engaged, or a hard absence; Degraded = a starter runs the matcher without NVOF pairs; On = a starter and a
	// running producer. Pure data here (light header: VectorEngineInterface.h includes it); VectorEngine derives it once per frame
	// (DeriveEngineStatus below) from the Decision and the producer facts. Reasons are appended only (bit = 1 << reason).
	enum class MotionEngineState : uint8_t { On, Degraded, Off };
	enum class MotionEngineReason : uint8_t
	{
		None, NoStarter, OpticalFlowOff, LowLatency, ProducerRecovering, NvofUnavailable, SmoothMotion, VramRelease, Resync,
		Interlaced, VectorDemandOff
	};
	constexpr int kMotionEngineReasonCount = 11;

	struct MotionEngineStatus
	{
		MotionEngineState state = MotionEngineState::Off;
		MotionEngineReason reason = MotionEngineReason::None;   // the leading reason (the first in enum order of `reasons`)
		unsigned reasons = 0;    // bit (1 << MotionEngineReason) per reason that holds
		unsigned starters = 0;   // bit (1 << Consumer) per starter engaged
		unsigned affected = 0;   // bit (1 << Consumer) per consumer without full motion: unserved, or served by a degraded engine
		uint64_t frame = 0;      // the frame the status was derived at
		bool Has(MotionEngineReason r) const { return (reasons & (1u << static_cast<int>(r))) != 0; }
	};

	// What the engine did (monotonic counters, render thread; zero-cost to read). refusedStarts[c] = frames a reuse-only consumer c
	// wanted vectors while no starter ran (Decision::refused).
	struct MotionEngineWork
	{
		uint64_t matcherCreates = 0, matcherEstimates = 0, warmUps = 0, analysisEstimates = 0, producerStarts = 0, producerPairs = 0;
		uint64_t refusedStarts[kConsumerCount] = {};
	};

	inline const char* StateWord(MotionEngineState s)
	{
		return s == MotionEngineState::On ? "on" : s == MotionEngineState::Degraded ? "degraded" : "off";
	}

	inline const char* ReasonWords(MotionEngineReason r)
	{
		switch (r)
		{
		case MotionEngineReason::None: return "";
		case MotionEngineReason::NoStarter: return "no starter (de-judder, hiccup repair or camera fit) runs it";
		case MotionEngineReason::OpticalFlowOff: return "optical flow is off (the matcher runs alone)";
		case MotionEngineReason::LowLatency: return "low latency";
		case MotionEngineReason::ProducerRecovering: return "the optical-flow producer is recovering";
		case MotionEngineReason::NvofUnavailable: return "NVIDIA optical flow is not available";
		case MotionEngineReason::SmoothMotion: return "libplacebo smooth motion owns output";
		case MotionEngineReason::VramRelease: return "video memory pressure";
		case MotionEngineReason::Resync: return "resync";
		case MotionEngineReason::Interlaced: return "interlaced source";
		case MotionEngineReason::VectorDemandOff: return "vector demand is off";
		}
		return "?";
	}

	// The facts the state is derived from (one frame; producer facts are PlanProducer's of the previous frame, see VectorEngineState.h).
	struct EngineFacts
	{
		bool vectorDemand = true;         // the vector_demand setting (off = the Original pathway)
		Decision decision;                // this frame's VectorDemand decision
		bool starterEngaged = false;      // a starter runs the engine (Input::starterEngaged or a served camera fit)
		bool opticalFlow = true;          // the optical_flow setting
		bool lowLatency = false;
		bool lowLatencyNoVectors = false; // low latency without the vectors path (competitive, field consumers)
		bool producerRunning = false;
		bool producerRecovering = false;
		bool nvofStartError = false;      // the producer's start failed (NVOF not available on this adapter / size)
		bool vramRelease = false;         // the idle keeper is pressured (MotionEngineIdle::Keeper::Pressured)
		bool resyncWindow = false;        // inside a resync window (vectors invalid)
		uint64_t frame = 0;
	};

	inline MotionEngineStatus DeriveEngineStatus(const EngineFacts& f)
	{
		MotionEngineStatus st;
		st.frame = f.frame;
		const Decision& d = f.decision;
		const unsigned starters = d.starters | (d.served & StarterMask());
		st.starters = f.starterEngaged ? starters : 0u;
		unsigned r = 0;
		const auto bit = [](MotionEngineReason x) { return 1u << static_cast<int>(x); };
		if (!f.vectorDemand)
			r |= bit(MotionEngineReason::VectorDemandOff);
		if (d.reason == Reason::Interlaced)
			r |= bit(MotionEngineReason::Interlaced);
		if (d.reason == Reason::SmoothMotionExclusive)
			r |= bit(MotionEngineReason::SmoothMotion);
		if (f.vramRelease)
			r |= bit(MotionEngineReason::VramRelease);
		if (f.lowLatency && f.lowLatencyNoVectors)
			r |= bit(MotionEngineReason::LowLatency);
		const bool hardOff = r != 0;
		if (!f.starterEngaged)
			r |= bit(MotionEngineReason::NoStarter);
		bool degraded = false;
		if (!hardOff && f.starterEngaged)
		{
			if (!f.opticalFlow)
			{
				r |= bit(MotionEngineReason::OpticalFlowOff);
				degraded = true;
			}
			if (f.nvofStartError)
			{
				r |= bit(MotionEngineReason::NvofUnavailable);
				degraded = true;
			}
			if (f.producerRecovering)
			{
				r |= bit(MotionEngineReason::ProducerRecovering);
				degraded = true;
			}
			if (d.reason == Reason::ForwardShallow)
			{
				r |= bit(MotionEngineReason::LowLatency);
				degraded = true;
			}
			if (f.resyncWindow)
			{
				r |= bit(MotionEngineReason::Resync);
				degraded = true;
			}
			if (!degraded && !f.producerRunning)
				degraded = true;   // a starter runs the matcher while the producer has not started yet (its first frame)
		}
		st.reasons = r;
		// A hard off (VRAM release, low latency without vectors ...) leaves the decision's served consumers without motion too: affected,
		// so the "motion engine" entry names them (never a silent fallback).
		st.affected = d.unserved | ((degraded || hardOff) ? d.served : 0u);
		st.state = (hardOff || !f.starterEngaged) ? MotionEngineState::Off : degraded ? MotionEngineState::Degraded : MotionEngineState::On;
		for (int i = 0; i < kMotionEngineReasonCount; ++i)
			if ((r & (1u << i)) != 0)
			{
				st.reason = static_cast<MotionEngineReason>(i);
				break;
			}
		return st;
	}
}
