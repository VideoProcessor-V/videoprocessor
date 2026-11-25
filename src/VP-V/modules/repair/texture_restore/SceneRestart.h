// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Restart decision of detail_pulse_stabilise and texture_restore (D8 / U7 v3 / B6-D8-VECTOR-VETO):
// cuts restart, fades and high-bar soft-reset candidates apply partial historyKeep, missing records restart visibly.
// Reader-selectable vectorVeto (default OFF) lets DP/TR veto false pan cuts when P7/P2 vectors verify continuity.
// Header-only, C++14.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>

#include <atomic>
#include <cstdint>

namespace VpSkeleton
{
	class SkeletonMemo;
}

namespace VpDetailRestore
{
	// Reads the scene group of one source sequence. False = no readable record.
	using SceneReaderFn = bool (*)(uint64_t sequence, FrameAnalysis::Scene& scene);
	// Reads motion-vector continuity trust for `sequence` (false = absent or untrusted; fail-closed).
	using VectorTrustReaderFn = bool (*)(uint64_t sequence, double* trustOut);

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

	// cut_soft_reset's built-in default (owner U7 Q4 / settings register row 9: on, at a high bar).
	constexpr bool kDefaultCutSoftReset = true;

	struct SceneRestartAnswer
	{
		bool restart = true;     // cut or no record; U7 v3: a fade is NOT a restart (partial reset), except a fade group without its keep
		bool fromRecord = false; // false: the record was missing (restart by the no-private-fallback rule)
		// How far to start over, 0..1: 1 on a restart; with softReset, the soft-reset strength when the rule fired (softPartial),
		// else 0 (below the bar: no reset).
		float strength = 1.0f;
		bool softPartial = false;   // softReset on and the rule fired: no restart, the history reduced by historyKeep (SoftResetKeep)
		// U7 v3, the partial reset on fades (owner 2026-11-15 09:20; 102b U7-FADE-NO-HARDRESTART): the share of the history the
		// consumer keeps on this frame, amplitude domain (VpUnit::CutFadeJudge::FrameFadeKeep; energy holders apply its square).
		// 1 on a frame that is no fade (SoftResetKeep on a soft reset), 0 on a restart. Consumers multiply their history by it on every frame (a product over the
		// fade: the history shrinks monotonically as the picture fades, so pre-fade detail is not carried into the new scene).
		float historyKeep = 0.0f;
		bool fadePartial = false;        // a fade with its keep: no restart, the history reduced by historyKeep
		bool fadeWithoutKeep = false;    // a fade group without the v3 keep (fadeKeep < 0): FULL restart instead, shown (rule 5)
		// cut_soft_reset, no silent fallbacks (review softreset r1 M3): the group was read but its verdict is PROVISIONAL (frame
		// seq + 1 not yet seen: low latency, a thin lane), so no soft reset can fire on it; softWaiting = softReset on and the rule
		// would have fired had the verdict been confirmed (counted, never acted on).
		bool provisional = false;
		bool softWaiting = false;
		bool vectorVetoed = false;       // B6-D8-VECTOR-VETO: true when trusted motion vectors vetoed a false pan cut (visible)
	};

	// Resolves restart / fade-keep / soft-reset / optional vector-veto (vectorVeto defaults OFF) for `sequence`.
	inline SceneRestartAnswer SceneRestartOf(uint64_t sequence, SceneReaderFn reader, bool softReset = false,
		bool vectorVeto = false, VectorTrustReaderFn trustReader = nullptr, VpSkeleton::SkeletonMemo* memo = nullptr)
	{
		SceneRestartAnswer a;
		FrameAnalysis::Scene scene;
		const bool found = sequence != 0 && (reader ? reader : DefaultSceneReader())(sequence, scene);
		a.fromRecord = found;
		const bool cutDecision = (vectorVeto && found && scene.cut) ? VpUnit::CutFadeJudge::ResolveSceneVectorVeto(sequence, true, trustReader, memo, &a.vectorVetoed) : (found && scene.cut);
		const bool keepGiven = scene.fadeKeep >= 0.0f;
		a.fadeWithoutKeep = found && !cutDecision && scene.fade && !keepGiven;
		a.restart = !found || cutDecision || a.fadeWithoutKeep;
		a.fadePartial = found && !a.restart && scene.fade;
		a.historyKeep = a.restart ? 0.0f : a.fadePartial ? (scene.fadeKeep < 1.0f ? scene.fadeKeep : 1.0f) : 1.0f;
		const float soft = softReset && found && !a.restart && !a.fadePartial ?
			VpUnit::CutFadeJudge::SoftResetStrength(scene.confirmed, scene.cutPossible, scene.cut || scene.cutHere, scene.flash, scene.fade,
				scene.cutStrength) : 0.0f;
		a.softPartial = soft > 0.0f;
		if (a.softPartial)
			a.historyKeep = VpUnit::CutFadeJudge::SoftResetKeep(soft);
		a.strength = a.restart ? 1.0f : soft;
		a.provisional = found && !scene.confirmed;
		a.softWaiting = softReset && found && !scene.confirmed && !a.restart && !a.fadePartial && VpUnit::CutFadeJudge::SoftResetStrength(true, scene.cutPossible, scene.cut || scene.cutHere, scene.flash, scene.fade, scene.cutStrength) > 0.0f;
		return a;
	}

	// cut_soft_reset, no silent fallbacks (review softreset r1 M3; owner rule "every automatic downgrade visible"): the soft reset
	// fires only on CONFIRMED verdicts and, unlike the cut, is never delivered late. Where the scene groups a consumer reads stay
	// provisional (low latency, a lane too thin for frame seq + 1's record at render), the switch is on but cannot act: the status
	// line says so. kSoftResetInactiveGroups consecutive provisional groups (no confirmed one among them) enter that state; the first
	// confirmed group leaves it. A lone provisional group in an ordinary lane does not flip the status line (logged on change).
	constexpr int kSoftResetInactiveGroups = 12;

	struct SoftResetWatch
	{
		int provisionalRun = 0;            // consecutive readable provisional groups (0 after a confirmed one), capped at the bar
		unsigned long long waiting = 0;    // frames the rule would have fired on had their verdict been confirmed
		void Note(const SceneRestartAnswer& a)
		{
			if (!a.fromRecord)
				return;   // no group: counted as missing by the consumer; says nothing about confirmation
			provisionalRun = a.provisional ? (provisionalRun < kSoftResetInactiveGroups ? provisionalRun + 1 : provisionalRun) : 0;
			waiting += a.softWaiting ? 1u : 0u;
		}
		bool Inactive() const { return provisionalRun >= kSoftResetInactiveGroups; }
		void Forget() { provisionalRun = 0; }
	};

	// The soft-reset part of the detail pulse / texture restore status lines (Ctrl+I processing list): "" when on and able to act,
	// ", soft reset: off" when switched off, ", soft reset: on (inactive: provisional verdicts only)" when on but inert (M3).
	inline const char* SoftResetStatusText(bool softReset, bool inactive)
	{
		return !softReset ? ", soft reset: off" : inactive ? ", soft reset: on (inactive: provisional verdicts only)" : "";
	}

	// Why a fade restarted although fades are partial resets (shown in the scene judge's fallback line; never silent).
	inline const char* SceneFadeWithoutKeepWhy()
	{
		return "a fade group without its history keep (producer before U7 v3): full restart instead of the partial reset";
	}
}
