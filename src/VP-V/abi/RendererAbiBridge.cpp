// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>

#include "RendererAbiBridge.h"

#include <vprenderer/LibplaceboVideoRenderer.h>
#include <VP-V/abi/VpvAbi.h>
#include <VP-V/abi/VpvAttachGate.h>

#include <DebugLog.h>

// VP-V extension ABI (M2 P2, D-P2b): the export the loader requires (VpvPluginGuard.h refuses a DLL without it or
// with another number). It lives here, in a VP-V file compiled into both renderer DLLs, so the host's export file
// stays Bill's; the name must equal Vpv::kVpvAbiExport (checked by PluginGuardTests against the built DLLs).
extern "C" __declspec(dllexport) uint32_t __cdecl
VideoProcessorLibplaceboVpvAbi()
{
	return Vpv::kVpvExtensionAbi;
}

namespace
{
	Vpv::AttachGate g_attachGate;   // this DLL instance's handshake state
}

// F1 of the P2 opus gate: the attach export. A VP loader calls it with kVpvExtensionAbi after the pairing checks;
// until then VideoProcessorLibplaceboCreateRenderer returns nullptr (a host that never attaches, such as Bill's, gets
// its own visible "failed to create a renderer").
extern "C" __declspec(dllexport) bool __cdecl
VideoProcessorLibplaceboVpvAttach(uint32_t abi)
{
	try
	{
		const bool accepted = g_attachGate.Attach(abi);
		DebugLog::Log("VP-V extension attach: abi=%u expected=%u %s", abi, Vpv::kVpvExtensionAbi, accepted ? "accepted" : "refused");
		return accepted;
	}
	catch (...)
	{
		return false;
	}
}

namespace Vpv
{
	bool AttachedForCreate()
	{
		if (g_attachGate.IsAttached())
			return true;
		DebugLog::Log("VP Renderer factory refused: the host did not attach with the VP-V extension ABI (%u); "
			"this DLL's layouts are not Bill's, so it builds no renderer for a host that does not know them", kVpvExtensionAbi);
		return false;
	}

	std::unique_ptr<RendererAbiBridge> MakeRendererAbiBridge(LibplaceboVideoRenderer& renderer)
	{
		return std::unique_ptr<RendererAbiBridge>(new RendererAbiBridgeT<LibplaceboVideoRenderer>(renderer));
	}
}
