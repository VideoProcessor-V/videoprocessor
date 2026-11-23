// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/modules/drive/led_compensation/LedSdrLiftState.h>

#include <libplacebo/colorspace.h>

namespace Vpv
{
	namespace LedSdrLift
	{
		void UpdateAmbientSdrLift(LedSdrLiftState& sdrLift, double sdrBlackNits, double sdrTargetNits,
			double renderBlackNits, bool inputIsSdr, enum pl_color_transfer transfer);

		double DisplayGamma(enum pl_color_transfer transfer);
	}
}
