// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P11a-f (refs/110 s2.2, lead decision Q1): the ambient, black-intent and LED-limit parts of the intent
// interpreter items 2-5 (2026-10-17), out of LibplaceboVideoRenderer.cpp's Impl. Out-of-line member definitions
// of the renderer's Impl, included ONCE by LibplaceboVideoRenderer.cpp with the other module .inl files. Moved
// VERBATIM; only the moved state is name-qualified (`vpv.led.intent.x`, `vpv.motion.intent.x`; state in
// LedIntentState.h / IntentMotionState.h, reached through the S8 anchor `Vpv::HostState vpv`).
//   - ConfigureIntentInterpreter: the old entry, now calling the motion half (motion/consumers/
//     IntentMotionMethods.inl) and the ambient half below in the old order.
//   - ConfigureIntentAmbient: the second half of the old body (ambient settings, published modes).
//   - IntentTargetBlackNits, EvaluateIntentBlackDisplay: the old bodies.
// Render thread (RenderLocked, renderMutex held); intentMutex guards what the GUI thread also reads.
#pragma once

void LibplaceboVideoRenderer::Impl::ConfigureIntentInterpreter()
{
	ConfigureIntentMotion();
	ConfigureIntentAmbient();
}

void LibplaceboVideoRenderer::Impl::ConfigureIntentAmbient()
{
	const RendererSettings& s = activeSettings;
	AmbientAdapt::Settings ambient;
	ambient.enabled = s.ambientAdapt;
	ambient.strength = s.ambientAdaptStrength;
	ambient.screenReflectNitsPerLux = s.ambientScreenReflect;
	ambient.maxBlackNits = s.ambientMaxBlackNits;
	ambient.tauUpSeconds = s.ambientTauUpSeconds;
	ambient.tauDownSeconds = s.ambientTauDownSeconds;
	std::lock_guard<std::mutex> guard(vpv.led.intent.intentMutex);
	vpv.led.intent.intentBlackPublished = s.intentBlack;
	vpv.led.intent.intentDisplayPublished = s.intentDisplay;
	const AmbientAdapt::Settings& was = vpv.led.intent.ambientSettings;
	if (was.enabled != ambient.enabled || was.strength != ambient.strength ||
		was.screenReflectNitsPerLux != ambient.screenReflectNitsPerLux || was.maxBlackNits != ambient.maxBlackNits ||
		was.tauUpSeconds != ambient.tauUpSeconds || was.tauDownSeconds != ambient.tauDownSeconds)
	{
		vpv.led.intent.ambientSettings = ambient;
		vpv.led.intent.ambientFilter.Configure(ambient);
		DebugLog::Log("Ambient adaptation: %s strength=%.2f screen_reflect=%.4f cd/m2 per lux max_black=%.3f nits "
			"tau up %.0f s down %.0f s", ambient.enabled ? "on" : "off", ambient.strength,
			ambient.screenReflectNitsPerLux, ambient.maxBlackNits, ambient.tauUpSeconds, ambient.tauDownSeconds);
	}
}

// The black the frame renders to: the owner's sdr_black_nits exactly while
// ambient_adapt is off or its strength is 0 (bit-identical); otherwise the
// quantised B_eff, stepped only when the picture crosses a shot boundary (or by
// the filter's slew on a picture without cuts). Also moves the shot observer's
// display position (motion intent decides at the same cuts).
double LibplaceboVideoRenderer::Impl::IntentTargetBlackNits(uint64_t sourceSequence)
{
	vpv.motion.intent.intentInterpreter->AdvanceDisplay(sourceSequence);
	const uint64_t cuts = vpv.motion.intent.intentInterpreter->DisplayedCuts();
	const bool atCut = cuts != vpv.led.intent.ambientCutsSeen;
	vpv.led.intent.ambientCutsSeen = cuts;
	std::lock_guard<std::mutex> guard(vpv.led.intent.intentMutex);
	const AmbientAdapt::Settings& settings = vpv.led.intent.ambientSettings;
	if (!settings.enabled || !(settings.strength > 0.0))
	{
		vpv.led.intent.ambientApply.valid = false;
		vpv.led.intent.ambientAppliedBlackNits = 0.0;
		return sdrBlackNits;
	}
	const double before = vpv.led.intent.ambientApply.valid ? vpv.led.intent.ambientApply.applied : sdrBlackNits;
	const double now = static_cast<double>(GetTickCount64()) / 1000.0;
	const double black = IntentInterpreter::AmbientStep(vpv.led.intent.ambientApply, settings, vpv.led.intent.ambientFilter.Valid(),
		vpv.led.intent.ambientFilter.SmoothedLux(), sdrBlackNits, atCut, now);
	if (black != before)
		DebugLog::Log("Ambient adaptation: target black %.5f -> %.5f nits (%s; smoothed %.1f lux, owner black %.5f nits)",
			before, black, atCut ? "at a cut" : "slew on a static picture", vpv.led.intent.ambientFilter.SmoothedLux(), sdrBlackNits);
	vpv.led.intent.ambientAppliedBlackNits = black;
	vpv.motion.intent.intentInterpreter->SetAmbientField(IntentField<float>::Make(static_cast<float>(black),
		black != sdrBlackNits ? IntentSource::Inferred : IntentSource::Off));
	return black;
}

