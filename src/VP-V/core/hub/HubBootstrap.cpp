// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Explicit registration of the skeleton hub (M2 D-g). This translation unit is kept by
// /INCLUDE:VpvHubBootstrapAnchor in the renderer projects' items props, so the registration cannot be
// dropped by the linker and costs no line in the host files. It runs at module load, before any
// renderer, capture or file thread exists.

#include "ExtensionHub.h"

#include <VP-V/modules/input/signal/ResyncExtension.h>

namespace
{
	int RegisterAtLoad()
	{
		// Fixed fan-out order (doc 63 section 1): resync / FrameSeal first, so every later module
		// sees the frame already sealed.
		Vpv::Resync::RegisterResyncExtension();
		Vpv::Hub::RegisterHub();
		return 1;
	}
}

extern "C" int VpvHubBootstrapAnchor = RegisterAtLoad();
