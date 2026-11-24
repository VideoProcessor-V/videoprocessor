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
// ONE decision, DecideForHost: the engine host acts on it (VectorEngine::UpdateVectorEngine) and the pure tests pin the same
// function. Libplacebo smooth motion owns output exclusively (owner 2026-11-12 18:47: every non-synthesis consumer goes visibly
// without vectors, no producer) and competitive low latency is motion-free. Batch B fold (gate-union-b1 (i), owner 2026-11-15): the
// older Decide() semantics are retired with it (kAnalysisUnderSmoothMotion: source analysis kept under smooth motion; Mode::Backward:
// backward pairs at queue depth 0).
namespace VectorDemand
{
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

	// Batch B fold: Backward (the backward pairs of competitive low latency) is retired with them; Off and Forward keep their values.
	enum class Mode { Off, Forward };
	// W2-C1 (B9): NoStarter appended (no index moved). Batch B (motion engine switch): the starter gate is retired, so NoStarter is
	// retired, never derived; the value stays (append-only enum, tests and logs name it). NoPairs appended: statistics consumers
	// without optical-flow pairs (the matcher cannot make their whole-frame global). BackwardDepth0 (the name is historical; no index
	// moves): competitive low latency, no frame ahead: motion-free since the fold (mode Off, every consumer unserved).
	enum class Reason { Disabled, SlotsOff, NoConsumer, Interlaced, SmoothMotionExclusive, Forward, ForwardShallow, BackwardDepth0, NoStarter, NoPairs };

	// The motion engine is ONE ON/OFF switch (vector_demand, default ON; lead18 plan-motion-engine-switch.md). ON: every consumer that
	// registers a vector want is served (the starter gate of W2-C1 is gone). The matcher is the fallback of the ON engine while no
	// optical-flow pairs are expected (optical flow off, NVOF start failed, producer recovering).
	struct Input
	{
		bool enabled = false;
		bool slotsRun = false;
		bool lowLatency = false;
		int queueAheadCapacity = 0;
		bool smoothMotion = false;
		bool interlaced = false;
		bool wants[kConsumerCount] = {};
		// Previous-frame facts say optical-flow pairs will exist (optical_flow on, producer start not failed, not recovering,
		// motion_ahead_frames > 0). False: the matcher fallback serves the source-field consumers; the statistics consumers have no
		// matcher form and go visibly without vectors (Reason::NoPairs).
		bool pairsExpected = true;
		// No pairs because of a SETTING (optical_flow off, or motion_ahead_frames 0): deliberate, like OpticalFlowOff. The unserved
		// statistics consumers are still named in Ctrl+I (StatusLine / UnservedText) but are not a Fallbacks entry (plan (b), M4).
		bool pairsDeliberatelyOff = false;
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
		// Every consumer that registered a want (bit per Consumer, synthesis included), whether or not the engine is enabled: the OFF
		// status line names them ("motion engine off (setting): <consumers> use their non-motion path").
		unsigned wants = 0;
		// Statistics consumers left unserved because no optical-flow pairs are expected (visible: UnservedWhy).
		unsigned noPairs = 0;
		// The matcher serves the source-field consumers because no pairs are expected (the fallback of the ON engine).
		bool matcherFallback = false;
		// Copied from Input: the no-pairs cause is a setting, so FallbackCatalog::VectorEngine leaves `noPairs` consumers out.
		bool pairsDeliberatelyOff = false;
		bool Serves(Consumer c) const { return (served & (1u << static_cast<int>(c))) != 0; }
	};

	inline Consumer ConsumerAt(int i) { return static_cast<Consumer>(i); }

	// The consumer whose render-time matcher estimate runs even with pairs expected (its own P3 field): the camera fit.
	inline unsigned MatcherFieldMask() { return 1u << static_cast<int>(Consumer::CameraFit); }

