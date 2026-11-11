// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Facts the skeleton module publishes about the vector engine's field and the scene, for the readers that sit beside the lane
// (the camera fit, guidance). Plain values, no GPU object: a reader that needs the texture asks the engine (IVectorEngine::FieldOf)
// on the render thread. SK-W stage D.

#include <VP-V/core/Indicator.h>
#include <VP-V/core/LaneLease.h>
#include <VP-V/core/Products.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Skeleton
	{
		// The verified field of one pair as the engine made it, recorded at the moment the module published the P2m / P3 slots of
		// frame `seq` (= pairTo). `stamp` is the stamp the field was computed under (the engine's Validity facade: MotionValidity::StampAt);
		// the slots carry it as their honoured stamp, so the seal of frame `seq` rejects them when a resync event moved the stamp between.
		struct EngineFieldFacts
		{
			uint64_t seq = 0;                 // the frame the slots were published for (= pairTo)
			uint64_t pairFrom = 0, pairTo = 0;
			uint64_t era = 0;                 // IVectorEngine::FieldEra() when published
			int gridW = 0, gridH = 0, blockSize = 0;   // blockSize in source px
			float scaleToSourcePx = 1.0f;
			VpIndicator::Stamp stamp;
			bool globalValid = false;         // IVectorEngine::EngineGlobal(seq): the producer's whole-frame vector (source px over the pair)
			double globalX = 0.0, globalY = 0.0;
			bool p2Published = false;         // the NVOF pair slot (P2) was published (only when the graph holds P2 and the engine had a global)
			bool p2mPublished = false;        // the matcher pair slot (P2m)
			bool p3Published = false;         // the analysis field slot (P3)
		};

		// The scene of frame `seq` against the previous frame, derived from the two P4 records exactly as every consumer does
		// (VpUnit::CutFadeJudge::ClassifyEvents; the classification is derived, the cut / fade verdict is also stored once per frame by SceneOf's publisher). The raw record values are given so a reader can
		// run the fade fit (CutFadeJudge::FadeFit) with its own scaling. Codes are the 16-bit half-plane sample codes of the P4 record.
		struct SceneFacts
		{
			bool valid = false;               // both P4 records were readable
			bool cut = false, cutPossible = false, fade = false, flash = false;
			uint32_t histDistance = 0;        // 0..65535
			bool fullBetween = false;         // RESERVED, always false: scene events come from P4 alone; resync / validity is the P5a MotionValidity facade's
			bool continuityBetween = false;   // RESERVED, always false (see above)
			uint32_t prevMean = 0, curMean = 0;
			uint32_t prevSpread = 0, curSpread = 0;   // p99 - p01
			bool rectFound = false;           // Bill's active-picture rectangle of the CURRENT frame (half-plane px)
			uint32_t rectX0 = 0, rectY0 = 0, rectX1 = 0, rectY1 = 0;
		};

		// SCENE as a skeleton product (W1-C7): what a reader of "cut / fade of frame `seq`" gets from SkeletonModule::SceneOf. The product is
		// sealed per frame: the module publishes it into the frame-analysis store at the frame's seal (once; skipped when present). `from`
		// says where this answer came from: Store = the record's scene group; Live = filled from the two P4 records now (the store is off, has
		// no record for the frame, or the frame is not sealed yet). Both give the same cut / fade (one classification, ClassifyEvents).
		struct SceneAnswer
		{
			enum class From : uint8_t { None, Store, Live };
			bool known = false;               // cut / fade are answered (false: the frame has no sealed pair of P4 records yet)
			bool cut = false, fade = false;
			From from = From::None;
		};

		// What the scene product did (any thread; relaxed counters copied out).
		struct SceneCounters
		{
			uint64_t published = 0;       // records written at a seal
			uint64_t alreadyPresent = 0;  // seal found the scene already there (compute-once: nothing recomputed)
			uint64_t notReady = 0;        // seal could not classify or had no record to write into (counted once, not re-tried every call)
			uint64_t storeReads = 0;      // SceneOf answered from the store
			uint64_t liveFills = 0;       // SceneOf answered by classifying the P4 records now (and filled the store when it holds the record)
			uint64_t cacheHits = 0;       // SceneOf answered from this frame's live classification (no second classification)
			uint64_t unknown = 0;         // SceneOf could not answer
		};

		// What the engine feed did (render thread): shown on the status lines, read by tests.
		struct FeedCounters
		{
			uint64_t published = 0;       // fields whose slots were published
			uint64_t noEngine = 0;        // calls without an engine for this renderer
			uint64_t noField = 0;         // the engine named a latest field it could not give
			uint64_t slip = 0;            // fields not published: the pair spans a slip / an invalid pair (PairSpansSlip)
			uint64_t ancestorsMissing = 0;   // publish attempts that found an input slot (P1) not published yet (retried)
			uint64_t claimFailed = 0;     // a slot could not be claimed (the frame left the ring)
			uint64_t abandoned = 0;       // fields given up after the retries
			uint64_t eraResets = 0;       // the engine's numbering restarted (seek, resync, format change)
			uint64_t lastSeq = 0;         // the last frame published
		};

		// The EXTERNAL DEMAND entry of the skeleton module (SK-W stage D), for a component beside the lane that does not include the module
		// (the camera extension). SkeletonModule implements it and hands it out through Query(kExternalDemandQueryId). Any thread. The
		// contract is SkeletonModule::SetExternalDemand / ClearExternalDemand (one entry per (host, owner), replaced by the next call).
		constexpr const char* kExternalDemandQueryId = "vpv.skeleton.external_demand.v1";
		struct IExternalDemand
		{
			virtual void SetExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
				const std::vector<VpSkeleton::ProductId>& publishedOutside) = 0;
			virtual void ClearExternalDemand(const void* host, const void* owner) = 0;
			// The CURRENT lease of the host's lane for `product` (AnalysisLane::Lease: the product's bytes ring added), for a component that
			// publishes a product it declared publishedOutside (the camera's on-time P7). Any thread. EMPTY (no table) when the host has no
			// lane (analysis_lane off, no source yet) or the graph does not hold the product: the caller uses its own path, visibly.
			// Generations are per lane (a new source size builds a new lane and restarts them): take a fresh lease per use, never install
			// one across a rebuild with LaneLeaseNewer. Default: empty (an implementer without lanes).
			virtual VpSkeleton::LaneLease LeaseFor(const void* /*host*/, VpSkeleton::ProductId /*product*/) { return VpSkeleton::LaneLease(); }
		protected:
			~IExternalDemand() {}
		};

		// The visible explanation of FeedCounters::claimFailed (empty while none). `backward` = the lane runs without a look-ahead lead (low-latency /
		// backward pairs, lane lead 0): the engine's pair (N-1, N) is only complete AFTER frame N was sealed, so the slot is already taken and the field
		// cannot be published for that frame; its readers fall back to their own form. That is how this mode works, not a fault, and it is said so.
		// With a lead the same counter means the frame had left the ring (or was published already) before the field arrived.
		inline std::string ClaimFailedNote(uint64_t claimFailed, bool backward)
		{
			if (claimFailed == 0)
				return std::string();
			const std::string n = std::to_string(claimFailed);
			return backward
				? "Skeleton engine feed: " + n + " pair(s) arrived after the seal of their frame (backward / low-latency mode: no look-ahead lead, so the field is complete only after its frame was sealed); no field is published for those frames and their readers use their own form (expected in this mode)"
				: "Skeleton engine feed: " + n + " field(s) not published because their frame had already left the ring or was published before the field arrived; their readers use their own form";
		}
	}
}
