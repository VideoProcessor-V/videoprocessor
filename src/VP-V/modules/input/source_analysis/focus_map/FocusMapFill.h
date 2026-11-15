// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the OWNER FILL of kFocusMap (CPU form). Every input is obtained through Acquire, which publishes
// anything missing (the recursive rule); the fill measures nothing itself and keeps no state.
//   kEdgeMap (seq) the SHARPNESS input (owner 11-14 05:25; FOCUS-EDGEMAP-SWITCH, unit v2): the shared Sobel |G| per source pixel, reduced
//                  per P11 block (EdgeSums) -> the edge law. Not served on the CPU (C3: the edge map's only owner fill is the GPU form,
//                  regions/EdgeMapFill.h, so a CPU Acquire is NoOwnerFill) or of the wrong size = the INTERIM law (P11 band energies)
//                  for BOTH frames of the pair, kSharpnessInterim on every block, counted (Stats::sharpnessInterim) and on the
//                  Fallbacks list with the cause ("sharpness: P11 interim, edge map pending"). Never a private gradient.
//   kEdgeMap (seq - 1) the previous frame's, under the edge law only: absent = kNoPrevious (never compared across two laws);
//                  RELEASED = kNoPrevious + kNoPreviousRetained, as for P11 below
//   P11 (seq)      the block statistics (bands E1 / E2 / E3 and N are inside them)  -> required, else the fill declines (visible)
//   P11 (seq - 1)  the previous frame's statistics: the focus-change reference       -> absent = kNoPrevious; RELEASED (the P11 owner
//                  kept no spot for frame seq - 1: its owner fill must declare keepFrames >= 1, 102b FOCUS-P11-LAG1-RETENTION) =
//                  kNoPrevious + kNoPreviousRetained on every block, counted (Stats::noPreviousRetained) and on the Fallbacks list
//   P10 (seq)      the grain split: a DECLARED edge, not Acquired (audit SHOULD 4): P11 carries N = -1 when there was no split
//                  (RegionEvidence.h), which is kNoSplit on that block, so the full P10 CPU form is never forced only to set a flag
//   P2  (seq)      the backward vector grid (kP2BytesPerCell per cell; the cell edge is derived from the payload size) -> absent = P7 below
//   P7  (seq)      the camera translation (FocusMap::kP7Bytes)                          -> absent = zero motion + kNoMotion (visible)
// A previous-frame defocus is a pure function of P11(seq - 1) (and kEdgeMap(seq - 1) under the edge law), so the map of a frame never
// depends on which frame was asked first.
// The frame geometry is supplied by the host through FillConfig::geometry (SkeletonModule: the lane graph's frame size); the fill
// refuses without it.
#pragma once

