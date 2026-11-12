// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P11a-f (refs/110 s2.2, lead decision Q1): the motion-intent part of the intent interpreter items 2-5
// (2026-10-17), out of LibplaceboVideoRenderer.cpp's Impl. Out-of-line member definitions of the renderer's
// Impl, included ONCE by LibplaceboVideoRenderer.cpp with the other module .inl files. Moved VERBATIM; only the
// moved state is name-qualified (`vpv.motion.intent.x`, `vpv.led.intent.x`; state in IntentMotionState.h /
// LedIntentState.h, reached through the S8 anchor `Vpv::HostState vpv`).
//   - ConfigureIntentMotion: the first half of the old ConfigureIntentInterpreter (the shot observer's
//     switches, motion settings and content field). The wrapper ConfigureIntentInterpreter and the ambient half
//     are in drive/led_compensation/LedIntentMethods.inl.
//   - EaseBlackShare: led_protect's fade model (ShotEase), the old body.
// Render thread (RenderLocked, renderMutex held).
#pragma once

// ---- Intent interpreter items 2-5 (2026-10-17) ----
// The switches and settings from the active profile (render loop, per frame;
// each consumer logs only on a change).
void LibplaceboVideoRenderer::Impl::ConfigureIntentMotion()
{
	const RendererSettings& s = activeSettings;
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
	vpv.motion.intent.intentInterpreter->Configure(switches, motion);
	if (switches.Any())
	{
		const ContentIntentStatus content = contentIntent->Status();
		vpv.motion.intent.intentInterpreter->SetContentField(IntentField<ContentClass>::Make(content.content,
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
double LibplaceboVideoRenderer::Impl::EaseBlackShare(double measured, bool haveMeasurement)
{
	const double ease = activeSettings.intentEaseSeconds;
	if (!(ease > 0.0))
	{
		vpv.motion.intent.blackShareValid = false;
		return measured;
	}
	const uint64_t shot = vpv.motion.intent.intentInterpreter->DisplayedShotStart();
	if (!vpv.motion.intent.blackShareValid || shot != vpv.motion.intent.blackShareShot)
	{
		vpv.motion.intent.blackShareShot = shot;
		if (haveMeasurement || !vpv.motion.intent.blackShareValid)
			vpv.motion.intent.blackShareApplied = measured;   // provisional at the cut (carried over without statistics)
		vpv.motion.intent.blackShareValid = true;
		vpv.motion.intent.blackShareEase.Start(ease);
		vpv.motion.intent.blackShareTime = std::chrono::steady_clock::now();
		return vpv.motion.intent.blackShareApplied;
	}
	// Display time since the previous rendered frame (wall clock: repeats and
	// extra refreshes add no source frames but do pass time on screen).
	const auto nowTime = std::chrono::steady_clock::now();
	const double dt = (std::min)(0.25, (std::max)(0.0,
		std::chrono::duration<double>(nowTime - vpv.motion.intent.blackShareTime).count()));
	vpv.motion.intent.blackShareTime = nowTime;
	vpv.motion.intent.blackShareApplied = vpv.motion.intent.blackShareEase.Step(vpv.motion.intent.blackShareApplied, measured, 1.0, dt, haveMeasurement);
	return vpv.motion.intent.blackShareApplied;
}
