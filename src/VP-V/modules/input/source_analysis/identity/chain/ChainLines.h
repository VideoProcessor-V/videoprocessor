// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// LineSnapshot (SID-C2b): a compact copy of exactly the lines the chain estimator reads from one plane, and nothing else.
//
// The SourceFrameTap sink copies the LineSampler lines into a snapshot (integer 10-bit codes, 2 bytes per sample, about 0.4 MB
// at 3840x2160 for 32 rows + 32 columns) and releases the plane; the estimator's worker converts it to the units' `LineSet`
// (doubles) and runs on that. The live path and a replay of a dumped snapshot are the same code on the same integers, so a
// dumped ring reproduces the live record bytes exactly (doc 78 section 9b: "the chain estimator is a pure function of its
// sampled lines"). `scalerRect` of the units' LineSet is the active rectangle (the bars were not part of the scaled raster).
//
// Header-only, standard library only, C++14, no clock.
#include "LineSampler.h"

#include <cstdint>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		// EvalFlags: what the call site knew about the evaluated frame (consumed events, doc 78 section 5 contract).
		constexpr uint32_t kEvCut = 1u << 0;
		constexpr uint32_t kEvBlank = 1u << 1;
		constexpr uint32_t kEvFade = 1u << 2;
		constexpr uint32_t kEvDiscontinuity = 1u << 3;
		constexpr uint32_t kEvAreaChanged = 1u << 4;
		constexpr uint32_t kEvClearWindow = 1u << 5;   // a ClearWindow event was consumed: the evidence window was cleared before this evaluation
		constexpr uint32_t kEvMenuHold = 1u << 6;      // SID r2 R7 (safety rules on): a menu / overlay episode began; the upscaled latch is not carried into it
		constexpr uint32_t kEvAbrStep = 1u << 7;       // SID r2 R5 (safety rules on, switch): an ABR rendition step event

		struct LineSnapshot
		{
			uint64_t seq = 0;
			int64_t timeUs = 0;               // source time (the frame clock's domain)
			uint64_t streamKey = 0;           // the Indicator's FULL / era key
			uint32_t flags = 0;               // kEv* consumed with this evaluation
			int32_t cuts = 0;                 // cuts consumed with this evaluation (each is a shot; kEvCut set means >= 1)
			int64_t eventTimeUs = -1;         // source time of the newest consumed cut / blank / fade (-1: unknown)
			int32_t rasterW = 0;              // delivered frame size (SID r2 L3: the geometry set derives its ratios from it); 0 = unknown,
			int32_t rasterH = 0;              // the estimator then uses the active extent (SubmitLines callers that do not set it)
			LineRect active;                  // the active rectangle of the frame
			LineRect inner;                   // active shrunk by the sampler margin (the lines' extent)
			int rowCount = 0;
			int colCount = 0;
			std::vector<int32_t> rowY;        // frame y of every row line
			std::vector<int32_t> colX;        // frame x of every column line
			std::vector<uint16_t> rows;       // rowCount x inner.w, 10-bit codes
			std::vector<uint16_t> cols;       // colCount x inner.h, 10-bit codes

			size_t Bytes() const
			{
				return sizeof(LineSnapshot) + rowY.size() * sizeof(int32_t) + colX.size() * sizeof(int32_t) +
					rows.size() * sizeof(uint16_t) + cols.size() * sizeof(uint16_t);
			}
		};

		// Copies the lines of `plan` out of a plane (samples of `Pix`, row stride in samples). `scale` converts a sample to 10-bit
		// codes (1 for the tap's plane). False when the plan is invalid or the plane is null.
		template <class Pix>
		inline bool CaptureLines(const LinePlan& plan, const Pix* plane, std::ptrdiff_t stride, double scale, LineSnapshot& out)
		{
			if (!plan.valid || plane == nullptr || plan.inner.w < 1 || plan.inner.h < 1)
				return false;
			auto code = [scale](Pix v) -> uint16_t {
				const double c = std::floor(static_cast<double>(v) * scale + 0.5);
				return static_cast<uint16_t>(c < 0.0 ? 0.0 : (c > 65535.0 ? 65535.0 : c));
			};
			out.active = plan.active;
			out.inner = plan.inner;
			out.rowCount = plan.rowCount;
			out.colCount = plan.colCount;
			out.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
			out.colX.assign(plan.colX, plan.colX + plan.colCount);
			out.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 0);
			out.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 0);
			for (int j = 0; j < plan.rowCount; ++j)
			{
				const Pix* src = plane + static_cast<std::ptrdiff_t>(plan.rowY[j]) * stride + plan.inner.x;
				uint16_t* dst = &out.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = code(src[i]);
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				const Pix* src = plane + static_cast<std::ptrdiff_t>(plan.inner.y) * stride + plan.colX[j];
				uint16_t* dst = &out.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = code(src[static_cast<std::ptrdiff_t>(i) * stride]);
			}
			return true;
		}

		// The units' LineSet of one axis of a snapshot (the same geometry LineSampler::Fill gives with the active rectangle as the
		// scaler rectangle). False when the axis has no lines.
		inline bool ToLineSet(const LineSnapshot& s, int axis, LineSet& out)
		{
			if (axis == 0)
			{
				if (s.rowCount <= 0 || s.inner.w < 1 || s.rows.size() != static_cast<size_t>(s.rowCount) * static_cast<size_t>(s.inner.w))
					return false;
				out.axis = 0;
				out.axisLen = s.active.w;
				out.origin = s.inner.x - s.active.x;
				out.Resize(s.rowCount, s.inner.w);
				for (size_t i = 0; i < s.rows.size(); ++i)
					out.data[i] = static_cast<double>(s.rows[i]);
				for (int j = 0; j < s.rowCount; ++j)
					out.cross[static_cast<size_t>(j)] = s.rowY[static_cast<size_t>(j)] - s.active.y;
				return true;
			}
			if (s.colCount <= 0 || s.inner.h < 1 || s.cols.size() != static_cast<size_t>(s.colCount) * static_cast<size_t>(s.inner.h))
				return false;
			out.axis = 1;
			out.axisLen = s.active.h;
			out.origin = s.inner.y - s.active.y;
			out.Resize(s.colCount, s.inner.h);
			for (size_t i = 0; i < s.cols.size(); ++i)
				out.data[i] = static_cast<double>(s.cols[i]);
			for (int j = 0; j < s.colCount; ++j)
				out.cross[static_cast<size_t>(j)] = s.colX[static_cast<size_t>(j)] - s.active.x;
			return true;
		}

		// The output quantisation step of the delivered picture in 10-bit codes, from its own samples: 4 when every sampled code is
		// a multiple of 4 (an 8-bit picture carried in 10-bit codes), else 1 (a true 10-bit picture). Looks at up to ~4096 samples.
		inline int DetectStepCodes(const LineSnapshot& s)
		{
			auto allMultiple = [](const std::vector<uint16_t>& v) {
				const size_t stride = v.size() > 2048 ? v.size() / 2048 : 1;
				for (size_t i = 0; i < v.size(); i += stride)
					if ((v[i] & 3u) != 0u)
						return false;
				return true;
			};
			if (s.rows.empty() && s.cols.empty())
				return 1;
			return (allMultiple(s.rows) && allMultiple(s.cols)) ? 4 : 1;
		}

		// FNV-1a over the snapshot's content (tests; the dump's self-check).
		inline uint64_t SnapshotHash(const LineSnapshot& s)
		{
			uint64_t h = 1469598103934665603ull;
			auto mix = [&h](uint64_t v) {
				for (int i = 0; i < 8; ++i)
				{
					h ^= (v >> (8 * i)) & 0xFFu;
					h *= 1099511628211ull;
				}
			};
			mix(s.seq);
			mix(static_cast<uint64_t>(s.timeUs));
			mix(s.streamKey);
			mix(s.flags);
			mix(static_cast<uint64_t>(static_cast<int64_t>(s.cuts)));
			mix(static_cast<uint64_t>(s.eventTimeUs));
			mix(static_cast<uint64_t>(s.active.x)); mix(static_cast<uint64_t>(s.active.y));
			mix(static_cast<uint64_t>(s.active.w)); mix(static_cast<uint64_t>(s.active.h));
			mix(static_cast<uint64_t>(s.inner.x)); mix(static_cast<uint64_t>(s.inner.y));
			mix(static_cast<uint64_t>(s.inner.w)); mix(static_cast<uint64_t>(s.inner.h));
			for (int32_t v : s.rowY) mix(static_cast<uint64_t>(v));
			for (int32_t v : s.colX) mix(static_cast<uint64_t>(v));
			for (uint16_t v : s.rows) mix(v);
			for (uint16_t v : s.cols) mix(v);
			return h;
		}
	}
}
