// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the v1 interim GLSL mirror was deleted in S18 F1 (edge-map-only v2 law;
// no GPU form of kFocusMap is registered yet).
#pragma once

namespace FocusMap
{
	constexpr bool kHasGpuMirror = false;
}
