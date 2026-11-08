// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>

#include "RendererAbiBridge.h"

#include <vprenderer/LibplaceboVideoRenderer.h>

namespace Vpv
{
	std::unique_ptr<RendererAbiBridge> MakeRendererAbiBridge(LibplaceboVideoRenderer& renderer)
	{
		return std::unique_ptr<RendererAbiBridge>(new RendererAbiBridgeT<LibplaceboVideoRenderer>(renderer));
	}
}
