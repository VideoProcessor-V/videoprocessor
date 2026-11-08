// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "VectorEngineExtension.h"

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cstring>
#include <memory>

namespace VpMotion
{
	void* VectorEngineExtension::Query(const char* iid)
	{
		if (!iid || std::strcmp(iid, kVectorEngineQueryId) != 0)
			return nullptr;
		return static_cast<void*>(static_cast<IVectorEngineDirectory*>(&VectorEngine::Directory()));
	}

	void RegisterVectorEngineExtension()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;
		Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new VectorEngineExtension()));
	}
}
