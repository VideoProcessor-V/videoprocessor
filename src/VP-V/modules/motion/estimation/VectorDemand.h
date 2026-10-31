// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
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

	enum class Consumer : int
	{
		DejudderExtras, Interpolator, HiccupRepair, OcclusionMask,
		SigmaDelta, GrainMap, DebandHold, DetailPulse, EntranceSegment,
		SourceMonitorPan, MotionIntent
	};
	constexpr int kConsumerCount = 11;

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
		case Consumer::EntranceSegment: return Domain::SourceField;
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
		}
		return "?";
	}

	enum class Mode { Off, Forward, Backward };
	enum class Reason { Disabled, SlotsOff, NoConsumer, Interlaced, SmoothMotionExclusive, Forward, ForwardShallow, BackwardDepth0 };

	struct Input
	{
		bool enabled = false;
		bool slotsRun = false;
		bool lowLatency = false;
		int queueAheadCapacity = 0;
		bool smoothMotion = false;
		bool interlaced = false;
		bool wants[kConsumerCount] = {};
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
		bool Serves(Consumer c) const { return (served & (1u << static_cast<int>(c))) != 0; }
	};

	inline Consumer ConsumerAt(int i) { return static_cast<Consumer>(i); }

	inline Decision DecideWith(const Input& in, bool analysisUnderSmoothMotion)
	{
		Decision d;
		if (!in.enabled)
			return d;
		if (!in.slotsRun)
		{
			d.reason = Reason::SlotsOff;
			return d;
		}
		bool anyWant = false;
		unsigned wanted = 0;   // non-synthesis wants
		for (int i = 0; i < kConsumerCount; ++i)
		{
			if (!in.wants[i])
				continue;
			anyWant = true;
			if (DomainOf(ConsumerAt(i)) != Domain::Synthesis)
				wanted |= 1u << i;
		}
		d.reason = Reason::NoConsumer;
		if (!anyWant)
			return d;
		if (wanted == 0)
		{
			d.synthesisOnly = true;
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
			d.producer = stats != 0;
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
		d.estimate = served != 0;
		d.producer = served != 0;
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
}
