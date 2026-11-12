// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P11a-d (refs/110 s2.4): the ambient SDR black-lift state of the renderer (Hunt 3 F2), moved out of
// LibplaceboVideoRenderer.cpp's Impl. It is `Vpv::HostState::Led::SdrLift` (core/hub/HostState.h), reached from
// the Impl code as `vpv.led.sdrLift.x`. Data only; the members keep their old names, types and initial values.
// This frame's BT.1886 black lift for SDR input (applied through the frame's colour adjustment; invalid = nothing
// changes). Render thread. The behaviour is in LedSdrLiftMethods.inl; the reader is segments/CoreSegments.inl.
// C++14 only.

#include <string>

namespace Vpv
{
	struct LedSdrLiftState
	{
		bool ambientSdrLift = false;
		float ambientSdrContrast = 1.0f, ambientSdrBrightness = 0.0f;
		std::string ambientSdrLogged;
	};
}
