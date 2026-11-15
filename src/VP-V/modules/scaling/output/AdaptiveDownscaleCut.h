// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// FIX-X5-CUT-P4 (102b row X5-CUT-P4; design-EDGEMAP-DOMAIN-AXIS s4b): the adaptive downscale's grain term snaps to its target on a
// CUT. The cut used to come from a private mean luma of the measure pass (a step of more than 20 codes, U7 MeanStepCut); it is now
// the shared cut event: the two P4 source-stats records (the previous advanced source frame and this one), ACQUIRED through the
// renderer's skeleton memo (get-or-compute-and-publish, nobody computes a private mean), judged by U7
// (VpUnit::CutFadeJudge::ClassifyEvents, the one classification every consumer derives). The frame's APL is the P4 record's mean.
//
// A source frame the module has not advanced before (first frame, or after a Release) is a cut by definition (a definition, not a fallback).
// The P4 record is ASKED FOR through the standard Acquire, which computes it through its owner fill when it is not published yet.
// Only a genuine failure (no skeleton memo: the Original pathway or no analysis lane; the compute failed with a cause) gives an
// Unavailable verdict: no cut verdict that frame, shown by the standard visible fallback (status line, "output scaling" fallback
// entry, counter; never log-only). There is no private backup and no special snap path.
//
// The previous frame's record is KEPT by the module (CutRecordCache, audit x5dup M3), not re-asked from the memo: the memo releases a
// frame at the next seal. Only the current frame is acquired; the cache is cleared at discontinuities.
//
// Header-only, C++14. The pure decision (VerdictFrom) has no hub, memo or GPU dependency; VerdictFromMemo() runs it on a memo;
// P4CutSource() binds it to the current frame's memo (render thread).

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
		float apl = -1.0f;      // the frame's APL in 8-bit codes (P4 mean / 257); negative: not read
		std::string cause;      // Unavailable: why (shown with the fallback)
	};

	// (previous advanced source sequence, this source sequence): 0 = none before. Render thread.
	using CutSource = std::function<CutVerdict(uint64_t, uint64_t)>;

	// Audit x5dup M3: the P4 record of the previous ADVANCED source frame, kept by the module. The skeleton memo releases a frame's
	// spots at the next seal (keepFrames may be 0) and answers a released frame with FrameReleased, never a recompute, so re-asking
	// the memo for the previous frame would leave every verdict Unavailable. Only the CURRENT frame is asked from the memo; its record
	// is offered here and becomes the previous one when the module advances to that frame (Commit). Cleared at every discontinuity
	// (Release, a stream discontinuity: the vector-validity rule): the next frame then asks the memo for the previous record through
	// the standard Acquire, and a refusal there is the visible Unavailable (no cut verdict that frame), never a private recompute.
	// Render thread (owned by the module, like the CutSource).
	struct CutRecordCache
	{
		// The current frame's record was read (VerdictFrom).
		void Offer(uint64_t sequence, const VpLaneProducts::SourceStatsRecord& record)
		{
			pendingSequence = sequence;
			pending = record;
			pendingValid = true;
		}
		// The module advanced to `sequence`: its offered record (if any) is now the previous one; none offered = nothing kept.
		void Commit(uint64_t sequence)
		{
			valid = pendingValid && pendingSequence == sequence;
			if (valid)
			{
				previousSequence = sequence;
				previous = pending;
			}
			pendingValid = false;
		}
		// The kept record of `sequence`, or null when it is not the one kept.
		const VpLaneProducts::SourceStatsRecord* Previous(uint64_t sequence) const
		{
			return valid && sequence != 0 && previousSequence == sequence ? &previous : nullptr;
		}
		void Clear()
		{
			valid = false;
			pendingValid = false;
			previousSequence = 0;
			pendingSequence = 0;
		}
		bool Holds() const { return valid; }

	private:
		VpLaneProducts::SourceStatsRecord previous, pending;
		uint64_t previousSequence = 0;
		uint64_t pendingSequence = 0;
		bool valid = false;
		bool pendingValid = false;
	};

	// `read(seq, record, cause)` fills the P4 record of source frame `seq` (false + cause when it cannot). With a `cache`, the current
	// record is offered to it and the previous record is taken from it when it holds that frame (the memo is asked only on a miss).
	template <class Read>
	inline CutVerdict VerdictFrom(uint64_t prevSequence, uint64_t sequence, Read&& read, CutRecordCache* cache = nullptr)
	{
		CutVerdict v;
		VpLaneProducts::SourceStatsRecord cur, prev;
		std::string cause;
		if (!read(sequence, cur, cause))
		{
			v.cause = cause.empty() ? "P4 source stats unavailable" : cause;
			return v;
		}
		if (cache != nullptr)
			cache->Offer(sequence, cur);
		v.apl = static_cast<float>(cur.mean) / 257.0f;
		if (prevSequence == 0)
		{
			v.state = CutVerdict::State::Known;
			v.cut = true;   // the first source frame: the grain term starts at its target
			return v;
		}
		const VpLaneProducts::SourceStatsRecord* kept = cache != nullptr ? cache->Previous(prevSequence) : nullptr;
		if (kept == nullptr)
		{
			if (!read(prevSequence, prev, cause))
			{
				const std::string why = cause.empty() ? std::string("P4 source stats of the previous frame unavailable") : cause;
				v.cause = cache != nullptr ? why + " (previous record not kept: a discontinuity, or the previous frame had none)" : why;
				return v;
			}
			kept = &prev;
		}
		v.state = CutVerdict::State::Known;
		v.cut = VpUnit::CutFadeJudge::ClassifyEvents(*kept, cur, false, false).cut;
		return v;
	}

	// One verdict through a skeleton memo: Acquire (CPU form) of kP4SourceStats, the standard get-or-compute-and-publish.
	inline CutVerdict VerdictFromMemo(VpSkeleton::SkeletonMemo& memo, uint64_t prevSequence, uint64_t sequence, CutRecordCache* cache)
	{
		return VerdictFrom(prevSequence, sequence,
			[&memo](uint64_t seq, VpLaneProducts::SourceStatsRecord& rec, std::string& cause)
			{
				const VpSkeleton::AcquireResult r = memo.Acquire(VpSkeleton::kP4SourceStats, seq, VpSkeleton::AcquireForm::Cpu);
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
			}, cache);
	}

	// The production source: the current frame's skeleton memo, with the module's record cache (null: no cache, the previous record
	// is asked from the memo too). The cache must outlive the returned source (the module owns both).
	inline CutSource P4CutSource(CutRecordCache* cache = nullptr)
	{
		return [cache](uint64_t prevSequence, uint64_t sequence)
		{
			const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::CurrentFrameMemo();
			if (!fm.Valid())
			{
				CutVerdict v;
				// Audit x5dup S8: expected with the analysis lane off or the Original pathway (a configuration, not a fault).
				v.cause = "no skeleton memo for this renderer: Original pathway or analysis lane off (expected there, not a fault)";
				return v;
			}
			return VerdictFromMemo(*fm.memo, prevSequence, sequence, cache);
		};
	}
}
