// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K8-1 (C4): the trust / occlusion filler's per-block decision, as a standalone CPU mirror. NOT yet called by DejudderWarp (the
// binding waits for P5b, which makes the P8 model available to the motion extension, and K8-2 status).
//
// Rule 1, fill: a block the matcher cannot trust (measured vector untrusted) that P8 labels as a CAMERA block takes the P8 model
// vector (the accessor's Step 2, CameraModel::FieldVector: the policy weight blends it with today's hold value; weight 0 or P8
// absent gives today's value exactly). Object / Unknown / Overlay blocks keep today's hold. A trusted measured block is untouched.
// Rule 2, edge entry: for an interpolated frame at phase t between source frames A and B, the block centre p is shown from
//     srcA = p - t * v        and        srcB = p + (1 - t) * v
// If a FILLED block's srcA or srcB falls outside the active picture, the content is not in both frames: it is NEW CONTENT
// (entering from outside, or leaving through the edge) and is never filled with warped pixels. The block keeps its filler kind
// out of the fill path (Kind::NewContent, fill = false); the consumer shows it as it does today for an unfilled block, and the
// entry flags say which side left the picture so it can take the single frame that has the content. The flags are reported for
// every block, filled or not (for the status and tests); only a filled (Model / Blend) block is turned into NewContent, so
// today's behaviour for measured and held blocks is unchanged.
//
// Only the block CENTRE is projected (a block whose centre stays inside is filled; its outer pixels clamp as today).
// Pure, deterministic, header-only, C++14, no Windows headers. Frame pixel coordinates, picture = the record's active picture
// (continuous positions; both edges count as inside, so the rule is exactly mirror symmetric).
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace FillerEdgeEntry
{
	enum class Kind : uint8_t { Measured, Model, Blend, Hold, NewContent };

	// Which source frame's back-projection left the active picture.
	enum class Entry : uint8_t { None, OutsideA, OutsideB, Both };

	struct Geometry
	{
		CameraModel::Rect picture;      // active picture, frame px
		double blockSize = 16.0;
		double phase = 0.5;             // 0 = frame A, 1 = frame B (clamped to [0, 1])
		double margin = 0.0;            // px the projected centre must stay inside the picture by (0 = on the edge counts as inside)
	};

	struct BlockResult
	{
		float vx = 0.0f, vy = 0.0f;     // the vector the filler uses (today's value when the model is not applied)
		Kind kind = Kind::Hold;
		Entry entry = Entry::None;
		bool fill = false;              // a model-filled block that may be warped (Model / Blend, both projections inside)
	};

	struct Counts
	{
		uint32_t measured = 0, model = 0, blend = 0, hold = 0, newContent = 0, entryA = 0, entryB = 0;
	};

	inline Geometry GeometryFor(const CameraModel::CameraMotion& record, double blockSize, double phase)
	{
		Geometry g;
		g.picture = record.picture;
		g.blockSize = blockSize;
		g.phase = phase;
		return g;
	}

	inline bool InsidePicture(const CameraModel::Rect& r, double x, double y, double margin)
	{
		return x >= r.left + margin && x <= r.right - margin && y >= r.top + margin && y <= r.bottom - margin;
	}

	// One block. `measuredTrusted` / (mx, my) are the caller's measured vector and trust, or its hold value when untrusted.
	inline BlockResult Classify(const CameraModel::Policy& policy, const CameraModel::BlendState& state,
		const CameraModel::CameraMotion* record, const Geometry& geo, int bx, int by, bool measuredTrusted, float mx, float my)
	{
		BlockResult r;
		float ox = mx, oy = my;
		const CameraModel::Choice c = CameraModel::FieldVector(policy, state, record, bx, by, measuredTrusted, mx, my, ox, oy);
		r.vx = ox;
		r.vy = oy;
		switch (c)
		{
		case CameraModel::Choice::Measured: r.kind = Kind::Measured; break;
		case CameraModel::Choice::Model: r.kind = Kind::Model; break;
		case CameraModel::Choice::Blend: r.kind = Kind::Blend; break;
		default: r.kind = Kind::Hold; break;
		}
		double t = geo.phase;
		t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
		const double px = (bx + 0.5) * geo.blockSize, py = (by + 0.5) * geo.blockSize;
		const bool outA = !InsidePicture(geo.picture, px - t * ox, py - t * oy, geo.margin);
		const bool outB = !InsidePicture(geo.picture, px + (1.0 - t) * ox, py + (1.0 - t) * oy, geo.margin);
		r.entry = outA ? (outB ? Entry::Both : Entry::OutsideA) : (outB ? Entry::OutsideB : Entry::None);
		const bool filled = r.kind == Kind::Model || r.kind == Kind::Blend;
		if (filled)
		{
			if (r.entry != Entry::None)
			{
				if (!measuredTrusted)   // a trusted measured block is never relabelled (or filled) by the entry rule
					r.kind = Kind::NewContent;
			}
			else
				r.fill = true;
		}
		return r;
	}

	// A grid of blocks (row-major). `trusted` / (mx, my) per block as above. Returns the counts; out is resized to the grid.
	inline Counts ClassifyGrid(const CameraModel::Policy& policy, const CameraModel::BlendState& state,
		const CameraModel::CameraMotion* record, const Geometry& geo, int gridW, int gridH,
		const std::vector<uint8_t>& trusted, const std::vector<float>& mx, const std::vector<float>& my,
		std::vector<BlockResult>& out)
	{
		Counts n;
		const size_t total = static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0);
		out.assign(total, BlockResult());
		if (trusted.size() < total || mx.size() < total || my.size() < total)
			return n;
		for (int by = 0; by < gridH; ++by)
			for (int bx = 0; bx < gridW; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * static_cast<size_t>(gridW) + static_cast<size_t>(bx);
				const BlockResult r = Classify(policy, state, record, geo, bx, by, trusted[i] != 0, mx[i], my[i]);
				out[i] = r;
				switch (r.kind)
				{
				case Kind::Measured: ++n.measured; break;
				case Kind::Model: ++n.model; break;
				case Kind::Blend: ++n.blend; break;
				case Kind::Hold: ++n.hold; break;
				case Kind::NewContent: ++n.newContent; break;
				}
				if (r.entry == Entry::OutsideA || r.entry == Entry::Both) ++n.entryA;
				if (r.entry == Entry::OutsideB || r.entry == Entry::Both) ++n.entryB;
			}
		return n;
	}
}
