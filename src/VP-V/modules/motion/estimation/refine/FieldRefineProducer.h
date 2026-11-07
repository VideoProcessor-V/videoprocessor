// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P3r `refined_field` producer PLACEHOLDER (doc 72 item 7, P5a reservation). Until the P5a lane stage binds
// FieldRefineRef.h / FieldRefineGlsl.h behind IVectorEngine, the producer publishes "absent": every consumer reads
// the unrefined P3 field (visible, never silent) and the product is never fabricated. It never starts the matcher.
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <string>

namespace VpUnit
{
	namespace FieldRefine
	{
		enum class Availability
		{
			Absent,   // placeholder / not bound: no field published
			Fresh,    // a refined field exists for the pair (bound at P5a)
			Late      // the refinement job missed its slot: consumers take unrefined P3 this frame
		};

		class RefinedFieldProducer
		{
		public:
			// v1 placeholder: always Absent. P5a replaces this class's body, not its readers.
			Availability Status() const { return state; }
			bool Bound() const { return false; }
			const char* Why() const
			{
				switch (state)
				{
				case Availability::Fresh: return "";
				case Availability::Late: return "refinement late";
				default: return "refinement not bound (placeholder until the vector-engine lane stage)";
				}
			}

			// The "Fallbacks engaged" entry: engaged when a consumer asked for refined_field but it is not Fresh.
			FallbackCatalog::Decision Decision(bool consumerRequested) const
			{
				if (!consumerRequested || state == Availability::Fresh)
					return FallbackCatalog::Released("motion refinement");
				return FallbackCatalog::Engaged("motion refinement", std::string("motion: ") + Why() + "; unrefined P3 vectors used");
			}

			std::string StatusLine() const
			{
				return state == Availability::Fresh ? std::string("motion: refined") : std::string("motion: ") + Why();
			}

		private:
			Availability state = Availability::Absent;
		};
	}
}
