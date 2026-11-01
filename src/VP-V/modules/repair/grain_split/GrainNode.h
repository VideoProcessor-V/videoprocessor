// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

	enum class LaneOutcome : uint8_t { Published, Late, Skipped, WriteFailed, Shed };

	struct NodeCounters
	{
		uint64_t laneComputes = 0;   // fills by the lane
		uint64_t liveFills = 0;      // the single live fill per frame, by its first consumer
		uint64_t ringReads = 0;      // reads served from the sealed ring buffer
		uint64_t liveReuses = 0;     // reads served from an existing live fill (later consumers)
		uint64_t fillFailures = 0;
		uint64_t missing = 0, stale = 0, wrongSize = 0, shed = 0;
		uint64_t shedClaims = 0;     // lane claims refused because the governor dropped the product
	};

	class GrainNode
	{
	public:
		GrainNode(VpSkeleton::ProductId id, uint16_t ring, uint16_t livePool, const GrainDesc& desc)
			: slots_(id, ring, livePool), desc_(desc), frames_(slots_.Ring()) {}
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
			{
				++counters_.shedClaims;
				return VpSkeleton::ClaimResult::TooEarly;   // a dropped product is not precomputed (LanePrecompute: Shed)
			}
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
			if (shed_)
			{
				++counters_.shedClaims;
				return LaneOutcome::Shed;
			}
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
			FrameState& f = State(seq);
			if (!(f.sealed && f.seq == seq))
				f = FrameState();   // a new frame in this ring slot: its own record
			if (r.reason != VpSkeleton::SealReason::AlreadySealed)
				f.sealResult = r;
			f.seq = seq;
			f.sealed = true;
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
			if (!(State(seq).sealed && State(seq).seq == seq))
				Seal(seq);
			FrameState& fs = State(seq);
			VpSkeleton::ReadInfo r = slots_.Read(seq);
			if (r.kind == VpSkeleton::ReadInfo::None)
			{
				if (fs.failed)
				{
					++counters_.missing;
					return out;   // no second producer inside the frame
				}
				const VpSkeleton::SealResult s = fs.sealResult;
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
					fs.failed = true;
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
					fs.failed = true;
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
			FrameState& f = State(seq);
			if (f.seq == seq)
				f = FrameState();
			return slots_.Release(seq, releaseValue);
		}

	private:
		// The render-side record of one frame, per ring slot (indexed by SlotOf(seq), stamped with seq): two frames in
		// flight never share a seal result or a fill-failed flag.
		struct FrameState
		{
			uint64_t seq = ~uint64_t(0);
			bool sealed = false;
			bool failed = false;
			VpSkeleton::SealResult sealResult;
		};
		FrameState& State(uint64_t seq) { return frames_[slots_.SlotOf(seq)]; }

		VpSkeleton::SlotSet slots_;
		GrainDesc desc_;
		NodeCounters counters_;
		std::vector<FrameState> frames_;
		bool shed_ = false;
		std::string lastFiller_;
	};

	// The layout of one P10 buffer for the plane `desc` describes, for the GRANTED storage. Full: split rgba32f + blocks
	// rgba32f; Compact: A rg16f + B rg16 + blocks rgba32f (N, e3, e1, e2). The block grid is ABSOLUTE: ceil(w / B) x
	// ceil(h / B) of the node's real block edge (the declaration counts it at the smallest edge, 16 px, an upper bound:
	// ProductTexLayout::MatchesDecl matches per shift and bounds the grid per axis).
	inline VpSkeleton::ProductTexLayout GrainLayout(Storage storage, const GrainDesc& desc)
	{
		using VpSkeleton::TexFormat;
		VpSkeleton::ProductTexLayout l;
		auto term = [&l](uint8_t shift, TexFormat f, uint32_t absW, uint32_t absH) {
			VpSkeleton::TexTerm t;
			t.shift = shift;
			t.fmt = f;
			t.absW = absW;
			t.absH = absH;
			l.terms.push_back(t);
		};
		const int B = desc.B > 0 ? desc.B : 1;
		const uint32_t bw = static_cast<uint32_t>((desc.w + B - 1) / B), bh = static_cast<uint32_t>((desc.h + B - 1) / B);
		if (storage == Storage::Compact)
		{
			term(0, TexFormat::RG16F, 0, 0);
			term(0, TexFormat::RG16, 0, 0);
			term(4, TexFormat::RGBA32F, bw, bh);
		}
		else
		{
			term(0, TexFormat::RGBA32F, 0, 0);
			term(4, TexFormat::RGBA32F, bw, bh);
		}
		return l;
	}

	// The P10 declaration of the GRANTED storage (finisher MUST 2): the governor plans, and ProductTextures checks, what is
	// really allocated. Built from the one core declaration (VpSkeleton::AppendGrainSplitProducts), never a private copy.
	inline VpSkeleton::ProductDecl GrainDecl(Storage granted)
	{
		return VpSkeleton::SkeletonProducts(granted == Storage::Compact).Get(VpSkeleton::kP10GrainSplit);
	}
	// The skeleton registry for the GRANTED storages (what the tier plan / governor must be built from): luma (P10) and the
	// chroma planes (P10c) are granted separately (ChooseStorage per plane).
	inline VpSkeleton::Registry GrainRegistryFor(Storage grantedLuma, Storage grantedChroma)
	{
		return VpSkeleton::SkeletonProducts(grantedLuma == Storage::Compact, grantedChroma == Storage::Compact);
	}
	inline VpSkeleton::Registry GrainRegistryFor(Storage granted) { return GrainRegistryFor(granted, granted); }

	// THE EXPLICIT LAYOUT (owner decision): the node stores ONE main map plus small MODIFIER fields; consumers derive every
	// other map from them cheaply, nobody recomputes a grain estimate. Base = the per-pixel picture split (clean = base + s1 + s2,
	// grain G = Y - clean); modifiers = what steers a derivation (per-pixel Wiener share f, per-block grain level N, per-block
	// focus-band energy e3). Both layouts carry the same fields (Full: split rgba32f (s1, s2, f, base) + blocks rgba32f (N, e3, -, -);
	// Compact: A rg16f (s1, s2), B rg16 (f, base x codeUnit), blocks rgba32f (N, e3, e1, e2)); read only through gs_fetch / gs_blocks
	// / gs_energies / gs_tilt (the GPU field table is GrainSplit::Gpu::GrainFieldTable).
	// The block texel's last two channels in Full are RESERVED for further modifiers (band energies E1/E2, spectral shape
	// descriptors): they are not written yet, and a new modifier bumps kUnitVersion and extends this table.
	enum class FieldKind : uint8_t { Base, Modifier };

	struct FieldSpec
	{
		const char* name;
		FieldKind kind;
		const char* where;       // texture and channel (Full / Compact)
		const char* derivedBy;   // who derives what from it
	};

	inline const FieldSpec* GrainFields(size_t* count)
	{
		static const FieldSpec kFields[] = {
			{ "base", FieldKind::Base, "split.w / B.y / cu", "temporal detail, chroma-from-luma: clean = base + s1 + s2 (the grain-free picture they work on)" },
			{ "s1", FieldKind::Base, "split.x / A.x", "temporal detail, chroma-from-luma: clean picture; G = Y - clean is added back exactly" },
			{ "s2", FieldKind::Base, "split.y / A.y", "temporal detail, chroma-from-luma: clean picture; region evidence: mid-band energy E2 per block" },
			{ "f", FieldKind::Modifier, "split.z / B.x", "per-pixel Wiener share: texture restore (SA8) splits grain from texture with it; region evidence: texture and ringing maps" },
			{ "N", FieldKind::Modifier, "blocks.x", "per-block grain level (codes^2): temporal detail guard and measurement, region evidence (grain sigma, texture = E1 + E2 - N, banding), debander (SA5) grain gate, camera model grain map" },
			{ "e3", FieldKind::Modifier, "blocks.y", "per-block focus-band energy: temporal detail focus evidence, texture restore (SA8) acutance" },
			{ "e1", FieldKind::Modifier, "blocks.z (Compact)", "per-block fine-band energy: temporal detail MEAS E1, detail pulse (SA5) E1, adaptive downscale (SA8) grain map" },
			{ "e2", FieldKind::Modifier, "blocks.w (Compact)", "per-block mid-band energy: temporal detail MEAS E2, detail pulse (SA5) E2; tilt = log2(e1 / e2) (derived by gs_tilt): camera grain-map refinement (K5)" },
		};
		if (count) *count = sizeof(kFields) / sizeof(kFields[0]);
		return kFields;
	}

	// The ledger comparison (Ctrl+Alt+D): the PEAK bytes of the grain split per storage layout, scratch included.
	// Full: slots at 16 B/px (+ 16 B per block) and a bands scratch of 16 B/px (rows is the product itself).
	// Compact: slots at 8 B/px (+ 8 B per block) and a rows + bands scratch of 32 B/px. So with N = ring + live slots
	// compact is 8N + 32 B/px (+ 16 B per block) against full's 16N + 16: it saves only when N > 2.
	inline std::vector<std::string> StorageLedgerLines(VpSkeleton::Dims d, unsigned slotsRingPlusLive, int blockShift = 4)
	{
		const uint64_t px = VpSkeleton::CellsAt(d, 0), blocks = VpSkeleton::CellsAt(d, static_cast<uint8_t>(blockShift));
		const uint64_t fullSlot = px * 16 + blocks * 16, compactSlot = px * 8 + blocks * 16;
		const uint64_t fullScratch = px * 16 + blocks * 16, compactScratch = px * 32 + blocks * 16;
		const uint64_t fullPeak = fullSlot * slotsRingPlusLive + fullScratch, compactPeak = compactSlot * slotsRingPlusLive + compactScratch;
		std::vector<std::string> lines;
		lines.push_back("VRAM grain split full: " + VpSkeleton::FormatMB(fullPeak) + " (16 x " + std::to_string(slotsRingPlusLive) +
			" + 16 B/px incl. scratch)");
		lines.push_back("VRAM grain split compact: " + VpSkeleton::FormatMB(compactPeak) + " (8 x " + std::to_string(slotsRingPlusLive) +
			" + 32 B/px incl. scratch)" + std::string(compactPeak < fullPeak ? ", saves " + VpSkeleton::FormatMB(fullPeak - compactPeak) :
			std::string(", saves nothing (ring + live <= 2)")));
		return lines;
	}

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
