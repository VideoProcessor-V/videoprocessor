// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VP-V side of the renderer settings extension point: the host header
// vprenderer/RendererSettingsExtension.h includes this file when the H7 props define
// RENDERER_SETTINGS_EXTENSION_HEADER, and the host struct RendererSettings derives from the type below.

#include "VpvRendererFields.h"

struct RendererSettingsExtension : VpvRenderer::Fields {};
