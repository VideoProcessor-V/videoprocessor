// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/drive/led_compensation/DynamicDriveFrame.h>
#include <VP-V/modules/drive/led_compensation/LedIntentState.h>
#include <VP-V/modules/motion/consumers/IntentMotionState.h>

namespace Vpv
{
	namespace LedIntent
	{
		void ConfigureAmbient(LedIntentState& ledIntent, const VpvRenderer::Fields& settings, bool appearanceActive = false);

		double TargetBlackNits(LedIntentState& ledIntent, MotionIntentState& motionIntent,
			double sdrBlackNits, uint64_t sourceSequence);

		void EvaluateBlackDisplay(LedIntentState& ledIntent, MotionIntentState& motionIntent,
			const VpvRenderer::Fields& settings, double sdrBlackNits,
			LedDrive::DynamicDriveFrame& dynamicDrive,
			const SceneDescriptor& scene, const DisplayProfile& display);
	}
}
