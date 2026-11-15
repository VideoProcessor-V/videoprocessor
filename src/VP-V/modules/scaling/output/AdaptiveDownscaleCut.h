// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// FIX-X5-CUT-P4 (102b row X5-CUT-P4; design-EDGEMAP-DOMAIN-AXIS s4b), moved onto the skeleton SCENE PRODUCT in C5 (audit x5dup MUST 1):
// the adaptive downscale's grain term snaps to its target on a CUT. The cut is the shared one, judged ONCE per frame by the skeleton
// (U7 v2/v3: VpUnit::CutFadeJudge::CutTracker over the P4 record sequence, SkeletonModule::FillScene) and published at the frame's seal
// as the frame-analysis store's scene group (FrameAnalysis::Scene). This file only READS that group (Store::PeekScene of the renderer's
// own store, the same read DetailPulse / TextureRestore and the content-intent observers do): no classification here, no ClassifyEvents
// call, no private copy of a P4 record, no second computation of the scene product (producer chains, OD 2026-11-15 10:02).
//
// Snap rule (owner 2026-11-15 08:35: high firing bar, precision first; false cuts equally unacceptable):
// - snap on the DELIVERED confirmed cut only (FrameAnalysis::Scene::cut: once per confirmed cut, normally with the group of the frame
//   after the cut, cutLate; a provisional verdict never delivers one). The grain term therefore snaps one source frame after the cut's
//   first frame in the normal case (Scene::cutLate); that frame is smoothed from the old scene's term, as every scene-product consumer is.
// - a source frame the module has not advanced before (first frame, after a Release) is a cut by definition (a definition, not a fallback).
// - frames the module did not advance through (the renderer showed a later source frame) are read too, within the store's ring: a cut
//   the product delivered with a skipped frame's group is not lost. Groups that are not there (never sealed) are not a cut and not a fault:
//   the product itself does not judge across a gap (the tracker re-primes, partStart is never a cut).
// - NO snap on a fade (owner 2026-11-15 09:20: fades are a partial reset, never a restart). Choice recorded for C5: the grain term keeps
//   its one-pole smoothing through a fade; it does NOT apply Scene::fadeKeep (an owner option, refs\102b row X5-FADE-KEEP proposed). The
//   fade flag is carried for diagnostics only.
// - a missing scene group of the CURRENT frame (no memo / Original pathway, no store for the renderer, the store off, the group not
//   sealed) is the visible Unavailable: no cut verdict, no snap, cause shown (status line, "output scaling" fallback entry, counter).
//   There is no private re-classification and no backup path.
//
// The frame's APL (the scalar's diagnostic mean luma) is read from the current frame's P4 record through the renderer's skeleton memo
// (the standard Acquire: get-or-compute-and-publish). It is not part of the cut; a failed read leaves the APL unread (aplCause, shown).
//
// Header-only, C++14. The pure decision (VerdictFromScene) has no hub, memo, store or GPU dependency; CutFromSceneProduct() binds it to the
// renderer's store and memo of the current frame (render thread).

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace AdaptiveDownscale
{
	struct CutVerdict
	{
		enum class State : uint8_t { Known, Unavailable };
		State state = State::Unavailable;
		bool cut = false;       // snap the grain term: the scene product's delivered cut, or the first frame
		bool first = false;     // no previous advanced frame: a cut by definition (never scored as a true or a false cut)
		bool cutLate = false;   // the delivered cut belongs to an earlier frame (Scene::cutLate, or a skipped frame's group)
		uint64_t cutGroup = 0;  // the source frame whose scene group delivered the cut (0: none)
		bool fade = false;      // the current group says fade: NO snap (partial-reset territory; diagnostics only)
		float apl = -1.0f;      // the frame's APL in 8-bit codes (P4 mean / 257); negative: not read
		std::string cause;      // Unavailable: why (shown with the fallback)
		std::string aplCause;   // the mean luma was not read: why (shown; not a cut fault)
	};

	// (previous advanced source sequence, this source sequence): 0 = none before. Render thread.
	using CutSource = std::function<CutVerdict(uint64_t, uint64_t)>;

	// `readScene(seq, scene)` reads the published scene group of source frame `seq` (false: none). `reach` = how many frames back the
	// store can still hold (its ring depth; 0 = read only the current frame). `missingWhy` = the cause when the current group is missing.
	template <class ReadScene>
	inline CutVerdict VerdictFromScene(uint64_t prevSequence, uint64_t sequence, ReadScene&& readScene, size_t reach,
		const std::string& missingWhy)
	{
		CutVerdict v;
		if (prevSequence == 0)
		{
			v.state = CutVerdict::State::Known;
			v.cut = true;   // the first source frame: the grain term starts at its target
			v.first = true;
			return v;
		}
		FrameAnalysis::Scene own;
		if (!readScene(sequence, own))
		{
			v.cause = missingWhy.empty() ? std::string("scene group not readable") : missingWhy;
			return v;
		}
		v.state = CutVerdict::State::Known;
		v.fade = own.fade;
		if (own.cut)
		{
			v.cut = true;
			v.cutLate = own.cutLate;
			v.cutGroup = sequence;
			return v;
		}
		// Frames between the previous advance and this one that the module did not advance through: a cut their groups delivered.
		if (sequence > prevSequence + 1 && reach > 0)
		{
			uint64_t from = prevSequence + 1;
			if (sequence - from > reach)
				from = sequence - static_cast<uint64_t>(reach);
			for (uint64_t d = from; d < sequence; ++d)
			{
				FrameAnalysis::Scene skipped;
				if (readScene(d, skipped) && skipped.cut)
				{
					v.cut = true;
					v.cutLate = true;
					v.cutGroup = d;
					break;
				}
			}
		}
		return v;
	}

	// Why the current frame's scene group could not be read, for the renderer's store (null: none registered).
	inline std::string SceneMissingWhy(const FrameAnalysis::Store* store, uint64_t sequence)
	{
		if (store == nullptr)
			return "no frame analysis store for this renderer (no scene product to read)";
		if (!store->Enabled())
			return "frame analysis store off (frame_analysis_store=off): no scene product to read";
		return "scene group not sealed for source frame " + std::to_string(sequence);
	}

	// The frame's APL from its P4 record through the memo (Acquire, CPU form: the standard get-or-compute-and-publish).
	inline void ReadAplFromMemo(VpSkeleton::SkeletonMemo& memo, uint64_t sequence, CutVerdict& v)
	{
		const VpSkeleton::AcquireResult r = memo.Acquire(VpSkeleton::kP4SourceStats, sequence, VpSkeleton::AcquireForm::Cpu);
		if (!r.Ok())
		{
			v.aplCause = std::string("P4 source stats unavailable: ") + VpSkeleton::Token(r.cause);
			return;
		}
		VpLaneProducts::SourceStatsRecord cur;
		if (!VpLaneProducts::ReadSourceStats(r.cpu, r.cpuBytes, &cur))
		{
			v.aplCause = "P4 source stats record unreadable";
			return;
		}
		v.apl = static_cast<float>(cur.mean) / 257.0f;
	}

	// The production source: the renderer of the frame this thread renders (Hub::ThisThreadFrame through the skeleton's memo lookup),
	// its frame-analysis store's scene groups, and its memo for the APL.
	inline CutSource CutFromSceneProduct()
	{
		return [](uint64_t prevSequence, uint64_t sequence)
		{
			const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::CurrentFrameMemo();
			if (!fm.Valid())
			{
				CutVerdict v;
				// Audit x5dup S8: expected with the analysis lane off or the Original pathway (a configuration, not a fault).
				v.cause = "no skeleton memo for this renderer: Original pathway or analysis lane off (expected there, not a fault)";
				return v;
			}
			const FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(fm.host);
			CutVerdict v = VerdictFromScene(prevSequence, sequence,
				[store](uint64_t seq, FrameAnalysis::Scene& scene) { return store != nullptr && store->PeekScene(seq, scene); },
				store != nullptr ? store->Depth() : 0, SceneMissingWhy(store, sequence));
			ReadAplFromMemo(*fm.memo, sequence, v);
			return v;
		};
	}
}
