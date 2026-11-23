// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// scaling.output (X5): the picture ordinal of X5's kEdgeMap stage key (CB slice 18 S3; coreplan-CB-18 ruling Q5 /
// EDGEMAP-EXTRAS-KEY). One source frame can be measured more than once: the de-judder extras re-render the frame being shown
// (another picture of the same seq), and a cadence repeat renders it again. Each of those pictures is offered to P13 and
// asked of kEdgeMap under (seq, stage, size, ordinal), so two different pictures of one seq must never share an ordinal, or
// the second would be served the first one's edge map. Next(seq) counts the measures of a seq: 0 for the first (the main
// render), 1, 2, ... for every further one (the extras ordinal + 1); a new seq starts at 0 again. Wraps at 65536 measures of
// one seq, harmlessly: the memo's kEdgeMap and P13 stage entries are frame-scoped (released at each render Begin).
// Render thread only (X5's hook). Header-only, C++14.

#include <cstdint>

namespace AdaptiveDownscale
{
	class EdgeMapStageOrdinal
	{
	public:
		uint16_t Next(uint64_t seq)
		{
			if (!started_ || seq != seq_)
			{
				started_ = true;
				seq_ = seq;
				count_ = 0;
			}
			const uint16_t ordinal = count_;
			count_ = static_cast<uint16_t>(count_ + 1u);
			return ordinal;
		}
		// A discontinuity or a released device: the next measure is a first one.
		void Reset() { started_ = false; seq_ = 0; count_ = 0; }
	private:
		bool started_ = false;
		uint64_t seq_ = 0;
		uint16_t count_ = 0;
	};
}