#include "FocusMap.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SkeletonAcquire.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace FocusMap
{
	struct FillConfig
	{
		// The source frame size of `seq` (false = unknown: the fill declines).
		std::function<bool(uint64_t seq, int& width, int& height)> geometry;
		Params params;
		int p2CellPx = 8;
		// Counters for the tests and the status line (written by the fill on the asking thread, read by the status on another: atomic).
		// fills / noPreviousRetained count FILLS; the flag counters count BLOCKS. lastNoPreviousRetained is the latest fill's state
		// (the Fallbacks line is engaged while it holds and released when a fill is served its reference again).
		struct Stats
		{
			std::atomic<uint64_t> fills{ 0 }, noPrevious{ 0 }, noMotion{ 0 }, cameraMotion{ 0 }, noSplit{ 0 };
			std::atomic<uint64_t> noPreviousRetained{ 0 };
			std::atomic<bool> lastNoPreviousRetained{ false };
			// FOCUS-EDGEMAP-SWITCH: fills that used the edge law / the interim law, and the latest fill's state + the edge map's cause
			// (an AcquireCause value; AcquireCause::None with lastSharpnessInterim = the CPU form was served at the wrong size).
			std::atomic<uint64_t> edgeLaw{ 0 }, sharpnessInterim{ 0 };
			std::atomic<bool> lastSharpnessInterim{ false };
			std::atomic<uint8_t> lastEdgeCause{ 0 };
		};
		std::shared_ptr<Stats> stats = std::make_shared<Stats>();
	};

	// P11 CPU form: the reference publishes doubles (RegionEvidence::Evidence::data), a GPU read-back publishes floats. The element width is
	// told by the byte count (blocks x 24 x 8 or x 4); anything shorter is a declined input.
	inline bool CopyP11(const VpSkeleton::AcquireResult& r, size_t blocks, std::vector<float>& out)
	{
		const size_t n = blocks * RegionEvidence::kPrecursorChannels;
		if (!r.Ok() || !r.cpu) return false;
		out.resize(n);
		if (r.cpuBytes >= n * sizeof(double))
		{
			for (size_t i = 0; i < n; ++i)
			{
				double d;
				std::memcpy(&d, r.cpu + i * sizeof(double), sizeof(d));
				out[i] = static_cast<float>(d);
			}
			return true;
		}
		if (r.cpuBytes < n * sizeof(float)) return false;
		std::memcpy(out.data(), r.cpu, n * sizeof(float));
		return true;
	}

	// The P2 cell edge in source px follows the published grid: the candidate edge whose grid size matches the payload
	// (cfg.p2CellPx is the first candidate and the default when the payload does not identify one).
	inline int P2CellOf(size_t payloadBytes, int w, int h, int fallback)
	{
		if (payloadBytes % kP2BytesPerCell != 0) return fallback;
		const size_t cells = payloadBytes / kP2BytesPerCell;
		for (int c : { fallback, 4, 8, 16, 32 })
			if (c > 0 && static_cast<size_t>((w + c - 1) / c) * static_cast<size_t>((h + c - 1) / c) == cells) return c;
		return fallback;
	}

	// A previous-frame read that was refused because its frame's spot was already RELEASED (retention missing), as distinct from a
	// previous frame that is simply unavailable (first frame, owner declined, ...).
	inline bool PreviousReleased(const VpSkeleton::AcquireResult& r)
	{
		return !r.Ok() && r.cause == VpSkeleton::AcquireCause::FrameReleased;
	}

	inline VpSkeleton::OwnerFill MakeOwnerFill(const FillConfig& cfg)
	{
		using namespace VpSkeleton;
		OwnerFill f;
		f.fill = [cfg](const OwnerFillRequest& req, SkeletonMemo& memo, OwnerFillOutput& out) -> bool
		{
			if (req.form != AcquireForm::Cpu || !cfg.geometry) return false;
			int w = 0, h = 0;
			if (!cfg.geometry(req.seq, w, h) || w <= 0 || h <= 0) return false;
			const AcquireResult r2 = memo.Acquire(kP2VectorPair, req.seq, AcquireForm::Cpu);
			const Geometry g = Geometry::Make(w, h, r2.Ok() ? P2CellOf(r2.cpuBytes, w, h, cfg.p2CellPx) : cfg.p2CellPx);

			std::vector<float> cur, prev;
			const AcquireResult p11 = memo.Acquire(kP11RegionBlockStats, req.seq, AcquireForm::Cpu);
			if (!CopyP11(p11, g.Blocks(), cur)) return false;   // declined: P11 itself is unavailable (its own cause is counted)
			bool havePrev = false, prevReleased = false;
			if (req.seq > 0)
			{
				const AcquireResult pp = memo.Acquire(kP11RegionBlockStats, req.seq - 1, AcquireForm::Cpu);
				havePrev = CopyP11(pp, g.Blocks(), prev);
				prevReleased = PreviousReleased(pp);
			}

			// FOCUS-EDGEMAP-SWITCH: the sharpness input is the shared edge map, through Acquire (the owner's fill makes it; never a private
			// gradient). Only a CPU form of exactly width x height float32 is read; anything else is the interim law, visibly.
			const size_t edgeNeed = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(float);
			std::vector<float> e2, e4, pe2, pe4;
			EdgeInput edge, prevEdge;
			const AcquireResult re = memo.Acquire(kEdgeMap, req.seq, AcquireForm::Cpu);
			const bool edgeServed = re.Ok() && re.cpu != nullptr && re.cpuBytes == edgeNeed;
			if (edgeServed)
			{
				EdgeSums(g, re.cpu, e2, e4);
				edge.sum2 = e2.data();
				edge.sum4 = e4.data();
				if (havePrev)
				{
					const AcquireResult rp = memo.Acquire(kEdgeMap, req.seq - 1, AcquireForm::Cpu);
					if (rp.Ok() && rp.cpu != nullptr && rp.cpuBytes == edgeNeed)
					{
						EdgeSums(g, rp.cpu, pe2, pe4);
						prevEdge.sum2 = pe2.data();
						prevEdge.sum4 = pe4.data();
					}
					else
						prevReleased = prevReleased || PreviousReleased(rp);   // no reference under the edge law: ComputeGrid gives kNoPrevious
				}
			}
			const uint16_t interimFlag = edgeServed ? uint16_t(0) : uint16_t(kSharpnessInterim);

			std::vector<uint8_t> p2;
			const size_t p2Need = static_cast<size_t>(g.p2gw) * g.p2gh * kP2BytesPerCell;
			bool haveP2 = false;
			if (r2.Ok() && r2.cpu && r2.cpuBytes >= p2Need)
			{
				p2.assign(r2.cpu, r2.cpu + p2Need);
				haveP2 = true;
			}
			float p7[4] = { 0, 0, 0, 0 };
			bool haveP7 = false;
			if (!haveP2)
			{
				const AcquireResult r7 = memo.Acquire(kP7CameraFit, req.seq, AcquireForm::Cpu);
				if (r7.Ok() && r7.cpu && r7.cpuBytes >= kP7Bytes)
				{
					std::memcpy(p7, r7.cpu, sizeof(p7));
					haveP7 = p7[3] >= 0.5f;
				}
			}

			std::vector<Block> blocks;
			ComputeGrid(cfg.params, g, cur.data(), havePrev ? prev.data() : nullptr, haveP2 ? p2.data() : nullptr, haveP7 ? p7 : nullptr, false, blocks,
				edge, prevEdge);
			out.bytes.assign(blocks.size() * kBytesPerBlock, 0);
			uint64_t np = 0, nm = 0, nc = 0, ns = 0;
			for (size_t i = 0; i < blocks.size(); ++i)
			{
				if (prevReleased)   // the cause of this block's kNoPrevious: retention, not absence
					blocks[i].flags = static_cast<uint16_t>(blocks[i].flags | kNoPreviousRetained);
				blocks[i].flags = static_cast<uint16_t>(blocks[i].flags | interimFlag);
				Pack(blocks[i], out.bytes.data() + i * kBytesPerBlock);
				const uint16_t fl = blocks[i].flags;
				np += (fl & kNoPrevious) ? 1 : 0;
				nm += (fl & kNoMotion) ? 1 : 0;
				nc += (fl & kCameraMotion) ? 1 : 0;
				ns += (fl & kNoSplit) ? 1 : 0;
			}
			if (cfg.stats)
			{
				++cfg.stats->fills;
				cfg.stats->noPrevious += np;
				cfg.stats->noMotion += nm;
				cfg.stats->cameraMotion += nc;
				cfg.stats->noSplit += ns;
				if (prevReleased) ++cfg.stats->noPreviousRetained;
				cfg.stats->lastNoPreviousRetained.store(prevReleased);
				if (edgeServed) ++cfg.stats->edgeLaw; else ++cfg.stats->sharpnessInterim;
				cfg.stats->lastSharpnessInterim.store(!edgeServed);
				cfg.stats->lastEdgeCause.store(static_cast<uint8_t>(re.Ok() ? AcquireCause::None : re.cause));
			}
			return true;
		};
		f.variantAxes = 0;   // the default variant only
		f.reserveBytes = 0;
		return f;
	}

	// The catalog's owning unit of kFocusMap (ProductCatalog.h ProductRows).
	constexpr const char* kUnitPath = "input/source_analysis/focus_map";

	// The focus map's own Fallbacks line ("product.focus_map", FallbackCatalog ProductEntries), next to the memo's "skeleton acquire"
	// lines: engaged when the owner fill is NOT registered (the registration result is passed in: a refused registration is never
	// silent, audit SHOULD 5), when the latest fill had no previous-frame reference because its frame was released (retention missing,
	// MUST 3), or when the latest fill ran the INTERIM sharpness law because the edge map was not served on the CPU (FOCUS-EDGEMAP-SWITCH:
	// "sharpness: P11 interim, edge map pending", with the edge map's cause). Several causes are joined on the one line. Released otherwise.
	inline FallbackCatalog::Decision StatusDecision(const FillConfig::Stats* s, bool registered, const std::string& registrationWhy = std::string())
	{
		if (!registered)
			return FallbackCatalog::ProductFallback("product.focus_map", "the focus map owner fill is not registered ("
				+ (registrationWhy.empty() ? std::string("refused") : registrationWhy) + ")");
		std::string why;
		if (s && s->lastNoPreviousRetained.load())
			why = "no focus-change reference: the previous frame's spot was released (" + std::to_string(s->noPreviousRetained.load())
				+ " fill(s); its owner keeps no frame behind, keepFrames 0)";
		if (s && s->lastSharpnessInterim.load())
		{
			const VpSkeleton::AcquireCause c = static_cast<VpSkeleton::AcquireCause>(s->lastEdgeCause.load());
			why += std::string(why.empty() ? "" : "; ") + "sharpness: P11 interim, edge map pending (edge_map CPU form: "
				+ (c == VpSkeleton::AcquireCause::None ? std::string("served at another size") : std::string(VpSkeleton::Token(c))) + "; "
				+ std::to_string(s->sharpnessInterim.load()) + " interim fill(s))";
		}
		return why.empty() ? FallbackCatalog::Released("product.focus_map") : FallbackCatalog::ProductFallback("product.focus_map", why);
	}
}
