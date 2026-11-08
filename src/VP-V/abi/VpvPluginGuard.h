// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Load-time pairing check of the VP-V extension ABI (M2 P2, D-P2b).
//
// VP_LIBPLACEBO_PLUGIN_API_VERSION is Bill's number again, so it no longer tells a VP-V renderer DLL from a Bill
// DLL or from a VP-V DLL of another extension ABI. The loader (the executable's proxy and the DirectShow
// filter's plugin loader) therefore also requires the export VideoProcessorLibplaceboVpvAbi (VpvAbi.h) and
// that it returns kVpvExtensionAbi. A missing or different export refuses the plugin with a visible reason,
// the same path as an API mismatch ("Redeploy VideoProcessor.exe and the vprenderer folders together").
//
// Attach (below) is the second step: the DLL refuses to create renderers until a VP loader attached with the number.
// Check does not log; the caller shows or logs `reason`. C++14, header-only.

#include <VP-V/abi/VpvAbi.h>

#include <windows.h>

#include <string>

namespace Vpv
{
	namespace PluginGuard
	{
		// The decision on the export function (null: the plugin has no such export). Pure; testable without a DLL.
		inline bool CheckAbiFunction(VpvAbiFn function, std::string& reason)
		{
			if (!function)
			{
				reason = "VP Renderer DLL refused: it lacks the VP-V extension ABI export (" +
					std::string(kVpvAbiExport) + "), so it is not from this build line. Redeploy "
					"VideoProcessor.exe and the vprenderer folders together from the same build.";
				return false;
			}
			const uint32_t actual = function();
			if (actual != kVpvExtensionAbi)
			{
				reason = "VP Renderer DLL refused: VP-V extension ABI " + std::to_string(actual) +
					", VideoProcessor.exe expects " + std::to_string(kVpvExtensionAbi) +
					". Redeploy VideoProcessor.exe and the vprenderer folders together from the same build.";
				return false;
			}
			reason.clear();
			return true;
		}

		// The attach step (after Check): the DLL builds no renderer for a host that has not attached with the
		// matching number. Pure decision on the export function; null = the plugin lacks it.
		inline bool AttachFunction(VpvAttachFn function, std::string& reason)
		{
			if (!function)
			{
				reason = "VP Renderer DLL refused: it lacks the VP-V attach export (" + std::string(kVpvAttachExport) +
					"). Redeploy VideoProcessor.exe and the vprenderer folders together from the same build.";
				return false;
			}
			if (!function(kVpvExtensionAbi))
			{
				reason = "VP Renderer DLL refused: it did not accept VP-V extension ABI " + std::to_string(kVpvExtensionAbi) +
					". Redeploy VideoProcessor.exe and the vprenderer folders together from the same build.";
				return false;
			}
			reason.clear();
			return true;
		}

		// Attaches the loaded plugin module; call it after Check succeeded, before any renderer is created.
		inline bool Attach(HMODULE module, std::string& reason)
		{
			const VpvAttachFn function = module ?
				reinterpret_cast<VpvAttachFn>(GetProcAddress(module, kVpvAttachExport)) : nullptr;
			return AttachFunction(function, reason);
		}

		// True when the loaded plugin module carries the matching extension ABI.
		inline bool Check(HMODULE module, std::string& reason)
		{
			const VpvAbiFn function = module ?
				reinterpret_cast<VpvAbiFn>(GetProcAddress(module, kVpvAbiExport)) : nullptr;
			return CheckAbiFunction(function, reason);
		}
	}
}
