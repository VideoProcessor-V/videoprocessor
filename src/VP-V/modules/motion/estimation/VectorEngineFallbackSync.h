// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Plan 58 T1 / B3 (call-site group B): the vector engine's fallback entries synced in one call.
// The engine (VpMotion::VectorEngine::UpdateVectorEngine, formerly the renderer Impl) used to sync three entries inline;
// this adds the fourth, "vector producer" (the look-ahead stopped and is restarting with back-off,
// FallbackCatalog::VectorProducer), which had no call site. Render thread only.
//
// P5a-core port (m2/p5a-t1): the three entries' state moved to VpMotion::VectorEngineState, so the
// helpers take that state and the host that syncs. Templated so a test can use a fake of each:
//   State: vectorDemand, vectorProducerSkipSustained, vectorGlobalMissSustained, motionProducer (a
//          smart pointer to MotionAhead::Producer, may be null);
//   Host:  SyncFallback(const FallbackCatalog::Decision&) (IVectorEngineHost).
#include <VP-V/core/FallbackCatalog.h>

#include "MotionAheadProducer.h"

namespace VectorEngineFallbackSync
{
	template <class State, class Host>
	void Sync(State& state, Host& host)
	{
		host.SyncFallback(FallbackCatalog::VectorEngine(state.vectorDemand));
		host.SyncFallback(FallbackCatalog::VectorEngineProducer(state.vectorProducerSkipSustained));
		host.SyncFallback(FallbackCatalog::VectorEngineGlobal(state.vectorGlobalMissSustained));
		MotionAhead::Producer::Health health;   // no producer: not recovering
		if (state.motionProducer)
			health = state.motionProducer->GetHealth();
		host.SyncFallback(FallbackCatalog::VectorProducer(state.vectorDemand.producer, health.recovering, health.stops, health.error));
		host.SyncFallback(state.motionEngineEntry.For(state.State()));   // W2-C1: the one engine state (State() accessor; cached entry)
	}

	// vector_demand off: every entry of the group is released.
	template <class Host>
	void Release(Host& host)
	{
		host.SyncFallback(FallbackCatalog::Released("vector_engine"));
		host.SyncFallback(FallbackCatalog::Released("vector_engine_producer"));
		host.SyncFallback(FallbackCatalog::Released("vector_engine_global"));
		host.SyncFallback(FallbackCatalog::Released("vector producer"));
		host.SyncFallback(FallbackCatalog::Released("motion engine"));
	}
}
