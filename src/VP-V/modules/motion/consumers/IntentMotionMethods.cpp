// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "IntentMotionMethods.h"

#include <algorithm>
#include <chrono>

namespace Vpv
{
	namespace IntentMotion
	{
		// ---- Intent interpreter items 2-5 (2026-10-17) ----
		// The switches and settings from the active profile (render loop, per frame;
		// each consumer logs only on a change).
		void Configure(MotionIntentState& state, const VpvRenderer::Fields& s,
			const ContentIntent::Observer* contentIntent)
		{
			IntentInterpreter::Switches switches;
			switches.content = s.intentContent;
			switches.motion = s.intentMotion;
			switches.ambient = s.ambientAdapt;
			switches.black = s.intentBlack != BlackIntentMode::Off;
			switches.display = s.intentDisplay;
			IntentInterpreter::MotionSettings motion;
			motion.policy = s.intentMotionPolicy;
			motion.minScale = s.intentMotionMin;
			motion.minShotSeconds = s.intentMotionMinShotSeconds;
			motion.easeSeconds = s.intentEaseSeconds;
			motion.rolloffOn = s.dejudderMotionRolloff;
			state.intentInterpreter->Configure(switches, motion);
			if (switches.Any() && contentIntent)
			{
				const ContentIntentStatus content = contentIntent->Status();
				state.intentInterpreter->SetContentField(IntentField<ContentClass>::Make(content.content,
					content.content == ContentClass::Unknown ? IntentSource::Off : content.source));
			}
		}

		// led_protect's share under the fade model (intent_ease_s; owner decision
		// 2026-10-17). At the shot's first displayed frame: the share measured from
		// the queued frames when they gave statistics, otherwise the previous share
		// carried over; over the first intent_ease_s it follows the shot's newer
		// measurements rate-limited (the full 0..1 range per intent_ease_s), then it
		// holds for the rest of the shot. intent_ease_s 0: the measured share at once
		// (as before).
		double EaseBlackShare(MotionIntentState& state, double easeSeconds, double measured, bool haveMeasurement)
		{
			if (!(easeSeconds > 0.0))
			{
				state.blackShareValid = false;
				return measured;
			}
			const uint64_t shot = state.intentInterpreter->DisplayedShotStart();
			if (!state.blackShareValid || shot != state.blackShareShot)
			{
				state.blackShareShot = shot;
				if (haveMeasurement || !state.blackShareValid)
					state.blackShareApplied = measured;   // provisional at the cut (carried over without statistics)
				state.blackShareValid = true;
				state.blackShareEase.Start(easeSeconds);
				state.blackShareTime = std::chrono::steady_clock::now();
				return state.blackShareApplied;
			}
			// Display time since the previous rendered frame (wall clock: repeats and
			// extra refreshes add no source frames but do pass time on screen).
			const auto nowTime = std::chrono::steady_clock::now();
			const double dt = (std::min)(0.25, (std::max)(0.0,
				std::chrono::duration<double>(nowTime - state.blackShareTime).count()));
			state.blackShareTime = nowTime;
			state.blackShareApplied = state.blackShareEase.Step(state.blackShareApplied, measured, 1.0, dt, haveMeasurement);
			return state.blackShareApplied;
		}
	}
}
