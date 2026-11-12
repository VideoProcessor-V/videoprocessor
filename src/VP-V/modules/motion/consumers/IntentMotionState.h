// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P11a-f (refs/110 s2.2): the motion-intent state of the renderer's intent interpreter items 2-5
// (2026-10-17), moved out of LibplaceboVideoRenderer.cpp's Impl. It is `Vpv::HostState::Motion::Intent`
// (core/hub/HostState.h), reached from the Impl code as `vpv.motion.intent.x`. Data only; the members keep
// their old names, types, initial values and thread notes. Defined: the shot observer of the queued-frame
// walk (idle while every intent function is off) and led_protect's fade model (ShotEase) with its share.
// The behaviour is in IntentMotionMethods.inl (ConfigureIntentMotion, EaseBlackShare).
// C++14 only.

#include <IntentInterpreterObserver.h>
#include <MotionIntent.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace Vpv
{
	struct MotionIntentState
	{
		const std::shared_ptr<IntentInterpreter::Observer> intentInterpreter =
			std::make_shared<IntentInterpreter::Observer>();
		MotionIntent::ShotEase blackShareEase;                // render thread
		double blackShareApplied = 0.0;                       // render thread
		uint64_t blackShareShot = 0;                          // render thread
		bool blackShareValid = false;                         // render thread
		std::chrono::steady_clock::time_point blackShareTime;  // render thread
	};
}
