// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P11a-f (refs/110 s2.2): the ambient, black-intent and LED-limit state of the renderer's intent interpreter
// items 2-5 (2026-10-17), moved out of LibplaceboVideoRenderer.cpp's Impl. It is `Vpv::HostState::Led::Intent`
// (core/hub/HostState.h), reached from the Impl code as `vpv.led.intent.x`. Data only; the members keep their
// old names, types, initial values and thread notes. intentMutex guards the ambient filter and the published
// statuses (SetAmbientLux and GetIntentStatus come from the GUI thread). The behaviour is in
// LedIntentMethods.inl (ConfigureIntentInterpreter, ConfigureIntentAmbient, IntentTargetBlackNits,
// EvaluateIntentBlackDisplay) and control/diagnostics/StatusMethods.inl (GetIntentStatus, SetAmbientLux).
// C++14 only.

#include <BlackIntent.h>
#include <DisplayIntent.h>
#include <IntentInterpreterSupport.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace Vpv
{
	struct LedIntentState
	{
		mutable std::mutex intentMutex;
		AmbientAdapt::Settings ambientSettings;               // intentMutex
		AmbientAdapt::Filter ambientFilter;                   // intentMutex
		double ambientAppliedBlackNits = 0.0;                 // intentMutex: B_eff rendered now (0 = off)
		BlackIntentStatus blackIntentStatus;                  // intentMutex
		DisplayIntentStatus displayIntentStatus;              // intentMutex
		BlackIntentMode intentBlackPublished = BlackIntentMode::Off;   // intentMutex
		bool intentDisplayPublished = false;                  // intentMutex
		IntentInterpreter::AmbientApplyState ambientApply;    // render thread
		uint64_t ambientCutsSeen = 0;                         // render thread
		IntentInterpreter::LedIntentLimits ledIntentLimitsSent;   // render thread
		int blackIntentLogged = -1;                           // render thread: steps below last logged
		std::string displayIntentLogged;                      // render thread
	};
}
