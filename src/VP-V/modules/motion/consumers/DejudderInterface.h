// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-2 (doc 59 section 2.2 "Interfaces delivered"): what the de-judder module offers its consumers
// (P5c's extras, P6's render-ahead, K8's camera field choice) instead of renderer aliases.
//
// The hub's Query carries no host, so Query("vpv.dejudder.v1") answers an IDejudderDirectory*; a consumer
// asks it for its own renderer's IDejudder / IPhaseTimeline (null: that renderer did not register). The
// views read the renderer's DejudderPolicyState; render thread only (as the state). C++14.

#include <cstdint>

// The reason type only (an opaque declaration: this header stays free of DejudderPolicy.h, whose std::min/max
// forms a windows.h without NOMINMAX breaks in the hub module translation units).
namespace DejudderPolicy { enum class Reason; }

namespace VpMotion
{
	constexpr const char* kDejudderQueryId = "vpv.dejudder.v1";
	constexpr const char* kPhaseTimelineQueryId = "vpv.phase_timeline.v1";

	class IDejudder
	{
	public:
		virtual ~IDejudder() {}
		virtual DejudderPolicy::Reason Reason() const = 0;
		virtual float EffectiveStrength() const = 0;
		virtual int EvenCycle() const = 0;
		virtual bool ExtrasAllowed() const = 0;
		virtual bool Interpolating() const = 0;
		virtual uint64_t EstimatedSeq() const = 0;     // the source frame whose (A, B) pair was estimated
		virtual void Hold(const char* why) = 0;         // the de-judder hold (log, diagnostics, render-ahead line)
		// K8 (C4) insertion point: which field the extras take for this frame. 0 = today's (the live
		// estimate's field); no other value is defined yet.
		virtual int FieldChoice() const = 0;
	};

	class IPhaseTimeline
	{
	public:
		virtual ~IPhaseTimeline() {}
		virtual bool Active() const = 0;       // this source frame is presented by the per-vsync timeline
		virtual bool SkipOutput() const = 0;   // no refresh falls in this frame's interval
		virtual double OutputPhase() const = 0;   // the phase of the refresh being drawn
		virtual double ContentRateHz() const = 0; // the content rate the timeline schedules with
	};

	class IDejudderDirectory
	{
	public:
		virtual ~IDejudderDirectory() {}
		virtual IDejudder* DejudderFor(const void* host) = 0;
		virtual IPhaseTimeline* PhaseTimelineFor(const void* host) = 0;
	};
}
