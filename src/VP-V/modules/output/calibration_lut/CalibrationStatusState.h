// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P11a-e (refs/110 s2.5): the calibration status the renderer reports, moved out of LibplaceboVideoRenderer.cpp's
// Impl as `Vpv::HostState::Calibration` (core/hub/HostState.h), reached as `vpv.calibration.x`. Data only; names,
// types and initial values unchanged. Render thread writes; the status snapshot reads it.
// C++14 only.

#include <vprenderer/LibplaceboOutputPolicy.h>

namespace Vpv
{
	struct CalibrationStatusState
	{
		bool calibrationStatusAvailable = false;
		LibplaceboOutput::SdrTransfer lastRenderedTargetTransfer = LibplaceboOutput::SdrTransfer::UNKNOWN;
		bool calibrationLutAttached = false;
	};
}