// Black protection (report / led_protect) for the shot on screen, the display
// mapping readout, and the LED's intent limits; per rendered frame, cheap
// (pure), logged on a change only.
void LibplaceboVideoRenderer::Impl::EvaluateIntentBlackDisplay(const SceneDescriptor& scene, const DisplayProfile& display)
{
	const BlackIntentMode mode = activeSettings.intentBlack;
	BlackIntentStatus black;
	black.mode = mode;
	double share = 0.0;
	if (mode != BlackIntentMode::Off)
	{
		const BlackIntent::ShadowStats shadows = vpv.motion.intent.intentInterpreter->DisplayedShadows();
		IntentField<float> l1;
		if (scene.sceneMin.present)
			l1 = IntentField<float>::Make(scene.sceneMin.nits, IntentSource::DvL1);
		black = BlackIntent::Evaluate(mode, shadows, l1, display.peakNits, display.blackNits);
		black.mode = mode;
		if (mode == BlackIntentMode::LedProtect)
		{
			share = EaseBlackShare(BlackIntent::LedMinimumShare(shadows, display.peakNits, display.blackNits),
				shadows.valid);
			black.ledMinimumShare = static_cast<float>(share);
		}
		else
			vpv.motion.intent.blackShareValid = false;
		const int logged = black.valid ? black.stepsBelow * 1000 + static_cast<int>(share * 100.0 + 0.5) : -2;
		if (logged != vpv.led.intent.blackIntentLogged)
		{
			vpv.led.intent.blackIntentLogged = logged;
			DebugLog::Log("Black intent: %s %s: %d of %d shadow steps below %.5f nits (display black %.5f, "
				"L1 min %s, p1/p5/p10 %.5f/%.5f/%.5f nits) LED minimum share %.3f",
				ToString(mode), black.valid ? "valid" : "no statistics", black.stepsBelow, black.stepsTotal,
				black.thresholdNits, display.blackNits,
				l1.valid ? std::to_string(l1.value).c_str() : "absent", shadows.p1Nits, shadows.p5Nits, shadows.p10Nits, share);
		}
		if (black.valid)
			vpv.motion.intent.intentInterpreter->SetBlackField(IntentField<float>::Make(static_cast<float>(black.stepsBelow),
				l1.valid ? IntentSource::DvL1 : IntentSource::Inferred));
	}
	else
		vpv.led.intent.blackIntentLogged = -1;
	DisplayIntentStatus mapping;
	if (activeSettings.intentDisplay)
	{
		mapping = DisplayIntent::Evaluate(true, scene, display, 0.0);
		mapping.enabled = true;
		if (vpv.led.intent.displayIntentLogged != mapping.line)
		{
			vpv.led.intent.displayIntentLogged = mapping.line;
			DebugLog::Log("Display intent: %s", mapping.line[0] ? mapping.line : "no scene maximum");
		}
		if (mapping.valid)
			vpv.motion.intent.intentInterpreter->SetDisplayField(IntentField<float>::Make(mapping.compressionStops,
				mapping.sceneMaxSource));
	}
	else
		vpv.led.intent.displayIntentLogged.clear();
	IntentInterpreter::LedIntentLimits limits;
	{
		std::lock_guard<std::mutex> guard(vpv.led.intent.intentMutex);
		limits.ambientBlackNits = vpv.led.intent.ambientAppliedBlackNits > sdrBlackNits ? vpv.led.intent.ambientAppliedBlackNits : 0.0;
		vpv.led.intent.blackIntentStatus = black;
		vpv.led.intent.displayIntentStatus = mapping;
	}
	limits.shadowMinimumShare = share;
	if (IntentInterpreter::LedLimitsDiffer(limits, vpv.led.intent.ledIntentLimitsSent) &&
		dynamicDrive.SendIntentLimits(limits.shadowMinimumShare, limits.ambientBlackNits, sdrBlackNits))
		vpv.led.intent.ledIntentLimitsSent = limits;
}
