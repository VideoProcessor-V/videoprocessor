// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "VectorDemand.h"

#include <VP-V/core/Products.h>

#include <vector>

// Skeleton S0 (design doc 25 B1, coordinator answer 11): vector demand's aggregation as product-
// registry consumer declarations. VectorDemand::Decide stays the one decision; this only states
// it in registry terms, so the graph holds the vector pair (P2, or the matcher pair P2m without an
// optical-flow engine) exactly when the decision runs the producer, and the analysis motion field
// (P3) exactly when it asks for a render-time estimate. Synthesis consumers keep their own
// policies and declare nothing here (as in VectorDemand, they never make the engine run).
namespace VectorDemand
{
	constexpr uint16_t kConsumerIdBase = 0x0100;   // registry consumer id = base + Consumer index

	// The products a served consumer reads: the field and output consumers read the analysis field
	// (which itself reads the pair); the statistics consumers read the pair's global vector only.
	inline VpSkeleton::ConsumerDecl DeclarationOf(Consumer c)
	{
		VpSkeleton::ConsumerDecl d;
		d.id = uint16_t(kConsumerIdBase + static_cast<int>(c));
		d.name = Name(c);
		const Domain dom = DomainOf(c);
		VpSkeleton::AddConsume(d, VpSkeleton::kP2VectorPair, 0);
		if (dom == Domain::SourceField || dom == Domain::Output)
			VpSkeleton::AddConsume(d, VpSkeleton::kP3AnalysisField, 0);
		// The output dither carries a frame's field into the next frame's first present.
		d.holdsSpan = c == Consumer::SigmaDelta;
		// Every non-synthesis consumer has a visible no-vector form (UnservedText / fallbacks).
		d.hasReducedMode = true;
		return d;
	}

	// Appends one declaration per served consumer, in Consumer order (deterministic).
	inline void DeclareConsumers(const Decision& d, std::vector<VpSkeleton::ConsumerDecl>& out)
	{
		for (int i = 0; i < kConsumerCount; ++i)
			if (d.Serves(ConsumerAt(i)))
				out.push_back(DeclarationOf(ConsumerAt(i)));
	}

	// Lane lead for the graph: a forward engine needs the frame ahead; backward pairs (no frame
	// ahead, Competitive 0) run on arrival with no lead.
	inline uint8_t LaneLeadFor(const Decision& d, uint8_t forwardLead)
	{
		return d.mode == Mode::Backward ? uint8_t(0) : forwardLead;
	}
}
