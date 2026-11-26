// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the OWNER FILL of kFocusMap (CPU form, S18 F1 edge-map-only law at lag >= 1).
// Every input is obtained through Acquire; the fill measures nothing itself and keeps no state.
//   kEdgeMap (seq - 1) REQUIRED sharpness input (lag 1 ONLY; NEVER lag 0 — async GPU -> CPU readback has >= 1 frame latency):
//                      - seq == 0: declined visibly (no prior frame).
//                      - AcquireCause::ReadbackPending: propagated as non-latched out.pending = true (retries on next Acquire).
//                      - Any other unavailable cause or wrong byte size: declined visibly (NO v1 interim fallback!).
//   kEdgeMap (seq - 2) previous-frame sharpness reference (lag 2, when seq >= 2):
//                      - ReadbackPending: propagated as non-latched out.pending = true.
//                      - Absent: kNoPrevious; RELEASED: kNoPrevious + kNoPreviousRetained.
//   P11 (seq - 1)      frame-aligned block statistics (bands E1 / E2 / E3 and N) -> required, else declined visibly.
//   P11 (seq - 2)      previous-frame block statistics -> absent = kNoPrevious; RELEASED = kNoPrevious + kNoPreviousRetained.
//   P2  (seq - 1)      backward vector grid -> absent = P7(seq - 1) below.
//   P7  (seq - 1)      camera translation -> absent = zero motion + kNoMotion (visible).
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
		// Optional active picture rect [x0, x1) x [y0, y1) of `seq` in source pixels (false / null = full frame).
		std::function<bool(uint64_t seq, int& x0, int& y0, int& x1, int& y1)> activeRect;
		Params params;
		int p2CellPx = 8;
		int p2CellPy = 0;         // 0 = isotropic (= p2CellPx); non-zero = vertical P2 cell size (A-lens / anisotropic)
		double p2ScaleX = 0.0;    // 0 = derive from p2Cell / 4.0
		double p2ScaleY = 0.0;    // 0 = derive from p2CellY / 4.0
		// Counters for tests and the status line (atomic: written on asking thread, read by status).
		// fills / noPreviousRetained / edgeLaw / edgePending / edgeUnavailable count FILLS; flag counters count BLOCKS.
		struct Stats
		{
			std::atomic<uint64_t> fills{ 0 }, noPrevious{ 0 }, noMotion{ 0 }, cameraMotion{ 0 }, noSplit{ 0 };
			std::atomic<uint64_t> noPreviousRetained{ 0 };
			std::atomic<bool> lastNoPreviousRetained{ false };
			std::atomic<uint64_t> edgeLaw{ 0 }, edgePending{ 0 }, edgeUnavailable{ 0 };
			std::atomic<bool> lastEdgeUnavailable{ false };
			std::atomic<uint8_t> lastEdgeCause{ 0 };
			std::atomic<uint8_t> lastPendingCause{ 0 };
		};
		std::shared_ptr<Stats> stats = std::make_shared<Stats>();
	};

	// P11 CPU form: the reference publishes doubles (RegionEvidence::Precursors::data), a GPU read-back publishes floats. The element width is
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
			if (req.form != AcquireForm::Cpu || !cfg.geometry)
			{
				out.declineReason = "focus_map: requires CPU form and frame geometry";
				return false;
			}
			// S18 F1: kEdgeMap CPU readback is ALWAYS read at lag >= 1 (NEVER lag 0).
			if (req.seq == 0)
			{
				out.declineReason = "focus_map: requires lag >= 1 edge map (frame 0 has no prior frame)";
				if (cfg.stats)
				{
					++cfg.stats->edgeUnavailable;
					cfg.stats->lastEdgeUnavailable.store(true);
					cfg.stats->lastEdgeCause.store(static_cast<uint8_t>(AcquireCause::OwnerDeclined));
					cfg.stats->lastPendingCause.store(static_cast<uint8_t>(AcquireCause::None));
				}
				return false;
			}
			const uint64_t sampleSeq = req.seq - 1;
			const bool havePrevSeq = sampleSeq > 0;
			const uint64_t prevSeq = havePrevSeq ? sampleSeq - 1 : uint64_t(0);

			// Validate lag-1 frame geometry using sampleSeq (req.seq - 1), NOT req.seq.
			int w = 0, h = 0;
			if (!cfg.geometry(sampleSeq, w, h) || w <= 0 || h <= 0)
			{
				out.declineReason = "focus_map: unknown frame geometry";
				return false;
			}

			// Acquire lag-2 (prevSeq) BEFORE lag-1 (sampleSeq) so taking sampleSeq never stale-drops an
			// unconsumed prevSeq in any ReadbackRing (ascending seq order; defense-in-depth with keepOlder).
			const AcquireResult rp = havePrevSeq ? memo.Acquire(kEdgeMap, prevSeq, AcquireForm::Cpu) : AcquireResult();
			const AcquireResult re = memo.Acquire(kEdgeMap, sampleSeq, AcquireForm::Cpu);

			// Check ReadbackPending on BOTH lag-1 and lag-2 BEFORE any P11 copy or edge-map reduction.
			if (!re.Ok() && re.cause == AcquireCause::ReadbackPending)
			{
				out.pending = true;
				out.declineReason = "focus_map: edge_map lag-1 CPU readback pending";
				if (cfg.stats)
				{
					++cfg.stats->edgePending;
					cfg.stats->lastPendingCause.store(static_cast<uint8_t>(AcquireCause::ReadbackPending));
				}
				return false;
			}
			if (havePrevSeq && !rp.Ok() && rp.cause == AcquireCause::ReadbackPending)
			{
				out.pending = true;
				out.declineReason = "focus_map: edge_map lag-2 CPU readback pending";
				if (cfg.stats)
				{
					++cfg.stats->edgePending;
					cfg.stats->lastPendingCause.store(static_cast<uint8_t>(AcquireCause::ReadbackPending));
				}
				return false;
			}

			// Edge-map-only law (v2, zero v1 interim fallback): accept either GPU-reduced EMBS block sums or full-res R32F.
			const Geometry gBase = Geometry::Make(w, h, cfg.p2CellPx, cfg.p2CellPy, cfg.p2ScaleX, cfg.p2ScaleY);
			if (!re.Ok() || !IsValidEdgePayload(gBase, re.cpu, re.cpuBytes))
			{
				out.pending = false;
				const AcquireCause c = re.Ok() ? AcquireCause::None : re.cause;
				out.declineReason = std::string("focus_map: edge_map lag-1 CPU unavailable (")
					+ (c == AcquireCause::None ? std::string("served at wrong size") : std::string(Token(c))) + ")";
				if (cfg.stats)
				{
					++cfg.stats->edgeUnavailable;
					cfg.stats->lastEdgeUnavailable.store(true);
					cfg.stats->lastEdgeCause.store(static_cast<uint8_t>(c));
					cfg.stats->lastPendingCause.store(static_cast<uint8_t>(AcquireCause::None));
				}
				return false;
			}

			// Frame-aligned P2 and P11 at sampleSeq (lag 1).
			const AcquireResult r2 = memo.Acquire(kP2VectorPair, sampleSeq, AcquireForm::Cpu);
			const int p2CellX = (r2.Ok() && cfg.p2CellPy == 0) ? P2CellOf(r2.cpuBytes, w, h, cfg.p2CellPx) : cfg.p2CellPx;
			Geometry g = Geometry::Make(w, h, p2CellX, cfg.p2CellPy, cfg.p2ScaleX, cfg.p2ScaleY);
			if (cfg.activeRect)
			{
				int ax0 = 0, ay0 = 0, ax1 = w, ay1 = h;
				if (cfg.activeRect(sampleSeq, ax0, ay0, ax1, ay1))
					g.SetActiveRect(ax0, ay0, ax1, ay1);
			}

			std::vector<float> cur, prev;
			const AcquireResult p11 = memo.Acquire(kP11RegionBlockStats, sampleSeq, AcquireForm::Cpu);
			if (!CopyP11(p11, g.Blocks(), cur))
			{
				out.declineReason = "focus_map: P11 lag-1 unavailable";
				return false;
			}

			std::vector<float> e2, e4, pe2, pe4;
			EdgeInput edge, prevEdge;
			DecodeEdgePayload(g, re.cpu, re.cpuBytes, e2, e4);
			edge.sum2 = e2.data();
			edge.sum4 = e4.data();

			// Previous-frame delta reference at prevSeq (lag 2, when req.seq >= 2 and same frame geometry).
			bool havePrev = false, prevReleased = false;
			if (havePrevSeq)
			{
				int pw = 0, ph = 0;
				const bool prevGeomSame = cfg.geometry(prevSeq, pw, ph) && pw == w && ph == h;
				const AcquireResult pp = prevGeomSame ? memo.Acquire(kP11RegionBlockStats, prevSeq, AcquireForm::Cpu) : AcquireResult();
				Geometry gPrev = g;
				if (prevGeomSame)
				{
					// W23-NEON-L2: reset gPrev to full-frame when cfg.activeRect returns false (never inherit sampleSeq's crop).
					gPrev.SetActiveRect(0, 0, pw, ph);
					if (cfg.activeRect)
					{
						int pax0 = 0, pay0 = 0, pax1 = pw, pay1 = ph;
						if (cfg.activeRect(prevSeq, pax0, pay0, pax1, pay1))
							gPrev.SetActiveRect(pax0, pay0, pax1, pay1);
					}
				}
				const bool havePrevEdge = prevGeomSame && rp.Ok() && IsValidEdgePayload(gPrev, rp.cpu, rp.cpuBytes);
				const bool havePrevP11 = prevGeomSame && CopyP11(pp, g.Blocks(), prev);
				if (havePrevEdge && havePrevP11)
				{
					DecodeEdgePayload(gPrev, rp.cpu, rp.cpuBytes, pe2, pe4);
					prevEdge.sum2 = pe2.data();
					prevEdge.sum4 = pe4.data();
					havePrev = true;
				}
				else if (prevGeomSame)
				{
					prevReleased = PreviousReleased(rp) || PreviousReleased(pp);
				}
			}

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
				const AcquireResult r7 = memo.Acquire(kP7CameraFit, sampleSeq, AcquireForm::Cpu);
				if (r7.Ok() && r7.cpu && r7.cpuBytes >= kP7Bytes)
				{
					std::memcpy(p7, r7.cpu, sizeof(p7));
					haveP7 = std::isfinite(p7[0]) && std::isfinite(p7[1]) && std::isfinite(p7[3]) && p7[3] >= 0.5f;
				}
			}

			std::vector<Block> blocks;
			ComputeGrid(cfg.params, g, cur.data(), havePrev ? prev.data() : nullptr, haveP2 ? p2.data() : nullptr, haveP7 ? p7 : nullptr, false, blocks,
				edge, prevEdge);
			out.bytes.assign(blocks.size() * kBytesPerBlock, 0);
			uint64_t np = 0, nm = 0, nc = 0, ns = 0;
			for (size_t i = 0; i < blocks.size(); ++i)
			{
				if (prevReleased && (blocks[i].flags & kNoPrevious) != 0)   // the cause of this block's kNoPrevious: retention, not absence
					blocks[i].flags = static_cast<uint16_t>(blocks[i].flags | kNoPreviousRetained);
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
				++cfg.stats->edgeLaw;
				cfg.stats->lastEdgeUnavailable.store(false);
				cfg.stats->lastEdgeCause.store(static_cast<uint8_t>(AcquireCause::None));
				cfg.stats->lastPendingCause.store(static_cast<uint8_t>(AcquireCause::None));
			}
			return true;
		};
		f.variantAxes = 0;   // the default variant only
		f.reserveBytes = 0;
		return f;
	}

	// The catalog's owning unit of kFocusMap (ProductCatalog.h ProductRows).
	constexpr const char* kUnitPath = "input/source_analysis/focus_map";

	// The focus map's own Fallbacks line ("product.focus_map", FallbackCatalog ProductEntries): engaged when the owner fill is
	// not registered, when the latest fill lost its lag-2 reference because the previous spot was released (retention missing),
	// or when the latest fill declined because the lag-1 kEdgeMap CPU form was unavailable (no v1 interim fallback). Released
	// once an edge-law fill succeeds with retention intact.
	inline FallbackCatalog::Decision StatusDecision(const FillConfig::Stats* s, bool registered, const std::string& registrationWhy = std::string())
	{
		if (!registered)
			return FallbackCatalog::ProductFallback("product.focus_map", "the focus map owner fill is not registered ("
				+ (registrationWhy.empty() ? std::string("refused") : registrationWhy) + ")");
		std::string why;
		if (s && s->lastNoPreviousRetained.load())
			why = "no focus-change reference: the lag-2 frame's spot was released (" + std::to_string(s->noPreviousRetained.load())
				+ " fill(s); its owner keeps fewer than 2 frames behind)";
		if (s && s->lastEdgeUnavailable.load())
		{
			const VpSkeleton::AcquireCause c = static_cast<VpSkeleton::AcquireCause>(s->lastEdgeCause.load());
			why += std::string(why.empty() ? "" : "; ") + "edge_map lag-1 CPU unavailable ("
				+ (c == VpSkeleton::AcquireCause::None ? std::string("served at wrong size") : std::string(VpSkeleton::Token(c))) + "; "
				+ std::to_string(s->edgeUnavailable.load()) + " unavailable fill(s))";
		}
		return why.empty() ? FallbackCatalog::Released("product.focus_map") : FallbackCatalog::ProductFallback("product.focus_map", why);
	}
}
