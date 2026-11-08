// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-2: the IDejudder / IPhaseTimeline views over the renderer's DejudderPolicyState (members of the
// renderer's Impl, registered through DejudderHostBinding). Read-only views apart from Hold, which calls
// the renderer's DejudderHold. Render thread only. C++14.

#include <VP-V/modules/motion/consumers/DejudderInterface.h>
#include <VP-V/modules/motion/consumers/DejudderState.h>

#include <cstdint>

namespace VpMotion
{
	using DejudderHoldFn = void (*)(void* self, const char* why);

	class DejudderView : public IDejudder
	{
	public:
		DejudderView(const DejudderPolicyState& state, const uint64_t& estimatedSequence, DejudderHoldFn hold, void* self)
			: state_(state), estimated_(estimatedSequence), hold_(hold), self_(self) {}
		DejudderPolicy::Reason Reason() const override { return state_.dejudderReason; }
		float EffectiveStrength() const override { return state_.dejudderEffectiveStrength; }
		int EvenCycle() const override { return state_.dejudderEvenCycle; }
		bool ExtrasAllowed() const override { return state_.dejudderExtrasAllowed; }
		bool Interpolating() const override { return state_.interpolating; }
		uint64_t EstimatedSeq() const override { return estimated_; }
		void Hold(const char* why) override { if (hold_) hold_(self_, why); }
		int FieldChoice() const override { return 0; }

	private:
		const DejudderPolicyState& state_;
		const uint64_t& estimated_;
		DejudderHoldFn hold_;
		void* self_;
	};

	class PhaseTimelineView : public IPhaseTimeline
	{
	public:
		explicit PhaseTimelineView(const DejudderPolicyState& state) : state_(state) {}
		bool Active() const override { return state_.vpTimelineActive; }
		bool SkipOutput() const override { return state_.vpSkipOutput; }
		double OutputPhase() const override { return state_.vpOutputPhase; }
		double ContentRateHz() const override { return state_.vpCaptureRateHz; }

	private:
		const DejudderPolicyState& state_;
	};
}
