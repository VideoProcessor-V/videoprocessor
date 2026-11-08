// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>

#include "RendererAbiBridge.h"

#include <vprenderer/LibplaceboVideoRenderer.h>
#include <VP-V/abi/VpvAbi.h>

// VP-V extension ABI (M2 P2, D-P2b): the export the loader requires (VpvPluginGuard.h refuses a DLL without it or
// with another number). It lives here, in a VP-V file compiled into both renderer DLLs, so the host's export file
// stays Bill's; the name must equal Vpv::kVpvAbiExport (checked by PluginGuardTests against the built DLLs).
extern "C" __declspec(dllexport) uint32_t __cdecl
VideoProcessorLibplaceboVpvAbi()
{
	return Vpv::kVpvExtensionAbi;
}

namespace Vpv
{
	std::unique_ptr<RendererAbiBridge> MakeRendererAbiBridge(LibplaceboVideoRenderer& renderer)
	{
		return std::unique_ptr<RendererAbiBridge>(new RendererAbiBridgeT<LibplaceboVideoRenderer>(renderer));
	}
}
