// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// FIX-X5-CUT-P4 (102b row X5-CUT-P4; design-EDGEMAP-DOMAIN-AXIS s4b): the adaptive downscale's grain term snaps to its target on a
// CUT. The cut used to come from a private mean luma of the measure pass (a step of more than 20 codes, U7 MeanStepCut); it is now
// the shared cut event: the two P4 source-stats records (the previous advanced source frame and this one), ACQUIRED through the
// renderer's skeleton memo (get-or-compute-and-publish, nobody computes a private mean), judged by U7
// (VpUnit::CutFadeJudge::ClassifyEvents, the one classification every consumer derives). The frame's mean level is the P4 record's mean.
//
// A source frame the module has not advanced before (first frame, or after a Release) is a cut by definition and needs no read.
// When a P4 record cannot be Acquired (no skeleton memo: the Original pathway or no analysis lane; the product unavailable with a
// cause) the verdict is Unavailable: the module then snaps on the first frame only and the stage shows the cause (status line and
// the "output scaling" fallback entry; never log-only). The private mean-luma step is gone, so no mean-step cut is made then.
//
// Header-only, C++14. The pure decision (VerdictFrom) has no hub, memo or GPU dependency; P4CutSource() binds it to the current
// frame's memo (render thread).

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <cstdint>
#include <functional>
#include <string>

namespace AdaptiveDownscale
{
	struct CutVerdict
	{
		enum class State : uint8_t { Known, Unavailable };
		State state = State::Unavailable;
		bool cut = false;
		float p4MeanCodes = -1.0f;   // the P4 record's mean in 8-bit codes (mean / 257); negative: not read
		std::string cause;      // Unavailable: why (shown with the fallback)
	};

	// (previous advanced source sequence, this source sequence): 0 = none before. Render thread.
	using CutSource = std::function<CutVerdict(uint64_t, uint64_t)>;

	// `read(seq, record, cause)` fills the P4 record of source frame `seq` (false + cause when it cannot).
	template <class Read>
	inline CutVerdict VerdictFrom(uint64_t prevSequence, uint64_t sequence, Read&& read)
	{
		CutVerdict v;
		VpLaneProducts::SourceStatsRecord cur, prev;
		std::string cause;
		if (!read(sequence, cur, cause))
		{
			v.cause = cause.empty() ? "P4 source stats unavailable" : cause;
			return v;
		}
		v.p4MeanCodes = static_cast<float>(cur.mean) / 257.0f;
		if (prevSequence == 0)
		{
			v.state = CutVerdict::State::Known;
			v.cut = true;   // the first source frame: the grain term starts at its target
			return v;
		}
		if (!read(prevSequence, prev, cause))
		{
			v.cause = cause.empty() ? "P4 source stats of the previous frame unavailable" : cause;
			return v;
		}
		v.state = CutVerdict::State::Known;
		v.cut = VpUnit::CutFadeJudge::ClassifyEvents(prev, cur, false, false).cut;
		return v;
	}

	// The production source: Acquire (CPU form) of kP4SourceStats through the current frame's skeleton memo.
	inline CutSource P4CutSource()
	{
		return [](uint64_t prevSequence, uint64_t sequence)
		{
			const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::CurrentFrameMemo();
			if (!fm.Valid())
			{
				CutVerdict v;
				v.cause = "no skeleton memo for this renderer (Original pathway or no analysis lane)";
				return v;
			}
			return VerdictFrom(prevSequence, sequence,
				[&](uint64_t seq, VpLaneProducts::SourceStatsRecord& rec, std::string& cause)
				{
					const VpSkeleton::AcquireResult r = fm.memo->Acquire(VpSkeleton::kP4SourceStats, seq, VpSkeleton::AcquireForm::Cpu);
					if (!r.Ok())
					{
						cause = std::string("P4 source stats unavailable: ") + VpSkeleton::Token(r.cause);
						return false;
					}
					if (!VpLaneProducts::ReadSourceStats(r.cpu, r.cpuBytes, &rec))
					{
						cause = "P4 source stats record unreadable";
						return false;
					}
					return true;
				});
		};
	}
}
