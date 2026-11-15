// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The restart decision of detail_pulse_stabilise and texture_restore (registered fix D8, WAIVER-B6-D8-MODELS): a cut starts the
// references over; a fade (U7 v3, owner 2026-11-15) is a PARTIAL reset: the history is reduced by the frame's fade keep, never
// restarted mid-fade. The judgement is the shared unit U7 (CutFadeJudge v3: the sequence law's delivered cut, the pair or multi-frame fade and its keep), read from
// the skeleton's SCENE PRODUCT for the source frame through its get-or-compute-and-publish reader (SkeletonModule::SceneOf via
// Vpv::Skeleton::SceneGroupOf; owner 2026-11-14 17:38 "Ask the skeleton for it same as everything else", C5): the store's scene
// group when it is there, else the skeleton classifies the frame now and publishes it. With frame_analysis_store off a cut therefore
// still restarts and a fade still reduces. Nothing here classifies; the modules' private tile-change judges (share of changed tiles,
// two-step mean ramp) are gone.
//
// No private fallback: a frame the skeleton cannot answer (no skeleton module / Original pathway, no renderer frame on this thread,
// no sealed pair of P4 records) is a RESTART (references start over, gain 1: the safe answer, the picture is never stabilised on an
// unknown history) and is counted as missing (visible in the module's SceneMissing(), cause in SceneUnavailableWhy()). Tests inject
// the reader through Params::sceneReader or DefaultSceneReader().
//
// Header-only, C++14.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <atomic>
#include <cstdint>

namespace VpDetailRestore
{
	// Reads the scene group of one source sequence. False = no readable record.
	using SceneReaderFn = bool (*)(uint64_t sequence, FrameAnalysis::Scene& scene);

	// The cause of the last unanswered read of the production reader (a string literal; shown by SceneUnavailableWhy()).
	inline std::atomic<const char*>& LastSceneMissingWhy()
	{
		static std::atomic<const char*> why{ "scene product not asked yet" };
		return why;
	}

	// PRODUCTION reader: the scene product of the renderer this thread renders (Hub::ThisThreadFrame), through the skeleton's
	// get-or-compute reader. Render thread (the consumers run inside the render).
	inline bool ReadSceneFromSkeleton(uint64_t sequence, FrameAnalysis::Scene& scene)
	{
		const char* why = nullptr;
		if (Vpv::Skeleton::SceneGroupOf(Vpv::Hub::ThisThreadFrame().host, sequence, scene, &why))
			return true;
		LastSceneMissingWhy().store(why != nullptr ? why : "scene product not answered", std::memory_order_relaxed);
		return false;
	}

	// A PEEK of the single store's published group (never classifies, never publishes). Not the production path: kept for the test
	// harnesses that drive a bare store without a skeleton module (D8SceneJudgeAgreementTests and the module suites).
	inline bool ReadSceneFromStore(uint64_t sequence, FrameAnalysis::Scene& scene)
	{
		FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::SingleStore();
		return store && store->PeekScene(sequence, scene);
	}

	// The reader used when Params::sceneReader is null: the skeleton's scene product (ReadSceneFromSkeleton). Tests replace it (a
	// test seam: the unit tests run without a skeleton module); production never sets it.
	inline SceneReaderFn& DefaultSceneReader()
	{
		static SceneReaderFn reader = &ReadSceneFromSkeleton;
		return reader;
	}

	// Why a scene could not be read (for the visible "scene judge unavailable" status line; never silent).
	inline const char* SceneUnavailableWhy()
	{
		if (DefaultSceneReader() == &ReadSceneFromSkeleton)
			return LastSceneMissingWhy().load(std::memory_order_relaxed);
		return VpFrameAnalysis::SourceFrameTap::SingleStore() ? "scene group not sealed for the frame" : "no frame analysis store (frame_analysis_store off)";
	}

	struct SceneRestartAnswer
	{
		bool restart = true;     // cut or no record; U7 v3: a fade is NOT a restart (partial reset), except a fade group without its keep
		bool fromRecord = false; // false: the record was missing (restart by the no-private-fallback rule)
		float strength = 1.0f;   // how far to start over, 0..1: 1 on a restart; with softReset, the U7 v2 cut strength of a non-cut
		// U7 v3, the partial reset on fades (owner 2026-11-15 09:20; 102b U7-FADE-NO-HARDRESTART): the share of the history the
		// consumer keeps on this frame, amplitude domain (VpUnit::CutFadeJudge::FrameFadeKeep; energy holders apply its square).
		// 1 on a frame that is no fade, 0 on a restart. Consumers multiply their history by it on every frame (a product over the
		// fade: the history shrinks monotonically as the picture fades, so pre-fade detail is not carried into the new scene).
		float historyKeep = 0.0f;
		bool fadePartial = false;        // a fade with its keep: no restart, the history reduced by historyKeep
		bool fadeWithoutKeep = false;    // a fade group without the v3 keep (fadeKeep < 0): FULL restart instead, shown (rule 5)
	};

	// U7 v2 (U7-accuracy): `cut` is the scene group's DELIVERED cut (once per confirmed cut; normally one frame late, later only when the
	// frame after the cut had no scene group; FrameAnalysis::Scene). Consumers keep the HARD reset on it (owner ruling Q4). softReset
	// (default OFF) also reports the cut strength of a frame that is not a cut, for a partial start-over; the restart decision itself is
	// unchanged by it. No consumer has a switch for it yet: DetailPulse and TextureRestore call with the default (OFF); the per-consumer
	// switches are deferred (refs/102b, U7-accuracy row "soft reset switch").
	// U7 v3: a fade no longer restarts. A fade group carries fadeKeep (the producer's FrameFadeKeep): the answer is no restart with
	// historyKeep = fadeKeep (fadePartial). A fade group WITHOUT a keep (a producer before v3) restarts, flagged fadeWithoutKeep: the
	// consumer shows it as its scene judge fallback (FallbackCatalog::SceneJudgeUnavailable, why = SceneFadeWithoutKeepWhy()).
	inline SceneRestartAnswer SceneRestartOf(uint64_t sequence, SceneReaderFn reader, bool softReset = false)
	{
		SceneRestartAnswer a;
		FrameAnalysis::Scene scene;
		const bool found = sequence != 0 && (reader ? reader : DefaultSceneReader())(sequence, scene);
		a.fromRecord = found;
		const bool keepGiven = scene.fadeKeep >= 0.0f;   // NaN is not given either
		a.fadeWithoutKeep = found && !scene.cut && scene.fade && !keepGiven;
		a.restart = !found || scene.cut || a.fadeWithoutKeep;
		a.fadePartial = found && !a.restart && scene.fade;
		a.historyKeep = a.restart ? 0.0f : a.fadePartial ? (scene.fadeKeep < 1.0f ? scene.fadeKeep : 1.0f) : 1.0f;
		a.strength = a.restart ? 1.0f : softReset && scene.cutStrength > 0.0f ? (scene.cutStrength < 1.0f ? scene.cutStrength : 1.0f) : 0.0f;
		return a;
	}

	// Why a fade restarted although fades are partial resets (shown in the scene judge's fallback line; never silent).
	inline const char* SceneFadeWithoutKeepWhy()
	{
		return "a fade group without its history keep (producer before U7 v3): full restart instead of the partial reset";
	}
}
