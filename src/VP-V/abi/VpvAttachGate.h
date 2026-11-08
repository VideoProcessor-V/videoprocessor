// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The DLL-side state of the attach handshake (VpvAbi.h, kVpvExtensionAbi 2). One gate per DLL instance: createRenderer
// asks IsAttached(); VideoProcessorLibplaceboVpvAttach calls Attach(). A wrong number never attaches. Header-only,
// C++14, no logging (the export file logs a refusal).

#include <VP-V/abi/VpvAbi.h>

#include <atomic>
#include <cstdint>

namespace Vpv
{
	class AttachGate
	{
	public:
		// True when `abi` is this DLL's kVpvExtensionAbi (the gate is then open for good).
		bool Attach(uint32_t abi)
		{
			if (abi != kVpvExtensionAbi)
				return false;
			attached_.store(true, std::memory_order_release);
			return true;
		}
		bool IsAttached() const { return attached_.load(std::memory_order_acquire); }
	private:
		std::atomic<bool> attached_{ false };
	};
}
