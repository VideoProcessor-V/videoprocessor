// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The hub's Query(iid) for code that cannot include ExtensionHub.h (C5 union compile fix, 2026-11-15).
//
// ExtensionHub.h needs vprenderer/RendererExtension.h and so libplacebo's headers. VideoProcessor-Lib code (the LED drive,
// SceneRecordReader) has no libplacebo include path, and Lib is also linked into the GUI, which has no hub. Such code asks the
// hub through this slot instead. ExtensionHub.cpp (compiled into each renderer DLL and the test DLL) installs its Query here
// at static initialisation, so in those binaries QueryHub(iid) == ExtensionHub::Instance().Query(iid). Where no hub is linked
// the slot stays empty and QueryHub returns null: the caller's visible "no skeleton module" path, as with an empty hub.

#include <atomic>

namespace Vpv
{
	namespace Hub
	{
		using QueryFn = void* (*)(const char* iid);

		inline std::atomic<QueryFn>& QuerySlot()
		{
			static std::atomic<QueryFn> slot{ nullptr };
			return slot;
		}

		inline void* QueryHub(const char* iid)
		{
			const QueryFn q = QuerySlot().load(std::memory_order_acquire);
			return q != nullptr ? q(iid) : nullptr;
		}
	}
}
