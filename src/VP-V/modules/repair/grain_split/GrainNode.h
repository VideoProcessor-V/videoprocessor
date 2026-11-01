// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <cstdint>
#include <string>

// repair.grain_split as a skeleton node (SA2, doc 51 P10): ONE node per plane-component over a core SlotSet
// (ProductSlots.h). Pure C++14 protocol, no GPU: the compute is behind an interface (`Compute::Fill`), so the CPU
// tests drive it with a fake and the renderer drives it with the GrainSplit::Gpu::Splitter.
//
// FILL RULE (owner, doc 25):
//   * the lane precomputes frame N into the slot's ring buffer in an idle gap and PUBLISHES (claim -> write ->
//     stamp last -> publish) BEFORE the frame's seal point at render start; after the seal the slot is locked
//     (read-only for that frame) and every consumer reuses the same buffer;
//   * if nothing valid was published by the seal, the FIRST consumer along the pathway that needs the product
//     does ONE live fill into the live buffer of the same slot; every later consumer of that frame reuses it;
//   * never two producers for one slot in one frame; no write after the seal except that one live fill; a lane
//     result that arrives after the seal is DISCARDED (PublishResult::Late), never merged;
//   * a live fill that failed is not retried inside the frame (no second producer): later consumers see Missing.
//
// GRACEFUL FALLBACK (owner): Acquire answers Ring / Live, or why not: Missing (nothing, fill declined or failed),
// Stale (the slot holds another frame), WrongSize (the reader's plane does not match: size, block, scale, code
// unit, component), Shed (the governor dropped the product). A reader then runs its own reduced computation or
// goes off, VISIBLY (ReaderFallback: a FallbackCatalog entry per consumer) and recovers by itself the first frame
// the product is Ok again (ReaderFallback releases).
//
// Threading: lane methods on the lane thread, render methods on the render thread (the SlotSet protocol).
namespace GrainSplit
{
	// What a node holds: the plane-component's geometry and the code unit it is computed with.
	struct GrainDesc
	{
		int w = 0, h = 0, B = 16;
		double scale = 1.0;
		double codeUnit = 0.0;
		int component = -1;
	};

	inline bool Same(const GrainDesc& a, const GrainDesc& b)
	{
		return a.w == b.w && a.h == b.h && a.B == b.B && a.scale == b.scale && a.codeUnit == b.codeUnit &&
			a.component == b.component;
	}

	// Where Compute::Fill writes: a ring buffer (the lane) or a live-pool buffer (the render's live fill).
	struct GrainTarget
	{
		bool live = false;
		int index = -1;   // ring slot or live-pool buffer
	};

	enum class GrainStatus : uint8_t { Ring, Live, Missing, Stale, WrongSize, Shed };

	inline const char* Token(GrainStatus s)
	{
		switch (s)
		{
		case GrainStatus::Ring: return "ring";
		case GrainStatus::Live: return "live";
		case GrainStatus::Missing: return "missing";
		case GrainStatus::Stale: return "stale";
		case GrainStatus::WrongSize: return "wrong_size";
		case GrainStatus::Shed: return "shed";
		}
		return "?";
	}

	struct GrainRead
	{
		GrainStatus status = GrainStatus::Missing;
		int index = -1;               // buffer to read (ring slot or live buffer)
		bool filledHere = false;      // THIS call made the live fill (the first consumer)
		VpSkeleton::SealReason reason = VpSkeleton::SealReason::LaneMissing;
		bool Ok() const { return status == GrainStatus::Ring || status == GrainStatus::Live; }
	};

	enum class LaneOutcome : uint8_t { Published, Late, Skipped, WriteFailed };

	struct NodeCounters
	{
		uint64_t laneComputes = 0;   // fills by the lane
		uint64_t liveFills = 0;      // the single live fill per frame, by its first consumer
		uint64_t ringReads = 0;      // reads served from the sealed ring buffer
		uint64_t liveReuses = 0;     // reads served from an existing live fill (later consumers)
		uint64_t fillFailures = 0;
		uint64_t missing = 0, stale = 0, wrongSize = 0, shed = 0;
	};

