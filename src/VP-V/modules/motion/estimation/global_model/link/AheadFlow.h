// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera AHEAD FIT (owner decision, reset 1): the camera fit's input taken from the AHEAD motion, the NVOF pair the one producer
// computed ahead of render (P2, served by IVectorEngine::FlowOf), instead of the optical-flow seed the matcher read at render time
// (F2's SeedField). Pure, header-only, C++14, no GPU, no Windows.
//
// Provenance (F2 kept): the pair is the producer's UNGUIDED NVOF output, which the camera guidance never shapes. It reaches the fit only
// as an AheadCopy (a distinct type: no conversion from VerifiedField or SeedField), converted by AheadToSourceGrid into a FieldCopy whose
// origin is FieldOrigin::Ahead; CameraLinkCore::SubmitField accepts Seed / Ahead / Synthetic and still refuses Live (the guided field).
//
// Lifetime: FlowOf's view points into the engine's lease, valid only until the NEXT FlowOf call (VectorEngine.h, SK-MV). CopyAhead copies
// every array the fit needs at once, in the same call as FlowOf, so neither a later FlowOf nor a Prune of the P2 slot can free them.
//
// Layout (read from the view, never assumed): gridW x gridH cells of cellEnginePx x cellEnginePx ENGINE px (the producer's input, the
// matcher's searching size); vectors interleaved (x, y) per cell, row-major, signed fixed point (value / rawUnitsPerEnginePx = engine
// px; x enginePxToSource = source px); one cost byte per cell (higher = less sure). Today: S10.5 (32 units per px), 4 x 4 cells of the
// half-size luma, so a cell is 8 source px at a half-size search and 16 at a quarter-size (lowRes 4K) search.
//
// FULL NATIVE RESOLUTION (owner): AheadToSourceGrid gives the fit the NATIVE cell grid, one sample per cell, full sub-pel, never an
// aggregate; only the record's per-block OUTPUTS are put on the 16-px consumer grid (RegridToConsumerGrid). Cell trust = the engine's ONE trust definition, OpticalFlowTrust::CellTrust (the
// cost weight of the cell's de-judder block x the forward/backward consistency weight from the matcher's consistency limit), the same
// arithmetic as SK-MV MV5's TD source. Deterministic: fixed loop order, no threads, no hash or time input.
#pragma once

