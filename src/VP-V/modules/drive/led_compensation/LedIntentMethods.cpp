// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "LedIntentMethods.h"

#include <DebugLog.h>
#include <VP-V/modules/motion/consumers/IntentMotionMethods.h>

#include <string>

namespace Vpv
{
	namespace LedIntent
	{
		void ConfigureAmbient(LedIntentState& ledIntent, const VpvRenderer::Fields& s)
		{
			AmbientAdapt::Settings ambient;
			ambient.enabled = s.ambientAdapt;
			ambient.strength = s.ambientAdaptStrength;
			ambient.screenReflectNitsPerLux = s.ambientScreenReflect;
			ambient.maxBlackNits = s.ambientMaxBlackNits;
			ambient.tauUpSeconds = s.ambientTauUpSeconds;
			ambient.tauDownSeconds = s.ambientTauDownSeconds;
			std::lock_guard<std::mutex> guard(ledIntent.intentMutex);
			ledIntent.intentBlackPublished = s.intentBlack;
			ledIntent.intentDisplayPublished = s.intentDisplay;
			const AmbientAdapt::Settings& was = ledIntent.ambientSettings;
			if (was.enabled != ambient.enabled || was.strength != ambient.strength ||
				was.screenReflectNitsPerLux != ambient.screenReflectNitsPerLux || was.maxBlackNits != ambient.maxBlackNits ||
				was.tauUpSeconds != ambient.tauUpSeconds || was.tauDownSeconds != ambient.tauDownSeconds)
			{
				ledIntent.ambientSettings = ambient;
				ledIntent.ambientFilter.Configure(ambient);
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
		double TargetBlackNits(LedIntentState& ledIntent, MotionIntentState& motionIntent,
			double sdrBlackNits, uint64_t sourceSequence)
		{
			motionIntent.intentInterpreter->AdvanceDisplay(sourceSequence);
			const uint64_t cuts = motionIntent.intentInterpreter->DisplayedCuts();
			const bool atCut = cuts != ledIntent.ambientCutsSeen;
			ledIntent.ambientCutsSeen = cuts;
			std::lock_guard<std::mutex> guard(ledIntent.intentMutex);
			const AmbientAdapt::Settings& settings = ledIntent.ambientSettings;
			if (!settings.enabled || !(settings.strength > 0.0))
			{
				ledIntent.ambientApply.valid = false;
				ledIntent.ambientAppliedBlackNits = 0.0;
				return sdrBlackNits;
			}
			const double before = ledIntent.ambientApply.valid ? ledIntent.ambientApply.applied : sdrBlackNits;
			const double now = static_cast<double>(GetTickCount64()) / 1000.0;
			const double black = IntentInterpreter::AmbientStep(ledIntent.ambientApply, settings, ledIntent.ambientFilter.Valid(),
				ledIntent.ambientFilter.SmoothedLux(), sdrBlackNits, atCut, now);
			if (black != before)
				DebugLog::Log("Ambient adaptation: target black %.5f -> %.5f nits (%s; smoothed %.1f lux, owner black %.5f nits)",
					before, black, atCut ? "at a cut" : "slew on a static picture", ledIntent.ambientFilter.SmoothedLux(), sdrBlackNits);
			ledIntent.ambientAppliedBlackNits = black;
			motionIntent.intentInterpreter->SetAmbientField(IntentField<float>::Make(static_cast<float>(black),
				black != sdrBlackNits ? IntentSource::Inferred : IntentSource::Off));
			return black;
		}

		// Black protection (report / led_protect) for the shot on screen, the display
		// mapping readout, and the LED's intent limits; per rendered frame, cheap
		// (pure), logged on a change only.
		void EvaluateBlackDisplay(LedIntentState& ledIntent, MotionIntentState& motionIntent,
			const VpvRenderer::Fields& activeSettings, double sdrBlackNits,
			LedDrive::DynamicDriveFrame& dynamicDrive,
			const SceneDescriptor& scene, const DisplayProfile& display)
		{
			const BlackIntentMode mode = activeSettings.intentBlack;
			BlackIntentStatus black;
			black.mode = mode;
			double share = 0.0;
			if (mode != BlackIntentMode::Off)
			{
				const BlackIntent::ShadowStats shadows = motionIntent.intentInterpreter->DisplayedShadows();
				IntentField<float> l1;
				if (scene.sceneMin.present)
					l1 = IntentField<float>::Make(scene.sceneMin.nits, IntentSource::DvL1);
				black = BlackIntent::Evaluate(mode, shadows, l1, display.peakNits, display.blackNits);
				black.mode = mode;
				if (mode == BlackIntentMode::LedProtect)
				{
					share = IntentMotion::EaseBlackShare(motionIntent, activeSettings.intentEaseSeconds,
						BlackIntent::LedMinimumShare(shadows, display.peakNits, display.blackNits), shadows.valid);
					black.ledMinimumShare = static_cast<float>(share);
				}
				else
					motionIntent.blackShareValid = false;
				const int logged = black.valid ? black.stepsBelow * 1000 + static_cast<int>(share * 100.0 + 0.5) : -2;
				if (logged != ledIntent.blackIntentLogged)
				{
					ledIntent.blackIntentLogged = logged;
					DebugLog::Log("Black intent: %s %s: %d of %d shadow steps below %.5f nits (display black %.5f, "
						"L1 min %s, p1/p5/p10 %.5f/%.5f/%.5f nits) LED minimum share %.3f",
						ToString(mode), black.valid ? "valid" : "no statistics", black.stepsBelow, black.stepsTotal,
						black.thresholdNits, display.blackNits,
						l1.valid ? std::to_string(l1.value).c_str() : "absent", shadows.p1Nits, shadows.p5Nits, shadows.p10Nits, share);
				}
				if (black.valid)
					motionIntent.intentInterpreter->SetBlackField(IntentField<float>::Make(static_cast<float>(black.stepsBelow),
						l1.valid ? IntentSource::DvL1 : IntentSource::Inferred));
			}
			else
				ledIntent.blackIntentLogged = -1;
			DisplayIntentStatus mapping;
			if (activeSettings.intentDisplay)
			{
				mapping = DisplayIntent::Evaluate(true, scene, display, 0.0);
				mapping.enabled = true;
				if (ledIntent.displayIntentLogged != mapping.line)
				{
					ledIntent.displayIntentLogged = mapping.line;
					DebugLog::Log("Display intent: %s", mapping.line[0] ? mapping.line : "no scene maximum");
				}
				if (mapping.valid)
					motionIntent.intentInterpreter->SetDisplayField(IntentField<float>::Make(mapping.compressionStops,
						mapping.sceneMaxSource));
			}
			else
				ledIntent.displayIntentLogged.clear();
			IntentInterpreter::LedIntentLimits limits;
			{
				std::lock_guard<std::mutex> guard(ledIntent.intentMutex);
				limits.ambientBlackNits = ledIntent.ambientAppliedBlackNits > sdrBlackNits ? ledIntent.ambientAppliedBlackNits : 0.0;
				ledIntent.blackIntentStatus = black;
				ledIntent.displayIntentStatus = mapping;
			}
			limits.shadowMinimumShare = share;
			if (IntentInterpreter::LedLimitsDiffer(limits, ledIntent.ledIntentLimitsSent) &&
				dynamicDrive.SendIntentLimits(limits.shadowMinimumShare, limits.ambientBlackNits, sdrBlackNits))
				ledIntent.ledIntentLimitsSent = limits;
		}
	}
}
