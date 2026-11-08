// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-5: the vector engine's hub module. It answers Query("vpv.vector_engine.v1") (VpMotion::kVectorEngineQueryId)
// with the IVectorEngineDirectory of the renderers' engines (VectorEngine::Directory: each renderer's VectorEngine
// registers itself under its host key); the consumer (the camera link) asks it for its host's IVectorEngine. It has
// no other callback: no state, no per-frame work, nothing for the Original pathway or with every VP-V motion
// feature off. Registered by HubBootstrap right after ResyncExtension (doc 52 3.2 list order).
#pragma once

#include <VP-V/core/hub/ExtensionHub.h>

namespace VpMotion
{
	class VectorEngineExtension : public Vpv::Hub::ISkeletonModule
	{
	public:
		static constexpr const char* kName = "vector_engine";
		const char* Name() const override { return kName; }
		void* Query(const char* iid) override;
	};

	// Adds VectorEngineExtension to the hub. Start-up only; idempotent.
	void RegisterVectorEngineExtension();
}
