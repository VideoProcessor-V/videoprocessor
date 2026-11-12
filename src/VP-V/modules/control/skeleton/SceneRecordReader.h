// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The content-intent observers' reader of the SCENE record (W1-C7, doc 93 row "row 2"): frame `seq`'s cut as published by the skeleton
// at the frame's seal (SkeletonModule::FillScene -> FrameAnalysis::Store::PublishScene; the verdict is VpUnit::CutFadeJudge::ClassifyEvents
// over the two P4 records). Compute-once: this only READS the store (PeekScene), it never classifies and never starts any producer.
//
// Not found (false) when: no store is registered, more than one renderer is registered (the observers' walk carries no renderer key; the
// key is never guessed), the store is off (frame_analysis_store=false), or the frame has no scene group yet (look-ahead frames are walked
// BEFORE the render thread seals them: the normal case for a deep queue). The caller then keeps its previous private judgement and counts
// the frame as such, so every step down is visible.
//
// Epoch: NOT checked. The store keeps its slots across a discontinuity (Invalidate only moves the epoch), and the verdict of a
// published scene group is a property of that frame pair, not of the epoch. The observers' own rule bounds the use: a pair is read only
// when it is contiguous and adjacent (previous + 1, no discontinuity flag), so a record of another epoch is not asked for in practice.
// "Stale" in the README therefore means: no scene group for that sequence (not published yet, or the slot left the ring).
//
// Header-only, C++14.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <cstdint>

namespace VpSkeleton
{
	namespace SceneRecordReader
	{
		inline bool CutOf(uint64_t seq, bool& cut)
		{
			cut = false;
			FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::SingleStore();
			if (!store)
				return false;
			FrameAnalysis::Scene scene;
			if (!store->PeekScene(seq, scene))
				return false;
			cut = scene.cut;
			return true;
		}
	}
}