	class GrainNode
	{
	public:
		GrainNode(VpSkeleton::ProductId id, uint16_t ring, uint16_t livePool, const GrainDesc& desc)
			: slots_(id, ring, livePool), desc_(desc) {}
		GrainNode(const GrainNode&) = delete;
		GrainNode& operator=(const GrainNode&) = delete;

		const GrainDesc& Desc() const { return desc_; }
		VpSkeleton::SlotSet& Slots() { return slots_; }
		const VpSkeleton::SlotSet& Slots() const { return slots_; }
		const NodeCounters& Counters() const { return counters_; }
		// The tier plan's lane grant (no lane: a missing precompute is expected, not counted).
		void SetLaneGranted(bool granted) { slots_.SetLaneGranted(granted); }
		// The governor dropped the product: every read is Shed (visible fallback), nothing is computed.
		void SetShed(bool shed) { shed_ = shed; }
		bool Shed() const { return shed_; }
		// Who made the live fill of the last frame that had one (visibility).
		const std::string& LastFiller() const { return lastFiller_; }

		// ---- lane side (one producer thread) ----

		VpSkeleton::ClaimResult LaneClaim(uint64_t seq, VpSkeleton::LaneTicket* t, uint64_t completedRelease = ~uint64_t(0))
		{
			if (shed_)
				return VpSkeleton::ClaimResult::TooEarly;   // a dropped product is not precomputed
			return slots_.Claim(seq, VpSkeleton::Provenance::AheadLane, completedRelease, t);
		}
		bool LaneWritable(const VpSkeleton::LaneTicket& t) const { return slots_.Writable(t); }
		template <class Compute>
		bool LaneWrite(const VpSkeleton::LaneTicket& t, Compute& c)
		{
			GrainTarget target;
			target.live = false;
			target.index = t.slot;
			++counters_.laneComputes;
			return c.Fill(t.seq, target);
		}
		VpSkeleton::PublishResult LanePublish(VpSkeleton::LaneTicket& t) { return slots_.Publish(t); }
		bool LaneAbandon(VpSkeleton::LaneTicket& t) { return slots_.Abandon(t); }

		// The whole lane step for frame `seq`: claim, write, publish. Late when the seal got there first.
		template <class Compute>
		LaneOutcome LanePrecompute(uint64_t seq, Compute& c)
		{
			VpSkeleton::LaneTicket t;
			if (LaneClaim(seq, &t) != VpSkeleton::ClaimResult::Claimed || !LaneWritable(t))
			{
				if (t.valid)
					LaneAbandon(t);
				return LaneOutcome::Skipped;
			}
			if (!LaneWrite(t, c))
			{
				LaneAbandon(t);
				return LaneOutcome::WriteFailed;
			}
			return LanePublish(t) == VpSkeleton::PublishResult::Published ? LaneOutcome::Published : LaneOutcome::Late;
		}

		// ---- render side (render thread) ----

		// The seal point of frame `seq` at render start: accepts a published lane result (the stamp / ancestry
		// validation of a real graph is SlotTable's check; here the slot protocol decides), after which the slot
		// is locked. Idempotent.
		VpSkeleton::SealResult Seal(uint64_t seq)
		{
			const VpSkeleton::SealResult r = slots_.Seal(seq, [](const VpSkeleton::Header&) { return VpSkeleton::SealReason::Accepted; });
			if (r.reason != VpSkeleton::SealReason::AlreadySealed)
				sealResult_ = r;
			sealedSeq_ = seq;
			sealed_ = true;
			return r;
		}

