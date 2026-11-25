// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The content-intent observers', LED's, and SRM's reader of the shared SCENE record and P4 source stats.
// CutOf / AcquireCut keep their original inline bodies for Bill-TU line parity; SceneOf / AcquireScene /
// AcquireSourceStats live out-of-line in SceneRecordReader.cpp (VideoProcessor-Lib) so SourceStats.h is
// never pulled into Bill-owned TUs. Pure C++14.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>   // Demand(), SceneAnswer; not SkeletonAccess.h (hub header: no libplacebo in Lib)
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <cstdint>

namespace VpUnit
{
	namespace SourceStats
	{
		struct Record;
	}
}

namespace VpSkeleton
{
	namespace SceneRecordReader
	{
		struct SceneVerdict
		{
			bool cut = false;
			bool fade = false;
			float fadeKeep = -1.0f;
		};

		// Peek-only store reader of full scene verdict (out-of-line in SceneRecordReader.cpp).
		bool SceneOf(uint64_t seq, SceneVerdict& out);

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

		// GET-OR-COMPUTE readers (out-of-line in SceneRecordReader.cpp): full scene verdict and P4 SourceStats.
		bool AcquireScene(uint64_t seq, SceneVerdict& out, const char** why);
		bool AcquireSourceStats(uint64_t seq, VpUnit::SourceStats::Record& out, const char** why);

		// Capture-counter -> renderer sourceSequence alignment (BLOCKER-2): recorded at RendererExtension::Ingress
		// and cleared on queue flush / renderer teardown so capture-thread readers (e.g. HDR SDR veto) never
		// pass a raw capture driver counter into a renderer-sequence-keyed U7 store.
		void RecordCaptureSequence(const void* owner, uint64_t captureCounter, int64_t timestampTicks, uint64_t sourceSeq);
		void ClearCaptureSequences(const void* owner);
		bool ResolveCaptureSequence(uint64_t captureCounter, int64_t timestampTicks, uint64_t& sourceSeqOut);

		// GET-OR-COMPUTE reader (the LED's, measured frame AND look-ahead): the skeleton's scene product (SceneOf) for frame `seq`. It reads the
		// store's group when present, else the two P4 records are acquired through the renderer's memo (made now for a frame ahead of the seal)
		// and the verdict is published for the later seal (computed once). The renderer key is the ONE registered owner (the Host registers its
		// store even with frame_analysis_store=off, so the key exists); more than one renderer is ambiguous and never guessed.
		// False = genuine failure; `*why` (when given) names it, static text. cut is false then.
		inline bool AcquireCut(uint64_t seq, bool& cut, const char** why)
		{
			cut = false;
			const char* reason = "";
			bool found = false;
			const std::vector<const void*> owners = VpFrameAnalysis::SourceFrameTap::StoreOwners();
			if (owners.size() != 1)
				reason = owners.empty() ? "no store owner" : "renderer ambiguous";
			else if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
			{
				const Vpv::Skeleton::SceneAnswer a = demand->SceneOf(owners[0], seq);
				if (a.known)
				{
					cut = a.cut;
					found = true;
				}
				else
					reason = (a.why != nullptr && a.why[0] != 0) ? a.why : "scene unknown";
			}
			else
				reason = "no skeleton module";
			if (why != nullptr)
				*why = reason;
			return found;
		}
	}
}