	// THE decision (batch B fold, gate-union-b1 (i)): the engine host acts on it and the pure tests pin it (VectorDemandTests).
	// Libplacebo smooth motion owns output exclusively (plan (b)/(d4); owner 2026-11-12 18:47: VP's engine stays off and the dither
	// uses its own visible fallback search): every non-synthesis consumer unserved, no producer. Competitive low latency (no frame
	// ahead, queue capacity 0) is motion-free (doc 91 C / register 115 G6, WAIVER-C1-08): every consumer unserved with its reason,
	// mode Off and lowLatencyAllowed false, so DeriveEngineStatus reports hard Off / LowLatency. Every decision equals the host's
	// before the fold (exclusive smooth motion; the competitive backward decision turned Off), field for field.
	inline Decision DecideForHost(const Input& in)
	{
		Decision d;
		d.pairsDeliberatelyOff = in.pairsDeliberatelyOff;
		unsigned wanted = 0;   // non-synthesis wants
		for (int i = 0; i < kConsumerCount; ++i)
		{
			if (!in.wants[i])
				continue;
			d.wants |= 1u << i;
			if (DomainOf(ConsumerAt(i)) != Domain::Synthesis)
				wanted |= 1u << i;
		}
		if (!in.enabled)
			return d;
		if (!in.slotsRun)
		{
			d.reason = Reason::SlotsOff;
			return d;
		}
		d.reason = Reason::NoConsumer;
		if (d.wants == 0)
			return d;
		if (wanted == 0)
		{
			d.synthesisOnly = true;   // synthesis consumers decide for themselves (their own policies)
			return d;
		}
		d.smoothMotion = in.smoothMotion;
		if (in.interlaced)
		{
			d.unserved = wanted;
			d.reason = Reason::Interlaced;
			return d;
		}
		// Libplacebo smooth motion owns output exclusively: every non-synthesis consumer goes visibly without vectors, nothing starts.
		if (in.smoothMotion)
		{
			d.unserved = wanted;
			d.reason = Reason::SmoothMotionExclusive;
			return d;
		}
		unsigned served = wanted;
		// No optical-flow pairs expected (optical flow off, producer start failed or recovering): the statistics consumers read the
		// global of a producer pair, which the matcher cannot make. They go visibly without vectors; the others fall back to the matcher.
		unsigned statsServed = 0;
		for (int i = 0; i < kConsumerCount; ++i)
			if ((served & (1u << i)) != 0 && DomainOf(ConsumerAt(i)) == Domain::SourceStatistics)
				statsServed |= 1u << i;
		if (!in.pairsExpected && statsServed != 0)
		{
			d.noPairs = statsServed;
			d.unserved |= statsServed;
			served &= ~statsServed;
			if (served == 0)
			{
				d.reason = Reason::NoPairs;
				return d;
			}
		}
		// Low latency (owner 2026-11-01): vectors never ADD queue depth. A mode that queues no frame ahead (competitive, capacity 0)
		// is motion-free: every consumer still served here goes visibly without vectors (mode Off, served 0, no producer, no estimate,
		// lowLatencyAllowed false). A mode whose own queue holds frames ahead (games, capacity >= 1) uses those already-queued frames
		// (ForwardShallow, EngageAhead bounded by the capacity).
		if (in.lowLatency && in.queueAheadCapacity == 0)
		{
			d.unserved |= served;
			d.reason = Reason::BackwardDepth0;
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
		// The producer runs while any consumer is served. The render-time matcher estimate serves the camera fit always (it needs the
		// matcher's own field) and the other source-field / output consumers only while no pairs are expected (the matcher is the
		// fallback of the ON engine: it costs nothing while pairs exist).
		unsigned fieldMask = 0;
		for (int i = 0; i < kConsumerCount; ++i)
			if (DomainOf(ConsumerAt(i)) == Domain::SourceField || DomainOf(ConsumerAt(i)) == Domain::Output)
				fieldMask |= 1u << i;
		d.producer = served != 0;
		d.estimate = (served & fieldMask) != 0 && (!in.pairsExpected || (served & MatcherFieldMask()) != 0);
		d.matcherFallback = !in.pairsExpected && (served & fieldMask & ~MatcherFieldMask()) != 0;
		return d;
	}

	// The look-ahead depth the engine engages at: the configured depth bounded by the queue's
	// capacity, at least one frame.
	inline int EngageAhead(int configured, int queueAheadCapacity)
	{
		if (queueAheadCapacity <= 0)
			return 1;
		const int bounded = configured < queueAheadCapacity ? configured : queueAheadCapacity;
		return bounded > 1 ? bounded : 1;
	}

	// Why a consumer the engine cannot serve goes without vectors. DecideForHost leaves one unserved for exactly these causes:
	// interlaced, no optical-flow pairs, smooth motion (exclusive), competitive low latency (Reason::BackwardDepth0).
	inline const char* UnservedWhy(const Decision& d, Consumer c)
	{
		if (d.reason == Reason::Interlaced)
			return "interlaced";
		if ((d.noPairs & (1u << static_cast<int>(c))) != 0)
			return "no optical-flow pairs (optical flow off or not running)";
		if (d.reason == Reason::SmoothMotionExclusive)
			return "libplacebo smooth motion owns output";
		// Competitive low latency is motion-free (review b1-fin L1; was "needs a frame ahead", the retired backward-pair cause).
		return "competitive low latency (motion-free)";
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

	// Engine OFF by the setting (vector_demand off; Q2: a status line, NOT a Fallbacks-engaged entry; empty when no consumer wants
	// vectors, and the Original pathway never shows it): "motion engine off (setting): <consumers> use their non-motion path".
	inline std::string OffLine(const Decision& d)
	{
		if (d.wants == 0)
			return std::string();
		return "motion engine off (setting): " + NamesOf(d.wants) + " use their non-motion path";
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
		if (d.mode == Mode::Forward)
			s += d.matcherFallback ? "on (forward; matcher fallback, no optical-flow pairs)" : "on (forward)";
		else
			s += "off";
		if (d.served != 0)
			s += " for " + NamesOf(d.served);
		if (d.unserved != 0)
			s += "; " + UnservedText(d);
		return s;
	}
	// refs/109 F1: the status line plus the look-ahead depth while the ceiling binds. `cappedLead` = MotionAhead::kMaxLead when the lead
	// is held at it (VectorEngine: sustained, forward look-ahead), else 0 (the line above, unchanged). R3 (lead 12): at the owner's
	// ceiling this is the DESIGNED state, worded as information (the Ctrl+I line), not as a downgrade.
	inline std::string StatusLine(const Decision& d, int cappedLead)
	{
		std::string s = StatusLine(d);
		if (cappedLead > 0 && d.mode == Mode::Forward && !s.empty())
			s += "; look-ahead computes " + std::to_string(cappedLead) + " frames ahead (the ahead-compute ceiling; more are queued)";
		return s;
	}
	// The whole-frame motion statistics every source-statistics consumer reads (source monitor fast-pan,
	// motion intent), from one engine pair's global vector. One formula for the forward producer hit
	// and a render-time sample. `sourcePixels` = the global vector in source pixels
	// per frame (DejudderPolicy::EngineGlobalSourcePixels); z / w = the ambiguity and validity of
	// the engine's global (usable below 0.9, valid above 0.5).
	struct GlobalSample
	{
		bool usable = false;          // w > 0.5 && z < 0.9: the source monitor's fast-pan switch may use it
		bool intentValid = false;     // the intent interpreter may use it
		double sourcePixels = 0.0;
		double heightsPerFrame = 0.0; // picture heights per frame
	};
	inline GlobalSample SampleGlobal(double sourcePixels, float z, float w, int pictureHeight)
	{
		GlobalSample g;
		g.usable = w > 0.5f && z < 0.9f;   // GEM VE-GLOBAL: the weight test too (a failed FlowGlobal has z = 1; an unset one has w = 0)
		g.sourcePixels = sourcePixels;
		g.intentValid = w > 0.5f && z < 0.9f && pictureHeight > 0;
		g.heightsPerFrame = pictureHeight > 0 ? sourcePixels / pictureHeight : 0.0;
		return g;
	}

	// ---- W2-C1 (B9): the ONE global motion-engine state every consumer reads (IVectorEngine::State) --------------------------
	// Off = nothing wants vectors, or a hard absence; Degraded = the engine runs the matcher without NVOF pairs; On = a running
	// producer (batch B: the starter gate is retired, any registered want runs the engine). Pure data here (light header: VectorEngineInterface.h includes it); VectorEngine derives it once per frame
	// (DeriveEngineStatus below) from the Decision and the producer facts. Reasons are appended only (bit = 1 << reason).
	enum class MotionEngineState : uint8_t { On, Degraded, Off };
	enum class MotionEngineReason : uint8_t
	{
		None, NoStarter /* retired, never derived (batch B) */, OpticalFlowOff, LowLatency, ProducerRecovering, NvofUnavailable, SmoothMotion, VramRelease, Resync,
		Interlaced, VectorDemandOff, ProducerNotRunning
	};
	constexpr int kMotionEngineReasonCount = 12;

	struct MotionEngineStatus
	{
		MotionEngineState state = MotionEngineState::Off;
		MotionEngineReason reason = MotionEngineReason::None;   // the leading reason (the first in enum order of `reasons`)
		unsigned reasons = 0;    // bit (1 << MotionEngineReason) per reason that holds
		unsigned affected = 0;   // bit (1 << Consumer) per consumer without full motion: unserved, or served by a degraded engine
		uint64_t frame = 0;      // the frame the status was derived at
		bool Has(MotionEngineReason r) const { return (reasons & (1u << static_cast<int>(r))) != 0; }
	};

	// What the engine did (monotonic counters, render thread; zero-cost to read).
	struct MotionEngineWork
	{
		uint64_t matcherCreates = 0, matcherEstimates = 0, warmUps = 0, analysisEstimates = 0, producerStarts = 0, producerPairs = 0;
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
		case MotionEngineReason::NoStarter: return "no starter runs it (retired)";
		case MotionEngineReason::OpticalFlowOff: return "optical flow is off (the matcher runs alone)";
		case MotionEngineReason::LowLatency: return "low latency";
		case MotionEngineReason::ProducerRecovering: return "the optical-flow producer is recovering";
		case MotionEngineReason::NvofUnavailable: return "NVIDIA optical flow is not available";
		case MotionEngineReason::SmoothMotion: return "libplacebo smooth motion owns output";
		case MotionEngineReason::VramRelease: return "video memory pressure";
		case MotionEngineReason::Resync: return "resync";
		case MotionEngineReason::Interlaced: return "interlaced source";
		case MotionEngineReason::VectorDemandOff: return "vector demand is off";
		case MotionEngineReason::ProducerNotRunning: return "the optical-flow producer is not running";
		}
		return "?";
	}

	// The facts the state is derived from (one frame; producer facts are PlanProducer's of the previous frame, see VectorEngineState.h).
	struct EngineFacts
	{
		bool vectorDemand = true;         // the vector_demand setting (off = the Original pathway)
		Decision decision;                // this frame's VectorDemand decision
		bool opticalFlow = true;          // the optical_flow setting
		bool lowLatency = false;
		bool lowLatencyNoVectors = false; // low latency without the vectors path (competitive, field consumers)
		bool producerRunning = false;
		bool producerRecovering = false;
		bool nvofStartError = false;      // the producer's start failed (NVOF not available on this adapter / size)
		bool producerNotRunning = false;  // GEM-112 (VE-LIFECYCLE): a producer exists but is neither running nor recovering (e.g. async start failure)
		bool vramRelease = false;         // the idle keeper is pressured (MotionEngineIdle::Keeper::Pressured)
		bool resyncWindow = false;        // inside a resync window (vectors invalid)
		uint64_t frame = 0;
	};

	inline MotionEngineStatus DeriveEngineStatus(const EngineFacts& f)
	{
		MotionEngineStatus st;
		st.frame = f.frame;
		const Decision& d = f.decision;
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
		// Batch B: the engine runs while any consumer registered a want (NoStarter is never derived). Nothing wanting vectors: Off, no
		// reason, nothing affected.
		const bool wanted = d.wants != 0 && d.reason != Reason::SlotsOff;
		bool degraded = false;
		if (!hardOff && wanted)
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
			if (f.producerNotRunning)
			{
				r |= bit(MotionEngineReason::ProducerNotRunning);
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
				degraded = true;   // the matcher runs while the producer has not started yet (its first frame)
		}
		st.reasons = r;
		// A hard off (VRAM release, low latency without vectors ...) leaves the decision's served consumers without motion too: affected,
		// so the "motion engine" entry names them (never a silent fallback).
		st.affected = d.unserved | ((degraded || hardOff) ? d.served : 0u);
		st.state = (hardOff || !wanted) ? MotionEngineState::Off : degraded ? MotionEngineState::Degraded : MotionEngineState::On;
		for (int i = 0; i < kMotionEngineReasonCount; ++i)
			if ((r & (1u << i)) != 0)
			{
				st.reason = static_cast<MotionEngineReason>(i);
				break;
			}
		return st;
	}
}