		// A consumer asks for frame `seq` of its plane. The first consumer of an unpublished frame makes the one
		// live fill (`compute` is called once); later consumers reuse it.
		template <class Compute>
		GrainRead Acquire(uint64_t seq, const GrainDesc& want, const char* consumer, Compute& compute)
		{
			GrainRead out;
			if (shed_)
			{
				++counters_.shed;
				out.status = GrainStatus::Shed;
				return out;
			}
			if (!Same(want, desc_))
			{
				++counters_.wrongSize;
				out.status = GrainStatus::WrongSize;
				return out;
			}
			if (!sealed_ || sealedSeq_ != seq)
				Seal(seq);
			VpSkeleton::ReadInfo r = slots_.Read(seq);
			if (r.kind == VpSkeleton::ReadInfo::None)
			{
				if (failedSeq_ == seq && failed_)
				{
					++counters_.missing;
					return out;   // no second producer inside the frame
				}
				const VpSkeleton::SealResult s = sealResult_;
				if (s.outcome == VpSkeleton::SealOutcome::Unavailable)
				{
					const bool stale = s.reason == VpSkeleton::SealReason::Overrun || s.reason == VpSkeleton::SealReason::OrderError;
					out.status = stale ? GrainStatus::Stale : GrainStatus::Missing;
					out.reason = s.reason;
					++(stale ? counters_.stale : counters_.missing);
					return out;
				}
				const int idx = slots_.BeginLive(seq);
				if (idx < 0)
				{
					out.reason = VpSkeleton::SealReason::NoLiveBuffer;
					++counters_.missing;
					return out;
				}
				GrainTarget target;
				target.live = true;
				target.index = idx;
				if (!compute.Fill(seq, target))
				{
					slots_.CancelLive(seq);
					failed_ = true;
					failedSeq_ = seq;
					++counters_.fillFailures;
					++counters_.missing;
					out.reason = VpSkeleton::SealReason::FillDeclined;
					return out;
				}
				VpSkeleton::Header hdr;
				hdr.prov = VpSkeleton::Provenance::LiveRender;
				if (!slots_.CommitLive(seq, hdr))
				{
					slots_.CancelLive(seq);
					failed_ = true;
					failedSeq_ = seq;
					++counters_.missing;
					out.reason = VpSkeleton::SealReason::FillDeclined;
					return out;
				}
				++counters_.liveFills;
				lastFiller_ = consumer ? consumer : "";
				out.status = GrainStatus::Live;
				out.index = idx;
				out.filledHere = true;
				out.reason = VpSkeleton::SealReason::Accepted;
				return out;
			}
			out.reason = VpSkeleton::SealReason::Accepted;
			out.index = r.index;
			if (r.kind == VpSkeleton::ReadInfo::Ring)
			{
				out.status = GrainStatus::Ring;
				++counters_.ringReads;
			}
			else
			{
				out.status = GrainStatus::Live;
				++counters_.liveReuses;
			}
			return out;
		}

		// After the frame's last use by any consumer.
		bool Release(uint64_t seq, uint64_t releaseValue = 0)
		{
			if (sealedSeq_ == seq)
				sealed_ = false;
			if (failedSeq_ == seq)
				failed_ = false;
			return slots_.Release(seq, releaseValue);
		}

	private:
		VpSkeleton::SlotSet slots_;
		GrainDesc desc_;
		NodeCounters counters_;
		VpSkeleton::SealResult sealResult_;
		uint64_t sealedSeq_ = 0;
		bool sealed_ = false;
		uint64_t failedSeq_ = 0;
		bool failed_ = false;
		bool shed_ = false;
		std::string lastFiller_;
	};

	// The visible fallback of one reader (FallbackCatalog entry "grain split (<consumer>)"): engaged while the
	// shared product is not Ok for it (the reader runs its own reduced computation or goes off), released by
	// itself on the first frame it is Ok again.
	inline FallbackCatalog::Decision ReaderFallback(const char* consumer, GrainStatus status)
	{
		const std::string id = std::string("grain split (") + (consumer ? consumer : "?") + ")";
		const char* why = nullptr;
		switch (status)
		{
		case GrainStatus::Ring:
		case GrainStatus::Live:
			return FallbackCatalog::Released(id.c_str());
		case GrainStatus::Missing: why = "shared grain split not available: estimating its own grain (reduced)"; break;
		case GrainStatus::Stale: why = "shared grain split is from another frame: estimating its own grain (reduced)"; break;
		case GrainStatus::WrongSize: why = "shared grain split does not fit this plane: estimating its own grain (reduced)"; break;
		case GrainStatus::Shed: why = "shared grain split dropped (not enough free video memory): estimating its own grain (reduced)"; break;
		}
		FallbackCatalog::Decision d;
		d.id = id;
		d.engaged = true;
		d.reason = why ? why : "shared grain split unavailable";
		return d;
	}
}
