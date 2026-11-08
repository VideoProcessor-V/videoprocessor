// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Facts the skeleton module publishes about the vector engine's field and the scene, for the readers that sit beside the lane
// (the camera fit, guidance). Plain values, no GPU object: a reader that needs the texture asks the engine (IVectorEngine::FieldOf)
// on the render thread. SK-W stage D.

#include <VP-V/core/Indicator.h>

#include <cstdint>

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
		// (VpUnit::CutFadeJudge::ClassifyEvents; cut / fade / flash are never stored). The raw record values are given so a reader can
		// run the fade fit (CutFadeJudge::FadeFit) with its own scaling. Codes are the 16-bit half-plane sample codes of the P4 record.
		struct SceneFacts
		{
			bool valid = false;               // both P4 records were readable
			bool cut = false, cutPossible = false, fade = false, flash = false;
			uint32_t histDistance = 0;        // 0..65535
			bool fullBetween = false;         // the Indicator saw a FULL event between the two frames
			bool continuityBetween = false;   // ... a continuity event
			uint32_t prevMean = 0, curMean = 0;
			uint32_t prevSpread = 0, curSpread = 0;   // p99 - p01
			bool rectFound = false;           // Bill's active-picture rectangle of the CURRENT frame (half-plane px)
			uint32_t rectX0 = 0, rectY0 = 0, rectX1 = 0, rectY1 = 0;
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
	}
}
