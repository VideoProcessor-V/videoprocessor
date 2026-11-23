// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/motion/consumers/IntentMotionState.h>
#include <ContentIntentObserver.h>

namespace Vpv
{
	namespace IntentMotion
	{
		void Configure(MotionIntentState& state, const VpvRenderer::Fields& settings,
			const ContentIntent::Observer* contentIntent);

		double EaseBlackShare(MotionIntentState& state, double easeSeconds, double measured, bool haveMeasurement);
	}
}