#include "FieldCopy.h"

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <VP-V/core/Indicator.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace CameraLink
{
	// The de-judder block the engine's cost weight is judged over, in engine px (MotionAhead::Flow::kBlockPx; pinned equal by
	// CameraAheadFitTests). The trust maths takes it as a parameter; it is the producer's FlowExtras block, not a camera choice.
	constexpr int kEngineTrustBlockPx = 8;

	// Why an ahead pair was not taken (counted per reason in AheadCounters, shown in the status line).
	enum class AheadRefusal : uint8_t
	{
		None,
		NoPair,        // FlowOf false (no producer, low latency, pair not computed / pruned / invalidated, size unknown)
		Incomplete,    // the view has no backward vectors or no forward costs: the engine trust cannot be computed (never guessed)
		Layout,        // the view's layout is inconsistent (units, cell size, grid) or does not cover the camera's 16-px source grid
		EpochMoved,    // G-A3: the engine's vectors-valid epoch changed around FlowOf (read before and after): the provenance is unknown
	};

	inline const char* Text(AheadRefusal r)
	{
		switch (r)
		{
		case AheadRefusal::None: return "taken";
		case AheadRefusal::NoPair: return "no ahead pair";
		case AheadRefusal::Incomplete: return "ahead pair without backward vectors or costs";
		case AheadRefusal::Layout: return "ahead pair layout does not cover the source grid";
		case AheadRefusal::EpochMoved: return "the vectors-valid epoch moved during FlowOf";
		}
		return "?";
	}

	// The ahead pair (seq - 1 -> seq), copied out of FlowOf. Owns its arrays. The only input AheadToSourceGrid accepts.
	struct AheadCopy
	{
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		int gridW = 0, gridH = 0;            // cells
		int cellEnginePx = 0;                // engine px per cell and axis
		int engineW = 0, engineH = 0;        // the engine-size luma
		double unitsPerEnginePx = 0;         // fixed-point units per engine px
		double enginePxToSource = 0;         // source px per engine px
		std::vector<int16_t> forward, backward;   // interleaved (x, y), gridW * gridH * 2
		std::vector<uint8_t> forwardCost;         // gridW * gridH
		VpIndicator::Stamp stamp;            // the Indicator's stamp of seq when the pair was taken (render thread)
		uint64_t epoch = 0;                  // G-A3: the engine's vectors-valid epoch FlowOf served the pair under (IVectorEngine::FieldEra)
		// The engine's whole-frame motion of the pair (source px), filled by the source the way the engine fills its own record
		// (MotionAhead::FlowGlobal); a fit prior like the render-time EngineGlobal. Not filled: no prior.
		bool globalValid = false;
		double globalX = 0, globalY = 0;
		size_t Cells() const { return static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0); }
		double CellSourcePx() const { return cellEnginePx * enginePxToSource; }
	};

	// Copies the view at once (the pointers die at the next FlowOf). Checks the layout; never guesses a missing part.
	inline AheadRefusal CopyAhead(const VpMotion::FlowView& v, uint64_t seq, AheadCopy& out)
	{
		out = AheadCopy();
		if (v.to != seq || v.from + 1 != v.to || !v.rawForward)
			return AheadRefusal::NoPair;
		if (v.gridW <= 0 || v.gridH <= 0 || v.cellEnginePx <= 0 || v.engineW <= 0 || v.engineH <= 0 ||
			!(v.rawUnitsPerEnginePx > 0) || !(v.enginePxToSource > 0) ||
			(v.engineW + v.cellEnginePx - 1) / v.cellEnginePx != v.gridW || (v.engineH + v.cellEnginePx - 1) / v.cellEnginePx != v.gridH)
			return AheadRefusal::Layout;
		if (!v.rawBackward || !v.forwardCost)
			return AheadRefusal::Incomplete;
		const size_t cells = static_cast<size_t>(v.gridW) * static_cast<size_t>(v.gridH);
		out.seq = seq;
		out.pairFrom = v.from;
		out.pairTo = v.to;
		out.gridW = v.gridW;
		out.gridH = v.gridH;
		out.cellEnginePx = v.cellEnginePx;
		out.engineW = v.engineW;
		out.engineH = v.engineH;
		out.unitsPerEnginePx = v.rawUnitsPerEnginePx;
		out.enginePxToSource = v.enginePxToSource;
		out.forward.assign(v.rawForward, v.rawForward + cells * 2);
		out.backward.assign(v.rawBackward, v.rawBackward + cells * 2);
		out.forwardCost.assign(v.forwardCost, v.forwardCost + cells);
		return AheadRefusal::None;
	}

	// The engine's per-cell trust of the pair (forward direction): OpticalFlowTrust::CellTrust over the producer's own extras maths
	// (CostWeights on the forward costs, FbError of the two directions), with the matcher's consistency limit in ENGINE px.
	inline bool AheadCellTrust(const AheadCopy& a, float consistencyEnginePx, std::vector<float>& out)
	{
		out.clear();
		const int blocksX = (a.engineW + kEngineTrustBlockPx - 1) / kEngineTrustBlockPx;
		const int blocksY = (a.engineH + kEngineTrustBlockPx - 1) / kEngineTrustBlockPx;
		std::vector<float> blockCost, cellError;
		if (!OpticalFlowTrust::CostWeights(a.forwardCost, a.gridW, a.gridH, a.cellEnginePx, kEngineTrustBlockPx, blocksX, blocksY, blockCost))
			return false;
		// FbError reads S10.5; other units are rescaled to it first (exactly, when the units are 32).
		const std::vector<int16_t>* fwd = &a.forward;
		const std::vector<int16_t>* bwd = &a.backward;
		std::vector<int16_t> f32, b32;
		if (a.unitsPerEnginePx != 32.0)
		{
			const double k = 32.0 / a.unitsPerEnginePx;
			f32.resize(a.forward.size());
			b32.resize(a.backward.size());
			for (size_t i = 0; i < a.forward.size(); ++i)
				f32[i] = static_cast<int16_t>(std::lround(a.forward[i] * k));
			for (size_t i = 0; i < a.backward.size(); ++i)
				b32[i] = static_cast<int16_t>(std::lround(a.backward[i] * k));
			fwd = &f32;
			bwd = &b32;
		}
		if (!OpticalFlowTrust::FbError(*fwd, *bwd, a.gridW, a.gridH, a.cellEnginePx, cellError))
			return false;
		return OpticalFlowTrust::CellTrust(blockCost, blocksX, blocksY, cellError, a.gridW, a.gridH, a.cellEnginePx, kEngineTrustBlockPx,
			consistencyEnginePx, out);
	}

	// What the conversion did (diagnostics / the harness).
	struct AheadConvertInfo
	{
		double cellSourcePx = 0;      // source px per cell
		int maxCellsPerBlock = 0;     // cells pooled by one block at most (4 at a half-size search)
		double meanTrust = 0;         // mean block trust
	};

	// FULL NATIVE RESOLUTION (owner, 2026-11-08): the ahead pair AS the fit's field, one sample per native cell (cellEnginePx engine px =
	// CellSourcePx() source px: 8 at a half-size search of a 1080 source, 16 at a quarter-size 4K search), full sub-pel (raw units /
	// unitsPerEnginePx x enginePxToSource), trust = the engine's cell trust (AheadCellTrust). NO aggregation: the fit runs on the native
	// cell grid (the Fitter is grid-generic: blockSize = the cell's source px). sourceGridW x sourceGridH is the camera's 16-px consumer
	// grid, used only to check that the pair covers the picture. Forward only (the fit reads no backward field). False when the pair does
	// not cover the grid or its trust cannot be computed. Deterministic (fixed loop order). The record's per-block OUTPUTS are put on the
	// consumer grid afterwards (RegridToConsumerGrid); the input is never reduced.
	inline bool AheadToSourceGrid(const AheadCopy& a, float consistencyEnginePx, int sourceGridW, int sourceGridH, FieldCopy& out,
		AheadConvertInfo* info = nullptr)
	{
		const size_t cells = a.Cells();
		const double cs = a.CellSourcePx();
		if (cells == 0 || sourceGridW <= 0 || sourceGridH <= 0 || !(cs > 0) || !(a.unitsPerEnginePx > 0) ||
			a.forward.size() < cells * 2 || a.backward.size() < cells * 2 || a.forwardCost.size() < cells)
			return false;
		// The pair must cover the consumer grid (to within one cell of rounding).
		const double spanW = a.gridW * cs, spanH = a.gridH * cs;
		const double needW = (sourceGridW - 1) * static_cast<double>(FieldCopy::kSourceBlockPx), needH = (sourceGridH - 1) * static_cast<double>(FieldCopy::kSourceBlockPx);
		if (spanW + cs < needW || spanH + cs < needH)
			return false;
		std::vector<float> trust;
		if (!AheadCellTrust(a, consistencyEnginePx, trust))
			return false;
		const double toSource = a.enginePxToSource / a.unitsPerEnginePx;
		out.seq = a.seq;
		out.pairFrom = a.pairFrom;
		out.pairTo = a.pairTo;
		out.gridW = a.gridW;
		out.gridH = a.gridH;
		out.blockSize = cs;
		out.stamp = a.stamp;
		out.origin = FieldOrigin::Ahead;
		out.haveP2 = true;
		out.p2Epoch = a.epoch;
		out.fwdX.resize(cells);
		out.fwdY.resize(cells);
		out.fwdTrust = trust;
		out.bwdX.clear();
		out.bwdY.clear();
		out.bwdTrust.clear();
		double trustSum = 0;
		for (size_t i = 0; i < cells; ++i)
		{
			out.fwdX[i] = static_cast<float>(a.forward[i * 2] * toSource);
			out.fwdY[i] = static_cast<float>(a.forward[i * 2 + 1] * toSource);
			trustSum += trust[i];
		}
		if (info)
		{
			info->cellSourcePx = cs;
			info->maxCellsPerBlock = 1;   // one sample per native cell: nothing pooled
			info->meanTrust = trustSum / static_cast<double>(cells);
		}
		return true;
	}

	// The record's per-block OUTPUTS on the consumer grid (the 16-px grid the P8 labels and the lane are sized by). Fitted on the native
	// cell grid, the record's BlockData is put onto gridW x gridH blocks of blockSize source px: each block takes the native cell its centre
	// lies in for the categorical / measured arrays (label, residual, object id, tiny mover, excluded), and the model prediction is evaluated
	// at the block's own centre (exact, from the params). The fit itself and its params are unchanged; only what the consumers index is
	// re-gridded. A record already on that grid, or without blocks, is returned as is.
	inline std::shared_ptr<const CameraModel::CameraMotion> RegridToConsumerGrid(const std::shared_ptr<const CameraModel::CameraMotion>& rec,
		int gridW, int gridH, double blockSize)
	{
		if (!rec || !rec->blocks || gridW <= 0 || gridH <= 0 || !(blockSize > 0))
			return rec;
		const CameraModel::BlockData& b = *rec->blocks;
		if (b.gridW == gridW && b.gridH == gridH && b.blockSize == blockSize)
			return rec;
		if (b.gridW <= 0 || b.gridH <= 0 || !(b.blockSize > 0))
			return rec;
		auto out = std::make_shared<CameraModel::BlockData>();
		out->gridW = gridW;
		out->gridH = gridH;
		out->blockSize = blockSize;
		out->absThresholdPx = b.absThresholdPx;
		out->relThreshold = b.relThreshold;
		const size_t n = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
		const size_t src = static_cast<size_t>(b.gridW) * static_cast<size_t>(b.gridH);
		auto cellOf = [&](int t, int grid, double centre) {
			int c = static_cast<int>(std::floor(centre / b.blockSize));
			(void)t;
			return c < 0 ? 0 : c > grid - 1 ? grid - 1 : c;
		};
		auto take8 = [&](const std::vector<uint8_t>& from, std::vector<uint8_t>& to, const std::vector<size_t>& map) {
			if (from.size() != src) return;
			to.resize(n);
			for (size_t i = 0; i < n; ++i) to[i] = from[map[i]];
		};
		auto takeF = [&](const std::vector<float>& from, std::vector<float>& to, const std::vector<size_t>& map) {
			if (from.size() != src) return;
			to.resize(n);
			for (size_t i = 0; i < n; ++i) to[i] = from[map[i]];
		};
		std::vector<size_t> map(n);
		for (int ty = 0; ty < gridH; ++ty)
		{
			const int cy = cellOf(ty, b.gridH, (ty + 0.5) * blockSize);
			for (int tx = 0; tx < gridW; ++tx)
			{
				const int cx = cellOf(tx, b.gridW, (tx + 0.5) * blockSize);
				map[static_cast<size_t>(ty) * static_cast<size_t>(gridW) + static_cast<size_t>(tx)] =
					static_cast<size_t>(cy) * static_cast<size_t>(b.gridW) + static_cast<size_t>(cx);
			}
		}
		take8(b.label, out->label, map);
		takeF(b.resX, out->resX, map);
		takeF(b.resY, out->resY, map);
		take8(b.objectId, out->objectId, map);
		take8(b.tinyMover, out->tinyMover, map);
		take8(b.excluded, out->excluded, map);
		if (b.modelX.size() == src && b.modelY.size() == src)
		{
			out->modelX.resize(n);
			out->modelY.resize(n);
			for (int ty = 0; ty < gridH; ++ty)
				for (int tx = 0; tx < gridW; ++tx)
				{
					double x = 0, y = 0, dx = 0, dy = 0;
					CameraModel::BlockCentre(tx, ty, blockSize, rec->picture, &x, &y);
					CameraModel::Displacement(rec->params, x, y, &dx, &dy);
					const size_t o = static_cast<size_t>(ty) * static_cast<size_t>(gridW) + static_cast<size_t>(tx);
					out->modelX[o] = static_cast<float>(dx);
					out->modelY[o] = static_cast<float>(dy);
				}
		}
		auto copy = std::make_shared<CameraModel::CameraMotion>(*rec);
		copy->blocks = out;
		return copy;
	}
}
