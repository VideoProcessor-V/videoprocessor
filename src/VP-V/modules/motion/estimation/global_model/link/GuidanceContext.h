// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-2/3): the PairContext provider of the guidance scheduler, built from the three things the host knows about pair
// N that are not in P7: the P4(N) scene facts, the vector engine's slip answer and (optionally) its CPU flow. ONE function builds
// the context for the lane (camera worker) and the live fill (render thread), so the two see the same inputs.
//   * P4Reader   reads P4(N) from a PUBLISHED / pinned slot (never render-thread-only state) and fills the scene part of the
//                context; false = P4(N) is not published yet (the lane job asks again later; the live fill treats it as "record late").
//                It also carries the stamp's ancient flag.
//   * engine     IVectorEngine::PairSpansSlip(N-d, N) and, when the flow is CPU-resident, FlowOf(N) for temporalSeed. P5a must make
//                these two calls safe to make from the camera worker (read-only over the sealed ring); null engine = no slip info
//                and no flow (the context is still built).
//   * temporalSeed input: the engine flow is resampled onto the RECORD's block grid by ResampleFlowToGrid (source px per cell
//                -> the record's blocks, nearest cell at the block centre). A flow that cannot be resampled is dropped (the plan
//                then reports "no temporal seed input"), never guessed.
// Pure apart from the calls it is given. C++14, header-only, deterministic.
#pragma once

#include "GuidanceScheduler.h"

#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace CameraLink
{
	// Reads P4(N) for the guide lane and fills the SCENE part of the context: cut / fade / flash (and the fade's gain and offset),
	// nextPairSeconds (the measured duration of the pair about to be searched; <= 0 unknown) and ancientStamp
	// (Indicator::StampOf(N).ancient). The reader leaves spansSlip and the flow to the provider. False = P4(N) not published yet.
	using P4Reader = std::function<bool(uint64_t pairSeq, PairContext& ctx)>;

	// The engine's flow (forward, source px over the pair; one vector per cell of `v.blockSize` source px) onto a block grid:
	// block (bx, by) takes the cell under its centre. False (outputs cleared) when the view or the grid is unusable.
	inline bool ResampleFlowToGrid(const VpMotion::FlowView& v, int gridW, int gridH, double blockSize,
		std::vector<float>& x, std::vector<float>& y)
	{
		x.clear();
		y.clear();
		if (!v.x || !v.y || v.gridW <= 0 || v.gridH <= 0 || !(v.blockSize > 0) || gridW <= 0 || gridH <= 0 || !(blockSize > 0))
			return false;
		x.resize(static_cast<size_t>(gridW) * static_cast<size_t>(gridH));
		y.resize(x.size());
		for (int by = 0; by < gridH; ++by)
		{
			int cy = static_cast<int>(std::floor((by + 0.5) * blockSize / v.blockSize));
			cy = cy < 0 ? 0 : cy >= v.gridH ? v.gridH - 1 : cy;
			for (int bx = 0; bx < gridW; ++bx)
			{
				int cx = static_cast<int>(std::floor((bx + 0.5) * blockSize / v.blockSize));
				cx = cx < 0 ? 0 : cx >= v.gridW ? v.gridW - 1 : cx;
				const size_t src = static_cast<size_t>(cy) * static_cast<size_t>(v.gridW) + static_cast<size_t>(cx);
				const size_t dst = static_cast<size_t>(by) * static_cast<size_t>(gridW) + static_cast<size_t>(bx);
				x[dst] = v.x[src];
				y[dst] = v.y[src];
			}
		}
		return true;
	}

	// The provider for GuidanceScheduler::SetContextProvider. `lag` returns d (the scheduler's worker-side SnapshotLag()).
	inline std::function<bool(uint64_t, PairContext&)> MakeContextProvider(VpMotion::IVectorEngine* engine, ICameraLink& link,
		P4Reader p4, std::function<uint8_t()> lag)
	{
		ICameraLink* l = &link;
		return [engine, l, p4, lag](uint64_t pair, PairContext& ctx) -> bool
		{
			ctx = PairContext();
			if (!p4 || !p4(pair, ctx))
				return false;   // P4(N) not published yet
			ctx.spansSlip = false;   // the engine part below is the provider's, whatever the reader left there
			ctx.flowCpuResident = false;
			ctx.nvofX.clear();
			ctx.nvofY.clear();
			const uint64_t d = lag ? lag() : 2;
			if (!engine || pair < d)
				return true;
			ctx.spansSlip = engine->PairSpansSlip(pair - d, pair);
			if (engine->FlowIsCpuResident())
			{
				VpMotion::FlowView view;
				const std::shared_ptr<const CameraModel::CameraMotion> rec = l->CausalRecord(pair - d);
				if (rec && rec->blocks && engine->FlowOf(pair, view) &&
					ResampleFlowToGrid(view, rec->blocks->gridW, rec->blocks->gridH, rec->blocks->blockSize, ctx.nvofX, ctx.nvofY))
					ctx.flowCpuResident = true;
				else
				{
					ctx.nvofX.clear();
					ctx.nvofY.clear();
				}
			}
			return true;
		};
	}
}
