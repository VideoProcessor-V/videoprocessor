// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the OWNER FILL of kFocusMap (CPU form). Every input is obtained through Acquire, which publishes
// anything missing (the recursive rule); the fill measures nothing itself and keeps no state.
//   P11 (seq)      the block statistics (bands E1 / E2 / E3 and N are inside them)  -> required, else the fill declines (visible)
//   P11 (seq - 1)  the previous frame's statistics: the focus-change reference       -> absent = kNoPrevious
//   P10 (seq)      the grain split: Acquired only to settle that the band energies are real; unavailable = kNoSplit on every block
//   P2  (seq)      the backward vector grid (FocusMap::kP2BytesPerCell per cell)      -> absent = P7 below
//   P7  (seq)      the camera translation (FocusMap::kP7Bytes)                          -> absent = zero motion + kNoMotion (visible)
// A previous-frame defocus is a pure function of P11(seq - 1), so the map of a frame never depends on which frame was asked first.
// The frame geometry is supplied by the host (the source geometry product); the fill refuses without it.
#pragma once

#include "FocusMap.h"

#include <VP-V/core/SkeletonAcquire.h>

#include <functional>

namespace FocusMap
{
	struct FillConfig
	{
		// The source frame size of `seq` (false = unknown: the fill declines).
		std::function<bool(uint64_t seq, int& width, int& height)> geometry;
		Params params;
		int p2CellPx = 8;
		// Counters for the tests and the status line (written by the fill).
		struct Stats
		{
			uint64_t fills = 0, noPrevious = 0, noMotion = 0, cameraMotion = 0, noSplit = 0;
		};
		std::shared_ptr<Stats> stats = std::make_shared<Stats>();
	};

	inline bool CopyP11(const VpSkeleton::AcquireResult& r, size_t blocks, std::vector<float>& out)
	{
		const size_t need = blocks * RegionEvidence::kPrecursorChannels * sizeof(float);
		if (!r.Ok() || !r.cpu || r.cpuBytes < need) return false;
		out.resize(blocks * RegionEvidence::kPrecursorChannels);
		std::memcpy(out.data(), r.cpu, need);
		return true;
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
			const Geometry g = Geometry::Make(w, h, cfg.p2CellPx);

			std::vector<float> cur, prev;
			const AcquireResult p11 = memo.Acquire(kP11RegionBlockStats, req.seq, AcquireForm::Cpu);
			if (!CopyP11(p11, g.Blocks(), cur)) return false;   // declined: P11 itself is unavailable (its own cause is counted)
			bool havePrev = false;
			if (req.seq > 0)
			{
				const AcquireResult pp = memo.Acquire(kP11RegionBlockStats, req.seq - 1, AcquireForm::Cpu);
				havePrev = CopyP11(pp, g.Blocks(), prev);
			}
			const bool split = memo.Acquire(kP10GrainSplit, req.seq, AcquireForm::Cpu).Ok();

			std::vector<uint8_t> p2;
			const AcquireResult r2 = memo.Acquire(kP2VectorPair, req.seq, AcquireForm::Cpu);
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
			ComputeGrid(cfg.params, g, cur.data(), havePrev ? prev.data() : nullptr, haveP2 ? p2.data() : nullptr, haveP7 ? p7 : nullptr, !split, blocks);
			out.bytes.assign(blocks.size() * kBytesPerBlock, 0);
			uint64_t np = 0, nm = 0, nc = 0, ns = 0;
			for (size_t i = 0; i < blocks.size(); ++i)
			{
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
			}
			return true;
		};
		f.variantAxes = 0;   // the default variant only
		f.reserveBytes = 0;
		return f;
	}

	// The catalog's owning unit of kFocusMap (ProductCatalog.h ProductRows).
	constexpr const char* kUnitPath = "input/source_analysis/focus_map";
}
