// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The restart decision of detail_pulse_stabilise and texture_restore (registered fix D8, WAIVER-B6-D8-MODELS): a cut or a fade
// starts the references over. The judgement is the shared unit U7 (CutFadeJudge::ClassifyEvents over the two P4 records), read from
// the SCENE group the skeleton published for the source frame (FrameAnalysis::Store::PeekScene via Acquire semantics: read only,
// never classifies, never starts a producer). The modules' private tile-change judges (share of changed tiles, two-step mean ramp)
// are gone.
//
// No private fallback: a frame without a readable scene group (no store, store off, ambiguous owner, not sealed yet) is a RESTART
// (references start over, gain 1: the safe answer, the picture is never stabilised on an unknown history) and is counted as missing
// (visible in the module's SceneMissing()). Tests inject the reader through Params::sceneReader.
//
// Header-only, C++14.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <cstdint>

namespace VpDetailRestore
{
	// Reads the scene group of one source sequence. False = no readable record.
	using SceneReaderFn = bool (*)(uint64_t sequence, FrameAnalysis::Scene& scene);

	inline bool ReadSceneFromStore(uint64_t sequence, FrameAnalysis::Scene& scene)
	{
		FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::SingleStore();
		return store && store->PeekScene(sequence, scene);
	}

	// The reader used when Params::sceneReader is null: the skeleton's store. Tests replace it (a test seam: the unit tests run
	// without a skeleton store); production never sets it.
	inline SceneReaderFn& DefaultSceneReader()
	{
		static SceneReaderFn reader = &ReadSceneFromStore;
		return reader;
	}

	struct SceneRestartAnswer
	{
		bool restart = true;     // cut, fade or no record
		bool fromRecord = false; // false: the record was missing (restart by the no-private-fallback rule)
	};

	inline SceneRestartAnswer SceneRestartOf(uint64_t sequence, SceneReaderFn reader)
	{
		SceneRestartAnswer a;
		FrameAnalysis::Scene scene;
		const bool found = sequence != 0 && (reader ? reader : DefaultSceneReader())(sequence, scene);
		a.fromRecord = found;
		a.restart = !found || scene.cut || scene.fade;
		return a;
	}
}
